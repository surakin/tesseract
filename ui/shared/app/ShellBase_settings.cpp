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

// placement — see RoomSettingsView::set_image_pack_*. This view has no
// Client dependency, so ShellBase fetches and pushes data in, mirroring
// seed_room_media_section_'s shape. list_image_packs()/list_pack_images()
// are cached local reads (no network round-trip), so unlike
// fetch_room_security_state_ these are synchronous — no request_id
// bookkeeping needed. Called from each shell's on_room_settings_opened
// handler, right after fetch_room_security_state_. `target` is whichever
// RoomSettingsView instance is asking — room_view_->room_settings_view()
// for a normal room, or main_app_->space_root()->settings_view() for a
// space root; image packs are ordinary room state, so a space's own
// packs are seeded the same way.
void ShellBase::seed_image_pack_tab_(const std::string& room_id,
                                     views::RoomSettingsView* target,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (!target || !acting_client)
        return;
    if (!target->is_open() || target->room_id() != room_id)
        return;
    target->set_image_pack_field_permissions(
        acting_client->can_set_room_image_packs(room_id));
    auto packs = acting_client->list_image_packs();
    std::vector<tesseract::ImagePack> room_packs;
    for (auto& p : packs)
    {
        if (p.source_kind == tesseract::PackSourceKind::Room &&
            p.source_room == room_id)
            room_packs.push_back(std::move(p));
    }
    target->set_image_pack_available_packs(std::move(room_packs));
}

void ShellBase::handle_image_pack_images_needed_(const std::string& pack_id,
                                                 views::RoomSettingsView* target)
{
    if (!target || !client_)
        return;
    if (!target->is_open())
        return;
    auto images =
        client_->list_pack_images(pack_id, tesseract::PackUsageFilter::Any);
    target->set_image_pack_images(pack_id, std::move(images));
}

void ShellBase::handle_image_pack_pending_image_added_(
    std::uint64_t local_id, std::vector<uint8_t> bytes, std::string /*mime*/,
    views::RoomSettingsView* target)
{
    run_async_(
        [this, local_id, bytes = std::move(bytes), target]()
        {
            DecodedImage decoded = decode_image_(bytes, 256, 256);
            std::shared_ptr<tk::Image> preview;
            if (decoded.still)
                preview = std::shared_ptr<tk::Image>(std::move(decoded.still));
            else if (!decoded.frames.empty())
                preview =
                    std::shared_ptr<tk::Image>(std::move(decoded.frames.front()));
            post_to_ui_alive_(
                [this, local_id, target, preview = std::move(preview)]() mutable
                {
                    if (target)
                        target->set_image_pack_tile_preview(local_id, std::move(preview));
                });
        });
}

void ShellBase::handle_user_pack_pending_image_added_(
    std::uint64_t local_id, std::vector<uint8_t> bytes, std::string /*mime*/,
    views::UserPackEditor* target)
{
    run_async_(
        [this, local_id, bytes = std::move(bytes), target]()
        {
            DecodedImage decoded = decode_image_(bytes, 256, 256);
            std::shared_ptr<tk::Image> preview;
            if (decoded.still)
                preview = std::shared_ptr<tk::Image>(std::move(decoded.still));
            else if (!decoded.frames.empty())
                preview =
                    std::shared_ptr<tk::Image>(std::move(decoded.frames.front()));
            post_to_ui_alive_(
                [this, local_id, target, preview = std::move(preview)]() mutable
                {
                    if (target)
                        target->set_tile_preview(local_id, std::move(preview));
                });
        });
}

void ShellBase::apply_bridge_overrides_(std::vector<RoomInfo>&          rooms,
                                        const std::vector<std::string>& overrides) const
{
    // An empty list must still clear flags left from a removed override.
    for (auto& r : rooms)
    {
        r.bridge_overridden =
            std::find(overrides.begin(), overrides.end(), r.id) != overrides.end();
    }
}

void ShellBase::set_bridge_override_(const std::string& room_id, bool not_bridged)
{
    if (!active_account_ || room_id.empty())
        return;

    auto& overrides = active_account_->bridge_not_bridged_overrides;
    auto  it        = std::find(overrides.begin(), overrides.end(), room_id);
    if (not_bridged && it == overrides.end())
    {
        overrides.push_back(room_id);
    }
    else if (!not_bridged && it != overrides.end())
    {
        overrides.erase(it);
    }
    else
    {
        return; // no change
    }

    // Local-only preference — applies immediately rather than being staged
    // with the rest of RoomSettingsView's fields (see Accept/Cancel there).
    persist_room_layout_pref_();

    // Refresh the in-memory cache + dependent UI without waiting for the next
    // sync tick, so the badge/call-button/threads-button update right away.
    apply_bridge_overrides_(rooms_, overrides);
    if (auto it2 = per_account_rooms_.find(my_user_id_); it2 != per_account_rooms_.end())
        apply_bridge_overrides_(it2->second, overrides);

    refresh_bridge_dependent_ui_(room_id);
}

tesseract::emoji::SkinTone ShellBase::emoji_skin_tone_()
{
    settle_emoji_skin_tone_();
    return active_account_ ? active_account_->emoji_skin_tone
                           : tesseract::emoji::SkinTone::None;
}

void ShellBase::settle_emoji_skin_tone_()
{
    if (active_account_)
        tesseract::settle_emoji_skin_tone(*active_account_,
                                          std::chrono::steady_clock::now());
}

void ShellBase::set_emoji_skin_tone_(tesseract::emoji::SkinTone tone)
{
    if (!active_account_ || active_account_->emoji_skin_tone == tone)
        return;
    tesseract::set_local_emoji_skin_tone(*active_account_, tone,
                                         std::chrono::steady_clock::now());
    persist_room_layout_pref_();
}

