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

void ShellBase::debounce_(DebounceSlot slot, int ms, std::function<void()> fn)
{
    const int key = static_cast<int>(slot);
    const std::uint64_t gen = ++debounce_gen_[key];
    post_to_ui_after_(ms,
                      [this, key, gen, fn = std::move(fn)]()
                      {
                          // Honour only the most recent schedule on this slot;
                          // earlier pending fires were superseded.
                          auto it = debounce_gen_.find(key);
                          if (it != debounce_gen_.end() && it->second == gen)
                          {
                              fn();
                          }
                      });
}

void ShellBase::cancel_debounce_(DebounceSlot slot)
{
    ++debounce_gen_[static_cast<int>(slot)];
}

void ShellBase::populate_pending_restore_popouts_()
{
    if (!pending_restore_popouts_.empty())
        return;
    for (const auto& e : Settings::instance().popout_windows)
    {
        if (e.room_id.empty())
            continue;
        // Skip entries that explicitly belong to a different account.
        // An empty user_id means the entry predates the multi-account field
        // (pre-migration data) and should be restored for the active account.
        if (!e.user_id.empty() && active_account_ &&
            e.user_id != active_account_->user_id)
            continue;
        pending_restore_popouts_.push_back(e.room_id);
    }
}

void ShellBase::save_settings_debounced_()
{
    if (tearing_down_)
        return;
    debounce_(DebounceSlot::SaveSettings, 500,
              []() {
                  tesseract::Settings::instance().save_to_disk(
                      tesseract::config_dir());
              });
}

Settings::WindowGeometry ShellBase::clamp_to_screens_(
    const Settings::WindowGeometry& saved,
    int default_w,
    int default_h,
    const std::vector<tk::Rect>& screens)
{
    return shell_helpers::clamp_to_screens(saved, default_w, default_h, screens);
}

std::vector<tesseract::StatusSegment>
ShellBase::parse_status_message_(const std::string& msg) const
{
    if (status_message_allows_links_)
        return tesseract::parse_status_links(msg);
    return {{msg, std::string{}}}; // plain: never linkify un-opted-in text
}

// Show `msg` in the platform status bar for `auto_clear_ms` milliseconds,
// then restore the sync-status text. `auto_clear_ms <= 0` → the message
// persists until the next status change (e.g. an update notification).
// `allow_links` opts into markdown-style "[label](url)" hyperlink parsing
// (see app/status_links.h) — pass it ONLY for app-authored text. It defaults
// to false so server/error-sourced messages (subscribe / sync / sign-out
// failures whose tail is a homeserver string) can never inject a clickable
// link. Safe to call from any thread.
void ShellBase::show_status_message_(std::string msg, int auto_clear_ms,
                                     bool allow_links)
{
    const std::uint32_t gen = ++status_msg_gen_;
    const bool persistent  = (auto_clear_ms <= 0);
    post_to_ui_alive_(
        [this, msg = std::move(msg), allow_links, gen, persistent]
        {
            // Drop if a newer message or a restore has already superseded this.
            if (status_msg_gen_ != gen)
                return;
            if (persistent)
                status_override_active_ = true;
            status_message_allows_links_ = allow_links;
            on_show_status_message_ui_(msg);
            // The status line is the shell's channel for errors and
            // notices (call-ended reasons, sync trouble, …) — speak it too,
            // but not the same text again within a minute (the sync-error
            // path re-posts its message on every 5 s retry).
            const auto now = std::chrono::steady_clock::now();
            if (main_app_ && main_app_->host() &&
                (msg != last_announced_status_ ||
                 now - last_announced_status_at_ > std::chrono::minutes(1)))
            {
                last_announced_status_    = msg;
                last_announced_status_at_ = now;
                main_app_->host()->announce(msg);
            }
        });
    // auto_clear_ms <= 0 → persistent: the message stays until a newer
    // status message (which bumps the generation) replaces it.
    if (auto_clear_ms <= 0)
        return;
    post_to_ui_after_(auto_clear_ms,
                      [this, gen]
                      {
                          if (status_msg_gen_ == gen)
                          {
                              status_override_active_ = false;
                              on_restore_status_ui_();
                          }
                      });
}

