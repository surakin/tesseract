#pragma once

#include <filesystem>

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
    int reaction_chip_height = 28;
    int reaction_chip_gap = 6;

    // ── Message grouping ─────────────────────────────────────────────
    // Consecutive messages from the same sender within this window
    // suppress the repeated avatar + sender name (continuation rows).
    // Set to 0 to disable grouping entirely.
    int message_group_interval_s = 300;

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
    // scroll into view so ImageViewerOverlay opens instantly. When false (the
    // default), full media is fetched on demand when the viewer is opened.
    bool prefetch_full_media = false;

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

    // ── Room list ─────────────────────────────────────────────────────
    // Group rooms with no activity for `inactive_room_threshold_days` into a
    // separate "Inactive" room-list section (DMs + Rooms only). Default off.
    bool group_inactive_rooms = false;
    int inactive_room_threshold_days = 30;

    // Collapsed state of each room-list section; persisted across restarts.
    // Defaults match the hardcoded initial state in RoomListView.
    bool room_section_invites_collapsed   = false;
    bool room_section_favorites_collapsed = false;
    bool room_section_dms_collapsed       = false;
    bool room_section_rooms_collapsed     = false;
    bool room_section_spaces_collapsed    = false;
    bool room_section_inactive_collapsed  = true;

    // ── Privacy ───────────────────────────────────────────────────────
    // When false, the app neither publishes its own presence status to the
    // server nor polls other users' presence. Default on.
    bool send_presence = true;

    // Persist / restore settings in <config_dir>/app_settings.json.
    // load_from_disk is a no-op when the file is missing.
    // save_to_disk creates the directory if needed.
    void load_from_disk(const std::filesystem::path& config_dir);
    void save_to_disk(const std::filesystem::path& config_dir) const;

private:
    Settings() = default;
};

} // namespace tesseract
