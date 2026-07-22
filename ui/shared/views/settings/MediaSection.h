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

    // ── Capture device selection ──────────────────────────────────────────
    // Populate combos. Prepends "System default" (value="") automatically.
    // Does not fire the callbacks below.
    void set_audio_input_devices(std::vector<tk::DeviceListing> devices);
    void set_audio_output_devices(std::vector<tk::DeviceListing> devices);
    void set_camera_devices(std::vector<tk::DeviceListing> devices);

    // Silently pre-select a value. Does not fire the callbacks.
    void set_selected_audio_input(const std::string& id);
    void set_selected_audio_output(const std::string& id);
    void set_selected_camera(const std::string& id);

    // Fires with the newly selected device ID (empty = system default).
    std::function<void(std::string)> on_audio_input_changed;
    std::function<void(std::string)> on_audio_output_changed;
    std::function<void(std::string)> on_camera_changed;

    // ComboBox accessors used by tests to simulate user selections.
    tk::ComboBox* audio_input_combo()  const { return audio_input_combo_; }
    tk::ComboBox* audio_output_combo() const { return audio_output_combo_; }
    tk::ComboBox* camera_combo()       const { return camera_combo_; }

private:
    tk::ComboBox*    previews_combo_    = nullptr;
    tk::CheckButton* invite_avatars_cb_ = nullptr;
    tk::CheckButton* prefetch_cb_       = nullptr;

    tk::ComboBox* audio_input_combo_  = nullptr;
    tk::ComboBox* audio_output_combo_ = nullptr;
    tk::ComboBox* camera_combo_       = nullptr;
};

} // namespace tesseract::views