void ShellBase::refresh_bridge_dependent_ui_(const std::string& room_id)
{
    const auto* r = room_by_id_(room_id);
    if (!r)
        return;

    if (room_id == current_room_id_ && room_view_)
    {
        if (auto* header = room_view_->header())
            update_call_btn_visibility_(header, room_id);
        if (client_)
            apply_threads_list_(client_->list_room_threads(room_id));
        room_view_->set_room(*r); // refreshes the info panel's badge if open
    }
    for (auto& [rid, w] : active_account_popouts_())
    {
        if (rid != room_id || !w->room_view())
            continue;
        if (auto* h = w->room_view()->header())
            update_call_btn_visibility_(h, rid);
        w->room_view()->set_room(*r);
    }
    refresh_call_banners_(); // calls are suppressed for bridged rooms
}

void ShellBase::open_activity_monitor_()
{
    if (activity_window_)
    {
        activity_window_->bring_to_front();
        return;
    }
    auto view     = std::make_unique<tesseract::views::ActivityMonitorView>();
    activity_view_ = view.get();
    auto* win = create_aux_window_(tk::tr("Activity Monitor"), std::move(view), 640, 560);
    if (!win)
    {
        activity_view_ = nullptr;
        show_status_message_(tk::tr("The Activity Monitor is not available on this platform yet."));
        return;
    }
    activity_window_.reset(win);
    activity_window_->on_window_closed = [this] { on_activity_window_closed_(); };
    refresh_activity_monitor_();
    activity_window_->bring_to_front();
}

void ShellBase::on_activity_window_closed_()
{
    cancel_debounce_(DebounceSlot::ActivityMonitor);
    activity_view_ = nullptr;
    // Runs from the native close handler: hand ownership to the platform's
    // deferred delete instead of destroying the window mid-event.
    if (activity_window_)
        activity_window_.release()->schedule_delete();
}

void ShellBase::teardown_activity_monitor_()
{
    cancel_debounce_(DebounceSlot::ActivityMonitor);
    activity_view_ = nullptr;
    if (activity_window_)
    {
        activity_window_->on_window_closed = nullptr;
        activity_window_->close_window();
        activity_window_.reset();
    }
}

void ShellBase::refresh_activity_monitor_()
{
    if (!activity_view_)
        return;
    auto sess = active_account_;
    run_async_(
        [this, sess]
        {
            auto entries = activity_snapshot_(sess && sess->client ? sess->client.get() : nullptr);
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            post_to_ui_alive_(
                [this, entries = std::move(entries), now_ms]() mutable
                {
                    if (!activity_view_)
                        return;
                    activity_view_->set_snapshot(std::move(entries), now_ms);
                    if (activity_window_)
                        activity_window_->request_repaint();
                    debounce_(DebounceSlot::ActivityMonitor, 1000,
                              [this] { refresh_activity_monitor_(); });
                });
        });
}

void ShellBase::trigger_update_check_()
{
    // Disabled at runtime for installs updated by something else (MSIX).
    if (!tesseract::update_checks_enabled())
        return;
    // kVersion is generated from PROJECT_VERSION in CMakeLists.txt via version.h.in.
#if defined(TESSERACT_AUR_PACKAGE)
    // TESSERACT_AUR_PACKAGE is set at configure time with -DTESSERACT_AUR_PACKAGE=pkgname.
    if (std::exchange(update_check_triggered_, true))
        return;
    update_checker_ = std::make_unique<AurUpdateChecker>(
        *client_,
        [this](std::function<void()> fn) { run_async_("update-check", std::move(fn)); },
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); },
        TESSERACT_AUR_PACKAGE,
        kVersion);
#elif defined(TESSERACT_GITHUB_REPO)
    // TESSERACT_GITHUB_REPO is set at configure time with -DTESSERACT_GITHUB_REPO=owner/repo.
    if (std::exchange(update_check_triggered_, true))
        return;
    update_checker_ = std::make_unique<GithubUpdateChecker>(
        *client_,
        [this](std::function<void()> fn) { run_async_("update-check", std::move(fn)); },
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); },
        TESSERACT_GITHUB_REPO,
        kVersion);
#else
    return; // no update-check backend configured at build time
#endif
    update_checker_->check_async([this](std::string version, std::string url) {
        show_status_message_(
            "[" + tk::trf(tk::tr("Tesseract {0} available"), {version}) + "](" +
                url + ")",
            0,
            true);
    });
}

void ShellBase::handle_image_packs_updated_ui_()
{
    refresh_pickers_packs_();
    cached_emoticons_.clear();
    emoticon_packs_.clear();
    if (client_)
    {
        for (auto& pack : client_->list_image_packs())
        {
            auto imgs = client_->list_pack_images(
                pack.id, tesseract::PackUsageFilter::Emoticon);
            for (const auto& img : imgs)
            {
                cached_emoticons_.push_back(img);
            }
            emoticon_packs_.emplace_back(pack, std::move(imgs));
        }
    }
    // Keep the global Settings "Emojis & Stickers" tab's known-packs list
    // and personal-pack snapshot current too — cheap (local cache reads
    // only), same justification as the cached_emoticons_ rebuild above.
    if (settings_controller_)
        settings_controller_->load_image_packs();
    // Re-seed any currently-open Room Settings "Emojis & Stickers" tab too.
    // seed_image_pack_tab_ no-ops on its own if the target isn't open or
    // doesn't match room_id, so this is safe to call unconditionally —
    // covers the race where the user opens Room Settings for a room whose
    // pack fetch (kicked off by set_active_room) hasn't resolved yet.
    if (room_view_)
    {
        if (auto* v = room_view_->room_settings_view())
            seed_image_pack_tab_(v->room_id(), v);
    }
    for (const auto& [rid, w] : active_account_popouts_())
    {
        if (w->room_view())
        {
            if (auto* v = w->room_view()->room_settings_view())
                seed_image_pack_tab_(v->room_id(), v);
        }
    }
}

