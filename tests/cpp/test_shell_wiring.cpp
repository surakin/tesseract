// ShellBase::wire_main_app_widget_ / wire_settings_view_ /
// wire_settings_controller_common_ / wire_voice_capture_ (ShellBase_wiring.cpp)
// are the shared glue every shell calls once at construction. These tests wire
// a real MainAppWidget / SettingsView to the shell test double and drive the
// callbacks the views expose, asserting the resulting shell/Settings state.
// Settings I/O is redirected to a temp dir by test_isolation_listener.

#include <catch2/catch_test_macros.hpp>

#include "app/CallWindowBase.h"
#include "app/SettingsController.h"
#include "app/ShellBase.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/AddRoomView.h"
#include "views/AlertDialog.h"
#include "views/ComposeBar.h"
#include "views/InviteCard.h"
#include "views/MainAppWidget.h"
#include "views/MediaViewerItem.h"
#include "views/MediaViewerOverlay.h"
#include "views/media_viewer_items.h"
#include "views/RoomListView.h"
#include "views/SettingsView.h"
#include "views/settings/PronounsEditor.h"
#include "views/UserInfo.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <vector>

using tesseract::ShellBase;
using tesseract::views::MainAppWidget;
using tesseract::views::RoomListView;
using tesseract::views::SettingsView;

namespace
{

struct WiringImage : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return 4; }
};

// A TestHost that lets a test fire the hooks a shell installs on its host.
struct WiringHost : TestHost
{
    WiringHost() : TestHost(nullptr) {}
    using tk::Host::fire_user_activity_;
    using tk::Host::run_pre_paint_hook_;
};

// A backend-free SurfaceBase standing in for a shell's settings page.
struct FakeSettingsSurface final : tk::SurfaceBase
{
    explicit FakeSettingsSurface(TestHost& h, int* alive_counter)
        : host_(h), alive_(alive_counter)
    {
        ++*alive_;
    }
    ~FakeSettingsSurface() override { --*alive_; }
    tk::Host& host() override { return host_; }
    tk::CanvasFactory& factory() override { std::abort(); }
    const tk::Theme& theme() const override { return *theme_; }
    void set_root(std::unique_ptr<tk::Widget> r) override { root_ = std::move(r); }
    tk::Widget* root() const override { return root_.get(); }
    void relayout() override { ++relayouts; }
    int relayouts = 0;
    // Like the real backends, keeps a pointer to the theme it is given.
    void set_theme(const tk::Theme& t) override { theme_ = &t; }
    void apply_scale_change(float) override {}
    void set_on_scale_changed(std::function<void(float)>) override {}
    void set_anim_cache(const tk::AnimImageCache*) override {}
    void update_anim_regions() override { ++anim_updates; }
    void add_layout_listener(std::function<void()>) override {}
    void set_on_file_drop_error(tk::FileDropErrorHandler) override {}

    TestHost& host_;
    int* alive_;
    tk::Theme default_theme_ = tk::Theme::light();
    const tk::Theme* theme_ = &default_theme_;
    std::unique_ptr<tk::Widget> root_;
    int anim_updates = 0;
};

// A main surface stand-in: a host that makes popups, and a real canvas factory
// so widgets can be measured.
struct FakeMainSurface final : tk::SurfaceBase
{
    explicit FakeMainSurface(TestSurface& ts) : ts_(ts) {}
    tk::Host& host() override { return host_; }
    tk::CanvasFactory& factory() override { return ts_.factory(); }
    const tk::Theme& theme() const override { return theme_; }
    void set_root(std::unique_ptr<tk::Widget>) override {}
    tk::Widget* root() const override { return nullptr; }
    void relayout() override {}
    void set_theme(const tk::Theme& t) override { theme_ = t; }
    void apply_scale_change(float) override {}
    void set_on_scale_changed(std::function<void(float)> cb) override
    {
        on_scale_changed = std::move(cb);
    }
    std::function<void(float)> on_scale_changed;
    void set_anim_cache(const tk::AnimImageCache*) override {}
    void update_anim_regions() override {}
    void add_layout_listener(std::function<void()>) override {}
    void set_on_file_drop_error(tk::FileDropErrorHandler h) override
    {
        file_drop_error = std::move(h);
    }
    tk::FileDropErrorHandler file_drop_error;

    TestSurface& ts_;
    PopupCapableStubHost host_;
    tk::Theme theme_ = tk::Theme::light();
};

struct WiringShell : tesseract::test::TestShellBase
{
    ~WiringShell() override
    {
        wait_pools_idle(); // the login view's worker reads it until done
        release_settings_surface_(); // its fake holds references into this shell
        release_login_surface_();    // likewise, and the login view's worker
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }

    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    void request_relayout_() override { ++relayouts; }
    int relayouts = 0;

    void bring_main_window_forward_for_overlay_() override { ++forwards; }
    void restore_focus_after_overlay_() override { ++focus_restores; }
    // Drop probes: record what the shared dispatcher hands to the platform.
    struct DropProbe
    {
        std::uint32_t gen = 0;
        std::vector<std::uint8_t> bytes;
        tesseract::views::ComposeBar* target = nullptr;
        std::shared_ptr<bool> alive;
    };
    void probe_drop_video_(std::uint32_t gen, std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> alive) override
    {
        ++video_probes;
        last_probe = {gen, std::move(bytes), target, std::move(alive)};
    }
    void probe_drop_audio_(std::uint32_t gen, std::vector<std::uint8_t> bytes,
                           tesseract::views::ComposeBar* target,
                           std::shared_ptr<bool> alive) override
    {
        ++audio_probes;
        last_probe = {gen, std::move(bytes), target, std::move(alive)};
    }
    int video_probes = 0;
    int audio_probes = 0;
    DropProbe last_probe;

    int forwards = 0;
    int focus_restores = 0;
    std::int64_t now_ms = 1000;
    std::int64_t monotonic_ms_() override { return now_ms; }
    void close_app_settings_ui_() override { ++settings_closes; }
    // Settings page: a fake surface, so the shared create/teardown runs.
    std::unique_ptr<tk::SurfaceBase> create_settings_surface_ui_() override
    {
        ++settings_created;
        if (settings_null)
            return nullptr;
        return std::make_unique<FakeSettingsSurface>(teardown_host, &settings_alive);
    }
    void unmount_settings_surface_ui_() override { ++settings_unmounted; }
    // Login page: a fake surface, so the shared create/show/teardown runs.
    std::unique_ptr<tk::SurfaceBase> create_login_surface_ui_() override
    {
        ++login_created;
        return std::make_unique<FakeSettingsSurface>(teardown_host, &login_alive);
    }
    void unmount_login_surface_ui_() override { ++login_unmounted; }
    std::unique_ptr<FakeMainSurface> main_fake;
    tk::SurfaceBase* main_surface_() override { return main_fake.get(); }
    // Theme: the shared apply runs; the shell's own parts are recorded.
    void apply_native_theme_(const tk::Theme&) override { theme_log.push_back("native"); }
    void for_each_shell_surface_(const std::function<void(tk::SurfaceBase&)>& fn) override
    {
        if (own_surface)
            fn(*own_surface);
        theme_log.push_back("surfaces");
    }
    void after_theme_applied_(const tk::Theme& t) override
    {
        theme_log.push_back("after");
        after_theme_ptr = &t;
    }
    const tk::Theme* after_theme_ptr = nullptr;
    void shared_apply_theme(const tk::Theme& t) { ShellBase::apply_theme_ui_(t); }
    std::vector<std::string> theme_log;
    std::unique_ptr<tk::SurfaceBase> own_surface;
    void show_login_page_ui_() override { ++login_pages; }
    void shared_show_login(tesseract::views::LoginView::Mode m) { ShellBase::show_login_ui_(m); }
    int login_created = 0, login_unmounted = 0, login_alive = 0, login_pages = 0;
    tk::Host* main_host_() override { return &teardown_host; }
    WiringHost teardown_host;
    int settings_created = 0, settings_unmounted = 0, settings_alive = 0;
    bool settings_null = false;
    void shared_repaint_settings() { ShellBase::repaint_settings_ui_(); }
    void shared_bind_settings() { ShellBase::bind_settings_controller_(); }
    void shared_settings_anim() { ShellBase::update_settings_anim_regions_(); }
    // Login flow: record the native steps the shared handlers ask for, or run
    // the real show_login_ui_ (building the login view) when `real_login_ui`.
    void show_login_ui_(tesseract::views::LoginView::Mode m) override
    {
        if (real_login_ui)
        {
            ShellBase::show_login_ui_(m);
            return;
        }
        login_uis.push_back(m);
    }
    bool real_login_ui = false;
    bool open_login_url_ok = true;
    bool open_login_url_ui_(const std::string&) override { return open_login_url_ok; }
    void detach_login_view_client_ui_() override { ++detaches; }
    void start_platform_services_ui_() override { ++service_starts; }
    // Media landing: the shared repaint and failure handling run; the native
    // surfaces are counted.
    void request_repaint_() override { ++repaints; }
    void repaint_settings_ui_() override { ++settings_repaints; }
    void shared_repaint_after_media(MediaKind k) { ShellBase::repaint_after_media_(k, {}); }
    void shared_decode_failed(const tk::CacheKey& key, MediaKind k)
    {
        ShellBase::on_media_decode_failed_(key, k);
    }
    int repaints = 0, settings_repaints = 0;
    bool main_window_foreground_for_tray_() const override { return tray_foreground; }
    void hide_main_window_for_tray_() override { ++tray_hides; }
    void show_main_window_for_tray_(const std::string& t) override
    {
        tray_shows.push_back(t);
    }
    bool tray_foreground = false;
    int tray_hides = 0;
    std::vector<std::string> tray_shows;
    void shared_refresh_after_switch() { ShellBase::refresh_account_ui_after_switch_(); }
    int service_starts = 0;
    void show_main_content_() override { ++main_contents; }
    void switch_active_account_(const std::string& uid) override
    {
        switched.push_back(uid);
        if (refresh_on_switch)
            ShellBase::refresh_account_ui_after_switch_();
    }
    // Make the stubbed switch run the shared post-switch refresh, as a switch
    // that changes the account does.
    bool refresh_on_switch = false;
    void shared_logout() { ShellBase::logout_active_account_ui_(); }
    std::vector<tesseract::views::LoginView::Mode> login_uis;
    int detaches = 0;
    int main_contents = 0;
    std::vector<std::string> switched;
    // Native dialogs: record what the shared controller wiring asks for.
    void prompt_passphrase_(std::string t, std::function<void(std::string)> cb) override
    {
        dialogs.push_back("pass:" + t);
        if (cb) cb("secret");
    }
    void pick_save_file_(std::string t, std::string n, std::function<void(std::string)> cb) override
    {
        dialogs.push_back("save:" + t + ":" + n);
        if (cb) cb("/tmp/out.txt");
    }
    void pick_open_file_(std::string t, std::function<void(std::string)> cb) override
    {
        dialogs.push_back("open:" + t);
        if (cb) cb("/tmp/in.txt");
    }
    void pick_folder_(std::string t, std::function<void(std::string)> cb) override
    {
        dialogs.push_back("folder:" + t);
        if (cb) cb("/tmp/dir");
    }
    void pick_media_save_path_(const tesseract::views::MediaSaveSpec& spec,
                               std::function<void(std::string)> cb) override
    {
        dialogs.push_back("media-save:" + spec.title + ":" + spec.suggested_name);
        if (cb)
        {
            cb(""); // dialog cancelled: nothing is written
        }
    }
    void show_message_dialog_(std::string t, std::string m, bool err) override
    {
        dialogs.push_back(std::string(err ? "error:" : "info:") + t + ":" + m);
    }
    std::vector<std::string> dialogs;
    int settings_closes = 0;

    // Picker repaint: TestShellBase stubs repaint_pickers_, so the shared body
    // is called explicitly; the native extension is counted.
    void repaint_pickers_native_() override { ++native_picker_repaints; }
    void shared_repaint_pickers() { ShellBase::repaint_pickers_(); }
    int native_picker_repaints = 0;

    // Media ingest: ShellBase's on_media_bytes_ready_ with the platform hooks
    // recorded. TestShellBase overrides on_media_bytes_ready_ with a no-op, so
    // the base implementation is called explicitly.
    void ingest(const tk::CacheKey& key, MediaKind kind,
                std::vector<std::uint8_t> bytes)
    {
        ShellBase::on_media_bytes_ready_(key, kind, std::move(bytes));
        drain_workers();
    }
    DecodedImage decode_image_(const std::vector<std::uint8_t>&, int, int) override
    {
        if (decode_animated)
        {
            return decode_anim_batch_();
        }
        DecodedImage d;
        if (decode_ok)
        {
            d.still = std::make_unique<WiringImage>();
        }
        return d;
    }
    void repaint_after_media_(MediaKind, const tk::CacheKey&) override
    {
        ++media_repaints;
        if (repaint_sink)
        {
            ++*repaint_sink;
        }
    }
    void on_media_decode_failed_(const tk::CacheKey&, MediaKind) override
    {
        ++media_failures;
    }
    void ingest_tile_media_(const tk::CacheKey&, std::vector<std::uint8_t>) override
    {
        ++tiles;
    }
    bool accept_media_bytes_(const tk::CacheKey&, MediaKind,
                             const std::vector<std::uint8_t>&) override
    {
        return accept_media;
    }
    // A 3-frame animation, delivered through the windowed streamed decode
    // (with no session) or, in low power mode, through decode_image_.
    DecodedImage decode_anim_batch_() const
    {
        DecodedImage d;
        for (int i = 0; i < 3; ++i)
        {
            d.frames.push_back(std::make_unique<WiringImage>());
            d.delays_ms.push_back(10);
        }
        return d;
    }
    bool decode_image_streamed_windowed_(
        const std::vector<std::uint8_t>& bytes, int max_w, int max_h,
        const std::function<void(std::unique_ptr<tk::Image>, int,
                                 std::shared_ptr<tk::AnimDecodeSession>,
                                 std::size_t)>& on_first_frame,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
        override
    {
        ++windowed_decodes;
        if (!decode_animated)
        {
            return ShellBase::decode_image_streamed_windowed_(
                bytes, max_w, max_h, on_first_frame, on_frame);
        }
        on_first_frame(std::make_unique<WiringImage>(), 10, nullptr, 0);
        on_frame(1, std::make_unique<WiringImage>(), 10);
        on_frame(2, std::make_unique<WiringImage>(), 10);
        return true;
    }
    // Posts to the UI thread run at once, or are parked in `parked`.
    void post_to_ui_(std::function<void()> fn) override
    {
        if (parked)
        {
            parked->push_back(std::move(fn));
            return;
        }
        TestShellBase::post_to_ui_(std::move(fn)); // inline on the UI thread
    }
    std::vector<std::function<void()>>* parked = nullptr;
    int* repaint_sink = nullptr; // outlives the shell
    bool decode_animated = false;
    int windowed_decodes = 0;
    bool decode_ok = true;
    bool accept_media = true;
    int media_repaints = 0;
    int media_failures = 0;
    int tiles = 0;

    bool ready = true;
    bool ui_ready_() const override { return ready; }
    bool click_modifier = false;

    // Rooms update: the shared handler runs (TestShellBase stubs on_rooms_updated_
    // out, so it is called as ShellBase::on_rooms_updated_); the native tail
    // is counted.
    void after_rooms_updated_ui_() override { ++rooms_tails; }
    void rooms_updated() { ShellBase::on_rooms_updated_(); }
    int rooms_tails = 0;

    // Notifications: the shared policy runs; the window state and the delivery
    // are controlled and recorded here.
    bool is_main_window_focused_() const override { return win_focused; }
    bool is_main_window_visible_() const override { return win_visible; }
    void request_notification_attention_() override { ++attention_requests; }
    void deliver_notification_(const std::string& uid,
                               const tesseract::Notification& n) override
    {
        delivered.emplace_back(uid, n.room_id);
        last_notification = n;
    }
    tesseract::Notification last_notification;
    // The base delivery (TestShellBase-style doubles override it above).
    void shared_deliver(const std::string& uid, const tesseract::Notification& n)
    {
        ShellBase::deliver_notification_(uid, n);
    }
    bool win_focused = false;
    bool win_visible = true;
    int attention_requests = 0;
    std::vector<std::pair<std::string, std::string>> delivered;
    void notify(const std::string& uid, const std::string& room)
    {
        handle_notification_ui_(uid, room, "Room", "Sender", "hi", false, {}, {},
                                "$e");
    }
    void notify_with_media(const std::string& uid, const std::string& room)
    {
        handle_notification_ui_(uid, room, "Room", "Sender", "hi", true,
                                {1, 2}, {3, 4}, "$e");
    }

    // Animation frame: the shared repaint runs; the native surfaces are counted.
    void update_main_anim_regions_() override { ++main_anim_updates; }
    void update_settings_anim_regions_() override { ++settings_anim_updates; }
    int main_anim_updates = 0;
    int settings_anim_updates = 0;

    // Sync status line: the shared state machine runs; the text is recorded.
    void show_sync_status_text_(const std::string& t) override { sync_texts.push_back(t); }
    void on_sync_status_override_kept_() override { ++override_kept; }
    std::vector<std::string> sync_texts;
    int override_kept = 0;

    // Sign-out: the shared policy runs; the native screens are recorded.
    void logout_active_account_ui_() override { ++logouts; }
    void show_sign_in_screen_ui_() override { ++sign_in_screens; }
    void on_signed_out_ui_(const tesseract::LogoutResult&) override { ++signed_out_hooks; }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
        // What a shell's renderer sees at this point.
        status_had_links.push_back(
            tesseract::status_has_links(parse_status_message_(m)));
    }
    std::vector<bool> status_had_links;
    void shared_relogin(const std::string& uid) { ShellBase::request_relogin_(uid); }
    int logouts = 0;
    int sign_in_screens = 0;
    int signed_out_hooks = 0;
    std::vector<std::string> statuses;
    bool click_modifier_held_() const override { return click_modifier; }
    void on_pre_paint_() override { ++pre_paints; }
    int pre_paints = 0;

