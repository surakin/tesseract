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
    int message_group_interval_s = 60;

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