// Fired via IEventHandler::on_bot_commands_updated when the cached set
// of MSC4391 bot commands for `room_id` changes. Concrete: no-op unless
// `room_id` is the active room, in which case it calls
// `on_active_room_bot_commands_changed_ui_()` — each shell overrides
// that no-op to refresh its own SlashCommandController's popup, since
// (unlike ComposeBar/RoomView) that controller is shell-owned, not
// shared — see SlashCommandController.h's doc comment.
void ShellBase::handle_bot_commands_updated_ui_(std::string room_id)
{
    if (!main_window_shows_(room_id))
        return;
    on_active_room_bot_commands_changed_ui_();
}

std::string ShellBase::shortcode_for_mxc_(const std::string& mxc) const
{
    if (mxc.empty())
    {
        return {};
    }
    for (const auto& e : cached_emoticons_)
    {
        if (e.url == mxc)
        {
            return e.shortcode;
        }
    }
    return {};
}

std::vector<tesseract::ImagePackImage>
ShellBase::emoticons_for_room_(const std::string& room_id) const
{
    const std::vector<std::string> parent_space_ids = parent_spaces_for_room_(room_id);
    std::vector<tesseract::ImagePackImage> out;
    for (const auto& [pack, imgs] : emoticon_packs_)
    {
        if (!views::is_pack_picker_visible(pack, room_id, parent_space_ids))
        {
            continue;
        }
        out.insert(out.end(), imgs.begin(), imgs.end());
    }
    return out;
}

void ShellBase::force_full_repaint_all_surfaces_()
{
    // Unclipped full repaint so ListView::paint skips no rows and every
    // on-screen image is peek()'d — the mark pass for the image GC.
    request_repaint_();
    for (auto& w : owned_secondary_windows_)
    {
        if (w)
        {
            w->repaint_anim_frame(); // full surface repaint (+ picker overrides)
        }
    }
}

void ShellBase::handle_send_presence_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.send_presence = enabled;
    s.save_to_disk(tesseract::config_dir());

    // Fold the new setting together with window-active / low-power state.
    resolve_presence_polling_();

    if (enabled)
    {
        start_presence_tracking_();
    }
    else
    {
        notify_presence_logout_();
        presence_tracker_.reset();
    }
}

void ShellBase::handle_index_messages_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.index_messages_for_search = enabled;
    s.save_to_disk(tesseract::config_dir());

    // Apply to every logged-in account's client: the index is per-account, but
    // the preference is global. Enabling lazily backfills history; disabling
    // clears each account's on-disk index. The call is non-blocking (sets a
    // flag + spawns/cleans up on the SDK runtime), so it is safe on the UI
    // thread.
    for (const auto& sess : account_manager_.accounts())
    {
        if (sess && sess->client)
            sess->client->set_search_indexing_enabled(enabled);
    }
}

void ShellBase::handle_show_membership_events_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.show_room_join_leave_events = enabled;
    s.save_to_disk(tesseract::config_dir());

    if (client_)
    {
        client_->set_show_membership_events(enabled);
        if (!current_room_id_.empty())
            client_->subscribe_room(current_room_id_);
    }
}

void ShellBase::handle_msc2545_legacy_compat_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.msc2545_legacy_compat = enabled;
    s.save_to_disk(tesseract::config_dir());

    // set_msc2545_legacy_compat synchronously rebuilds the image-pack cache
    // and fires on_image_packs_updated Rust-side, which the shells already
    // handle to refresh list_image_packs()-driven UI — no extra refresh
    // call needed here.
    if (client_)
        client_->set_msc2545_legacy_compat(enabled);
}

void ShellBase::handle_developer_mode_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.developer_mode = enabled;
    s.save_to_disk(tesseract::config_dir());
}

void ShellBase::refresh_all_message_lists_display_prefs_()
{
    auto refresh = [](views::MessageListView* ml)
    {
        if (ml)
            ml->on_display_prefs_changed();
    };
    if (main_app_ && main_app_->room_view())
    {
        refresh(main_app_->room_view()->message_list());
        if (auto* tv = main_app_->room_view()->thread_view())
            refresh(tv->message_list());
    }
    for (auto& [rid, w] : secondary_windows_)
    {
        if (auto* rv = w->room_view())
        {
            refresh(rv->message_list());
            if (auto* tv = rv->thread_view())
                refresh(tv->message_list());
        }
    }
}

void ShellBase::handle_show_sender_status_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    if (s.show_sender_status_in_timeline == enabled)
        return;
    s.show_sender_status_in_timeline = enabled;
    s.save_to_disk(tesseract::config_dir());
    refresh_all_message_lists_display_prefs_();
    request_relayout_();
}

void ShellBase::handle_message_layout_changed_(
    tesseract::Settings::MessageLayout layout)
{
    auto& s = tesseract::Settings::instance();
    if (s.message_layout == layout)
        return;
    s.message_layout = layout;
    s.save_to_disk(tesseract::config_dir());

    refresh_all_message_lists_display_prefs_();
    request_relayout_();
}

#ifdef TESSERACT_CRASH_HANDLER_ENABLED
void ShellBase::handle_crash_reporting_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.crash_reporting_enabled = enabled;
    s.save_to_disk(tesseract::config_dir());
    tesseract::set_crash_reporting_enabled(enabled);
}
#endif

void ShellBase::handle_send_maps_urls_as_location_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.send_maps_urls_as_location = enabled;
    s.save_to_disk(tesseract::config_dir());
}

void ShellBase::handle_bundled_url_previews_toggle_(bool enabled, bool direct)
{
    auto& s = tesseract::Settings::instance();
    s.send_bundled_url_previews = enabled;
    s.fetch_url_previews_directly = direct;
    s.save_to_disk(tesseract::config_dir());

    // Global preference, per-account flag: push to every logged-in client.
    // A plain atomic store on the Rust side — safe on the UI thread.
    for (const auto& sess : account_manager_.accounts())
    {
        if (sess && sess->client)
            sess->client->set_bundled_url_previews(enabled, direct);
    }
}

