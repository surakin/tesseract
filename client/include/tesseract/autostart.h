#pragma once

namespace tesseract
{

// Cross-platform "launch at login" registration. The concrete shell
// constructs the platform impl at startup and installs it on ShellBase via
// set_autostart_(). is_enabled() always queries the OS directly (registry
// key / SMAppService status / XDG autostart file presence) rather than
// trusting Settings::launch_at_login, so the Settings UI checkbox
// self-heals if the OS-level registration is removed outside the app, and
// "default false" holds trivially on first run (nothing registered yet).
// Mirrors the INotifier / IScreenLock dependency-injection pattern.
class IAutostart
{
public:
    virtual ~IAutostart() = default;

    // Query actual OS state. Fails closed (returns false) if the query
    // itself cannot be performed.
    virtual bool is_enabled() const = 0;

    // Register (true) or unregister (false) the app as a login item.
    // Returns false on failure (e.g. registry write denied, SMAppService
    // error, filesystem permission error) — callers should not assume the
    // OS state changed unless this returns true.
    virtual bool set_enabled(bool enabled) = 0;
};

// Stand-in when no platform autostart impl is wired (unit tests /
// headless). Always reports disabled and rejects enable attempts, which is
// the safe default — a headless/test environment should never silently
// register a real login item.
class NullAutostart final : public IAutostart
{
public:
    bool is_enabled() const override
    {
        return false;
    }

    bool set_enabled(bool /*enabled*/) override
    {
        return false;
    }
};

} // namespace tesseract
