#pragma once

// Runtime-mutable application settings. Today every field is hardcoded to
// the historical visual default; a future settings dialog will mutate this
// singleton in place (on the UI thread). Read sites should always go
// through Settings::instance() so the dialog has one place to write.
//
// Font sizes here correspond 1:1 with tk::FontRole values; reaction-chip
// sizes are read by MessageListView and the per-platform shells.

namespace tesseract {

class Settings {
public:
    static Settings& instance() {
        static Settings s;
        return s;
    }

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    Settings(Settings&&) = delete;
    Settings& operator=(Settings&&) = delete;

    // ── Font sizes (pt) — one per tk::FontRole ────────────────────────
    int font_small           =  8;
    int font_body            = 12;
    int font_sender_name     = 11;
    int font_timestamp       =  9;
    int font_sidebar_name    = 12;
    int font_sidebar_preview = 10;
    int font_unread_badge    = 10;
    int font_title           = 14;
    int font_ui_semibold     = 10;

    // ── Reaction chip ────────────────────────────────────────────────
    int reaction_chip_height = 28;
    int reaction_chip_gap    =  6;

    // ── Message grouping ─────────────────────────────────────────────
    // Consecutive messages from the same sender within this window
    // suppress the repeated avatar + sender name (continuation rows).
    // Set to 0 to disable grouping entirely.
    int message_group_interval_s = 60;

    // ── Image send ───────────────────────────────────────────────────
    // Image-sending quality. Read by the per-platform shell on each
    // image send to decide whether to re-encode before upload.
    //   Compressed → cap to 1600×1200 (keep aspect ratio), re-encode
    //                as image/jpeg quality 75.
    //   Unmodified → pass clipboard bytes through unchanged.
    enum class ImageQuality { Compressed, Unmodified };
    ImageQuality image_quality = ImageQuality::Compressed;

private:
    Settings() = default;
};

} // namespace tesseract
