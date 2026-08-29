#include "GtkSearchProviderGtk.h"

#include "app/AccountManager.h"
#include "app/SearchBackend.h"

#include <tesseract/visual.h>

#include <gio/gio.h>
#include <glib.h>

#include <string>
#include <unordered_map>

namespace
{

const char* kSearchProviderXml = R"XML(
<node>
  <interface name="org.gnome.Shell.SearchProvider2">
    <method name="GetInitialResultSet">
      <arg name="terms" type="as" direction="in"/>
      <arg name="results" type="as" direction="out"/>
    </method>
    <method name="GetSubsearchResultSet">
      <arg name="previous_results" type="as" direction="in"/>
      <arg name="terms" type="as" direction="in"/>
      <arg name="results" type="as" direction="out"/>
    </method>
    <method name="GetResultMetas">
      <arg name="identifiers" type="as" direction="in"/>
      <arg name="metas" type="aa{sv}" direction="out"/>
    </method>
    <method name="ActivateResult">
      <arg name="identifier" type="s" direction="in"/>
      <arg name="terms" type="as" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="LaunchSearch">
      <arg name="terms" type="as" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
  </interface>
</node>)XML";

std::vector<std::string> variant_to_strv(GVariant* v)
{
    std::vector<std::string> out;
    GVariantIter iter;
    g_variant_iter_init(&iter, v);
    const char* s = nullptr;
    while (g_variant_iter_next(&iter, "&s", &s))
        out.emplace_back(s);
    return out;
}

std::string join_terms(const std::vector<std::string>& terms)
{
    std::string out;
    for (const auto& t : terms)
    {
        if (!out.empty())
            out += ' ';
        out += t;
    }
    return out;
}

} // namespace

struct GtkSearchProviderGtk::Impl
{
    tesseract::AccountManager* account_manager = nullptr;

    GDBusConnection* conn = nullptr;
    guint reg_id = 0;

    // Results from the most recent GetInitialResultSet/GetSubsearchResultSet
    // call, keyed by id — GetResultMetas only receives ids, not the query
    // that produced them, so this is the natural place to resolve titles.
    std::unordered_map<std::string, tesseract::SearchBackend::Result> last_results;

    ~Impl()
    {
        if (conn && reg_id)
            g_dbus_connection_unregister_object(conn, reg_id);
        if (conn)
            g_object_unref(conn);
    }
};