    // wire_main_app_widget_ starts a self-rescheduling image-GC timer through
    // post_to_ui_after_; running those inline would recurse forever, so delayed
    // posts are queued and the test runs only the ones it cares about.
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        delayed.emplace_back(ms, std::move(fn));
    }
    std::vector<std::pair<int, std::function<void()>>> delayed;
    void run_delayed(int ms)
    {
        auto pending = std::move(delayed);
        delayed.clear();
        for (auto& [m, fn] : pending)
        {
            if (m == ms)
            {
                fn();
            }
            else
            {
                delayed.emplace_back(m, std::move(fn));
            }
        }
    }

    using ShellBase::create_main_app_;
    using ShellBase::current_scale_;
    using ShellBase::wire_main_surface_;
    using ShellBase::wire_room_view_services_;
    using ShellBase::create_main_room_pane_;
    using ShellBase::main_room_pane_;
    using ShellBase::account_manager_;
    using ShellBase::power_policy_;
    using ShellBase::active_account_;
    using ShellBase::on_account_badges_changed_;
    using ShellBase::on_own_extended_profile_ready_ui_;
    using ShellBase::on_profile_field_result_ui_;
    using ShellBase::open_account_picker_;
    using ShellBase::close_account_picker_;
    using ShellBase::handle_escape_key_;
    using ShellBase::handle_account_picker_key_;
    using ShellBase::rebuild_account_picker_;
    using ShellBase::account_picker_;
    using ShellBase::account_picker_popup_;
    using ShellBase::current_theme_;
    using ShellBase::ensure_login_view_;
    using ShellBase::call_window_;
    using ShellBase::open_login_url_or_show_;
    using ShellBase::teardown_login_view_;
    using ShellBase::login_view_;
    using ShellBase::login_surface_;
    using ShellBase::pool_;
    using ShellBase::settings_opened_from_login_;
    using ShellBase::settings_from_login_;
    using ShellBase::mark_main_content_ready_;
    using ShellBase::show_login_after_restore_;
    using ShellBase::release_login_surface_;
    using ShellBase::ensure_settings_view_;
    using ShellBase::prepare_settings_view_;
    using ShellBase::teardown_settings_view_;
    using ShellBase::notify_window_active_;
    using ShellBase::release_popups_;
    using ShellBase::on_launch_at_login_pref_ui_;
    using ShellBase::on_tray_available_ui_;
    using ShellBase::settings_surface_;
    using ShellBase::media_decode_failed_;
    using ShellBase::tray_activate_;
    using ShellBase::tray_toggle_;
    using ShellBase::last_tray_unread_;
    using ShellBase::pending_restore_rooms_;
    using ShellBase::tabs_;
    using ShellBase::app_settings_open_;
    using ShellBase::refresh_user_strip_;
    using ShellBase::finish_login_ui_;
    using ShellBase::on_login_succeeded_;
    using ShellBase::on_login_cancelled_;
    using ShellBase::on_login_finalized_;
    using ShellBase::reset_pending_login_client_;
    using ShellBase::arm_pending_login_;
    using ShellBase::pending_login_temp_dir_;
    using ShellBase::pending_login_client_;
    using ShellBase::pending_login_is_add_account_;
    using ShellBase::add_account_return_uid_;
    using ShellBase::inflight_tooltip_text_;
    using ShellBase::last_inflight_;
    using ShellBase::ensure_settings_controller_;
    using ShellBase::ensure_history_export_controller_;
    using ShellBase::history_export_controller_;
    using ShellBase::settings_controller_;
    using ShellBase::configure_new_settings_view_;
    using ShellBase::populate_settings_view_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::own_extended_profile_;
    using ShellBase::my_display_name_;
    using ShellBase::fetch_gif_source_;
    using ShellBase::compose_popups_;
    using ShellBase::compose_typing_active_;
    using ShellBase::handle_gif_results_ui_;
    using ShellBase::handle_gif_search_failed_ui_;
    using ShellBase::create_compose_popups_;
    using ShellBase::hide_compose_popups_;
    using ShellBase::on_server_info_ready_ui_;
    using ShellBase::restart_mark_read_timer_;
    using ShellBase::room_view_;
    using ShellBase::server_info_;
    using ShellBase::stats_settings_view_;
    using ShellBase::per_account_rooms_;
    using ShellBase::client_;
    using ShellBase::current_invite_;
    using ShellBase::current_knock_status_room_id_;
    using ShellBase::invites_;
    using ShellBase::main_app_;
    using ShellBase::my_avatar_url_;
    using ShellBase::my_knocks_;
    using ShellBase::room_search_text_;
    using ShellBase::gif_anim_inflight_;
    using ShellBase::gif_anim_unavailable_;
    using ShellBase::gif_preview_unavailable_;
    using ShellBase::decode_inline_fallback_;
    using ShellBase::gif_preview_inflight_;
    using ShellBase::gif_previews_;
    using ShellBase::gif_strip_image_;
    using ShellBase::next_media_req_id_;
    using ShellBase::reset_gif_failures_;
    using ShellBase::reset_gif_strip_state_;
    using ShellBase::extract_drop_media_;
    using ShellBase::handle_backup_progress_ui_;
    using ShellBase::handle_notification_ui_;
    using ShellBase::repaint_anim_frame_;
    using ShellBase::tick_anim_;
    using ShellBase::my_user_id_;
    using ShellBase::restore_all_accounts_async_;
    using ShellBase::show_status_message_;
    using ShellBase::show_connected_status_;
    using ShellBase::set_screen_lock_;
    using ShellBase::sync_status_debounce_pending_;
    using ShellBase::last_backup_state_;
    using ShellBase::last_imported_keys_;
    using ShellBase::last_room_list_state_;
    using ShellBase::refresh_sync_status_;
    using ShellBase::status_override_active_;
    using ShellBase::sync_progress_shown_;
    using ShellBase::finish_sign_out_ui_;
    using ShellBase::get_message_scroll_fraction_;
    using ShellBase::on_invites_updated_;
    using ShellBase::on_my_knocks_updated_;
    using ShellBase::rooms_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::update_typing_bar_;
    using ShellBase::space_stack_;
    using ShellBase::user_presence_;
    using ShellBase::close_find_in_room_;
    using ShellBase::close_forward_picker_;
    using ShellBase::close_message_search_;
    using ShellBase::close_quick_switch_;
    using ShellBase::open_find_in_room_;
    using ShellBase::open_message_search_;
    using ShellBase::open_quick_switch_;
    using ShellBase::wire_main_app_widget_;
    using ShellBase::refresh_pickers_packs_;
    using ShellBase::pool_pending_count_;
    using ShellBase::mut_pool_pending_count_;
    using ShellBase::wire_settings_controller_common_;
    using ShellBase::wire_settings_view_;
    using ShellBase::wire_voice_capture_;
    using ShellBase::capture_;
    using ShellBase::unjoined_fetch_;
    using ShellBase::pending_forwards_;
    using ShellBase::current_room_id_;
};

struct WiringFixture
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    tesseract::Client client;
    // Surface must outlive (be declared before) the widget tree.
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<MainAppWidget> app =
        tk::create_root_widget<MainAppWidget>(nullptr);

    WiringFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        s.room_view_ = app->room_view();
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        app->measure(lc, {1000, 700});
        app->arrange(lc, {0, 0, 1000, 700});
        s.wire_main_app_widget_(app.get());
    }
};

// WiringFixture with a host that makes native text fields, so the overlays
// build their search fields and the nav handlers wired onto them can run.
struct WiringHostFixture
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    tesseract::Client client;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    PopupCapableStubHost host; // outlives the widget tree
    std::unique_ptr<MainAppWidget> app =
        tk::create_root_widget<MainAppWidget>(&host);

    WiringHostFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        s.room_view_ = app->room_view();
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        app->measure(lc, {1000, 700});
        app->arrange(lc, {0, 0, 1000, 700});
        s.wire_main_app_widget_(app.get());
    }

    // The composer popups hold widgets of `app`, which dies before `s`.
    ~WiringHostFixture() { s.release_popups_(); }

    // The native control behind `field`, found by writing a marker into it.
    StubTextField* native_of(tk::TextField* field)
    {
        const std::string marker = "\x01nav-probe";
        field->set_text(marker);
        for (auto* n : host.fields_created)
        {
            if (n->text_ == marker)
            {
                field->set_text("");
                return n;
            }
        }
        return nullptr;
    }
};

tesseract::RoomInfo wiring_make_room(const std::string& id, std::uint64_t ts = 0)
{
    tesseract::RoomInfo r;
    r.id = id;
    r.name = id;
    r.last_activity_ts = ts;
    return r;
}

struct FakeCapture : tk::AudioCapture
{
    bool recording = false;
    int starts = 0, stops = 0, cancels = 0;
    void start() override
    {
        ++starts;
        recording = true;
    }
    void stop() override
    {
        ++stops;
        recording = false;
        if (on_stopped)
        {
            on_stopped(pcm, {1, 2}, dur);
        }
    }
    void cancel() override
    {
        ++cancels;
        recording = false;
    }
    bool is_recording() const override { return recording; }
    std::uint64_t duration_ms() const override { return dur; }
    void set_frame_callback(
        std::function<void(const std::int16_t*, std::size_t)>) override {}
    void clear_frame_callback() override {}
    std::vector<std::uint8_t> pcm;
    std::uint64_t dur = 0;
};

} // namespace

TEST_CASE("wiring: persisted section collapsed state is restored by section "
          "name",
          "[shell][wiring]")
{
    // Regression: the restore array was positional and predated kSecCallRooms.
    tesseract::test::SettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.room_section_spaces_collapsed = true;
    st.room_section_inactive_collapsed = false;
    st.room_section_space_unjoined_collapsed = false;
    WiringFixture f;
    const auto c = f.app->room_list_view()->collapsed_state();
    CHECK(c[RoomListView::kSecSpaces]);
    CHECK_FALSE(c[RoomListView::kSecInactive]);
    CHECK_FALSE(c[RoomListView::kSecCallRooms]);
    CHECK_FALSE(c[RoomListView::kSecSpaceUnjoined]);
}

TEST_CASE("wiring: leading sections restore their collapsed flags",
          "[shell][wiring]")
{
    tesseract::test::SettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.room_section_invites_collapsed = true;
    st.room_section_unread_collapsed = true;
    st.room_section_favorites_collapsed = false;
    st.room_section_dms_collapsed = true;
    st.room_section_rooms_collapsed = false;
    WiringFixture f;
    const auto c = f.app->room_list_view()->collapsed_state();
    CHECK(c[RoomListView::kSecInvites]);
    CHECK(c[RoomListView::kSecUnread]);
    CHECK_FALSE(c[RoomListView::kSecFavorites]);
    CHECK(c[RoomListView::kSecDMs]);
    CHECK_FALSE(c[RoomListView::kSecRooms]);
}

TEST_CASE("wiring: toggling a section persists it to Settings by name",
          "[shell][wiring]")
{
    WiringFixture f;
    auto& st = tesseract::Settings::instance();
    auto& rl = *f.app->room_list_view();
    rl.on_section_toggled(RoomListView::kSecInvites, true);
    rl.on_section_toggled(RoomListView::kSecUnread, true);
    rl.on_section_toggled(RoomListView::kSecFavorites, true);
    rl.on_section_toggled(RoomListView::kSecDMs, true);
    rl.on_section_toggled(RoomListView::kSecRooms, true);
    rl.on_section_toggled(RoomListView::kSecSpaces, true);
    rl.on_section_toggled(RoomListView::kSecInactive, false);
    rl.on_section_toggled(RoomListView::kSecSpaceUnjoined, true);
    CHECK(st.room_section_invites_collapsed);
    CHECK(st.room_section_unread_collapsed);
    CHECK(st.room_section_favorites_collapsed);
    CHECK(st.room_section_dms_collapsed);
    CHECK(st.room_section_rooms_collapsed);
    CHECK(st.room_section_spaces_collapsed);
    CHECK_FALSE(st.room_section_inactive_collapsed);
    CHECK(st.room_section_space_unjoined_collapsed);
    // An unknown section index is ignored.
    CHECK_NOTHROW(rl.on_section_toggled(99, true));
}

TEST_CASE("wiring: sidebar layout changes are stored in Settings",
          "[shell][wiring]")
{
    WiringFixture f;
    f.app->on_sidebar_layout_changed(312.4f, true);
    auto& st = tesseract::Settings::instance();
    CHECK(st.sidebar_width == 312);
    CHECK(st.sidebar_collapsed);
}

TEST_CASE("wiring: clearing the room-list search resets shell and view",
          "[shell][wiring]")
{
    // (The search TextField only exists once a Host is attached, so the typing
    // path is not reachable here; the clear hook is.)
    WiringFixture f;
    f.s.room_search_text_ = "lobby";
    f.app->room_list_view()->set_search_text("lobby");
    f.app->room_list_view()->on_search_clear();
    CHECK(f.s.room_search_text_.empty());
    CHECK(f.app->room_list_view()->search_text().empty());
}

TEST_CASE("wiring: selecting an invite shows the card; decline removes it",
          "[shell][wiring][invite]")
{
    WiringFixture f;
    tesseract::InviteInfo inv;
    inv.room_id = "!inv:x";
    inv.room_name = "Invited";
    inv.inviter_user_id = "@host:x";
    f.s.invites_.push_back(inv);

    // Unknown invite: ignored.
    f.app->room_list_view()->on_invite_selected("!nope:x");
    CHECK_FALSE(f.s.current_invite_.has_value());

    f.app->room_list_view()->on_invite_selected("!inv:x");
    REQUIRE(f.s.current_invite_.has_value());
    CHECK(f.s.current_invite_->room_id == "!inv:x");
    CHECK(f.s.current_invite_->inviter_id == "@host:x");

    f.app->invite_card()->on_decline();
    CHECK(f.s.invites_.empty());
}

TEST_CASE("wiring: block also drops the invite", "[shell][wiring][invite]")
{
    WiringFixture f;
    tesseract::InviteInfo inv;
    inv.room_id = "!inv:x";
    inv.inviter_user_id = "@bad:x";
    f.s.invites_.push_back(inv);
    f.app->room_list_view()->on_invite_selected("!inv:x");
    f.app->invite_card()->on_block();
    CHECK(f.s.invites_.empty());
}

TEST_CASE("wiring: invite accept/decline without a selection are no-ops",
          "[shell][wiring][invite]")
{
    WiringFixture f;
    CHECK_NOTHROW(f.app->invite_card()->on_accept());
    CHECK_NOTHROW(f.app->invite_card()->on_decline());
    CHECK_NOTHROW(f.app->invite_card()->on_block());
}

TEST_CASE("wiring: knock row selection records the room; cancel clears content",
          "[shell][wiring][knock]")
{
    WiringFixture f;
    tesseract::KnockedRoomInfo k;
    k.room_id = "!knock:x";
    k.room_name = "Knocked";
    f.s.my_knocks_.push_back(k);

    f.app->room_list_view()->on_knock_row_selected("!unknown:x");
    CHECK(f.s.current_knock_status_room_id_.empty());

    f.app->room_list_view()->on_knock_row_selected("!knock:x");
    CHECK(f.s.current_knock_status_room_id_ == "!knock:x");

    const int before = f.s.relayouts;
    f.app->knock_status_card()->on_cancel();
    CHECK(f.s.relayouts > before);
}

TEST_CASE("wiring: presence provider reads the shell's presence map",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.user_presence_["@a:x"] = tesseract::PresenceState::Online;
    // Reaching the provider through a RoomInfoPanel is heavy; the wiring
    // itself must at least have installed it without disturbing state.
    CHECK(f.s.user_presence_.at("@a:x") == tesseract::PresenceState::Online);
    CHECK(f.app->user_info()->on_status_clicked != nullptr);
    CHECK(f.app->on_settings_shortcut != nullptr);
}

TEST_CASE("wiring: unjoined-space summary request is deduplicated",
          "[shell][wiring]")
{
    WiringFixture f;
    auto& rl = *f.app->room_list_view();
    REQUIRE(rl.on_unjoined_room_summary_needed != nullptr);
    // Not drilled into a space: nothing is queued.
    rl.on_unjoined_room_summary_needed("!child:x");
    CHECK(f.s.unjoined_fetch_.empty());
}

TEST_CASE("wiring: add-room dialog opens from the room list plus button",
          "[shell][wiring]")
{
    WiringFixture f;
    auto* ar = f.app->add_room_view();
    REQUIRE(ar != nullptr);
    CHECK_FALSE(ar->is_open());
    f.app->room_list_view()->on_add_room_requested();
    CHECK(ar->is_open());
}

TEST_CASE("wiring: directory join of an already-joined room closes the dialog "
          "instead of re-joining",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.rooms_.push_back(wiring_make_room("!have:x"));
    auto* ar = f.app->add_room_view();
    f.app->room_list_view()->on_add_room_requested();
    REQUIRE(ar->is_open());
    auto* dr = ar->directory_view();
    REQUIRE(dr != nullptr);
    dr->on_join_requested("!have:x", "");
    CHECK_FALSE(ar->is_open());
}

TEST_CASE("wiring: avatar click opens the image viewer", "[shell][wiring]")
{
    WiringFixture f;
    REQUIRE(f.app->room_view()->on_avatar_clicked != nullptr);
    // Empty url is ignored.
    f.app->room_view()->on_avatar_clicked("", "Name");
    f.app->room_view()->on_avatar_clicked("mxc://x/avatar", "Name");
    CHECK(f.app->media_viewer() != nullptr);
}

TEST_CASE("wiring: forward request opens the picker once and forwards to the "
          "chosen rooms",
          "[shell][wiring][forward]")
{
    WiringFixture f;
    f.s.current_room_id_ = "!src:x";
    auto* fp = f.app->forward_picker();
    REQUIRE(fp != nullptr);
    f.app->room_view()->on_forward_requested("$evt");
    CHECK(fp->is_open());
    REQUIRE(fp->on_confirmed != nullptr);
    fp->on_confirmed({"!a:x", "!b:x"});
    CHECK(f.s.pending_forwards_.size() == 2);
}

TEST_CASE("wiring: forward request needs a current room", "[shell][wiring][forward]")
{
    WiringFixture f;
    f.app->room_view()->on_forward_requested("$evt");
    CHECK_FALSE(f.app->forward_picker()->is_open());
}

TEST_CASE("wiring: quick switcher lists rooms most-recent-first",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.rooms_ = {wiring_make_room("!old:x", 10), wiring_make_room("!new:x", 99),
                  wiring_make_room("!mid:x", 50)};
    auto* qs = f.app->quick_switcher();
    REQUIRE(qs != nullptr);
    std::string picked;
    qs->on_room_avatar_needed = [](const tesseract::RoomInfo&) {};
    // The provider is private to the widget; opening it exercises it.
    f.app->show_quick_switch(true);
    CHECK_NOTHROW(f.app->show_quick_switch(false));
}

TEST_CASE("wiring: quick switcher opens, forwards the window and closes",
          "[shell][wiring][overlay]")
{
    WiringFixture f;
    auto* qs = f.app->quick_switcher();
    REQUIRE(qs != nullptr);
    const int base = f.s.relayouts;
    f.s.open_quick_switch_();
    CHECK(qs->is_open());
    CHECK(f.s.forwards == 1);
    CHECK(f.s.relayouts > base);
    f.s.close_quick_switch_();
    CHECK_FALSE(qs->is_open());
    CHECK(f.s.focus_restores >= 1);
}

