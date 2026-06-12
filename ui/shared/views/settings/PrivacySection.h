#pragma once

// Settings panel section: Security & Privacy.
//
// Groups:
//   "Presence"   — checkbox to enable/disable sending and polling presence
//   "Encryption" — "Export room keys…" and "Import room keys…" buttons
//
// Reads initial presence state from Settings::instance(). The export/import
// flows are orchestrated by SettingsController (called from SettingsView).

#include "SettingsPage.h"

#include "tk/controls.h"

#include <tesseract/types.h>

#include <functional>

namespace tesseract::views
{

class PrivacySection : public SettingsPage
{
public:
    PrivacySection();
    ~PrivacySection() override = default;

    // Silently update the presence checkbox without firing the callback.
    void set_send_presence(bool enabled);

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

    // Fired when the user clicks "Export room keys…".
    std::function<void()> on_export_keys;

    // Fired when the user clicks "Import room keys…".
    std::function<void()> on_import_keys;

    // Fired when the user clicks "Reset cryptographic identity…" (destructive).
    std::function<void()> on_reset_identity;

private:
    tk::CheckButton* presence_cb_ = nullptr;
    tk::CheckButton* search_index_cb_ = nullptr;
    tk::Label* search_stats_label_ = nullptr; // counts + status
    tk::Label* search_date_label_ = nullptr;  // "covers messages since …"
};

} // namespace tesseract::views
