#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <filesystem>
#include <string_view>

// Runtime-mutable application settings. Today every field is hardcoded to
// the historical visual default; a future settings dialog will mutate this
// singleton in place (on the UI thread). Read sites should always go
// through Settings::instance() so the dialog has one place to write.
//
// Font sizes here correspond 1:1 with tk::FontRole values; reaction-chip
// sizes are read by MessageListView and the per-platform shells.

namespace tesseract
{

class Settings
{
public:
    static Settings& instance()
    {
        static Settings s;
        return s;
    }

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    Settings(Settings&&) = delete;
    Settings& operator=(Settings&&) = delete;

    // ── Font sizes (pt) — one per tk::FontRole ────────────────────────
    int font_small = 8;
    int font_body = 12;
    int font_sender_name = 11;
    int font_timestamp = 9;
    int font_sidebar_name = 12;
    int font_sidebar_preview = 10;
    int font_unread_badge = 10;
    int font_title = 14;
    int font_ui_semibold = 10;
    int font_big_emoji = 24;         // 2× body — emoji-only messages
    int font_emoji_picker_cell = 17; // emoji picker grid (≈ 1.2× title)

    // ── Reaction chip ────────────────────────────────────────────────
    int reaction_chip_height = 24;
    int reaction_chip_gap = 6;
    int reaction_chip_pad_x = 6;

    // ── Message grouping ─────────────────────────────────────────────
    // Consecutive messages from the same sender within this window
    // suppress the repeated avatar + sender name (continuation rows).
    // Set to 0 to disable grouping entirely.
    int message_group_interval_s = 300;

    // ── Timeline ──────────────────────────────────────────────────────
    // Show room membership-change rows (join/leave/kick/ban/invite/knock/…)
    // in the message timeline, grouped by consecutive same-action runs.
    // Default off.
    bool show_room_join_leave_events = false;

    // Draw each sender's MSC4426 status emoji after their name in the
    // timeline (hover shows the status text). Default off.
    bool show_sender_status_in_timeline = false;

    // Play animated avatars (GIF / animated WebP / APNG) instead of showing
    // their first frame. Not applied while low power mode is active. Default on.
    bool animate_avatars = true;

    // ── Message layout ──────────────────────────────────────────────
    // How the timeline arranges message rows. Live-applied.
    //   Classic → avatar in the left gutter, one left-aligned column (default).
    //   Bubbles → own messages align right in a subtle rounded bubble (avatar
    //             dropped); other messages keep the left avatar column with a
    //             faint bubble behind the body.
    //   Irc     → mIRC look-alike: one flat monospaced left-aligned column,
    //             every line prefixed "[HH:MM] <nick>", per-nick colours, no
    //             avatars or bubbles.
    enum class MessageLayout
    {
        Classic,
        Bubbles,
        Irc
    };
    MessageLayout message_layout = MessageLayout::Classic;

    // ── Read state ───────────────────────────────────────────────────
    // Delay (ms) after selecting a room before it is marked as read.
    // Prevents spurious receipts when flipping through rooms quickly.
    int mark_as_read_delay_ms = 2000;

    // ── Image send ───────────────────────────────────────────────────
    // Image-sending quality. Read by the per-platform shell on each
    // image send to decide whether to re-encode before upload.
    //   Compressed → cap to 1600×1200 (keep aspect ratio), re-encode
    //                as image/jpeg quality 75.
    //   Unmodified → pass clipboard bytes through unchanged.
    enum class ImageQuality
    {
        Compressed,
        Unmodified
    };
    ImageQuality image_quality = ImageQuality::Compressed;

    // ── Theme preference ─────────────────────────────────────────────
    // System → follow the OS light/dark setting (default).
    // Light / Dark → override the OS preference.
    enum class ThemePreference
    {
        Light,
        Dark,
        System
    };
    ThemePreference theme_pref = ThemePreference::System;

