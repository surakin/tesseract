#pragma once

// Settings panel section: "Messages" tab.
//
// Groups:
//   "Composer"      - checkboxes to replace text emoticons with emoji, send
//                     Google Maps / OpenStreetMap links as m.location events,
//                     and recompress images before sending
//   "Timeline"      - checkboxes for join/leave events, sender status emoji
//                     and sender timezone difference
//   "Link previews" - bundled-preview and direct-fetch checkboxes, plus a note
//                     when the homeserver has link previews off
//   "Search"        - local message-index checkbox with index stats
//
// Every checkbox reads its initial state from Settings::instance() and is
// bound straight to its field via the SettingBinder.

#include "SettingBinder.h"
#include "SettingsPage.h"

#include "tk/controls.h"
#include "tk/host.h"

#include <tesseract/types.h>

namespace tesseract::views
{

class MessagesSection : public SettingsPage
{
public:
    explicit MessagesSection(SettingBinder& binder);
    ~MessagesSection() override = default;

    void paint_before_children(tk::PaintCtx& ctx) override;

    // Show the "homeserver doesn't provide link previews" note (MSC4452
    // m.preview_url capability) when `available` is false.
    void set_homeserver_previews_available(bool available);

    // Update the index-stats lines under the checkbox. Shown only while
    // `enabled`; hidden otherwise. Called by the shell on settings-open and on
    // a slow poll while the history backfill runs.
    void set_search_index_stats(const tesseract::SearchIndexStats& stats, bool enabled);

private:
    tk::CheckButton* send_maps_urls_as_location_cb_ = nullptr;
    tk::CheckButton* recompress_cb_ = nullptr;
    tk::CheckButton* show_membership_events_cb_ = nullptr;
    tk::CheckButton* show_sender_status_cb_ = nullptr;
    tk::CheckButton* show_sender_timezone_cb_ = nullptr;
    tk::CheckButton* bundled_url_previews_cb_ = nullptr;
    tk::CheckButton* url_previews_direct_cb_ = nullptr;
    tk::Label* previews_unavailable_label_ = nullptr;
    tk::CheckButton* search_index_cb_ = nullptr;
    tk::Label* search_stats_label_ = nullptr; // counts + status
    tk::Label* search_date_label_ = nullptr;  // "covers messages since …"

    // Cached from paint() so the checkboxes' hover callbacks (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip.
    tk::Host* host_ = nullptr;
};

} // namespace tesseract::views
