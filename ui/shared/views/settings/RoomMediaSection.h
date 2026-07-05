#pragma once

// The "Media" tab of RoomSettingsView: a single combo overriding MSC4278
// media_previews for this room only (Always/Never), or "Use global default"
// to clear the override and inherit the account-wide setting. Unlike the
// app-wide MediaSection combo it otherwise mirrors, there is no "In private
// rooms only" option here: that mode is a heuristic for picking a behavior
// before the room is known, and this combo already applies to one specific,
// already-known room, so picking it would just silently resolve to Always
// or Never depending on the room's own join rule — confusing with no
// benefit. Like every other RoomSettingsView field, changes here are staged
// and only take effect when the user clicks Accept. There is no permission
// check of any kind — a per-room preview override is a personal safety
// setting, not a moderated room property (see MSC4278's own "Alternatives"
// section, which explicitly rejects a permissioned room-state-event design
// for this reason). invite_avatars has no per-room equivalent (global-only,
// per MSC4278 scope) so there is no second control here.

#include "SettingsPage.h"

#include "tk/combobox.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>

namespace tesseract::views
{

class RoomMediaSection : public SettingsPage
{
public:
    RoomMediaSection();
    ~RoomMediaSection() override = default;

    // Silently seed the combo from the current effective override without
    // firing on_override_changed. `has_override` mirrors
    // MediaPreviewOverride::has_media_previews; `mode` is ignored when
    // `has_override` is false (combo shows "Use global default").
    void set_override(bool has_override, tesseract::MediaPreviewConfig::Mode mode);

    // Fired when the user picks a new option. std::nullopt means "Use
    // global default" (clear the override); otherwise a concrete pick.
    std::function<void(std::optional<tesseract::MediaPreviewConfig::Mode>)>
        on_override_changed;

    // Constrain the combobox dropdown popup to the page bounds (mirrors
    // MediaSection/AppearanceSection) so it doesn't paint outside the panel.
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

    // ComboBox accessor used by tests to simulate user selections (mirrors
    // MediaSection's audio_input_combo() etc.).
    tk::ComboBox* override_combo() const { return override_combo_; }

private:
    tk::ComboBox* override_combo_ = nullptr;
};

} // namespace tesseract::views
