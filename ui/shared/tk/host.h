#pragma once

// Per-platform integration surface. The shared widget tree is owned by
// a Host: the platform-native window, event pump, and bridge to
// off-thread callbacks (Rust SDK → UI thread) all live behind this
// interface. The toolkit owns paint and layout; the host owns:
//
//   - the native window / view / HWND
//   - the run loop ("repaint please", "run this on the UI thread")
//   - menus, file dialogs, accessibility, IME (kept native by design)
//   - native edit-control overlays for text input, since IME stays
//     native until tk::TextField + the IME-passthrough hybrid lands

#include "audio.h"
#include "canvas.h"
#include "video.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk {

// A borrowed handle to a native text input control overlaid on top of
// the canvas, positioned to align with a tk::Widget's rect. Caller
// destroys to remove the overlay. Hosts back this with QLineEdit,
// GtkEntry, Win32 EDIT, UITextField respectively.
class NativeTextField {
public:
    virtual ~NativeTextField() = default;

    // Position + size of the overlay in widget-tree coordinates (the
    // same coordinate space root_widget->arrange() operates in).
    virtual void set_rect       (Rect r)              = 0;
    virtual void set_text       (std::string text)    = 0;
    virtual std::string text    () const              = 0;
    virtual void set_placeholder(std::string text)    = 0;
    virtual void set_focused    (bool focused)        = 0;
    virtual void set_visible    (bool visible)        = 0;
    virtual void set_enabled    (bool enabled)        = 0;

    // Password / obscured-entry mode. Used by the recovery-banner key
    // field so the user's recovery secret isn't shoulder-surfed.
    virtual void set_password   (bool password)       = 0;

    // Text-change + submit (Enter) hooks. Hosts invoke these on the UI
    // thread; the callable can mutate the widget tree freely.
    virtual void set_on_changed(std::function<void(const std::string&)>) = 0;
    virtual void set_on_submit (std::function<void()>)                    = 0;

    // Focus-change hook. Called with `true` when the field gains focus,
    // `false` when it loses it. Default no-op so backends can opt in.
    virtual void set_on_focus_changed(std::function<void(bool)>) {}
};

// Multi-line variant. Auto-grows up to a host-clamped envelope so the
// compose bar's height tracks the text content. Backs the ComposeBar's
// input affordance; IME / selection stay native.
class NativeTextArea {
public:
    virtual ~NativeTextArea() = default;

    virtual void set_rect       (Rect r)              = 0;
    virtual void set_text       (std::string text)    = 0;
    virtual std::string text    () const              = 0;
    virtual void set_placeholder(std::string text)    = 0;
    virtual void set_focused    (bool focused)        = 0;
    virtual void set_visible    (bool visible)        = 0;
    virtual void set_enabled    (bool enabled)        = 0;

    // Set the font used to render and measure input text. Callers pass a
    // FontRole so each backend can apply the role's metrics natively (QFont
    // point size, GTK CSS font-size, etc.). Defaults to a no-op so backends
    // that haven't implemented it yet compile without changes.
    virtual void set_font_role(FontRole) {}

    // Set the foreground text color to match the active tk::Theme instead of
    // inheriting the system application palette. Call once after creation and
    // again whenever the theme changes. Default no-op for backends that rely
    // solely on the system palette.
    virtual void set_text_color(Color) {}

    // Push the natural content height up to the host on every change so
    // the parent layout can resize the compose envelope inside its
    // [min, max] clamp.
    virtual float natural_height() const = 0;

    virtual void set_on_changed       (std::function<void(const std::string&)>) = 0;
    virtual void set_on_submit        (std::function<void()>)                    = 0;
    virtual void set_on_height_changed(std::function<void(float)>)               = 0;

    // Insert `text` at the current cursor position (or replace the
    // selection). Leaves the cursor after the inserted text, so successive
    // insertions compose correctly. Preferred over set_text() for emoji /
    // autocomplete injection since it preserves cursor position and undo.
    virtual void insert_at_cursor(std::string text) = 0;

    // Tab / ShiftTab cycle forward / backward through the shortcode popup
    // (wrapping at the ends); Up / Down clamp at the ends.
    enum class NavKey { Up, Down, Escape, Tab, ShiftTab };

    /// Bounding rect of the insertion cursor in surface-local coordinates.
    virtual tk::Rect cursor_rect() const = 0;

    /// Replace the UTF-8 byte range [start, end) with `text`.
    /// Preserves undo history.
    virtual void replace_range(int start, int end, std::string text) = 0;

    /// Install a navigation callback for when the shortcode popup is open.
    /// Return true from the callback to suppress default key handling.
    virtual void set_on_popup_nav(std::function<bool(NavKey)> fn) = 0;

