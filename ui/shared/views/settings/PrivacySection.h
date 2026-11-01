#pragma once

// Settings panel section: Security & Privacy.
//
// Groups:
//   "Presence"   — checkbox to enable/disable sending and polling presence
//   "Location"   — checkbox to send composed Google Maps / OpenStreetMap
//                   links as interactive m.location events
//   "Encryption" — "Exclude insecure devices" (MSC4153) checkbox, and
//                   "Export room keys…" / "Import room keys…" buttons
//
// Reads initial presence state from Settings::instance(). The export/import
// flows are orchestrated by SettingsController (called from SettingsView).

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tk/controls.h"
#include "tk/host.h"

#include <tesseract/types.h>

#include <functional>

namespace tesseract::views
{

class PrivacySection : public SettingsPage
{
public:
    // Presence / location / link-preview / search / update / encryption
    // checkboxes are bound straight to Settings fields via `binder`.
    explicit PrivacySection(SettingBinder& binder);
    ~PrivacySection() override = default;

    void paint_before_children(tk::PaintCtx& ctx) override;

    // Show the "homeserver doesn't provide link previews" note (MSC4452
    // m.preview_url capability) when `available` is false.
    void set_homeserver_previews_available(bool available);

    // Update the index-stats lines under the checkbox. Shown only while
    // `enabled`; hidden otherwise. Called by the shell on settings-open and on
    // a slow poll while the history backfill runs.
    void set_search_index_stats(const tesseract::SearchIndexStats& stats, bool enabled);

    // Fired when the user clicks "Export room keys…".
    std::function<void()> on_export_keys;

    // Fired when the user clicks "Import room keys…".
    std::function<void()> on_import_keys;

    // Fired when the user clicks "Reset cryptographic identity…" (destructive).
    std::function<void()> on_reset_identity;

private:
    tk::CheckButton* send_maps_urls_as_location_cb_ = nullptr;
    tk::CheckButton* bundled_url_previews_cb_ = nullptr;
    tk::CheckButton* url_previews_direct_cb_ = nullptr;
    tk::Label* previews_unavailable_label_ = nullptr;
    tk::CheckButton* search_index_cb_ = nullptr;
    tk::CheckButton* exclude_insecure_cb_ = nullptr;
    tk::CheckButton* block_unverified_cb_ = nullptr;
    tk::Label* search_stats_label_ = nullptr; // counts + status
    tk::Label* search_date_label_ = nullptr;  // "covers messages since …"

    // Cached from paint() so the checkbox's hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