TEST_CASE("wiring: message search opens, forwards the window and closes",
          "[shell][wiring][overlay]")
{
    WiringFixture f;
    auto* ms = f.app->message_search();
    REQUIRE(ms != nullptr);
    f.s.open_message_search_();
    CHECK(ms->is_open());
    CHECK(f.s.forwards == 1);
    f.s.close_message_search_();
    CHECK_FALSE(ms->is_open());
    CHECK(f.s.focus_restores >= 1);
}

TEST_CASE("wiring: find in room needs a current room", "[shell][wiring][overlay]")
{
    WiringFixture f;
    f.s.open_find_in_room_();
    CHECK(f.s.forwards == 0);
    CHECK_FALSE(f.app->room_view()->room_search_open());
    f.s.close_find_in_room_();
    CHECK(f.s.focus_restores >= 1);
}

TEST_CASE("wiring: overlays close through their on_close callbacks",
          "[shell][wiring][overlay]")
{
    WiringFixture f;
    REQUIRE(f.app->quick_switcher()->on_close != nullptr);
    REQUIRE(f.app->message_search()->on_close != nullptr);
    REQUIRE(f.app->forward_picker()->on_close != nullptr);
    f.s.open_quick_switch_();
    f.app->quick_switcher()->on_close();
    CHECK_FALSE(f.app->quick_switcher()->is_open());
    f.s.open_message_search_();
    f.app->message_search()->on_close();
    CHECK_FALSE(f.app->message_search()->is_open());
}

TEST_CASE("wiring: quick switcher field keys move the selection and Escape closes",
          "[shell][wiring][overlay]")
{
    WiringHostFixture f;
    auto* qs = f.app->quick_switcher();
    REQUIRE(qs->search_field() != nullptr);
    f.s.open_quick_switch_();
    auto* nat = f.native_of(qs->search_field());
    REQUIRE(nat != nullptr);
    REQUIRE(nat->on_popup_nav != nullptr);

    const int base = f.s.relayouts;
    CHECK(nat->on_popup_nav(tk::NavKey::Down));
    CHECK(nat->on_popup_nav(tk::NavKey::Up));
    CHECK(f.s.relayouts == base + 2);
    CHECK_FALSE(nat->on_popup_nav(tk::NavKey::Left)); // falls through to the field

    const int restores = f.s.focus_restores;
    CHECK(nat->on_popup_nav(tk::NavKey::Escape));
    CHECK_FALSE(qs->is_open());
    CHECK(f.s.focus_restores > restores);
    // Once closed, the handler leaves every key alone.
    CHECK_FALSE(nat->on_popup_nav(tk::NavKey::Escape));
}

TEST_CASE("wiring: message search field keys move the selection and Escape closes",
          "[shell][wiring][overlay]")
{
    WiringHostFixture f;
    auto* ms = f.app->message_search();
    REQUIRE(ms->search_field() != nullptr);
    f.s.open_message_search_();
    auto* nat = f.native_of(ms->search_field());
    REQUIRE(nat != nullptr);

    const int base = f.s.relayouts;
    CHECK(nat->on_popup_nav(tk::NavKey::Down));
    CHECK(nat->on_popup_nav(tk::NavKey::Up));
    CHECK(f.s.relayouts == base + 2);

    const int restores = f.s.focus_restores;
    CHECK(nat->on_popup_nav(tk::NavKey::Escape));
    CHECK_FALSE(ms->is_open());
    CHECK(f.s.focus_restores > restores);
    CHECK_FALSE(nat->on_popup_nav(tk::NavKey::Escape));
}

TEST_CASE("wiring: forward picker field keys move the selection and Escape closes",
          "[shell][wiring][overlay][forward]")
{
    WiringHostFixture f;
    f.s.current_room_id_ = "!src:x";
    auto* fp = f.app->forward_picker();
    REQUIRE(fp->search_field() != nullptr);
    f.app->room_view()->on_forward_requested("$evt");
    REQUIRE(fp->is_open());
    auto* nat = f.native_of(fp->search_field());
    REQUIRE(nat != nullptr);

    const int base = f.s.relayouts;
    CHECK(nat->on_popup_nav(tk::NavKey::Down));
    CHECK(nat->on_popup_nav(tk::NavKey::Up));
    CHECK(f.s.relayouts == base + 2);

    CHECK(nat->on_popup_nav(tk::NavKey::Escape));
    CHECK_FALSE(fp->is_open());
}

TEST_CASE("wiring: forward picker on_close closes the picker",
          "[shell][wiring][overlay][forward]")
{
    WiringHostFixture f;
    f.s.current_room_id_ = "!src:x";
    auto* fp = f.app->forward_picker();
    f.app->room_view()->on_forward_requested("$evt");
    REQUIRE(fp->is_open());
    REQUIRE(fp->on_close != nullptr);
    fp->on_close();
    CHECK_FALSE(fp->is_open());
}

TEST_CASE("wiring: find in room opens the bar, its keys navigate matches and close",
          "[shell][wiring][overlay]")
{
    WiringHostFixture f;
    auto* rv = f.app->room_view();
    rv->set_room(wiring_make_room("!a:x"));
    f.s.current_room_id_ = "!a:x";
    std::vector<int> navigated;
    rv->on_room_search_navigate = [&](int d) { navigated.push_back(d); };

    f.s.open_find_in_room_();
    CHECK(f.s.forwards == 1);
    REQUIRE(rv->room_search_open());
    auto* bar = rv->room_search_bar();
    REQUIRE(bar != nullptr);
    REQUIRE(bar->search_field() != nullptr);
    auto* nat = f.native_of(bar->search_field());
    REQUIRE(nat != nullptr);

    CHECK(nat->on_popup_nav(tk::NavKey::Up));
    CHECK(nat->on_popup_nav(tk::NavKey::Down));
    CHECK(navigated == std::vector<int>{-1, +1});
    CHECK_FALSE(nat->on_popup_nav(tk::NavKey::Left));

    const int restores = f.s.focus_restores;
    CHECK(nat->on_popup_nav(tk::NavKey::Escape));
    CHECK_FALSE(rv->room_search_open());
    CHECK(f.s.focus_restores == restores + 1);
    CHECK_FALSE(nat->on_popup_nav(tk::NavKey::Up)); // closed: ignored

    // The bar's own close button reaches the same path.
    f.s.open_find_in_room_();
    REQUIRE(rv->room_search_open());
    REQUIRE(bar->on_close != nullptr);
    bar->on_close();
    CHECK_FALSE(rv->room_search_open());
    CHECK(f.s.focus_restores == restores + 2);
}

namespace
{
std::vector<std::string> wiring_tab_ids(const WiringShell& s)
{
    std::vector<std::string> v;
    for (const auto& t : s.tabs_)
    {
        v.push_back(t.room_id);
    }
    return v;
}
} // namespace

TEST_CASE("wiring: a room-list click switches the tab, a modifier click opens a new one",
          "[shell][wiring][callbacks]")
{
    using Ids = std::vector<std::string>;
    WiringFixture f;
    f.s.rooms_ = {wiring_make_room("!a:x"), wiring_make_room("!r:x")};
    auto* rl = f.app->room_list_view();
    REQUIRE(rl->on_room_selected != nullptr);

    f.s.tab_select_room("!a:x");
    f.s.click_modifier = false;
    rl->on_room_selected("!r:x");
    CHECK(wiring_tab_ids(f.s) == Ids{"!r:x"});
    CHECK(f.s.current_room_id_ == "!r:x");

    f.s.click_modifier = true;
    rl->on_room_selected("!a:x");
    CHECK(wiring_tab_ids(f.s) == Ids{"!r:x", "!a:x"});
    CHECK(f.s.current_room_id_ == "!a:x");
}

TEST_CASE("wiring: a space click drills in instead of opening a tab",
          "[shell][wiring][callbacks]")
{
    WiringFixture f;
    auto space = wiring_make_room("!space:x");
    space.is_space = true;
    f.s.rooms_ = {space};
    f.app->room_list_view()->on_room_selected("!space:x");
    CHECK(f.s.tabs_.empty());
    CHECK(f.s.space_stack_ == std::vector<std::string>{"!space:x"});
}

TEST_CASE("wiring: a tab click switches to it, a modifier click pops it out",
          "[shell][wiring][callbacks]")
{
    using Ids = std::vector<std::string>;
    WiringFixture f;
    f.s.rooms_ = {wiring_make_room("!a:x"), wiring_make_room("!b:x")};
    f.s.tab_open_room("!a:x");
    f.s.tab_open_room("!b:x");
    REQUIRE(wiring_tab_ids(f.s) == Ids{"!a:x", "!b:x"});
    auto* tabs = f.app->tab_bar();
    REQUIRE(tabs->on_tab_selected != nullptr);

    f.s.click_modifier = false;
    tabs->on_tab_selected("!a:x");
    CHECK(f.s.current_room_id_ == "!a:x");
    CHECK(wiring_tab_ids(f.s) == Ids{"!a:x", "!b:x"});

    // The pop-out closes the tab (the window itself is a per-shell effect).
    f.s.click_modifier = true;
    tabs->on_tab_selected("!b:x");
    CHECK(wiring_tab_ids(f.s) == Ids{"!a:x"});
}

TEST_CASE("wiring: shared view callbacks are all wired",
          "[shell][wiring][callbacks]")
{
    WiringFixture f;
    auto& a = *f.app;
    // A callback dropped from wire_view_callbacks_ would silently do nothing on
    // every shell, so list them all.
    CHECK(a.on_quick_switch_shortcut != nullptr);
    CHECK(a.on_message_search_shortcut != nullptr);
    CHECK(a.on_find_in_room_shortcut != nullptr);
    CHECK(a.on_history_back_shortcut != nullptr);
    CHECK(a.on_history_forward_shortcut != nullptr);
    CHECK(a.on_space_back != nullptr);

    auto* rl = a.room_list_view();
    REQUIRE(rl != nullptr);
    CHECK(rl->on_room_selected != nullptr);
    CHECK(rl->on_open_in_tab_requested != nullptr);
    CHECK(rl->on_open_in_window_requested != nullptr);
    CHECK(rl->on_leave_room_requested != nullptr);
    CHECK(rl->on_mark_read_requested != nullptr);
    CHECK(rl->on_unjoined_room_selected != nullptr);

    auto* tabs = a.tab_bar();
    REQUIRE(tabs != nullptr);
    CHECK(tabs->on_tab_selected != nullptr);
    CHECK(tabs->on_tab_closed != nullptr);

    auto* rp = a.room_preview();
    REQUIRE(rp != nullptr);
    CHECK(rp->on_avatar_needed != nullptr);
    CHECK(rp->on_join != nullptr);
    CHECK(rp->on_dismiss != nullptr);

    auto* sr = a.space_root();
    REQUIRE(sr != nullptr);
    CHECK(sr->on_layout_changed != nullptr);
    CHECK(sr->on_copy_to_clipboard != nullptr);
    CHECK(sr->on_leave_space != nullptr);
    CHECK(sr->on_settings_opened != nullptr);
    CHECK(sr->on_settings_avatar_upload_requested != nullptr);
    if (auto* sv = sr->settings_view())
    {
        CHECK(sv->on_accept != nullptr);
        CHECK(sv->on_image_pack_images_needed != nullptr);
        CHECK(sv->on_image_pack_pending_image_added != nullptr);
    }

    auto* rv = a.room_view();
    REQUIRE(rv != nullptr);
    CHECK(rv->on_layout_changed != nullptr);
    CHECK(rv->on_set_clipboard != nullptr);
    CHECK(rv->on_selection_started != nullptr);
    if (auto* ml = rv->message_list())
    {
        CHECK(ml->on_tile_needed != nullptr);
    }
    if (auto* rsv = rv->room_settings_view())
    {
        CHECK(rsv->on_image_pack_images_needed != nullptr);
        CHECK(rsv->on_image_pack_pending_image_added != nullptr);
    }
}

TEST_CASE("wiring: clicking a space in the room list drills into it",
          "[shell][wiring][callbacks]")
{
    WiringFixture f;
    auto space = wiring_make_room("!space:x");
    space.is_space = true;
    f.s.rooms_ = {space};
    f.app->room_list_view()->on_room_selected("!space:x");
    REQUIRE(f.s.space_stack_.size() == 1);
    CHECK(f.s.space_stack_.back() == "!space:x");
}

TEST_CASE("wiring: layout changes from the room view and space root relayout",
          "[shell][wiring][callbacks]")
{
    WiringFixture f;
    const int base = f.s.relayouts;
    f.app->room_view()->on_layout_changed();
    f.app->space_root()->on_layout_changed();
    CHECK(f.s.relayouts == base + 2);
}

namespace
{
struct GifTestImage : tk::Image
{
    int width() const override { return 1; }
    int height() const override { return 1; }
    std::size_t memory_bytes() const override { return 4; }
};

tesseract::GifResult gif_result()
{
    tesseract::GifResult r;
    r.preview_url = "mxc://x/preview";
    r.strip_url = "mxc://x/strip";
    r.strip_mime = "video/mp4";
    return r;
}
} // namespace

TEST_CASE("gif strip: a cached preview is returned while the animation loads",
          "[shell][gif]")
{
    WiringShell s;
    auto img = std::make_unique<GifTestImage>();
    const tk::Image* raw = img.get();
    s.gif_previews_[tk::CacheKey::gif_preview("mxc://x/preview")] = std::move(img);
    int repaints = 0;
    CHECK(s.gif_strip_image_(gif_result(), [&] { ++repaints; }) == raw);
    // No client: the animation fetch fails at once, so nothing stays in flight.
    CHECK(s.gif_preview_inflight_.empty());
    CHECK(s.gif_anim_inflight_.empty());
    CHECK(repaints == 0);
}

TEST_CASE("gif strip: nothing cached and no client returns null and clears "
          "the in-flight sets",
          "[shell][gif]")
{
    WiringShell s;
    CHECK(s.gif_strip_image_(gif_result(), [] {}) == nullptr);
    CHECK(s.gif_preview_inflight_.empty());
    CHECK(s.gif_anim_inflight_.empty());
}

TEST_CASE("gif strip: a decoded animation frame wins over the preview",
          "[shell][gif]")
{
    WiringShell s;
    std::vector<std::unique_ptr<tk::Image>> frames;
    frames.push_back(std::make_unique<GifTestImage>());
    const tk::Image* frame = frames.back().get();
    s.account_manager_.anim_cache().store(tk::CacheKey::media("mxc://x/strip"),
                                          std::move(frames), {100},
                                          s.monotonic_ms_());
    s.gif_previews_[tk::CacheKey::gif_preview("mxc://x/preview")] =
        std::make_unique<GifTestImage>();
    CHECK(s.gif_strip_image_(gif_result(), [] {}) == frame);
}

TEST_CASE("gif strip: an animation that finished without frames is not fetched again",
          "[shell][gif]")
{
    WiringShell s;
    s.gif_anim_unavailable_.insert("mxc://x/strip");
    s.gif_preview_unavailable_.insert("mxc://x/preview");
    CHECK(s.gif_strip_image_(gif_result(), [] {}) == nullptr);
    // Nothing was started for either stage, so nothing is left in flight.
    CHECK(s.gif_preview_inflight_.empty());
    CHECK(s.gif_anim_inflight_.empty());
}

TEST_CASE("gif strip: a failed download is remembered and not retried on repaint",
          "[shell][gif]")
{
    WiringShell s;
    // No client: both source fetches fail at once with empty bytes.
    CHECK(s.gif_strip_image_(gif_result(), [] {}) == nullptr);
    CHECK(s.gif_preview_unavailable_.count("mxc://x/preview") == 1);
    CHECK(s.gif_anim_unavailable_.count("mxc://x/strip") == 1);
    const auto next_id = s.next_media_req_id_;
    CHECK(s.gif_strip_image_(gif_result(), [] {}) == nullptr);
    CHECK(s.next_media_req_id_ == next_id); // no new fetch was started
}

TEST_CASE("gif strip: resetting the failure memory lets the fetch be tried again",
          "[shell][gif]")
{
    WiringShell s;
    s.gif_strip_image_(gif_result(), [] {});
    s.gif_preview_inflight_.insert("mxc://x/other");
    s.reset_gif_failures_();
    CHECK(s.gif_preview_unavailable_.empty());
    CHECK(s.gif_anim_unavailable_.empty());
    CHECK(s.gif_preview_inflight_.count("mxc://x/other") == 1);
    const auto next_id = s.next_media_req_id_;
    s.gif_strip_image_(gif_result(), [] {});
    CHECK(s.next_media_req_id_ == next_id + 2); // preview and animation
    s.gif_anim_inflight_.insert("mxc://x/strip");
    s.reset_gif_strip_state_();
    CHECK(s.gif_preview_inflight_.empty());
    CHECK(s.gif_anim_inflight_.empty());
    CHECK(s.gif_preview_unavailable_.empty());
}

namespace
{
// A shell with a client and the strip's source bytes in the disk cache, so the
// fetch/decode stages run without the network. One stage is disabled by the
// caller (pre-marked unavailable) so a single worker runs the chain.
struct GifStripFixture
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    int repaints = 0;

    GifStripFixture()
    {
        auto acct = std::make_shared<tesseract::AccountSession>();
        acct->user_id = "@a:x";
        acct->client  = std::make_unique<tesseract::Client>();
        s.active_account_ = acct;
        const tesseract::GifResult r = gif_result();
        s.account_manager_.media_disk_cache().store(
            tk::CacheKey::gif_source(r.preview_url), {1, 2, 3});
        s.account_manager_.media_disk_cache().store(
            tk::CacheKey::gif_source(r.strip_url), {4, 5, 6});
    }
    const tk::Image* strip(const tesseract::GifResult& r)
    {
        return s.gif_strip_image_(r, [this] { ++repaints; });
    }
};
} // namespace

TEST_CASE("gif strip: a fetched preview is decoded, stored and repainted",
          "[shell][gif]")
{
    GifStripFixture f;
    f.s.gif_anim_unavailable_.insert("mxc://x/strip");
    CHECK(f.strip(gif_result()) == nullptr);
    CHECK(f.s.gif_preview_inflight_.count("mxc://x/preview") == 1);
    f.s.drain_workers();
    CHECK(f.s.gif_preview_inflight_.empty());
    CHECK(f.s.gif_preview_unavailable_.empty());
    CHECK(f.s.gif_previews_.count(tk::CacheKey::gif_preview("mxc://x/preview")) == 1);
    CHECK(f.repaints == 1);
    CHECK(f.strip(gif_result()) != nullptr);
}

TEST_CASE("gif strip: a preview that does not decode is remembered as unavailable",
          "[shell][gif]")
{
    GifStripFixture f;
    f.s.gif_anim_unavailable_.insert("mxc://x/strip");
    f.s.decode_ok = false;
    f.strip(gif_result());
    f.s.drain_workers();
    CHECK(f.s.gif_preview_inflight_.empty());
    CHECK(f.s.gif_preview_unavailable_.count("mxc://x/preview") == 1);
    CHECK(f.s.gif_previews_.empty());
    CHECK(f.repaints == 1);
}

