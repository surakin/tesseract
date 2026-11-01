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

std::vector<tk::ComboBox::Option> make_device_options(const std::vector<tk::DeviceListing>& devices)
{
    std::vector<tk::ComboBox::Option> opts;
    opts.push_back({tk::tr("System default"), ""});
    for (const auto& d : devices)
    {
        opts.push_back({d.display_name, d.id});
    }
    return opts;
}
} // namespace

MediaSection::MediaSection(SettingBinder& binder)
{
    using S = tesseract::Settings;
    const auto& s = S::instance();

    // ── Media previews (MSC4278) ────────────────────────────────────────────
    auto* previews_group = add_group(tk::tr("Media previews"));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Always"), "on"},
        {tk::tr("In private rooms only"), "private"},
        {tk::tr("Never"), "off"},
    });
    previews_combo_ = previews_group->add_widget(std::move(combo));
    binder.bind<MP>(previews_combo_, &S::media_previews, SettingKey::MediaPreviews,
                    {{MP::On, "on"}, {MP::Private, "private"}, {MP::Off, "off"}});

    auto invite_cb = tk::create_widget<tk::CheckButton>(this, tk::tr("Show avatars in invites"),
                                                        s.invite_avatars);
    invite_avatars_cb_ = previews_group->add_widget(std::move(invite_cb));
    binder.bind(invite_avatars_cb_, &S::invite_avatars, SettingKey::InviteAvatars);

    // ── Local media loading ─────────────────────────────────────────────────
    auto* group = add_group(tk::tr("Media"));

    auto prefetch_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Pre-load full images while scrolling"), s.prefetch_full_media);
    prefetch_cb_ = group->add_widget(std::move(prefetch_cb));
    binder.bind(prefetch_cb_, &S::prefetch_full_media, SettingKey::Prefetch);

    auto external_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Open images, videos and files in the system's default viewer"),
        s.open_media_externally);
    open_external_cb_ = group->add_widget(std::move(external_cb));
    binder.bind(open_external_cb_, &S::open_media_externally, SettingKey::OpenExternal);

    auto recompress_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Recompress images before sending (animated images are never changed)"),
        s.recompress_images_on_send);
    recompress_cb_ = group->add_widget(std::move(recompress_cb));
    binder.bind(recompress_cb_, &S::recompress_images_on_send, SettingKey::RecompressImages);

    // ── Capture devices ─────────────────────────────────────────────────────
    auto* dev_group = add_group(tk::tr("Capture devices"));
    auto* dev_form = dev_group->add_widget(tk::create_widget<tk::FormLayout>(this));
    dev_form->set_label_gap(8.0f).set_spacing(8.0f);

    auto mic_combo = tk::create_widget<tk::ComboBox>(this);
    mic_combo->set_options({{tk::tr("System default"), ""}});
    audio_input_combo_ = dev_form->add_row(tk::tr("Microphone"), std::move(mic_combo));
    binder.bind(audio_input_combo_, &S::audio_input_device_id, SettingKey::AudioInput);

    auto spk_combo = tk::create_widget<tk::ComboBox>(this);
    spk_combo->set_options({{tk::tr("System default"), ""}});
    audio_output_combo_ = dev_form->add_row(tk::tr("Speaker"), std::move(spk_combo));
    binder.bind(audio_output_combo_, &S::audio_output_device_id, SettingKey::AudioOutput);

    auto cam_combo = tk::create_widget<tk::ComboBox>(this);
    cam_combo->set_options({{tk::tr("System default"), ""}});
    camera_combo_ = dev_form->add_row(tk::tr("Camera"), std::move(cam_combo));
    binder.bind(camera_combo_, &S::camera_device_id, SettingKey::Camera);
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
