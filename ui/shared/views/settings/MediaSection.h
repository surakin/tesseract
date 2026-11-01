#pragma once

// Settings panel section under a "Media" header:
//   - "Media previews" selector (MSC4278 media_previews: off / private / on)
//   - "Show avatars in invites" checkbox (MSC4278 invite_avatars)
//   - "Pre-load full images while scrolling" checkbox (local prefetch)
//   - "Recompress images before sending" checkbox (still images only)
// The MSC4278 rows mirror the global m.media_preview_config account-data event;
// their change callbacks write it back through the shell. Reads initial state
// from Settings::instance().

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/device_listing.h"

#include <tesseract/settings.h>

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

class MediaSection : public SettingsPage
{
public:
    explicit MediaSection(SettingBinder& binder);
    ~MediaSection() override = default;

    // ── Capture device selection ──────────────────────────────────────────
    // Populate combos. Prepends "System default" (value="") automatically.
    // Does not change the stored selection.
    void set_audio_input_devices(std::vector<tk::DeviceListing> devices);
    void set_audio_output_devices(std::vector<tk::DeviceListing> devices);
    void set_camera_devices(std::vector<tk::DeviceListing> devices);

    // Silently pre-select a value. Does not fire the binder. (The stored ids
    // are bound to Settings; these remain for shells that re-select after
    // rebuilding the option list.)
    void set_selected_audio_input(const std::string& id);
    void set_selected_audio_output(const std::string& id);
    void set_selected_camera(const std::string& id);

    // ComboBox accessors used by tests to simulate user selections.
    tk::ComboBox* audio_input_combo() const
    {
        return audio_input_combo_;
    }
    tk::ComboBox* audio_output_combo() const
    {
        return audio_output_combo_;
    }
    tk::ComboBox* camera_combo() const
    {
        return camera_combo_;
    }

private:
    tk::ComboBox* previews_combo_ = nullptr;
    tk::CheckButton* invite_avatars_cb_ = nullptr;
    tk::CheckButton* prefetch_cb_ = nullptr;
    tk::CheckButton* open_external_cb_ = nullptr;
    tk::CheckButton* recompress_cb_ = nullptr;

    tk::ComboBox* audio_input_combo_ = nullptr;
    tk::ComboBox* audio_output_combo_ = nullptr;
    tk::ComboBox* camera_combo_ = nullptr;
};

} // namespace tesseract::views
