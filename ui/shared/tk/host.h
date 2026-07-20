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
#include "audio_capture.h"
#include "audio_playback.h"
#include "canvas.h"
#include "device_listing.h"
#include "toast.h"
#include "tooltip.h"
#include "video.h"
#include "widget.h"

#include <tesseract/mentions.h> // tesseract::MentionSeg

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tk
{

class Button;

// Navigation keys a focused native text control forwards to a popup / list it
// drives while it keeps keyboard focus — the shortcode / mention popups (via
// NativeTextArea) and the Ctrl+K quick switcher (via NativeTextField). Lives at
// namespace scope so both control types can share one type.
enum class NavKey
{
    Up,
    Down,
    Left,
    Right,
    Escape,
    Tab,
    ShiftTab
};

// A borrowed handle to a native text input control overlaid on top of
// the canvas, positioned to align with a tk::Widget's rect. Caller
// destroys to remove the overlay. Hosts back this with QLineEdit,
// GtkEntry, Win32 EDIT, UITextField respectively.
class NativeTextField
{
public:
    virtual ~NativeTextField() = default;

    // Position + size of the overlay in widget-tree coordinates (the
    // same coordinate space root_widget->arrange() operates in).
    virtual void set_rect(Rect r) = 0;
    virtual void set_text(std::string text) = 0;
    virtual std::string text() const = 0;
    virtual void set_placeholder(std::string text) = 0;
    virtual void set_focused(bool focused) = 0;
    virtual void set_visible(bool visible) = 0;
    virtual void set_enabled(bool enabled) = 0;

    // Password / obscured-entry mode. Used by the recovery-banner key
    // field so the user's recovery secret isn't shoulder-surfed.
    virtual void set_password(bool password) = 0;

    // Text-change + submit (Enter) hooks. Hosts invoke these on the UI
    // thread; the callable can mutate the widget tree freely.
    virtual void set_on_changed(std::function<void(const std::string&)>) = 0;
    virtual void set_on_submit(std::function<void()>) = 0;

    // Up / Down / Escape navigation forwarded to a popup the field drives
    // while it holds focus (the Ctrl+K quick switcher). Return true from the
    // callback to consume the key. Default no-op so backends opt in; the
    // single-line field never inserts Tab so only Up/Down/Escape are raised.
    virtual void set_on_popup_nav(std::function<bool(NavKey)>) {}

    // Override the text (and placeholder) colour so the field respects
    // the application's dark/light palette. Default no-op; backends that
    // rely on the system theme for text colour don't need to implement it.
    virtual void set_text_color(Color) {}

    // Focus-change hook. Called with `true` when the field gains focus,
    // `false` when it loses it. Default no-op so backends can opt in.
    virtual void set_on_focus_changed(std::function<void(bool)>)
    {
    }

    // Reduce internal padding so the field fits inside a compact inline row
    // (e.g. the account settings display-name row). Default no-op.
    virtual void set_compact(bool) {}
};

// Multi-line variant. Auto-grows up to a host-clamped envelope so the
// compose bar's height tracks the text content. Backs the ComposeBar's
// input affordance; IME / selection stay native.
class NativeTextArea
{
public:
    virtual ~NativeTextArea() = default;

    virtual void set_rect(Rect r) = 0;
    virtual void set_text(std::string text) = 0;
    virtual std::string text() const = 0;
    virtual void set_placeholder(std::string text) = 0;
    virtual void set_focused(bool focused) = 0;
    virtual void set_visible(bool visible) = 0;
    // Last value passed to set_visible() (defaults to true at construction —
    // matches the existing "show on first set_rect" semantics shells rely on
    // to detect the hidden→visible transition for prefill).
    virtual bool visible() const = 0;
    virtual void set_enabled(bool enabled) = 0;

    // Set the font used to render and measure input text. Callers pass a
    // FontRole so each backend can apply the role's metrics natively (QFont
    // point size, GTK CSS font-size, etc.). Defaults to a no-op so backends
    // that haven't implemented it yet compile without changes.
    virtual void set_font_role(FontRole)
    {
    }

    // Set the foreground text color to match the active tk::Theme instead of
    // inheriting the system application palette. Call once after creation and
    // again whenever the theme changes. Default no-op for backends that rely
    // solely on the system palette.
    virtual void set_text_color(Color)
    {
    }

    // Push the natural content height up to the host on every change so
    // the parent layout can resize the compose envelope inside its
    // [min, max] clamp.
    virtual float natural_height() const = 0;

    virtual void set_on_changed(std::function<void(const std::string&)>) = 0;
    virtual void set_on_submit(std::function<void()>) = 0;
    virtual void set_on_height_changed(std::function<void(float)>) = 0;

    // Insert `text` at the current cursor position (or replace the
    // selection). Leaves the cursor after the inserted text, so successive
    // insertions compose correctly. Preferred over set_text() for emoji /
    // autocomplete injection since it preserves cursor position and undo.
    virtual void insert_at_cursor(std::string text) = 0;

    // Tab / ShiftTab cycle forward / backward through the shortcode popup
    // (wrapping at the ends); Up / Down clamp at the ends. Aliased to the
    // namespace-scope tk::NavKey so existing `NativeTextArea::NavKey`
    // references keep compiling.
    using NavKey = tk::NavKey;

    /// Bounding rect of the insertion cursor in surface-local coordinates.
    virtual tk::Rect cursor_rect() const = 0;

    /// Replace the UTF-8 byte range [start, end) with `text`.
    /// Preserves undo history.
    virtual void replace_range(int start, int end, std::string text) = 0;

    /// Byte offset of the caret within text(), where each inline mention pill
    /// counts as its placeholder codepoint (3 UTF-8 bytes, U+FFFC). Backends
    /// that don't track the caret fall back to end-of-text. Use this — not
    /// `text().size()` — when locating an `@`-prefix so mid-text mentions work.
    virtual int cursor_byte_pos() const
    {
        return static_cast<int>(text().size());
    }

    /// Replace the UTF-8 byte range [start, end) with an atomic inline mention
    /// pill rendered as a chip showing `display_name`. The pill behaves as a
    /// single character for caret movement / backspace and is reported by
    /// composer_draft(). For an @room mention pass `is_room = true` (the
    /// `user_id` is ignored). The default inserts plain text — backends
    /// override to draw a real chip.
    virtual void insert_mention(int start, int end, const std::string& user_id,
                                const std::string& display_name, bool is_room)
    {
        (void)user_id;
        replace_range(start, end, is_room ? "@room" : display_name);
    }

    /// Replace the UTF-8 byte range [start, end) with an atomic inline
    /// MSC2545 custom-emoticon pill painted from `image` — the same
    /// already-decoded bitmap the caller's EmojiPicker/ShortcodePopup used
    /// to render its own tile/thumbnail. Behaves as a single character for
    /// caret movement / backspace, same as insert_mention, and is reported
    /// by composer_draft(). `image` may be null (bitmap not yet decoded) —
    /// the default, and every platform override, then falls back to
    /// literal ":shortcode:" text.
    virtual void insert_emoticon(int start, int end, const std::string& shortcode,
                                 const std::string& mxc_url, const tk::Image* image)
    {
        (void)mxc_url;
        (void)image;
        replace_range(start, end, ":" + shortcode + ":");
    }

    /// The composer content as ordered segments (typed text, mention pills,
    /// and emoticon pills), for building the outgoing message via
    /// `tesseract::build_mention_message`. The default (no pill support)
    /// returns the whole text as one Text segment.
    virtual std::vector<tesseract::MentionSeg> composer_draft() const
    {
        std::vector<tesseract::MentionSeg> segs;
        tesseract::MentionSeg s;
        s.kind = tesseract::MentionSeg::Kind::Text;
        s.text = text();
        segs.push_back(std::move(s));
        return segs;
    }

    /// Theme the inline mention pills (background + text colour). Call once
    /// after creation and again when the theme changes. Default no-op.
    virtual void set_mention_colors(Color bg, Color fg)
    {
        (void)bg;
        (void)fg;
    }

    /// Install a navigation callback for when the shortcode popup is open.
    /// Return true from the callback to suppress default key handling.
    virtual void set_on_popup_nav(std::function<bool(NavKey)> fn) = 0;

    // Focus-change hook. Called with `true` when the area gains focus,
    // `false` when it loses it. Default no-op so backends can opt in —
    // mirrors NativeTextField::set_on_focus_changed above.
    virtual void set_on_focus_changed(std::function<void(bool)>)
    {
    }

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

    // Backends that render custom-emoticon/inline-image runs (currently only
    // the Windows BetterText backend) call this synchronously to resolve a
    // uri (an mxc:// URL) to an already-decoded image. Return the cached
    // image if the shell already has it; otherwise kick off a fetch as a
    // side effect (matching every other `ensure_media_image_`-style caller
    // in the codebase — fire-and-forget, no completion callback) and return
    // nullptr — the backend is expected to retry the same uri later once the
    // shell's cache has it. Default no-op for backends that don't need it.
    virtual void set_image_resolver(std::function<const tk::Image*(const std::string& uri)>)
    {
    }
};

// Callback invoked when a drag-drop file could not be read (e.g. a VFS file
// that isn't materialised). `reason` is a human-readable error description.
using FileDropErrorHandler = std::function<void(std::string reason)>;

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
struct EncodedImage
{
    std::vector<std::uint8_t> bytes;
    std::string mime;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class Host
{
public:
    virtual ~Host() = default;

    // RootWidget::queue_for_deletion() forwards straight to the protected
    // queue_for_deletion() below — it's the only external caller, so this
    // stays a narrow grant rather than a public API.
    friend class RootWidget;

    // Schedule a repaint of the entire root widget. Cheap to call
    // multiple times — the host coalesces.
    virtual void request_repaint() = 0;

    // True if a Ctrl modifier was held during the most recent pointer press/
    // release dispatched through this host. tk's pointer events don't carry
    // modifier state, so shells use this to distinguish a plain click from a
    // Ctrl+click inside widget callbacks (e.g. TabBar::on_tab_selected). Only
    // the GTK4 host needs to track this — the Win32/Qt6/macOS shells query
    // their native modifier state directly — so the default returns false.
    virtual bool pointer_ctrl_held() const
    {
        return false;
    }

    // Post `task` to be run on the UI thread. The most common caller is
    // Rust async completion handlers that finish on a worker thread and
    // need to mutate widgets. Safe to call from any thread.
    virtual void post_to_ui(std::function<void()> task) = 0;

    // Install a callback invoked on every pointer / keyboard / wheel event
    // routed through this host. ShellBase wires this to feed activity into
    // PresenceTracker so the user shows as Online while engaging with the
    // app. Called on the UI thread; cheap implementations only — every
    // event passes through this hook. Set to `{}` (empty std::function) to
    // disable.
    void set_on_user_activity(std::function<void()> cb)
    {
        on_user_activity_ = std::move(cb);
    }

protected:
    // Each Host impl invokes this from its native input handlers — see
    // host_qt.cpp / host_gtk.cpp / host_win32.cpp / host_macos.mm.
    void fire_user_activity_()
    {
        if (on_user_activity_) on_user_activity_();
    }

public:

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
    virtual std::unique_ptr<NativeTextArea> make_text_area() = 0;

    // Create an `AudioPlayer` backed by the platform's native audio stack
    // (QMediaPlayer / GStreamer / AVAudioPlayer / Media Foundation). The
    // caller owns the returned player and may destroy it at any time. May
    // return `nullptr` on platforms that do not yet ship a backend; callers
    // must check before invoking.
    virtual std::unique_ptr<AudioPlayer> make_audio_player() = 0;

    /// Create an AudioCapture backed by the platform's native capture stack.
    /// Returns nullptr when no audio input device is present — callers must
    /// check for null before use.
    virtual std::unique_ptr<AudioCapture> make_audio_capture() = 0;

    /// Create an AudioPlayback for routing remote call audio to the speaker.
    virtual std::unique_ptr<AudioPlayback> make_audio_playback() = 0;

    // Enumerate available audio input devices (microphones).
    // Returns an empty vector on platforms that haven't implemented this yet.
    virtual std::vector<DeviceListing> enumerate_audio_inputs() const { return {}; }

    // Enumerate available audio output devices (speakers/headphones).
    virtual std::vector<DeviceListing> enumerate_audio_outputs() const { return {}; }

    // Enumerate available camera (video capture) devices.
    virtual std::vector<DeviceListing> enumerate_cameras() const { return {}; }

    // Create a `VideoPlayer` backed by the platform's native media stack.
    // Returns `nullptr` on platforms without a video backend; callers must
    // check before invoking.
    virtual std::unique_ptr<VideoPlayer> make_video_player() = 0;

    // Decode `data[0..len)` and produce a payload ready to upload to the
    // homeserver. `compress=true` caps the image at 1600×1200 (preserving
    // aspect ratio) and re-encodes it as image/jpeg quality 75.
    // `compress=false` returns the input bytes unchanged with the original
    // mime sniffed by the decoder. Returns an `EncodedImage` with an empty
    // `bytes` vector if the input can't be decoded.
    virtual EncodedImage encode_for_send(const std::uint8_t* data,
                                         std::size_t len, bool compress) = 0;

    // Write `text` to the system clipboard as plain text.
    virtual void set_clipboard_text(std::string_view text) = 0;

    // Decode `encoded_bytes` (a self-describing PNG/JPEG/GIF/WebP blob) and
    // place the resulting bitmap on the system clipboard. Returns false if the
    // bytes could not be decoded or the platform has no image-clipboard
    // support. Used by the image lightbox's "copy image" button.
    virtual bool
    set_clipboard_image(std::span<const std::uint8_t> encoded_bytes) = 0;

    // ── Popup management ─────────────────────────────────────────────────────
    // A widget that wants to render and receive input above the entire widget
    // tree (e.g. an open ComboBox dropdown) calls register_popup(this) during
    // its paint(). The host draws the popup via paint_overlay() after the full
    // tree paint, and routes pointer events to it with priority.

    void register_popup(Widget* w) { pending_popup_ = track(w); }

    // Returns the currently active popup (valid between paint frames).
    Widget* popup() const { return popup_.lock().get(); }

    // ── Focus scope ──────────────────────────────────────────────────────────
    // A widget that wants to scope keyboard Tab/Shift-Tab traversal to its own
    // subtree while it's the active modal (e.g. a full-panel overlay covering
    // its parent view) calls set_focus_scope(this) while open and
    // clear_focus_scope() while closed. advance_focus_() then walks this
    // subtree instead of the whole window. Falls back to input_root_() when
    // unset or the scoped widget is gone/hidden.
    void set_focus_scope(Widget* w) { focus_scope_ = track(w); }
    void clear_focus_scope() { focus_scope_.reset(); }

    // ── Canvas-level keyboard focus ──────────────────────────────────────────
    // Tracks which tk::Widget currently holds tk-level keyboard focus, scoped
    // entirely to the canvas widget tree — native NativeTextField/NativeTextArea
    // overlays are a separate, untouched system (they intercept keys at the OS
    // level while they hold real OS focus; see set_on_popup_nav above). No-op
    // if `w` is null, not focusable(), or not enabled(); moving focus fires
    // on_focus_lost() on the previous holder (if any) and on_focus_gained() on
    // `w`, then requests a repaint (for the focus-ring redraw).
    void request_focus(Widget* w);

    // Clears canvas-level focus, firing on_focus_lost() on the current holder
    // (if any).
    void clear_focus();

    // Currently tk-focused widget, or nullptr.
    Widget* focused_widget() const { return focused_widget_.lock().get(); }

    // Moves tk-level focus to the next (forward) or previous focusable
    // canvas widget, exactly as Tab/Shift-Tab already does via
    // dispatch_key_down. Public (unlike dispatch_key_down) so native-overlay
    // code outside Host/its subclasses — e.g. tk::TextField forwarding an
    // unhandled Tab out of a focused native control — can drive the same
    // traversal without needing key-dispatch internals. Returns false if
    // there's no next focusable widget.
    bool advance_focus(bool forward) { return advance_focus_(forward); }

    // ── Tooltip management ───────────────────────────────────────────────────
    // Any widget can request a tooltip near itself. Since on_pointer_move()/
    // on_pointer_leave() don't receive a PaintCtx, callers cache `ctx.host`
    // from their own paint() override (see EncryptionSetupOverlay's host_
    // member for the existing precedent of this idiom) and call these
    // directly from their hover handlers. `owner` is an opaque identity token
    // (normally `this`, but any stable pointer works) used only to
    // disambiguate stale/superseded calls — Host never dereferences it. At
    // most one tooltip is ever active at a time, and none is shown while a
    // real popup (register_popup()) is open.

    // Request a tooltip for `owner`, showing `text` near `anchor_world` after
    // a short dwell delay. A repeated call from the same owner refreshes the
    // text/anchor in place (immediately if already visible, otherwise the
    // original delay keeps running); a call from a different owner resets
    // the delay. No-op while a popup is open, UNLESS `from_popup` is true —
    // set that when `owner` is itself content of the currently-open
    // register_popup()'d widget (e.g. TabbedGridPicker's own grid-cell
    // shortcode tooltip): the "no tooltip while a popup is open" rule exists
    // to stop background/tree content from tooltipping behind an unrelated
    // open popup, not to block the popup's own content from tooltipping
    // itself. A repeated call whose text and anchor are unchanged from
    // what's already stored is a pure no-op (no repaint requested) — safe
    // for callers that must re-assert the tooltip on every paint() frame
    // instead of on a hover-transition edge.
    virtual void show_tooltip(const void* owner, std::string text,
                              Rect anchor_world, bool from_popup = false);

    // Hide (or cancel the pending delay for) the tooltip owned by `owner`.
    // No-op if `owner` isn't the current tooltip owner.
    virtual void hide_tooltip(const void* owner);

    // Update the text of a tooltip already owned by `owner`, with no delay —
    // used when content that wasn't available yet arrives while the pointer
    // is still parked over the widget/region `owner` represents. If nobody
    // currently owns the tooltip, this adopts ownership for `owner` and
    // shows immediately, skipping the dwell delay (the caller is expected to
    // only call this while it knows itself to be genuinely hovered — e.g. via
    // its own hover-count bookkeeping — since Host has no way to verify that
    // for an `owner` token that isn't itself the widget under the pointer,
    // such as a container tooltipping on behalf of its children). No-op
    // while a popup is open or while a different owner already holds it.
    virtual void update_tooltip_text(const void* owner, std::string text);

    // ── Toast notifications ──────────────────────────────────────────────────
    // Shows a brief "toast" pill (e.g. "Copied to clipboard"), bottom-centered
    // over the whole window, above everything else. Auto-hides after
    // kToastDurationMs via post_delayed(). A second call while one is showing
    // just restarts the duration with the new message. Available to any
    // widget via host() — this is the one global mechanism; no widget should
    // own a private toast instance of its own.
    void show_toast(std::string message);

    // ── Shared pointer dispatch ──────────────────────────────────────────────
    // The pointer state machine (drag/hover tracking, release-inside check) and
    // the popup input/hover routing live here, once, for all four backends.
    // Each native host translates its platform event to a world-space `Point`
    // (and performs any native capture/grab step), then calls these. The shared
    // logic operates on `input_root_()`, `popup_`, and the tracked-pointer
    // fields below, calling the virtual `request_repaint()` to schedule redraws.
protected:
    // Pointer-down: route into an open popup if the press lands inside it,
    // otherwise dismiss the popup and hit-test the tree, capturing the pressed
    // widget so subsequent moves drag it and the matching up fires its click.
    void dispatch_pointer_down(Point world);

    // Pointer-move: drag the captured widget if one is held; otherwise route
    // hover into an open popup (when inside it) or update Button / widget hover
    // tracking against the tree.
    void dispatch_pointer_move(Point world);

    // Pointer-up: deliver the release to the captured widget with an
    // inside-its-local-bounds flag, then clear the capture.
    void dispatch_pointer_up(Point world);

    // Pointer-leave: clear hover state and synthesise a pointer-up outside any
    // widget so a captured widget can clean up its pressed state.
    void dispatch_pointer_leave();

    // Wheel: route into an open popup when the pointer is inside it; otherwise
    // route into the widget tree. Never dismisses the popup (wheel outside just
    // reaches the tree beneath). Returns true if the event was consumed.
    bool dispatch_wheel(Point world, float dx, float dy, bool is_touchpad = false);

    // Keyboard input, shared across all four backends. Each platform's native
    // key handler translates its event into a KeyEvent and calls this
    // directly (it is not reached while a NativeTextField/NativeTextArea
    // holds real OS focus — that native control consumes the key itself).
    // Order: an open popup gets first refusal; then, if a canvas widget
    // currently holds tk-level focus, Tab/Backtab advance the tk focus
    // traversal (see next_focusable() in widget.h) and any other key is
    // offered to that widget's own subtree first; finally falls back to the
    // existing whole-tree broadcast (this is what keeps MainAppWidget's
    // global accelerators / Escape-dismiss-transient working as a catch-all,
    // since it sits near the root and is reached last either way). Returns
    // true if the event was consumed.
    bool dispatch_key_down(const KeyEvent& event);

    // Dropped-file input, mirroring dispatch_pointer_down. `world` is in
    // root-surface coordinates. Walks the tree via
    // `input_root_()->dispatch_file_drop(...)` — no popup/capture bookkeeping
    // is needed, since a drop is a single synchronous event rather than a
    // press/drag/release gesture. Returns the widget that accepted the drop,
    // or nullptr if the drop landed outside every drop-aware widget (in which
    // case `payload` is left untouched).
    Widget* dispatch_file_drop(Point world, FileDropPayload& payload);

    // Drag-hover feedback while a drag is over the surface but not yet
    // dropped. Re-evaluates which widget (if any) claims `world` via
    // `input_root_()->dispatch_drag_hover(...)`, firing on_drag_leave on the
    // previous claimant when the claim changes, and requesting a repaint on
    // any change or continued claim (covers a claiming widget's own internal
    // sub-target moving, e.g. between two pack sections). Returns the new
    // claimant, or nullptr if none.
    Widget* dispatch_drag_hover(Point world);

    // Explicit end-of-drag: fires on_drag_leave on the current claimant (if
    // any) and clears it. Call on native drag-leave and after a drop (via
    // dispatch_file_drop), since no further hover events will arrive.
    void dispatch_drag_leave();

    // Hook returning the root widget the dispatch operates on. Each subclass
    // owns its `root_` (a std::unique_ptr<Widget>) and returns `root_.get()`.
    virtual Widget* input_root_() const = 0;

    // Called by request_focus() whenever the newly tk-focused widget does
    // NOT hold_native_focus() (see Widget::holds_native_focus) — i.e. it's
    // a plain canvas widget (Button, ListView, ...) with no real OS control
    // of its own to receive keyboard focus. Backends override this to move
    // real native OS keyboard focus onto their own canvas-hosting container
    // (Qt's Surface, the Win32 HWND, the GTK/macOS window), so native key
    // events (Tab, Enter, ...) keep reaching Host::dispatch_key_down.
    //
    // Without this, Tab-ing from a native text field (which releases real
    // OS focus via its own on_focus_lost() -> set_focused(false) once it's
    // no longer tk-focused) to a plain Button leaves NOTHING holding real
    // OS keyboard focus at all: on_focus_gained() is a no-op for a plain
    // widget, so no native control ever claims it. Reproduced live: Tab
    // from the compose box reaches the emoji button once (forwarded by the
    // text area's own native Tab handling before it let go of focus), then
    // Tab does nothing (no native widget is listening to deliver the key
    // event to Host::dispatch_key_down at all), then Tab again snaps back
    // to the very first Tab stop (whatever picked up stray native focus by
    // then starts traversal over with no tk-widget considered current).
    //
    // No-op default: backends with no such distinction (tests) don't need
    // it — TestHost's fake native controls aren't real OS widgets, so
    // nothing can dangle.
    virtual void claim_native_focus_container_()
    {
    }

    // Take ownership of a subtree removed via Widget::remove_child()/
    // clear_children() (handed off by RootWidget::queue_for_deletion() — the
    // root widget every backend's Surface::set_root() wraps its tree in) and
    // destroy it on the next turn of this host's event loop instead of
    // inline. This is what lets a
    // widget callback (e.g. CheckButton::on_change) safely rebuild its own
    // parent without freeing its own std::function mid-invocation: the
    // subtree is detached immediately (so paint/hit-test never see it
    // again), but the actual free happens after the current dispatch call
    // has fully returned.
    void queue_for_deletion(std::unique_ptr<Widget> subtree);

    // Widget* fields below are tracked via tk::track() (a weak_ptr taken
    // from Widget::self_alive_), not raw pointers, so a stale reference
    // here is always safe to detect via .lock() regardless of when the
    // underlying widget is actually destroyed — whether synchronously or
    // via queue_for_deletion() above.
    std::weak_ptr<Widget> popup_;         // active popup (set after each paint)
    std::weak_ptr<Widget> pending_popup_; // populated during the current paint
    std::weak_ptr<Widget> focus_scope_;   // active Tab-traversal scope, if any

    // Tracked pointer state, shared by the dispatch_pointer_* state machine.
    std::weak_ptr<Widget> pressed_widget_;      // captured on pointer-down
    std::weak_ptr<Button> hovered_btn_;         // Button currently under the pointer
    std::weak_ptr<Widget> hovered_widget_;      // widget currently under the pointer
    std::weak_ptr<Widget> drag_hovered_widget_; // widget currently claiming drag-hover
    std::weak_ptr<Widget> focused_widget_;      // canvas widget holding tk-level keyboard focus

    // True only when the most recent input was keyboard-driven (any key via
    // dispatch_key_down, or a Tab/Shift-Tab forwarded out of a native text
    // control via advance_focus() — see its own doc comment) — false after
    // any canvas mouse click (dispatch_pointer_down). Gates whether
    // paint_focus_overlay() actually draws the ring; tk-level focus itself
    // (focused_widget_) is unaffected either way. Mirrors the web
    // ":focus-visible" pattern: keyboard users still see where focus is,
    // mouse users aren't shown a ring they didn't ask for. Deliberately not
    // set from request_focus()/on_focus_gained() or the native
    // on_focus_changed callback — those fire identically for a genuine
    // click on a native control and for the synchronous echo of a
    // keyboard-driven focus change moving onto it, so they can't be used to
    // tell the two apart (see the TextField ctor's syncing_from_native_
    // comment). Only the raw input-dispatch entry points below are
    // trustworthy.
    bool focus_visible_ = false;

    // Advances focused_widget_ to the next (forward) or previous (!forward)
    // focusable widget in document order, per next_focusable() in widget.h.
    // Returns false (leaving focus unchanged) if the tree has no focusable
    // widget to move to.
    bool advance_focus_(bool forward);

    // Draws the active tooltip (if any) above everything, called by each
    // backend's paint() right after root_->paint_overlay(ctx). `surface_bounds`
    // is the same whole-surface Rect used for that frame's measure/arrange.
    void paint_tooltip_overlay(PaintCtx& ctx, Rect surface_bounds);

    // Draws a focus ring around the currently tk-focused widget (if any),
    // above everything else — called by each backend's paint() at the same
    // site as paint_tooltip_overlay().
    void paint_focus_overlay(PaintCtx& ctx);

    // Clears any active/pending tooltip. Called on every pointer-down (any
    // click anywhere dismisses a tooltip) and when a real popup opens.
    void cancel_tooltip_();

    // Tooltip state — protected (not private) so test fakes derived from
    // Host can assert on it directly (mirrors hovered_widget_ above).
    std::unique_ptr<Tooltip> tooltip_widget_;
    const void* tooltip_owner_    = nullptr;
    std::string tooltip_text_;
    Rect        tooltip_anchor_{};
    bool        tooltip_visible_ = false;   // false = still in its dwell delay
    // Set whenever tooltip_visible_ transitions to true; consumed by
    // paint_tooltip_overlay() to restart Tooltip's entrance reveal exactly
    // once per appearance (set_content() is reasserted every paint frame
    // while visible, so it can't safely detect "just appeared" itself).
    bool        tooltip_reveal_pending_ = false;
    std::uint64_t tooltip_gen_   = 0;
    std::shared_ptr<bool> tooltip_alive_{std::make_shared<bool>(true)};
    static constexpr int kTooltipShowDelayMs = 500;

    // Draws the active toast (if any) above everything, called by each
    // backend's paint() right after paint_focus_overlay(ctx). `surface_bounds`
    // is the same whole-surface Rect used for that frame's measure/arrange
    // (and for paint_tooltip_overlay's own surface_bounds argument).
    void paint_toast_overlay(PaintCtx& ctx, Rect surface_bounds);

    // Toast state — protected (not private), mirroring the tooltip state
    // above, so test fakes derived from Host can assert on it directly.
    std::unique_ptr<Toast> toast_widget_;
    std::string toast_message_;
    bool        toast_visible_ = false;
    std::uint64_t toast_gen_   = 0;
    std::shared_ptr<bool> toast_alive_{std::make_shared<bool>(true)};
    static constexpr int kToastDurationMs = 1500; // matches both prior per-view usages

private:
    std::function<void()> on_user_activity_;
    // Alive-flag: captured as a weak_ptr on the stack before any dispatch
    // callback that may destroy this host synchronously (e.g. hang-up button
    // fires on_hang_up → call_window_.reset() → ~Host). If the weak_ptr
    // expires, the post-callback code skips any `this` dereference.
    std::shared_ptr<bool> dispatch_alive_{std::make_shared<bool>(true)};

    // Backing store for queue_for_deletion(). Drained on the next post_to_ui()
    // turn — see drain_deferred_deletions_().
    void drain_deferred_deletions_();
    std::vector<std::unique_ptr<Widget>> pending_deletions_;
    // Coalescing guard, not a correctness requirement: without it, a single
    // clear_children() call tearing down N children would post N separate
    // drain closures (each one after the first finding an empty queue and
    // no-op'ing) instead of just one.
    bool drain_scheduled_ = false;
};

} // namespace tk
