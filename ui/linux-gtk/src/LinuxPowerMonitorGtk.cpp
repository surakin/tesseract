#include "LinuxPowerMonitorGtk.h"

namespace gtk4
{

namespace
{
namespace pw = tesseract::power;

bool variant_bool(GVariant* v, bool fallback)
{
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
        return g_variant_get_boolean(v);
    return fallback;
}

// Unbox the "(v)" reply from org.freedesktop.DBus.Properties.Get.
GVariant* unbox_get_reply(GVariant* reply)
{
    if (!reply)
        return nullptr;
    GVariant* boxed = nullptr;
    g_variant_get(reply, "(v)", &boxed);
    g_variant_unref(reply);
    return boxed;
}
} // namespace

GVariant* LinuxPowerMonitorGtk::get_prop_(const char* service, const char* path,
                                          const char* iface, const char* name)
{
    if (!bus_)
        return nullptr;
    GError* err = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, service, path, pw::kPropsIface, "Get",
        g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
    if (err)
    {
        g_error_free(err);
        return nullptr;
    }
    return unbox_get_reply(reply);
}

LinuxPowerMonitorGtk::LinuxPowerMonitorGtk()
{
    bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!bus_)
        return;

    // power-profiles-daemon: prefer the new bus name, fall back to legacy.
    if (GVariant* v = get_prop_(pw::kPpdService, pw::kPpdPath, pw::kPpdIface,
                                "ActiveProfile"))
    {
        g_variant_unref(v);
        ppd_service_ = pw::kPpdService;
        ppd_path_ = pw::kPpdPath;
        ppd_iface_ = pw::kPpdIface;
    }
    else if (GVariant* lv = get_prop_(pw::kPpdServiceLegacy, pw::kPpdPathLegacy,
                                      pw::kPpdIfaceLegacy, "ActiveProfile"))
    {
        g_variant_unref(lv);
        ppd_service_ = pw::kPpdServiceLegacy;
        ppd_path_ = pw::kPpdPathLegacy;
        ppd_iface_ = pw::kPpdIfaceLegacy;
    }

    refresh_on_battery_();
    refresh_power_saver_();

    sub_upower_ = g_dbus_connection_signal_subscribe(
        bus_, pw::kUPowerService, pw::kPropsIface, "PropertiesChanged",
        pw::kUPowerPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        &LinuxPowerMonitorGtk::on_upower_props, this, nullptr);

    if (!ppd_service_.empty())
    {
        sub_ppd_ = g_dbus_connection_signal_subscribe(
            bus_, ppd_service_.c_str(), pw::kPropsIface, "PropertiesChanged",
            ppd_path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            &LinuxPowerMonitorGtk::on_ppd_props, this, nullptr);
    }
}

LinuxPowerMonitorGtk::~LinuxPowerMonitorGtk()
{
    if (bus_)
    {
        if (sub_upower_)
            g_dbus_connection_signal_unsubscribe(bus_, sub_upower_);
        if (sub_ppd_)
            g_dbus_connection_signal_unsubscribe(bus_, sub_ppd_);
        g_object_unref(bus_);
    }
}

void LinuxPowerMonitorGtk::on_upower_props(GDBusConnection*, const char*,
                                           const char*, const char*,
                                           const char*, GVariant* params,
                                           gpointer user_data)
{
    auto* self = static_cast<LinuxPowerMonitorGtk*>(user_data);
    // params: (sa{sv}as) — iface, changed, invalidated
    GVariant* changed = nullptr;
    g_variant_get_child(params, 1, "@a{sv}", &changed);
    GVariant* on_batt = changed ? g_variant_lookup_value(changed, "OnBattery",
                                                         G_VARIANT_TYPE_BOOLEAN)
                                : nullptr;
    if (on_batt)
    {
        if (self->state_.set_on_battery(g_variant_get_boolean(on_batt)))
            self->notify_();
        g_variant_unref(on_batt);
    }
    else
    {
        // Some property (maybe OnBattery, maybe not) changed — re-read to be safe.
        self->refresh_on_battery_();
    }
    if (changed)
        g_variant_unref(changed);
}

void LinuxPowerMonitorGtk::on_ppd_props(GDBusConnection*, const char*,
                                        const char*, const char*, const char*,
                                        GVariant* params, gpointer user_data)
{
    auto* self = static_cast<LinuxPowerMonitorGtk*>(user_data);
    GVariant* changed = nullptr;
    g_variant_get_child(params, 1, "@a{sv}", &changed);
    GVariant* prof = changed ? g_variant_lookup_value(changed, "ActiveProfile",
                                                      G_VARIANT_TYPE_STRING)
                             : nullptr;
    if (prof)
    {
        const char* s = g_variant_get_string(prof, nullptr);
        const bool saver = g_strcmp0(s, tesseract::power::kPowerSaverProfile) == 0;
        if (self->state_.set_power_saver(saver))
            self->notify_();
        g_variant_unref(prof);
    }
    else
    {
        self->refresh_power_saver_();
    }
    if (changed)
        g_variant_unref(changed);
}

void LinuxPowerMonitorGtk::refresh_on_battery_()
{
    GVariant* v = get_prop_(tesseract::power::kUPowerService,
                            tesseract::power::kUPowerPath,
                            tesseract::power::kUPowerIface, "OnBattery");
    if (v)
    {
        if (state_.set_on_battery(variant_bool(v, false)))
            notify_();
        g_variant_unref(v);
    }
}

void LinuxPowerMonitorGtk::refresh_power_saver_()
{
    if (ppd_service_.empty())
        return;
    GVariant* v = get_prop_(ppd_service_.c_str(), ppd_path_.c_str(),
                            ppd_iface_.c_str(), "ActiveProfile");
    if (v)
    {
        const char* s = nullptr;
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
            s = g_variant_get_string(v, nullptr);
        const bool saver =
            s && g_strcmp0(s, tesseract::power::kPowerSaverProfile) == 0;
        if (state_.set_power_saver(saver))
            notify_();
        g_variant_unref(v);
    }
}

void LinuxPowerMonitorGtk::notify_()
{
    // GDBus signal callbacks run on the thread that owns the GMainContext the
    // connection was created on — here the GTK main thread — so calling
    // on_change directly is safe.
    if (on_change)
        on_change();
}

} // namespace gtk4
