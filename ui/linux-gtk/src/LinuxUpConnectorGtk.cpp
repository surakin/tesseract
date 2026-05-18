#include "LinuxUpConnectorGtk.h"
#include <tesseract/client.h>
#include <cctype>
#include <cstring>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Introspection XML for org.unifiedpush.Connector1
// ---------------------------------------------------------------------------

static const char kConnector1Xml[] = R"(
<node>
  <interface name="org.unifiedpush.Connector1">
    <method name="Message">
      <arg type="s" name="token"   direction="in"/>
      <arg type="ay" name="message" direction="in"/>
      <arg type="s" name="id"      direction="in"/>
    </method>
    <method name="NewEndpoint">
      <arg type="s" name="token"    direction="in"/>
      <arg type="s" name="endpoint" direction="in"/>
    </method>
    <method name="Unregistered">
      <arg type="s" name="token" direction="in"/>
    </method>
  </interface>
</node>
)";

// D-Bus RequestName reply codes.
static constexpr guint32 kNamePrimaryOwner = 1;

// DBUS_NAME_FLAG_DO_NOT_QUEUE — do not queue if name is taken.
static constexpr guint32 kFlagDoNotQueue = 0x4;

// ---------------------------------------------------------------------------
// UpSharedBusGtk — process singleton (mirrors UpSharedBusQt for GDBus).
// ---------------------------------------------------------------------------

struct UpSharedBusGtk
{
    static UpSharedBusGtk& get()
    {
        static UpSharedBusGtk inst;
        return inst;
    }

    // Request "im.gnomos.Tesseract"; returns false if taken by another process.
    bool acquire(GDBusConnection* bus);
    void release(GDBusConnection* bus);

    void add_route(const std::string& token, LinuxUpConnectorGtk* conn)
    {
        routes_[token] = conn;
    }
    void remove_route(const std::string& token)
    {
        routes_.erase(token);
    }

    std::string find_distributor(GDBusConnection* bus);
    void distributor_call(GDBusConnection* bus, const char* method,
                          const char* svc, const std::string& token);

    void dispatch_new_endpoint(const char* token, const char* endpoint)
    {
        auto it = routes_.find(token);
        if (it != routes_.end())
        {
            it->second->on_new_endpoint(endpoint);
        }
    }
    void dispatch_unregistered(const char* token)
    {
        auto it = routes_.find(token);
        if (it != routes_.end())
        {
            it->second->on_unregistered();
        }
    }

private:
    UpSharedBusGtk() = default;

    int ref_ = 0;
    bool active_ = false;
    guint reg_id_ = 0;
    std::unordered_map<std::string, LinuxUpConnectorGtk*> routes_;
};

// ---------------------------------------------------------------------------
// GDBus vtable — dispatches incoming method calls to UpSharedBusGtk.
// ---------------------------------------------------------------------------

static void
handle_method_call(GDBusConnection* /*conn*/, const gchar* /*sender*/,
                   const gchar* /*object_path*/, const gchar* /*iface*/,
                   const gchar* method_name, GVariant* params,
                   GDBusMethodInvocation* invocation, gpointer /*user_data*/)
{
    UpSharedBusGtk& bus = UpSharedBusGtk::get();

    if (std::strcmp(method_name, "NewEndpoint") == 0)
    {
        const char* token = nullptr;
        const char* endpoint = nullptr;
        g_variant_get(params, "(&s&s)", &token, &endpoint);
        bus.dispatch_new_endpoint(token, endpoint);
    }
    else if (std::strcmp(method_name, "Unregistered") == 0)
    {
        const char* token = nullptr;
        g_variant_get(params, "(&s)", &token);
        bus.dispatch_unregistered(token);
    }
    // Message: no-op — sync delivers event content.
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static const GDBusInterfaceVTable kConnector1Vtable = {handle_method_call,
                                                       nullptr, nullptr};

// ---------------------------------------------------------------------------
// UpSharedBusGtk method bodies
// ---------------------------------------------------------------------------

bool UpSharedBusGtk::acquire(GDBusConnection* bus)
{
    if (ref_++ > 0)
    {
        return active_;
    }

    // RequestName with DBUS_NAME_FLAG_DO_NOT_QUEUE — synchronous, returns fast.
    GVariant* result = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "im.gnomos.Tesseract", kFlagDoNotQueue),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

    if (!result)
    {
        return false;
    }
    guint32 reply = 0;
    g_variant_get(result, "(u)", &reply);
    g_variant_unref(result);

    active_ = (reply == kNamePrimaryOwner);
    if (!active_)
    {
        return false;
    }

    GError* err = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kConnector1Xml, &err);
    if (!info)
    {
        g_error_free(err);
        return false;
    }

    reg_id_ = g_dbus_connection_register_object(
        bus, "/org/unifiedpush/Connector", info->interfaces[0],
        &kConnector1Vtable, nullptr, nullptr, &err);
    g_dbus_node_info_unref(info);

    if (!reg_id_)
    {
        g_error_free(err);
        active_ = false;
    }
    return active_;
}

void UpSharedBusGtk::release(GDBusConnection* bus)
{
    if (--ref_ <= 0)
    {
        ref_ = 0;
        if (active_)
        {
            if (reg_id_)
            {
                g_dbus_connection_unregister_object(bus, reg_id_);
            }
            reg_id_ = 0;
            active_ = false;
            GVariant* rel = g_dbus_connection_call_sync(
                bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "ReleaseName",
                g_variant_new("(s)", "im.gnomos.Tesseract"),
                G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                nullptr);
            if (rel)
            {
                g_variant_unref(rel);
            }
        }
    }
}

