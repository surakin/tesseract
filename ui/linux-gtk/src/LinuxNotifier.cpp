#include "LinuxNotifier.h"
#include <cstdlib>
#include <gdk-pixbuf/gdk-pixbuf.h>

LinuxNotifierGtk::LinuxNotifierGtk(std::function<void(std::string)> on_activate)
    : on_activate_(std::move(on_activate))
{
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus_) return;

    action_sub_ = g_dbus_connection_signal_subscribe(
        bus_,
        "org.freedesktop.Notifications",
        "org.freedesktop.Notifications",
        "ActionInvoked",
        "/org/freedesktop/Notifications",
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_action_invoked_cb, this, nullptr);

    closed_sub_ = g_dbus_connection_signal_subscribe(
        bus_,
        "org.freedesktop.Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed",
        "/org/freedesktop/Notifications",
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_notification_closed_cb, this, nullptr);
}

LinuxNotifierGtk::~LinuxNotifierGtk() {
    if (!bus_) return;
    if (action_sub_) g_dbus_connection_signal_unsubscribe(bus_, action_sub_);
    if (closed_sub_) g_dbus_connection_signal_unsubscribe(bus_, closed_sub_);
    g_object_unref(bus_);
}

bool LinuxNotifierGtk::use_portal() const {
    return g_getenv("FLATPAK_ID") != nullptr;
}

void LinuxNotifierGtk::notify(const tesseract::Notification& n) {
    if (!bus_) return;

    // Decode avatar bytes to a GdkPixbuf (kept alive through the D-Bus call so
    // that the pixel pointer in the image-data variant stays valid).
    GdkPixbufLoader* loader = nullptr;
    GdkPixbuf*       rgba   = nullptr;
    if (!n.avatar_bytes.empty()) {
        loader = gdk_pixbuf_loader_new();
        gdk_pixbuf_loader_write(loader,
            reinterpret_cast<const guchar*>(n.avatar_bytes.data()),
            static_cast<gsize>(n.avatar_bytes.size()), nullptr);
        gdk_pixbuf_loader_close(loader, nullptr);
        GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pb) {
            GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
                pb, 64, 64, GDK_INTERP_BILINEAR);
            if (gdk_pixbuf_get_has_alpha(scaled)) {
                rgba = scaled;
            } else {
                rgba = gdk_pixbuf_add_alpha(scaled, FALSE, 0, 0, 0);
                g_object_unref(scaled);
            }
        }
    }

    if (use_portal()) {
        GVariantBuilder notif_b;
        g_variant_builder_init(&notif_b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&notif_b, "{sv}", "title",
            g_variant_new_string(n.sender.c_str()));
        g_variant_builder_add(&notif_b, "{sv}", "body",
            g_variant_new_string(n.body.c_str()));
        if (!n.avatar_bytes.empty()) {
            // Pass raw encoded bytes as a bytes-icon GIcon — the portal daemon
            // handles decode. g_bytes_new copies so the GVariant owns the data.
            GBytes*   gb  = g_bytes_new(n.avatar_bytes.data(), n.avatar_bytes.size());
            GVariant* icv = g_variant_new_from_bytes(G_VARIANT_TYPE("ay"), gb, TRUE);
            g_bytes_unref(gb);
            g_variant_builder_add(&notif_b, "{sv}", "icon",
                g_variant_new("(sv)", "bytes-icon", icv));
        }
        g_dbus_connection_call(
            bus_,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Notification",
            "AddNotification",
            g_variant_new("(sa{sv})", n.room_id.c_str(), &notif_b),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        if (rgba)   g_object_unref(rgba);
        if (loader) g_object_unref(loader);
        return;
    }

    const uint32_t replaces = room_to_id_.count(n.room_id)
                              ? room_to_id_.at(n.room_id) : 0u;

    GVariantBuilder actions_b;
    g_variant_builder_init(&actions_b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&actions_b, "s", "default");
    g_variant_builder_add(&actions_b, "s", "Open");

    GVariantBuilder hints_b;
    g_variant_builder_init(&hints_b, G_VARIANT_TYPE("a{sv}"));
    if (rgba) {
        // image-data hint: (iiibiiay) — width, height, rowstride, has_alpha,
        // bits_per_sample, channels, pixel_data.  The pixel pointer is owned by
        // rgba which outlives the synchronous g_dbus_connection_call_sync below.
        const int    w  = gdk_pixbuf_get_width(rgba);
        const int    h  = gdk_pixbuf_get_height(rgba);
        const int    rs = gdk_pixbuf_get_rowstride(rgba);
        const int    ch = gdk_pixbuf_get_n_channels(rgba);
        const gboolean ha = gdk_pixbuf_get_has_alpha(rgba);
        const guchar*  px = gdk_pixbuf_get_pixels(rgba);
        GVariant* data_v = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, px, static_cast<gsize>(rs) * h, 1);
        g_variant_builder_add(&hints_b, "{sv}", "image-data",
            g_variant_new("(iiibii@ay)", w, h, rs, ha, 8, ch, data_v));
    }

    GVariant* params = g_variant_new(
        "(susssasa{sv}i)",
        "Tesseract",
        replaces,
        "tesseract",
        n.sender.c_str(),
        n.body.c_str(),
        &actions_b,
        &hints_b,
        5000);

    GVariant* result = g_dbus_connection_call_sync(
        bus_,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify",
        params,
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

    // Safe to release avatar resources now that the sync call has serialised params.
    if (rgba)   g_object_unref(rgba);
    if (loader) g_object_unref(loader);

    if (result) {
        uint32_t id = 0;
        g_variant_get(result, "(u)", &id);
        g_variant_unref(result);
        id_to_room_[id]        = n.room_id;
        room_to_id_[n.room_id] = id;
    }
}

void LinuxNotifierGtk::on_action_invoked_cb(
    GDBusConnection*, const char*, const char*,
    const char*, const char*, GVariant* parameters, gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    uint32_t id = 0;
    const char* action = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &action);
    auto it = self->id_to_room_.find(id);
    if (it != self->id_to_room_.end())
        self->on_activate_(it->second);
}

void LinuxNotifierGtk::on_notification_closed_cb(
    GDBusConnection*, const char*, const char*,
    const char*, const char*, GVariant* parameters, gpointer user_data)
{
    auto* self = static_cast<LinuxNotifierGtk*>(user_data);
    uint32_t id = 0, reason = 0;
    g_variant_get(parameters, "(uu)", &id, &reason);
    auto it = self->id_to_room_.find(id);
    if (it != self->id_to_room_.end()) {
        self->room_to_id_.erase(it->second);
        self->id_to_room_.erase(it);
    }
}
