#include "MediaSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"

#include <memory>

namespace tesseract::views
{

MediaSection::MediaSection()
{
    const auto& s = tesseract::Settings::instance();

    auto* group = add_group("Media");

    auto prefetch_cb = std::make_unique<tk::CheckButton>(
        "Pre-load full images while scrolling", s.prefetch_full_media);
    prefetch_cb_ = group->add_widget(std::move(prefetch_cb));
    prefetch_cb_->on_change = [this](bool v)
    {
        if (on_prefetch_changed) on_prefetch_changed(v);
    };
}

void MediaSection::set_prefetch_checked(bool enabled)
{
    prefetch_cb_->set_checked(enabled);
}

} // namespace tesseract::views