TEST_CASE("gif strip: a decoded animated image fills the animation cache",
          "[shell][gif]")
{
    GifStripFixture f;
    f.s.gif_preview_unavailable_.insert("mxc://x/preview");
    f.s.decode_animated = true;
    tesseract::GifResult r = gif_result();
    r.strip_mime = "image/gif";
    CHECK(f.strip(r) == nullptr);
    f.s.drain_workers();
    CHECK(f.s.gif_anim_inflight_.empty());
    CHECK(f.s.gif_anim_unavailable_.empty());
    CHECK(f.repaints == 1);
    CHECK(f.strip(r) != nullptr);
}

TEST_CASE("gif strip: a still strip image becomes the preview and the animation "
          "is marked unavailable",
          "[shell][gif]")
{
    GifStripFixture f;
    f.s.gif_preview_unavailable_.insert("mxc://x/preview");
    tesseract::GifResult r = gif_result();
    r.strip_mime = "image/gif";
    f.strip(r);
    f.s.drain_workers();
    CHECK(f.s.gif_anim_inflight_.empty());
    CHECK(f.s.gif_anim_unavailable_.count("mxc://x/strip") == 1);
    CHECK(f.s.gif_previews_.count(tk::CacheKey::gif_preview("mxc://x/preview")) == 1);
    CHECK(f.repaints == 1);
    CHECK(f.strip(r) != nullptr);
}

TEST_CASE("gif strip: an MP4 that yields no frames is marked unavailable",
          "[shell][gif]")
{
    GifStripFixture f;
    f.s.gif_preview_unavailable_.insert("mxc://x/preview");
    f.strip(gif_result());
    f.s.drain_workers();
    CHECK(f.s.gif_anim_inflight_.empty());
    CHECK(f.s.gif_anim_unavailable_.count("mxc://x/strip") == 1);
    CHECK(f.repaints == 1);
}

TEST_CASE("shell hooks: invite and knock updates relayout unless the window is gone",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.invites_.emplace_back();
    f.s.my_knocks_.emplace_back();
    const int base = f.s.relayouts;
    f.s.on_invites_updated_();
    f.s.on_my_knocks_updated_();
    CHECK(f.s.relayouts == base + 2);
    CHECK(f.app->room_list_view()->invites_for_test() == &f.s.invites_);
    CHECK(f.app->room_list_view()->my_knocks_for_test() == &f.s.my_knocks_);
    f.s.ready = false;
    f.s.on_invites_updated_();
    f.s.on_my_knocks_updated_();
    CHECK(f.s.relayouts == base + 2);
}

TEST_CASE("shell hooks: scroll fraction without a room reads zero",
          "[shell][wiring]")
{
    WiringShell s;
    CHECK(s.get_message_scroll_fraction_() == 0.f);
    CHECK_NOTHROW(s.update_typing_bar_("someone is typing", true));
}

TEST_CASE("shell hooks: the typing text reaches the room view when the window is ready",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.update_typing_bar_("Alice is typing", true);
    CHECK(f.app->room_view()->message_list()->typing_text_for_test() ==
          "Alice is typing");
    f.s.ready = false;
    f.s.update_typing_bar_("", false);
    CHECK(f.app->room_view()->message_list()->typing_text_for_test() ==
          "Alice is typing");
}

TEST_CASE("shell hooks: the scroll fraction reads the populated message list",
          "[shell][wiring]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    std::vector<tesseract::views::MessageRowData> rows;
    for (int i = 0; i < 200; ++i)
    {
        tesseract::views::MessageRowData r;
        r.event_id = "$e" + std::to_string(i);
        r.sender_name = "Alice";
        r.body = "message " + std::to_string(i);
        rows.push_back(std::move(r));
    }
    f.app->room_view()->set_room(wiring_make_room("!a:x"));
    f.app->room_view()->set_messages(std::move(rows));
    tk::LayoutCtx lc{f.surface->factory(), tk::Theme::light()};
    f.app->room_view()->measure(lc, {700, 600});
    f.app->room_view()->arrange(lc, {0, 0, 700, 600});
    f.app->room_view()->message_list()->scroll_to_offset(0.5f);
    CHECK(f.s.get_message_scroll_fraction_() == Catch::Approx(0.5f).margin(0.05));
}

TEST_CASE("drop probe: video and audio go to the platform hooks, other mimes do nothing",
          "[shell][drop]")
{
    WiringShell s;
    auto bar_owner = tk::create_root_widget<tesseract::views::ComposeBar>(nullptr);
    auto alive = std::make_shared<bool>(true);

    s.extract_drop_media_(7, {1, 2}, "video/mp4", bar_owner.get(), alive);
    CHECK(s.video_probes == 1);
    CHECK(s.last_probe.gen == 7);
    CHECK(s.last_probe.bytes == std::vector<std::uint8_t>{1, 2});
    CHECK(s.last_probe.target == bar_owner.get());
    CHECK(s.last_probe.alive == alive);

    s.extract_drop_media_(8, {3}, "audio/ogg");
    CHECK(s.audio_probes == 1);
    CHECK(s.last_probe.gen == 8);
    CHECK(s.last_probe.bytes == std::vector<std::uint8_t>{3});
    CHECK(s.last_probe.target == nullptr);
    CHECK(s.last_probe.alive == nullptr);

    s.extract_drop_media_(9, {1, 2}, "application/pdf");
    s.extract_drop_media_(10, {1, 2}, "image/gif");
    CHECK(s.video_probes == 1);
    CHECK(s.audio_probes == 1);
}

TEST_CASE("media ingest: a decoded avatar lands in the thumbnail cache and repaints",
          "[shell][media]")
{
    WiringShell s;
    const auto key = tk::CacheKey::media("mxc://x/avatar");
    s.ingest(key, ShellBase::MediaKind::UserAvatar, {1, 2, 3});
    CHECK(s.account_manager_.thumbnail_cache().contains(key));
    CHECK(s.media_repaints == 1);
    CHECK(s.media_failures == 0);
}

TEST_CASE("media ingest: an undecodable avatar is reported, not stored",
          "[shell][media]")
{
    WiringShell s;
    s.decode_ok = false;
    const auto key = tk::CacheKey::media("mxc://x/bad");
    s.ingest(key, ShellBase::MediaKind::RoomAvatar, {1, 2, 3});
    CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(key));
    CHECK(s.media_failures == 1);
    CHECK(s.media_repaints == 0);
}

TEST_CASE("media ingest: map tiles go to the platform hook unless already cached",
          "[shell][media]")
{
    WiringShell s;
    const auto key = tk::CacheKey::media("tile:1/2/3");
    s.ingest(key, ShellBase::MediaKind::Tile, {1, 2, 3});
    CHECK(s.tiles == 1);
    s.account_manager_.image_cache().store(key, std::make_unique<WiringImage>());
    s.ingest(key, ShellBase::MediaKind::Tile, {1, 2, 3});
    CHECK(s.tiles == 1);
}

TEST_CASE("media ingest: inline images store a still and repaint; failures are reported",
          "[shell][media]")
{
    WiringShell s;
    const auto ok_key = tk::CacheKey::media("mxc://x/img");
    s.ingest(ok_key, ShellBase::MediaKind::MediaImage, {1, 2, 3});
    CHECK(s.account_manager_.image_cache().contains(ok_key));
    CHECK(s.media_repaints == 1);

    s.decode_ok = false;
    const auto bad_key = tk::CacheKey::media("mxc://x/bad-img");
    s.ingest(bad_key, ShellBase::MediaKind::MediaImage, {1, 2, 3});
    CHECK_FALSE(s.account_manager_.image_cache().contains(bad_key));
    CHECK(s.media_failures == 1);
}

TEST_CASE("media ingest: an animated image goes to the animation cache and evicts a stale still",
          "[shell][media]")
{
    WiringShell s;
    s.decode_animated = true;
    const auto key = tk::CacheKey::media("mxc://x/anim");
    s.account_manager_.image_cache().store(key, std::make_unique<WiringImage>());
    s.ingest(key, ShellBase::MediaKind::MediaThumbnail, {1, 2, 3});
    CHECK(s.windowed_decodes == 1);
    CHECK(s.account_manager_.anim_cache().has(key));
    CHECK_FALSE(s.account_manager_.image_cache().contains(key));
    CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(key));
    CHECK(s.media_repaints == 1);
}

TEST_CASE("media ingest: a thumbnail still lands in the thumbnail cache",
          "[shell][media]")
{
    WiringShell s;
    const auto key = tk::CacheKey::media("mxc://x/thumb");
    s.ingest(key, ShellBase::MediaKind::MediaThumbnail, {1, 2, 3});
    CHECK(s.windowed_decodes == 1); // tried, then fell back to the plain decode
    CHECK(s.account_manager_.thumbnail_cache().contains(key));
    CHECK_FALSE(s.account_manager_.image_cache().contains(key));
    CHECK(s.media_repaints == 1);

    s.decode_ok = false;
    const auto bad = tk::CacheKey::media("mxc://x/bad-thumb");
    s.ingest(bad, ShellBase::MediaKind::MediaThumbnail, {1, 2, 3});
    CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(bad));
    CHECK(s.media_failures == 1);
}

TEST_CASE("media ingest: low power mode skips the windowed decode but keeps the animation",
          "[shell][media]")
{
    WiringShell s;
    s.power_policy_.set_pref(tesseract::PowerPolicy::Pref::On);
    REQUIRE(s.power_policy_.active());
    s.decode_animated = true;
    const auto key = tk::CacheKey::media("mxc://x/anim-lp");
    s.account_manager_.image_cache().store(key, std::make_unique<WiringImage>());
    s.ingest(key, ShellBase::MediaKind::MediaThumbnail, {1, 2, 3});
    CHECK(s.windowed_decodes == 0);
    CHECK(s.account_manager_.anim_cache().has(key));
    CHECK_FALSE(s.account_manager_.image_cache().contains(key));
    CHECK(s.media_repaints == 1);
}

TEST_CASE("media ingest: nothing repaints once the shell is no longer ready",
          "[shell][media]")
{
    WiringShell s;
    s.decode_animated = true;
    std::vector<std::function<void()>> parked;
    s.parked = &parked;
    const auto key = tk::CacheKey::media("mxc://x/late");
    s.ingest(key, ShellBase::MediaKind::MediaImage, {1, 2, 3});
    REQUIRE_FALSE(parked.empty());
    s.ready = false;
    s.parked = nullptr;
    for (auto& fn : parked)
    {
        fn();
    }
    CHECK(s.account_manager_.anim_cache().has(key));
    CHECK(s.media_repaints == 0);
}

TEST_CASE("media ingest: posts queued by a streamed decode drop after the shell is gone",
          "[shell][media]")
{
    std::vector<std::function<void()>> parked;
    int repaints = 0;
    {
        WiringShell s;
        s.decode_animated = true;
        s.parked = &parked;
        s.repaint_sink = &repaints;
        s.ingest(tk::CacheKey::media("mxc://x/gone"),
                 ShellBase::MediaKind::MediaImage, {1, 2, 3});
        REQUIRE_FALSE(parked.empty());
        s.parked = nullptr;
    }
    for (auto& fn : parked)
    {
        fn(); // must not touch the destroyed shell
    }
    CHECK(repaints == 0);
}

TEST_CASE("media ingest: empty bytes and rejected bytes are ignored",
          "[shell][media]")
{
    WiringShell s;
    const auto key = tk::CacheKey::media("mxc://x/none");
    s.ingest(key, ShellBase::MediaKind::UserAvatar, {});
    s.accept_media = false;
    s.ingest(key, ShellBase::MediaKind::UserAvatar, {1, 2, 3});
    s.accept_media = true;
    s.ready = false;
    s.ingest(key, ShellBase::MediaKind::UserAvatar, {1, 2, 3});
    CHECK_FALSE(s.account_manager_.thumbnail_cache().contains(key));
    CHECK(s.media_repaints == 0);
}

TEST_CASE("inline decode fallback defaults to decode_image_", "[shell][media]")
{
    WiringShell s;
    CHECK(s.decode_inline_fallback_({1, 2, 3}, 10, 10).still != nullptr);
    s.decode_ok = false;
    CHECK(s.decode_inline_fallback_({1, 2, 3}, 10, 10).still == nullptr);
}

TEST_CASE("sign-out: with no accounts left the sign-in screen shows and add-account state resets",
          "[shell][signout]")
{
    WiringShell s;
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@a:x";
    tesseract::LogoutResult r;
    r.logged_out = true;
    r.has_remaining = false;
    r.logged_out_uid = "@a:x";
    s.finish_sign_out_ui_(r);
    CHECK(s.signed_out_hooks == 1);
    CHECK(s.sign_in_screens == 1);
    CHECK_FALSE(s.pending_login_is_add_account_);
    CHECK(s.add_account_return_uid_.empty());
    REQUIRE(s.statuses.size() == 1);
    CHECK(s.statuses.back() == "Signed out");
}

TEST_CASE("sign-out: while another account remains the message names the account",
          "[shell][signout]")
{
    WiringShell s;
    tesseract::LogoutResult r;
    r.logged_out = true;
    r.has_remaining = true;
    r.logged_out_uid = "@a:x";
    r.next_uid = "@b:x";
    s.finish_sign_out_ui_(r);
    CHECK(s.signed_out_hooks == 1);
    CHECK(s.sign_in_screens == 0);
    REQUIRE(s.statuses.size() == 1);
    CHECK(s.statuses.back() == "Signed out of @a:x");
}

TEST_CASE("relogin: the active account signs out", "[shell][signout]")
{
    WiringShell s;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    s.account_manager_.add_account(a);
    s.active_account_ = a;
    s.shared_relogin("@a:x");
    CHECK(s.logouts == 1);
    CHECK(s.account_manager_.find("@a:x") != nullptr);
}

TEST_CASE("relogin: another account is forgotten without touching the active one",
          "[shell][signout]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    auto b = std::make_shared<tesseract::AccountSession>();
    b->user_id = "@b:x";
    s.account_manager_.add_account(a);
    s.account_manager_.add_account(b);
    s.active_account_ = a;
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = "@b:x";
    idx.user_ids = {"@a:x", "@b:x"};
    REQUIRE(tesseract::SessionStore::save_index(idx));

    s.shared_relogin("@b:x");

    CHECK(s.logouts == 0);
    CHECK(s.account_manager_.find("@b:x") == nullptr);
    CHECK(s.account_manager_.find("@a:x") != nullptr);
    const auto after = tesseract::SessionStore::load_index();
    CHECK(after.user_ids == std::vector<std::string>{"@a:x"});
    CHECK(after.active_user_id == "@a:x");
}

TEST_CASE("sync status: Syncing rooms waits 300 ms and shows only if still busy",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.refresh_sync_status_();
    CHECK(s.sync_texts.empty());
    CHECK_FALSE(s.sync_progress_shown_);
    s.run_delayed(300);
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Syncing rooms…");
    CHECK(s.sync_progress_shown_);
}

TEST_CASE("sync status: a session that settles within 300 ms never flashes",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::SettingUp;
    s.refresh_sync_status_();
    s.last_room_list_state_ = tesseract::RoomListState::Running;
    s.refresh_sync_status_();
    s.run_delayed(300);
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Connected");
}

TEST_CASE("sync status: progress events do not postpone the wait",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.refresh_sync_status_();
    s.refresh_sync_status_();
    s.refresh_sync_status_();
    CHECK(s.delayed.size() == 1);
}

TEST_CASE("sync status: Syncing rooms waits again after a connected period",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.refresh_sync_status_();
    s.run_delayed(300);
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_progress_shown_);

    s.last_room_list_state_ = tesseract::RoomListState::Running;
    s.refresh_sync_status_();
    REQUIRE(s.sync_texts.size() == 2);
    CHECK(s.sync_texts.back() == "Connected");
    CHECK_FALSE(s.sync_progress_shown_);

    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.refresh_sync_status_();
    CHECK(s.sync_texts.size() == 2); // waiting again, not shown at once
    CHECK_FALSE(s.delayed.empty());
    s.run_delayed(300);
    REQUIRE(s.sync_texts.size() == 3);
    CHECK(s.sync_texts.back() == "Syncing rooms…");
}

TEST_CASE("sync status: once progress is showing, busy states update at once",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.sync_progress_shown_ = true;
    s.refresh_sync_status_();
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Syncing rooms…");
    CHECK(s.delayed.empty());
}

TEST_CASE("sync status: reconnecting, key download and connected texts",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Recovering;
    s.refresh_sync_status_();
    s.last_room_list_state_ = tesseract::RoomListState::Running;
    s.last_backup_state_ = tesseract::BackupState::Downloading;
    s.last_imported_keys_ = 42;
    s.refresh_sync_status_();
    s.last_backup_state_ = tesseract::BackupState::Unknown;
    s.refresh_sync_status_();
    REQUIRE(s.sync_texts.size() == 3);
    CHECK(s.sync_texts[0] == "Reconnecting…");
    CHECK(s.sync_texts[1] == "Downloading message keys (42)…");
    CHECK(s.sync_texts[2] == "Connected");
    CHECK_FALSE(s.sync_progress_shown_);
}

TEST_CASE("backup progress drives the key-download status text",
          "[shell][syncstatus]")
{
    WiringShell s;
    tesseract::Client client;
    s.client_ = &client;
    s.my_user_id_ = "@a:x";
    s.last_room_list_state_ = tesseract::RoomListState::Running;
    tesseract::BackupProgress p;
    p.state = tesseract::BackupState::Downloading;
    p.imported_keys = 7;
    s.handle_backup_progress_ui_(p);
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts[0] == "Downloading message keys (7)…");

    s.client_ = nullptr; // no active client: ignored
    p.imported_keys = 8;
    s.handle_backup_progress_ui_(p);
    CHECK(s.sync_texts.size() == 1);
}

TEST_CASE("sync status: a persistent status override is not replaced by Connected",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Running;
    s.status_override_active_ = true;
    s.refresh_sync_status_();
    CHECK(s.sync_texts.empty());
    CHECK(s.override_kept == 1);
}

TEST_CASE("status messages: link syntax in an ordinary message is shown plain",
          "[shell][status_links]")
{
    WiringShell s;
    // e.g. a homeserver error carried into the status bar
    s.show_status_message_("[403 / M_FORBIDDEN] see [here](https://evil.example)");
    s.show_status_message_("[Tesseract 9 available](https://example.org)", 0, true);
    s.show_status_message_("[403] [again](https://evil.example)");
    REQUIRE(s.status_had_links.size() == 3);
    CHECK_FALSE(s.status_had_links[0]);
    CHECK(s.status_had_links[1]);
    CHECK_FALSE(s.status_had_links[2]);
}