    /// Fired when the Up arrow is pressed while the composer is empty and
    /// the shortcode popup is not open — used to edit the last own message
    /// (Element/Slack convention). Return true to consume the key (an
    /// editable message was loaded into the composer); false lets Up fall
    /// through to default caret handling.
    virtual void set_on_edit_last(std::function<bool()> fn) = 0;

    // Hook for clipboard image pastes. When set, and the user pastes a
    // payload that includes image MIME types, the host invokes the handler
    // with the raw image bytes + mime ("image/png", "image/jpeg", …) and
    // suppresses the default (text) paste path for that paste cycle.
    // Non-image pastes fall through to the platform's normal text handling.
    using ImagePasteHandler =
        std::function<void(std::vector<std::uint8_t> bytes, std::string mime)>;
    virtual void set_on_image_paste(ImagePasteHandler) = 0;
};

// Drag-and-drop handler installed on a per-platform Surface. Fired once
// per dropped file when the user drops one or more files (or in-app
// image data) onto the surface. The shell inspects `mime` to dispatch to
// the image or file path. `filename` is the basename the homeserver
// should receive — empty for in-app image-data drops where no filename
// is available (the ComposeBar synthesises one). `application/octet-
// stream` is used when the backend can't sniff a more specific mime.
using FileDropHandler =
    std::function<void(std::vector<std::uint8_t> bytes,
                       std::string               mime,
                       std::string               filename)>;

// Deprecated alias for `FileDropHandler` kept while shells migrate.
using ImageDropHandler = FileDropHandler;

// Maximum size of an image we'll auto-encode from in-app drag data (no
// filename) into memory. Arbitrary file drops are gated by the
// homeserver-reported upload limit at the shell level, not this constant.
inline constexpr std::size_t kMaxDroppedImageBytes = 50 * 1024 * 1024;

// Maximum size of a file we'll read into memory from a drop. Hard ceiling
// even before the homeserver upload-limit check, so a 50 GB file drop
// doesn't OOM the renderer. 2 GiB matches typical homeserver upper bounds.
inline constexpr std::size_t kMaxDroppedFileBytes = 2ull * 1024 * 1024 * 1024;

// Result of `Host::encode_for_send`. Bytes are owned; `mime` is e.g.
// "image/png" or "image/jpeg". `width` and `height` are 0 only when the
// decoder couldn't recover them.
struct EncodedImage {
    std::vector<std::uint8_t> bytes;
    std::string               mime;
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
};

class Host {
public:
    virtual ~Host() = default;

    // Schedule a repaint of the entire root widget. Cheap to call
    // multiple times — the host coalesces.
    virtual void request_repaint() = 0;

    // Post `task` to be run on the UI thread. The most common caller is
    // Rust async completion handlers that finish on a worker thread and
    // need to mutate widgets. Safe to call from any thread.
    virtual void post_to_ui(std::function<void()> task) = 0;

    // Run `fn` on the UI thread after at least `ms` milliseconds. One-shot;
    // coalescing not required. Used by the message-list room-switch gate to
    // bound how long the list is held invisible waiting for media to load.
    // Must be called on the UI thread.
    virtual void post_delayed(int ms, std::function<void()> fn) = 0;

    // Allocate a native text input control parented to the host's
    // surface. The returned NativeTextField is owned by the caller; let
    // the unique_ptr destruct to remove the overlay.
    virtual std::unique_ptr<NativeTextField> make_text_field() = 0;

    // Multi-line variant — IME-friendly, expanding inside the host's
    // clamp. Used by the shared ComposeBar for the message-input row.
    virtual std::unique_ptr<NativeTextArea>  make_text_area()  = 0;

    // Create an `AudioPlayer` backed by the platform's native audio stack
    // (QMediaPlayer / GStreamer / AVAudioPlayer / Media Foundation). The
    // caller owns the returned player and may destroy it at any time. May
    // return `nullptr` on platforms that do not yet ship a backend; callers
    // must check before invoking.
    virtual std::unique_ptr<AudioPlayer>     make_audio_player() = 0;

    // Create a `VideoPlayer` backed by the platform's native media stack.
    // Returns `nullptr` on platforms without a video backend; callers must
    // check before invoking.
    virtual std::unique_ptr<VideoPlayer>     make_video_player() = 0;

    // Decode `data[0..len)` and produce a payload ready to upload to the
    // homeserver. `compress=true` caps the image at 1600×1200 (preserving
    // aspect ratio) and re-encodes it as image/jpeg quality 75.
    // `compress=false` returns the input bytes unchanged with the original
    // mime sniffed by the decoder. Returns an `EncodedImage` with an empty
    // `bytes` vector if the input can't be decoded.
    virtual EncodedImage encode_for_send(const std::uint8_t* data,
                                         std::size_t         len,
                                         bool                compress) = 0;
};

} // namespace tk
