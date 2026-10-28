#include "views/font_prefs.h"
#include "app/DeferredTeardown.h"
#include "app/Launch.h"
#include "app/ShellBase.h"
#include <tesseract/crash_handler.h>
#include <tesseract/version.h>
#include "app/shell_helpers.h"
#include "views/MainAppWidget.h"
#include "views/SettingsView.h"
#include "views/image_pack_order.h"
#include <algorithm>

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
            refresh_threads_list_async_(room_id);
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
// of MSC4391 bot commands for `room_id` changes. No-op unless `room_id` is the
// active room, in which case the open slash popup is refreshed.
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

void ShellBase::apply_send_presence_()
{
    const bool enabled = tesseract::Settings::instance().send_presence;

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

void ShellBase::apply_index_messages_()
{
    const bool enabled = tesseract::Settings::instance().index_messages_for_search;

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

void ShellBase::apply_show_membership_events_()
{
    if (client_)
    {
        client_->set_show_membership_events(
            tesseract::Settings::instance().show_room_join_leave_events);
        if (!current_room_id_.empty())
            client_->subscribe_room(current_room_id_);
    }
}

void ShellBase::apply_msc2545_legacy_compat_()
{
    // set_msc2545_legacy_compat synchronously rebuilds the image-pack cache
    // and fires on_image_packs_updated Rust-side, which the shells already
    // handle to refresh list_image_packs()-driven UI — no extra refresh
    // call needed here. That rebuild is a network-bound fetch, so it runs on
    // the mutable pool, never the UI thread (see apply_msc2545_legacy_compat_pref_).
    if (!client_)
        return;
    auto sess = active_account_;
    const bool want = tesseract::Settings::instance().msc2545_legacy_compat;
    run_async_mut_(
        [sess, want]()
        {
            if (!sess || !sess->client)
                return;
            sess->client->set_msc2545_legacy_compat(want);
        });
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

void ShellBase::apply_fonts_()
{
    const auto& fonts = tesseract::Settings::instance().fonts;
    if (!tesseract::apply_font_prefs(fonts))
        return; // clamped to the config already in force
    // Every RootWidget (windows, popups) drops its font-dependent state and
    // schedules a relayout; message lists / room lists flush their own row
    // caches from their on_fonts_changed() overrides. The per-shell native
    // part (apply_fonts_ui_) runs per window from apply_setting_to_window_.
    tk::RootWidget::apply_fonts_to_all_roots();
}

void ShellBase::apply_room_list_density_()
{
    if (main_app_)
        main_app_->set_room_list_compact(
            tesseract::Settings::instance().room_list_density ==
            tesseract::Settings::RoomListDensity::Compact);
    request_relayout_();
}

void ShellBase::apply_bundled_url_previews_()
{
    const auto& s = tesseract::Settings::instance();
    // Global preference, per-account flag: push to every logged-in client.
    // A plain atomic store on the Rust side — safe on the UI thread.
    for (const auto& sess : account_manager_.accounts())
    {
        if (sess && sess->client)
            sess->client->set_bundled_url_previews(s.send_bundled_url_previews,
                                                   s.fetch_url_previews_directly);
    }
}

void ShellBase::handle_setting_changed_(views::SettingKey key)
{
    using K = views::SettingKey;
    auto& s = tesseract::Settings::instance();

    // The settings view already wrote the new value into Settings; most keys
    // need nothing beyond persisting it (they are read when next used).
    s.save_to_disk(tesseract::config_dir());

    // Process-global effects run once, here; everything that touches a
    // window's own widgets / client / policy runs per window below.
    switch (key)
    {
    case K::LaunchAtLogin:
        apply_launch_at_login_();
        // The failure path may have corrected the cached bool.
        s.save_to_disk(tesseract::config_dir());
        break;
    case K::Proxy:
        // Signed in: existing clients keep the launch proxy, so the settings
        // view already shows "Restart now". Signed out (settings opened from
        // the login screen): no connection exists yet, so apply it now.
        if (settings_from_login_())
        {
            tesseract::apply_proxy_live(s.proxy_mode, s.proxy_url);
            if (stats_settings_view_)
                stats_settings_view_->set_proxy_restart_pending(
                    tesseract::proxy_changed_since_launch(s.proxy_mode,
                                                          s.proxy_url));
            rebind_login_client_ui_();
            login_recheck_pending_ = true;
        }
        break;
    case K::Fonts:
        apply_fonts_();
        break;
    case K::BundledUrlPreviews:
        apply_bundled_url_previews_();
        break;
    case K::IndexMessages:
        apply_index_messages_();
        break;
#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    case K::CrashReporting:
        tesseract::set_crash_reporting_enabled(s.crash_reporting_enabled);
        break;
#endif
    default:
        break;
    }

    // Every main window is its own ShellBase; a preference is app-wide, so each
    // one applies it (the originating window may be unregistered, e.g. in
    // tests or mid-teardown, so make sure it is covered exactly once).
    bool self_seen = false;
    for (ShellBase* w : account_manager_.all_windows())
    {
        self_seen = self_seen || w == this;
        w->apply_setting_to_window_(key);
    }
    if (!self_seen)
        apply_setting_to_window_(key);
}

void ShellBase::apply_setting_to_window_(views::SettingKey key)
{
    using K = views::SettingKey;
    auto& s = tesseract::Settings::instance();

    switch (key)
    {
    case K::LowPower:
        apply_low_power_preference_();
        break;
    case K::Theme:
    case K::Accent:
        apply_current_theme_();
        break;
    case K::GroupInactive:
    case K::GroupUnread:
    case K::InactivePeriod:
        refresh_room_list_();
        break;
    case K::ShowMembershipEvents:
        apply_show_membership_events_();
        break;
    case K::ShowSenderStatus:
    case K::MessageLayout:
    case K::BubblesOwnAvatar:
    case K::TimelineColumnWidth:
    case K::TimelineColumnAnchor:
        refresh_all_message_lists_display_prefs_();
        request_relayout_();
        break;
    case K::AnimateAvatars:
        apply_animate_avatars_();
        break;
    case K::RoomListDensity:
        apply_room_list_density_();
        break;
    case K::Fonts:
        apply_fonts_ui_();
        request_relayout_();
        break;
    case K::Notifications:
        // Each window's active account owns its own UnifiedPush connector.
        if (settings_controller_)
            settings_controller_->set_notifications_enabled(s.notifications_enabled);
        break;
    case K::MediaPreviews:
    case K::InviteAvatars:
        apply_media_preview_config_();
        break;
    case K::SendPresence:
        apply_send_presence_();
        break;
    case K::Msc2545Compat:
        apply_msc2545_legacy_compat_();
        break;
    default:
        break; // persist-only, or handled once in handle_setting_changed_
    }
}

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

void ShellBase::apply_low_power_preference_()
{
    if (!low_power_available())
        return; // no battery: the control is hidden; ignore stray calls
    const auto pref = tesseract::Settings::instance().low_power_pref;

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

void ShellBase::apply_theme_ui_(const tk::Theme& t)
{
    current_theme_ = t;
    // Backends keep a pointer to the theme they are given, so hand out the
    // member, not the parameter (callers may pass a temporary).
    const tk::Theme& theme = current_theme_;
    apply_native_theme_(theme);
    for_each_shell_surface_([&theme](tk::SurfaceBase& surface)
                            { surface.apply_theme(theme); });
    if (settings_surface_)
        settings_surface_->apply_theme(theme);
    if (login_surface_)
        login_surface_->apply_theme(theme);
    apply_theme_to_secondary_windows_(theme);
    after_theme_applied_(theme);
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
    if (call_window_)
    {
        call_window_->apply_theme(t);
    }
    if (compose_popups_)
    {
        compose_popups_->apply_theme(t);
    }
    if (account_picker_popup_)
    {
        account_picker_popup_->set_theme(t);
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
// without UnifiedPush — a no-op there), install the key export/import dialog
// hooks, then call bind_settings_controller_ to bind it to the settings view.
// Rebuilds on every call (per login / per account switch).
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

    // Message-key export/import dialogs and their result messages.
    settings_controller_->show_passphrase_prompt =
        [this](std::string title, std::function<void(std::string)> cb)
    { prompt_passphrase_(std::move(title), std::move(cb)); };
    settings_controller_->show_save_file_dialog =
        [this](std::string suggested_name, std::function<void(std::string)> cb)
    {
        pick_save_file_(tk::tr("Export message keys"),
                        std::move(suggested_name), std::move(cb));
    };
    settings_controller_->show_open_file_dialog =
        [this](std::function<void(std::string)> cb)
    { pick_open_file_(tk::tr("Import message keys"), std::move(cb)); };
    settings_controller_->on_export_keys_result =
        [this](bool ok, std::string error)
    {
        show_message_dialog_(
            ok ? tk::tr("Export complete") : tk::tr("Export failed"),
            ok ? tk::tr("Message keys exported successfully.")
               : std::move(error),
            !ok);
    };
    settings_controller_->on_import_keys_result =
        [this](bool ok, std::string error)
    {
        show_message_dialog_(
            ok ? tk::tr("Import complete") : tk::tr("Import failed"),
            ok ? tk::tr("Message keys imported successfully.")
               : std::move(error),
            !ok);
    };
    bind_settings_controller_();
}

// (Re)construct history_export_controller_ with the two standard
// callbacks (post_to_ui_ / run_async_) and the folder picker (pick_folder_).
void ShellBase::ensure_history_export_controller_()
{
    history_export_controller_ = std::make_unique<tesseract::HistoryExportController>(
        client_,
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); },
        [this](std::function<void()> fn) { run_async_(std::move(fn)); },
        active_account_);
    history_export_controller_->show_save_folder_dialog =
        [this](std::string /*suggested_name*/,
               std::function<void(std::string)> cb)
    {
        pick_folder_(tk::tr("Choose a folder for the exported history"),
                     std::move(cb));
    };

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

void ShellBase::on_server_info_ready_ui_()
{
    if (stats_settings_view_)
        stats_settings_view_->set_server_info(server_info_);
    if (main_app_ && main_app_->room_view())
        main_app_->room_view()->header()->set_jump_to_date_enabled(
            server_info_.supports_msc3030);
    request_relayout_();
}

void ShellBase::on_own_extended_profile_ready_ui_()
{
    if (!stats_settings_view_)
        return;
    stats_settings_view_->set_extended_profile(own_extended_profile_);
    stats_settings_view_->request_relayout();
}

void ShellBase::on_profile_field_result_ui_(const std::string& key, bool ok,
                                            const std::string& error)
{
    if (!stats_settings_view_)
        return;
    stats_settings_view_->set_profile_field_busy(key, false);
    if (!ok)
        stats_settings_view_->set_profile_field_error(key, error);
    stats_settings_view_->request_relayout();
}

void ShellBase::configure_new_settings_view_(views::SettingsView& view)
{
    stats_settings_view_ = &view;
    view.set_low_power_available(low_power_available());
    wire_settings_view_(&view);
    view.on_close = [this] { close_app_settings_ui_(); };
    view.on_logout = [this]
    {
        close_app_settings_ui_();
        logout_active_account_ui_();
    };
    view.on_reset_identity = [this]
    {
        close_app_settings_ui_();
        begin_crypto_identity_reset_();
    };
    view.on_tab_changed = [this]
    {
        if (stats_settings_view_)
            stats_settings_view_->request_relayout();
    };
    view.load_device_lists();
    view.set_server_info(server_info_);
}

void ShellBase::populate_settings_view_(views::SettingsView& view)
{
    view.set_account_info(my_display_name_, my_user_id_, my_avatar_url_);
    view.set_image_provider(make_avatar_image_provider_());
    view.load_persisted_settings();
    view.set_signed_out_mode(settings_from_login_());
    refresh_launch_at_login_pref_();
    push_tray_available_ui_();
    const auto& p = own_extended_profile_;
    if (!p.pronouns.empty() || !p.tz.empty() || !p.biography.empty() ||
        !p.status_emoji.empty() || !p.status_text.empty())
        view.set_extended_profile(p);
    view.request_relayout();
    compute_cache_sizes_(
        [this](uint64_t local, uint64_t sdk, uint64_t memory, uint64_t mh,
               uint64_t mm, uint64_t dh, uint64_t dm)
        {
            if (stats_settings_view_)
                stats_settings_view_->set_cache_sizes(local, sdk, memory, mh,
                                                      mm, dh, dm);
        });
}


void ShellBase::ensure_settings_view_()
{
    if (stats_settings_view_)
        return;
    // A teardown is still pending for the previous surface: finish it now so
    // the old page is unmounted before the new one replaces it.
    if (settings_surface_)
    {
        unmount_settings_surface_ui_();
        settings_surface_.reset();
    }
    ++settings_gen_;
    settings_surface_ = create_settings_surface_ui_();
    if (!settings_surface_)
        return;
    auto view = tk::create_root_widget<views::SettingsView>(
        &settings_surface_->host());
    views::SettingsView* v = view.get();
    settings_surface_->set_root(std::move(view));
    settings_surface_->apply_theme(current_theme_);
    configure_new_settings_view_(*v);
    settings_view_created_ui_(*v);
}

bool ShellBase::prepare_settings_view_()
{
    ensure_settings_view_();
    if (!stats_settings_view_)
        return false;
    populate_settings_view_(*stats_settings_view_);
    // Only now does the controller have a view to bind to: it was created at
    // login, before this lazily built view existed.
    if (settings_controller_)
        bind_settings_controller_();
    return true;
}

void ShellBase::teardown_settings_view_()
{
    if (!settings_surface_)
        return;
    stats_settings_view_ = nullptr;
    const std::uint64_t gen = ++settings_gen_;
    std::weak_ptr<bool> alive = weak_flag();
    auto destroy = [this, alive]
    {
        if (alive.expired())
            return;
        unmount_settings_surface_ui_();
        settings_surface_.reset();
    };
    auto is_current = [this, alive, gen]
    { return !alive.expired() && settings_gen_ == gen; };
    if (tk::Host* host = main_host_())
        schedule_deferred_teardown(*host, std::move(is_current), std::move(destroy));
    else
        destroy();
}

void ShellBase::bind_settings_controller_()
{
    if (!stats_settings_view_ || !settings_controller_)
        return;
    wire_settings_controller_common_(stats_settings_view_,
                                     settings_controller_.get());
    stats_settings_view_->set_user_pack_image_provider(
        make_static_image_provider_with_fetch_(96, 96));
    stats_settings_view_->on_user_pack_pending_image_added =
        [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
               const std::string& mime)
    {
        if (stats_settings_view_)
            handle_user_pack_pending_image_added_(
                local_id, bytes, mime, stats_settings_view_->user_pack_editor());
    };
}

void ShellBase::update_settings_anim_regions_()
{
    if (app_settings_open_ && settings_surface_)
        settings_surface_->update_anim_regions();
}

void ShellBase::repaint_settings_ui_()
{
    if (app_settings_open_ && settings_surface_)
        settings_surface_->host().request_repaint();
}

void ShellBase::on_launch_at_login_pref_ui_(bool enabled)
{
    if (stats_settings_view_)
        stats_settings_view_->set_launch_at_login_pref(enabled);
}

void ShellBase::on_tray_available_ui_(bool available)
{
    if (stats_settings_view_)
        stats_settings_view_->set_tray_available(available);
}

} // namespace tesseract
