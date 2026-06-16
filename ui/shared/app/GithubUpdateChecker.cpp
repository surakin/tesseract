#include "app/GithubUpdateChecker.h"

namespace tesseract {

GithubUpdateChecker::GithubUpdateChecker(tesseract::Client& client,
                                         Executor post_async,
                                         Executor post_to_ui,
                                         std::string repo,
                                         std::string current_version)
    : client_(client)
    , post_async_(std::move(post_async))
    , post_to_ui_(std::move(post_to_ui))
    , repo_(std::move(repo))
    , current_version_(std::move(current_version))
{
}

void GithubUpdateChecker::check_async(Callback on_update)
{
    if (triggered_)
        return;
    triggered_ = true;

    post_async_([this, cb = std::move(on_update)]() mutable {
        auto result = client_.check_for_update(repo_, current_version_);
        if (!result.has_update)
            return;
        std::string version = std::move(result.version);
        std::string url     = std::move(result.url);
        post_to_ui_([cb = std::move(cb),
                     version = std::move(version),
                     url     = std::move(url)]() mutable {
            cb(std::move(version), std::move(url));
        });
    });
}

} // namespace tesseract
