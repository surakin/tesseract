#include "app/AurUpdateChecker.h"

namespace tesseract {

AurUpdateChecker::AurUpdateChecker(tesseract::Client& client,
                                   Executor post_async,
                                   Executor post_to_ui,
                                   std::string pkgname,
                                   std::string current_version)
    : client_(client)
    , post_async_(std::move(post_async))
    , post_to_ui_(std::move(post_to_ui))
    , pkgname_(std::move(pkgname))
    , current_version_(std::move(current_version))
{
}

AurUpdateChecker::~AurUpdateChecker()
{
    invalidate_weak_self();
}

void AurUpdateChecker::check_async(Callback on_update)
{
    if (triggered_)
        return;
    triggered_ = true;

    post_async_(guarded([this, cb = std::move(on_update)]() mutable {
        auto result = client_.check_for_aur_update(pkgname_, current_version_);
        if (!result.has_update)
            return;
        std::string version = std::move(result.version);
        std::string url     = std::move(result.url);
        post_to_ui_([cb = std::move(cb),
                     version = std::move(version),
                     url     = std::move(url)]() mutable {
            cb(std::move(version), std::move(url));
        });
    }));
}

} // namespace tesseract
