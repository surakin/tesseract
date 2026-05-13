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

namespace tesseract::views {

class ComposeBar : public tk::Widget {
public:
    ComposeBar();
    ~ComposeBar() override = default;

    static constexpr float kMinHeight       = 56.0f;
    static constexpr float kMaxHeight       = 160.0f;
    static constexpr float kPreviewBandH    = 96.0f;
    static constexpr float kFileBandH       = 48.0f;
    static constexpr float kPreviewBandGap  =  8.0f;
    static constexpr float kReplyBandH      = 44.0f;
    static constexpr float kReplyBandGap    =  4.0f;
    static constexpr float kEditBandH       = 44.0f;
    static constexpr float kEditBandGap     =  4.0f;

    /// Rect inside the compose bar (widget-local coordinates, same space
    /// `root->arrange` operates in) for the host to overlay a
    /// NativeTextArea. Empty when the widget hasn't been arranged yet.
    tk::Rect text_area_rect() const { return text_area_rect_; }

    /// Host bridge: integration code pushes the latest natural height of
    /// the NativeTextArea here on every `on_height_changed` callback.
    /// The compose bar clamps to [kMinHeight, kMaxHeight] internally; the
    /// parent layout grows by re-measuring after this call.
    void set_text_area_natural_height(float h);
    float natural_height() const { return natural_height_; }

    /// Latest text — pushed by integration code on every `on_changed`
    /// callback from the NativeTextArea. Used to gate the send button.
    void set_current_text(std::string text);
    const std::string& current_text() const { return current_text_; }

    void  set_enabled(bool e);
    bool  enabled() const { return enabled_; }

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
    void set_pending_image(std::vector<std::uint8_t> bytes,
                           std::string               mime,
                           std::string               filename = {});

    /// Attach a non-image file as a pending payload. Renders as a single-
    /// line chip (paperclip + filename + size) above the input. Replaces
    /// any pending attachment (image or file) already attached.
    /// `filename` is required (no synthesis) — drag-and-drop / file-picker
    /// always supplies it.
    void set_pending_file(std::vector<std::uint8_t> bytes,
                          std::string               mime,
                          std::string               filename);

    /// Drop any attached payload (image or file). No-op when none.
    void clear_pending();
    /// Back-compat alias — same as `clear_pending()`.
    void clear_pending_image() { clear_pending(); }

    /// True while any attachment (image or file) is queued for send.
    bool has_pending() const { return pending_.has_value(); }
    /// Back-compat alias — same as `has_pending()`.
    bool has_pending_image() const { return pending_.has_value(); }

    /// Fires when the send button is clicked or NativeTextArea's submit
    /// callback fires (host wires both to the same target). Only fires
    /// for text-only sends — when a pending image or file is attached,
    /// the bar fires `on_send_image` / `on_send_file` instead and clears
    /// the attachment afterward.
    std::function<void(const std::string&)> on_send;

    /// Fires when send runs with a pending image attached. The host
    /// receives the raw clipboard bytes, the source mime, the generated
    /// filename, the trimmed caption (may be empty), the source dimensions,
    /// and the reply_event_id (empty when no reply is pending). The host
    /// re-encodes per `Settings::image_quality`, uploads, and posts the
    /// `m.image` event.
    std::function<void(std::vector<std::uint8_t> bytes,
                       std::string mime,
                       std::string filename,
                       std::string caption,
                       std::uint32_t width,
                       std::uint32_t height,
                       std::string reply_event_id)> on_send_image;

    /// Fires when send runs with a pending non-image file attached. The
    /// host receives the raw bytes, the OS-supplied (or guessed) mime,
    /// the file's basename, the trimmed caption (may be empty), and the
    /// reply_event_id (empty when no reply is pending). The host uploads
    /// as-is and posts the `m.file` event.
    std::function<void(std::vector<std::uint8_t> bytes,
                       std::string mime,
                       std::string filename,
                       std::string caption,
                       std::string reply_event_id)> on_send_file;

    /// Fires when the emoji button is clicked.
    std::function<void()>                    on_emoji;

    /// Fires when the sticker button is clicked. Hosts open the
    /// `StickerPicker` popup; the chosen entry is sent via
    /// `Client::send_sticker`.
    std::function<void()>                    on_sticker;

    /// Fires when `natural_height()` may have changed due to an image
    /// being attached or detached. The host should re-apply its
    /// fixed-height envelope and re-run `relayout()`.
    std::function<void()>                    on_size_changed;

    /// Enter reply mode. Displays a "Replying to <sender_name>" banner
    /// with `body_preview` above the text input. Grows `natural_height()`
    /// by `kReplyBandH + kReplyBandGap` and fires `on_size_changed`.
    void set_reply_to(std::string event_id,
                      std::string sender_name,
                      std::string body_preview);