    // ── Theme accent ─────────────────────────────────────────────────
    // Independent of theme_pref (light/dark/system) — selects which named
    // accent-color variant tk::Theme::variant() resolves to.
    // System → on a platform that can read one (currently only Windows,
    //          via the OS accent color), match it; elsewhere behaves like
    //          Blue. Default, so settings files predating this field (no
    //          "theme_accent" key) keep today's live behavior unchanged —
    //          notably, Windows already always matched the OS accent color
    //          before this field existed.
    enum class ThemeAccent
    {
        Blue,
        Forest,
        Sunset,
        Violet,
        System
    };
    ThemeAccent theme_accent = ThemeAccent::System;

    // ── Low power mode ───────────────────────────────────────────────
    // Auto  → active while the machine is on battery or the OS energy-saver
    //         profile is on (default).
    // On    → always active.
    // Off   → never active.
    // When active, the shell suspends background backfill, unread prefetch,
    // bridge-status checks and the decoded-image GC timer, and the SDK pauses
    // the search-index crawl and per-room warm-check pagination. Message sync
    // and encryption are never affected. Read by ShellBase's PowerPolicy.
    enum class LowPowerPreference
    {
        Auto,
        On,
        Off
    };
    LowPowerPreference low_power_pref = LowPowerPreference::Auto;

    // ── Language preference ──────────────────────────────────────────────
    // "auto" → derive locale from the OS at startup (default).
    // Any other value → explicit BCP47-style code, e.g. "en", "es".
    // Changes take effect after restart.
    std::string language = "auto";

    // ── GIF picker ────────────────────────────────────────────────────
    // Klipy API key used by the `/gif <query>` picker. Empty disables search
    // (the picker shows a "no API key configured" status). A built-in default
    // can be baked in here; users may override it via app_settings.json.
    //
    // NOTE: the embedded default below is intentional, not a leaked secret.
    // It is a free-tier, client-distributed GIF search key (same class as the
    // Giphy/Tenor SDK keys every chat client ships embedded), scoped to GIF
    // search under a hashed per-user customer_id — it grants no account access
    // or data. Worst case on extraction is free-tier quota abuse. Rotate by
    // replacing this constant. Automated secret scanners flag it; that is
    // expected and accepted.
    std::string gif_api_key = "fk7SzdJXhLgp4XwCaX7w8Yo9xOdtngpfPsoO8Dp1MknHYupTZGDwTivyiVioZe39";

    // ── Notifications ─────────────────────────────────────────────────
    // Whether to show desktop notifications for new messages (default: on).
    bool notifications_enabled = true;

    // Whether image/sticker messages embed a picture preview in the
    // notification (default: on). Independent of the lock-screen privacy
    // gate, which always suppresses the picture while the screen is locked
    // regardless of this setting.
    bool notification_image_previews = true;

    // Redact all identifying content from notifications: title becomes the
    // app name, body becomes a generic "New message", and avatar/image bytes
    // are cleared. Useful for screensharing or shared screens.
    bool notification_hide_content = false;

    // ── Media loading ────────────────────────────────────────────────
    // When true, full-resolution images and stickers are pre-fetched as rows
    // scroll into view so the media viewer opens instantly. When false (the
    // default), full media is fetched on demand when the viewer is opened.
    bool prefetch_full_media = false;

    // When true, sending a composed message that consists ONLY of a
    // recognized Google Maps / OpenStreetMap URL (direct-coordinate or
    // shortlink) sends an `m.location` event instead of plain text.
    // Shortlinks require a best-effort HTTP redirect-follow before send;
    // failure/timeout falls back to plain text silently. Off by default
    // since resolving a shortlink means an outbound network request the
    // user hasn't explicitly initiated.
    bool send_maps_urls_as_location = false;

    // When true, outgoing text messages (sends, replies, thread messages,
    // edits) carry MSC4095 bundled previews of their http(s) links, fetched
    // via the homeserver's /preview_url before send. On by default: the
    // homeserver already previews the user's own links for display, and
    // bundling spares recipients' homeservers from learning the URL. Can
    // delay sends that contain links by a few seconds.
    bool send_bundled_url_previews = true;