#ifdef TESSERACT_UPDATE_CHECKS
void ShellBase::handle_check_for_updates_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.check_for_updates = enabled;
    s.save_to_disk(tesseract::config_dir());
}
#endif

void ShellBase::apply_search_indexing_pref_(tesseract::Client& client)
{
    // Resume live indexing for this account when the global preference is on.
    // The Rust side skips the history backfill when the index is already
    // populated, so this is cheap on every launch after the first enable. The
    // call is non-blocking (fire-and-forget spawn on the Rust side).
    if (tesseract::Settings::instance().index_messages_for_search)
        client.set_search_indexing_enabled(true);
}

void ShellBase::apply_membership_events_pref_(tesseract::Client& client)
{
    // The Rust-side AtomicBool defaults to false; only push the flag when the
    // persisted preference is on so the first room subscription for this
    // account already surfaces membership-change rows instead of waiting for
    // the user to toggle the checkbox after launch. A plain atomic store on
    // the Rust side — non-blocking.
    if (tesseract::Settings::instance().show_room_join_leave_events)
        client.set_show_membership_events(true);
}

void ShellBase::apply_bundled_url_previews_pref_(tesseract::Client& client)
{
    // Rust-side flags default to off; push only when the preference is on.
    // Plain atomic stores — non-blocking.
    const auto& s = tesseract::Settings::instance();
    if (s.send_bundled_url_previews)
        client.set_bundled_url_previews(true, s.fetch_url_previews_directly);
}

void ShellBase::apply_low_power_pref_(tesseract::Client& client)
{
    // The Rust-side AtomicBool defaults to false; only push when low power is
    // currently active so an account logging in mid-low-power immediately
    // suspends its warm-check / search-crawl work too. Plain atomic store —
    // non-blocking, safe on the worker thread this runs on. Reads
    // power_policy_ like apply_membership_events_pref_ reads Settings (both
    // UI-thread-owned); a race here only means a redundant store the next
    // on_mode_change corrects.
    if (low_power_active())
        client.set_low_power_mode(true);
}

void ShellBase::set_power_monitor_(std::unique_ptr<IPowerMonitor> pm)
{
    if (!pm)
        return;
    power_monitor_ = std::move(pm);

    if (!power_monitor_->has_battery())
    {
        // No battery: the "on battery" trigger can never fire, so low power
        // mode is unavailable. Leave power_policy_ at its inert default
        // (active() stays false), don't wire on_change, and never read the
        // persisted preference. The Settings group is hidden separately (see
        // low_power_available() / SettingsView::set_low_power_available).
        return;
    }

    // Wire on_mode_change *before* seeding, so the initial resolve below
    // applies a persisted `On` (or a launch-on-battery Auto state).
    power_policy_.on_mode_change = [this](bool active)
    {
        apply_low_power_mode_(active);
        on_low_power_mode_ui_(active);
    };

    // Feed the current OS signals first, then set the preference: set_pref()
    // resolves and applies immediately against those signals, so the initial
    // state skips the debounce (there is no flapping to guard against at
    // startup) and we avoid scheduling a post_to_ui_after_ before the shell's
    // surface exists.
    power_policy_.notify_os_power_saver(power_monitor_->os_power_saver_active());
    power_policy_.notify_on_battery(power_monitor_->on_battery_discharging());

    using LP = tesseract::Settings::LowPowerPreference;
    switch (tesseract::Settings::instance().low_power_pref)
    {
    case LP::On:
        power_policy_.set_pref(PowerPolicy::Pref::On);
        break;
    case LP::Off:
        power_policy_.set_pref(PowerPolicy::Pref::Off);
        break;
    case LP::Auto:
        power_policy_.set_pref(PowerPolicy::Pref::Auto);
        break;
    }

    power_monitor_->on_change = [this] { refresh_low_power_signals_(); };
}

void ShellBase::refresh_low_power_signals_()
{
    power_policy_.notify_os_power_saver(power_monitor_->os_power_saver_active());
    power_policy_.notify_on_battery(power_monitor_->on_battery_discharging());

    if (power_policy_.has_pending())
    {
        // Don't wait up to a full 30 s presence tick to settle the debounce.
        const int ms =
            static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    power_policy_.debounce())
                    .count()) +
            1000;
        post_to_ui_after_(ms, guarded([this] { power_policy_.notify_tick(); }));
    }
}

void ShellBase::set_low_power_preference_(
    tesseract::Settings::LowPowerPreference pref)
{
    if (!low_power_available())
        return; // no battery: the control is hidden; ignore stray calls
    tesseract::Settings::instance().low_power_pref = pref;
    tesseract::Settings::instance().save_to_disk(tesseract::config_dir());

    using LP = tesseract::Settings::LowPowerPreference;
    power_policy_.set_pref(pref == LP::On    ? PowerPolicy::Pref::On
                           : pref == LP::Off ? PowerPolicy::Pref::Off
                                             : PowerPolicy::Pref::Auto);
}

void ShellBase::apply_low_power_mode_(bool active)
{
    // set_low_power_mode / stop_* are all non-blocking SDK calls (atomic flag
    // flip / task abort), safe to issue straight from the UI thread — same as
    // handle_index_messages_toggle_.
    for (const auto& sess : account_manager_.accounts())
    {
        if (!sess || !sess->client)
            continue;
        sess->client->set_low_power_mode(active);
        if (active)
        {
            sess->client->stop_background_backfill();
            sess->client->stop_unread_prefetch();
        }
    }

    if (active)
    {
        // The 30 s notify_presence_tick_ still calls run_image_gc_ as a
        // backstop, so on-screen decodes are reclaimed, just less often.
        media_sweep_timer_.stop();
    }
    else
    {
        media_sweep_timer_.start();
        // Clear the fingerprints so the next push_rooms_ re-schedules the
        // bridge-status / unread-prefetch work it skipped while suspended.
        bridge_check_fingerprint_ = 0;
        unread_prefetch_fingerprint_ = 0;
    }

    // Avatars animate only while low power mode is off. Leaving it evicts the
    // stills fetched meanwhile so they come back animated; entering it just
    // invalidates in-flight fetches (animated entries freeze via peek_frame).
    if (tesseract::Settings::instance().animate_avatars)
        on_avatar_animation_mode_changed_(/*evict=*/!active);

    resolve_presence_polling_();
}

