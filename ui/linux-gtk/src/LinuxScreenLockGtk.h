#pragma once
#include <tesseract/screen_lock.h>
#include <gio/gio.h>

namespace gtk4
{

// IScreenLock impl backed by systemd-logind over GDBus. Reads the caller
// session's `LockedHint` property and subscribes to the session
// `Lock` / `Unlock` signals on the system bus. Best-effort: if logind /
// D-Bus is unavailable it stays "unlocked" (the app is running
// interactively, so notifications keep working) rather than the Null
// fail-safe.
class LinuxScreenLockGtk final : public tesseract::IScreenLock
{
public:
    LinuxScreenLockGtk();
    ~LinuxScreenLockGtk() override;

    LinuxScreenLockGtk(const LinuxScreenLockGtk&) = delete;
    LinuxScreenLockGtk& operator=(const LinuxScreenLockGtk&) = delete;

    bool is_locked() const override
    {
        return locked_;
    }

private:
    static void on_signal(GDBusConnection*, const char* sender,
                          const char* path, const char* iface,
                          const char* signal, GVariant* params,
                          gpointer user_data);

    GDBusConnection* bus_ = nullptr; // owned (g_object_unref)
    guint sub_lock_ = 0;
    guint sub_unlock_ = 0;
    bool locked_ = false;
};

} // namespace gtk4
