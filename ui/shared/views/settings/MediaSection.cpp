#include "MediaSection.h"

#include "SettingsGroup.h"

#include "tk/form_layout.h"
#include "tk/i18n.h"
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

std::vector<tk::ComboBox::Option> make_device_options(
    const std::vector<tk::DeviceListing>& devices)
{
    std::vector<tk::ComboBox::Option> opts;
    opts.push_back({"System default", ""});
    for (const auto& d : devices)
        opts.push_back({d.display_name, d.id});
    return opts;
}
} // namespace

MediaSection::MediaSection()
{
    const auto& s = tesseract::Settings::instance();

    // ── Media previews (MSC4278) ────────────────────────────────────────────
    auto* previews_group = add_group("Media previews");

    auto combo = tk::create_widget<tk::ComboBox>(this);
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

    auto invite_cb = tk::create_widget<tk::CheckButton>(
        this, "Show avatars in invites", s.invite_avatars);
    invite_avatars_cb_ = previews_group->add_widget(std::move(invite_cb));
    invite_avatars_cb_->on_change = [this](bool v)
    {
        if (on_invite_avatars_changed)
            on_invite_avatars_changed(v);
    };

    // ── Local media loading ─────────────────────────────────────────────────
    auto* group = add_group("Media");

    auto prefetch_cb = tk::create_widget<tk::CheckButton>(
        this, "Pre-load full images while scrolling", s.prefetch_full_media);
    prefetch_cb_ = group->add_widget(std::move(prefetch_cb));
    prefetch_cb_->on_change = [this](bool v)
    {
        if (on_prefetch_changed) on_prefetch_changed(v);
    };

    // ── Capture devices ─────────────────────────────────────────────────────
    auto* dev_group = add_group("Capture devices");
    auto* dev_form  = dev_group->add_widget(tk::create_widget<tk::FormLayout>(this));
    dev_form->set_label_gap(8.0f).set_spacing(8.0f);

    auto mic_combo = tk::create_widget<tk::ComboBox>(this);
    mic_combo->set_options({{"System default", ""}});
    mic_combo->on_changed = [this](std::string value)
    {
        if (on_audio_input_changed) on_audio_input_changed(std::move(value));
    };
    audio_input_combo_ = dev_form->add_row(tk::tr("Microphone"), std::move(mic_combo));

    auto spk_combo = tk::create_widget<tk::ComboBox>(this);
    spk_combo->set_options({{"System default", ""}});
    spk_combo->on_changed = [this](std::string value)
    {
        if (on_audio_output_changed) on_audio_output_changed(std::move(value));
    };
    audio_output_combo_ = dev_form->add_row(tk::tr("Speaker"), std::move(spk_combo));

    auto cam_combo = tk::create_widget<tk::ComboBox>(this);
    cam_combo->set_options({{"System default", ""}});
    cam_combo->on_changed = [this](std::string value)
    {
        if (on_camera_changed) on_camera_changed(std::move(value));
    };
    camera_combo_ = dev_form->add_row(tk::tr("Camera"), std::move(cam_combo));
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

void MediaSection::set_audio_input_devices(std::vector<tk::DeviceListing> devices)
{
    audio_input_combo_->set_options(make_device_options(devices));
}

void MediaSection::set_audio_output_devices(std::vector<tk::DeviceListing> devices)
{
    audio_output_combo_->set_options(make_device_options(devices));
}

void MediaSection::set_camera_devices(std::vector<tk::DeviceListing> devices)
{
    camera_combo_->set_options(make_device_options(devices));
}

void MediaSection::set_selected_audio_input(const std::string& id)
{
    audio_input_combo_->set_selected_value(id);
}

void MediaSection::set_selected_audio_output(const std::string& id)
{
    audio_output_combo_->set_selected_value(id);
}

void MediaSection::set_selected_camera(const std::string& id)
{
    camera_combo_->set_selected_value(id);
}

} // namespace tesseract::views
