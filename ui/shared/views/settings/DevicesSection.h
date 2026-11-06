#pragma once

// Settings panel section: "Security & Devices" tab.
//
// Groups:
//   "Devices"      - the user's Matrix devices/sessions (see below)
//   "Encryption"   - "Exclude unconfirmed devices" (MSC4153) and "Don't send to
//                    people with unconfirmed devices" checkboxes
//   "Message keys" - "Export message keys…" / "Import message keys…" buttons
//   "Danger zone"  - "Reset digital identity…" (destructive)
//
// The device list shows one row per device, sorted current-first then by
// last-seen-ts desc. Each row
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

#include "SettingBinder.h"
#include "SettingsPage.h"
#include "tesseract/client.h"
#include "tk/host.h"

#include <functional>
#include <string>
#include <vector>

namespace tk
{
class Label;
class Button;
class CheckButton;
} // namespace tk

namespace tesseract::views
{

class SettingsGroup;

// The host a URL opens, for display: the authority with any user info removed,
// the port kept. Empty when the URL has no scheme.
std::string url_destination_host(const std::string& url);

class DevicesSection : public SettingsPage
{
public:
    // The Encryption checkboxes are bound straight to Settings fields via
    // `binder`.
    explicit DevicesSection(SettingBinder& binder);
    ~DevicesSection() override;

    void paint_before_children(tk::PaintCtx& ctx) override;

    // The device list is capped at half this page's viewport height (see
    // DeviceList), so the cap is refreshed here before the base lays out.
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;

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

    // Test-only inspection of the scrollable device list.
    tk::Rect list_bounds_for_testing() const;
    float list_scroll_y_for_testing() const;
    float list_content_height_for_testing() const;
    float list_max_height_for_testing() const;

    // Outputs. Wired in SettingsView::set_controller.
    //   on_delete_requested:  "Sign out" clicked — caller should start the
    //                          UIA flow via SettingsController::delete_device.
    //   on_uia_confirmed:     "I've confirmed" clicked — caller should call
    //                          SettingsController::confirm_device_deletion.
    std::function<void(std::string device_id)> on_delete_requested;
    std::function<void(std::string device_id, std::string session)> on_uia_confirmed;
    // Fired when the user clicks "Cancel" on a UIA-pending row. The owner
    // should call SettingsController::cancel_device_deletion(id) to release
    // the per-device in-flight slot.
    std::function<void(std::string device_id)> on_uia_cancelled;

    // Fired when the user clicks "Export message keys…".
    std::function<void()> on_export_keys;

    // Fired when the user clicks "Import message keys…".
    std::function<void()> on_import_keys;

    // Fired when the user clicks "Reset digital identity…" (destructive).
    std::function<void()> on_reset_identity;

private:
    class DeviceRow;  // defined in DevicesSection.cpp
    class DeviceList; // scrollable, height-capped container for the rows
    void rebuild_(); // re-populate the children from devices_

    bool loading_ = false;
    std::string current_device_id_;
    std::vector<tesseract::Client::Device> devices_;

    DeviceList* list_ = nullptr; // the sole child of the "Devices" group
    tk::Label* loading_label_ = nullptr;
    tk::Label* empty_label_ = nullptr;
    std::vector<DeviceRow*> rows_; // borrowed pointers; owned by list_

    tk::CheckButton* exclude_insecure_cb_ = nullptr;
    tk::CheckButton* block_unverified_cb_ = nullptr;

    // Cached from paint() so the checkboxes' hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
