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

#include "tk/access_tree.h"
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/text_area.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <memory>
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

class ComposeBar : public tk::Widget, public tk::WidgetRowAccessibility
{
protected:
    // host() is nullable: when null, text_area_ is simply not constructed —
    // lets tests that don't care about the native control default-construct
    // without a Host.
    ComposeBar();
    TK_WIDGET_FACTORY_FRIEND(ComposeBar)

public:
    // ── Accessibility ─────────────────────────────────────────────────────
    // The painted banner text ("Editing message" / "Replying to …") and the
    // pending attachment's name and size, read before the controls.
    std::size_t access_row_count() const override { return access_texts_().size(); }
    tk::Role    access_role_for_widget_row(std::size_t) const override
    {
        return tk::Role::StaticText;
    }
    std::string access_name_for_widget_row(std::size_t i) const override
    {
        const auto t = access_texts_();
        return i < t.size() ? t[i].first : std::string();
    }
    tk::Rect    access_rect_for_widget_row(std::size_t i) const override
    {
        const auto t = access_texts_();
        return i < t.size() ? t[i].second : tk::Rect{};
    }

    ~ComposeBar() override { invalidate_weak_self(); }

    static constexpr float kMinHeight = 56.0f;
    static constexpr float kMaxHeight = 160.0f;
    static constexpr float kPreviewBandH = 96.0f;
    static constexpr float kFileBandH = 48.0f;
    static constexpr float kPreviewBandGap = 8.0f;
    static constexpr float kReplyBandH = 44.0f;
    static constexpr float kReplyBandGap = 4.0f;
    static constexpr float kEditBandH = 30.0f;
    static constexpr float kEditBandGap = 4.0f;

    /// Rect inside the compose bar (widget-local coordinates, same space
    /// `root->arrange` operates in) that text_area() currently occupies, or
    /// empty while recording (or before the first arrange()). Used by
    /// RoomView/MainAppWidget to decide whether a modal or the voice-
    /// recording state should force text_area() invisible even though this
    /// widget's own arrange() already ran — see MainAppWidget::arrange().
    tk::Rect text_area_rect() const
    {
        return recording_ ? tk::Rect{} : text_area_rect_;
    }

    /// Self-owned text input control — see tk::TextArea. Non-null whenever
    /// this bar was constructed with a real Host. The 4 composer popup
    /// controllers (Gif/SlashCommand/Shortcode/Mention) are constructed by
    /// the shell against this pointer; the shell also wires its own
    /// set_on_changed/set_on_submit/push_popup_nav on it to arbitrate
    /// between those controllers — see ComposePopups.h.
    // Defined out-of-line in ComposeBar.cpp (after ComposerTextArea's full
    // definition) — the upcast from ComposerTextArea* needs the complete
    // type, which the forward declaration above doesn't provide here.
    tk::TextArea* text_area() const;

    /// The one place that asks the compose box to take keyboard focus —
    /// used by RoomView's default-focus policy (room activation, empty-
    /// canvas click) and every reply/edit/emoji/link-click flow that used
    /// to reach through text_area()->set_focused(true) directly. A single
    /// named entry point so future guard conditions ("don't steal focus
    /// while X") have exactly one place to live.
    void focus();

    /// Exposed so RoomView can register these as each picker's popup
    /// "trigger" (see tk::Host::register_popup()'s doc comment): a click on
    /// the button while its own picker is already open shouldn't
    /// dismiss-then-reopen it.
    tk::Button* emoji_button() const { return emoji_btn_; }
    tk::Button* sticker_button() const { return sticker_btn_; }

    void on_theme_changed(const tk::Theme& t) override;

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

    void set_enabled(bool e) override;

