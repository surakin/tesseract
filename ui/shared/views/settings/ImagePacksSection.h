#pragma once

// Settings panel section: global "Emojis & Stickers" tab. Composes two
// independent pieces —
//   A. The user's personal image pack (im.ponies.user_emotes /
//      m.image_pack — a de-facto Element/Cinny extension, not part of the
//      merged MSC2545 text, but one this app keeps alive as its "Saved
//      Stickers" pack) via UserPackEditor, with a Save button enabled only
//      once something has changed.
//   B. Every known room-sourced image pack (Client::list_image_packs()
//      filtered to PackSourceKind::Room) via KnownPacksList, letting the
//      user explicitly subscribe/unsubscribe each one to/from the account-
//      wide m.image_pack.rooms event. Checkbox toggles apply immediately —
//      no Save button, consistent with every other checkbox in SettingsView.
//
// Fully data-driven, mirroring DevicesSection/NotificationsSection: this
// section has no Client dependency of its own — SettingsController feeds it
// via the setters below and receives its outputs via the callbacks.

#include "SettingsPage.h"
#include "views/ImagePackEditorView.h" // ImagePackImageProvider
#include "views/settings/KnownPacksList.h"
#include "views/settings/UserPackEditor.h"

#include <tesseract/image_pack.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{
class Button;
} // namespace tk

namespace tesseract::views
{

class ImagePacksSection : public SettingsPage
{
public:
    ImagePacksSection();

    // ----- Personal pack passthrough ----------------------------------
    void set_user_pack_images(std::vector<tesseract::ImagePackImage> images);
    void set_user_pack_image_provider(ImagePackImageProvider p);
    void set_user_pack_tile_preview(std::uint64_t local_id,
                                    std::shared_ptr<tk::Image> image);
    // Gates the editor + Save button while a save is in flight.
    void set_user_pack_saving(bool saving);
    // Re-checks the Save button's enabled state once a save completes. On
    // success the host follows this with a fresh set_user_pack_images() call
    // (the new baseline) which itself clears has_changes(); `ok`/`error` are
    // accepted for symmetry with SettingsController's callback shape but
    // there is no separate error UI in this first cut — a failed save just
    // leaves the staged edits (and thus the enabled Save button) in place
    // for the user to retry.
    void set_user_pack_save_result(bool ok, std::string error);

    std::function<void(std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)>
        on_user_pack_pending_image_added;
    std::function<void()> on_user_pack_save_clicked;

    UserPackEditor* user_pack_editor() const { return user_pack_; }

    // ----- Known packs passthrough --------------------------------------
    void set_known_packs(std::vector<tesseract::ImagePack> all_room_packs);
    std::function<void(std::string room_id, std::string state_key, bool subscribed)>
        on_pack_subscription_toggled;

private:
    void refresh_save_button_();

    UserPackEditor* user_pack_ = nullptr;
    tk::Button* save_btn_ = nullptr;
    KnownPacksList* known_packs_ = nullptr;
};

} // namespace tesseract::views
