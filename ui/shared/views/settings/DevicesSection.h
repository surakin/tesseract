#pragma once

// Settings panel section: list of the user's Matrix devices/sessions.
//
// One row per device, sorted current-first then by last-seen-ts desc. Each row
// shows the display name, a "This device" chip for the current session, a
// verification chip (Verified / Unverified / Unknown), the device id, and
// last-seen IP + time. Non-current rows have a "Sign out" button that walks the
// UIAA fallback-URL flow (open browser → user authenticates → click "Confirm").
//
// The section is fully data-driven: SettingsView feeds it via set_devices() /
// set_loading() / set_current_device_id() / set_device_busy() / enter_uia_state()
// and the section emits on_delete_requested / on_uia_confirmed callbacks for
// the shell to route to SettingsController. Rename UI is deliberately not
// included in this first cut; the underlying rename plumbing exists in
// SettingsController and will be wired up by a follow-up.

#include "SettingsPage.h"
#include "tesseract/client.h"

#include <functional>
#include <string>
#include <vector>

namespace tk
{
class Label;
class Button;
} // namespace tk

namespace tesseract::views
{

class SettingsGroup;

class DevicesSection : public SettingsPage
{
public:
    DevicesSection();
    ~DevicesSection() override;

    // Replace the list with the given snapshot. Implicitly clears any
    // per-row error / UIA state — the source of truth is the new list.
    void set_devices(std::vector<tesseract::Client::Device> devices);

    // Toggle the "Loading sessions…" placeholder.
    void set_loading(bool loading);

    // Mark which device id is the current session so the row shows
    // "This device" and hides its sign-out button. Optional — set_devices()
    // already carries `is_current` from the SDK, but the shell may know it
    // before the list lands.
    void set_current_device_id(std::string id);

    // Per-row state setters keyed by device id. No-op when no matching row.
    void set_device_busy(const std::string& device_id, bool busy);
    void set_device_error(const std::string& device_id, std::string error);
    void enter_uia_state(const std::string& device_id, std::string fallback_url,
                         std::string session);
    void clear_uia_state(const std::string& device_id);

    // Outputs. Wired in SettingsView::set_controller.
    //   on_delete_requested:  "Sign out" clicked — caller should start the
    //                          UIA flow via SettingsController::delete_device.
    //   on_uia_confirmed:     "I've confirmed" clicked — caller should call
    //                          SettingsController::confirm_device_deletion.
    std::function<void(std::string device_id)>                       on_delete_requested;
    std::function<void(std::string device_id, std::string session)>  on_uia_confirmed;
    // Fired when the user clicks "Cancel" on a UIA-pending row. The owner
    // should call SettingsController::cancel_device_deletion(id) to release
    // the per-device in-flight slot.
    std::function<void(std::string device_id)>                       on_uia_cancelled;

private:
    class DeviceRow; // defined in DevicesSection.cpp
    void rebuild_(); // re-populate the children from devices_

    bool loading_ = false;
    std::string current_device_id_;
    std::vector<tesseract::Client::Device> devices_;

    SettingsGroup*           group_         = nullptr;
    tk::Label*               loading_label_ = nullptr;
    tk::Label*               empty_label_   = nullptr;
    std::vector<DeviceRow*>  rows_; // borrowed pointers; owned by group_
};

} // namespace tesseract::views
