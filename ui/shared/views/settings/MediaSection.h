#pragma once

// Settings panel section under a "Media" header:
//   - "Media previews" selector (MSC4278 media_previews: off / private / on)
//   - "Show avatars in invites" checkbox (MSC4278 invite_avatars)
//   - "Pre-load full images while scrolling" checkbox (local prefetch)
// The MSC4278 rows mirror the global m.media_preview_config account-data event;
// their change callbacks write it back through the shell. Reads initial state
// from Settings::instance().

#include "SettingsPage.h"

#include "tk/combobox.h"
#include "tk/controls.h"

#include <tesseract/settings.h>

#include <functional>

namespace tesseract::views
{

class MediaSection : public SettingsPage
{
public:
    MediaSection();
    ~MediaSection() override = default;

    // Silently update control state without firing the callbacks.
    void set_prefetch_checked(bool enabled);
    void set_media_previews(tesseract::Settings::MediaPreviews mode);
    void set_invite_avatars(bool enabled);

    // Fired with the new state when a row changes.
    std::function<void(bool)> on_prefetch_changed;
    std::function<void(tesseract::Settings::MediaPreviews)>
        on_media_previews_changed;
    std::function<void(bool)> on_invite_avatars_changed;

    // Constrain the combobox dropdown popup to the page bounds (mirrors
    // AppearanceSection) so it doesn't paint outside the settings panel.
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;

private:
    tk::ComboBox*    previews_combo_    = nullptr;
    tk::CheckButton* invite_avatars_cb_ = nullptr;
    tk::CheckButton* prefetch_cb_       = nullptr;
};

} // namespace tesseract::views
