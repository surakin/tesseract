// Pure-D-Bus StatusNotifierItem tray. Uses only gio (GDBus), gdk-pixbuf and
// cairo — NOT GTK — so it does not pull libgtk-3 into the GTK4 process the way
// libayatana-appindicator did. Implements org.kde.StatusNotifierItem and
// com.canonical.dbusmenu directly.

#include "GtkSniTrayIcon.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <glib.h>

#include <algorithm>
#include <cstdint>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace
{

// ── D-Bus interface definitions ────────────────────────────────────────────
// Only the members we implement are declared.
const char* kSniXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
    <signal name="NewTitle"/>
  </interface>
</node>)XML";

const char* kMenuXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <signal name="LayoutUpdated"><arg name="revision" type="u"/><arg name="parent" type="i"/></signal>
    <signal name="ItemsPropertiesUpdated"><arg name="updatedProps" type="a(ia{sv})"/><arg name="removedProps" type="a(ias)"/></signal>
  </interface>
</node>)XML";

constexpr gint32 kMenuShowId = 1;
constexpr gint32 kMenuQuitId = 2;

bool status_notifier_host_present()
{
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus)
    {
        g_clear_error(&err);
        return false;
    }
    GVariant* result = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.kde.StatusNotifierWatcher",
                      "IsStatusNotifierHostRegistered"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &err);
    g_object_unref(bus);
    if (!result)
    {
        g_clear_error(&err);
        return false;
    }
    GVariant* inner = nullptr;
    g_variant_get(result, "(v)", &inner);
    const bool present = inner && g_variant_get_boolean(inner);
    if (inner)
    {
        g_variant_unref(inner);
    }
    g_variant_unref(result);
    return present;
}

const char* resolve_icon_path()
{
#ifdef TESSERACT_ICON_SEARCH_PATH
    static const std::string dev_path =
        std::string(TESSERACT_ICON_SEARCH_PATH) +
        "/hicolor/scalable/apps/tesseract.svg";
    if (g_file_test(dev_path.c_str(), G_FILE_TEST_EXISTS))
    {
        return dev_path.c_str();
    }
#endif
    return nullptr;
}

// Paint a GdkPixbuf (RGBA, non-premultiplied) onto a cairo context without
// pulling in GDK (gdk_cairo_set_source_pixbuf lives in libgdk-3/4). We build a
// premultiplied ARGB32 surface by hand and use it as the source.
void blit_pixbuf(cairo_t* cr, GdkPixbuf* pb)
{
    const int w = gdk_pixbuf_get_width(pb);
    const int h = gdk_pixbuf_get_height(pb);
    const int nc = gdk_pixbuf_get_n_channels(pb);
    const int srcstride = gdk_pixbuf_get_rowstride(pb);
    const guchar* src = gdk_pixbuf_get_pixels(pb);

    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char* dst = cairo_image_surface_get_data(s);
    const int dststride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const guchar* p = src + y * srcstride + x * nc;
            const guint8 r = p[0], g = p[1], b = p[2];
            const guint8 a = (nc == 4) ? p[3] : 255;
            // cairo ARGB32 accessed as a host-endian uint32 is 0xAARRGGBB and
            // is premultiplied — the uint32 write below is endianness-safe.
            const guint8 pr = static_cast<guint8>((r * a + 127) / 255);
            const guint8 pg = static_cast<guint8>((g * a + 127) / 255);
            const guint8 pbl = static_cast<guint8>((b * a + 127) / 255);
            auto* d = reinterpret_cast<guint32*>(dst + y * dststride + x * 4);
            *d = (static_cast<guint32>(a) << 24) |
                 (static_cast<guint32>(pr) << 16) |
                 (static_cast<guint32>(pg) << 8) | pbl;
        }
    }
    cairo_surface_mark_dirty(s);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(s);
}

// One tray variant: base icon + optional coloured dot (bottom-right), returned
// as SNI IconPixmap bytes (ARGB32, network/big-endian, non-premultiplied).
struct IconVariant
{
    int w = 0;
    int h = 0;
    GBytes* argb = nullptr; // owned
};

