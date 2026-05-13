#include "LinuxNotifier.h"
#include <cstdlib>

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

    if (use_portal()) {
        GVariantBuilder notif_b;
        g_variant_builder_init(&notif_b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&notif_b, "{sv}", "title",
            g_variant_new_string(n.sender.c_str()));
        g_variant_builder_add(&notif_b, "{sv}", "body",
            g_variant_new_string(n.body.c_str()));
        g_dbus_connection_call(
            bus_,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Notification",
            "AddNotification",
            g_variant_new("(sa{sv})", n.room_id.c_str(), &notif_b),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
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
