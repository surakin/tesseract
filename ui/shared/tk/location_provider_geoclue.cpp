// GeoClue2 location backend for tk::LocationProvider, shared between
// ui/linux-qt and ui/linux-gtk (GeoClue2 talks D-Bus regardless of display
// server, so unlike screen capture there's no Wayland/X11 split here).
//
// Performs the CreateClient -> Start -> LocationUpdated D-Bus dance on a
// dedicated background thread, using a private GMainContext to dispatch the
// signal subscription — same shape (and same reason: g_bus_get_sync()'s
// shared connection dispatches on the *main* thread's default context, not
// ours) as screen_capture_portal.cpp's portal dance. Requires gio-2.0
// (already linked for the GStreamer backends).
#include "location_provider.h"

// Qt defines 'signals' as a keyword alias (expands to 'public'). GIO's
// gdbusintrospection.h uses 'signals' as a struct member name, which causes a
// parse error. Suppress the Qt macro for the duration of the GIO include and
// restore it immediately after. (Same guard as screen_capture_portal.cpp.)
#pragma push_macro("signals")
#undef signals
#include <gio/gio.h>
#pragma pop_macro("signals")

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace
{

using PostFn = tk::LocationProviderPostFn;

constexpr gint64 kStartTimeoutUsec = 30 * G_USEC_PER_SEC;

struct SignalWait {
    bool        done{false};
    std::string new_path;
};

static void on_location_updated(GDBusConnection*, const gchar*, const gchar*,
                                 const gchar*, const gchar*,
                                 GVariant* params, gpointer user_data)
{
    auto* w = static_cast<SignalWait*>(user_data);
    const gchar* old_path = nullptr;
    const gchar* new_path = nullptr;
    g_variant_get(params, "(&o&o)", &old_path, &new_path);
    if (new_path && new_path[0])
        w->new_path = new_path;
    w->done = true;
}

class LocationProviderGeoClue : public tk::LocationProvider
{
public:
    explicit LocationProviderGeoClue(PostFn post) : post_(std::move(post)) {}

    ~LocationProviderGeoClue() override { cancel(); }

    void request_current_location(LocationCallback cb) override
    {
        if (in_flight_.load(std::memory_order_acquire))
            return; // a request is already pending
        reap_(); // release any finished-but-unreaped previous request
        cb_        = std::move(cb);
        cancelled_ = g_cancellable_new();
        in_flight_.store(true, std::memory_order_release);
        thread_    = std::thread(&LocationProviderGeoClue::run_, this, cancelled_);
    }

    void cancel() override
    {
        if (cancelled_)
            g_cancellable_cancel(cancelled_);
        reap_();
        cb_ = nullptr;
    }

private:
    // Joins thread_ (blocking until run_() returns if a request is still in
    // flight) and releases cancelled_. Only ever called from the UI thread —
    // safe because run_() itself never frees cancelled_/joins thread_; it
    // only clears in_flight_ (as the first thing finish_on_ui_ does, before
    // touching anything else) to signal that it's done with them.
    void reap_()
    {
        if (thread_.joinable())
            thread_.join();
        if (cancelled_)
        {
            g_object_unref(cancelled_);
            cancelled_ = nullptr;
        }
        in_flight_.store(false, std::memory_order_release);
    }

    // Runs entirely on the background thread with |glib_ctx| as the
    // thread-default context, so the D-Bus connection created here (and the
    // signal subscription on it) dispatch through glib_ctx rather than
    // whichever thread's default context g_bus_get_sync()'s shared
    // connection happens to be bound to.
    void run_(GCancellable* cancel)
    {
        bool               success = false;
        tk::LocationFix    fix;
        tk::LocationError  error = tk::LocationError::Unknown;

        GMainContext* glib_ctx = g_main_context_new();
        g_main_context_push_thread_default(glib_ctx);
        auto pop_and_free_ctx = [&] {
            g_main_context_pop_thread_default(glib_ctx);
            g_main_context_unref(glib_ctx);
        };

        GError* err = nullptr;
        // GeoClue2 registers org.freedesktop.GeoClue2 as a SYSTEM-bus
        // activatable service (it arbitrates location access across all
        // users and needs elevated privilege for WiFi/GPS/modem queries) —
        // confirmed by its .service file living under
        // /usr/share/dbus-1/system-services/, not the session-bus
        // equivalent. Session bus here returns "ServiceUnknown: The name is
        // not activatable".
        gchar* bus_addr =
            g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, cancel, &err);
        if (!bus_addr)
        {
            std::fprintf(stderr, "[location] get bus address failed: %s\n",
                         err ? err->message : "(no error)");
            if (err) g_error_free(err);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, tk::LocationError::Unavailable);
            return;
        }
        GDBusConnection* conn = g_dbus_connection_new_for_address_sync(
            bus_addr,
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, cancel, &err);
        g_free(bus_addr);
        if (!conn)
        {
            std::fprintf(stderr, "[location] connect to system bus failed: %s\n",
                         err ? err->message : "(no error)");
            if (err) g_error_free(err);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, tk::LocationError::Unavailable);
            return;
        }

        // Unlike the availability probe in make_location_provider_geoclue()
        // (which deliberately uses DO_NOT_AUTO_START_AT_CONSTRUCTION so
        // merely checking availability doesn't start the service), this proxy
        // is used to actually call CreateClient — GeoClue2 is a normal
        // D-Bus-activatable service that sits idle until needed, and this is
        // exactly the moment (an explicit user request) it should start.
        GDBusProxy* manager = g_dbus_proxy_new_sync(
            conn, G_DBUS_PROXY_FLAGS_NONE, nullptr,
            "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
            "org.freedesktop.GeoClue2.Manager", cancel, &err);
        if (!manager)
        {
            std::fprintf(stderr, "[location] create Manager proxy failed: %s\n",
                         err ? err->message : "(no error)");
            if (err) g_error_free(err);
            g_object_unref(conn);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, tk::LocationError::Unavailable);
            return;
        }

        GVariant* create_ret = g_dbus_proxy_call_sync(
            manager, "CreateClient", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, cancel, &err);
        if (!create_ret)
        {
            std::fprintf(stderr, "[location] CreateClient failed: %s\n",
                         err ? err->message : "(no error)");
            error = classify_error_(err);
            if (err) g_error_free(err);
            g_object_unref(manager);
            g_object_unref(conn);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, error);
            return;
        }
        const gchar* client_path = nullptr;
        g_variant_get(create_ret, "(&o)", &client_path);
        std::string client_path_str = client_path ? client_path : "";
        g_variant_unref(create_ret);
        g_object_unref(manager);

        if (client_path_str.empty())
        {
            std::fprintf(stderr, "[location] CreateClient returned an empty path\n");
            g_object_unref(conn);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, tk::LocationError::Unavailable);
            return;
        }

        GDBusProxy* client = g_dbus_proxy_new_sync(
            conn, G_DBUS_PROXY_FLAGS_NONE, nullptr,
            "org.freedesktop.GeoClue2", client_path_str.c_str(),
            "org.freedesktop.GeoClue2.Client", cancel, &err);
        if (!client)
        {
            std::fprintf(stderr, "[location] create Client proxy failed: %s\n",
                         err ? err->message : "(no error)");
            if (err) g_error_free(err);
            g_object_unref(conn);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, tk::LocationError::Unavailable);
            return;
        }

        // DesktopId: identifies the requesting app to the GeoClue2 authorization
        // agent/polkit prompt. Matches the Wayland app_id used elsewhere
        // (MainWindow.cpp's xdg_activation_token_v1_set_app_id).
        GVariant* set_ret = g_dbus_connection_call_sync(
            conn, "org.freedesktop.GeoClue2", client_path_str.c_str(),
            "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.freedesktop.GeoClue2.Client", "DesktopId",
                          g_variant_new_string("tesseract")),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, cancel, &err);
        if (set_ret) g_variant_unref(set_ret);
        else
        {
            std::fprintf(stderr, "[location] set DesktopId failed: %s\n",
                         err ? err->message : "(no error)");
            if (err) { g_error_free(err); err = nullptr; }
        }

        SignalWait wait;
        guint sub = g_dbus_connection_signal_subscribe(
            conn, "org.freedesktop.GeoClue2", "org.freedesktop.GeoClue2.Client",
            "LocationUpdated", client_path_str.c_str(), nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, on_location_updated, &wait, nullptr);

        GVariant* start_ret = g_dbus_proxy_call_sync(
            client, "Start", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, cancel, &err);
        if (!start_ret)
        {
            std::fprintf(stderr, "[location] Start failed: %s\n",
                         err ? err->message : "(no error)");
            error = classify_error_(err);
            if (err) g_error_free(err);
            g_dbus_connection_signal_unsubscribe(conn, sub);
            g_object_unref(client);
            g_object_unref(conn);
            pop_and_free_ctx();
            finish_on_ui_(false, fix, error);
            return;
        }
        g_variant_unref(start_ret);

        const gint64 deadline = g_get_monotonic_time() + kStartTimeoutUsec;
        while (!wait.done && !g_cancellable_is_cancelled(cancel) &&
               g_get_monotonic_time() < deadline)
        {
            g_main_context_iteration(glib_ctx, TRUE);
        }
        g_dbus_connection_signal_unsubscribe(conn, sub);

        if (wait.done && !wait.new_path.empty())
        {
            GDBusProxy* loc = g_dbus_proxy_new_sync(
                conn, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                "org.freedesktop.GeoClue2", wait.new_path.c_str(),
                "org.freedesktop.GeoClue2.Location", cancel, &err);
            if (loc)
            {
                GVariant* lat = g_dbus_proxy_get_cached_property(loc, "Latitude");
                GVariant* lon = g_dbus_proxy_get_cached_property(loc, "Longitude");
                GVariant* acc = g_dbus_proxy_get_cached_property(loc, "Accuracy");
                if (lat && lon)
                {
                    fix.latitude        = g_variant_get_double(lat);
                    fix.longitude       = g_variant_get_double(lon);
                    fix.accuracy_meters = acc ? g_variant_get_double(acc) : -1.0;
                    success = true;
                    error   = tk::LocationError::None;
                }
                else
                {
                    error = tk::LocationError::Unknown;
                }
                if (lat) g_variant_unref(lat);
                if (lon) g_variant_unref(lon);
                if (acc) g_variant_unref(acc);
                g_object_unref(loc);
            }
            else
            {
                std::fprintf(stderr, "[location] create Location proxy failed: %s\n",
                             err ? err->message : "(no error)");
                if (err) g_error_free(err);
                error = tk::LocationError::Unknown;
            }
        }
        else if (g_cancellable_is_cancelled(cancel))
        {
            error = tk::LocationError::Unknown; // request was cancel()led
        }
        else
        {
            std::fprintf(stderr, "[location] timed out waiting for LocationUpdated\n");
            error = tk::LocationError::Timeout;
        }

        GVariant* stop_ret = g_dbus_proxy_call_sync(
            client, "Stop", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
        if (stop_ret) g_variant_unref(stop_ret);

        g_object_unref(client);
        g_object_unref(conn);
        pop_and_free_ctx();

        finish_on_ui_(success, fix, error);
    }

    // Maps a GeoClue2 D-Bus error to LocationError. GeoClue2 raises
    // org.freedesktop.GeoClue2.Error.NotAuthorized when the user (or a
    // polkit policy) denies the location request.
    static tk::LocationError classify_error_(GError* err)
    {
        if (err && err->message &&
            std::string(err->message).find("NotAuthorized") != std::string::npos)
            return tk::LocationError::PermissionDenied;
        return tk::LocationError::Unknown;
    }

    void finish_on_ui_(bool success, tk::LocationFix fix, tk::LocationError error)
    {
        // Runs synchronously on the background thread, right before run_()
        // returns. Clears in_flight_ first (before touching cb_) so
        // request_current_location()/cancel() know it's safe to reap
        // thread_/cancelled_ as soon as this store is visible.
        in_flight_.store(false, std::memory_order_release);
        if (!cb_)
            return;
        auto cb = cb_;
        post_([cb, success, fix, error]() { cb(success, fix, error); });
    }

    PostFn            post_;
    LocationCallback  cb_;
    std::thread       thread_;
    GCancellable*     cancelled_ = nullptr;
    std::atomic<bool> in_flight_{false};
};

} // namespace

namespace tk
{

std::unique_ptr<LocationProvider> make_location_provider_geoclue(LocationProviderPostFn post)
{
    // Probe GeoClue2 availability without auto-starting the service, mirroring
    // make_screen_capture_portal()'s availability check. GeoClue2 is a
    // SYSTEM-bus activatable service (see the longer note in run_()).
    GDBusProxy* test = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM,
        static_cast<GDBusProxyFlags>(
            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION |
            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
            G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
        nullptr, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
        "org.freedesktop.GeoClue2.Manager", nullptr, nullptr);
    if (!test)
        return nullptr;
    g_object_unref(test);
    return std::make_unique<LocationProviderGeoClue>(std::move(post));
}

} // namespace tk
