#pragma once
#include <functional>
#include <string>

namespace tesseract {

// Abstract interface for checking whether a newer application version is
// available. The concrete implementation (GithubUpdateChecker) calls
// Client::check_for_update() on a background worker thread and invokes the
// callback on the UI thread when a newer version is found.
class IUpdateChecker
{
public:
    // Called on the UI thread when a newer release is available.
    // `version` is the release tag without the leading 'v' (e.g. "0.9.0").
    // `url` is the release page URL (e.g. the GitHub release HTML URL).
    using Callback = std::function<void(std::string version, std::string url)>;

    // Trigger an async update check. `on_update` is called at most once, on
    // the UI thread, iff a strictly newer version is found. May only be called
    // once per instance; subsequent calls are no-ops.
    virtual void check_async(Callback on_update) = 0;

    virtual ~IUpdateChecker() = default;
};

} // namespace tesseract
