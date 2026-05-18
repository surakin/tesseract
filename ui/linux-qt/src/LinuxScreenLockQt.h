#pragma once
#include <tesseract/screen_lock.h>
#include <QObject>
#include <QString>

namespace qt6
{

// IScreenLock impl backed by systemd-logind. Reads the caller session's
// `LockedHint` property and subscribes to the session `Lock` / `Unlock`
// signals on the system bus. Best-effort: if logind / D-Bus is
// unavailable it stays "unlocked" (the app is running interactively, so
// notifications keep working) rather than the Null fail-safe.
class LinuxScreenLockQt final : public QObject, public tesseract::IScreenLock
{
    Q_OBJECT
public:
    LinuxScreenLockQt();
    ~LinuxScreenLockQt() override = default;

    bool is_locked() const override
    {
        return locked_;
    }

private slots:
    void onLock()
    {
        locked_ = true;
    }
    void onUnlock()
    {
        locked_ = false;
    }

private:
    void refresh_locked_hint();

    QString session_path_;
    bool locked_ = false;
};

} // namespace qt6