void ShellBase::resolve_presence_polling_()
{
    // Single writer of the SDK DM-presence-poll knob. Poll only when the user
    // allows presence, the window is on-screen, and we're not conserving power.
    const bool want = tesseract::Settings::instance().send_presence &&
                      last_window_active_ && !low_power_active();
    if (!client_)
        return;
    auto sess = active_account_;
    run_async_mut_(
        [sess, want]()
        {
            if (!sess || !sess->client)
                return;
            sess->client->set_presence_polling_enabled(want);
            if (want)
                sess->client->poll_presence_now();
        });
}

// Apply the persisted "Use historical MSC2545 compatibility" preference
// to a freshly-restored account's Rust client. Called right after
// restore_session/start_sync so the first image-pack rebuild already
// reflects the setting instead of defaulting to the Rust-side AtomicBool's
// on default (harmless when the setting is already on, but needed for a
// session where the user previously turned it off). NOT non-blocking:
// Client::set_msc2545_legacy_compat() synchronously rebuilds the image-pack
// cache (a per-room network-bound fetch) before returning, so this must
// only ever be called from a background thread (restore_all_accounts_
// blocking_ / finalize_login_blocking_), never the UI thread — confirmed
// by a ~2.6s stall measured on the UI thread before this was moved.
void ShellBase::apply_msc2545_legacy_compat_pref_(tesseract::Client& client)
{
    // Unlike show_room_join_leave_events, the Rust-side AtomicBool already
    // defaults to true (matching Settings::msc2545_legacy_compat's default),
    // but push it unconditionally so a session where the user previously
    // turned it off doesn't silently start back up in the (wrong) default-on
    // state. NOT cheap: Client::set_msc2545_legacy_compat() synchronously
    // rebuilds the image-pack cache (a per-room network-bound fetch) before
    // returning — this must only be called from a background thread, never
    // the UI thread (see the header doc comment).
    client.set_msc2545_legacy_compat(
        tesseract::Settings::instance().msc2545_legacy_compat);
}

void ShellBase::apply_current_theme_()
{
    auto& s = tesseract::Settings::instance();
    tk::ThemeMode mode =
        s.theme_pref == tesseract::Settings::ThemePreference::Dark
            ? tk::ThemeMode::Dark
        : s.theme_pref == tesseract::Settings::ThemePreference::Light
            ? tk::ThemeMode::Light
            : os_color_scheme_(); // System → ask the OS
    const tk::AccentTheme accent = shell_helpers::to_tk_accent(s.theme_accent);
    current_theme_ = tk::Theme::variant(mode, accent);
    if (accent == tk::AccentTheme::System)
    {
        last_system_accent_ = os_accent_color_();
        if (last_system_accent_)
            tk::apply_system_accent(current_theme_, *last_system_accent_);
    }
    else
    {
        last_system_accent_.reset();
    }
    apply_theme_ui_(current_theme_);
}

void ShellBase::on_system_accent_changed_()
{
    if (tesseract::Settings::instance().theme_accent !=
        tesseract::Settings::ThemeAccent::System)
    {
        return;
    }
    if (os_accent_color_() == last_system_accent_)
    {
        return; // nothing actually changed
    }
    apply_current_theme_(); // re-reads + stores last_system_accent_
}

void ShellBase::apply_theme_to_secondary_windows_(const tk::Theme& t)
{
    if (activity_window_)
    {
        activity_window_->apply_theme(t);
    }
    for (auto& w : owned_secondary_windows_)
    {
        if (w)
        {
            w->apply_theme(t);
        }
    }
}

// Called by each shell once at startup (with a freshly-queried native
// scale) and again from the main surface's set_on_scale_changed()
// callback whenever the display's scale changes live. A changed scale
// invalidates every cached thumbnail/avatar — they were fetched from
// the server at the old pixel size — so this clears both in-memory
// caches rather than leaving stale, wrong-size entries to linger
// indefinitely (thumbnail_cache()/image_cache() key purely by mxc/url,
// no size encoded, so a stale small entry would otherwise satisfy
// every future contains() check forever). DPI changes are rare, so a
// full flush + natural re-fetch on next paint is the simple, safe
// choice over rekeying every cache entry by requested size.
void ShellBase::set_current_scale_(float scale)
{
    if (std::abs(scale - current_scale_) < 0.01f)
    {
        return;
    }
    current_scale_ = scale;
    account_manager_.thumbnail_cache().clear();
    account_manager_.image_cache().clear();
    // The timeline and thread panel gate their avatar/media fetch callbacks on
    // a diff against the last-visible set (see MessageListView::
    // maybe_notify_visible_range_); a scale change doesn't alter which
    // messages are visible, so without this reset the diff would see no
    // change and never re-request the images just evicted above.
    if (main_app_ && main_app_->room_view())
    {
        if (auto* ml = main_app_->room_view()->message_list())
            ml->reset_visible_avatar_tracking();
        if (auto* tv = main_app_->room_view()->thread_view())
            if (auto* tml = tv->message_list())
                tml->reset_visible_avatar_tracking();
    }
    request_relayout_();
}

void ShellBase::set_theme_preference_(tesseract::Settings::ThemePreference pref)
{
    tesseract::Settings::instance().theme_pref = pref;
    tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
    apply_current_theme_();
}