    /// Show (or clear) the send button's busy spinner, used while this
    /// room's earlier sends are still being prepared or sent. The button
    /// stays usable: further sends queue behind the pending ones.
    void set_send_busy(bool busy);
    bool send_busy() const
    {
        return send_btn_ && send_btn_->busy();
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
    /// animation) — only meaningful when this ends up the sole attachment;
    /// see trigger_send()'s doc comment for the multi-attachment caveat.
    ///
    /// Appends to the queue (does not replace); no-ops once kMaxAttachments
    /// is reached. When trigger_send() runs with exactly one item queued,
    /// behavior is unchanged from the old single-attachment API — the
    /// existing on_send_image/file/video/audio callbacks fire exactly as
    /// before. With two or more, on_send_gallery fires instead (MSC4274).
    void add_pending_image(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename = {},
                           bool is_animated = false);

    /// Attach a non-image file as a pending payload. Renders as a single-
    /// line chip (paperclip + filename + size). Appends to the queue; see
    /// add_pending_image()'s doc comment for cap/gallery-dispatch behavior.
    /// `filename` is required (no synthesis) — drag-and-drop / file-picker
    /// always supplies it.
    void add_pending_file(std::vector<std::uint8_t> bytes, std::string mime,
                          std::string filename);

    /// Attach a video as a pending payload with `loading = true`. Renders as a
    /// film-icon chip until `update_pending_attachment()` fills in the thumbnail
    /// and metadata. Appends to the queue; see add_pending_image()'s doc
    /// comment for cap/gallery-dispatch behavior.
    void add_pending_video(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename);

    /// Attach an audio file as a pending payload with `loading = true`. Renders
    /// as an audio-icon chip; duration text is filled in by
    /// `update_pending_attachment()` once background extraction completes.
    /// Appends to the queue; see add_pending_image()'s doc comment for
    /// cap/gallery-dispatch behavior.
    void add_pending_audio(std::vector<std::uint8_t> bytes, std::string mime,
                           std::string filename);

    /// Called on the UI thread after background media extraction completes.
    /// Looks up the item whose `gen` matches `info.pending_gen` (there may
    /// be several in-flight probes at once when multiple files were just
    /// dropped) and fills in its thumbnail/dimensions/duration/is_animated,
    /// clearing `loading`. No-op if the item was removed before extraction
    /// finished — never assumes "the" pending item.
    void update_pending_attachment(const MediaInfo& info);

    /// Drop every attached payload. No-op when none are queued.
    void clear_pending();

    /// Remove the attachment at `index`. No-op when out of range.
    void remove_pending(std::size_t index);

    /// True while at least one attachment (image, video, audio, or file) is queued.
    bool has_pending() const
    {
        return !pending_.empty();
    }

    /// Current pending-attachment generation counter — also the source of
    /// each new item's stable `PendingAttachment::gen`. Increments on every
    /// add_pending_* call. Shells capture this immediately after
    /// add_pending_* (it equals the just-added item's `gen`) and embed it
    /// in the MediaInfo they pass to the background extractor so
    /// update_pending_attachment() can find that specific item later.
    std::uint32_t pending_gen() const { return pending_gen_; }

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

        // Stable per-item identity, assigned at add_pending_*() time from
        // the same monotonic counter pending_gen() exposes. Lets
        // update_pending_attachment() find *this* item in the vector even
        // after earlier items were removed (indices shift; this doesn't).
        // Distinct from list *position* — never reused, never renumbered.
        std::uint32_t gen = 0;
    };

    /// Maximum number of attachments queueable at once (send_gallery_async
    /// beyond a single item). Not an MSC4274 wire limit (galleries reference
    /// already-uploaded mxc:// URLs, so even 60 items' itemtypes JSON is
    /// tiny) — it's a UI/resource bound: each queued item holds a decoded
    /// preview in memory, and a single on_upload_complete fires for the
    /// whole batch (Client::send_gallery_async), so a much higher cap risks
    /// a large batch feeling hung with no per-item feedback.
    static constexpr std::size_t kMaxAttachments = 20;

    /// Test accessor: returns a pointer to the PendingAttachment at `index`,
    /// or nullptr when out of range. For unit tests only.
    const PendingAttachment* pending_for_test(std::size_t index = 0) const
    {
        return index < pending_.size() ? &pending_[index] : nullptr;
    }

    /// Number of attachments currently queued.
    std::size_t pending_count() const { return pending_.size(); }

    /// Test accessor: world-space rect of the multi-attachment mode's
    /// per-chip × remove badge at `index` (populated by arrange() when
    /// pending_count() >= 2), or an empty rect when out of range. For unit
    /// tests only.
    tk::Rect chip_remove_rect_for_test(std::size_t index) const
    {
        return index < chip_remove_rects_.size() ? chip_remove_rects_[index]
                                                  : tk::Rect{};
    }