// ── WorkerPool ─────────────────────────────────────────────────────────────

int ShellBase::pool_thread_count()
{
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0)
    {
        hc = 4; // some platforms/sandboxes can't report this
    }
    return std::clamp<int>(static_cast<int>(hc), 2, 8);
}

void ShellBase::run_async_(std::function<void()> fn)
{
    pool_.post(std::move(fn));
}

void ShellBase::run_async_(const char* label, std::function<void()> fn)
{
    pool_.post(
        [this, label = std::string(label), fn = std::move(fn)]() mutable
        {
            auto scope = activity_.begin(label, "Workers", "one-shot");
            fn();
        });
}

void ShellBase::run_async_mut_(const char* label, std::function<void()> fn)
{
    mut_pool_.post(
        [this, label = std::string(label), fn = std::move(fn)]() mutable
        {
            auto scope = activity_.begin(label, "Workers", "one-shot");
            fn();
        });
}

std::vector<tesseract::ActivityEntry> ShellBase::activity_snapshot_(tesseract::Client* client) const
{
    std::vector<tesseract::ActivityEntry> out = activity_.snapshot();
    if (client)
    {
        auto rust = client->activity_snapshot();
        out.insert(out.end(), std::make_move_iterator(rust.begin()),
                   std::make_move_iterator(rust.end()));
    }
    const struct
    {
        const char*       name;
        const WorkerPool& pool;
    } pools[] = {{"pool (shared reads)", pool_},
                 {"mut_pool (serialised writes)", mut_pool_},
                 {"media_prefetch_pool", media_prefetch_pool_}};
    for (const auto& p : pools)
    {
        const size_t queued = p.pool.pending_count();
        const size_t total  = p.pool.in_flight_.load(std::memory_order_relaxed);
        tesseract::ActivityEntry e;
        e.name   = p.name;
        e.group  = "Workers";
        e.kind   = "pool";
        e.state  = total > 0 ? tesseract::ActivityState::Running : tesseract::ActivityState::Idle;
        e.detail = std::to_string(total - std::min(total, queued)) + " running, " +
                   std::to_string(queued) + " queued";
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const tesseract::ActivityEntry& a, const tesseract::ActivityEntry& b)
                     { return a.group != b.group ? a.group < b.group : a.name < b.name; });
    return out;
}

void ShellBase::run_async_mut_(std::function<void()> fn)
{
    mut_pool_.post(std::move(fn));
}

void ShellBase::init_pool_callbacks_()
{
    auto notify = [this] { post_to_ui_alive_([this] { on_inflight_ui_(); }); };
    {
        std::lock_guard<std::mutex> lk(pool_.mu_);
        pool_.on_change_ = notify;
    }
    {
        std::lock_guard<std::mutex> lk(mut_pool_.mu_);
        mut_pool_.on_change_ = notify;
    }
}

// ── Secondary window registry ─────────────────────────────────────────────────

ShellBase::ShellBase(AccountManager& account_manager)
    : account_manager_(account_manager)
{
}

