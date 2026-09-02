#pragma once

// Win32 host. The Surface owns a child HWND that paints into a D2D
// HwndRenderTarget (managed by tk::d2d::Surface) and creates native EDIT
// child windows for NativeTextField overlays. Mouse events are dispatched
// from the surface's WndProc; off-thread completions land on the UI
// thread through PostMessage with a process-wide registered window
// message carrying a heap-allocated std::function.

#include "canvas.h"
#include "canvas_d2d.h"
#include "host.h"
#include "theme.h"
#include "widget.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

class IBetterTextFontProvider;

namespace tk
{
class AnimImageCache;
}

namespace tk::win32
{

class Host;

// UTF-8 <-> UTF-16 conversion helpers, shared across the Win32 backend
// (host_win32.cpp, win32_accessible.cpp, ...). Declared here (rather than
// file-local anonymous-namespace copies) so a unity build doesn't merge two
// same-named internal-linkage definitions from different .cpp files into one
// translation unit. Defined once in host_win32.cpp.
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);

// Pointer-shape requests routed from the application code (e.g. "change
// the cursor while hovering a hyperlink"). The Surface handles the platform
// detail of keeping the cursor sticky across WM_SETCURSOR; callers just
// flip between Default and Pointer.
//
// IBeam: canvas-drawn-text spike — BetterTextField/BetterTextArea's real
// hwnd_ is excluded from Win32's own input hit-testing (SetWindowRgn with
// an empty region — see the ctor comment in host_win32.cpp), so the OS
// never asks it for a cursor via WM_SETCURSOR the way a normal hovered
// child window would. set_hovering() on those controls requests this
// instead, routed through the same sticky-across-WM_SETCURSOR mechanism
// already used for hyperlinks.
enum class Cursor
{
    Default,
    Pointer,
    IBeam,
};

// Embed `hwnd()` (a child HWND) into your normal Win32 layout, then
// SetWindowPos / MoveWindow it as the parent resizes.
class Surface
{
public:
    // transparent=true creates the HWND with WS_EX_NOREDIRECTIONBITMAP and
    // uses a DXGI_ALPHA_MODE_PREMULTIPLIED swap chain so DWM composites the
    // window's per-pixel alpha against the content behind it. The host clears
    // each frame to {0,0,0,0} instead of palette.bg; the widget tree is
    // responsible for painting whatever it wants to be visible.
    Surface(HINSTANCE inst, HWND parent, const Theme& theme = Theme::light(),
            bool transparent = false);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    HWND hwnd() const;
    tk::Host& host();
    const Theme& theme() const;

    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Physical-pixel-to-DIP scale factor for this surface's monitor
    // (GetDpiForWindow(hwnd()) / 96). The widget tree (and so AccessNode::
    // rect) is in DIPs; a caller mapping to physical screen coordinates
    // (e.g. the accessibility bridge's ClientToScreen-based rect mapping)
    // needs this scale factor and has no other way to reach it, since the
    // equivalent internal Host::dpi_scale() is private to this pimpl.
    float dpi_scale() const;

    // Re-run measure + arrange + repaint on the existing root. WM_SIZE
    // calls this automatically.
    void relayout();
    void set_theme(const Theme& t);

    // Pushes `scale` through the whole widget tree via
    // Widget::apply_scale_change() (see widget.h) — call from WM_DPICHANGED
    // so native-control image captures (tk::NativeTextField/NativeTextArea)
    // don't stay stale/blurry.
    void apply_scale_change(float scale);

    // Fired at the tail of apply_scale_change() above, with the new scale.
    // Lets integration code (the owning shell) track the display's current
    // scale — see ShellBase::set_current_scale_()'s doc comment — without
    // needing its own separate DPI-change plumbing.
    void set_on_scale_changed(std::function<void(float)> cb);

    // Animated-image partial repaints. Point the surface at the shell's
    // animation cache once at setup; then call update_anim_regions() from
    // the animation timer to invalidate only the rects where animated images
    // were drawn on the last paint.
    void set_anim_cache(const tk::AnimImageCache* cache);
    void update_anim_regions();

    // Callback fired at the tail of every relayout — use this from
    // integration code to keep native overlays aligned with the widget
    // tree (e.g. SetWindowPos on a child EDIT). Single-slot: a second
    // caller silently replaces the first's callback, so use this only
    // where a Surface is known to have exactly one interested subsystem.
    void set_on_layout(std::function<void()> cb);

    // Same trigger point as set_on_layout, but additive — for a subsystem
    // that can't assume it's the only thing interested in relayout (e.g.
    // the accessibility bridge, which must coexist with whatever a given
    // Surface's owner already wired via set_on_layout).
    void add_layout_listener(std::function<void()> cb);

    // Borrowed reference to the per-process D2D + DWrite + WIC canvas
    // factory the Surface paints through. Integration code can call
    // factory().decode_image(bytes) to decode media on demand and hand
    // the resulting tk::Image to the shared views' provider lambdas
    // without re-decoding through GDI+.
    CanvasFactory& factory();

    // Called when a drop fails because the file could not be read.
    void set_on_file_drop_error(FileDropErrorHandler cb);

    // Install a right-click handler. Receives surface-local coordinates in
    // the same logical pixel space as pointer-down/up events. Fired on
    // WM_RBUTTONUP. Pass {} to clear.
    void set_on_right_click(std::function<void(tk::Point)> cb);

    // Request a pointer shape for the surface's client area. The Surface's
    // WM_SETCURSOR handler reapplies this on every cursor query, so the
    // change persists across mouse moves without callers having to deal
    // with the per-message Win32 cursor protocol.
    void set_cursor(Cursor c);

private:
    std::unique_ptr<Host> host_;
    std::function<void(float)> on_scale_changed_;
};

// Process-wide D2D backend. decode_image creates its own per-call WIC factory
// in the calling thread's MTA apartment, so it is safe to call from any worker
// thread without STA message-pump involvement. decode_animation still uses the
// backend's shared factory (see canvas_d2d.h) and is only called from
// dedicated threads that initialise COM themselves.
tk::d2d::Backend& backend_singleton();

// Wrapper around tk::d2d::decode_animation that uses the per-process
// backend singleton owned by host_win32.cpp. Hosts call this to detect
// + decode animated GIF / APNG / animated WebP without needing to plumb
// a backend reference into application code.
std::vector<tk::d2d::AnimatedFrame>
decode_animation(std::span<const std::uint8_t> bytes);

// The same IBetterTextFontProvider every BetterTextField/BetterTextArea in
// this app uses (see host_win32.cpp) — routes BetterText's emoji glyph
// fallback to the app's own bundled Noto Color Emoji font/collection
// instead of BetterText's OS-resolved default, so any BetterText-backed
// control (including a static one — see BetterText.h's
// BetterTextSetStatic) renders emoji identically to every other one.
// Stateless singleton; safe to call BetterTextSetFontProvider(hwnd,
// &noto_emoji_font_provider()) any time after the first Surface exists.
IBetterTextFontProvider& noto_emoji_font_provider();

} // namespace tk::win32