    /// Exit reply mode. Clears the reply banner, shrinks `natural_height()`,
    /// and fires `on_size_changed`. No-op when not in reply mode.
    void  clear_reply();
    bool  has_reply()              const { return !reply_event_id_.empty(); }
    const std::string& reply_event_id() const { return reply_event_id_; }

    /// Fires in place of `on_send` when a reply is pending. The host should
    /// call `Client::send_reply(current_room_, reply_event_id, body)`.
    std::function<void(const std::string& reply_event_id,
                        const std::string& body)> on_send_reply;

    /// Enter edit mode. Displays an "Editing message" banner above the text
    /// input (replaces any active reply mode). Grows `natural_height()` by
    /// `kEditBandH + kEditBandGap` and fires `on_size_changed`.
    void set_editing(std::string event_id);

    /// Exit edit mode. Clears the edit banner, shrinks `natural_height()`,
    /// and fires `on_size_changed`. No-op when not in edit mode.
    void  clear_editing();
    bool  has_editing()              const { return !edit_event_id_.empty(); }
    const std::string& edit_event_id() const { return edit_event_id_; }

    /// Fires in place of `on_send` when edit mode is active. The host should
    /// call `Client::send_edit(current_room_, event_id, new_body)`.
    std::function<void(const std::string& event_id,
                        const std::string& new_body)> on_send_edit;

    /// Fires when the user cancels an in-progress edit via the "×" button
    /// in the edit banner. The host should restore the original text to the
    /// compose field and call `clear_editing()`.
    std::function<void()> on_edit_cancelled;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;
    bool     on_pointer_down(tk::Point local)              override;
    void     on_pointer_up  (tk::Point local,
                              bool inside_self)            override;

private:
    struct PendingAttachment {
        enum class Kind { Image, File };
        Kind                       kind = Kind::Image;
        std::vector<std::uint8_t>  bytes;
        std::string                mime;
        std::string                filename;
        // Image-only fields. For Kind::File these stay 0/null.
        std::unique_ptr<tk::Image> preview;
        std::uint32_t              width  = 0;
        std::uint32_t              height = 0;
    };

    void refresh_send_enabled();
    void recompute_height();
    bool point_in_remove_btn(tk::Point world) const;
    static std::string make_filename(const std::string& mime);
    // Cached layout used to paint the filename (and a second line for size)
    // inside the file chip. Rebuilt lazily on first paint after a file is
    // attached or replaced.
    std::unique_ptr<tk::TextLayout> file_name_layout_;
    std::unique_ptr<tk::TextLayout> file_size_layout_;
    std::string                     file_layout_key_;  // mime+name+size cache key

    tk::Button* emoji_btn_   = nullptr;   // borrowed (owned by Widget tree)
    tk::Button* sticker_btn_ = nullptr;   // borrowed
    tk::Button* send_btn_    = nullptr;   // borrowed
    tk::Button* remove_btn_  = nullptr;   // borrowed; hidden when no image
    // Cached layouts for glyphs painted *over* the Icon-variant emoji and
    // sticker buttons — same Title-size + ascent-centring as reaction
    // chips, so the surfaces look consistent.
    std::unique_ptr<tk::TextLayout> emoji_layout_;
    std::unique_ptr<tk::TextLayout> sticker_layout_;
    tk::Rect    text_area_rect_{};
    tk::Rect    emoji_rect_{};
    tk::Rect    sticker_rect_{};
    tk::Rect    send_rect_{};
    tk::Rect    preview_band_rect_{};
    tk::Rect    preview_image_rect_{};
    tk::Rect    remove_btn_rect_{};

    // Pre-clamp natural height reported by the NativeTextArea. Starts at
    // 0 so the initial clamp() in recompute_height() floors to kMinHeight
    // (instead of pushing kMinHeight+padding through the clamp).
    float       text_area_natural_ = 0.0f;
    float       natural_height_    = kMinHeight;   // total (incl. preview)
    std::string current_text_;
    bool        enabled_ = true;

    std::optional<PendingAttachment> pending_;

    // Reply state. reply_event_id_ is empty when not in reply mode.
    std::string reply_event_id_;
    std::string reply_sender_name_;
    std::string reply_body_preview_;
    tk::Rect    reply_band_rect_{};
    tk::Rect    reply_cancel_rect_{};
    bool        press_reply_cancel_ = false;

    // Edit state. edit_event_id_ is empty when not in edit mode.
    // Edit mode and reply mode are mutually exclusive.
    std::string edit_event_id_;
    tk::Rect    edit_band_rect_{};
    tk::Rect    edit_cancel_rect_{};
    bool        press_edit_cancel_ = false;
};

} // namespace tesseract::views