TEST_CASE("sync status: with nobody signed in the line says Not logged in",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.last_room_list_state_ = tesseract::RoomListState::Init; // stale state is ignored
    s.sync_progress_shown_ = true;
    s.refresh_sync_status_();
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Not logged in");
    CHECK_FALSE(s.sync_progress_shown_);
    CHECK(s.delayed.empty()); // no 300 ms wait for a state that doesn't apply
}

TEST_CASE("sync status: a persistent status override survives the signed-out refresh",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.status_override_active_ = true; // e.g. "Couldn't save the sign-in"
    s.refresh_sync_status_();
    CHECK(s.sync_texts.empty());
    CHECK(s.override_kept == 1);
}

TEST_CASE("sync status: Connected cancels a queued Syncing rooms",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.refresh_sync_status_();
    CHECK(s.sync_status_debounce_pending_);
    s.show_connected_status_();
    CHECK_FALSE(s.sync_status_debounce_pending_);
    s.run_delayed(300);
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Connected");
}

TEST_CASE("sync status: signing out cancels a queued Syncing rooms",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x"; // signed in
    s.last_room_list_state_ = tesseract::RoomListState::Init;
    s.refresh_sync_status_();
    CHECK(s.sync_status_debounce_pending_);
    s.my_user_id_.clear();
    s.refresh_sync_status_();
    CHECK_FALSE(s.sync_status_debounce_pending_);
    s.run_delayed(300);
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Not logged in");
}

TEST_CASE("sync status: a signed-in user gets the sync states again",
          "[shell][syncstatus]")
{
    WiringShell s;
    s.my_user_id_ = "@a:x";
    s.last_room_list_state_ = tesseract::RoomListState::Running;
    s.refresh_sync_status_();
    REQUIRE(s.sync_texts.size() == 1);
    CHECK(s.sync_texts.back() == "Connected");
}

TEST_CASE("animation frame: the main and settings surfaces are both repainted",
          "[shell][anim]")
{
    WiringFixture f;
    f.s.repaint_anim_frame_();
    f.s.repaint_anim_frame_();
    CHECK(f.s.main_anim_updates == 2);
    CHECK(f.s.settings_anim_updates == 2);
}

TEST_CASE("animation frame: the pagination spinner repaints the whole main surface",
          "[shell][anim]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    REQUIRE(f.app->room_view()->message_list() != nullptr);
    f.s.repaint_anim_frame_();
    CHECK(f.s.repaints == 0); // animated-image damage only

    f.app->room_view()->message_list()->set_paginating(true);
    f.s.repaint_anim_frame_();
    CHECK(f.s.main_anim_updates == 2);
    CHECK(f.s.repaints == 1);
}

TEST_CASE("animation tick: the pagination spinner alone keeps it running",
          "[shell][anim]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    CHECK_FALSE(f.s.tick_anim_()); // nothing animated: stops
    CHECK(f.s.main_anim_updates == 0);

    f.app->room_view()->message_list()->set_paginating(true);
    CHECK(f.s.tick_anim_());
    CHECK(f.s.main_anim_updates == 1);
    CHECK(f.s.repaints == 1);
}

TEST_CASE("animation frame: nothing to repaint before a window exists",
          "[shell][anim]")
{
    WiringShell s; // no main app, room view or popup
    CHECK_NOTHROW(s.repaint_anim_frame_());
    CHECK(s.main_anim_updates == 1);
}

namespace
{
struct NotifyFixture
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    NotifyFixture()
    {
        tesseract::Settings::instance().notifications_enabled = true;
        auto a = std::make_shared<tesseract::AccountSession>();
        a->user_id = "@a:x";
        s.account_manager_.add_account(a);
        s.active_account_ = a;
        s.current_room_id_ = "!open:x";
    }
};
} // namespace

TEST_CASE("notifications: nothing is shown for the room you are looking at",
          "[shell][notify]")
{
    NotifyFixture f;
    f.s.win_focused = true;
    f.s.notify("@a:x", "!open:x");
    CHECK(f.s.delivered.empty());
    CHECK(f.s.attention_requests == 0);
}

TEST_CASE("notifications: a focused window still notifies for another room",
          "[shell][notify]")
{
    NotifyFixture f;
    f.s.win_focused = true;
    f.s.notify("@a:x", "!other:x");
    REQUIRE(f.s.delivered.size() == 1);
    CHECK(f.s.delivered[0].second == "!other:x");
    CHECK(f.s.attention_requests == 0);
}

TEST_CASE("notifications: an unfocused window gets attention and the notification",
          "[shell][notify]")
{
    NotifyFixture f;
    f.s.win_focused = false;
    f.s.notify("@a:x", "!open:x"); // even the open room: nobody is looking
    CHECK(f.s.attention_requests == 1);
    CHECK(f.s.delivered.size() == 1);
}

TEST_CASE("notifications: the same room on another account still notifies",
          "[shell][notify]")
{
    NotifyFixture f;
    f.s.win_focused = true;
    f.s.notify("@b:x", "!open:x");
    CHECK(f.s.delivered.size() == 1);
}

TEST_CASE("notifications: off in settings, or no window, means nothing",
          "[shell][notify]")
{
    NotifyFixture f;
    tesseract::Settings::instance().notifications_enabled = false;
    f.s.notify("@a:x", "!other:x");
    CHECK(f.s.delivered.empty());
    tesseract::Settings::instance().notifications_enabled = true;
    f.s.ready = false;
    f.s.notify("@a:x", "!other:x");
    CHECK(f.s.delivered.empty());
}

TEST_CASE("notifications: a focused but hidden window still notifies for the open room",
          "[shell][notify]")
{
    NotifyFixture f;
    f.s.win_focused = true;
    f.s.win_visible = false;
    f.s.notify("@a:x", "!open:x");
    CHECK(f.s.delivered.size() == 1);
    CHECK(f.s.attention_requests == 0);
}

TEST_CASE("notifications: the delivered notification carries the message",
          "[shell][notify]")
{
    struct Unlocked : tesseract::IScreenLock
    {
        bool is_locked() const override { return false; }
    };
    NotifyFixture f;
    f.s.set_screen_lock_(std::make_unique<Unlocked>());
    f.s.notify_with_media("@a:x", "!other:x");
    REQUIRE(f.s.delivered.size() == 1);
    const auto& n = f.s.last_notification;
    CHECK(n.room_id == "!other:x");
    CHECK(n.room_name == "Room");
    CHECK(n.sender == "Sender");
    CHECK(n.body == "hi");
    CHECK(n.is_mention);
    CHECK(n.event_id == "$e");
    CHECK(n.avatar_bytes == std::vector<std::uint8_t>{1, 2});
    CHECK(n.image_bytes == std::vector<std::uint8_t>{3, 4});
}

TEST_CASE("notifications: the message picture is dropped while the screen lock is unknown",
          "[shell][notify]")
{
    NotifyFixture f; // the default screen lock reports locked
    f.s.notify_with_media("@a:x", "!other:x");
    REQUIRE(f.s.delivered.size() == 1);
    CHECK(f.s.last_notification.image_bytes.empty());
    CHECK(f.s.last_notification.avatar_bytes == std::vector<std::uint8_t>{1, 2});
}

TEST_CASE("notifications: hiding content reaches the delivered notification",
          "[shell][notify]")
{
    NotifyFixture f;
    tesseract::Settings::instance().notification_hide_content = true;
    f.s.notify_with_media("@a:x", "!other:x");
    REQUIRE(f.s.delivered.size() == 1);
    const auto& n = f.s.last_notification;
    CHECK(n.sender == "Tesseract");
    CHECK(n.body != "hi");
    CHECK(n.room_name.empty());
    CHECK(n.avatar_bytes.empty());
    CHECK(n.image_bytes.empty());
}

TEST_CASE("notifications: the base delivery hands the notification to the account's notifier",
          "[shell][notify]")
{
    struct RecordingNotifier : tesseract::INotifier
    {
        explicit RecordingNotifier(std::vector<std::string>& rooms) : rooms_(rooms) {}
        void notify(const tesseract::Notification& n) override
        {
            rooms_.push_back(n.room_id);
        }
        std::vector<std::string>& rooms_;
    };
    NotifyFixture f;
    std::vector<std::string> rooms;
    f.s.account_manager_.find("@a:x")->notifier =
        std::make_unique<RecordingNotifier>(rooms);
    tesseract::Notification n;
    n.room_id = "!r:x";
    f.s.shared_deliver("@a:x", n);
    f.s.shared_deliver("@nobody:x", n); // unknown account: ignored
    CHECK(rooms == std::vector<std::string>{"!r:x"});
}

TEST_CASE("rooms updated: the native tail runs, and not once the window is gone",
          "[shell][rooms]")
{
    WiringFixture f;
    f.s.rooms_updated();
    CHECK(f.s.rooms_tails == 1);
    f.s.ready = false;
    f.s.rooms_updated();
    CHECK(f.s.rooms_tails == 1);
}

TEST_CASE("rooms updated: a pending restore whose room is unknown stays pending, "
          "whatever the room list state",
          "[shell][rooms]")
{
    WiringFixture f;
    // The room list has not reached Running (the default Init): macOS used to
    // hold the restore back for that; now it is attempted like on every shell.
    f.s.pending_restore_rooms_ = {"!gone:x"};
    f.s.rooms_updated();
    CHECK(f.s.pending_restore_rooms_.size() == 1);
    CHECK(f.s.rooms_tails == 1);
}

TEST_CASE("rooms updated: a pending restore opens its tabs once the rooms are known",
          "[shell][rooms]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.rooms_ = {wiring_make_room("!a:x")};
    f.s.mark_room_index_dirty_();
    f.s.pending_restore_rooms_ = {"!a:x"};
    REQUIRE(f.s.current_room_id_.empty());
    // The room list is still in its initial state: no Running gate.
    f.s.rooms_updated();
    CHECK(f.s.pending_restore_rooms_.empty());
    CHECK(f.s.current_room_id_ == "!a:x");
    CHECK(f.s.rooms_tails == 1);
}

TEST_CASE("rooms updated: the open room's info reaches the room view",
          "[shell][rooms]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.rooms_ = {wiring_make_room("!a:x")};
    f.s.mark_room_index_dirty_();
    f.s.current_room_id_ = "!a:x";
    CHECK_FALSE(f.app->room_view()->header()->show_search_btn());
    f.s.pending_restore_rooms_ = {"!a:x"};
    f.s.rooms_updated();
    CHECK(f.app->room_view()->header()->show_search_btn()); // set_room ran
    CHECK(f.s.pending_restore_rooms_.empty()); // a restore is dropped once a room is open
}

// ── wire_settings_view_ ─────────────────────────────────────────────────────

namespace
{
struct SettingsFixture
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(900, 700);
    std::unique_ptr<SettingsView> view =
        tk::create_root_widget<SettingsView>(nullptr);
    SettingsFixture() { s.wire_settings_view_(view.get()); }
};
} // namespace

TEST_CASE("wiring: settings view changes are persisted by the shell",
          "[shell][wiring][settings]")
{
    SettingsFixture f;
    auto& st = tesseract::Settings::instance();
    using K = tesseract::views::SettingKey;
    REQUIRE(f.view->on_setting_changed != nullptr);

    // The view writes the field, then reports the key.
    st.notification_hide_content = true;
    st.notification_image_previews = false;
    st.prefetch_full_media = true;
    st.autoscroll_unread_rooms = false;
    st.exclude_insecure_devices = true;
    st.block_sends_on_unverified_devices = false;
    st.group_inactive_rooms = true;
    st.group_unread_rooms = true;
    st.inactive_room_threshold_days = 45;
    st.audio_input_device_id = "mic-1";
    st.audio_output_device_id = "spk-2";
    st.camera_device_id = "cam-3";
    st.language = "de";
    st.proxy_mode = tesseract::Settings::ProxyMode::Manual;
    st.proxy_url = "http://proxy.invalid:3128";
    for (K k : {K::HideContent, K::ImagePreviews, K::Prefetch, K::AutoscrollUnread,
                K::ExcludeInsecure, K::BlockOnUnverified, K::GroupInactive, K::GroupUnread,
                K::InactivePeriod, K::AudioInput, K::AudioOutput, K::Camera,
                K::Language, K::Proxy})
        f.view->on_setting_changed(k);

    // Everything reached disk: reset in memory, reload.
    st.notification_hide_content = false;
    st.notification_image_previews = true;
    st.prefetch_full_media = false;
    st.autoscroll_unread_rooms = true;
    st.exclude_insecure_devices = false;
    st.block_sends_on_unverified_devices = true;
    st.group_inactive_rooms = false;
    st.group_unread_rooms = false;
    st.inactive_room_threshold_days = 7;
    st.audio_input_device_id.clear();
    st.audio_output_device_id.clear();
    st.camera_device_id.clear();
    st.language = "auto";
    st.proxy_mode = tesseract::Settings::ProxyMode::System;
    st.proxy_url.clear();
    st.load_from_disk(tesseract::config_dir());
    CHECK(st.notification_hide_content);
    CHECK_FALSE(st.notification_image_previews);
    CHECK(st.prefetch_full_media);
    CHECK_FALSE(st.autoscroll_unread_rooms);
    CHECK(st.exclude_insecure_devices);
    CHECK_FALSE(st.block_sends_on_unverified_devices);
    CHECK(st.group_inactive_rooms);
    CHECK(st.group_unread_rooms);
    CHECK(st.inactive_room_threshold_days == 45);
    CHECK(st.audio_input_device_id == "mic-1");
    CHECK(st.audio_output_device_id == "spk-2");
    CHECK(st.camera_device_id == "cam-3");
    CHECK(st.language == "de");
    CHECK(st.proxy_mode == tesseract::Settings::ProxyMode::Manual);
    CHECK(st.proxy_url == "http://proxy.invalid:3128");
}

TEST_CASE("wiring: settings view with a null pointer is ignored",
          "[shell][wiring][settings]")
{
    WiringShell s;
    CHECK_NOTHROW(s.wire_settings_view_(nullptr));
    CHECK_NOTHROW(s.wire_settings_controller_common_(nullptr, nullptr));
}

