#include "ImagePacksSection.h"

#include "SettingsGroup.h"

#include "tk/controls.h"
#include "tk/i18n.h"

namespace tesseract::views
{

ImagePacksSection::ImagePacksSection()
{
    user_pack_group_ = add_group(tk::tr("Your Sticker Pack"));

    auto editor = std::make_unique<UserPackEditor>();
    user_pack_ = user_pack_group_->add_widget(std::move(editor));
    user_pack_->on_layout_changed = [this] { refresh_save_button_(); };
    user_pack_->on_pending_image_added =
        [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
              const std::string& mime)
    {
        if (on_user_pack_pending_image_added)
            on_user_pack_pending_image_added(local_id, bytes, mime);
    };

    auto save_btn = tk::create_widget<tk::Button>(
        this, tk::tr("Save"), std::function<void()>{}, tk::Button::Variant::Primary);
    save_btn_ = user_pack_group_->add_widget(std::move(save_btn));
    save_btn_->set_on_click(
        [this]
        {
            if (on_user_pack_save_clicked)
                on_user_pack_save_clicked();
        });
    save_btn_->set_enabled(false);

    auto* known_group = add_group(tk::tr("Subscribed Packs"));
    auto known_list = std::make_unique<KnownPacksList>();
    known_packs_ = known_group->add_widget(std::move(known_list));
    known_packs_->on_subscription_toggled =
        [this](std::string room_id, std::string state_key, bool subscribed)
    {
        if (on_pack_subscription_toggled)
            on_pack_subscription_toggled(std::move(room_id), std::move(state_key),
                                         subscribed);
    };
}

void ImagePacksSection::set_user_pack_images(
    std::vector<tesseract::ImagePackImage> images)
{
    user_pack_->set_images(std::move(images));
    refresh_save_button_();
}

void ImagePacksSection::set_user_pack_image_provider(ImagePackImageProvider p)
{
    user_pack_->set_image_provider(std::move(p));
}

void ImagePacksSection::set_user_pack_tile_preview(
    std::uint64_t local_id, std::shared_ptr<tk::Image> image)
{
    user_pack_->set_tile_preview(local_id, std::move(image));
}

void ImagePacksSection::set_user_pack_saving(bool saving)
{
    user_pack_->set_committing(saving);
    save_btn_->set_enabled(!saving && user_pack_->has_changes());
}

void ImagePacksSection::set_user_pack_save_result(bool ok, std::string /*error*/)
{
    // On success the host follows with a fresh set_user_pack_images() call
    // (the new baseline), which itself resets has_changes() to false and
    // re-disables the Save button via refresh_save_button_() above — no
    // extra work needed here beyond re-checking the button in case of
    // failure, where the staged edits (and thus has_changes()) survive.
    refresh_save_button_();
}

void ImagePacksSection::set_known_packs(
    std::vector<tesseract::ImagePack> all_room_packs)
{
    std::vector<KnownPackRow> rows;
    rows.reserve(all_room_packs.size());
    for (auto& p : all_room_packs)
    {
        if (p.source_kind != tesseract::PackSourceKind::Room)
            continue;
        KnownPackRow row;
        row.pack_id      = std::move(p.id);
        row.display_name = std::move(p.display_name);
        row.room_id      = p.source_room;
        row.state_key    = p.source_state_key;
        row.subscribed   = p.is_subscribed;
        rows.push_back(std::move(row));
    }
    known_packs_->set_packs(std::move(rows));
}

void ImagePacksSection::refresh_save_button_()
{
    save_btn_->set_enabled(user_pack_->has_changes());
}

void ImagePacksSection::set_personal_pack_enabled(bool enabled)
{
    user_pack_group_->set_visible(enabled);
    save_btn_->set_enabled(enabled && user_pack_->has_changes());
}

} // namespace tesseract::views
