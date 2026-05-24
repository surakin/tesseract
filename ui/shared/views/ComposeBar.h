#pragma once

// Shared compose bar. A bottom-anchored strip with:
//   - optional image preview band (top, when a clipboard image is pending)
//   - emoji button (left of the input)
//   - text input area (host overlays a NativeTextArea on text_area_rect())
//   - send button (right of the input)
//
// The widget paints the background, separator, buttons, and pending-image
// thumbnail; the host is responsible for mounting a tk::NativeTextArea at
// text_area_rect() so IME / selection / undo stay native. The widget
// auto-grows between kMinHeight and kMaxHeight based on the natural
// content height reported by the host's NativeTextArea, and grows further
// to accommodate the preview band when an image is attached.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tesseract::views
{

/// Metadata extracted from a dropped video or audio file on a background
/// thread. Passed to ComposeBar::update_pending_attachment() on the UI thread
/// once extraction completes.
struct MediaInfo
{
    std::vector<std::uint8_t> thumb_bytes; // JPEG first frame; empty for audio/gif
    std::uint32_t thumb_w = 0, thumb_h = 0;
    std::uint32_t video_w = 0, video_h = 0;
    std::uint64_t duration_ms = 0;
    bool is_animated = false;   // gif/webp animation flag
    // Generation token: set to ComposeBar::pending_gen() immediately after
    // set_pending_*() and checked in update_pending_attachment() to discard
    // results that arrive after the user replaced or removed the attachment.
    std::uint32_t pending_gen = 0;
};

class ComposeBar : public tk::Widget
{
public:
    ComposeBar();
    ~ComposeBar() override = default;

    static constexpr float kMinHeight = 56.0f;
    static constexpr float kMaxHeight = 160.0f;
    static constexpr float kPreviewBandH = 96.0f;
    static constexpr float kFileBandH = 48.0f;
    static constexpr float kPreviewBandGap = 8.0f;
    static constexpr float kReplyBandH = 44.0f;
    static constexpr float kReplyBandGap = 4.0f;
    static constexpr float kEditBandH = 44.0f;
    static constexpr float kEditBandGap = 4.0f;

    /// Rect inside the compose bar (widget-local coordinates, same space
    /// `root->arrange` operates in) for the host to overlay a
    /// NativeTextArea. Empty when the widget hasn't been arranged yet.
    tk::Rect text_area_rect() const
    {
        return recording_ ? tk::Rect{} : text_area_rect_;
    }

    /// Host bridge: integration code pushes the latest natural height of
    /// the NativeTextArea here on every `on_height_changed` callback.
    /// The compose bar clamps to [kMinHeight, kMaxHeight] internally; the
    /// parent layout grows by re-measuring after this call.
    void set_text_area_natural_height(float h);
    float natural_height() const
    {
        return natural_height_;
    }

    /// Latest text — pushed by integration code on every `on_changed`
    /// callback from the NativeTextArea. Used to gate the send button.
    void set_current_text(std::string text);
    const std::string& current_text() const
    {
        return current_text_;
    }

    void set_enabled(bool e);
    bool enabled() const
    {
        return enabled_;
    }

    /// Hide or show the mic button. Pass false when no audio input device is
    /// detected at startup (capture_ == nullptr after make_audio_capture()).
    /// Defaults to true.
    void set_mic_available(bool available);
    bool mic_available() const { return mic_available_; }

    /// Attach an image as a pending payload. The shared widget stores
    /// the raw bytes + mime, decodes a thumbnail lazily on the next
    /// layout pass, and grows `natural_height()` to make room for the
    /// preview band. Replaces any pending attachment (image or file)
    /// already attached. Fires `on_size_changed` so the host can refresh
    /// its fixed-height envelope.
    ///
    /// `filename` is the basename the homeserver will receive in the
    /// `m.image` event's MSC2530 `filename` field. Pass an empty string
    /// for clipboard pastes (the widget synthesises
    /// `clipboard-YYYYMMDD-HHMMSS.ext`); pass the original filename for
    /// file drops so the recipient sees the real name.
    ///
    /// `is_animated` marks the payload as an animated GIF/WebP: it is
    /// forwarded verbatim through `on_send_image` so the host sends it via
    /// the MSC4230 raw path (skipping the re-encode that would flatten the
    /// animation).
    void set_pending_image(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename = {},
                           bool is_animated = false);

    /// Attach a non-image file as a pending payload. Renders as a single-
    /// line chip (paperclip + filename + size) above the input. Replaces
    /// any pending attachment (image or file) already attached.
    /// `filename` is required (no synthesis) — drag-and-drop / file-picker
    /// always supplies it.
    void set_pending_file(std::vector<std::uint8_t> bytes, std::string mime,
                          std::string filename);

    /// Attach a video as a pending payload with `loading = true`. Renders as a
    /// film-icon chip until `update_pending_attachment()` fills in the thumbnail
    /// and metadata, at which point it switches to the thumbnail-band view.
    void set_pending_video(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename);

    /// Attach an audio file as a pending payload with `loading = true`. Renders
    /// as an audio-icon chip; duration text is filled in by
    /// `update_pending_attachment()` once background extraction completes.
    void set_pending_audio(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename);

    /// Called on the UI thread after background media extraction completes.
    /// Fills in thumbnail/dimensions/duration/is_animated from `info` and
    /// clears the `loading` flag so the preview updates immediately.
    void update_pending_attachment(const MediaInfo& info);

    /// Drop any attached payload (image or file). No-op when none.
    void clear_pending();
    /// Back-compat alias — same as `clear_pending()`.
    void clear_pending_image()
    {
        clear_pending();
    }

    /// True while any attachment (image, video, audio, or file) is queued.
    bool has_pending() const
    {
        return pending_.has_value();
    }

    /// Current pending-attachment generation counter. Increment on every
    /// set_pending_* / clear_pending call. Shells capture this immediately
    /// after set_pending_* and embed it in the MediaInfo they pass to
    /// the background extractor so update_pending_attachment() can discard
    /// results that arrive after the user replaced or removed the attachment.
    std::uint32_t pending_gen() const { return pending_gen_; }
    /// Back-compat alias — same as `has_pending()`.
    bool has_pending_image() const
    {
        return pending_.has_value();
    }

    // Attachment kinds — public so pending_for_test() callers can inspect them.
    struct PendingAttachment
    {
        enum class Kind
        {
            Image,
            Video,
            Audio,
            File
        };
        Kind kind = Kind::Image;
        bool loading = false; // true while background extraction is in progress

        std::vector<std::uint8_t> bytes;
        std::string mime;
        std::string filename;

        // Image / Video: decoded preview thumbnail (lazy, from arrange()).
        std::unique_ptr<tk::Image> preview;
        std::uint32_t width = 0, height = 0;

        // Image kind: true for animated GIF/WebP.
        bool is_animated = false;

        // Image kind: non-null when decode_animated_image() decoded all frames.
        // paint() uses this in preference to `preview` when set.
        std::unique_ptr<tk::AnimatedImage> anim_preview;

        // Video kind: raw JPEG first-frame bytes passed to on_send_video.
        std::vector<std::uint8_t> thumb_bytes_raw;
        std::uint32_t thumb_width = 0, thumb_height = 0;

        // Video + Audio kinds: duration in milliseconds (0 = unknown).
        std::uint64_t duration_ms = 0;
    };

    /// Test accessor: returns a pointer to the current PendingAttachment, or
    /// nullptr when none is queued. For unit tests only.
    const PendingAttachment* pending_for_test() const
    {
        return pending_.has_value() ? &*pending_ : nullptr;
    }

    /// Execute the same dispatch as the send button: pending attachment →
    /// `on_send_image`/`on_send_file`; edit mode → `on_send_edit`; reply
    /// mode → `on_send_reply`; otherwise → `on_send`. Hosts wire both the
    /// send-button click and the NativeTextArea submit to this method so
    /// that attachments and reply/edit state are handled correctly on
    /// Enter key as well as button click.
    void trigger_send();

    /// Fires when `trigger_send()` runs in plain text mode (no attachment,
    /// no reply, no edit). The host sends the text as a plain message.
    std::function<void(const std::string&)> on_send;

    /// Fires when send runs with a pending image attached. The host
    /// receives the raw clipboard bytes, the source mime, the generated
    /// filename, the trimmed caption (may be empty), the source dimensions,
    /// the `is_animated` flag (true for animated GIF/WebP — the host must
    /// skip re-encoding and send via the MSC4230 raw path), and the
    /// reply_event_id (empty when no reply is pending). For still images the
    /// host re-encodes per `Settings::image_quality`, uploads, and posts the
    /// `m.image` event.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::uint32_t width, std::uint32_t height,
                       bool is_animated, std::string reply_event_id)>
        on_send_image;

    /// Fires when send runs with a pending non-image file attached. The
    /// host receives the raw bytes, the OS-supplied (or guessed) mime,
    /// the file's basename, the trimmed caption (may be empty), and the
    /// reply_event_id (empty when no reply is pending). The host uploads
    /// as-is and posts the `m.file` event.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::string reply_event_id)>
        on_send_file;

    /// Fires when send runs with a pending video attached. `width`/`height`
    /// are the video source dimensions; `thumb_bytes` is a JPEG first-frame
    /// thumbnail (empty when unavailable); `thumb_width`/`thumb_height` are
    /// its dimensions; `duration_ms` populates `info.duration` (0 when
    /// unknown). The host uploads via `client::send_video` and posts an
    /// `m.video` event.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::uint32_t width, std::uint32_t height,
                       std::vector<std::uint8_t> thumb_bytes,
                       std::uint32_t thumb_width, std::uint32_t thumb_height,
                       std::uint64_t duration_ms, std::string reply_event_id)>
        on_send_video;

    /// Fires when send runs with a pending audio file attached. `duration_ms`
    /// populates `info.duration` (0 when unknown). Sends a plain `m.audio`
    /// event — NOT the MSC3245 voice extension (that is for voice recordings
    /// only). The host uploads via `client::send_audio`.
    std::function<void(std::vector<std::uint8_t> bytes, std::string mime,
                       std::string filename, std::string caption,
                       std::uint64_t duration_ms, std::string reply_event_id)>
        on_send_audio;

    /// Fires when the emoji button is clicked. The rect is the button's
    /// bounding box in surface-local (world) coordinates, for precise
    /// picker anchoring.
    std::function<void(tk::Rect)> on_emoji;

    /// Fires when the sticker button is clicked. The rect is the button's
    /// bounding box in surface-local (world) coordinates, for precise
    /// picker anchoring.
    std::function<void(tk::Rect)> on_sticker;

    /// Fires when `natural_height()` may have changed due to an image
    /// being attached or detached. The host should re-apply its
    /// fixed-height envelope and re-run `relayout()`.
    std::function<void()> on_size_changed;

    /// Fired from paint() while an animated image preview is active.
    /// The shell should schedule a repaint of the compose surface after
    /// `delay_ms` milliseconds (the time until the next animation frame).
    std::function<void(int delay_ms)> on_request_anim_repaint_;

    /// Enter reply mode. Displays a "Replying to <sender_name>" banner
    /// with `body_preview` above the text input. Grows `natural_height()`
    /// by `kReplyBandH + kReplyBandGap` and fires `on_size_changed`.
    void set_reply_to(std::string event_id, std::string sender_name,
                      std::string body_preview);

    /// Exit reply mode. Clears the reply banner, shrinks `natural_height()`,
    /// and fires `on_size_changed`. No-op when not in reply mode.
    void clear_reply();
    bool has_reply() const
    {
        return !reply_event_id_.empty();
    }
    const std::string& reply_event_id() const
    {
        return reply_event_id_;
    }

    /// Fires in place of `on_send` when a reply is pending. The host should
    /// call `Client::send_reply(current_room_, reply_event_id, body)`.
    std::function<void(const std::string& reply_event_id,
                       const std::string& body)>
        on_send_reply;

    /// Enter edit mode. Displays an "Editing message" banner above the text
    /// input (replaces any active reply mode). Grows `natural_height()` by
    /// `kEditBandH + kEditBandGap` and fires `on_size_changed`.
    void set_editing(std::string event_id);

    /// Exit edit mode. Clears the edit banner, shrinks `natural_height()`,
    /// and fires `on_size_changed`. No-op when not in edit mode.
    void clear_editing();
    bool has_editing() const
    {
        return !edit_event_id_.empty();
    }
    const std::string& edit_event_id() const
    {
        return edit_event_id_;
    }

    /// Fires in place of `on_send` when edit mode is active. The host should
    /// call `Client::send_edit(current_room_, event_id, new_body)`.
    std::function<void(const std::string& event_id,
                       const std::string& new_body)>
        on_send_edit;

    /// Fires when the user cancels an in-progress edit via the "×" button
    /// in the edit banner. The host should restore the original text to the
    /// compose field and call `clear_editing()`.
    std::function<void()> on_edit_cancelled;

    // ── Voice recording state ────────────────────────────────────────────────

    /// Switch between idle and recording visual state.
    /// Transitioning idle → recording clears the amplitude history.
    void set_recording(bool recording);
    bool is_recording() const { return recording_; }

    /// Push a live amplitude sample [0, 1000] from the capture backend.
    /// No-op when not recording.
    void push_amplitude(std::uint16_t amplitude);

    /// Fires when the mic button is clicked (idle) or the stop button
    /// is clicked (recording). The shell distinguishes via AudioCapture::is_recording().
    std::function<void()> on_mic_clicked;

    /// Fires when the × cancel button is clicked during recording.
    std::function<void()> on_cancel_voice;

    /// Tooltip callbacks — wired through RoomView to the shell's native tooltip.
    std::function<void(std::string text, tk::Rect anchor)> on_show_tooltip;
    std::function<void()> on_hide_tooltip;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool contains_world(tk::Point world) const override;
    tk::Widget* hit_test(tk::Point world) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    tk::Widget* dispatch_pointer_move(tk::Point world, bool* dirty) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    void refresh_send_enabled();
    void recompute_height();
    bool point_in_remove_btn(tk::Point world) const;
    static std::string make_filename(const std::string& mime);
    // Cached layout used to paint the filename (and a second line for size or
    // duration) inside file/video/audio chips. Rebuilt lazily in arrange().
    std::unique_ptr<tk::TextLayout> file_name_layout_;
    std::unique_ptr<tk::TextLayout> file_size_layout_;
    std::string file_layout_key_; // cache key to detect staleness

    // ▶ badge painted over the video thumbnail in the preview band.
    std::unique_ptr<tk::TextLayout> video_badge_layout_;

    tk::Button* emoji_btn_ = nullptr;   // borrowed (owned by Widget tree)
    tk::Button* sticker_btn_ = nullptr; // borrowed
    tk::Button* mic_btn_ = nullptr;     // borrowed; hidden when no mic device
    tk::Button* send_btn_ = nullptr;    // borrowed
    tk::Button* remove_btn_ = nullptr;  // borrowed; hidden when no image
    // Cached SVG icons for the emoji, sticker, and mic buttons — rasterized
    // lazily on first paint via tk::rasterize_svg().
    std::unique_ptr<tk::Image> emoji_icon_;
    std::unique_ptr<tk::Image> sticker_icon_;
    std::unique_ptr<tk::Image> mic_icon_;
    std::unique_ptr<tk::Image> mic_stop_icon_;
    // × glyph for the remove-attachment button (Body size, hover-tinted).
    std::unique_ptr<tk::TextLayout> remove_layout_;
    tk::Rect text_area_rect_{};
    tk::Rect compose_card_rect_{}; // card outline wrapping text + icon buttons
    tk::Rect emoji_rect_{};
    tk::Rect sticker_rect_{};
    tk::Rect send_rect_{};
    tk::Rect preview_band_rect_{};
    tk::Rect preview_image_rect_{};
    tk::Rect remove_btn_rect_{};

    // Pre-clamp natural height reported by the NativeTextArea. Starts at
    // 0 so the initial clamp() in recompute_height() floors to kMinHeight
    // (instead of pushing kMinHeight+padding through the clamp).
    float text_area_natural_ = 0.0f;
    float natural_height_ = kMinHeight; // total (incl. preview)
    std::string current_text_;
    bool enabled_ = true;
    bool mic_available_ = true;

    std::optional<PendingAttachment> pending_;
    // Monotonically increasing counter, incremented by every set_pending_*()
    // and clear_pending(). Stored in MediaInfo.pending_gen at drop time and
    // checked in update_pending_attachment() to discard stale results.
    std::uint32_t pending_gen_ = 0;

    // Reply state. reply_event_id_ is empty when not in reply mode.
    std::string reply_event_id_;
    std::string reply_sender_name_;
    std::string reply_body_preview_;
    tk::Rect reply_band_rect_{};
    tk::Rect reply_cancel_rect_{};
    bool press_reply_cancel_ = false;

    // Edit state. edit_event_id_ is empty when not in edit mode.
    // Edit mode and reply mode are mutually exclusive.
    std::string edit_event_id_;
    tk::Rect edit_band_rect_{};
    tk::Rect edit_cancel_rect_{};
    bool press_edit_cancel_ = false;

    // Which compose button is currently showing a tooltip (None = none).
    enum class TooltipBtn { None, Emoji, Sticker, Mic };
    TooltipBtn tooltip_hover_ = TooltipBtn::None;

    // Voice recording state.
    bool recording_ = false;
    static constexpr std::size_t kMaxWaveformSamples = 80;
    std::vector<std::uint16_t> waveform_samples_;
    tk::Rect mic_btn_rect_{};
    tk::Rect waveform_strip_rect_{};
    tk::Rect voice_cancel_rect_{};
    std::unique_ptr<tk::TextLayout> elapsed_layout_;
    std::uint64_t recording_start_ms_ = 0;
    bool press_voice_cancel_ = false;
};

} // namespace tesseract::views