std::string UpSharedBusGtk::find_distributor(GDBusConnection* bus)
{
    GVariant* result = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames", nullptr, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    if (!result)
    {
        return {};
    }

    GVariant* names_v = g_variant_get_child_value(result, 0);
    gsize n = g_variant_n_children(names_v);
    std::string found;
    for (gsize i = 0; i < n && found.empty(); ++i)
    {
        GVariant* sv = g_variant_get_child_value(names_v, i);
        const char* svc = g_variant_get_string(sv, nullptr);
        if (svc[0] == ':')
        {
            g_variant_unref(sv);
            continue;
        } // skip unique names

        // Check for Distributor1 interface at the standard path.
        GVariant* xml_v = g_dbus_connection_call_sync(
            bus, svc, "/org/unifiedpush/Distributor",
            "org.freedesktop.DBus.Introspectable", "Introspect", nullptr,
            G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 200 /* ms */,
            nullptr, nullptr);
        if (xml_v)
        {
            const char* xml = nullptr;
            g_variant_get(xml_v, "(&s)", &xml);
            if (xml && std::strstr(xml, "org.unifiedpush.Distributor1"))
            {
                found = svc;
            }
            g_variant_unref(xml_v);
        }
        g_variant_unref(sv);
    }
    g_variant_unref(names_v);
    g_variant_unref(result);
    return found;
}

void UpSharedBusGtk::distributor_call(GDBusConnection* bus, const char* method,
                                      const char* svc, const std::string& token)
{
    g_dbus_connection_call(bus, svc, "/org/unifiedpush/Distributor",
                           "org.unifiedpush.Distributor1", method,
                           g_variant_new("(sss)", "im.gnomos.Tesseract",
                                         token.c_str(), "Tesseract"),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                           nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// LinuxUpConnectorGtk
// ---------------------------------------------------------------------------

static std::string sanitize_token(const std::string& user_id)
{
    std::string t;
    t.reserve(user_id.size());
    for (char c : user_id)
    {
        t += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return t;
}

LinuxUpConnectorGtk::LinuxUpConnectorGtk() = default;

LinuxUpConnectorGtk::~LinuxUpConnectorGtk()
{
    stop();
}

void LinuxUpConnectorGtk::start(tesseract::Client* client,
                                const std::string& user_id)
{
    if (client_)
    {
        return;
    }
    client_ = client;
    token_ = sanitize_token(user_id);

    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus)
    {
        client_ = nullptr;
        return;
    }

    UpSharedBusGtk& shared = UpSharedBusGtk::get();
    if (!shared.acquire(bus))
    {
        g_object_unref(bus);
        client_ = nullptr;
        return;
    }

    shared.add_route(token_, this);

    std::string dist = shared.find_distributor(bus);
    if (!dist.empty())
    {
        distributor_service_ = dist;
        shared.distributor_call(bus, "Register", dist.c_str(), token_);
    }
    g_object_unref(bus);
}

void LinuxUpConnectorGtk::stop()
{
    if (!client_)
    {
        return;
    }
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    UpSharedBusGtk& shared = UpSharedBusGtk::get();
    shared.remove_route(token_);
    distributor_service_.clear();
    if (bus)
    {
        shared.release(bus);
        g_object_unref(bus);
    }
    client_ = nullptr;
}

void LinuxUpConnectorGtk::logout()
{
    if (!client_)
    {
        return;
    }
    if (!distributor_service_.empty())
    {
        GDBusConnection* bus =
            g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        if (bus)
        {
            g_dbus_connection_call(
                bus, distributor_service_.c_str(),
                "/org/unifiedpush/Distributor", "org.unifiedpush.Distributor1",
                "Unregister", g_variant_new("(s)", token_.c_str()), nullptr,
                G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            g_object_unref(bus);
        }
    }
    client_->remove_pusher(token_, "im.gnomos.tesseract");
    stop();
}

void LinuxUpConnectorGtk::on_new_endpoint(const std::string& endpoint)
{
    if (!client_)
    {
        return;
    }
    // The endpoint is supplied by the UnifiedPush distributor over D-Bus —
    // untrusted. Require a well-formed https:// URL with a host before
    // registering it as this account's Matrix push gateway; a malicious or
    // buggy distributor must not be able to redirect push traffic.
    constexpr const char* kScheme = "https://";
    if (endpoint.rfind(kScheme, 0) != 0)
    {
        return;
    }
    std::string gateway = endpoint;
    const std::string prefix = "://";
    auto host_start = gateway.find(prefix);
    if (host_start == std::string::npos)
    {
        return;
    }
    std::size_t host_begin = host_start + prefix.size();
    auto path_start = gateway.find('/', host_begin);
    std::string host = gateway.substr(
        host_begin, path_start == std::string::npos ? std::string::npos
                                                    : path_start - host_begin);
    if (host.empty())
    {
        return;
    }
    if (path_start != std::string::npos)
    {
        gateway.erase(path_start);
    }
    // Matrix HTTP pushers require the URL path to be /_matrix/push/v1/notify.
    gateway += "/_matrix/push/v1/notify";
    client_->register_pusher(token_, "im.gnomos.tesseract", "Tesseract",
                             "Linux Desktop", gateway, "en");
}

void LinuxUpConnectorGtk::on_unregistered()
{
    if (!client_)
    {
        return;
    }
    client_->remove_pusher(token_, "im.gnomos.tesseract");
    if (!distributor_service_.empty())
    {
        GDBusConnection* bus =
            g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        if (bus)
        {
            UpSharedBusGtk::get().distributor_call(
                bus, "Register", distributor_service_.c_str(), token_);
            g_object_unref(bus);
        }
    }
}
