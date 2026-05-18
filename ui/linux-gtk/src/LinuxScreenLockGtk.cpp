#include "LinuxScreenLockGtk.h"

namespace gtk4
{

namespace
{
constexpr const char* kService = tesseract::screenlock::kLogindService;
constexpr const char* kSessIf = tesseract::screenlock::kSessionIface;
constexpr const char* kSessObj = tesseract::screenlock::kSessionPath;
} // namespace

void LinuxScreenLockGtk::on_signal(GDBusConnection*, const char* /*sender*/,
                                   const char* /*path*/, const char* /*iface*/,
                                   const char* signal, GVariant* /*params*/,
                                   gpointer user_data)
{
    auto* self = static_cast<LinuxScreenLockGtk*>(user_data);
    if (g_strcmp0(signal, "Lock") == 0)
    {
        self->state_.on_lock();
    }
    else if (g_strcmp0(signal, "Unlock") == 0)
    {
        self->state_.on_unlock();
    }
}

LinuxScreenLockGtk::LinuxScreenLockGtk()
{
    bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!bus_)
    {
        return;
    }

    // Initial state from the LockedHint property.
    GError* err = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, kService, kSessObj, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", kSessIf, "LockedHint"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
    if (reply)
    {
        GVariant* boxed = nullptr;
        g_variant_get(reply, "(v)", &boxed);
        if (boxed)
        {
            if (g_variant_is_of_type(boxed, G_VARIANT_TYPE_BOOLEAN))
            {
                state_.set_initial(g_variant_get_boolean(boxed));
            }
            g_variant_unref(boxed);
        }
        g_variant_unref(reply);
    }
    else if (err)
    {
        g_error_free(err);
    }

    sub_lock_ = g_dbus_connection_signal_subscribe(
        bus_, kService, kSessIf, "Lock", kSessObj, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &LinuxScreenLockGtk::on_signal, this,
        nullptr);
    sub_unlock_ = g_dbus_connection_signal_subscribe(
        bus_, kService, kSessIf, "Unlock", kSessObj, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &LinuxScreenLockGtk::on_signal, this,
        nullptr);
}

LinuxScreenLockGtk::~LinuxScreenLockGtk()
{
    if (bus_)
    {
        if (sub_lock_)
        {
            g_dbus_connection_signal_unsubscribe(bus_, sub_lock_);
        }
        if (sub_unlock_)
        {
            g_dbus_connection_signal_unsubscribe(bus_, sub_unlock_);
        }
        g_object_unref(bus_);
    }
}

} // namespace gtk4
