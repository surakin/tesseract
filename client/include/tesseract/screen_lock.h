#pragma once

namespace tesseract {

// Cross-platform "is the session / screen locked?" probe. Platform impls
// subscribe to the OS lock signal internally (WTS session notifications on
// Win32, the screenIsLocked distributed notification on macOS, logind on
// Linux) and cache the state, so is_locked() is cheap to call on every
// notification. A real implementation that momentarily cannot determine the
// state returns true (fail-safe: never risk leaking sensitive content onto
// a lock screen). Mirrors the INotifier dependency-injection pattern: the
// concrete shell constructs the platform impl at startup and installs it on
// ShellBase via set_screen_lock_().
class IScreenLock {
public:
    virtual ~IScreenLock() = default;
    virtual bool is_locked() const = 0;
};

// Stand-in when no detection is wired (unit tests / headless). Reports
// "locked" so the privacy gate degrades to hiding image previews rather
// than leaking them — the safe default. Every real shell installs a
// concrete platform impl, so this only affects test/headless paths.
class NullScreenLock final : public IScreenLock {
public:
    bool is_locked() const override { return true; }
};

} // namespace tesseract