    // Only honoured with send_bundled_url_previews: fetch the linked pages
    // from Tesseract itself (SSRF-guarded) instead of the homeserver,
    // falling back to the homeserver on failure. Off by default since it
    // exposes the user's IP address to every site they link.
    bool fetch_url_previews_directly = false;

    // MSC4153 "exclude insecure devices": share room keys only with devices
    // their owner cross-signed, and hide messages sent from devices that
    // aren't. Off by default while other clients catch up on cross-signing.
    // Applied via Client::set_exclude_insecure_devices at launch; a change
    // takes effect after restart.
    bool exclude_insecure_devices = false;

    // ── Network proxy ─────────────────────────────────────────────────
    // System → OS/environment proxy (default); None → never use a proxy;
    // Manual → proxy_url (`http(s)://[user:pass@]host:port`). Applied via
    // Client::set_proxy at launch; a change takes effect after restart.
    // Credentials in proxy_url are stored in plain text in app_settings.json.
    enum class ProxyMode
    {
        System,
        None,
        Manual
    };
    ProxyMode proxy_mode = ProxyMode::System;
    std::string proxy_url;

    // ── MSC4278 media-preview controls ────────────────────────────────
    // In-memory mirror of the active account's global `m.media_preview_config`
    // account-data event. NOT persisted to app_settings.json — account_data is
    // the source of truth; the shell populates these on first sync and on
    // on_media_preview_config_updated, and writes changes back via the SDK.
    // Read synchronously by the message-list and invite render paths.
    //   media_previews: Off = never auto-load media; Private = only in
    //     non-public rooms; On = always (the MSC default).
    //   invite_avatars: show room/inviter avatars on pending invites.
    enum class MediaPreviews
    {
        Off,
        Private,
        On
    };
    MediaPreviews media_previews = MediaPreviews::On;
    bool          invite_avatars = true;

    // ── Window geometry ───────────────────────────────────────────────────
    // Persisted locally (not synced) so windows reopen at the same position
    // and size. valid=false means "no saved geometry; use platform default".
    struct WindowGeometry
    {
        int  x = 0, y = 0, w = 0, h = 0;
        int  dpi   = 0; // 0 = unknown (written before this field existed)
        bool valid = false;
    };

    struct PopoutEntry
    {
        std::string    room_id;
        std::string    user_id;   // empty = pre-migration entry (restore for any account)
        WindowGeometry geometry;
    };

    WindowGeometry           main_window_geometry;
    std::vector<PopoutEntry> popout_windows;

    // ── Encryption reminder ──────────────────────────────────────────
    // user_id → unix seconds until which the "set up recovery / unlock this
    // device" reminder strip (and the automatic encryption dialog) stay
    // hidden. Device-local on purpose: it's this device that's locked.
    std::map<std::string, std::int64_t> encryption_reminder_snoozed_until;
    // Accounts whose recovery key Tesseract created by itself and is holding
    // in the OS keychain (SecretStore::load_recovery_key) until the user has
    // saved it; the reminder strip asks them to until then.
    std::set<std::string> recovery_key_unsaved;
    // user_id → how many times the "save your recovery key" reminder was
    // dismissed; picks the next, longer snooze
    // (EncryptionFlowController::save_key_snooze_seconds).
    std::map<std::string, int> save_key_reminder_dismissals;
    // user_id → unix seconds until which the "save your recovery key" strip
    // stays hidden. Separate from encryption_reminder_snoozed_until so
    // dismissing it never hides a more urgent reminder.
    std::map<std::string, std::int64_t> save_key_reminder_snoozed_until;
    // Accounts whose user turned backup off after Tesseract set it up
    // silently: never set it up silently again (the user can still do it
    // from the "set up recovery" reminder).
    std::set<std::string> silent_recovery_declined;

