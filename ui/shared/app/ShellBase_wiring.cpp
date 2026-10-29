#include "app/ShellBase.h"
#include "app/Launch.h"
#include <tesseract/open_media.h>
#include "views/ForwardRoomPicker.h"
#include "views/MainAppWidget.h"
#include "views/MessageListView.h"
#include "views/MessageSearchView.h"
#include "views/RoomListView.h"
#include "views/RoomPreviewView.h"
#include "views/RoomSettingsView.h"
#include "views/SpaceRootView.h"
#include "tk/tab_bar.h"
#include "views/QuickSwitcher.h"
#include "views/RoomSearchBar.h"
#include "views/RoomView.h"
#include "views/SettingsView.h"
#include <algorithm>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tesseract
{

namespace
{

// Up/Down move the list selection, Escape closes the overlay; every other key
// falls through to the search field. `open` is checked on each key because the
// handler outlives the overlay.
template <typename Overlay, typename Getter, typename Relayout, typename Close>
void push_list_nav_(tk::TextField* field, Getter get, Relayout relayout,
                    Close close)
{
    if (!field)
    {
        return;
    }
    field->push_popup_nav(
        [get, relayout, close](tk::NavKey nk) -> bool
        {
            Overlay* ov = get();
            if (!ov || !ov->is_open())
            {
                return false;
            }
            switch (nk)
            {
            case tk::NavKey::Up:
                ov->move_selection(-1);
                relayout();
                return true;
            case tk::NavKey::Down:
                ov->move_selection(+1);
                relayout();
                return true;
            case tk::NavKey::Escape:
                close();
                return true;
            default:
                return false;
            }
        });
}

} // namespace

// Shell-level Up/Down/Escape navigation and on_close for the window overlays.
// The search fields are self-owned by each overlay; only these callbacks need
// wiring.
void ShellBase::wire_overlay_nav_(views::MainAppWidget* app)
{
    auto relayout = [this] { request_relayout_(); };

    if (auto* qs = app->quick_switcher())
    {
        push_list_nav_<views::QuickSwitcher>(
            qs->search_field(),
            [this] { return main_app_ ? main_app_->quick_switcher() : nullptr; },
            relayout, [this] { close_quick_switch_(); });
        qs->on_close = [this] { close_quick_switch_(); };
    }
    if (auto* ms = app->message_search())
    {
        push_list_nav_<views::MessageSearchView>(
            ms->search_field(),
            [this] { return main_app_ ? main_app_->message_search() : nullptr; },
            relayout, [this] { close_message_search_(); });
        ms->on_close = [this] { close_message_search_(); };
    }
    if (auto* fp = app->forward_picker())
    {
        push_list_nav_<views::ForwardRoomPicker>(
            fp->search_field(),
            [this] { return main_app_ ? main_app_->forward_picker() : nullptr; },
            relayout, [this] { close_forward_picker_(); });
        fp->on_close = [this] { close_forward_picker_(); };
    }
    if (auto* rv = app->room_view())
    {
        if (auto* bar = rv->room_search_bar())
        {
            if (auto* field = bar->search_field())
            {
                // Same keys, but the list is the room view's match cursor.
                field->push_popup_nav(
                    [this, relayout](tk::NavKey nk) -> bool
                    {
                        auto* view = main_app_ ? main_app_->room_view() : nullptr;
                        if (!view || !view->room_search_open())
                        {
                            return false;
                        }
                        switch (nk)
                        {
                        case tk::NavKey::Up:
                        case tk::NavKey::Down:
                            if (view->on_room_search_navigate)
                            {
                                view->on_room_search_navigate(
                                    nk == tk::NavKey::Up ? -1 : +1);
                            }
                            relayout();
                            return true;
                        case tk::NavKey::Escape:
                            close_find_in_room_();
                            return true;
                        default:
                            return false;
                        }
                    });
            }
            bar->on_close = [this] { close_find_in_room_(); };
        }
    }
}

// Callbacks every shell used to wire identically. The ones that need a native
// effect (scroll debounce timers, context menus, status-bar links, cursors,
// the composer) stay in the shells.
void ShellBase::wire_view_callbacks_(views::MainAppWidget* app)
{
    app->on_quick_switch_shortcut = [this] { open_quick_switch_ui_(); };
    app->on_message_search_shortcut = [this] { open_message_search_ui_(); };
    app->on_find_in_room_shortcut = [this] { open_find_in_room_(); };
    app->on_history_back_shortcut = [this] { navigate_history_back(); };
    app->on_history_forward_shortcut = [this] { navigate_history_forward(); };
    app->on_space_back = [this] { space_back_command_(); };

    if (auto* rl = app->room_list_view())
    {
        rl->on_room_selected = [this, rl](const std::string& room_id)
        {
            // A space is not a room: clicking one drills into it rather than
            // opening it as the active room/tab (which would put the space
            // title in the room header).
            for (const auto& r : rooms_)
            {
                if (r.id == room_id && r.is_space)
                {
                    space_nav_frames_.push_back(SpaceNavFrame::capture(rl));
                    space_stack_.push_back(room_id);
                    refresh_room_list_();
                    SpaceNavFrame::enter(rl);
                    return;
                }
            }
            // Modifier+click opens the room in a new tab; a plain click
            // switches the active tab.
            if (click_modifier_held_())
            {
                tab_open_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        rl->on_open_in_tab_requested = [this](const std::string& rid)
        { tab_open_room(rid); };
        rl->on_open_in_window_requested = [this](const std::string& rid)
        { open_room_in_new_window(rid); };
        rl->on_leave_room_requested = [this](const std::string& rid)
        { confirm_leave_room_(rid); };
        rl->on_mark_read_requested = [this](const std::string& rid)
        { mark_room_read_(rid); };
        rl->on_unjoined_room_selected = [this](const tesseract::RoomSummary& s)
        {
            if (!s.avatar_url.empty())
            {
                ensure_media_thumbnail_(s.avatar_url, 64, 64, false);
            }
            if (main_app_)
            {
                main_app_->show_room_preview(s, make_avatar_image_provider_());
                request_relayout_();
            }
        };
    }

    if (auto* tabs = app->tab_bar())
    {
        tabs->on_tab_selected = [this](const std::string& room_id)
        {
            // Modifier+click pops the room out into its own window (and
            // closes the tab); a plain click just switches to it.
            if (click_modifier_held_())
            {
                tab_popout_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        tabs->on_tab_closed = [this](const std::string& room_id)
        { tab_close(room_id); };
    }

    if (auto* rp = app->room_preview())
    {
        rp->on_avatar_needed = [this](const std::string& mxc)
        { ensure_media_thumbnail_(mxc, 64, 64, false); };
        rp->on_join = [this, rp](const std::string& room_id)
        {
            rp->set_state(views::RoomPreviewView::State::Joining);
            join_room_command_(room_id);
        };
        rp->on_dismiss = [this]
        {
            if (main_app_)
            {
                main_app_->hide_room_preview();
            }
        };
    }

    auto copy_to_clipboard = [this](std::string_view text)
    {
        if (main_app_ && main_app_->host())
        {
            main_app_->host()->set_clipboard_text(text);
        }
    };

    if (auto* sr = app->space_root())
    {
        sr->on_layout_changed = [this] { request_relayout_(); };
        sr->on_copy_to_clipboard = [copy_to_clipboard](std::string t)
        { copy_to_clipboard(t); };
        sr->on_leave_space = [this](std::string rid)
        { confirm_leave_room_(rid); };
        sr->on_settings_opened = [this](std::string room_id)
        {
            auto* v = main_app_->space_root()->settings_view();
            if (!v)
            {
                return;
            }
            if (!client_)
            {
                v->set_field_permissions(false, false, false);
                v->set_security_field_permissions(false, false, false, false);
                v->set_permissions_field_permissions(false);
                v->set_image_pack_field_permissions(false);
                v->set_own_power_level({});
                seed_image_pack_tab_(room_id, v);
                return;
            }
            v->set_field_permissions(client_->can_set_room_name(room_id),
                                     client_->can_set_room_topic(room_id),
                                     client_->can_set_room_avatar(room_id));
            v->set_security_field_permissions(
                client_->can_set_room_encryption(room_id),
                client_->can_set_room_join_rules(room_id),
                client_->can_set_room_guest_access(room_id),
                client_->can_set_room_history_visibility(room_id));
            v->set_permissions_field_permissions(
                client_->can_set_room_power_levels(room_id));
            v->set_permissions_state(client_->room_power_levels(room_id));
            v->set_own_power_level(client_->room_own_power_level(room_id));
            fetch_room_security_state_(room_id);
            seed_image_pack_tab_(room_id, v);
        };
        sr->on_settings_avatar_upload_requested = [this](std::string room_id)
        {
            stage_room_settings_avatar_upload_(
                room_id, main_app_->space_root()->settings_view());
        };
        if (auto* sv = sr->settings_view())
        {
            sv->on_accept = [this](std::string room_id,
                                   views::RoomSettingsChanges changes)
            {
                if (!client_)
                {
                    return;
                }
                auto* c = client_;
                run_async_mut_(
                    [this, c, room_id = std::move(room_id),
                     changes = std::move(changes)]() mutable
                    {
                        auto outcome =
                            ShellBase::apply_room_settings_(c, room_id, changes);
                        post_to_ui_alive_(
                            [this, outcome, room_id,
                             media_override = changes.media_override]() mutable
                            {
                                if (!main_app_)
                                {
                                    return;
                                }
                                if (auto* v = main_app_->space_root()->settings_view())
                                {
                                    v->set_commit_result(outcome.ok, outcome.error);
                                }
                                if (outcome.ok && media_override)
                                {
                                    commit_room_media_preview_override_(
                                        room_id, media_override->has_override,
                                        media_override->mode);
                                }
                            });
                    });
            };
            sv->on_image_pack_images_needed = [this](std::string pack_id)
            {
                handle_image_pack_images_needed_(
                    pack_id, main_app_->space_root()->settings_view());
            };
            sv->on_image_pack_pending_image_added =
                [this](std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)
            {
                handle_image_pack_pending_image_added_(
                    local_id, bytes, mime,
                    main_app_->space_root()->settings_view());
            };
        }
    }

    if (auto* rv = app->room_view())
    {
        rv->on_layout_changed = [this] { request_relayout_(); };
        rv->on_set_clipboard = copy_to_clipboard;
        rv->on_selection_started = [this]
        {
            if (main_app_ && main_app_->host())
            {
                main_app_->host()->release_focus_to_canvas();
            }
        };
        if (auto* ml = rv->message_list())
        {
            // The main window's RoomPane never runs finish_init(), which sets
            // the same callback for pop-outs.
            ml->on_tile_needed = [this](int z, int x, int y)
            { ensure_tile_async(z, x, y); };
        }
        if (auto* rsv = rv->room_settings_view())
        {
            rsv->on_image_pack_images_needed = [this](std::string pack_id)
            {
                handle_image_pack_images_needed_(
                    pack_id, main_app_->room_view()->room_settings_view());
            };
            rsv->on_image_pack_pending_image_added =
                [this](std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)
            {
                handle_image_pack_pending_image_added_(
                    local_id, bytes, mime,
                    main_app_->room_view()->room_settings_view());
            };
        }
    }
}

// User-activity, pre-paint prefetch and display-scale hooks on the main
// surface and its host.
void ShellBase::wire_main_surface_(float initial_scale)
{
    // Pointer and wheel events keep the presence tracker "Online".
    main_host_()->set_on_user_activity([this] { notify_user_activity_(); });
    // Warm the decoded-image caches from the local disk cache only (never the
    // SDK/network) for whatever media the visible views are about to paint,
    // immediately before every paint pass; see run_media_prefetch_.
    main_host_()->set_pre_paint_hook(
        [this]
        {
            run_media_prefetch_();
            on_pre_paint_();
        });
    // Thumbnail and avatar requests are sized for the display's current scale;
    // the surface reports live changes.
    main_surface_()->set_on_scale_changed([this](float s) { set_current_scale_(s); });
    set_current_scale_(initial_scale);
}

std::unique_ptr<views::MainAppWidget> ShellBase::create_main_app_()
{
    auto owner = tk::create_root_widget<views::MainAppWidget>(main_host_());
    main_app_  = owner.get();
    room_view_ = main_app_->room_view();
    return owner;
}

void ShellBase::create_main_room_pane_()
{
    // The sole source of truth for room_view_'s image/video viewer callbacks
    // (see main_room_pane_'s doc comment).
    main_room_pane_ = std::make_unique<RoomPane>(
        RoomPane::Deps{
            .shell = this,
            .host = main_host_(),
            .repaint = [this] { request_repaint_(); },
            .relayout = [this] { request_relayout_(); },
            .grab_surface_focus = [this] { grab_main_surface_focus_(); },
            .update_window_title = [this](const std::string&)
            { refresh_window_title_(); },
            .on_left_room = [this](const std::string& room_id)
            {
                // Close the left room's tab; closing the last one also empties
                // the view.
                tab_close(room_id);
                if (current_room_id_ == room_id)
                {
                    current_room_id_.clear();
                    clear_messages_();
                    if (main_app_)
                        main_app_->room_list_view()->set_selected_room("");
                }
                refresh_window_title_();
            },
            .set_window_fullscreen = [this](bool on)
            { set_window_fullscreen_(on); },
        },
        current_room_id_);
    main_room_pane_->attach({
        .room_view = main_app_->room_view(),
        .media_viewer = main_app_->media_viewer(),
        .forward_picker = main_app_->forward_picker(),
        .room_media_view = main_app_->room_media_view(),
        .focus_forward_picker_field = [this] { focus_forward_picker_field_(); },
        .hide_forward_picker_field = [this] { hide_forward_picker_field_(); },
    });
    if (auto* host = main_host_())
        main_app_->media_viewer()->set_video_player(host->make_video_player());
    // One save handler for every viewer kind: RoomPane builds the spec and
    // writes the file; the shell only shows the dialog.
    main_room_pane_->install_media_viewer_save_(
        [this](const views::MediaSaveSpec& spec, std::function<void(std::string)> done)
        { pick_media_save_path_(spec, std::move(done)); });
    if (auto* surface = main_surface_())
    {
        // Lets the animation timer repaint only the rects that hold animated
        // images instead of the whole surface.
        surface->set_anim_cache(&account_manager_.anim_cache());
        surface->set_on_file_drop_error(
            [this](std::string reason) { show_status_message_(std::move(reason)); });
    }
}

// Wire MainAppWidget-level + RoomListView/RoomView/UserInfo providers
// that read from tk_avatars_, tk_images_, anim_cache_, and
// url_preview_data_, plus the window overlays' keyboard navigation
// (wire_overlay_nav_). Each shell calls this once during construction after
// creating its MainAppWidget. Does NOT touch media_viewer
// (RoomPane::wire_room_view_ owns those, via main_room_pane_). Callbacks that
// need a platform effect (native timers, menus, cursors) stay in the shell;
// a shell may also re-assign any callback set here after this returns.
void ShellBase::wire_main_app_widget_(views::MainAppWidget* app)
{
    // Leftovers from a previous session's "open in default app" temp files.
    tesseract::purge_open_media_dir();

    auto avatar_lookup = [this](const std::string& mxc) -> const tk::Image*
    { return avatar_image_(mxc); };

    app->set_avatar_provider(avatar_lookup);
    // Image packs are ordinary room state, so a space's settings (the wrench
    // on SpaceRootView) carries them too; only its Media tab is skipped.
    app->room_view()->room_settings_view()->set_image_pack_provider(
        make_static_image_provider_with_fetch_(96, 96));
    app->space_root()->settings_view()->set_image_pack_provider(
        make_static_image_provider_with_fetch_(96, 96));
    setup_dm_callbacks();
    if (auto* list = app->room_list_view())
    {
        list->set_room_open_in_tab_provider(
            [this](const std::string& rid) { return room_open_in_tab(rid); });
        list->set_room_open_in_window_provider(
            [this](const std::string& rid) { return room_open_in_window(rid); });
        // Background backfill follows the visible rows once scrolling settles.
        list->on_scroll = [this]
        {
            debounce_(DebounceSlot::RoomListBackfill, 300,
                      [this]
                      {
                          if (!main_app_ || !active_account_)
                              return;
                          auto ids  = main_app_->room_list_view()->visible_room_ids();
                          auto sess = active_account_;
                          run_async_mut_([sess, ids = std::move(ids)]() mutable
                          {
                              if (sess && sess->client)
                              {
                                  sess->client->stop_visible_backfill();
                                  sess->client->start_background_backfill(ids);
                              }
                          });
                          start_bridge_check_();
                      });
        };
    }
    if (auto* reminder = app->encryption_reminder())
    {
        reminder->on_open    = [this] { reopen_encryption_setup_(); };
        reminder->on_dismiss = [this] { snooze_encryption_reminder_(); };
    }
    app->on_space_header = [this]
    {
        show_space_root_(effective_space_id_());
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
            // The unjoined section only exists while the sidebar is drilled
            // into a space.
            if (!space_stack_.empty())
                request_summary_fetch_(space_stack_.back(), room_id);
        };

    app->room_list_view()->on_pin_space_requested =
        [this](const std::string& id) { pin_space_(id); };
    if (auto* picker = app->space_picker())
    {
        picker->on_select = [this](const std::string& id)
        { select_picker_space_(id); };
        picker->on_home  = [this] { select_picker_home_(); };
        picker->on_unpin = [this](const std::string& id) { unpin_space_(id); };
        picker->on_reorder = [this](const std::string& id, const std::string& before)
        { reorder_pinned_space_(id, before); };
        picker->on_avatar_needed = [this](const std::string& id)
        {
            if (const auto* r = room_by_id_(id))
                ensure_room_avatar_(*r);
        };
    }

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
        init[views::RoomListView::kSecCallRooms]     = s.room_section_call_rooms_collapsed;
        init[views::RoomListView::kSecKnocks]        = s.room_section_knocks_collapsed;
        for (int sec = 0; sec < views::RoomListView::kNumSections; ++sec)
            app->room_list_view()->set_section_collapsed(sec, init[sec]);
    }
    app->room_list_view()->on_section_toggled =
        [this](int section, bool collapsed)
    {
        // Inside a space the sections show that space's rooms, and their state
        // is not persisted: only the root list's state is, so a toggle made in
        // a space must not overwrite it.
        if (!space_stack_.empty())
            return;
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
        case views::RoomListView::kSecCallRooms:
            s.room_section_call_rooms_collapsed     = collapsed; break;
        case views::RoomListView::kSecKnocks:
            s.room_section_knocks_collapsed         = collapsed; break;
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
        app->set_room_list_compact(s.room_list_density ==
                                   tesseract::Settings::RoomListDensity::Compact);
    }
    // The picker column drops out (and pinned spaces return to the room list)
    // when collapsed or single-pane. Fired mid-layout, so rebuild afterwards.
    app->on_space_pins_shown_changed = [this](bool)
    { post_to_ui_([this] { refresh_room_list_(); }); };
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
    app->room_view()->on_bridge_avatar_needed = [this](const std::string& mxc)
    { ensure_media_thumbnail_(mxc, 64, 64, false); };
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
    app->room_view()->set_media_failed_provider(
        [this](const std::string& key) { return media_dep_known_failed_(key); });
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
                // With "hide content" on, the title and sender stay out of the
                // MPRIS metadata, which any local D-Bus client can read.
                const bool hide_content =
                    tesseract::Settings::instance().notification_hide_content;
                if (hide_content)
                {
                    np.title = np.kind == NowPlaying::Kind::Voice
                                   ? tk::tr("Voice message")
                                   : tk::tr("Audio");
                    np.artist.clear();
                }
                else
                {
                    np.title = row && !row->body.empty() ? row->body
                                                         : tk::tr("Voice message");
                    np.artist = row ? row->sender_name : std::string{};
                }
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
    // refresh_user_strip_() call.
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
        if (url.empty() || !app->media_viewer())
            return;
        ensure_viewer_fullres_(url);
        app->media_viewer()->open(views::item_from_avatar(url, name));
        app->show_media_viewer(true);
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
        if (ar->directory_view() && client_)
            refresh_directory_server_choices_();
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
            dr->on_server_validated = [this](const std::string& server)
            { on_directory_server_validated_(server); };
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

    wire_overlay_nav_(app);
    wire_view_callbacks_(app);

    // The shell is fully constructed and the widget tree is up: begin the ~2 s
    // image-GC tick (see run_image_gc_ / media_sweep_timer_).
    media_sweep_timer_.start();
}

// Wire every SettingsView callback that needs no Surface/Host/native dialog:
// preference changes (one on_setting_changed → handle_setting_changed_) and the
// command buttons that forward into an existing ShellBase handler. Each shell
// calls this once, right after constructing its SettingsView, then wires only
// what's left: on_close/on_logout/on_reset_identity (differ in how the
// settings surface is dismissed), on_tab_changed (needs the shell's Surface),
// and audio/camera/mic device enumeration (needs tk::Host).
void ShellBase::wire_settings_view_(views::SettingsView* view)
{
    if (!view)
        return;

    view->emoji_skin_tone_provider = [this] { return emoji_skin_tone_(); };
    view->on_emoji_skin_tone_changed = [this](tesseract::emoji::SkinTone tone)
    {
        set_emoji_skin_tone_(tone);
    };
    view->on_setting_changed = [this](views::SettingKey key)
    {
        handle_setting_changed_(key);
    };
    view->set_proxy_restart_pending(tesseract::proxy_changed_since_launch(
        tesseract::Settings::instance().proxy_mode,
        tesseract::Settings::instance().proxy_url));
    view->set_language_restart_pending(
        tesseract::Settings::instance().language != tesseract::launch_language());
    view->on_open_activity_monitor = [this] { open_activity_monitor_(); };
    view->on_restart_requested = [this] { restart_app_(); };
    view->on_about_tab_shown = [this] { refresh_cache_sizes_poll_(); };
    view->on_clear_caches = [this]
    {
        // The clear finishes long after the click, and Settings may have been
        // closed meanwhile, so the view is looked up when the sizes arrive
        // instead of being captured.
        clear_all_caches_([this](uint64_t local, uint64_t sdk, uint64_t memory,
                                 uint64_t mh, uint64_t mm, uint64_t dh,
                                 uint64_t dm)
        { show_cache_sizes_(local, sdk, memory, mh, mm, dh, dm); });
    };
}

void ShellBase::wire_settings_controller_common_(
    views::SettingsView* view, tesseract::SettingsController* ctrl)
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

    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        my_avatar_url_ = mxc;
        if (active_account_)
            active_account_->avatar_url = my_avatar_url_;
        // The upload can finish after Settings was closed.
        if (stats_settings_view_)
        {
            stats_settings_view_->set_avatar_url(mxc);
            stats_settings_view_->request_relayout();
        }
        refresh_user_strip_();
    };
}

// The main window's audio, video and repaint services on its RoomView.
void ShellBase::wire_room_view_services_()
{
    auto* rv = main_app_->room_view();
    auto* host = main_host_();
    if (auto player = host->make_audio_player())
    {
        rv->set_audio_player(std::move(player));
    }
    capture_ = host->make_audio_capture();
    wire_voice_capture_(
        rv, [this]
        {
            request_repaint_();
        },
        [this]
        {
            return current_room_id_;
        },
        [this]
        {
            if (auto* area = room_view_->compose_bar()->text_area())
            {
                area->set_text("");
            }
            room_view_->set_current_text({});
        });
    rv->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t>
        {
            // Non-blocking: warmed bytes or empty + async fetch (repaint on
            // arrival) so playback never freezes the UI thread.
            return voice_bytes_or_fetch_(source_json, [this]
            {
                request_relayout_();
            });
        });
    rv->set_video_player_factory([host]
    {
        return host->make_video_player();
    });
    rv->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready)
        {
            if (!client_)
            {
                return;
            }
            auto req_id = begin_media_req_(
                0, [on_ready = std::move(on_ready)](std::vector<std::uint8_t> bytes) mutable
                {
                    on_ready(std::move(bytes));
                });
            client_->fetch_source_bytes_async(req_id, src);
        });
    rv->set_repaint_requester([this]
    {
        request_repaint_();
    });
    rv->set_post_delayed(
        [host](int ms, std::function<void()> fn)
        {
            host->post_delayed(ms, std::move(fn));
        });
}

// Wire voice-capture callbacks onto rv. Called by wire_room_view_services_
// once capture_ is created (not from RoomWindowBase::wire_room_view_() — pop-out
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
                auto alive = weak_flag();
                run_async_mut_(
                    [this, alive, sess, request_id, rid,
                     pcm = std::move(pcm), waveform = std::move(waveform),
                     duration_ms, caption, reply_id]() mutable
                    {
                        if (!sess || !sess->client) return;
                        const std::uint64_t est = duration_ms * 3;
                        const std::uint64_t limit =
                            sess->client->media_upload_limit();
                        if (limit > 0 && est > limit)
                        {
                            // Refused on the worker: say so, rather than dropping the
                            // clip without a word.
                            if (!alive.expired())
                                post_to_ui_alive_([this]
                                {
                                    show_status_message_(tk::tr(
                                        "This voice message is too long to upload."));
                                });
                            return;
                        }
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