ShellBase::~ShellBase()
{
    // Signal any UI-thread continuations queued via post_to_ui_alive_ that this
    // shell is gone; they will no-op rather than dereference freed members.
    invalidate_weak_self();

    // Stop the media-sweep timer before the derived shell is torn down: a
    // pending tick must not re-arm (post_to_ui_after_ is pure-virtual once the
    // derived dtor has run). Its own dtor also stops it, but that is too late.
    media_sweep_timer_.stop();

    if (search_backend_handle_)
        account_manager_.search_backend().unregister_shell(*search_backend_handle_);

    // Join the screen-picker thumbnail worker (if any) before this object's
    // members start tearing down — it captures `this` to call
    // post_to_ui_alive_(), which would be a use-after-free if it ran after
    // ~ShellBase() returned. Bounded: it checks liveness between sources and
    // each capture_thumbnail() call is itself short (one PrintWindow/DXGI
    // acquire), so this is a brief wait, not an indefinite block.
    if (screen_thumb_worker_.joinable())
        screen_thumb_worker_.join();

    // Cancel an in-flight known-user roster build so its worker loop bails
    // between rooms instead of finishing a full member sweep — otherwise the
    // thread-pool join below would block until the sweep completes.
    if (roster_build_cancel_)
        roster_build_cancel_->store(true);

    // Prevent save_settings_debounced_() from calling post_to_ui_after_()
    // (pure virtual at this point in teardown) when ~RoomWindowBase runs below.
    tearing_down_ = true;

    // Tear down pop-out windows while the registries they unregister from are
    // still alive. Members are destroyed in reverse declaration order, so the
    // owned-window vector (declared before secondary_windows_ /
    // room_subscription_refs_) would otherwise be destroyed *last* — each
    // ~RoomWindowBase would then call unregister_room_window_() /
    // release_room_subscription_() on already-freed maps, a use-after-free that
    // crashes on shutdown whenever a pop-out is open. Clearing here runs every
    // ~RoomWindowBase while those maps are intact.
    teardown_activity_monitor_();
    owned_secondary_windows_.clear();
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
bool ShellBase::seed_screenshot_fixture_(tk::CanvasFactory& factory)
{
    if (!screenshot::install_assets(factory, account_manager_.thumbnail_cache()))
        return false;

    screenshot_fixture_ = screenshot::make_fixture();
    const auto& f    = screenshot_fixture_;
    my_user_id_      = f.user_id;
    my_display_name_ = f.display_name;
    my_avatar_url_   = f.avatar_url;
    rooms_           = f.rooms;
    current_room_id_ = f.selected_room_id;

    main_app_->show_room();
    main_app_->room_list_view()->set_rooms(rooms_);
    main_app_->room_list_view()->set_selected_room(current_room_id_);
    for (const auto& room : rooms_)
    {
        if (room.id == current_room_id_)
        {
            room_view_->set_room(room);
            break;
        }
    }
    room_view_->set_messages(f.messages);
    room_view_->set_typing_text(shell_helpers::format_typing_text(f.typing_names));
    return true;
}

std::vector<screenshot::Scene> ShellBase::make_screenshot_scenes_()
{
    using State = views::RoomView::ThreadPanelState;
    const auto& f = screenshot_fixture_;

    std::vector<screenshot::Scene> scenes;
    scenes.push_back({"main", {}, {}});
    scenes.push_back(
        {"thread",
         [this, &f]
         {
             room_view_->set_thread_panel(State::Open, f.thread_root_id);
             room_view_->thread_view()->set_messages(f.thread_messages,
                                                     /*room_switch=*/true);
         },
         [this] { room_view_->set_thread_panel(State::Closed, {}); }});
    scenes.push_back(
        {"room-info",
         [this, &f]
         {
             room_view_->show_room_info();
             auto* panel = room_view_->room_info_panel();
             panel->set_presence_provider(
                 [&f](const std::string& user_id)
                 {
                     for (const auto& [id, state] : f.presence)
                         if (id == user_id)
                             return state;
                     return tesseract::PresenceState::Offline;
                 });
             // After show_room_info(): RoomInfoPanel::open() clears members.
             panel->set_members(f.members);
         },
         [this] { room_view_->room_info_panel()->close(); }});
    scenes.push_back({"emoji",
                      [this] { room_view_->show_emoji_picker(); },
                      [this] { room_view_->close_pickers(); }});
    scenes.push_back({"settings",
                      [this]
                      {
                          open_app_settings_ui_();
                          if (stats_settings_view_)
                              stats_settings_view_->show_appearance_section();
                      },
                      [this] { close_app_settings_ui_(); }});
    return scenes;
}

void ShellBase::start_screenshot_director_(screenshot::ScreenshotHost& host,
                                           std::string prefix, int settle_ms)
{
    screenshot_director_ = std::make_unique<screenshot::ScreenshotDirector>(
        host, make_screenshot_scenes_(), std::move(prefix), settle_ms);
    screenshot_director_->start();
}
#endif

} // namespace tesseract
