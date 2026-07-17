#pragma once

// Settings panel section: Security & Privacy.
//
// Groups:
//   "Presence"   — checkbox to enable/disable sending and polling presence
//   "Location"   — checkbox to send composed Google Maps / OpenStreetMap
//                   links as interactive m.location events
//   "Encryption" — "Export room keys…" and "Import room keys…" buttons
//
// Reads initial presence state from Settings::instance(). The export/import
// flows are orchestrated by SettingsController (called from SettingsView).

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
    PrivacySection();
    ~PrivacySection() override = default;

    void paint(tk::PaintCtx& ctx) override;

    // Silently update the presence checkbox without firing the callback.
    void set_send_presence(bool enabled);

    // Silently update the "send maps links as location" checkbox without
    // firing the callback.
    void set_send_maps_urls_as_location(bool enabled);

    // Fired with the new state when the "send maps links as location"
    // checkbox is toggled.
    std::function<void(bool)> on_send_maps_urls_as_location_changed;

    // Silently update the message-search-index checkbox without firing.
    void set_index_messages(bool enabled);

    // Update the index-stats lines under the checkbox. Shown only while
    // `enabled`; hidden otherwise. Called by the shell on settings-open and on
    // a slow poll while the history backfill runs.
    void set_search_index_stats(const tesseract::SearchIndexStats& stats,
                                bool enabled);

    // Fired with the new state when the presence checkbox is toggled.
    std::function<void(bool)> on_send_presence_changed;

    // Fired with the new state when the "index messages for search" checkbox is
    // toggled. The shell persists the setting and calls
    // Client::set_search_indexing_enabled().
    std::function<void(bool)> on_index_messages_changed;

#ifdef TESSERACT_GITHUB_REPO
    // Silently update the "check for updates" checkbox without firing.
    void set_check_for_updates(bool enabled);

    // Fired with the new state when the "check for updates" checkbox is toggled.
    std::function<void(bool)> on_check_for_updates_changed;
#endif

    // Fired when the user clicks "Export room keys…".
    std::function<void()> on_export_keys;

    // Fired when the user clicks "Import room keys…".
    std::function<void()> on_import_keys;

    // Fired when the user clicks "Reset cryptographic identity…" (destructive).
    std::function<void()> on_reset_identity;

private:
    tk::CheckButton* presence_cb_ = nullptr;
    tk::CheckButton* send_maps_urls_as_location_cb_ = nullptr;
    tk::CheckButton* search_index_cb_ = nullptr;
    tk::Label* search_stats_label_ = nullptr; // counts + status
    tk::Label* search_date_label_ = nullptr;  // "covers messages since …"
#ifdef TESSERACT_GITHUB_REPO
    tk::CheckButton* check_updates_cb_ = nullptr;
#endif

    // Cached from paint() so the checkbox's hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
