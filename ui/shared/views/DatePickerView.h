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

#include "tk/access_tree.h"
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

class DatePickerView : public tk::Widget, public tk::WidgetRowAccessibility
{
public:
    // Drops every layout (they're rebuilt when the factory looks new).
    void on_fonts_changed() override
    {
        last_factory_ = nullptr;
    }

    // Popup card dimensions (logical px).
    static constexpr float kWidth = 256.0f;
    static constexpr float kHeight = 296.0f;

    DatePickerView();
    ~DatePickerView() override = default;

    // Position the popup at the given world rect and reset hover/press state.
    // Call once before making the picker visible each time it opens.
    void open_at(tk::Rect world_rect);

    // Restrict selectable dates to at most (year, month, day) inclusive.
    // Defaults to today (set on the first open_at() call if never called).
    // Re-clamps the shown month and keyboard cursor into the new range.
    void set_max_date(int year, int month, int day);

    // Forget the shown month and cursor so the next open_at() starts on
    // today, clamped to the max date (e.g. when the header switches rooms).
    void reset_view();

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
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    // paint() is never called in normal use (not a tree child), but the base
    // interface requires an implementation.
    void paint(tk::PaintCtx&) override
    {
    }
    // All visible drawing lives here; called by the host as the registered popup.
    void paint_overlay(tk::PaintCtx&) override;
    bool contains_world(tk::Point world) const override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;
    void on_popup_dismiss() override;
    // Reached via Host's popup-first-refusal path while this picker is the
    // registered popup (see class comment) — not via Tab traversal, since
    // this widget is never a tree child, so the picker runs its own
    // internal focus model:
    //   Tab / Shift-Tab   cycle ‹ · month · year · › · day grid · Today
    //   day grid          arrows move a day/week (crossing months),
    //                     Home/End start/end of the week
    //   month / year      arrows (or PageUp/PageDown) step that field
    //   ‹ › Today         Enter/Space activate
    //   anywhere          PageUp/PageDown ±1 month, Shift+PageUp/PageDown
    //                     ±1 year, T jumps to today, Escape dismisses;
    //                     Enter/Space on the grid picks the focused day
    bool on_key_down(const tk::KeyEvent& event) override;

    // Which internal part holds keyboard focus (see on_key_down). Public
    // for tests.
    enum class FocusPart
    {
        PrevBtn,
        Month,
        Year,
        NextBtn,
        Grid,
        TodayBtn
    };
    FocusPart focus_part() const
    {
        return focus_part_;
    }
    // The keyboard cursor date on the grid (also what Enter picks). Public
    // for tests.
    int cursor_year() const
    {
        return cursor_year_;
    }
    int cursor_month() const
    {
        return cursor_month_;
    }
    int cursor_day() const
    {
        return cursor_day_;
    }
    // Currently displayed month. Public for tests.
    int view_year() const
    {
        return view_year_;
    }
    int view_month() const
    {
        return view_month_;
    }

    // ── Accessibility ─────────────────────────────────────────────────────
    // A grid named by the shown month ("October 2026"): rows are the
    // previous/next-month buttons, the current month's day cells (6×7 grid
    // positions, the keyboard highlight as `selected`), and "Today".
    tk::Role access_role() const override
    {
        return tk::Role::Grid;
    }
    std::string access_name() const override;
    std::size_t access_row_count() const override;
    tk::Role access_role_for_widget_row(std::size_t i) const override;
    std::string access_name_for_widget_row(std::size_t i) const override;
    std::string access_description_for_widget_row(std::size_t i) const override;
    tk::AccessState access_state_for_widget_row(std::size_t i) const override;
    bool access_activate_widget_row(std::size_t i) override;
    tk::Rect access_rect_for_widget_row(std::size_t i) const override;
    std::pair<int, int> access_grid_cell_for_widget_row(std::size_t i) const override;
    std::pair<int, int> access_grid_size() const override
    {
        return {kRows, kCols};
    }

private:
    // ── layout constants ──────────────────────────────────────────────────────
    static constexpr float kHeaderH = 44.0f;
    static constexpr float kDowH = 24.0f;
    static constexpr float kDayH = 32.0f;
    static constexpr int kRows = 6;
    static constexpr int kCols = 7;
    static constexpr float kFooterH = 36.0f;
    static constexpr float kNavBtnSz = 28.0f;
    static constexpr float kNavBtnPad = 6.0f;
    static constexpr float kCardRadius = tesseract::visual::kRadiusMD;
    // Circular cell highlight: inscribed circle within the cell, with 2px margin.
    static constexpr float kCellCircleD = kDayH - 4.0f; // 28 px diameter