    // ── Room list ─────────────────────────────────────────────────────
    // Group rooms with no activity for `inactive_room_threshold_days` into a
    // separate "Inactive" room-list section (DMs + Rooms only). Default off.
    bool group_inactive_rooms = false;
    int inactive_room_threshold_days = 30;

    // Group rooms with any visible unread indicator (notification, highlight, or
    // quiet unread) into a separate "Unread" section at the top of the room list,
    // above Favorites. Rooms in this section are suppressed from their normal
    // section. Default off.
    bool group_unread_rooms = false;

    // When a room (or a space whose child rooms) receives new messages, scroll
    // the most-recent unread room into view in the room list. Default on.
    bool autoscroll_unread_rooms = true;

    // Proactively warm the SDK event cache for rooms with quiet unread messages
    // (unread, not muted) so opening them renders from cache instantly. One-shot
    // per relevant change, capped + LRU, bounded concurrency. Default on.
    bool prefetch_unread_rooms = true;

    // Collapsed state of each room-list section; persisted across restarts.
    // Defaults match the hardcoded initial state in RoomListView.
    bool room_section_invites_collapsed         = false;
    bool room_section_favorites_collapsed       = false;
    bool room_section_dms_collapsed             = false;
    bool room_section_rooms_collapsed           = false;
    bool room_section_spaces_collapsed          = false;
    bool room_section_inactive_collapsed        = true;
    bool room_section_unread_collapsed          = false;
    bool room_section_space_unjoined_collapsed  = false;

    // User-chosen width of the room-list sidebar, in logical px. 0 = unset →
    // fall back to visual::kSidebarWidth. When sidebar_collapsed is true the
    // sidebar renders in icon-only mode regardless of this value (which is
    // retained as the width to restore when it is expanded again).
    int  sidebar_width     = 0;
    bool sidebar_collapsed = false;

    // ── General ───────────────────────────────────────────────────────
    // Register/unregister the app with the OS login-item mechanism so it
    // launches on boot. This field is a bookkeeping cache for early-startup
    // gating (checked before any platform IAutostart exists); the checkbox
    // in Settings → General reads actual OS state via IAutostart::is_enabled()
    // instead of trusting this value, so the two may briefly diverge if the
    // OS registration itself fails or is removed outside the app. Default off.
    bool launch_at_login = false;

    // What closing the main window does. Only consulted by the window that
    // owns the app-wide tray icon — spawned per-account windows always close
    // for real. Shells fall back to a real quit when HideToTray is selected
    // but no tray icon exists (ITrayIcon::is_available() is false, e.g. Linux
    // without a StatusNotifierItem host, or before an account is signed in),
    // so the app can never be stranded with no way to reach it.
    enum class CloseAction
    {
        // Hide the window; the app keeps running in the tray. This is the
        // historical behaviour (FEATURES.md: "minimize-to-tray (default)").
        HideToTray,
        // Minimise to the taskbar / dock, leaving a reachable button. Note
        // this describes what *close* does — the minimise button itself is
        // never hijacked into hiding to the tray.
        Minimize,
        // Destroy the window; the app quits once the last window is gone.
        Quit
    };
    CloseAction close_action = CloseAction::HideToTray;

    // Start every launch — icon, taskbar or OS login item — with the window
    // hidden in the tray rather than showing it. Independent of close_action:
    // with HideToTray this makes a tray-resident app; with Quit it gives a
    // window that appears on launch and really exits when closed.
    //
    // Off by default, so enabling an OS login item no longer silently hides
    // the app: a login launch only stays hidden when the user has asked for
    // this. The --hidden/--minimized flag (an explicit per-launch request)
    // and --autostart (an OS-started launch) still force a hidden window.
    bool start_minimized = false;

    // ── Privacy ───────────────────────────────────────────────────────
    // When false, the app neither publishes its own presence status to the
    // server nor polls other users' presence. Default on.
    bool send_presence = true;

