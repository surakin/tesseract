#pragma once

// Cross-platform date-picker popup widget. Used by RoomHeader to implement
// the jump-to-date affordance without a per-platform native dialog.
//
// Usage:
//   1. Call open_at(world_rect) to position the picker before showing it.
//   2. Call set_max_date() to restrict selectable dates (defaults to today).
//   3. During RoomHeader::paint(), call ctx.host->register_popup(picker.get())
//      to register it as the active popup for this frame.
//   4. The host then calls paint_overlay() after the tree paint and routes
//      pointer events through the Widget::dispatch_pointer_* methods.
//   5. Wire on_date_picked / on_dismiss to react to user actions.

#include "tk/animator.h"
#include "tk/widget.h"

#include <tesseract/visual.h>

#include <array>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class DatePickerView : public tk::Widget
{
public:
    // Popup card dimensions (logical px).
    static constexpr float kWidth  = 256.0f;
    static constexpr float kHeight = 296.0f;

    DatePickerView();
    ~DatePickerView() override = default;

    // Position the popup at the given world rect and reset hover/press state.
    // Call once before making the picker visible each time it opens.
    void open_at(tk::Rect world_rect);

    // Restrict selectable dates to at most (year, month, day) inclusive.
    // Defaults to today (set on the first open_at() call if never called).
    void set_max_date(int year, int month, int day);

    // Get today's local date — exposed so callers (RoomHeader) can pass it
    // to set_max_date without duplicating the platform-agnostic logic.
    static void today(int& year, int& month, int& day);

    // Fired when the user confirms a specific date.
    std::function<void(int year, int month, int day)> on_date_picked;

    // Fired when the picker should close without selecting a date
    // (click outside or on_popup_dismiss).
    std::function<void()> on_dismiss;

    // ── tk::Widget overrides ─────────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    // paint() is never called in normal use (not a tree child), but the base
    // interface requires an implementation.
    void     paint(tk::PaintCtx&) override {}
    // All visible drawing lives here; called by the host as the registered popup.
    void     paint_overlay(tk::PaintCtx&) override;
    bool     contains_world(tk::Point world) const override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;
    bool     on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;
    void     on_popup_dismiss() override;
    // Reached via Host's popup-first-refusal path while this picker is the
    // registered popup (see class comment) — not via Tab traversal, since
    // this widget is never a tree child. Left/Right/Up/Down move the
    // highlighted day (reusing hovered_cell_ so paint_overlay's existing
    // hover-highlight logic draws it for free); PageUp/PageDown change
    // month; Enter/Space picks the highlighted day; Escape dismisses.
    bool     on_key_down(const tk::KeyEvent& event) override;

private:
    // ── layout constants ──────────────────────────────────────────────────────
    static constexpr float kHeaderH  = 44.0f;
    static constexpr float kDowH     = 24.0f;
    static constexpr float kDayH     = 32.0f;
    static constexpr int   kRows     = 6;
    static constexpr int   kCols     = 7;
    static constexpr float kFooterH  = 36.0f;
    static constexpr float kNavBtnSz = 28.0f;
    static constexpr float kNavBtnPad = 6.0f;
    static constexpr float kCardRadius = tesseract::visual::kRadiusMD;
    // Circular cell highlight: inscribed circle within the cell, with 2px margin.
    static constexpr float kCellCircleD = kDayH - 4.0f;  // 28 px diameter

    // ── date state ────────────────────────────────────────────────────────────
    int view_year_  = 0;  // currently displayed month (0 = uninitialised)
    int view_month_ = 0;  // 1-based
    int max_year_   = 9999;
    int max_month_  = 12;
    int max_day_    = 31;

    // ── cell model ────────────────────────────────────────────────────────────
    struct CellInfo
    {
        int  year   = 0;
        int  month  = 0;  // 1-based
        int  day    = 0;
        bool in_month = false; // belongs to view_month_ (not prev/next spill)
        bool enabled  = false; // clickable (in_month, <= max, >= 1970-01-01)
        bool is_today = false;
    };
    std::array<CellInfo, kRows * kCols> cells_{};

    // ── hover / press state ───────────────────────────────────────────────────
    enum class Zone { None, PrevBtn, NextBtn, DayCell, TodayBtn };
    Zone pressed_zone_ = Zone::None;
    int  pressed_cell_ = -1;
    Zone hovered_zone_ = Zone::None;

    // Opacity entrance reveal, restarted each time open_at() is called.
    tk::FloatTween reveal_{1.0f};
    int  hovered_cell_ = -1;

    // ── layout zones (world coords, set in open_at / compute_zones_) ─────────
    tk::Rect header_rect_  {};
    tk::Rect prev_btn_rect_{};
    tk::Rect next_btn_rect_{};
    tk::Rect dow_rect_     {};
    tk::Rect grid_rect_    {};
    tk::Rect footer_rect_  {};
    tk::Rect today_btn_rect_{};

    // ── text layout cache ─────────────────────────────────────────────────────
    // Invalidated when the CanvasFactory pointer changes (theme switch /
    // DPI change on some backends).
    tk::CanvasFactory* last_factory_ = nullptr;
    // Slots 1-7: "Su" … "Sa" day-of-week labels — built once.
    // Slot 8: "Today" footer button label — built once.
    // (Slot 0 is unused; month + year are in separate layouts below.)
    static constexpr int kLayoutSlots = 9;
    std::array<std::unique_ptr<tk::TextLayout>, kLayoutSlots> layouts_{};
    // Month name ("June") and year ("2025") drawn separately in the header so
    // wheel-over-year can navigate years independently of wheel-over-month.
    std::unique_ptr<tk::TextLayout> month_layout_;
    std::unique_ptr<tk::TextLayout> year_layout_;
    // World-space rect of the year text (updated each paint_overlay call).
    tk::Rect year_label_rect_{};
    // Per-cell day-number labels — rebuilt by rebuild_cells_() on navigation.
    std::array<std::unique_ptr<tk::TextLayout>, kRows * kCols> cell_layouts_{};
    // Navigation glyph layouts (‹ / ›) — built once, reused.
    std::unique_ptr<tk::TextLayout> nav_prev_layout_;
    std::unique_ptr<tk::TextLayout> nav_next_layout_;
    // Fractional wheel-scroll accumulator for stepped month/year navigation.
    float wheel_accum_ = 0.0f;

    // ── helpers ───────────────────────────────────────────────────────────────
    static int days_in_month(int year, int month);
    // 0=Sun … 6=Sat for the 1st day of (year, month), via mktime().
    static int first_weekday(int year, int month);

    // Fill cells_[] and cell_layouts_[] for the current view_year_/view_month_.
    void rebuild_cells_(tk::CanvasFactory& factory);

    // Recompute zone rects from the current bounds_.
    void compute_zones_();

    // Hit-test helpers: take LOCAL coords (relative to bounds_).
    Zone hit_zone(tk::Point local, int* cell_idx_out) const;

    // Rect (world coords) for cell index i in cells_.
    tk::Rect cell_world_rect(int i) const;
    // Circular highlight rect centred inside a cell world rect.
    tk::Rect circle_rect_in(tk::Rect cell) const;

    // Whether today's date is selectable (within max_date).
    bool today_enabled() const;

    // Helper: ensure all static layouts (DOW labels, "Today", nav glyphs)
    // have been built with the given factory, and rebuild on factory change.
    void ensure_layouts_(tk::CanvasFactory& factory);
};

} // namespace tesseract::views
