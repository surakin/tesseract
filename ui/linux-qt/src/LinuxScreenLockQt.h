#pragma once
#include "screen_lock_state.h"
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
        return state_.is_locked();
    }

private slots:
    void onLock()
    {
        state_.on_lock();
    }
    void onUnlock()
    {
        state_.on_unlock();
    }

private:
    void refresh_locked_hint();

    QString session_path_;
    tesseract::screenlock::State state_;
};

} // namespace qt6