void ShellBase::set_theme_accent_(tesseract::Settings::ThemeAccent accent)
{
    tesseract::Settings::instance().theme_accent = accent;
    tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
    apply_current_theme_();
}

// Mark the account-data-backed prefs (currently just the room layout —
// active room + open tabs — but this debounce+dirty-flag machinery is
// generic, so future im.gnomos.tesseract fields can reuse it) as changed
// and (re)start the debounced save timer — see persist_room_layout_pref_().
// Called from after_active_room_changed_() so every tab_open/tab_select/
// tab_close (and the account-switch path, which clears current_room_id_/
// tabs_ without calling after_active_room_changed_, and so correctly does
// NOT re-save the outgoing account's layout as empty) schedules a save.
// try_restore_tab_session_() also runs through after_active_room_changed_(),
// which re-schedules a save of the layout it just loaded — harmless (same
// content, coalesced by the debounce like any other save) rather than
// worth special-casing out.
void ShellBase::schedule_account_data_save_()
{
    if (tearing_down_ || !client_)
        return;
    account_data_dirty_ = true;
    // 2s: long enough that rapid tab-switching coalesces into one write
    // (matches the debounce_() pattern used for SaveSettings), short enough
    // that a crash or forced kill loses at most a couple of seconds of
    // layout changes instead of the whole session.
    debounce_(DebounceSlot::AccountDataSave, 2000,
              [this]() { persist_room_layout_pref_(); });
}

// (Re)construct settings_controller_ with the three standard callbacks
// (forwarding to post_to_ui_ / run_async_ / pick_image_file_) and wire its
// UnifiedPush up-connector from the active account (nullptr on platforms
// without UnifiedPush — a no-op there). Then calls bind_settings_controller_
// for the native widget + dialog-hook binding. Rebuilds on every call to
// match the per-login / per-account-switch behavior of the old inline sites.
void ShellBase::ensure_settings_controller_()
{
    settings_controller_ = std::make_unique<tesseract::SettingsController>(
        client_,
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); },
        [this](std::function<void()> fn) { run_async_(std::move(fn)); },
        [this](std::function<void(std::vector<uint8_t>, std::string)> cb)
        { pick_image_file_(std::move(cb)); },
        [this](const std::vector<uint8_t>& bytes) -> std::shared_ptr<tk::Image>
        {
            DecodedImage decoded = decode_image_(bytes, 256, 256);
            if (decoded.still)
                return std::shared_ptr<tk::Image>(std::move(decoded.still));
            if (!decoded.frames.empty())
                return std::shared_ptr<tk::Image>(std::move(decoded.frames.front()));
            return nullptr;
        },
        active_account_);
    // UnifiedPush up-connector (Linux only); nullptr elsewhere — a no-op.
    settings_controller_->set_up_connector(
        active_account_ ? active_account_->up_connector.get() : nullptr);
    bind_settings_controller_();
}

// (Re)construct history_export_controller_ with the two standard
// callbacks (post_to_ui_ / run_async_). Unlike
// ensure_settings_controller_, there is no matching bind_*_ pure
// virtual: show_save_folder_dialog stays unset (begin() is then a
// no-op) until a shell explicitly wires it, so all four shells compile
// untouched until they add the "Export History" trigger.
void ShellBase::ensure_history_export_controller_()
{
    history_export_controller_ = std::make_unique<tesseract::HistoryExportController>(
        client_,
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); },
        [this](std::function<void()> fn) { run_async_(std::move(fn)); },
        active_account_);

    // Wire the shared ExportHistoryDialog's request callbacks to the
    // controller, and the controller's results back into the dialog.
    // MainAppWidget's own confirm_provider_-style wiring (see
    // MainAppWidget.cpp) only knows how to *open* the dialog — reaching
    // Client-backed state needs ShellBase, same reasoning as every other
    // controller's bind_*_/ensure_*_ split.
    if (auto* dlg = main_app_ ? main_app_->export_history_dialog() : nullptr)
    {
        dlg->on_query_resume = [this](std::string room_id) {
            if (history_export_controller_) history_export_controller_->query_resume(std::move(room_id));
        };
        dlg->on_cancel_requested = [this]() {
            if (history_export_controller_) history_export_controller_->cancel();
        };
        dlg->on_stop_requested = [this]() {
            if (history_export_controller_) history_export_controller_->stop();
        };
        dlg->on_export_requested = [this](views::ExportHistoryDialog::Request req) {
            if (!history_export_controller_) return;
            tesseract::HistoryExportController::Request creq;
            creq.room_id = req.room_id;
            const RoomInfo* ri = room_by_id_(req.room_id);
            creq.room_display_name = (ri && !ri->name.empty()) ? ri->name : req.room_id;
            creq.format = req.format == views::ExportHistoryDialog::Format::Html
                              ? tesseract::HistoryExportController::Format::Html
                              : tesseract::HistoryExportController::Format::Text;
            creq.include_images = req.include_images;
            creq.zip_output = req.zip_output;
            creq.stop_at_ts_ms = req.stop_at_ts_ms;
            creq.resume_from_event_id = req.resume_from_event_id;
            history_export_controller_->begin(std::move(creq));
        };
        dlg->on_go_to_other_export = [this](std::string room_id) {
            navigate_to_room_(room_id);
            if (auto* d = main_app_->export_history_dialog())
            {
                const RoomInfo* ri = room_by_id_(room_id);
                const std::string display = (ri && !ri->name.empty()) ? ri->name : room_id;
                if (history_export_controller_)
                    d->open_in_progress(std::move(room_id), display,
                                        history_export_controller_->last_progress());
            }
        };

        history_export_controller_->on_started =
            [this](std::string room_id, std::string /*out_path*/) {
                on_persistent_status_activate_ = [this, room_id]() {
                    navigate_to_room_(room_id);
                    if (auto* d = main_app_ ? main_app_->export_history_dialog() : nullptr)
                    {
                        const RoomInfo* ri = room_by_id_(room_id);
                        const std::string display = (ri && !ri->name.empty()) ? ri->name : room_id;
                        if (history_export_controller_)
                            d->open_in_progress(room_id, display, history_export_controller_->last_progress());
                    }
                };
                // Switch the dialog (if it's the one that was just clicked
                // through) into the In-progress state immediately — don't
                // wait for the first real progress tick, which is a
                // network round-trip away and would otherwise leave the
                // Export button visibly clickable for that whole gap.
                if (auto* d = main_app_ ? main_app_->export_history_dialog() : nullptr)
                    d->show_progress(tesseract::RoomExportProgress{});
                show_status_message_(tk::tr("Exporting history…"), /*auto_clear_ms=*/0);
            };
        history_export_controller_->on_progress =
            [this](const tesseract::RoomExportProgress& p) {
                if (auto* d = main_app_ ? main_app_->export_history_dialog() : nullptr)
                    d->show_progress(p);
                show_status_message_(
                    tk::trf(tk::tr("Exporting history… {0} messages"),
                           {std::to_string(p.events_written)}),
                    /*auto_clear_ms=*/0);
            };
        history_export_controller_->on_finished =
            [this](bool ok, bool cancelled, std::string out_path,
                  std::uint64_t events_written, std::string error) {
                if (auto* d = main_app_ ? main_app_->export_history_dialog() : nullptr)
                    d->show_finished(ok, cancelled, std::move(out_path), events_written,
                                    std::move(error));
                on_persistent_status_activate_ = nullptr;
                if (status_override_active_)
                {
                    ++status_msg_gen_;
                    status_override_active_ = false;
                    on_restore_status_ui_();
                }
            };
        history_export_controller_->on_resume_available =
            [this](tesseract::RoomExportCheckpoint cp) {
                if (auto* d = main_app_ ? main_app_->export_history_dialog() : nullptr)
                    d->set_resume_checkpoint(std::move(cp));
            };
    }
}

