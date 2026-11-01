#include "app/AurUpdateChecker.h"

namespace tesseract
{

AurUpdateChecker::AurUpdateChecker(tesseract::Client& client, Executor post_async,
                                   Executor post_to_ui, std::string pkgname,
                                   std::string current_version)
    : client_(client), post_async_(std::move(post_async)), post_to_ui_(std::move(post_to_ui)),
      pkgname_(std::move(pkgname)), current_version_(std::move(current_version))
{
}

AurUpdateChecker::~AurUpdateChecker()
{
    invalidate_weak_self();
}

void AurUpdateChecker::check_async(Callback on_update)
{
    if (triggered_)
    {
        return;
    }
    triggered_ = true;

    // Everything the worker needs is copied here, on the calling thread: it
    // must not read this checker's members, which its owner may be
    // destroying meanwhile. A job whose checker is already gone is skipped;
    // one already under way finishes and posts cb as before.
    post_async_(
        [c = &client_, target = pkgname_, current = current_version_, post = post_to_ui_,
         alive = weak_flag(), cb = std::move(on_update)]() mutable
        {
            if (!alive.lock()) // destroyed before the job started: skip it
            {
                return;
            }
            auto result = c->check_for_aur_update(target, current);
            if (!result.has_update)
            {
                return;
            }
            std::string version = std::move(result.version);
            std::string url = std::move(result.url);
            post(
                [cb = std::move(cb), version = std::move(version), url = std::move(url)]() mutable
                {
                    cb(std::move(version), std::move(url));
                });
        });
}

} // namespace tesseract