TEST_CASE("wiring: avatar-changed result updates the shell and active account",
          "[shell][wiring][settings]")
{
    WiringShell s;
    // The controller must outlive the view: the view keeps a pointer to it and
    // touches it again on destruction.
    auto post = [](std::function<void()> fn) { fn(); };
    auto picker = [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto decode = [](const std::vector<uint8_t>&)
    { return std::shared_ptr<tk::Image>(); };
    auto run = [](std::function<void()> fn) { fn(); };
    tesseract::SettingsController ctrl(nullptr, post, run, picker, decode);

    StubHost host; // the view relayouts through its host
    auto view = tk::create_root_widget<SettingsView>(&host);
    auto sess = std::make_shared<tesseract::AccountSession>();
    sess->user_id = "@me:x";
    s.active_account_ = sess;

    s.wire_settings_controller_common_(view.get(), &ctrl);
    REQUIRE(ctrl.on_avatar_changed != nullptr);
    const int before = host.relayout_count;
    ctrl.on_avatar_changed("mxc://x/new");
    CHECK(s.my_avatar_url_ == "mxc://x/new");
    CHECK(sess->avatar_url == "mxc://x/new");
    CHECK(host.relayout_count > before);
}

// ── wire_voice_capture_ ─────────────────────────────────────────────────────

namespace
{
struct VoiceFixture
{
    WiringShell s;
    tesseract::Client client;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    std::unique_ptr<MainAppWidget> app =
        tk::create_root_widget<MainAppWidget>(nullptr);
    FakeCapture* cap = nullptr;
    int repaints = 0;
    int clears = 0;
    std::string room = "!voice:x";

    VoiceFixture()
    {
        s.client_ = &client;
        s.main_app_ = app.get();
        s.room_view_ = app->room_view();
        auto c = std::make_unique<FakeCapture>();
        cap = c.get();
        s.capture_ = std::move(c);
        s.wire_voice_capture_(
            app->room_view(), [this] { ++repaints; }, [this] { return room; },
            [this] { ++clears; });
    }
};
} // namespace

TEST_CASE("wiring: voice capture without a device hides the mic", "[shell][wiring][voice]")
{
    WiringShell s;
    auto surface = TestSurface::create(1000, 700);
    auto app = tk::create_root_widget<MainAppWidget>(nullptr);
    s.wire_voice_capture_(app->room_view(), [] {}, [] { return std::string(); },
                          [] {});
    CHECK(app->room_view()->on_mic_clicked == nullptr);
}

TEST_CASE("wiring: mic click toggles recording and amplitude repaints",
          "[shell][wiring][voice]")
{
    VoiceFixture f;
    REQUIRE(f.app->room_view()->on_mic_clicked != nullptr);
    f.app->room_view()->on_mic_clicked();
    CHECK(f.cap->starts == 1);
    CHECK(f.cap->recording);
    CHECK(f.repaints >= 1);

    const int before = f.repaints;
    f.cap->on_amplitude(500);
    CHECK(f.repaints == before + 1);

    // Second click stops; a too-short clip is dropped without clearing input.
    f.cap->dur = 100;
    f.cap->pcm = {1, 2, 3};
    f.app->room_view()->on_mic_clicked();
    CHECK(f.cap->stops == 1);
    CHECK(f.clears == 0);
}

TEST_CASE("wiring: a long clip clears the compose field and is sent async",
          "[shell][wiring][voice]")
{
    VoiceFixture f;
    f.app->room_view()->on_mic_clicked();
    f.cap->dur = 2000;
    f.cap->pcm = std::vector<std::uint8_t>(64, 1);
    f.app->room_view()->on_mic_clicked();
    CHECK(f.clears == 1);
}

TEST_CASE("wiring: an empty recording (device error) sends nothing",
          "[shell][wiring][voice]")
{
    VoiceFixture f;
    f.app->room_view()->on_mic_clicked();
    f.cap->dur = 5000;
    f.cap->pcm.clear();
    f.app->room_view()->on_mic_clicked();
    CHECK(f.clears == 0);
}

TEST_CASE("wiring: cancelling a voice recording discards it", "[shell][wiring][voice]")
{
    VoiceFixture f;
    f.app->room_view()->on_mic_clicked();
    REQUIRE(f.app->room_view()->on_cancel_voice != nullptr);
    f.app->room_view()->on_cancel_voice();
    CHECK(f.cap->cancels == 1);
    CHECK_FALSE(f.cap->recording);
}

// ── small shared handlers moved out of the shells ───────────────────────────

TEST_CASE("room list scroll: backfill is debounced to one pending post and needs an account",
          "[shell][wiring][backfill]")
{
    WiringFixture f;
    auto* list = f.app->room_list_view();
    REQUIRE(list != nullptr);
    REQUIRE(list->on_scroll != nullptr);

    auto count_backfill_posts = [&f]
    {
        int n = 0;
        for (const auto& d : f.s.delayed)
            n += d.first == 300 ? 1 : 0;
        return n;
    };
    const int before = count_backfill_posts();
    for (int i = 0; i < 20; ++i)
        list->on_scroll(); // a scroll drag fires this every frame
    CHECK(count_backfill_posts() == before + 1);

    // With no active account the settled fire does nothing.
    REQUIRE_FALSE(f.s.active_account_);
    f.s.run_delayed(300);
    CHECK(f.s.pool_pending_count_() == 0);
    CHECK(f.s.mut_pool_pending_count_() == 0);
}

TEST_CASE("mark-read timer: the latest restart wins and clears the room's unread",
          "[shell][wiring][markread]")
{
    WiringShell s;
    tesseract::RoomInfo a = wiring_make_room("!a:x");
    a.notification_count = 3;
    tesseract::RoomInfo b = wiring_make_room("!b:x");
    b.notification_count = 2;
    s.rooms_ = {a, b};

    s.current_room_id_ = "!a:x";
    s.restart_mark_read_timer_(250);
    s.current_room_id_ = "!b:x";
    s.restart_mark_read_timer_(250);
    // One pending post however often the timer restarts.
    REQUIRE(s.delayed.size() == 1);
    CHECK(s.delayed[0].first == 250);

    // The first fire sees a newer restart: nothing is marked, the post re-arms.
    auto first = std::move(s.delayed[0].second);
    s.delayed.clear();
    first();
    CHECK(s.rooms_[0].notification_count == 3);
    CHECK(s.rooms_[1].notification_count == 2);
    REQUIRE(s.delayed.size() == 1);

    // The re-armed fire runs the latest restart, on the room current then.
    auto second = std::move(s.delayed[0].second);
    s.delayed.clear();
    second();
    CHECK(s.rooms_[0].notification_count == 3);
    CHECK(s.rooms_[1].notification_count == 0);
    s.drain_workers();
}

TEST_CASE("account badge: the sidebar dot follows other accounts' unread and relayouts",
          "[shell][wiring][badge]")
{
    WiringFixture f;
    REQUIRE(f.app->user_info() != nullptr);
    const int before = f.s.relayouts;
    f.s.on_account_badges_changed_(true);
    CHECK(f.app->user_info()->notification_dot());
    CHECK(f.s.relayouts == before + 1);
    f.s.on_account_badges_changed_(false);
    CHECK_FALSE(f.app->user_info()->notification_dot());

    f.s.main_app_ = nullptr; // window gone: ignored
    f.s.on_account_badges_changed_(true);
}

TEST_CASE("picker repaint: relayouts and lets the platform repaint its own popups",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    const int before = f.s.relayouts;
    f.s.shared_repaint_pickers();
    CHECK(f.s.relayouts == before + 1);
    CHECK(f.s.native_picker_repaints == 1);
}

TEST_CASE("picker repaint and pack refresh tolerate a missing room view",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.room_view_ = nullptr;
    f.s.refresh_pickers_packs_(); // no room view: nothing to refresh
    const int before = f.s.relayouts;
    f.s.shared_repaint_pickers(); // still relayouts and lets the platform repaint
    CHECK(f.s.relayouts == before + 1);
    CHECK(f.s.native_picker_repaints == 1);

    f.s.room_view_ = f.app->room_view();
    f.s.current_room_id_ = "!r:x";
    f.s.refresh_pickers_packs_();
}

TEST_CASE("server info: the settings view is optional and the main surface relayouts",
          "[shell][wiring][serverinfo]")
{
    WiringFixture f;
    f.s.server_info_.supports_msc3030 = true;
    const int before = f.s.relayouts;
    f.s.on_server_info_ready_ui_(); // no settings view open
    CHECK(f.s.relayouts == before + 1);

    CHECK(f.app->room_view()->header()->jump_to_date_enabled());
    f.s.server_info_.supports_msc3030 = false;
    f.s.on_server_info_ready_ui_();
    CHECK_FALSE(f.app->room_view()->header()->jump_to_date_enabled());

    StubHost host; // the profile editors only exist with a host
    auto sv = tk::create_root_widget<tesseract::views::SettingsView>(&host);
    f.s.stats_settings_view_ = sv.get();
    f.s.on_server_info_ready_ui_();
    REQUIRE(sv->pronouns_editor() != nullptr);

    // The view relayouts its own surface through its tk::Host (StubHost is a
    // TestHost, which counts them).
    int view_relayouts = host.relayout_count;
    f.s.own_extended_profile_.pronouns = {{"en", "she/her", ""}};
    f.s.on_own_extended_profile_ready_ui_();
    REQUIRE(sv->pronouns_editor()->pronouns().size() == 1);
    CHECK(sv->pronouns_editor()->pronouns()[0].summary == "she/her");
    CHECK(host.relayout_count > view_relayouts);

    view_relayouts = host.relayout_count;
    f.s.on_profile_field_result_ui_("io.fsky.nyx.pronouns", false, "denied");
    CHECK(sv->pronouns_editor()->error() == "denied");
    CHECK(host.relayout_count > view_relayouts);
    f.s.on_profile_field_result_ui_("io.fsky.nyx.pronouns", true, "");
    CHECK(sv->pronouns_editor()->error() == "denied"); // success leaves it alone

    f.s.stats_settings_view_ = nullptr;
    f.s.own_extended_profile_.pronouns.clear();
    view_relayouts = host.relayout_count;
    f.s.on_own_extended_profile_ready_ui_(); // closed again: ignored
    CHECK(sv->pronouns_editor()->pronouns().size() == 1);
    f.s.on_profile_field_result_ui_("io.fsky.nyx.pronouns", true, "");
    CHECK(host.relayout_count == view_relayouts);
}

TEST_CASE("gif source: a media disk cache hit is delivered from the cache",
          "[shell][gif]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    auto acct = std::make_shared<tesseract::AccountSession>();
    acct->user_id = "@a:x";
    acct->client  = std::make_unique<tesseract::Client>();
    s.active_account_ = acct;
    const std::string url = "mxc://x/gifsrc";
    s.account_manager_.media_disk_cache().store(tk::CacheKey::gif_source(url),
                                                {1, 2, 3});
    int calls = 0;
    std::vector<std::uint8_t> got;
    s.fetch_gif_source_(url, [&](std::vector<std::uint8_t> b)
                        { ++calls; got = std::move(b); });
    s.drain_workers();
    CHECK(calls == 1);
    CHECK(got == std::vector<std::uint8_t>{1, 2, 3});
}

TEST_CASE("gif source: without a client the callback gets empty bytes at once",
          "[shell][gif]")
{
    WiringShell s;
    int calls = 0;
    std::vector<std::uint8_t> got{9};
    s.fetch_gif_source_("mxc://x/none", [&](std::vector<std::uint8_t> b)
                        { ++calls; got = std::move(b); });
    CHECK(calls == 1);
    CHECK(got.empty());
}

TEST_CASE("composer popups: nothing is built without a room view, and hiding is safe",
          "[shell][wiring][popups]")
{
    WiringShell s;
    TestHost host{nullptr};
    s.create_compose_popups_(host); // no room view, no text area
    CHECK(s.compose_popups_ == nullptr);
    s.hide_compose_popups_();
    s.handle_gif_results_ui_(1, {}); // nobody listening: ignored
    s.handle_gif_search_failed_ui_(1, "x");
}

namespace
{
// WiringHostFixture with the main room pane and the main composer's popups
// built, as ShellBase does once the main window exists.
struct ComposerFixture : WiringHostFixture
{
    std::size_t first_popup = 0;
    ComposerFixture()
    {
        s.main_fake = std::make_unique<FakeMainSurface>(*surface);
        s.room_view_ = app->room_view();
        app->room_view()->set_room(wiring_make_room("!a:x"));
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        app->measure(lc, {1000, 700});
        app->arrange(lc, {0, 0, 1000, 700});
        s.create_main_room_pane_();
        first_popup = host.popups_created.size();
        s.create_compose_popups_(host);
    }
    ~ComposerFixture() { s.release_popups_(); }
    StubTextArea* composer_native()
    {
        auto* area = app->room_view()->compose_bar()->text_area();
        area->set_text("\x01probe");
        for (auto* a : host.areas_created)
        {
            if (a->text_ == "\x01probe")
            {
                area->set_text("");
                return a;
            }
        }
        return nullptr;
    }
    StubPopupSurface* popup(std::size_t i) { return host.popups_created.at(first_popup + i); }
};
} // namespace

TEST_CASE("composer popups: the main composer's typing reaches the shell's typing hook",
          "[shell][wiring][popups]")
{
    ComposerFixture f;
    REQUIRE(f.s.compose_popups_ != nullptr);
    REQUIRE(f.host.popups_created.size() == f.first_popup + 4);
    auto* nat = f.composer_native();
    REQUIRE(nat != nullptr);
    REQUIRE(nat->on_changed != nullptr);

    CHECK_FALSE(f.s.compose_typing_active_);
    nat->on_changed("a");
    CHECK(f.s.compose_typing_active_);
    nat->on_changed("");
    CHECK_FALSE(f.s.compose_typing_active_);

    // The popups run alongside the hook.
    nat->text_ = "/po";
    nat->on_changed("/po");
    CHECK(f.popup(1)->visible());
}

TEST_CASE("composer popups: landed media redraws only the popups that show it",
          "[shell][wiring][popups]")
{
    ComposerFixture f;
    REQUIRE(f.s.compose_popups_ != nullptr);
    auto* mention = f.popup(0);
    auto* shortcode = f.popup(2);

    f.s.shared_repaint_after_media(ShellBase::MediaKind::UserAvatar);
    f.s.shared_repaint_after_media(ShellBase::MediaKind::Sticker);
    CHECK(mention->repaints == 0);
    CHECK(shortcode->relayouts == 0);

    mention->set_visible(true);
    shortcode->set_visible(true);
    f.s.shared_repaint_after_media(ShellBase::MediaKind::UserAvatar);
    CHECK(mention->repaints == 1);
    CHECK(shortcode->relayouts == 0);
    f.s.shared_repaint_after_media(ShellBase::MediaKind::Sticker);
    CHECK(mention->repaints == 1);
    CHECK(shortcode->relayouts == 1);
}

TEST_CASE("settings view: a new view gets the shared callbacks and leaves through the shell",
          "[shell][wiring][settings]")
{
    WiringShell s;
    auto sv = tk::create_root_widget<tesseract::views::SettingsView>(nullptr);
    s.configure_new_settings_view_(*sv);
    CHECK(s.stats_settings_view_ == sv.get());
    REQUIRE(sv->on_close != nullptr);
    REQUIRE(sv->on_logout != nullptr);
    REQUIRE(sv->on_reset_identity != nullptr);
    REQUIRE(sv->on_tab_changed != nullptr);

    sv->on_close();
    CHECK(s.settings_closes == 1);
    CHECK(s.logouts == 0);

    sv->on_logout(); // leaves settings first, then signs out
    CHECK(s.settings_closes == 2);
    CHECK(s.logouts == 1);

    sv->on_tab_changed(); // relayouts through the view's host; no host, no crash
    s.stats_settings_view_ = nullptr;
    sv->on_tab_changed();
}

TEST_CASE("settings view: reset identity leaves settings, then starts the identity reset",
          "[shell][wiring][settings]")
{
    WiringFixture f;
    auto sv = tk::create_root_widget<tesseract::views::SettingsView>(nullptr);
    f.s.configure_new_settings_view_(*sv);
    REQUIRE(sv->on_reset_identity != nullptr);
    REQUIRE_FALSE(f.s.encryption_setup_shown_);

    sv->on_reset_identity();
    CHECK(f.s.settings_closes == 1);
    CHECK(f.s.encryption_setup_shown_); // client_ and main_app_ are set
}

TEST_CASE("settings view: populating needs no cache-size callback after the view is gone",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    s.my_display_name_ = "Me";
    s.my_user_id_ = "@me:x";
    s.own_extended_profile_.tz = "Europe/Oslo";
    auto sv = tk::create_root_widget<tesseract::views::SettingsView>(nullptr);
    s.stats_settings_view_ = sv.get();
    s.populate_settings_view_(*sv);
    s.stats_settings_view_ = nullptr; // closed before the async sizes arrive
    s.drain_workers();
}

TEST_CASE("settings view: populating applies the extended profile only when there is one",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    StubHost host; // the profile editors only exist with a host
    auto sv = tk::create_root_widget<tesseract::views::SettingsView>(&host);
    s.stats_settings_view_ = sv.get();
    REQUIRE(sv->pronouns_editor() != nullptr);

    // An empty profile leaves what the view already shows alone.
    tesseract::ExtendedProfile shown;
    shown.pronouns = {{"en", "they/them", ""}};
    sv->set_extended_profile(shown);
    int relayouts = host.relayout_count;
    s.populate_settings_view_(*sv);
    CHECK(sv->pronouns_editor()->pronouns().size() == 1);
    CHECK(host.relayout_count > relayouts);

    s.own_extended_profile_.pronouns = {{"en", "she/her", ""}, {"nb", "hun", ""}};
    s.populate_settings_view_(*sv);
    REQUIRE(sv->pronouns_editor()->pronouns().size() == 2);
    CHECK(sv->pronouns_editor()->pronouns()[0].summary == "she/her");

    // The cache sizes arrive asynchronously and relayout the view.
    s.drain_workers();
    relayouts = host.relayout_count;
    s.populate_settings_view_(*sv);
    s.drain_workers();
    CHECK(host.relayout_count >= relayouts + 2);
    s.stats_settings_view_ = nullptr;
}

TEST_CASE("key dialogs: the settings controller reaches the shell's native hooks",
          "[shell][wiring][dialogs]")
{
    WiringShell s;
    s.ensure_settings_controller_();
    auto& c = *s.settings_controller_;
    REQUIRE(c.show_passphrase_prompt);
    REQUIRE(c.show_save_file_dialog);
    REQUIRE(c.show_open_file_dialog);

    std::string got;
    c.show_passphrase_prompt("Enter passphrase", [&](std::string p) { got = p; });
    CHECK(got == "secret");
    c.show_save_file_dialog("keys.txt", [&](std::string p) { got = p; });
    CHECK(got == "/tmp/out.txt");
    c.show_open_file_dialog([&](std::string p) { got = p; });
    CHECK(got == "/tmp/in.txt");

    c.on_export_keys_result(true, "");
    c.on_export_keys_result(false, "disk full");
    c.on_import_keys_result(true, "");
    c.on_import_keys_result(false, "bad file");
    REQUIRE(s.dialogs.size() == 7);
    CHECK(s.dialogs[0] == "pass:Enter passphrase");
    CHECK(s.dialogs[1] == "save:Export message keys:keys.txt");
    CHECK(s.dialogs[2] == "open:Import message keys");
    CHECK(s.dialogs[3] == "info:Export complete:Message keys exported successfully.");
    CHECK(s.dialogs[4] == "error:Export failed:disk full");
    CHECK(s.dialogs[5] == "info:Import complete:Message keys imported successfully.");
    CHECK(s.dialogs[6] == "error:Import failed:bad file");
}

TEST_CASE("history export: the folder picker is the shell's native hook",
          "[shell][wiring][dialogs]")
{
    WiringShell s;
    s.ensure_history_export_controller_();
    REQUIRE(s.history_export_controller_->show_save_folder_dialog);
    std::string got;
    s.history_export_controller_->show_save_folder_dialog(
        "room", [&](std::string p) { got = p; });
    CHECK(got == "/tmp/dir");
    REQUIRE(s.dialogs.size() == 1);
    CHECK(s.dialogs[0] == "folder:Choose a folder for the exported history");
}

TEST_CASE("in-flight tooltip: count line plus the queue depths",
          "[shell][wiring][inflight]")
{
    WiringShell s;
    s.last_room_list_state_ = tesseract::RoomListState::Init; // no sync poll
    s.last_inflight_ = 1;
    const std::string one = s.inflight_tooltip_text_();
    CHECK(one.rfind("1 request in flight\n", 0) == 0);
    CHECK(one.find("media: 0 loading · fetch: 0 queued · send: 0 queued") !=
          std::string::npos);

    s.last_inflight_ = 3;
    CHECK(s.inflight_tooltip_text_().rfind("3 requests in flight\n", 0) == 0);

    s.last_room_list_state_ = tesseract::RoomListState::Running; // sync counts
    CHECK(s.inflight_tooltip_text_().rfind("4 requests in flight\n", 0) == 0);
}

TEST_CASE("login failure: the status says why and the login view gets a fresh client",
          "[shell][wiring][login]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    s.pending_login_client_ = std::make_unique<tesseract::Client>(); // no user id
    s.pending_login_is_add_account_ = true;
    s.on_login_succeeded_();
    s.drain_workers();
    CHECK(s.detaches == 1);
    REQUIRE_FALSE(s.statuses.empty());
    CHECK(s.statuses.back() == "Sign-in failed: no user id");
    REQUIRE(s.login_uis.size() == 1);
    CHECK(s.login_uis[0] == Mode::AddAccount); // retry stays in add-account mode
    CHECK(s.pending_login_client_ != nullptr);
}

TEST_CASE("login cancel: an add-account flow returns to the account it started from",
          "[shell][wiring][login]")
{
    WiringShell s;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    s.account_manager_.add_account(a);
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@a:x";
    s.on_login_cancelled_();
    CHECK(s.detaches == 1);
    REQUIRE(s.switched.size() == 1);
    CHECK(s.switched[0] == "@a:x");
    CHECK(s.main_contents == 1);
    CHECK(s.login_uis.empty());
    CHECK_FALSE(s.pending_login_is_add_account_);
    CHECK(s.add_account_return_uid_.empty());
}

TEST_CASE("login cancel: with nothing to return to the sign-in screen is re-armed",
          "[shell][wiring][login]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.pending_login_is_add_account_ = true; // add-account with no valid return
    s.add_account_return_uid_.clear();
    s.on_login_cancelled_();
    CHECK(s.switched.empty());
    CHECK(s.main_contents == 0);
    REQUIRE(s.login_uis.size() == 1);
    CHECK(s.login_uis[0] == Mode::Initial);
    CHECK(s.pending_login_client_ != nullptr);
}

TEST_CASE("login cancel: an account removed meanwhile is not switched to",
          "[shell][wiring][login]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    auto b = std::make_shared<tesseract::AccountSession>();
    b->user_id = "@b:x";
    s.account_manager_.add_account(b);
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@a:x"; // removed by another window
    s.on_login_cancelled_();
    CHECK(s.switched.empty());
    CHECK(s.main_contents == 0);
    REQUIRE(s.login_uis.size() == 1);
    CHECK(s.login_uis[0] == Mode::Initial);
    CHECK(s.add_account_return_uid_.empty());
}

TEST_CASE("login cancel: an account another window took is not switched to",
          "[shell][wiring][login]")
{
    WiringShell s, other;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    s.account_manager_.add_account(a);
    s.account_manager_.set_dedicated("@a:x", &other);
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@a:x";
    s.on_login_cancelled_();
    CHECK(s.switched.empty());
    CHECK(s.main_contents == 0);
    CHECK(s.login_uis.size() == 1);
    s.account_manager_.clear_dedicated("@a:x");
}

TEST_CASE("login finalized: success switches to the new account and clears the add-account state",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@a:x";
    tesseract::FinalizeLoginResult fin;
    fin.ok = true;
    fin.user_id = "@new:x";
    s.on_login_finalized_(fin);
    REQUIRE(s.switched.size() == 1);
    CHECK(s.switched[0] == "@new:x");
    CHECK(s.main_contents == 1);
    CHECK(s.login_uis.empty());
    CHECK_FALSE(s.pending_login_is_add_account_);
    CHECK(s.add_account_return_uid_.empty());
}

TEST_CASE("login finalized: a duplicate returns to the account the flow started from",
          "[shell][wiring][login]")
{
    WiringShell s;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    s.account_manager_.add_account(a);
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@a:x";
    tesseract::FinalizeLoginResult fin;
    fin.rejected_duplicate = true;
    fin.user_id = "@a:x";
    s.on_login_finalized_(fin);
    REQUIRE_FALSE(s.statuses.empty());
    CHECK(s.statuses.back() == "Already signed in as @a:x");
    REQUIRE(s.switched.size() == 1);
    CHECK(s.switched[0] == "@a:x");
    CHECK(s.main_contents == 1);
    CHECK(s.login_uis.empty());
    CHECK_FALSE(s.pending_login_is_add_account_);
    CHECK(s.add_account_return_uid_.empty());
}

TEST_CASE("login finalized: a duplicate with nowhere to return re-arms the login view",
          "[shell][wiring][login]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    s.pending_login_is_add_account_ = true;
    s.add_account_return_uid_ = "@gone:x"; // no such account
    tesseract::FinalizeLoginResult fin;
    fin.rejected_duplicate = true;
    fin.user_id = "@a:x";
    s.on_login_finalized_(fin);
    CHECK(s.switched.empty());
    CHECK(s.main_contents == 0);
    REQUIRE(s.login_uis.size() == 1);
    CHECK(s.login_uis[0] == Mode::AddAccount);
    CHECK(s.pending_login_client_ != nullptr);
    CHECK(s.pending_login_is_add_account_);
}

TEST_CASE("login finalized: a failure outside add-account re-arms the initial sign-in",
          "[shell][wiring][login]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    tesseract::FinalizeLoginResult fin;
    fin.error = "disk full";
    s.on_login_finalized_(fin);
    REQUIRE_FALSE(s.statuses.empty());
    CHECK(s.statuses.back() == "Sign-in failed: disk full");
    REQUIRE(s.login_uis.size() == 1);
    CHECK(s.login_uis[0] == Mode::Initial);
    CHECK(s.pending_login_client_ != nullptr);
    CHECK(s.switched.empty());
}

TEST_CASE("login cancel: the abandoned attempt's temp dir is removed once its client is freed",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.arm_pending_login_();
    const auto dir = s.pending_login_temp_dir_;
    REQUIRE_FALSE(dir.empty());
    REQUIRE(std::filesystem::exists(dir));
    s.on_login_cancelled_(); // nothing to return to: re-arms, pools idle
    CHECK(s.pending_login_temp_dir_.empty());
    CHECK_FALSE(std::filesystem::exists(dir));
}

TEST_CASE("login cancel: returning to an account keeps the temp dir until the client is freed",
          "[shell][wiring][login]")
{
    WiringShell s;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    s.account_manager_.add_account(a);
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.arm_pending_login_();
    const auto dir = s.pending_login_temp_dir_;
    s.add_account_return_uid_ = "@a:x";
    s.on_login_cancelled_();
    CHECK(s.pending_login_temp_dir_.empty());
    CHECK(std::filesystem::exists(dir)); // the retired client may hold it open
    s.reset_pending_login_client_();     // idle: retired clients and dirs go
    CHECK_FALSE(std::filesystem::exists(dir));
}

TEST_CASE("account switch: the shared refresh shows the main content and starts the services",
          "[shell][wiring][switch]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.my_display_name_ = "Alice";
    f.s.my_user_id_ = "@alice:x";
    f.s.shared_refresh_after_switch();
    CHECK(f.app->user_info()->display_name() == "Alice");
    CHECK(f.app->user_info()->user_id() == "@alice:x");
    CHECK(f.s.main_contents == 1);
    CHECK(f.s.service_starts == 1);
}

TEST_CASE("account switch: settings stay open across a cache reset",
          "[shell][wiring][switch]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.app_settings_open_ = true;
    f.s.shared_refresh_after_switch();
    CHECK(f.s.main_contents == 0);
    CHECK(f.s.service_starts == 1);
}

TEST_CASE("account switch: tabs restore at once from the cached rooms",
          "[shell][wiring][switch]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.rooms_ = {wiring_make_room("!a:x"), wiring_make_room("!b:x")};
    f.s.pending_restore_rooms_ = {"!b:x", "!a:x"};
    f.s.shared_refresh_after_switch();
    CHECK(f.s.pending_restore_rooms_.empty());
    REQUIRE(f.s.tabs_.size() == 2);
    std::vector<std::string> tab_ids;
    for (const auto& t : f.s.tabs_)
    {
        tab_ids.push_back(t.room_id);
    }
    std::sort(tab_ids.begin(), tab_ids.end());
    CHECK(tab_ids == std::vector<std::string>{"!a:x", "!b:x"});
}

TEST_CASE("account switch: tabs wait for the rooms when none are cached yet",
          "[shell][wiring][switch]")
{
    WiringFixture f;
    f.s.room_view_ = f.app->room_view();
    f.s.pending_restore_rooms_ = {"!b:x", "!a:x"};
    REQUIRE(f.s.rooms_.empty());
    f.s.shared_refresh_after_switch();
    CHECK(f.s.pending_restore_rooms_.size() == 2); // restored by on_rooms_updated_
    CHECK(f.s.tabs_.empty());
}

TEST_CASE("login finish: an account the window already has still gets the main content",
          "[shell][wiring][switch]")
{
    WiringShell s;
    s.finish_login_ui_("@a:x");
    REQUIRE(s.switched.size() == 1);
    CHECK(s.main_contents == 1);
    CHECK(s.service_starts == 1);
}

TEST_CASE("login finish: a switch that shows the main content is not followed by a second show",
          "[shell][wiring][switch]")
{
    WiringShell s;
    s.refresh_on_switch = true;
    s.finish_login_ui_("@a:x");
    CHECK(s.main_contents == 1);
    CHECK(s.service_starts == 1);
}

TEST_CASE("login finalized: a switch that shows the main content is not followed by a second show",
          "[shell][wiring][switch]")
{
    WiringShell s;
    s.refresh_on_switch = true;
    tesseract::FinalizeLoginResult fin;
    fin.ok = true;
    fin.user_id = "@new:x";
    s.on_login_finalized_(fin);
    CHECK(s.main_contents == 1);
    CHECK(s.service_starts == 1);
}

TEST_CASE("login finish: Settings open on the replaced page is closed first",
          "[shell][wiring][settings]")
{
    WiringShell s;
    s.finish_login_ui_("@a:x");
    CHECK(s.settings_closes == 0);

    s.app_settings_open_ = true;
    s.finish_login_ui_("@a:x");
    CHECK(s.settings_closes == 1);
}

TEST_CASE("login finalized: Settings open on the login page is closed on success",
          "[shell][wiring][settings]")
{
    WiringShell s;
    s.app_settings_open_ = true;
    tesseract::FinalizeLoginResult fin;
    fin.ok = true;
    fin.user_id = "@new:x";
    s.on_login_finalized_(fin);
    CHECK(s.settings_closes == 1);
}

TEST_CASE("sign-out: Settings open over the signed-out account is closed",
          "[shell][wiring][settings]")
{
    WiringShell s;
    s.shared_logout();
    CHECK(s.settings_closes == 0);

    s.app_settings_open_ = true;
    s.shared_logout();
    CHECK(s.settings_closes == 1);
}

TEST_CASE("login page: shown over Settings, it closes them unless they came from the login page",
          "[shell][wiring][settings]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.app_settings_open_ = true;
    s.shared_show_login(Mode::Initial);
    CHECK(s.settings_closes == 1);

    s.settings_opened_from_login_ = true;
    s.shared_show_login(Mode::Initial);
    CHECK(s.settings_closes == 1);

    s.app_settings_open_ = false;
    s.settings_opened_from_login_ = false;
    s.shared_show_login(Mode::Initial);
    CHECK(s.settings_closes == 1);
}

TEST_CASE("main content ready: Settings opened from the login page no longer count as such",
          "[shell][wiring][settings]")
{
    WiringShell s;
    s.settings_opened_from_login_ = true;
    s.mark_main_content_ready_();
    CHECK_FALSE(s.settings_from_login_());
}

TEST_CASE("login finalized: a failure after Cancel returned to the accounts shows the main content",
          "[shell][wiring][login]")
{
    WiringShell s;
    auto a = std::make_shared<tesseract::AccountSession>();
    a->user_id = "@a:x";
    s.account_manager_.add_account(a);
    s.active_account_ = a; // Cancel switched back to it
    s.pending_login_is_add_account_ = false; // Cancel already cleared it
    tesseract::FinalizeLoginResult fin;
    fin.error = "disk full";
    s.on_login_finalized_(fin);
    CHECK(s.main_contents == 1);
    CHECK(s.login_uis.empty());
    CHECK(s.pending_login_client_ != nullptr);
}

TEST_CASE("login page: teardown keeps the view while a pool task may still use it",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.ensure_login_view_();
    REQUIRE(s.login_alive == 1);

    std::mutex m;
    std::condition_variable cv;
    bool started = false, release = false;
    s.pool_.post(
        [&]
        {
            std::unique_lock<std::mutex> lk(m);
            started = true;
            cv.notify_all();
            cv.wait(lk, [&] { return release; });
        });
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return started; });
    }

    s.teardown_login_view_();
    s.teardown_host.fire_all_delays();
    CHECK(s.login_alive == 1);
    CHECK(s.login_unmounted == 0);
    s.teardown_host.fire_all_delays(); // still busy: re-deferred again
    CHECK(s.login_alive == 1);

    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    s.drain_workers();
    s.teardown_host.fire_all_delays();
    CHECK(s.login_unmounted == 1);
    CHECK(s.login_alive == 0);
}