void ShellBase::pick_and_set_room_avatar_(const std::string& room_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    auto* c = acting_client;
    if (!c)
        return;

    pick_image_file_(
        [this, c, weak_acting = std::weak_ptr<AccountSession>(acting),
         room_id](std::vector<uint8_t> bytes, std::string mime) mutable
        {
            if (bytes.empty())
                return; // cancelled
            auto sess = weak_acting.lock();
            if (!sess || sess->client.get() != c)
                return; // logged out between pick and callback
            run_async_mut_(
                [this, sess, room_id,
                 bytes = std::move(bytes),
                 mime  = std::move(mime)]() mutable
                {
                    if (!sess || !sess->client)
                        return;
                    auto upload = sess->client->upload_media(bytes, mime);
                    std::string status;
                    if (!upload.ok)
                    {
                        status = tk::trf(tk::tr("Failed to upload avatar: {0}"), {upload.message});
                    }
                    else if (auto set = sess->client->set_user_room_avatar(room_id, upload.message);
                             !set.ok)
                    {
                        status = tk::trf(tk::tr("Failed to set room avatar: {0}"), {set.message});
                    }
                    else
                    {
                        status = tk::tr("Avatar updated for this room");
                    }
                    post_to_ui_alive_([this, status = std::move(status)]
                                      { show_status_message_(status, 4000); });
                });
        });
}

// Open a file picker, upload the selected image as raw media (never
// committing it to any room/profile state), and stage the resulting
// mxc:// URI into `target` via set_staged_avatar(). The room-level
// m.room.avatar state event is only sent when the user clicks Accept
// (see apply_room_settings_). No-op if not logged in or `target` is
// null. Call from the UI thread. `target` is whichever RoomSettingsView
// instance requested the upload — room_view_->room_settings_view() for
// a normal room, or main_app_->space_root()->settings_view() for a
// space root — both operate on room ids generically, so this one
// implementation serves both without duplicating the upload/retry logic.
void ShellBase::stage_room_settings_avatar_upload_(const std::string& room_id,
                                                   views::RoomSettingsView* target,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    auto* c = acting_client;
    if (!c || !target)
        return;

    target->set_avatar_busy(true);

    pick_image_file_(
        [this, c, weak_acting = std::weak_ptr<AccountSession>(acting), room_id,
         target](std::vector<uint8_t> bytes, std::string mime) mutable
        {
            if (bytes.empty())
            {
                // Cancelled — clear the busy indicator. Arrives asynchronously
                // from the platform file dialog, not nested in the click
                // dispatch that set busy(true), so nothing else repaints.
                target->set_avatar_busy(false);
                request_repaint_();
                return;
            }
            auto still = weak_acting.lock();
            if (!still || still->client.get() != c)
                return; // logged out between pick and callback

            const std::uint64_t gen = target->open_generation();
            auto shared_bytes =
                std::make_shared<std::vector<uint8_t>>(std::move(bytes));

            // Purely local: decode a preview off-thread. No Client/network
            // call here — the room settings dialog stages every field
            // locally until Accept, and the avatar is no exception: upload
            // happens only inside apply_room_settings_ at commit time (see
            // RoomSettingsChanges::avatar_upload).
            run_async_(
                [this, room_id, target, gen, shared_bytes, mime]()
                {
                    DecodedImage decoded = decode_image_(*shared_bytes, 256, 256);
                    std::shared_ptr<tk::Image> preview;
                    if (decoded.still)
                        preview = std::shared_ptr<tk::Image>(std::move(decoded.still));
                    else if (!decoded.frames.empty())
                        preview =
                            std::shared_ptr<tk::Image>(std::move(decoded.frames.front()));
                    post_to_ui_alive_(
                        [this, room_id, target, gen, shared_bytes, mime,
                         preview = std::move(preview)]() mutable
                        {
                            if (!target->is_open() || target->room_id() != room_id ||
                                target->open_generation() != gen)
                                return; // dialog closed/switched/reopened meanwhile
                            target->set_avatar_busy(false);
                            target->set_staged_avatar_pending(
                                std::move(*shared_bytes), std::move(mime),
                                std::move(preview));
                            request_repaint_();
                        });
                });
        });
}

