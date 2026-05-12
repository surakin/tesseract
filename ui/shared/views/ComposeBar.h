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
    static constexpr float kPreviewBandGap  =  8.0f;

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

    /// Attach a clipboard image as a pending payload. The shared widget
    /// stores the raw bytes + mime, generates a filename, decodes a
    /// thumbnail lazily on the next layout pass, and grows
    /// `natural_height()` to make room for the preview band. Replaces
    /// any image already attached. Fires `on_size_changed` so the host
    /// can refresh its fixed-height envelope.
    void set_pending_image(std::vector<std::uint8_t> bytes, std::string mime);

    /// Drop any attached image. No-op when none is attached.
    void clear_pending_image();

    /// True while an image is queued for the next send.
    bool has_pending_image() const { return pending_.has_value(); }

    /// Fires when the send button is clicked or NativeTextArea's submit
    /// callback fires (host wires both to the same target). Only fires
    /// for text-only sends — when a pending image is attached, the bar
    /// fires `on_send_image` instead and clears the image afterward.
    std::function<void(const std::string&)> on_send;

    /// Fires when send runs with a pending image attached. The host
    /// receives the raw clipboard bytes, the source mime, the generated
    /// filename, the trimmed caption (may be empty), and the source
    /// dimensions. The host re-encodes per `Settings::image_quality`,
    /// uploads, and posts the `m.image` event.
    std::function<void(std::vector<std::uint8_t> bytes,
                       std::string mime,
                       std::string filename,
                       std::string caption,
                       std::uint32_t width,
                       std::uint32_t height)> on_send_image;

    /// Fires when the emoji button is clicked.
    std::function<void()>                    on_emoji;

    /// Fires when `natural_height()` may have changed due to an image
    /// being attached or detached. The host should re-apply its
    /// fixed-height envelope and re-run `relayout()`.
    std::function<void()>                    on_size_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    struct PendingImage {
        std::vector<std::uint8_t> bytes;
        std::string               mime;
        std::string               filename;
        std::unique_ptr<tk::Image> preview;
        std::uint32_t             width  = 0;
        std::uint32_t             height = 0;
    };

    void refresh_send_enabled();
    void recompute_height();
    bool point_in_remove_btn(tk::Point world) const;
    static std::string make_filename(const std::string& mime);

    tk::Button* emoji_btn_  = nullptr;   // borrowed (owned by Widget tree)
    tk::Button* send_btn_   = nullptr;   // borrowed
    tk::Button* remove_btn_ = nullptr;   // borrowed; hidden when no image
    tk::Rect    text_area_rect_{};
    tk::Rect    emoji_rect_{};
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

    std::optional<PendingImage> pending_;
};

} // namespace tesseract::views