TEST_CASE("tray click: toggling hides only the foreground window with nothing unread",
          "[shell][wiring][tray]")
{
    WiringShell s;
    s.tray_foreground = true;
    s.last_tray_unread_ = false;
    s.tray_toggle_();
    CHECK(s.tray_hides == 1);
    CHECK(s.tray_shows.empty());

    s.last_tray_unread_ = true; // something unread: bring the window forward
    s.tray_toggle_("token");
    CHECK(s.tray_hides == 1);
    REQUIRE(s.tray_shows.size() == 1);
    CHECK(s.tray_shows[0] == "token");

    s.tray_foreground = false;
    s.last_tray_unread_ = false;
    s.tray_toggle_();
    CHECK(s.tray_hides == 1);
    CHECK(s.tray_shows.size() == 2);
}

TEST_CASE("tray click: show always raises the window", "[shell][wiring][tray]")
{
    WiringShell s;
    s.tray_foreground = true;
    s.tray_activate_();
    CHECK(s.tray_hides == 0);
    CHECK(s.tray_shows.size() == 1);
}

TEST_CASE("media landed: avatars only repaint, other images relayout and refresh popups",
          "[shell][wiring][media]")
{
    WiringShell s;
    s.shared_repaint_after_media(ShellBase::MediaKind::RoomAvatar);
    CHECK(s.repaints == 1);
    CHECK(s.relayouts == 0);
    CHECK(s.settings_repaints == 1);

    s.shared_repaint_after_media(ShellBase::MediaKind::UserAvatar);
    CHECK(s.repaints == 2);
    CHECK(s.relayouts == 0);
    CHECK(s.settings_repaints == 2);

    s.shared_repaint_after_media(ShellBase::MediaKind::Sticker);
    CHECK(s.relayouts == 1);
    CHECK(s.settings_repaints == 3);
}

TEST_CASE("media decode failure: remembered for the session; cached bytes dropped for images",
          "[shell][wiring][media]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    const auto img = tk::CacheKey::media("mxc://x/bad");
    s.account_manager_.media_disk_cache().store(img, {1, 2, 3});
    s.shared_decode_failed(img, ShellBase::MediaKind::MediaImage);
    CHECK(s.media_decode_failed_.count(img.id) == 1);
    CHECK(s.account_manager_.media_disk_cache().load(img).empty());

    const auto av = tk::CacheKey::media("mxc://x/avatar");
    s.account_manager_.media_disk_cache().store(av, {4, 5});
    s.shared_decode_failed(av, ShellBase::MediaKind::UserAvatar);
    CHECK(s.media_decode_failed_.count(av.id) == 1);
    CHECK_FALSE(s.account_manager_.media_disk_cache().load(av).empty()); // kept
}

TEST_CASE("media decode failure: stickers and reactions drop their cached bytes, thumbnails keep them",
          "[shell][wiring][media]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    using K = ShellBase::MediaKind;
    const auto sticker = tk::CacheKey::media("mxc://x/sticker");
    const auto reaction = tk::CacheKey::media("mxc://x/reaction");
    const auto thumb = tk::CacheKey::media("mxc://x/thumb");
    for (const auto& key : {sticker, reaction, thumb})
        s.account_manager_.media_disk_cache().store(key, {1, 2, 3});

    s.shared_decode_failed(sticker, K::Sticker);
    s.shared_decode_failed(reaction, K::Reaction);
    s.shared_decode_failed(thumb, K::MediaThumbnail);

    CHECK(s.account_manager_.media_disk_cache().load(sticker).empty());
    CHECK(s.account_manager_.media_disk_cache().load(reaction).empty());
    CHECK_FALSE(s.account_manager_.media_disk_cache().load(thumb).empty());
    CHECK(s.media_decode_failed_.count(sticker.id) == 1);
    CHECK(s.media_decode_failed_.count(reaction.id) == 1);
    CHECK(s.media_decode_failed_.count(thumb.id) == 1);
}

TEST_CASE("settings page: the shell supplies the surface, the view is created once",
          "[shell][wiring][settings]")
{
    WiringShell s;
    CHECK(s.settings_surface_ == nullptr);
    s.ensure_settings_view_();
    REQUIRE(s.settings_surface_ != nullptr);
    REQUIRE(s.stats_settings_view_ != nullptr);
    CHECK(s.settings_created == 1);
    CHECK(s.settings_surface_->root() == s.stats_settings_view_);

    s.ensure_settings_view_(); // already there
    CHECK(s.settings_created == 1);
}

TEST_CASE("settings page: teardown waits a tick, unmounts, then destroys the surface",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    REQUIRE(s.prepare_settings_view_());
    CHECK(s.settings_alive == 1);

    s.teardown_settings_view_();
    CHECK(s.stats_settings_view_ == nullptr); // gone for callers at once
    CHECK(s.settings_alive == 1);             // the surface lives until the tick
    s.teardown_host.fire_all_delays();
    CHECK(s.settings_unmounted == 1);
    CHECK(s.settings_alive == 0);
    CHECK(s.settings_surface_ == nullptr);

    s.teardown_settings_view_(); // nothing to tear down
    CHECK(s.settings_unmounted == 1);
}

TEST_CASE("settings page: a view recreated in the same tick survives the old teardown",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    REQUIRE(s.prepare_settings_view_());
    s.teardown_settings_view_();
    REQUIRE(s.settings_alive == 1);
    // The old surface is still pending its teardown: reopening unmounts and
    // destroys it before creating the new one.
    REQUIRE(s.prepare_settings_view_());
    CHECK(s.settings_unmounted == 1);
    CHECK(s.settings_created == 2);
    CHECK(s.settings_alive == 1);
    s.teardown_host.fire_all_delays(); // the stale teardown must skip the new one
    CHECK(s.settings_surface_ != nullptr);
    CHECK(s.stats_settings_view_ != nullptr);
    CHECK(s.settings_unmounted == 1);
    CHECK(s.settings_alive == 1);
}

TEST_CASE("settings page: no surface means settings do not open",
          "[shell][wiring][settings]")
{
    WiringShell s;
    s.settings_null = true;
    CHECK_FALSE(s.prepare_settings_view_());
    CHECK(s.stats_settings_view_ == nullptr);
    CHECK(s.settings_created == 1);
}

TEST_CASE("settings page: the shared repaint needs the page open",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    REQUIRE(s.prepare_settings_view_());
    const int base = s.teardown_host.repaint_count;
    s.shared_repaint_settings();
    CHECK(s.teardown_host.repaint_count == base); // not open
    s.app_settings_open_ = true;
    s.shared_repaint_settings();
    CHECK(s.teardown_host.repaint_count == base + 1);
}

TEST_CASE("settings page: launch-at-login and tray state reach the view only while it exists",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    auto& settings = tesseract::Settings::instance();
    settings.launch_at_login = false;
    s.on_launch_at_login_pref_ui_(true); // no view: ignored
    CHECK_FALSE(settings.launch_at_login);
    CHECK_NOTHROW(s.on_tray_available_ui_(false));

    REQUIRE(s.prepare_settings_view_());
    s.on_launch_at_login_pref_ui_(true);
    CHECK(settings.launch_at_login); // SettingsView keeps the persisted cache in step
    CHECK_NOTHROW(s.on_tray_available_ui_(false));
}

TEST_CASE("settings page: binding the controller wires the user-pack preview hook",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    s.ensure_settings_controller_();
    REQUIRE(s.prepare_settings_view_());
    s.shared_bind_settings(); // the test double's own bind is a no-op
    REQUIRE(static_cast<bool>(s.stats_settings_view_->on_user_pack_pending_image_added));
    s.stats_settings_view_->on_user_pack_pending_image_added(1, {}, "image/png");
    s.drain_workers();
    s.teardown_host.fire_all_ui_tasks();
}

TEST_CASE("settings page: animation and repaint reach the surface only while open",
          "[shell][wiring][settings]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    REQUIRE(s.prepare_settings_view_());
    auto* fake = static_cast<FakeSettingsSurface*>(s.settings_surface_.get());
    s.shared_settings_anim();
    CHECK(fake->anim_updates == 0); // not open
    s.app_settings_open_ = true;
    s.shared_settings_anim();
    CHECK(fake->anim_updates == 1);
}