    /// Move every pending attachment out without sending it (e.g. to stash
    /// them as part of a per-room compose draft when leaving a room). Leaves
    /// the widget in the same state as clear_pending(). Returns an empty
    /// vector when none were queued.
    std::vector<PendingAttachment> take_pending();

    /// Re-install previously take_pending()'d attachments (e.g. restoring a
    /// per-room draft). Replaces any attachments currently queued. No-op on
    /// an empty vector.
    void restore_pending(std::vector<PendingAttachment> attachments);

    /// Execute the same dispatch as the send button: one pending attachment
    /// → the matching scalar `on_send_image`/`on_send_file`/`on_send_video`/
    /// `on_send_audio`; two or more → `on_send_gallery`; edit mode →
    /// `on_send_edit`; reply mode → `on_send_reply`; otherwise → `on_send`.
    /// Hosts wire both the send-button click and the NativeTextArea submit
    /// to this method so that attachments and reply/edit state are handled
    /// correctly on Enter key as well as button click.
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

    /// Fires when the user pastes one or more files copied from a file
    /// manager (Finder/Explorer/Nautilus/Dolphin) into the compose text
    /// area. Mirrors `RoomView::on_file_drop`, but paste has no drop point,
    /// so the host is responsible for the same upload-limit check /
    /// `route_file_drop_to_compose_bar` / outcome reporting that
    /// `on_file_drop` does per dropped file.
    std::function<void(std::vector<tk::FileDropPayload>)> on_file_paste;

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

    /// Fires when send runs with 2+ pending attachments queued (MSC4274
    /// `m.gallery`). Does NOT fire for exactly one attachment — that still
    /// goes through the scalar on_send_image/file/video/audio callbacks
    /// above, unchanged. `caption` is the one gallery-level caption from the
    /// compose text box; v1 has no per-item captions. Attachment order in
    /// the vector matches queue/display order.
    std::function<void(std::vector<PendingAttachment> items, std::string caption,
                       std::string reply_event_id)>
        on_send_gallery;

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
    // The composer's accessible label becomes "Message {room}", so landing in
    // it after a room switch tells a screen-reader user where they are.
    void set_room_name(const std::string& room_name);
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
    /// `kEditBandH + kEditBandGap` and fires `on_size_changed`. `is_caption`
    /// marks a media-caption edit (vs. a text-body edit) and is threaded
    /// through unchanged to `on_send_edit` on submit.
    void set_editing(std::string event_id, bool is_caption = false);

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
    bool editing_is_caption() const
    {
        return edit_is_caption_;
    }

    /// Fires in place of `on_send` when edit mode is active. The host should
    /// call `Client::send_edit(current_room_, event_id, new_body)` when
    /// `is_caption` is false, or `Client::send_caption_edit(current_room_,
    /// event_id, new_body)` when true.
    std::function<void(const std::string& event_id,
                       const std::string& new_body, bool is_caption)>
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
    // A tk::TextArea whose keyboard-focus ring traces the whole compose
    // card's rounded-rect outline instead of its own narrow text-column
    // bounds — see ComposeBar.cpp for the full definition and rationale.
    class ComposerTextArea;

    void refresh_send_enabled();
    void recompute_height();
    void notify_size_changed_();
    void rebuild_chip_layouts_(tk::LayoutCtx& ctx, const std::string& key,
                               const std::string& secondary_text);
    void paint_two_line_chip_(tk::PaintCtx& ctx) const;
    // Multi-attachment mode (pending_.size() >= 2): paints the small
    // thumbnail grid at chip_rects_, each with a corner × at
    // chip_remove_rects_[i] — see press_chip_remove_/on_pointer_down/up for
    // the manual (non-Button) click handling, matching reply/edit cancel.
    void paint_gallery_chips_(tk::PaintCtx& ctx) const;
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
    tk::BusyButton* send_btn_ = nullptr; // borrowed
    // Single-attachment mode only (pending_.size() == 1): borrowed remove
    // button, hidden when nothing is queued. Multi-attachment mode (2+)
    // uses manually hit-tested per-chip × glyphs instead — see
    // chip_rects_/chip_remove_rects_ below, same pattern as
    // reply_cancel_rect_/edit_cancel_rect_.
    tk::Button* remove_btn_ = nullptr;  // borrowed; hidden when no image
    // The "×" on the edit / reply banners and the recording strip — real
    // buttons positioned in arrange() and painted in paint().
    tk::Button* edit_cancel_btn_  = nullptr;
    tk::Button* reply_cancel_btn_ = nullptr;
    tk::Button* voice_cancel_btn_ = nullptr;

