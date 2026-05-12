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
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace tk::win32 {

class Host;

// Embed `hwnd()` (a child HWND) into your normal Win32 layout, then
// SetWindowPos / MoveWindow it as the parent resizes.
class Surface {
public:
    Surface(HINSTANCE inst, HWND parent,
            const Theme& theme = Theme::light());
    ~Surface();
    Surface(const Surface&)            = delete;
    Surface& operator=(const Surface&) = delete;

    HWND        hwnd() const;
    tk::Host&   host();
    const Theme& theme() const;

    void set_root(std::unique_ptr<Widget> root);
    Widget* root() const;

    // Re-run measure + arrange + repaint on the existing root. WM_SIZE
    // calls this automatically.
    void relayout();

    // Callback fired at the tail of every relayout — use this from
    // integration code to keep native overlays aligned with the widget
    // tree (e.g. SetWindowPos on a child EDIT).
    void set_on_layout(std::function<void()> cb);

    // Borrowed reference to the per-process D2D + DWrite + WIC canvas
    // factory the Surface paints through. Integration code can call
    // factory().decode_image(bytes) to decode media on demand and hand
    // the resulting tk::Image to the shared views' provider lambdas
    // without re-decoding through GDI+.
    CanvasFactory& factory();

    // Install a drag-and-drop handler. When set, the underlying HWND
    // registers itself as an OLE drop target and forwards every dropped
    // file (and in-app image data) to the callback once per file with
    // raw bytes, sniffed mime, and basename. Pass {} to disable.
    // Requires OleInitialize() to have been called on the calling (UI)
    // thread before the first Surface is constructed.
    void set_on_file_drop(FileDropHandler cb);
    // Deprecated alias.
    void set_on_image_drop(FileDropHandler cb) { set_on_file_drop(std::move(cb)); }

private:
    std::unique_ptr<Host> host_;
};

// Wrapper around tk::d2d::decode_animation that uses the per-process
// backend singleton owned by host_win32.cpp. Hosts call this to detect
// + decode animated GIF / APNG / animated WebP without needing to plumb
// a backend reference into application code.
std::vector<tk::d2d::AnimatedFrame> decode_animation(
    std::span<const std::uint8_t> bytes);

} // namespace tk::win32