IconVariant render_variant(GdkPixbuf* base, int side, std::int32_t dot_rgb)
{
    IconVariant out;
    cairo_surface_t* surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS)
    {
        cairo_surface_destroy(surf);
        return out;
    }
    cairo_t* cr = cairo_create(surf);
    if (base)
    {
        blit_pixbuf(cr, base);
    }
    if (dot_rgb >= 0)
    {
        const double dot   = tesseract::badge_dot_px(side);
        const double inset = tesseract::badge_inset_px(side);
        const double cx = side - dot / 2.0 - inset;
        const double cy = side - dot / 2.0 - inset;
        const double r = dot / 2.0;
        const double rr = ((dot_rgb >> 16) & 0xFF) / 255.0;
        const double gg = ((dot_rgb >> 8) & 0xFF) / 255.0;
        const double bb = (dot_rgb & 0xFF) / 255.0;
        cairo_set_source_rgb(cr, rr, gg, bb);
        cairo_arc(cr, cx, cy, r, 0.0, 2.0 * G_PI);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_line_width(cr, tesseract::badge_inset_px(side));
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surf);

    const int w = cairo_image_surface_get_width(surf);
    const int h = cairo_image_surface_get_height(surf);
    const int stride = cairo_image_surface_get_stride(surf);
    const unsigned char* data = cairo_image_surface_get_data(surf);
    std::vector<guint8> argb(static_cast<size_t>(w) * h * 4);
    size_t o = 0;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const guint32 px =
                *reinterpret_cast<const guint32*>(data + y * stride + x * 4);
            guint8 a = (px >> 24) & 0xFF;
            guint8 r = (px >> 16) & 0xFF;
            guint8 g = (px >> 8) & 0xFF;
            guint8 b = px & 0xFF;
            // Un-premultiply for SNI (hosts expect straight alpha).
            if (a > 0 && a < 255)
            {
                r = static_cast<guint8>(std::min(255, r * 255 / a));
                g = static_cast<guint8>(std::min(255, g * 255 / a));
                b = static_cast<guint8>(std::min(255, b * 255 / a));
            }
            argb[o++] = a; // network byte order: A,R,G,B
            argb[o++] = r;
            argb[o++] = g;
            argb[o++] = b;
        }
    }
    cairo_surface_destroy(surf);
    out.w = w;
    out.h = h;
    out.argb = g_bytes_new(argb.data(), argb.size());
    return out;
}

GVariant* pixmap_to_variant(const IconVariant& v)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
    if (v.argb)
    {
        gsize len = 0;
        const guint8* data =
            static_cast<const guint8*>(g_bytes_get_data(v.argb, &len));
        GVariant* ay =
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, 1);
        g_variant_builder_add(&b, "(ii@ay)", v.w, v.h, ay);
    }
    return g_variant_builder_end(&b); // floating a(iiay)
}

GVariant* make_menu_item(gint32 id, const char* label)
{
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(label));
    g_variant_builder_add(&props, "{sv}", "enabled",
                          g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "visible",
                          g_variant_new_boolean(TRUE));
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    return g_variant_new("(ia{sv}av)", id, &props, &children);
}

} // namespace

struct GtkSniTrayIcon::Impl
{
    std::function<void()> on_show;
    std::function<void()> on_quit;

    GDBusConnection* conn = nullptr;
    guint sni_reg_id = 0;
    guint menu_reg_id = 0;
    guint own_name_id = 0;
    std::string well_known_name;

    IconVariant normal;
    IconVariant unread;
    IconVariant mention;
    const IconVariant* current = nullptr;

    std::string tooltip = "Tesseract";

    ~Impl()
    {
        if (conn && sni_reg_id)
            g_dbus_connection_unregister_object(conn, sni_reg_id);
        if (conn && menu_reg_id)
            g_dbus_connection_unregister_object(conn, menu_reg_id);
        if (own_name_id)
            g_bus_unown_name(own_name_id);
        if (normal.argb)
            g_bytes_unref(normal.argb);
        if (unread.argb)
            g_bytes_unref(unread.argb);
        if (mention.argb)
            g_bytes_unref(mention.argb);
        if (conn)
            g_object_unref(conn);
    }

    void emit(const char* iface, const char* signal, GVariant* args)
    {
        if (!conn)
            return;
        const char* path = (g_strcmp0(iface, "com.canonical.dbusmenu") == 0)
                               ? "/MenuBar"
                               : "/StatusNotifierItem";
        g_dbus_connection_emit_signal(conn, nullptr, path, iface, signal, args,
                                      nullptr);
    }
};