namespace
{

void search_provider_method(GDBusConnection*, const char*, const char*,
                            const char*, const char* method, GVariant* params,
                            GDBusMethodInvocation* invocation,
                            gpointer user_data)
{
    auto* impl = static_cast<GtkSearchProviderGtk::Impl*>(user_data);
    auto& sb = impl->account_manager->search_backend();

    if (g_strcmp0(method, "GetInitialResultSet") == 0)
    {
        GVariant* terms_v = nullptr;
        g_variant_get(params, "(@as)", &terms_v);
        const auto term = join_terms(variant_to_strv(terms_v));
        g_variant_unref(terms_v);

        const auto results = sb.query(term);
        impl->last_results.clear();
        GVariantBuilder ids;
        g_variant_builder_init(&ids, G_VARIANT_TYPE("as"));
        for (const auto& r : results)
        {
            g_variant_builder_add(&ids, "s", r.id.c_str());
            impl->last_results.emplace(r.id, r);
        }
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(as)", &ids));
        return;
    }
    if (g_strcmp0(method, "GetSubsearchResultSet") == 0)
    {
        // The room/contact set is small enough that a fresh full query is
        // simpler (and no slower) than incrementally narrowing
        // previous_results, so this just re-runs GetInitialResultSet's logic
        // against the new terms.
        GVariant* prev_v = nullptr;
        GVariant* terms_v = nullptr;
        g_variant_get(params, "(@as@as)", &prev_v, &terms_v);
        g_variant_unref(prev_v);
        const auto term = join_terms(variant_to_strv(terms_v));
        g_variant_unref(terms_v);

        const auto results = sb.query(term);
        impl->last_results.clear();
        GVariantBuilder ids;
        g_variant_builder_init(&ids, G_VARIANT_TYPE("as"));
        for (const auto& r : results)
        {
            g_variant_builder_add(&ids, "s", r.id.c_str());
            impl->last_results.emplace(r.id, r);
        }
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(as)", &ids));
        return;
    }
    if (g_strcmp0(method, "GetResultMetas") == 0)
    {
        GVariant* ids_v = nullptr;
        g_variant_get(params, "(@as)", &ids_v);
        const auto ids = variant_to_strv(ids_v);
        g_variant_unref(ids_v);

        GVariantBuilder metas;
        g_variant_builder_init(&metas, G_VARIANT_TYPE("aa{sv}"));
        for (const auto& id : ids)
        {
            auto it = impl->last_results.find(id);
            if (it == impl->last_results.end())
                continue;
            GVariantBuilder meta;
            g_variant_builder_init(&meta, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&meta, "{sv}", "id",
                                  g_variant_new_string(it->second.id.c_str()));
            g_variant_builder_add(
                &meta, "{sv}", "name",
                g_variant_new_string(it->second.title.c_str()));
            if (!it->second.subtitle.empty())
                g_variant_builder_add(
                    &meta, "{sv}", "description",
                    g_variant_new_string(it->second.subtitle.c_str()));
            if (!it->second.avatar_url.empty())
            {
                // Disk-cache lookup only — GetResultMetas must answer
                // synchronously, so a miss (avatar never rendered yet
                // elsewhere in the app) just omits the icon rather than
                // blocking on a network fetch.
                const std::string tkey = tesseract::visual::thumb_key(
                    it->second.avatar_url, tesseract::visual::kAvatarCacheSize,
                    tesseract::visual::kAvatarCacheSize);
                const auto bytes =
                    impl->account_manager->media_disk_cache().load(tkey);
                if (!bytes.empty())
                {
                    GBytes* gb = g_bytes_new(bytes.data(), bytes.size());
                    GIcon*  ic = g_bytes_icon_new(gb);
                    if (GVariant* icon_v = g_icon_serialize(ic))
                        g_variant_builder_add(&meta, "{sv}", "icon", icon_v);
                    g_object_unref(ic);
                    g_bytes_unref(gb);
                }
            }
            g_variant_builder_add(&metas, "a{sv}", &meta);
        }
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(aa{sv})", &metas));
        return;
    }
    if (g_strcmp0(method, "ActivateResult") == 0)
    {
        const char* id = nullptr;
        GVariant* terms_v = nullptr;
        guint32 ts = 0;
        g_variant_get(params, "(&s@asu)", &id, &terms_v, &ts);
        g_variant_unref(terms_v);
        auto it = impl->last_results.find(id);
        if (it != impl->last_results.end())
            sb.activate(id, it->second.kind);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_strcmp0(method, "LaunchSearch") == 0)
    {
        // No-op in v1 — GNOME Shell falls back to nothing happening, which is
        // acceptable (this is the "open the app's own search UI" affordance,
        // not required for Match/Activate to work).
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

} // namespace

GtkSearchProviderGtk::GtkSearchProviderGtk(
    tesseract::AccountManager& account_manager)
    : impl_(std::make_unique<Impl>())
{
    impl_->account_manager = &account_manager;

    GError* err = nullptr;
    impl_->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!impl_->conn)
    {
        g_clear_error(&err);
        return;
    }

    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kSearchProviderXml, &err);
    if (!node)
    {
        g_clear_error(&err);
        return;
    }

    static const GDBusInterfaceVTable kVtable = {search_provider_method,
                                                 nullptr, nullptr, {nullptr}};
    // Exported on the app's own GApplication-owned bus name
    // ("org.tesseract.gtk", see main.cpp) — this connection is the process's
    // shared session-bus connection, so no separate g_bus_own_name call is
    // needed the way GtkSniTrayIcon needs one for its SNI-spec-mandated name.
    impl_->reg_id = g_dbus_connection_register_object(
        impl_->conn, "/org/tesseract/gtk/SearchProvider", node->interfaces[0],
        &kVtable, impl_.get(), nullptr, &err);
    g_dbus_node_info_unref(node);
    if (!impl_->reg_id)
    {
        g_clear_error(&err);
        return;
    }

    available_ = true;
}

GtkSearchProviderGtk::~GtkSearchProviderGtk() = default;