std::vector<std::string> ShellBase::apply_image_pack_changes_(
    tesseract::Client* client, const views::ImagePackEditorResult& result)
{
    std::vector<std::string> errors;
    if (!client)
    {
        errors.push_back("image_packs: not logged in");
        return errors;
    }

    for (const auto& state_key : result.removed_state_keys)
    {
        auto r = client->remove_room_pack(result.room_id, state_key);
        if (!r.ok)
            errors.push_back("image_packs.remove: " + r.message);
    }

    for (const auto& pack : result.packs)
    {
        std::vector<tesseract::PackImageInput> resolved;
        resolved.reserve(pack.images.size());
        for (const auto& img : pack.images)
        {
            if (!img.existing_url.empty())
            {
                resolved.push_back(tesseract::PackImageInput{
                    .shortcode  = img.shortcode,
                    .url        = img.existing_url,
                    .body       = img.body,
                    .info_json  = img.info_json,
                });
            }
            else if (!img.pending_bytes.empty())
            {
                auto upload = client->upload_media(img.pending_bytes, img.pending_mime);
                if (!upload.ok)
                {
                    errors.push_back("image_packs." + pack.display_name + ": " +
                                     upload.message);
                    continue; // drop just this image; keep the rest of the pack
                }
                resolved.push_back(tesseract::PackImageInput{
                    .shortcode  = img.shortcode,
                    .url        = upload.message,
                    .body       = img.body,
                    .info_json  = img.info_json,
                });
            }
        }

        auto r = client->save_room_pack(result.room_id, pack.state_key, pack.is_new,
                                        pack.display_name,
                                        static_cast<std::uint8_t>(pack.usage), resolved);
        if (!r.ok)
            errors.push_back("image_packs." + pack.display_name + ": " + r.message);
    }

    return errors;
}

// Send a state event for each populated optional field in `changes`,
// attempting every one even if an earlier call fails so a partial
// success (e.g. topic saved, avatar denied) isn't silently lost. The
// media-override write (personal account data, not a state event) is
// fire-and-forget and never contributes to the joined error string —
// its optimistic cache update happens separately, in
// commit_room_media_preview_override_, called by the caller only after
// this function reports success. Takes the whole RoomSettingsChanges
// (rather than exploding it into one param per field) since it's
// already the exact aggregate RoomSettingsView produces from Accept.
// Blocks — call from a worker thread (run_async_mut_).
ShellBase::RoomSettingsCommitOutcome ShellBase::apply_room_settings_(
    tesseract::Client* client, const std::string& room_id,
    const views::RoomSettingsChanges& changes)
{
    RoomSettingsCommitOutcome out;
    if (!client)
    {
        out.error = tk::tr("not logged in");
        return out;
    }
    std::vector<std::string> errors;
    if (changes.name)
    {
        auto r = client->set_room_display_name(room_id, *changes.name);
        if (!r.ok) errors.push_back("name: " + r.message);
    }
    if (changes.topic)
    {
        auto r = client->set_room_topic(room_id, *changes.topic);
        if (!r.ok) errors.push_back("topic: " + r.message);
    }
    if (changes.avatar_upload)
    {
        auto upload = client->upload_media(changes.avatar_upload->bytes,
                                           changes.avatar_upload->mime);
        if (!upload.ok)
            errors.push_back("avatar: " + upload.message);
        else
        {
            auto r = client->set_room_avatar(room_id, upload.message);
            if (!r.ok) errors.push_back("avatar: " + r.message);
        }
    }
    else if (changes.avatar_mxc)
    {
        auto r = client->set_room_avatar(room_id, *changes.avatar_mxc);
        if (!r.ok) errors.push_back("avatar: " + r.message);
    }
    // Encryption can only be turned on — compute_room_settings_changes never
    // populates this with false, but the guard restates that invariant
    // locally so it holds even if that changes upstream.
    if (changes.is_encrypted && *changes.is_encrypted)
    {
        auto r = client->set_room_encryption(room_id);
        if (!r.ok) errors.push_back("encryption: " + r.message);
    }
    if (changes.join_rule)
    {
        auto r = client->set_room_join_rule(room_id, *changes.join_rule);
        if (!r.ok) errors.push_back("join_rule: " + r.message);
    }
    if (changes.guest_access)
    {
        auto r = client->set_room_guest_access(room_id, *changes.guest_access);
        if (!r.ok) errors.push_back("guest_access: " + r.message);
    }
    if (changes.history_visibility)
    {
        auto r = client->set_room_history_visibility(room_id, *changes.history_visibility);
        if (!r.ok) errors.push_back("history_visibility: " + r.message);
    }
    if (changes.permissions)
    {
        auto r = client->set_room_power_levels(room_id, *changes.permissions);
        if (!r.ok) errors.push_back("permissions: " + r.message);
    }
    if (changes.image_packs)
    {
        auto pack_errors = apply_image_pack_changes_(client, *changes.image_packs);
        errors.insert(errors.end(), pack_errors.begin(), pack_errors.end());
    }
    // Blocking write — like every other field above, a failure here is
    // surfaced in the aggregate error string. The optimistic cache update
    // happens separately (commit_room_media_preview_override_), called by
    // the caller only once this function reports overall success.
    if (changes.media_override)
    {
        auto r = client->save_room_media_preview_override(
            room_id, changes.media_override->has_override, changes.media_override->mode);
        if (!r.ok) errors.push_back("media_preview: " + r.message);
    }
    out.ok = errors.empty();
    for (std::size_t i = 0; i < errors.size(); ++i)
    {
        if (i) out.error += "; ";
        out.error += errors[i];
    }
    return out;
}
} // namespace tesseract
