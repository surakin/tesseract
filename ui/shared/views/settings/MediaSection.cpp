#include "MediaSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"

#include <memory>
#include <string>

namespace tesseract::views
{

namespace
{
using MP = tesseract::Settings::MediaPreviews;

const char* mp_to_value(MP m)
{
    switch (m)
    {
    case MP::Off:
        return "off";
    case MP::Private:
        return "private";
    case MP::On:
    default:
        return "on";
    }
}

MP value_to_mp(const std::string& v)
{
    if (v == "off")
        return MP::Off;
    if (v == "private")
        return MP::Private;
    return MP::On;
}
} // namespace

MediaSection::MediaSection()
{
    const auto& s = tesseract::Settings::instance();

    // ── Media previews (MSC4278) ────────────────────────────────────────────
    auto* previews_group = add_group("Media previews");

    auto combo = std::make_unique<tk::ComboBox>();
    combo->set_options({
        {"Always",                "on"},
        {"In private rooms only",  "private"},
        {"Never",                 "off"},
    });
    combo->set_selected_value(mp_to_value(s.media_previews));
    previews_combo_ = previews_group->add_widget(std::move(combo));
    previews_combo_->on_changed = [this](std::string value)
    {
        if (on_media_previews_changed)
            on_media_previews_changed(value_to_mp(value));
    };

    auto invite_cb = std::make_unique<tk::CheckButton>(
        "Show avatars in invites", s.invite_avatars);
    invite_avatars_cb_ = previews_group->add_widget(std::move(invite_cb));
    invite_avatars_cb_->on_change = [this](bool v)
    {
        if (on_invite_avatars_changed)
            on_invite_avatars_changed(v);
    };

    // ── Local media loading ─────────────────────────────────────────────────
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

void MediaSection::set_media_previews(tesseract::Settings::MediaPreviews mode)
{
    if (previews_combo_)
        previews_combo_->set_selected_value(mp_to_value(mode));
}

void MediaSection::set_invite_avatars(bool enabled)
{
    if (invite_avatars_cb_)
        invite_avatars_cb_->set_checked(enabled);
}

void MediaSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    SettingsPage::arrange(ctx, bounds);
    if (previews_combo_)
        previews_combo_->set_popup_clip(bounds);
}

} // namespace tesseract::views