    // ── date state ────────────────────────────────────────────────────────────
    int view_year_ = 0;  // currently displayed month (0 = uninitialised)
    int view_month_ = 0; // 1-based
    int max_year_ = 9999;
    int max_month_ = 12;
    int max_day_ = 31;

    // ── cell model ────────────────────────────────────────────────────────────
    struct CellInfo
    {
        int year = 0;
        int month = 0; // 1-based
        int day = 0;
        bool in_month = false; // belongs to view_month_ (not prev/next spill)
        bool enabled = false;  // clickable (in_month, <= max, >= 1970-01-01)
        bool is_today = false;
    };
    std::array<CellInfo, kRows * kCols> cells_{};

    // ── keyboard state ────────────────────────────────────────────────────────
    FocusPart focus_part_ = FocusPart::Grid;
    int cursor_year_ = 0, cursor_month_ = 0, cursor_day_ = 0;
    // Set by the first key press after open_at(); gates the focus ring so a
    // mouse-opened picker shows no keyboard cursor.
    bool kbd_active_ = false;

    // ── hover / press state ───────────────────────────────────────────────────
    enum class Zone
    {
        None,
        PrevBtn,
        NextBtn,
        DayCell,
        TodayBtn
    };
    Zone pressed_zone_ = Zone::None;
    int pressed_cell_ = -1;
    Zone hovered_zone_ = Zone::None;

    // Opacity entrance reveal, restarted each time open_at() is called.
    tk::FloatTween reveal_{1.0f};
    int hovered_cell_ = -1;

    // ── layout zones (world coords, set in open_at / compute_zones_) ─────────
    tk::Rect header_rect_{};
    tk::Rect prev_btn_rect_{};
    tk::Rect next_btn_rect_{};
    tk::Rect dow_rect_{};
    tk::Rect grid_rect_{};
    tk::Rect footer_rect_{};
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
    tk::CachedTextLayout month_layout_;
    tk::CachedTextLayout year_layout_;
    // World-space rects of the month / year text (updated each
    // paint_overlay call).
    tk::Rect month_label_rect_{};
    tk::Rect year_label_rect_{};
    // Per-cell day-number labels — rebuilt by rebuild_cells_() on navigation.
    std::array<std::unique_ptr<tk::TextLayout>, kRows * kCols> cell_layouts_{};
    // Navigation glyph layouts (‹ / ›) — built once, reused.
    tk::CachedTextLayout nav_prev_layout_;
    tk::CachedTextLayout nav_next_layout_;
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
    // Shift the shown month by -1 / +1 within [1970-01, max]; true if it moved.
    bool step_month_(int dir);
    // Move the keyboard cursor to (y, m, d), clamped into the selectable
    // range [1970-01-01, max date], and show its month.
    void set_cursor_(int y, int m, int d);
    // Shift the cursor by whole days (crossing month/year boundaries).
    void move_cursor_days_(int days);
    // Shift the cursor by whole months / years, clamping the day to the
    // target month's length.
    void move_cursor_months_(int months);
    // Cell index of the cursor in the shown month, or -1.
    int cursor_cell_() const;
    // Whether a FocusPart can take keyboard focus right now (‹/› at the
    // range ends and a disabled Today are skipped by Tab).
    bool part_enabled_(FocusPart p) const;
    void paint_keyboard_focus_(tk::PaintCtx& ctx);
    void pick_cell_(int cell);
    void pick_today_();

    // Helper: ensure all static layouts (DOW labels, "Today", nav glyphs)
    // have been built with the given factory, and rebuild on factory change.
    void ensure_layouts_(tk::CanvasFactory& factory);
};

// Milliseconds since the Unix epoch at local midnight of a calendar date: the
// start of that day for the user. The picker works in the user's calendar, so
// a jump to a picked date must not start at UTC midnight. 0 when the date
// cannot be represented.
std::uint64_t local_midnight_ms(int year, int month, int day);

} // namespace tesseract::views
