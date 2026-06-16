#pragma once
#include "app/UpdateChecker.h"
#include <tesseract/client.h>
#include <functional>
#include <string>

namespace tesseract {

// IUpdateChecker implementation that queries the GitHub Releases API.
// Calls Client::check_for_update() (blocking) on a provided async executor,
// then delivers the result on the UI thread via a provided post-to-UI executor.
class GithubUpdateChecker : public IUpdateChecker
{
public:
    using Executor = std::function<void(std::function<void()>)>;

    // `client`      — used to call check_for_update() (must outlive this object)
    // `post_async`  — wraps ShellBase::run_async_(); posts work to a worker thread
    // `post_to_ui`  — wraps ShellBase::post_to_ui_(); posts lambdas to the UI thread
    // `repo`        — GitHub "owner/repo" slug (e.g. "acme/tesseract")
    // `current_version` — running version string (e.g. tesseract::kVersion)
    GithubUpdateChecker(tesseract::Client& client,
                        Executor post_async,
                        Executor post_to_ui,
                        std::string repo,
                        std::string current_version);

    void check_async(Callback on_update) override;

private:
    tesseract::Client& client_;
    Executor post_async_;
    Executor post_to_ui_;
    std::string repo_;
    std::string current_version_;
    bool triggered_ = false;
};

} // namespace tesseract