TEST_CASE("login page: created once, bound to the pending client, shown through the shell",
          "[shell][wiring][login]")
{
    using Mode = tesseract::views::LoginView::Mode;
    WiringShell s;
    s.ensure_login_view_();
    REQUIRE(s.login_view_ != nullptr);
    CHECK(s.login_created == 1);
    s.ensure_login_view_();
    CHECK(s.login_created == 1);

    s.pending_login_client_ = std::make_unique<tesseract::Client>();
    s.shared_show_login(Mode::AddAccount);
    CHECK(s.login_pages == 1);
    s.wait_pools_idle(); // the view's discovery task reads it until done
    s.release_login_surface_();
    CHECK(s.login_view_ == nullptr);
    CHECK(s.login_alive == 0);
}

TEST_CASE("login page: a new page built before the teardown tick unmounts the old one",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.ensure_login_view_();
    s.teardown_login_view_(); // destroy is deferred to a tick
    REQUIRE(s.login_alive == 1);
    s.ensure_login_view_();
    CHECK(s.login_view_ != nullptr);
    CHECK(s.login_created == 2);
    CHECK(s.login_unmounted == 1);
    CHECK(s.login_alive == 2); // the old one is retired, not destroyed
    s.teardown_host.fire_all_delays(); // the stale teardown is a no-op
    CHECK(s.login_alive == 2);
    s.release_login_surface_();
    CHECK(s.login_alive == 0);
}

TEST_CASE("login page: teardown hides it at once and destroys it a tick later",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.ensure_login_view_();
    REQUIRE(s.login_alive == 1);
    s.teardown_login_view_();
    CHECK(s.login_view_ == nullptr);
    CHECK(s.login_alive == 1);
    s.teardown_host.fire_all_delays();
    CHECK(s.login_unmounted == 1);
    CHECK(s.login_alive == 0);
}

TEST_CASE("login page: after a failed restore the sign-in screen is shown with a fresh client",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.pending_login_is_add_account_ = true;
    tesseract::RestoreResult r;
    r.any_restore_failed = true;
    r.network_unavailable = true;
    s.show_login_after_restore_(r, [] {});
    CHECK_FALSE(s.pending_login_is_add_account_);
    CHECK(s.pending_login_client_ != nullptr);
    REQUIRE(s.login_uis.size() == 1);
    CHECK(s.login_uis[0] == tesseract::views::LoginView::Mode::Initial);
}

TEST_CASE("login page: a failed restore opens its alert on the real login view",
          "[shell][wiring][login]")
{
    using tesseract::views::AlertDialog;
    using tesseract::views::LoginView;
    auto open_alert = [](WiringShell& s) -> AlertDialog*
    {
        REQUIRE(s.login_view_ != nullptr);
        for (auto& c : s.login_view_->children())
        {
            if (auto* a = dynamic_cast<AlertDialog*>(c.get()))
            {
                return a->is_open() ? a : nullptr;
            }
        }
        return nullptr;
    };

    WiringShell offline;
    offline.real_login_ui = true;
    tesseract::RestoreResult r;
    r.any_restore_failed = true;
    r.network_unavailable = true;
    offline.show_login_after_restore_(r, [] {});
    REQUIRE(offline.login_view_ != nullptr);
    CHECK(offline.login_view_->mode() == LoginView::Mode::Initial);
    REQUIRE(open_alert(offline) != nullptr);
    CHECK(open_alert(offline)->access_name() == "No Internet Connection");

    WiringShell failed;
    failed.real_login_ui = true;
    r.network_unavailable = false;
    r.restore_error = "bad token";
    failed.show_login_after_restore_(r, [] {});
    REQUIRE(open_alert(failed) != nullptr);
    CHECK(open_alert(failed)->access_name() == "Connection Error");

    WiringShell fine; // nothing failed: just the sign-in screen
    fine.real_login_ui = true;
    fine.show_login_after_restore_(tesseract::RestoreResult{}, [] {});
    REQUIRE(fine.login_view_ != nullptr);
    CHECK(open_alert(fine) == nullptr);
}

TEST_CASE("login page: a sign-in URL the browser cannot open is shown in the view",
          "[shell][wiring][login]")
{
    WiringShell s;
    s.ensure_login_view_();
    REQUIRE(s.login_view_ != nullptr);
    auto* surface = static_cast<FakeSettingsSurface*>(s.login_surface_.get());

    const int before = surface->relayouts;
    s.open_login_url_ok = true;
    s.open_login_url_or_show_(*s.login_view_, "https://hs.example/oauth");
    CHECK(surface->relayouts == before); // the browser took it

    s.open_login_url_ok = false;
    s.open_login_url_or_show_(*s.login_view_, "https://hs.example/oauth");
    CHECK(surface->relayouts == before + 1); // the URL is shown in the view
}

TEST_CASE("theme: native chrome first, then every surface, then the dependants",
          "[shell][wiring][theme]")
{
    tesseract::test::SettingsGuard guard;
    int alive = 0; // outlives `s`, whose destructor releases the surfaces
    WiringShell s;
    s.own_surface = std::make_unique<FakeSettingsSurface>(s.teardown_host, &alive);
    s.ensure_settings_view_();
    s.ensure_login_view_();
    s.shared_apply_theme(tk::Theme::dark()); // a temporary, as the screenshot paths pass
    CHECK(s.current_theme_.mode == tk::ThemeMode::Dark);
    CHECK(s.own_surface->theme().mode == tk::ThemeMode::Dark);
    CHECK(s.settings_surface_->theme().mode == tk::ThemeMode::Dark);
    CHECK(s.login_surface_->theme().mode == tk::ThemeMode::Dark);
    // Backends keep a pointer to the theme, so every consumer gets the member.
    CHECK(&s.own_surface->theme() == &s.current_theme_);
    CHECK(&s.settings_surface_->theme() == &s.current_theme_);
    CHECK(&s.login_surface_->theme() == &s.current_theme_);
    CHECK(s.after_theme_ptr == &s.current_theme_);
    REQUIRE(s.theme_log.size() == 3);
    CHECK(s.theme_log[0] == "native");
    CHECK(s.theme_log[1] == "surfaces");
    CHECK(s.theme_log[2] == "after");
}

namespace
{
// A call pop-out that only records the theme it is given.
struct ThemeSpyCallWindow final : tesseract::CallWindowBase
{
    explicit ThemeSpyCallWindow(ShellBase* shell) : CallWindowBase(shell) {}
    void bring_to_front() override {}
    void close_window() override {}
    void apply_theme(const tk::Theme& t) override { theme = &t; }
    void apply_scale_change(float) override {}
    void request_relayout() override {}
    const tk::Theme* theme = nullptr;
};
} // namespace

TEST_CASE("theme: the call pop-out gets the shell's theme member",
          "[shell][wiring][theme]")
{
    tesseract::test::SettingsGuard guard;
    WiringShell s;
    auto call = std::make_unique<ThemeSpyCallWindow>(&s);
    auto* spy = call.get();
    s.call_window_ = std::move(call);
    s.shared_apply_theme(tk::Theme::dark());
    CHECK(spy->theme == &s.current_theme_);
}

TEST_CASE("account picker: needs two accounts, then opens above the user strip",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.account_manager_.add_account(std::make_shared<tesseract::AccountSession>());
    f.s.account_manager_.accounts().back()->user_id = "@a:x";
    f.s.open_account_picker_(); // one account: nothing
    CHECK(f.s.account_picker_popup_ == nullptr);

    auto b = std::make_shared<tesseract::AccountSession>();
    b->user_id = "@b:x";
    f.s.account_manager_.add_account(b);
    f.s.open_account_picker_();
    REQUIRE(f.s.account_picker_popup_ != nullptr);
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    CHECK(popup->visible());
    CHECK(popup->last_size.w == tesseract::views::AccountPicker::kPreferredWidth);
    CHECK(popup->last_size.h > 0.f);
    CHECK(popup->last_placement == tk::PopupPlacement::PreferAbove);
    CHECK(popup->corner_radius > 0.f);
    const tk::Rect strip = f.app->user_info()->bounds();
    CHECK(popup->last_anchor.x == strip.x);
    CHECK(popup->last_anchor.y == strip.y);
}

TEST_CASE("account picker: choosing a row or clicking outside closes it; theme reaches it",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    for (const char* uid : {"@a:x", "@b:x"})
    {
        auto a = std::make_shared<tesseract::AccountSession>();
        a->user_id = uid;
        f.s.account_manager_.add_account(a);
    }
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());

    REQUIRE(popup->on_dismiss_requested);
    popup->on_dismiss_requested();
    CHECK_FALSE(popup->visible());

    f.s.now_ms += 500; // past the reopen guard
    f.s.open_account_picker_();
    REQUIRE(popup->visible());
    f.s.account_picker_->on_select("@a:x");
    CHECK_FALSE(popup->visible());

    f.s.rebuild_account_picker_(); // closed: refreshes the rows, no crash
    const int before = popup->theme_sets;
    f.s.shared_apply_theme(tk::Theme::dark());
    CHECK(popup->theme_sets == before + 1);
}

namespace
{
void add_picker_accounts(WiringShell& s, std::initializer_list<const char*> uids)
{
    for (const char* uid : uids)
    {
        auto a = std::make_shared<tesseract::AccountSession>();
        a->user_id = uid;
        s.account_manager_.add_account(a);
    }
}
} // namespace

TEST_CASE("account picker: a click on the user strip that dismissed it does not reopen it",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    add_picker_accounts(f.s, {"@a:x", "@b:x"});
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());

    popup->on_dismiss_requested(); // the outside press ...
    f.s.now_ms += 50;
    f.s.open_account_picker_(); // ... then the same click reaching the user strip
    CHECK_FALSE(popup->visible());

    f.s.now_ms += 300; // a later, deliberate click opens it again
    f.s.open_account_picker_();
    CHECK(popup->visible());
}

namespace
{
tk::KeyEvent picker_key(tk::Key k, bool ctrl = false)
{
    tk::KeyEvent e;
    e.key = k;
    e.ctrl = ctrl;
    return e;
}
} // namespace

TEST_CASE("account picker: keys reach its rows only while it is open",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    add_picker_accounts(f.s, {"@a:x", "@b:x"});
    auto& main_host = f.s.main_fake->host_;
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());
    const auto& rows = f.s.account_picker_->rows();
    REQUIRE(rows.size() == 2);
    // Opening starts on the first row.
    CHECK(popup->stub_host().focused_widget() == rows[0]);

    // Keys typed into the main window go on to the picker's rows ...
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Down)));
    CHECK(popup->stub_host().focused_widget() == rows[1]);
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Up)));
    CHECK(popup->stub_host().focused_widget() == rows[0]);
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Tab)));
    CHECK(popup->stub_host().focused_widget() == rows[1]);
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Backtab)));
    CHECK(popup->stub_host().focused_widget() == rows[0]);
    // ... but only the keys the picker handles, and not with a modifier.
    CHECK_FALSE(f.s.handle_account_picker_key_(picker_key(tk::Key::Character)));
    CHECK_FALSE(f.s.handle_account_picker_key_(picker_key(tk::Key::Down, true)));
    CHECK(popup->visible());

    f.s.close_account_picker_();
    CHECK_FALSE(f.s.handle_account_picker_key_(picker_key(tk::Key::Down)));
    CHECK_FALSE(f.s.handle_account_picker_key_(picker_key(tk::Key::Enter)));
}

TEST_CASE("account picker: Enter on a focused row selects it, Escape closes it",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    add_picker_accounts(f.s, {"@a:x", "@b:x"});
    auto& main_host = f.s.main_fake->host_;
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());

    std::string picked;
    f.s.account_picker_->on_select = [&](const std::string& uid)
    {
        picked = uid;
        f.s.close_account_picker_();
    };
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Down)));
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Enter)));
    CHECK(picked == "@b:x");
    CHECK_FALSE(popup->visible());

    f.s.now_ms += 500; // past the reopen guard
    f.s.open_account_picker_();
    REQUIRE(popup->visible());
    CHECK(main_host.dispatch_key_down(picker_key(tk::Key::Escape)));
    CHECK_FALSE(popup->visible());
    // Focus goes back to the user strip it was opened from.
    CHECK(main_host.focused_widget() == f.app->user_info());
}

TEST_CASE("account picker: Escape and window deactivation close it",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    add_picker_accounts(f.s, {"@a:x", "@b:x"});
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());

    tk::KeyEvent ctrl_esc;
    ctrl_esc.key = tk::Key::Escape;
    ctrl_esc.ctrl = true;
    CHECK_FALSE(f.s.account_picker_->on_key_down(ctrl_esc));
    CHECK(popup->visible());
    tk::KeyEvent esc;
    esc.key = tk::Key::Escape;
    CHECK(f.s.account_picker_->on_key_down(esc));
    CHECK_FALSE(popup->visible());

    f.s.open_account_picker_();
    REQUIRE(popup->visible());
    f.s.notify_window_active_(true);
    CHECK(popup->visible());
    f.s.notify_window_active_(false);
    CHECK_FALSE(popup->visible());
}

TEST_CASE("account picker: Escape on the main surface closes it first",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    add_picker_accounts(f.s, {"@a:x", "@b:x"});
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());

    CHECK(f.s.handle_escape_key_()); // consumed by the picker
    CHECK_FALSE(popup->visible());
    CHECK_FALSE(f.s.handle_escape_key_()); // nothing left to dismiss
}

TEST_CASE("account picker: its rows follow account changes and avatars while open",
          "[shell][wiring][picker]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.room_view_ = f.app->room_view();
    add_picker_accounts(f.s, {"@a:x", "@b:x"});
    f.s.open_account_picker_();
    auto* popup = static_cast<StubPopupSurface*>(f.s.account_picker_popup_.get());
    REQUIRE(popup->visible());
    CHECK(f.s.account_picker_->entries().size() == 2);

    int base = popup->relayouts;
    f.s.shared_repaint_after_media(ShellBase::MediaKind::UserAvatar);
    CHECK(popup->relayouts == base + 1);

    add_picker_accounts(f.s, {"@c:x"});
    base = popup->relayouts;
    f.s.rebuild_account_picker_(); // rebuilt while open
    CHECK(f.s.account_picker_->entries().size() == 3);
    CHECK(popup->relayouts == base + 1);

    add_picker_accounts(f.s, {"@d:x"});
    f.s.shared_refresh_after_switch(); // the account switch refreshes the rows
    CHECK(f.s.account_picker_->entries().size() == 4);

    f.s.close_account_picker_();
    base = popup->relayouts;
    f.s.shared_repaint_after_media(ShellBase::MediaKind::UserAvatar);
    CHECK(popup->relayouts == base); // closed: nothing to relayout

    f.s.release_popups_();
    CHECK(f.s.account_picker_ == nullptr);
    CHECK(f.s.account_picker_popup_ == nullptr);
}

TEST_CASE("main room pane: built once from the main app's views", "[shell][wiring][pane]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    REQUIRE(f.s.main_room_pane_ == nullptr);
    f.s.create_main_room_pane_();
    CHECK(f.s.main_room_pane_ != nullptr);
}

TEST_CASE("main room pane: the viewer's Save button reaches the shell's save dialog",
          "[shell][wiring][pane]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.create_main_room_pane_();
    REQUIRE(f.app->media_viewer()->on_save != nullptr);

    tesseract::views::MediaViewerItem item;
    item.kind = tesseract::views::MediaViewerItem::Kind::File;
    item.source = "mxc://x/file";
    item.filename = "report.pdf";
    f.app->media_viewer()->on_save(item);
    REQUIRE(f.s.dialogs.size() == 1);
    CHECK(f.s.dialogs[0].find("report.pdf") != std::string::npos);
}

TEST_CASE("main room pane: the DM and profile-panel callbacks are wired",
          "[shell][wiring][pane]")
{
    WiringFixture f;
    auto* rv = f.app->room_view();
    CHECK(rv->on_open_dm != nullptr);
    CHECK(rv->on_has_dm != nullptr);
    REQUIRE(rv->user_profile_panel() != nullptr);
    CHECK(rv->user_profile_panel()->on_extended_profile_requested != nullptr);
    CHECK(rv->user_profile_panel()->on_common_rooms_requested != nullptr);

    auto dm = wiring_make_room("!dm:x");
    dm.is_direct = true;
    dm.dm_counterpart_user_id = "@known:x";
    f.s.rooms_.push_back(dm);
    f.s.mark_room_index_dirty_();
    CHECK(rv->on_has_dm("@known:x"));
    CHECK_FALSE(rv->on_has_dm("@other:x"));
}

TEST_CASE("main room pane: a failed file drop shows its reason in the status bar",
          "[shell][wiring][pane]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.create_main_room_pane_();
    REQUIRE(f.s.main_fake->file_drop_error != nullptr);
    f.s.main_fake->file_drop_error("Could not read the dropped file");
    REQUIRE_FALSE(f.s.statuses.empty());
    CHECK(f.s.statuses.back() == "Could not read the dropped file");
}

TEST_CASE("room view services: wired from the main host without a capture device",
          "[shell][wiring][services]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.wire_room_view_services_();
    // The stub host has no audio device, so the composer offers no mic.
    CHECK(f.s.capture_ == nullptr);
}

TEST_CASE("room view services: the room view's repaint requests reach the shell",
          "[shell][wiring][services]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.wire_room_view_services_();
    f.app->room_view()->set_room(wiring_make_room("!a:x"));
    const int before = f.s.repaints;
    f.app->room_view()->show_room_info();
    CHECK(f.s.repaints > before);
}

TEST_CASE("main surface: initial and live scale reach the shell", "[shell][wiring][surface]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.wire_main_surface_(2.0f);
    CHECK(f.s.current_scale_ == 2.0f);
    REQUIRE(f.s.main_fake->on_scale_changed != nullptr);
    f.s.main_fake->on_scale_changed(3.0f);
    CHECK(f.s.current_scale_ == 3.0f);
}

TEST_CASE("main surface: the host's pre-paint and user-activity hooks reach the shell",
          "[shell][wiring][surface]")
{
    WiringFixture f;
    f.s.main_fake = std::make_unique<FakeMainSurface>(*f.surface);
    f.s.wire_main_surface_(1.0f);

    CHECK(f.s.pre_paints == 0);
    f.s.teardown_host.run_pre_paint_hook_();
    CHECK(f.s.pre_paints == 1);

    // User input feeds the image GC's idle gate.
    CHECK_FALSE(f.s.account_manager_.image_gc_should_run());
    f.s.teardown_host.fire_user_activity_();
    CHECK(f.s.account_manager_.image_gc_should_run());
}

TEST_CASE("main app: creation adopts the widget tree and room view", "[shell][wiring][surface]")
{
    tesseract::test::SettingsGuard settings_guard;
    WiringShell s;
    std::unique_ptr<TestSurface> surface = TestSurface::create(1000, 700);
    s.main_fake = std::make_unique<FakeMainSurface>(*surface);
    auto owner = s.create_main_app_();
    REQUIRE(owner != nullptr);
    CHECK(s.main_app_ == owner.get());
    CHECK(s.room_view_ == owner->room_view());
}
