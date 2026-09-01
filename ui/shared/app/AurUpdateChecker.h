#pragma once
#include "app/UpdateChecker.h"
#include "tk/weak_self.h"
#include <tesseract/client.h>
#include <functional>
#include <string>

namespace tesseract {

// IUpdateChecker implementation that queries the AUR RPC API.
// Calls Client::check_for_aur_update() (blocking) on a provided async executor,
// then delivers the result on the UI thread via a provided post-to-UI executor.
class AurUpdateChecker : public IUpdateChecker,
                         public tk::EnableWeakSelf<AurUpdateChecker>
{
public:
    using Executor = std::function<void(std::function<void()>)>;

    // `client`      — used to call check_for_aur_update() (must outlive this object)
    // `post_async`  — wraps ShellBase::run_async_(); posts work to a worker thread
    // `post_to_ui`  — wraps ShellBase::post_to_ui_(); posts lambdas to the UI thread
    // `pkgname`     — AUR package name (e.g. "tesseract-matrix")
    // `current_version` — running version string (e.g. tesseract::kVersion)
    AurUpdateChecker(tesseract::Client& client,
                     Executor post_async,
                     Executor post_to_ui,
                     std::string pkgname,
                     std::string current_version);
    ~AurUpdateChecker() override;

    void check_async(Callback on_update) override;

private:
    tesseract::Client& client_;
    Executor post_async_;
    Executor post_to_ui_;
    std::string pkgname_;
    std::string current_version_;
    bool triggered_ = false;
};

} // namespace tesseract