    // Build a local full-text search index of decrypted message bodies so that
    // messages — including those in encrypted rooms — can be searched. Stores
    // decrypted plaintext on disk (in the per-account search_index.db), so it is
    // opt-in and OFF by default. Toggling drives Client::set_search_indexing_
    // enabled(): enabling lazily backfills history, disabling clears the index.
    bool index_messages_for_search = false;

    // When true, check for a newer release (GitHub or AUR, per build config)
    // once per session on sync-ready. Default on. No-op when neither
    // TESSERACT_GITHUB_REPO nor TESSERACT_AUR_PACKAGE is set at build time.
    bool check_for_updates = true;

    // ── Advanced ─────────────────────────────────────────────────────
    // MSC2545 image-pack historical compatibility. When true (default), reads/
    // writes both unstable (im.ponies.*) and stable (m.image_pack*) event-type
    // names for room image packs and the emote-rooms subscription list, and
    // loads the user's personal pack (im.ponies.user_emotes, no stable name
    // exists). OFF is a stricter mode: stable-name-only, personal pack not
    // loaded. Surfaced in Settings → About → Advanced (hidden tab, revealed
    // via the "Advanced" button since it's a compatibility knob most users
    // never need). Drives Client::set_msc2545_legacy_compat().
    bool msc2545_legacy_compat = true;

    // Surfaced in Settings → About → Advanced → Developer. Off by default;
    // no behavior gated on this yet — a foundation flag future
    // developer-only features can check.
    bool developer_mode = false;

    // When true, install a native crash handler + Rust panic hook
    // (tesseract::install_crash_handler / set_crash_reporting_enabled) that
    // write a local, plain-text crash report (stack trace + basic metadata)
    // to data_dir()/crashes/ if the app crashes. Nothing is transmitted
    // anywhere — the file just sits on disk for the user to find and attach
    // to a bug report if they choose. Off by default. Surfaced in
    // Settings → About → Advanced → Diagnostics.
    bool crash_reporting_enabled = false;

    // Minimum log level forwarded by the Rust/matrix-sdk tracing subscriber.
    // Accepted values: "error", "warn", "info", "debug", "trace".
    // Overridden at runtime by the RUST_LOG environment variable.
    // Default "warn" suppresses routine INFO chatter from the SDK.
    std::string sdk_log_level = "warn";

    // ── Call overlay ─────────────────────────────────────────────────
    enum class CallOverlayMode { Docked, DockedExpanded, Floating, Popout };
    CallOverlayMode call_overlay_mode = CallOverlayMode::Docked;
    float call_overlay_float_x = 40.0f;
    float call_overlay_float_y = 40.0f;

    // ── Audio / video device selection ───────────────────────────────────────
    // Platform-specific device IDs for microphone, speaker, and camera.
    // Empty string means "use the system default" on all platforms.
    // Populated by the Audio & Video settings section; applied at the start
    // of each capture/playback session (not hot-swapped on a running session).
    std::string audio_input_device_id;
    std::string audio_output_device_id;
    std::string camera_device_id;

    // Persist / restore settings in <config_dir>/app_settings.json.
    // load_from_disk is a no-op when the file is missing.
    // save_to_disk creates the directory if needed.
    void load_from_disk(const std::filesystem::path& config_dir);
    void save_to_disk(const std::filesystem::path& config_dir) const;

private:
    Settings() = default;
};

// Persisted spelling of Settings::CloseAction: "tray", "minimize" or "quit".
// Shared by the settings serializer and the Settings → General dropdown, so
// the on-disk key and the combo's selected value cannot drift apart.
const char* close_action_to_string(Settings::CloseAction action);

// Parse a persisted "close_action". Anything unrecognised — an unknown value,
// or one written by a newer build — maps to HideToTray, the field default,
// so a hand-edited or older settings.json can never make closing the window
// start killing the app.
Settings::CloseAction close_action_from_string(std::string_view value);

} // namespace tesseract
