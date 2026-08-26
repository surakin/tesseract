#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace win32
{

// Draws and hit-tests a custom Windows 11-style extended title bar: the
// window's non-client caption is zeroed out (see adjust_nccalcsize) and this
// class paints its own replacement strip directly into the top-level HWND's
// own client area (plain GDI, not tk::Canvas) rather than via a child
// Surface HWND. That matters for one reason: WM_NCHITTEST over the maximize
// button has to be serviced by the top-level window itself for Windows 11
// Snap Layouts to work — its hover flyout only appears while the OS keeps
// probing HTMAXBUTTON there, and a child HWND covering that pixel would
// swallow the hit test before the top-level window ever saw it.
//
// Usage: the owning window forwards WM_NCCALCSIZE / WM_NCHITTEST /
// WM_NCMOUSEMOVE / WM_NCMOUSELEAVE / WM_NCLBUTTONDOWN / WM_NCLBUTTONUP /
// WM_DPICHANGED to the matching method below, reserves height_px() at the
// top of its own client-area layout, and paints this strip itself (e.g.
// from its own WM_ERASEBKGND handler, as MainWindow already does for its
// plain background fill).
class CustomTitleBar
{
public:
    static constexpr float kHeightDip = 32.0f;
    static constexpr float kButtonWidthDip = 46.0f;

    ~CustomTitleBar();

    // Creates the offscreen BetterText control paint() renders the window
    // title through (see its doc comment there for why: color-emoji
    // rendering, which plain GDI text APIs can't do). Call once, right
    // after the owning top-level window exists — e.g. alongside the
    // existing on_dpi_changed() call in MainWindow::on_create() /
    // RoomWindow's constructor.
    void attach(HWND owner);

    void on_dpi_changed(UINT dpi);
    int height_px() const { return height_px_; }

    // Releases every cached rasterized caption-button icon (see
    // CustomTitleBar.cpp) — a process-wide cache shared by every
    // CustomTitleBar instance, since the icons are identical regardless of
    // which window draws them. Must be called exactly once, before
    // Gdiplus::GdiplusShutdown(): destroying a Gdiplus::Bitmap after GDI+
    // has shut down hangs/crashes, and this cache would otherwise only be
    // torn down at CRT static-destruction time, which runs after
    // MainWindow::~MainWindow() has already called GdiplusShutdown(). Call
    // from there, right before that call.
    static void shutdown_icon_cache();

    // Call from WM_NCCALCSIZE when wParam == TRUE. Lets DefWindowProcW
    // compute the standard sizing first (this is what preserves normal
    // resize-border feel on the left/right/bottom edges), then resets the
    // top inset back to flush so client content extends up into the old
    // caption area. When the window is maximized, adds back a frame-sized
    // inset at the top only — DefWindowProc's own maximized sizing already
    // over-extends the window rect slightly past the monitor's visible
    // bounds to hide the resize border, and zeroing the top unconditionally
    // would let our own content spill into that hidden strip.
    void adjust_nccalcsize(HWND hwnd, WPARAM wParam, LPARAM lParam) const;

    // Give DWM first refusal on non-client mouse messages so Snap Layouts'
    // hover flyout keeps working. Call at the top of the WM_NCHITTEST /
    // WM_NCLBUTTONDOWN / WM_NCLBUTTONUP handlers; if this returns a value,
    // return it immediately without further handling.
    static std::optional<LRESULT> dwm_hittest_hook(HWND hwnd, UINT msg,
                                                    WPARAM wParam,
                                                    LPARAM lParam);

    // lParam is WM_NCHITTEST's screen-coordinate point. Returns HTMINBUTTON
    // / HTMAXBUTTON / HTCLOSE / HTCAPTION for a point inside the strip, or
    // HTNOWHERE outside it — callers should fall back to DefWindowProcW
    // in that case (this covers the resize-border edges/corners, which are
    // untouched by this class).
    LRESULT handle_nchittest(HWND hwnd, LPARAM lParam) const;

    // ht is the HT* code carried by the message (WM_NCMOUSEMOVE's wParam)
    // or most recently returned by handle_nchittest (WM_NCLBUTTONDOWN/UP's
    // wParam carries the same code the preceding WM_NCHITTEST returned).
    void handle_ncmousemove(HWND hwnd, WPARAM ht);
    void handle_ncmouseleave(HWND hwnd);
    // Returns true when ht is one of our three buttons (caller should
    // return 0 without falling back to DefWindowProcW). No SetCapture is
    // used — Windows would redirect subsequent messages to client-area
    // WM_MOUSEMOVE/WM_LBUTTONUP if we captured, breaking the NC-message
    // flow this depends on — so a press only fires its action if the
    // matching WM_NCLBUTTONUP lands back on the same button, which is
    // enough for ordinary clicks and degrades gracefully (simply does
    // nothing) if the user drags off before releasing.
    bool handle_nclbuttondown(HWND hwnd, WPARAM ht);
    bool handle_nclbuttonup(HWND hwnd, WPARAM ht);

    // DefWindowProc no longer auto-shows the system menu on right-click once
    // WM_NCCALCSIZE has zeroed the caption's non-client rect — this
    // re-implements it manually. Call from WM_NCRBUTTONUP with its wParam
    // (the hit-test code, same convention as WM_NCLBUTTONUP) and lParam
    // (screen coords, used as the popup menu's anchor point). Returns true
    // (menu shown) only when ht is HTCAPTION.
    bool show_system_menu(HWND hwnd, WPARAM ht, LPARAM lParam) const;

    // client_rc is the window's full client rect (as from GetClientRect);
    // only the top height_px() of it is painted. Not const: drives the
    // owned BetterText title-text control (position, content, capture).
    void paint(HDC hdc, HWND hwnd, const RECT& client_rc, bool active);

    // Forces a full repaint of the strip. The button rects are anchored to
    // the right edge (see compute_button_rects), so on a live resize only
    // the newly-exposed sliver is invalidated by default — the old button
    // position, now stranded mid-strip, is left un-repainted and ghosts
    // until the drag ends and something finally invalidates the rest.
    // Call from the owner's WM_SIZE handler on every resize, not just
    // hover/press changes (which invalidate_strip() already covers).
    void invalidate_strip(HWND hwnd) const;

private:
    void compute_button_rects(int client_w_px, RECT& btn_min, RECT& btn_max,
                              RECT& btn_close) const;

    UINT hovered_ht_ = HTNOWHERE;
    UINT pressed_ht_ = HTNOWHERE;
    UINT dpi_ = 96;
    int height_px_ = 32;

    // Offscreen BetterText control rendering the title text — see attach()
    // and paint(). paint() is called on every strip repaint, including
    // every hover-move over the caption buttons (see invalidate_strip()),
    // so it must not touch text_hwnd_ at all unless the text/rect/color
    // actually changed since last time: resizing it (even to the same
    // size — SetWindowPos still dispatches WM_SIZE) makes BetterText tear
    // down and recreate its render target unconditionally, and re-driving
    // BetterTextSetText/SetTheme + a capture round-trip on every mouse-move
    // tick was the actual cause of visible flicker. last_bitmap_* caches
    // the most recent capture so an unrelated repaint just re-blits it.
    HWND text_hwnd_ = nullptr;
    std::wstring last_text_;
    RECT last_rect_{-1, -1, -1, -1};
    COLORREF last_fg_ = 0xFFFFFFFFu; // sentinel; no real COLORREF has bit 24 set
    std::vector<std::uint8_t> last_bitmap_pixels_;
    int last_bitmap_w_ = 0;
    int last_bitmap_h_ = 0;
    bool last_bitmap_valid_ = false;
};

} // namespace win32