namespace
{

// ── org.kde.StatusNotifierItem vtable ──────────────────────────────────────
void sni_method(GDBusConnection*, const char*, const char*, const char*,
                const char* method, GVariant*,
                GDBusMethodInvocation* invocation, gpointer user_data)
{
    auto* impl = static_cast<GtkSniTrayIcon::Impl*>(user_data);
    if (g_strcmp0(method, "Activate") == 0 && impl->on_show)
    {
        impl->on_show();
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* sni_get_property(GDBusConnection*, const char*, const char*,
                           const char*, const char* prop, GError**,
                           gpointer user_data)
{
    auto* impl = static_cast<GtkSniTrayIcon::Impl*>(user_data);
    if (g_strcmp0(prop, "Category") == 0)
        return g_variant_new_string("Communications");
    if (g_strcmp0(prop, "Id") == 0)
        return g_variant_new_string("tesseract");
    if (g_strcmp0(prop, "Title") == 0)
        return g_variant_new_string("Tesseract");
    if (g_strcmp0(prop, "Status") == 0)
        return g_variant_new_string("Active");
    if (g_strcmp0(prop, "WindowId") == 0)
        return g_variant_new_int32(0);
    if (g_strcmp0(prop, "IconName") == 0 ||
        g_strcmp0(prop, "OverlayIconName") == 0 ||
        g_strcmp0(prop, "AttentionIconName") == 0)
        return g_variant_new_string("");
    if (g_strcmp0(prop, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "Menu") == 0)
        return g_variant_new_object_path("/MenuBar");
    if (g_strcmp0(prop, "IconPixmap") == 0)
        return pixmap_to_variant(impl->current ? *impl->current
                                               : IconVariant{});
    if (g_strcmp0(prop, "ToolTip") == 0)
    {
        GVariant* empty =
            g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
        return g_variant_new("(s@a(iiay)ss)", "", empty,
                             impl->tooltip.c_str(), "");
    }
    return nullptr;
}

// ── com.canonical.dbusmenu vtable ──────────────────────────────────────────
void menu_method(GDBusConnection*, const char*, const char*, const char*,
                 const char* method, GVariant* params,
                 GDBusMethodInvocation* invocation, gpointer user_data)
{
    auto* impl = static_cast<GtkSniTrayIcon::Impl*>(user_data);

    if (g_strcmp0(method, "GetLayout") == 0)
    {
        GVariantBuilder root_children;
        g_variant_builder_init(&root_children, G_VARIANT_TYPE("av"));
        g_variant_builder_add(&root_children, "v",
                              make_menu_item(kMenuShowId, "Show Tesseract"));
        g_variant_builder_add(&root_children, "v",
                              make_menu_item(kMenuQuitId, "Quit"));
        GVariantBuilder root_props;
        g_variant_builder_init(&root_props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&root_props, "{sv}", "children-display",
                              g_variant_new_string("submenu"));
        GVariant* root =
            g_variant_new("(ia{sv}av)", 0, &root_props, &root_children);
        // `@` inserts the already-built `root` GVariant; without it the format
        // parser would try to construct the inner tuple from varargs.
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(u@(ia{sv}av))", 1u, root));
        return;
    }
    if (g_strcmp0(method, "GetGroupProperties") == 0)
    {
        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        const struct
        {
            gint32 id;
            const char* label;
        } items[] = {{kMenuShowId, "Show Tesseract"}, {kMenuQuitId, "Quit"}};
        for (const auto& it : items)
        {
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&props, "{sv}", "label",
                                  g_variant_new_string(it.label));
            g_variant_builder_add(&props, "{sv}", "enabled",
                                  g_variant_new_boolean(TRUE));
            g_variant_builder_add(&props, "{sv}", "visible",
                                  g_variant_new_boolean(TRUE));
            g_variant_builder_add(&out, "(ia{sv})", it.id, &props);
        }
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(a(ia{sv}))", &out));
        return;
    }
    if (g_strcmp0(method, "Event") == 0)
    {
        gint32 id = 0;
        const char* event_id = nullptr;
        GVariant* data = nullptr;
        guint32 ts = 0;
        g_variant_get(params, "(i&svu)", &id, &event_id, &data, &ts);
        if (data)
            g_variant_unref(data);
        if (g_strcmp0(event_id, "clicked") == 0)
        {
            if (id == kMenuShowId && impl->on_show)
                impl->on_show();
            else if (id == kMenuQuitId && impl->on_quit)
                impl->on_quit();
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    if (g_strcmp0(method, "AboutToShow") == 0)
    {
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(b)", FALSE));
        return;
    }
    if (g_strcmp0(method, "GetProperty") == 0)
    {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(v)", g_variant_new_string("")));
        return;
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* menu_get_property(GDBusConnection*, const char*, const char*,
                            const char*, const char* prop, GError**, gpointer)
{
    if (g_strcmp0(prop, "Version") == 0)
        return g_variant_new_uint32(3);
    if (g_strcmp0(prop, "Status") == 0)
        return g_variant_new_string("normal");
    if (g_strcmp0(prop, "TextDirection") == 0)
        return g_variant_new_string("ltr");
    if (g_strcmp0(prop, "IconThemePath") == 0)
        return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

} // namespace

GtkSniTrayIcon::GtkSniTrayIcon(std::function<void()> on_show,
                               std::function<void()> on_quit)
    : impl_(std::make_unique<Impl>())
{
    impl_->on_show = std::move(on_show);
    impl_->on_quit = std::move(on_quit);

    if (!status_notifier_host_present())
    {
        return; // available_ stays false → caller falls back to quit-on-close
    }

    // Render icon variants up front (in-memory ARGB; no temp files).
    if (const char* svg = resolve_icon_path())
    {
        GError* err = nullptr;
        GdkPixbuf* base =
            gdk_pixbuf_new_from_file_at_scale(svg, 64, 64, TRUE, &err);
        if (base)
        {
            impl_->normal = render_variant(base, 64, -1);
            impl_->unread = render_variant(base, 64, static_cast<std::int32_t>(tesseract::kBadgeColorUnread));
            impl_->mention = render_variant(base, 64, static_cast<std::int32_t>(tesseract::kBadgeColorMention));
            g_object_unref(base);
        }
        else
        {
            g_clear_error(&err);
        }
    }
    impl_->current = &impl_->normal;

    GError* err = nullptr;
    impl_->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!impl_->conn)
    {
        g_clear_error(&err);
        return;
    }

    GDBusNodeInfo* sni_node = g_dbus_node_info_new_for_xml(kSniXml, &err);
    GDBusNodeInfo* menu_node =
        err ? nullptr : g_dbus_node_info_new_for_xml(kMenuXml, &err);
    if (!sni_node || !menu_node)
    {
        g_clear_error(&err);
        if (sni_node)
            g_dbus_node_info_unref(sni_node);
        return;
    }

    static const GDBusInterfaceVTable kSniVtable = {sni_method, sni_get_property,
                                                    nullptr, {nullptr}};
    static const GDBusInterfaceVTable kMenuVtable = {
        menu_method, menu_get_property, nullptr, {nullptr}};

    impl_->sni_reg_id = g_dbus_connection_register_object(
        impl_->conn, "/StatusNotifierItem", sni_node->interfaces[0],
        &kSniVtable, impl_.get(), nullptr, &err);
    if (impl_->sni_reg_id)
    {
        impl_->menu_reg_id = g_dbus_connection_register_object(
            impl_->conn, "/MenuBar", menu_node->interfaces[0], &kMenuVtable,
            impl_.get(), nullptr, &err);
    }
    g_dbus_node_info_unref(sni_node);
    g_dbus_node_info_unref(menu_node);
    if (!impl_->sni_reg_id || !impl_->menu_reg_id)
    {
        g_clear_error(&err);
        return;
    }

    // Own the spec-mandated well-known name, then register with the watcher.
    impl_->well_known_name = std::string("org.kde.StatusNotifierItem-") +
                             std::to_string(static_cast<long>(getpid())) + "-1";
    impl_->own_name_id = g_bus_own_name_on_connection(
        impl_->conn, impl_->well_known_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
        nullptr, nullptr, nullptr, nullptr);

    g_dbus_connection_call(
        impl_->conn, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", impl_->well_known_name.c_str()), nullptr,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);

    available_ = true;
}

GtkSniTrayIcon::~GtkSniTrayIcon() = default;

void GtkSniTrayIcon::set_tooltip(const std::string& text)
{
    if (!available_)
        return;
    impl_->tooltip = text;
    impl_->emit("org.kde.StatusNotifierItem", "NewToolTip", nullptr);
}

void GtkSniTrayIcon::set_unread(bool has_unread, bool has_highlight)
{
    if (!available_)
        return;
    const IconVariant* pick = &impl_->normal;
    if (has_highlight && impl_->mention.argb)
    {
        pick = &impl_->mention;
    }
    else if ((has_unread || has_highlight) && impl_->unread.argb)
    {
        pick = &impl_->unread;
    }
    impl_->current = pick;
    impl_->emit("org.kde.StatusNotifierItem", "NewIcon", nullptr);
}