    // Painted-only context an AT should hear (see access_row_count).
    std::vector<std::pair<std::string, tk::Rect>> access_texts_() const;
    // Emoji/sticker/mic SVG glyphs are now self-painted by tk::Button
    // (Button::set_icon()); ComposeBar just refreshes the hover tint (and,
    // for mic, the recording-state SVG swap) every paint() — see the
    // btn_tint block there.
    // Cached Lucide close icon for the remove-attachment button (tint-aware,
    // stays crisp across DPI — same convention as ThreadListView/ThreadView).
    tk::IconCache remove_icon_;
    tk::Rect text_area_rect_{};
    ComposerTextArea* text_area_ = nullptr; // self-owned; null if constructed without a Host
    tk::Rect compose_card_rect_{}; // card outline wrapping text + icon buttons
    tk::Rect emoji_rect_{};
    tk::Rect sticker_rect_{};
    tk::Rect send_rect_{};
    tk::Rect preview_band_rect_{};
    tk::Rect preview_image_rect_{};
    tk::Rect remove_btn_rect_{};

    // Multi-attachment mode (pending_.size() >= 2) grid geometry, rebuilt in
    // arrange(). Index-aligned with pending_. Empty in single-attachment
    // mode, which keeps using preview_band_rect_/preview_image_rect_/
    // remove_btn_rect_ above unchanged.
    std::vector<tk::Rect> chip_rects_;
    std::vector<tk::Rect> chip_remove_rects_;
    bool press_chip_remove_ = false;
    std::size_t press_chip_remove_index_ = 0;

    // Pre-clamp natural height reported by the NativeTextArea. Starts at
    // 0 so the initial clamp() in recompute_height() floors to kMinHeight
    // (instead of pushing kMinHeight+padding through the clamp).
    float text_area_natural_ = 0.0f;
    float natural_height_ = kMinHeight; // total (incl. preview)
    std::string current_text_;
    bool mic_available_ = true;

    std::vector<PendingAttachment> pending_;
    // Monotonically increasing counter, incremented by every add_pending_*()
    // call — also the source of each new item's PendingAttachment::gen.
    // Stored in MediaInfo.pending_gen at drop time and looked up (by gen,
    // not position) in update_pending_attachment() to find the right item
    // and discard results for since-removed ones.
    std::uint32_t pending_gen_ = 0;

    // Reply state. reply_event_id_ is empty when not in reply mode.
    std::string reply_event_id_;
    std::string reply_sender_name_;
    std::string reply_body_preview_;
    tk::Rect reply_band_rect_{};
    tk::Rect reply_cancel_rect_{};

    // Edit state. edit_event_id_ is empty when not in edit mode.
    // Edit mode and reply mode are mutually exclusive.
    std::string edit_event_id_;
    // True when editing a media caption (Client::send_caption_edit) rather
    // than a text body (Client::send_edit).
    bool edit_is_caption_ = false;
    tk::Rect edit_band_rect_{};
    tk::Rect edit_cancel_rect_{};

    // Which compose button is currently showing a tooltip (None = none).
    enum class TooltipBtn { None, Emoji, Sticker, Mic };
    TooltipBtn tooltip_hover_ = TooltipBtn::None;

    // dispatch_pointer_move() always returns `this` (see its doc comment), which
    // hides the actual hit child's transitions from Host's hover bookkeeping.
    // Tracked separately here so the previous inner child (e.g. text_area_)
    // still gets on_pointer_leave() when the pointer moves to a different spot
    // inside ComposeBar's own bounds.
    std::weak_ptr<tk::Widget> inner_hovered_;

    // Voice recording state.
    bool recording_ = false;
    static constexpr std::size_t kMaxWaveformSamples = 80;
    std::vector<std::uint16_t> waveform_samples_;
    tk::Rect mic_btn_rect_{};
    tk::Rect waveform_strip_rect_{};
    tk::Rect voice_cancel_rect_{};
    std::unique_ptr<tk::TextLayout> elapsed_layout_;
    std::uint64_t recording_start_ms_ = 0;
};

} // namespace tesseract::views
