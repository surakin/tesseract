#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/Launch.h"
#include <tesseract/crash_handler.h>
#include <tesseract/version.h>
#include "app/MediaPlaybackHub.h"
#include "app/RoomPane.h"
#include "app/RoomWindowBase.h"
#include "app/SearchBackend.h"
#include "app/SlashCommands.h"
#include "app/shell_helpers.h"
#include "app/UnreadPrefetch.h"
#include "app/media_preview_policy.h"
#include "tk/blurhash.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/text_util.h"
#include "tk/theme.h"
#include "views/AddRoomView.h"
#include "views/CreateRoomView.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/JoinRoomView.h"
#include "views/ConfirmDialog.h"
#include "views/MainAppWidget.h"
#include "views/VideoViewerOverlay.h"
#include "views/RoomListView.h"
#include "views/InviteDialog.h"
#include "views/text_util.h"
#include "views/RoomSearchBar.h"
#include "views/SettingsView.h"
#include "views/RoomView.h"
#include "views/UserInfo.h"
#include "tk/image_sniff.h"
#include "views/html_spans.h"
#include "views/image_pack_order.h"
#include "views/map_tiles.h"
#include "views/pronoun_utils.h"
#include "views/thread_unread.h"
#include <tesseract/paths.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/secret_store.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace tesseract
{

// Wire MainAppWidget-level + RoomListView/RoomView/UserInfo providers
// that read from tk_avatars_, tk_images_, anim_cache_, and
// url_preview_data_. Each shell calls this once during construction after
// creating its MainAppWidget. Does NOT touch image_viewer/video_viewer
// (RoomPane::wire_room_view_ owns those, via main_room_pane_) nor
// non-provider callbacks (on_room_selected, on_scroll, on_search_clear,
// etc.) — those touch shell-specific state and stay in the per-shell ctor.
void ShellBase::wire_main_app_widget_(views::MainAppWidget* app)
{
    auto avatar_lookup = [this](const std::string& mxc) -> const tk::Image*
    { return avatar_image_(mxc); };

    app->set_avatar_provider(avatar_lookup);
    if (auto* reminder = app->encryption_reminder())
    {
        reminder->on_open    = [this] { reopen_encryption_setup_(); };
        reminder->on_dismiss = [this] { snooze_encryption_reminder_(); };
    }
    app->on_space_header = [this]
    {
        if (!space_stack_.empty())
            show_space_root_(space_stack_.back());
        else if (!current_room_id_.empty())
            show_space_root_(current_room_id_);
    };
    if (auto* sr = app->space_root())
    {
        sr->on_avatar_needed = [this](const std::string& mxc)
        {
            ensure_media_thumbnail_(mxc, 64, 64, false);
        };
        // Mirrors setup_link_clicked_'s identical RoomView wiring.
        sr->on_link_clicked = [this](const std::string& url)
        {
            if (Client::parse_matrix_link(url).kind != Client::MatrixLink::Kind::Unknown)
                open_matrix_link(url);
            else
                Client::open_in_browser(url);
        };
        // Room-management section (add candidates / space children). Same
        // rooms_-derived source RoomListView uses, just filtered
        // differently — see set_children() forwarding the exclusion set.
        sr->set_candidate_rooms_provider([this]() { return rooms_; });
        sr->on_room_avatar_needed = [this](const tesseract::RoomInfo& r)
        {
            ensure_room_avatar_(r);
        };
        sr->on_add_room_to_space = [this](std::string space_id, std::string room_id)
        {
            request_add_room_to_space_(space_id, room_id);
        };
        sr->on_remove_room_from_space = [this](std::string space_id, std::string room_id)
        {
            request_remove_room_from_space_(space_id, room_id);
        };
        // Not wired: get_cached_unjoined_summaries_() (called from
        // refresh_space_root_children_ below) already proactively fetches
        // every unjoined child's summary whenever it's called — the same
        // mechanism RoomListView's own "Available to join" section relies
        // on — so there is nothing left for this widget-triggered hook to
        // do beyond what refresh_space_root_children_'s triggers already
        // cover.
        sr->on_child_summary_needed = [](std::string) {};
    }
    app->room_list_view()->set_avatar_provider(avatar_lookup);
    // Lazy avatar fetching: the provider above is a pure cache peek, so the
    // room list requests an avatar only when a row is first painted (visible).
    // Avatars for rooms in collapsed / off-screen sections are never fetched.
    app->room_list_view()->on_room_avatar_needed =
        [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
    app->room_list_view()->on_unjoined_room_avatar_needed =
        [this](const tesseract::RoomSummary& s)
        {
            if (!s.avatar_url.empty())
                ensure_media_thumbnail_(s.avatar_url, 64, 64, false);
        };
    app->room_list_view()->on_unjoined_room_summary_needed =
        [this](const std::string& room_id)
        {
            if (active_space_id_.empty() ||
                unjoined_fetch_pending_.count(room_id))
                return;
            auto retry_it = unjoined_fetch_retry_.find(room_id);
            if (retry_it != unjoined_fetch_retry_.end() &&
                std::chrono::steady_clock::now() < retry_it->second.next_retry)
                return;
            unjoined_fetch_pending_.insert(room_id);
            fetch_single_room_summary_(active_space_id_, room_id);
        };

    register_search_backend_();

    // Quick switcher (Ctrl+K): data + activation are shared. The native search
    // field, the keyboard accelerator, and on_close stay per-shell.
    if (auto* qs = app->quick_switcher())
    {
        qs->set_rooms_provider(
            [this]() -> std::vector<tesseract::RoomInfo>
            {
                auto v = rooms_;
                std::sort(v.begin(), v.end(),
                          [](const tesseract::RoomInfo& a,
                             const tesseract::RoomInfo& b)
                          { return a.last_activity_ts > b.last_activity_ts; });
                return v;
            });
        qs->set_recent_provider(
            [this]() -> std::vector<tesseract::RoomInfo>
            {
                std::vector<tesseract::RoomInfo> out;
                out.reserve(recent_room_ids_.size());
                for (const auto& id : recent_room_ids_)
                {
                    auto it = std::find_if(
                        rooms_.begin(), rooms_.end(),
                        [&](const tesseract::RoomInfo& r) { return r.id == id; });
                    if (it != rooms_.end())
                        out.push_back(*it);
                }
                return out;
            });
        qs->on_room_avatar_needed =
            [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
        qs->on_room_selected =
            [this](const std::string& room_id) { tab_select_room(room_id); };

        // User mode ('@'): filter the known-user roster + live-resolve a typed
        // mxid (on_user_query_changed), open/create the DM (on_user_selected),
        // and lazily fetch user-row avatars.
        qs->on_user_query_changed =
            [this](const std::string& q) { handle_user_query_(q); };
        qs->on_user_selected =
            [this](const std::string& mxid, const std::string& reason)
            { handle_open_dm_(mxid, reason); };
        qs->on_user_avatar_needed =
            [this](const std::string& mxc) { ensure_user_avatar_(mxc); };
    }

    // MRU room switcher (Ctrl+Tab / Ctrl+Shift+Tab): reuses the exact same
    // recent_room_ids_-backed provider as the Quick Switcher's Recent strip
    // above. The cycle lifecycle (begin/advance/commit/cancel) and native
    // key handling live in MainAppWidget/each shell; this just supplies data
    // + the room-switch action.
    if (auto* mru = app->mru_switcher())
    {
        mru->set_recent_provider(
            [this]() -> std::vector<tesseract::RoomInfo>
            {
                std::vector<tesseract::RoomInfo> out;
                out.reserve(recent_room_ids_.size());
                for (const auto& id : recent_room_ids_)
                {
                    auto it = std::find_if(
                        rooms_.begin(), rooms_.end(),
                        [&](const tesseract::RoomInfo& r) { return r.id == id; });
                    if (it != rooms_.end())
                        out.push_back(*it);
                }
                return out;
            });
        mru->on_room_avatar_needed =
            [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
        mru->on_room_selected =
            [this](const std::string& room_id) { tab_select_room(room_id); };
    }

    // Message search (Ctrl+Shift+F): the query runs against the local FTS
    // index on the active account; results jump to the message. The native
    // search field, keyboard accelerator and on_close stay per-shell.
    if (auto* ms = app->message_search())
    {
        ms->on_query_changed =
            [this](const std::string& q) { handle_search_query_(q); };
        ms->on_result_activated =
            [this](const std::string& room_id, const std::string& event_id)
            { handle_search_result_activated_(room_id, event_id); };
    }

    // Per-room "find in conversation" (Ctrl+F / Cmd+F): callbacks forwarded
    // from RoomView → ShellBase. The native text field, keyboard accelerator,
    // and on_close stay per-shell.
    if (auto* rv = app->room_view())
    {
        rv->on_room_search_query =
            [this](const std::string& q) { handle_in_room_search_query_(q); };
        rv->on_room_search_navigate =
            [this](int delta) { in_room_search_navigate_(delta); };
        rv->on_room_search_paginate_toggled =
            [this](bool enabled) { set_in_room_search_paginate_(enabled); };
        rv->on_room_search_closed = [this]()
        {
            in_room_search_clear_();
            ++status_msg_gen_; // cancel any queued "Fetching…" display post
            status_override_active_ = false;
            on_restore_status_ui_();
        };

        // Find-in-thread: same forwarding shape as the in-room search above,
        // but ThreadView (and thus its search bar) is created lazily by
        // set_thread_panel(), so these fields are wired once here and
        // ThreadView's own on_close-style forwarding (in set_thread_panel())
        // routes whichever instance exists back through them.
        rv->on_thread_search_query =
            [this](const std::string& q) { handle_thread_search_query_(q); };
        rv->on_thread_search_navigate =
            [this](int delta) { thread_search_navigate_(delta); };
    }

    app->room_list_view()->set_sticker_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_sticker_(mxc);
        });

    app->room_list_view()->set_media_allowed_provider(
        [this](const std::string& room_id, bool is_own) -> bool
        {
            return media_allowed_(room_id, is_own);
        });

    // Room-list search field. The field is a plain shared tk::TextField created
    // by RoomListView's constructor (host is already attached here), so all four
    // shells share this wiring: debounce the typed query into set_search_text()
    // then refresh_room_list_(), which both widens RoomListView to the full
    // rooms_ set (is_room_search_active_() is now true) and schedules the
    // relayout/repaint. on_search_clear resets everything the same way.
    if (auto* sf = app->room_list_view()->search_field())
    {
        sf->set_on_changed(
            [this](const std::string& q)
            {
                room_search_text_ = q;
                debounce_(DebounceSlot::RoomSearch,
                          views::RoomListView::kSearchDebounceMs,
                          [this]
                          {
                              if (!main_app_)
                                  return;
                              main_app_->room_list_view()->set_search_text(
                                  room_search_text_);
                              refresh_room_list_();
                          });
            });
    }
    app->room_list_view()->on_search_clear = [this]
    {
        cancel_debounce_(DebounceSlot::RoomSearch);
        room_search_text_.clear();
        if (main_app_)
        {
            if (auto* sf = main_app_->room_list_view()->search_field())
                sf->set_text("");
            main_app_->room_list_view()->set_search_text("");
        }
        refresh_room_list_();
    };

    // Restore section collapsed state from the previous session.
    {
        auto& s = tesseract::Settings::instance();
        // Indexed by section id, not positional: sections have been inserted
        // mid-enum before (kSecCallRooms), which silently shifted the mapping.
        bool init[views::RoomListView::kNumSections] = {};
        init[views::RoomListView::kSecInvites]       = s.room_section_invites_collapsed;
        init[views::RoomListView::kSecUnread]        = s.room_section_unread_collapsed;
        init[views::RoomListView::kSecFavorites]     = s.room_section_favorites_collapsed;
        init[views::RoomListView::kSecDMs]           = s.room_section_dms_collapsed;
        init[views::RoomListView::kSecRooms]         = s.room_section_rooms_collapsed;
        init[views::RoomListView::kSecSpaces]        = s.room_section_spaces_collapsed;
        init[views::RoomListView::kSecInactive]      = s.room_section_inactive_collapsed;
        init[views::RoomListView::kSecSpaceUnjoined] = s.room_section_space_unjoined_collapsed;
        for (int sec = 0; sec < views::RoomListView::kNumSections; ++sec)
            app->room_list_view()->set_section_collapsed(sec, init[sec]);
    }
    app->room_list_view()->on_section_toggled =
        [](int section, bool collapsed)
    {
        auto& s = tesseract::Settings::instance();
        switch (section)
        {
        case views::RoomListView::kSecInvites:
            s.room_section_invites_collapsed   = collapsed; break;
        case views::RoomListView::kSecUnread:
            s.room_section_unread_collapsed    = collapsed; break;
        case views::RoomListView::kSecFavorites:
            s.room_section_favorites_collapsed = collapsed; break;
        case views::RoomListView::kSecDMs:
            s.room_section_dms_collapsed       = collapsed; break;
        case views::RoomListView::kSecRooms:
            s.room_section_rooms_collapsed     = collapsed; break;
        case views::RoomListView::kSecSpaces:
            s.room_section_spaces_collapsed    = collapsed; break;
        case views::RoomListView::kSecInactive:
            s.room_section_inactive_collapsed       = collapsed; break;
        case views::RoomListView::kSecSpaceUnjoined:
            s.room_section_space_unjoined_collapsed = collapsed; break;
        default: break;
        }
        s.save_to_disk(tesseract::config_dir());
    };

    // Restore the resizable sidebar width + icon-only state from last session,
    // and persist changes (debounced — a drag fires continuously).
    {
        auto& s = tesseract::Settings::instance();
        app->set_sidebar_width(s.sidebar_width > 0
                                   ? static_cast<float>(s.sidebar_width)
                                   : static_cast<float>(tesseract::visual::kSidebarWidth));
        app->set_sidebar_collapsed(s.sidebar_collapsed);
    }
    app->on_sidebar_layout_changed = [this](float width, bool collapsed)
    {
        auto& s = tesseract::Settings::instance();
        s.sidebar_width     = static_cast<int>(std::lround(width));
        s.sidebar_collapsed = collapsed;
        save_settings_debounced_();
    };

    app->room_view()->set_avatar_provider(avatar_lookup);
    app->room_view()->on_room_avatar_needed =
        [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
    // Visible-row media prioritization: when the timeline's visible rows change,
    // move their still-pending downloads to the front of the queue.
    app->room_view()->on_visible_range_changed =
        [this](const std::vector<std::string>& keys)
    { on_visible_rows_changed_(keys); };
    // Lazy avatar fetch: only request avatars for currently-visible rows so
    // switching rooms doesn't kick off fetches for the entire room history.
    app->room_view()->on_visible_avatars_changed =
        [this](const std::vector<std::string>& urls)
    {
        for (const auto& url : urls)
            ensure_user_avatar_(url, active_media_group_);
    };
    app->room_view()->set_image_provider(
        [this](const std::string& mxc, bool hovered) -> const tk::Image*
        {
            // "thumb::"-prefixed keys are the client-generated video-
            // thumbnail sentinel (see make_row_data), never a real mxc://
            // or JSON MediaSource — fetching one here would just fail and
            // land the key in backoff. paint_video_card's
            // on_video_thumbnail_needed handles regenerating those instead.
            if (mxc.starts_with("thumb::"))
            {
                return account_manager_.image_cache().peek(
                    tk::CacheKey::video_thumbnail(mxc.substr(7)));
            }
            // "tile:"-prefixed keys are OSM map tiles (see
            // LocationMapPanner's shared use of this same provider via
            // MessageListView::image_provider_) — a different namespace
            // within image_cache_ than a plain mxc://.
            if (mxc.starts_with("tile:"))
            {
                return account_manager_.image_cache().peek(
                    tk::CacheKey{tk::CacheUsage::Tile, mxc.substr(5)});
            }
            // "blurhash::"-prefixed keys are the synthetic decoded-blurhash
            // placeholder (see ensure_blurhash_image_) — event_id-keyed, not
            // a real mxc://.
            if (mxc.starts_with("blurhash::"))
            {
                return account_manager_.image_cache().peek(
                    tk::CacheKey::blurhash(mxc.substr(10)));
            }
            const tk::CacheKey key = tk::CacheKey::media(mxc);
            gate_anim_playback_(key, hovered);
            if (const auto* f = account_manager_.anim_cache().current_frame(key))
            {
                start_anim_tick_();
                return f;
            }
            if (const auto* img = account_manager_.image_cache().peek(key))
                return img;
            if (const auto* img = account_manager_.thumbnail_cache().peek(key))
                return img;
            // Cache miss after eviction — re-fetch. Deduplicated by the
            // in-flight set; uses the disk cache when bytes were previously
            // downloaded, so re-display is usually instant.
            ensure_media_image_(mxc, visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight,
                                media_group_for_room_(current_room_id_));
            return nullptr;
        });
    // Stickers must always decode/cache at kStickerSize, never the generic
    // inline-image bound above — see make_sticker_image_provider_'s comment.
    if (auto* ml = app->room_view()->message_list())
        ml->set_sticker_image_provider(make_sticker_image_provider_());
    // MSC4278: gate inline media behind the media-preview config + reveal set.
    wire_media_preview_gating_(app->room_view()->message_list());
    // Retry/abort a failed outgoing message's hover actions (and the
    // identity-change banner) are wired by main_room_pane_->attach()
    // (RoomPane::wire_room_view_), called before this function on every
    // shell — do not re-wire on_retry_send/on_abort_send here, it would
    // silently clobber RoomPane's handlers.
    if (auto* ml = app->room_view()->message_list())
    {
        ml->on_video_thumbnail_needed =
            [this](const std::string& event_id, const std::string& source_token)
        {
            request_video_thumbnail_(event_id, source_token);
        };

        // MPRIS (GtkMprisPlayer / QtMprisPlayer): forward every voice/audio
        // playback state change into the process-wide MediaPlaybackHub, with
        // controls bound back to this room's TimelineMediaController so
        // Play/Pause/Seek always target whichever ShellBase most recently
        // reported (see MediaPlaybackHub.h).
        ml->set_playback_observer(
            [this, ml](const views::TimelineMediaController::PlaybackSnapshot& snap)
            {
                auto& hub = account_manager_.media_playback_hub();
                if (snap.event_id.empty())
                {
                    hub.report_stopped();
                    return;
                }
                const auto* row = ml->row_for_event_id(snap.event_id);
                NowPlaying np;
                np.kind = row && row->kind == views::MessageRowData::Kind::Voice
                              ? NowPlaying::Kind::Voice
                              : NowPlaying::Kind::Audio;
                np.room_id    = current_room_id_;
                np.event_id   = snap.event_id;
                np.title      = row && !row->body.empty() ? row->body
                                                          : tk::tr("Voice message");
                np.artist     = row ? row->sender_name : std::string{};
                np.position_ms = snap.position_ms;
                np.duration_ms = snap.duration_ms;
                np.is_playing  = snap.is_playing;

                MediaPlaybackHub::Controls ctl;
                ctl.play = [ml] { ml->playback_controller().resume_active(); };
                ctl.pause = [ml] { ml->playback_controller().pause_active(); };
                ctl.play_pause =
                    [ml] { ml->playback_controller().toggle_active_playback(); };
                ctl.stop = [ml] { ml->playback_controller().stop_active_playback(); };
                ctl.seek = [ml](std::int64_t off)
                { ml->playback_controller().seek_active(off); };
                hub.report(std::move(np), std::move(ctl));
            });
    }
    app->room_view()->set_preview_provider(
        [this](const std::string& url) -> const views::UrlPreviewData*
        {
            auto it = url_preview_data_.find(url);
            if (it == url_preview_data_.end())
            {
                return nullptr;
            }
            if (it->second.image_source)
            {
                const std::string& key = it->second.image_source->fetch_token();
                const tk::CacheKey mem_key = tk::CacheKey::media(key);
                if (!account_manager_.image_cache().contains(mem_key) &&
                    !account_manager_.anim_cache().has(mem_key))
                {
                    ensure_media_image_(key, 64, 64);
                }
            }
            return &it->second;
        });

    app->user_info()->set_image_provider(avatar_lookup);
    // The strip shows the user's own avatar in the active room (see
    // strip_avatar_url_), resolved on every paint.
    app->user_info()->set_avatar_url_provider([this] { return strip_avatar_url_(); });
    // Lazy avatar fetch: avatar_lookup above is a pure cache peek, so request
    // the user's own avatar whenever the strip paints with a miss — mirrors
    // room_list_view's on_room_avatar_needed and self-heals after a cache
    // flush (e.g. a display scale change) without needing an explicit
    // populate_user_strip() call.
    app->user_info()->on_avatar_needed =
        [this](const std::string& mxc) { ensure_user_avatar_(mxc); };
    app->on_settings_shortcut = [this] { open_app_settings_ui_(); };
    // Keyboard: Enter on the strip opens the account picker when there's
    // more than one account to pick from (the click's behaviour), else the
    // user menu (Settings, Add account, ...), so a single-account user's
    // Enter isn't a dead key.
    app->user_info()->set_keyboard_focusable(true);
    app->user_info()->on_keyboard_activate = [this, app]
    {
        views::UserInfo* ui = app->user_info();
        const tk::Rect b = ui->bounds();
        const tk::Point centre{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
        if (account_manager_.accounts().size() >= 2 && ui->on_primary)
            ui->on_primary(centre);
        else if (ui->on_secondary)
            ui->on_secondary(centre);
    };

    // MSC4426: the sidebar strip (not AccountPicker rows) shows a third line
    // for the user's own status, with a "Click to set status" placeholder
    // when unset. A click on that line opens Settings → Account. The
    // placeholder and click are suppressed until server_info_ confirms
    // MSC4133 profile-field support (push_own_status_to_strip_()).
    app->user_info()->set_status_line_enabled(true);
    app->user_info()->on_status_clicked = [this]
    { open_settings_to_account_tab_(); };
    app->user_info()->set_status_editable(
        server_info_.supports_profile_fields &&
        server_info_.profile_fields_enabled);
    app->user_info()->set_status(own_extended_profile_.status_emoji,
                                 own_extended_profile_.status_text);

    auto presence_lookup = [this](const std::string& uid) -> PresenceState
    {
        return presence_for_(uid);
    };
    app->room_list_view()->set_presence_provider(presence_lookup);
    app->room_view()->room_info_panel()->set_presence_provider(presence_lookup);
    // Lazy avatar fetching: image_provider above (avatar_lookup) is a pure
    // cache peek, so the panel requests a member's avatar only when their row
    // is actually visible in the open panel — mirrors room_list_view's
    // on_room_avatar_needed.
    app->room_view()->room_info_panel()->on_member_avatar_needed =
        [this](const tesseract::RoomMember& m)
    {
        ensure_user_avatar_(m.avatar_url, media_group_for_room_(current_room_id_));
    };

    // Room media gallery (open/close/pagination/thumbnail fetching) already
    // provided by main_room_pane_->attach() above (RoomPane::wire_room_view_),
    // using RoomPane's own media_view_group_ instead of this window's
    // now-removed copy.

    // Avatar click in UserProfilePanel → open the image viewer. The list only
    // holds an ≤80px thumbnail, so kick a full-size fetch into account_manager_.image_cache();
    // the viewer's image_provider returns the thumbnail instantly and swaps to
    // full-res when it arrives.
    app->room_view()->on_avatar_clicked =
        [this, app](std::string url, std::string name)
    {
        if (url.empty() || !app->image_viewer())
            return;
        ensure_viewer_fullres_(url);
        app->image_viewer()->open(url, url, name, 0, 0);
        app->show_image_viewer(true);
        // Trigger the shell-wired relayout so the surface repaints with the
        // viewer visible (mirrors the per-shell on_image_clicked path).
        if (app->room_view()->on_layout_changed)
            app->room_view()->on_layout_changed();
    };

    app->room_view()->on_fetch_notification_mode =
        [this, app](std::string room_id)
    {
        if (!client_) return;
        auto sess = active_account_;
        run_async_([this, app, sess, room_id = std::move(room_id)]() mutable {
            if (!sess || !sess->client) return;
            auto mode = sess->client->get_room_notification_mode(room_id);
            post_to_ui_alive_([app, mode = std::move(mode)]() mutable {
                app->room_view()->room_info_panel()->set_notification_mode(
                    std::move(mode));
            });
        });
    };
    app->room_view()->on_notification_mode_changed =
        [this](std::string room_id, std::string mode)
    {
        set_room_notification_mode_(room_id, mode);
    };
    app->room_view()->on_favourite_changed =
        [this](std::string room_id, bool on)
    {
        set_room_favourite_(room_id, on);
    };
    app->room_view()->on_low_priority_changed =
        [this](std::string room_id, bool on)
    {
        set_room_low_priority_(room_id, on);
    };

    // ── Invite selection and action wiring ────────────────────────────────
    app->room_list_view()->on_invite_selected =
        [this, app, avatar_lookup](const std::string& room_id)
    {
        const InviteInfo* inv = find_invite_(room_id);
        if (!inv)
            return;
        current_invite_ = InviteContext{ inv->room_id, inv->inviter_user_id };
        app->show_invite(*inv, avatar_lookup);
        app->room_list_view()->set_selected_room(""); // clear room highlight
        request_relayout_();
    };

    app->invite_card()->on_accept = [this]
    {
        if (current_invite_) accept_invite_async_(current_invite_->room_id);
    };
    app->invite_card()->on_decline = [this]
    {
        if (current_invite_) decline_invite_async_(current_invite_->room_id);
        if (main_app_)
            main_app_->clear_content();
        request_relayout_();
    };
    app->invite_card()->on_block = [this]
    {
        if (current_invite_)
            block_invite_async_(current_invite_->room_id, current_invite_->inviter_id);
        if (main_app_)
            main_app_->clear_content();
        request_relayout_();
    };

    // ── Knock (MSC2403) row selection and cancel wiring ────────────────────
    app->room_list_view()->on_knock_row_selected =
        [this, app, avatar_lookup](const std::string& room_id)
    {
        const KnockedRoomInfo* k = find_my_knock_(room_id);
        if (!k)
            return;
        current_knock_status_room_id_ = k->room_id;
        app->show_knock_status(*k, avatar_lookup);
        app->room_list_view()->set_selected_room(""); // clear room highlight
        request_relayout_();
    };

    app->knock_status_card()->on_cancel = [this]
    {
        if (!current_knock_status_room_id_.empty())
            retract_knock_command_(current_knock_status_room_id_);
        if (main_app_)
            main_app_->clear_content();
        request_relayout_();
    };

    // Forward picker: stable providers wired once so open() always has rooms.
    // The native text field, keyboard accelerator, and on_close stay per-shell.
    if (auto* fp = app->forward_picker())
    {
        fp->set_rooms_provider(
            [this]() -> std::vector<tesseract::RoomInfo> { return rooms_; });
        fp->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image*
            { return avatar_image_(mxc); });
        fp->on_room_avatar_needed =
            [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
        fp->on_close = [this] { hide_forward_picker_field_(); request_relayout_(); };
    }

    // Add Room dialog (Join + Create tabs): the room-list "+" button opens
    // it defaulting to the Join tab; all async plumbing for both tabs is
    // wired here, non-virtually, mirroring how the forward picker above is
    // wired — no shell needs its own copy of any of this.
    app->room_list_view()->on_add_room_requested = [this]
    {
        if (!main_app_) return;
        auto* ar = main_app_->add_room_view();
        // Refreshed here rather than once at wire-up time: client_ is null
        // until login completes, well after wire_main_app_widget_() runs.
        if (auto* dr = ar->directory_view(); dr && client_)
        {
            const std::string uid = client_->get_user_id();
            const auto colon = uid.find(':');
            dr->set_homeserver_placeholder(
                colon == std::string::npos ? std::string{} : uid.substr(colon + 1));
        }
        ar->open();
    };
    if (auto* ar = app->add_room_view())
    {
        if (auto* jr = ar->join_view())
        {
            jr->set_avatar_provider(
                [this](const std::string& mxc) -> const tk::Image*
                { return avatar_image_(mxc); });
            jr->on_lookup_requested =
                [this](const std::string& alias) { lookup_room_command_(alias); };
            jr->on_join_requested =
                [this](const std::string& id) { join_room_command_(id); };
            jr->on_knock_requested =
                [this](const std::string& id, const std::string& reason)
                { knock_room_command_(id, reason); };
            jr->on_link_clicked =
                [this](std::string url)
                {
                    if (Client::parse_matrix_link(url).kind !=
                        Client::MatrixLink::Kind::Unknown)
                        open_matrix_link(url);
                    else
                        Client::open_in_browser(url);
                };
        }
        if (auto* cr = ar->create_view())
        {
            cr->on_create_requested =
                [this](tesseract::RoomCreateOptions options)
                { create_room_command_(options); };
        }
        if (auto* dr = ar->directory_view())
        {
            dr->set_avatar_provider(
                [this](const std::string& mxc) -> const tk::Image*
                { return avatar_image_(mxc); });
            // Homeserver placeholder is (re-)set from on_add_room_requested
            // above, not here — client_ is still null at this point (this
            // wiring runs once at shell construction, before login).
            dr->on_avatar_needed =
                [this](const std::string& mxc)
                { ensure_media_thumbnail_(mxc, 96, 96, false); };
            dr->on_search_requested =
                [this](std::uint64_t request_id, const std::string& filter,
                       const std::string& server)
                {
                    if (client_)
                        client_->search_room_directory(request_id, filter, server);
                };
            dr->on_next_page_requested =
                [this](std::uint64_t request_id)
                {
                    if (client_) client_->room_directory_next_page(request_id);
                };
            dr->set_is_room_joined(
                [this](const std::string& id)
                {
                    return std::any_of(rooms_.begin(), rooms_.end(),
                                       [&](const tesseract::RoomInfo& r)
                                       { return r.id == id; });
                });
            dr->on_join_requested =
                [this, ar](const std::string& id, const std::string& via_server)
                {
                    // Already in this room — the button read "Go", so
                    // switch to it instead of re-issuing a join.
                    if (std::any_of(rooms_.begin(), rooms_.end(),
                                    [&](const tesseract::RoomInfo& r)
                                    { return r.id == id; }))
                    {
                        ar->close();
                        tab_select_room(id);
                    }
                    else
                    {
                        // The room was found by browsing via_server's
                        // directory (or, if empty, our own homeserver's) —
                        // our homeserver otherwise has no route to a room it
                        // doesn't already know, so pass it as a join hint.
                        std::vector<std::string> via;
                        if (!via_server.empty()) via.push_back(via_server);
                        join_room_command_(id, std::move(via));
                    }
                };
            // Every keystroke in the server/search fields debounces into a
            // fresh search; losing focus, pressing Enter, or clicking
            // "Search" (wired inside RoomDirectoryView itself) all bypass
            // this and call search_now() immediately instead.
            dr->on_field_edited = [this]
            {
                debounce_(DebounceSlot::RoomDirectorySearch,
                          views::RoomDirectoryView::kSearchDebounceMs,
                          [this]
                          {
                              if (!main_app_) return;
                              if (auto* ar2 = main_app_->add_room_view())
                                  if (auto* dr2 = ar2->directory_view())
                                      dr2->search_now();
                          });
            };
        }
        ar->on_close = [this, ar]
        {
            cancel_debounce_(DebounceSlot::RoomDirectorySearch);
            if (client_ && ar->directory_view())
                client_->cancel_room_directory_search(
                    ar->directory_view()->active_request_id());
            request_relayout_();
        };
    }
    if (auto* rv = app->room_view())
    {
        rv->on_forward_requested =
            [this](const std::string& event_id)
        {
            auto* fp = main_app_ ? main_app_->forward_picker() : nullptr;
            if (!fp || current_room_id_.empty() || fp->is_open())
                return;
            fp->on_confirmed =
                [this, source_room = current_room_id_, event_id]
                (std::vector<std::string> room_ids)
            {
                if (!client_) return;
                auto* fp_ptr = main_app_ ? main_app_->forward_picker() : nullptr;
                if (!fp_ptr) return;
                fp_ptr->set_forwarding(static_cast<int>(room_ids.size()));
                for (const auto& rid : room_ids)
                {
                    const auto req_id = next_request_id_++;
                    pending_forwards_[req_id] = rid;
                    client_->forward_event(req_id, source_room, event_id, rid);
                }
            };
            fp->open(current_room_id_);
            focus_forward_picker_field_();
            request_relayout_();
        };

        rv->on_start_call =
            [this](const std::string& room_id, const std::string& slot_id,
                   bool audio_only)
        {
            request_call_(room_id, slot_id, audio_only);
        };
    }

    // The shell is fully constructed and the widget tree is up: begin the ~2 s
    // image-GC tick (see run_image_gc_ / media_sweep_timer_).
    media_sweep_timer_.start();
}

// Wire every SettingsView callback whose body is pure Settings
// persistence or a forward into an existing ShellBase handler — i.e.
// everything that does NOT need a Surface/Host/native dialog. Each shell
// calls this once, right after constructing its SettingsView, then wires
// only what's left: on_close/on_logout/on_reset_identity (differ in how
// the settings surface is dismissed), on_tab_changed (needs the shell's
// Surface), and audio/camera/mic device enumeration (needs tk::Host —
// though the *_changed callbacks themselves are wired here).
void ShellBase::wire_settings_view_(views::SettingsView* view)
{
    if (!view)
        return;

    view->emoji_skin_tone_provider = [this] { return emoji_skin_tone_(); };
    view->on_emoji_skin_tone_changed = [this](tesseract::emoji::SkinTone tone)
    {
        set_emoji_skin_tone_(tone);
    };
    view->on_theme_preference_changed =
        [this](tesseract::Settings::ThemePreference pref)
    {
        set_theme_preference_(pref);
    };
    view->on_theme_accent_changed =
        [this](tesseract::Settings::ThemeAccent accent)
    {
        set_theme_accent_(accent);
    };
    view->on_low_power_preference_changed =
        [this](tesseract::Settings::LowPowerPreference pref)
    {
        set_low_power_preference_(pref);
    };
    view->on_notifications_changed = [this](bool enabled)
    {
        if (settings_controller_)
            settings_controller_->set_notifications_enabled(enabled);
    };
    view->on_hide_content_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_hide_content = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    view->on_image_previews_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_image_previews = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    view->on_prefetch_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.prefetch_full_media = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    view->on_autoscroll_unread_changed = [](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.autoscroll_unread_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    view->on_group_inactive_changed = [this](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.group_inactive_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
        refresh_room_list_();
    };
    view->on_group_unread_changed = [this](bool enabled)
    {
        auto& s = tesseract::Settings::instance();
        s.group_unread_rooms = enabled;
        s.save_to_disk(tesseract::config_dir());
        refresh_room_list_();
    };
    view->on_inactive_period_changed = [this](int days)
    {
        auto& s = tesseract::Settings::instance();
        s.inactive_room_threshold_days = days;
        s.save_to_disk(tesseract::config_dir());
        refresh_room_list_();
    };
    view->on_show_membership_events_changed = [this](bool enabled)
    {
        handle_show_membership_events_toggle_(enabled);
    };
    view->on_show_sender_status_changed = [this](bool enabled)
    {
        handle_show_sender_status_toggle_(enabled);
    };
    view->on_animate_avatars_changed = [this](bool enabled)
    {
        handle_animate_avatars_toggle_(enabled);
    };
    view->on_launch_at_login_changed = [this](bool enabled)
    {
        handle_launch_at_login_toggle_(enabled);
    };
    view->on_start_minimized_changed = [this](bool enabled)
    {
        handle_start_minimized_toggle_(enabled);
    };
    view->on_close_action_changed = [this](tesseract::Settings::CloseAction action)
    {
        handle_close_action_toggle_(action);
    };
    view->on_send_presence_changed = [this](bool enabled)
    {
        handle_send_presence_toggle_(enabled);
    };
    view->on_index_messages_changed = [this](bool enabled)
    {
        handle_index_messages_toggle_(enabled);
    };
#ifdef TESSERACT_UPDATE_CHECKS
    view->on_check_for_updates_changed = [this](bool enabled)
    {
        handle_check_for_updates_toggle_(enabled);
    };
#endif
    view->on_msc2545_legacy_compat_changed = [this](bool enabled)
    {
        handle_msc2545_legacy_compat_toggle_(enabled);
    };
    view->on_developer_mode_changed = [this](bool enabled)
    {
        handle_developer_mode_toggle_(enabled);
    };
    view->on_open_activity_monitor = [this] { open_activity_monitor_(); };
    view->on_message_layout_changed =
        [this](tesseract::Settings::MessageLayout layout)
    {
        handle_message_layout_changed_(layout);
    };
#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    view->on_crash_reporting_changed = [this](bool enabled)
    {
        handle_crash_reporting_toggle_(enabled);
    };
#endif
    view->on_send_maps_urls_as_location_changed = [this](bool enabled)
    {
        handle_send_maps_urls_as_location_toggle_(enabled);
    };
    view->on_exclude_insecure_devices_changed = [](bool enabled)
    {
        // Persist only: the mode is read when a client is built, so it
        // applies from the next launch (see Settings::exclude_insecure_devices).
        auto& s = tesseract::Settings::instance();
        s.exclude_insecure_devices = enabled;
        s.save_to_disk(tesseract::config_dir());
    };
    view->set_proxy_restart_pending(tesseract::proxy_changed_since_launch(
        tesseract::Settings::instance().proxy_mode,
        tesseract::Settings::instance().proxy_url));
    view->on_proxy_changed = [view](tesseract::Settings::ProxyMode mode, std::string url)
    {
        // Persist only: the proxy is applied at launch, so it takes effect
        // after a restart.
        auto& s = tesseract::Settings::instance();
        s.proxy_mode = mode;
        s.proxy_url = std::move(url);
        s.save_to_disk(tesseract::config_dir());
        view->set_proxy_restart_pending(
            tesseract::proxy_changed_since_launch(s.proxy_mode, s.proxy_url));
    };
    view->on_bundled_url_previews_changed = [this](bool enabled, bool direct)
    {
        handle_bundled_url_previews_toggle_(enabled, direct);
    };
    view->on_media_previews_changed =
        [this](tesseract::Settings::MediaPreviews mode)
    {
        apply_media_preview_config_(
            mode, tesseract::Settings::instance().invite_avatars);
    };
    view->on_invite_avatars_changed = [this](bool enabled)
    {
        apply_media_preview_config_(
            tesseract::Settings::instance().media_previews, enabled);
    };
    view->on_audio_input_changed = [](std::string id)
    {
        auto& s = tesseract::Settings::instance();
        s.audio_input_device_id = std::move(id);
        s.save_to_disk(tesseract::config_dir());
    };
    view->on_audio_output_changed = [](std::string id)
    {
        auto& s = tesseract::Settings::instance();
        s.audio_output_device_id = std::move(id);
        s.save_to_disk(tesseract::config_dir());
    };
    view->on_camera_changed = [](std::string id)
    {
        auto& s = tesseract::Settings::instance();
        s.camera_device_id = std::move(id);
        s.save_to_disk(tesseract::config_dir());
    };
    view->set_language_restart_pending(
        tesseract::Settings::instance().language != tesseract::launch_language());
    view->on_language_changed = [view](std::string code)
    {
        auto& s = tesseract::Settings::instance();
        s.language = std::move(code);
        s.save_to_disk(tesseract::config_dir());
        view->set_language_restart_pending(s.language != tesseract::launch_language());
    };
    view->on_restart_requested = [this] { restart_app_(); };
    view->on_about_tab_shown = [this] { refresh_cache_sizes_poll_(); };
    view->on_clear_caches = [this, view]
    {
        clear_all_caches_([view](uint64_t local, uint64_t sdk, uint64_t memory,
                                 uint64_t mh, uint64_t mm, uint64_t dh,
                                 uint64_t dm)
        {
            if (view)
                view->set_cache_sizes(local, sdk, memory, mh, mm, dh, dm);
        });
    };
}

void ShellBase::wire_settings_controller_common_(
    views::SettingsView* view, tesseract::SettingsController* ctrl,
    std::function<void()> relayout)
{
    if (!view || !ctrl)
        return;

    view->set_controller(ctrl);

    view->on_avatar_upload_requested = [this]
    {
        if (settings_controller_) settings_controller_->upload_avatar();
    };
    view->on_avatar_remove_requested = [this]
    {
        if (settings_controller_) settings_controller_->remove_avatar();
    };
    view->on_profile_field_changed =
        [this](std::string key, std::string value_json)
    {
        handle_profile_field_change_(key, value_json);
    };

    ctrl->on_avatar_changed = [this, view, relayout](std::string mxc)
    {
        my_avatar_url_ = mxc;
        if (active_account_)
            active_account_->avatar_url = my_avatar_url_;
        view->set_avatar_url(mxc);
        if (relayout) relayout();
        refresh_user_strip_();
    };
}

// Wire voice-capture callbacks onto rv. Call once per shell after capture_
// is initialised (not from RoomWindowBase::wire_room_view_() — pop-out
// windows hide the mic button instead). `request_repaint` is called each
// time an amplitude sample arrives. `get_room_id` is invoked when the user
// starts recording so the message targets the room active at that moment,
// not when the callback was registered. `clear_text_fn` clears the compose
// field (and any native text widget) after a successful voice send.
void ShellBase::wire_voice_capture_(
    views::RoomView*             rv,
    std::function<void()>        request_repaint,
    std::function<std::string()> get_room_id,
    std::function<void()>        clear_text_fn)
{
    rv->compose_bar()->set_mic_available(capture_ != nullptr);
    if (!capture_)
        return;

    // on_mic_clicked: start or stop recording. When starting, snapshot the
    // target room so a mid-recording room switch doesn't mis-deliver the
    // voice message. Assign on_stopped fresh each start so it carries the
    // correct room_id.
    rv->on_mic_clicked =
        [this, rv, get_room_id, clear_text_fn, request_repaint]() mutable
    {
        auto* cb = rv->compose_bar();
        if (!capture_->is_recording())
        {
            const std::string rid = get_room_id();
            // Assign on_stopped before start() so WASAPI init failures that
            // call fire_error_() synchronously see a valid callback and can
            // reset the UI rather than being silently dropped.
            capture_->on_stopped =
                [this, rv, rid, clear_text_fn, request_repaint](
                    std::vector<std::uint8_t>  pcm,
                    std::vector<std::uint16_t> waveform,
                    std::uint64_t              duration_ms) mutable
            {
                auto* cb2 = rv->compose_bar();
                cb2->set_recording(false);
                request_repaint();
                if (pcm.empty() || duration_ms < 500)
                    return;

                // Capture UI state and clear compose bar before going async,
                // so the user can start a new recording immediately.
                std::string caption  = cb2->current_text();
                std::string reply_id = cb2->reply_event_id();
                cb2->clear_reply();
                clear_text_fn();

                // Encoding and upload run on the SDK runtime. A process-wide
                // request ID lets platform shells aggregate taskbar progress.
                auto sess = active_account_;
                const auto request_id = account_manager_.next_upload_request_id();
                run_async_mut_(
                    [sess, request_id, rid,
                     pcm = std::move(pcm), waveform = std::move(waveform),
                     duration_ms, caption, reply_id]() mutable
                    {
                        if (!sess || !sess->client) return;
                        const std::uint64_t est = duration_ms * 3;
                        const std::uint64_t limit =
                            sess->client->media_upload_limit();
                        if (limit > 0 && est > limit)
                            return;
                        sess->client->send_voice_async(
                            request_id, rid, pcm.data(), pcm.size(), duration_ms,
                            waveform, caption, reply_id);
                    });
            };
            capture_->start();
            if (!capture_->is_recording())
                return; // WASAPI init failed; fire_error_ queued on_stopped({},{},0)
            cb->set_recording(true);
            request_repaint();
        }
        else
        {
            capture_->stop();
        }
    };

    rv->on_cancel_voice = [this, rv, request_repaint]()
    {
        if (!capture_)
            return;
        capture_->cancel();
        rv->compose_bar()->set_recording(false);
        request_repaint();
    };

    capture_->on_amplitude = [rv, request_repaint](std::uint16_t amp) mutable
    {
        rv->compose_bar()->push_amplitude(amp);
        request_repaint();
    };
}
} // namespace tesseract
