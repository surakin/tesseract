#include "DatePickerView.h"
#include "shortcut_registry.h"

#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iterator>
#include <optional>
#include <sstream>

namespace tesseract::views
{

namespace
{


// UTF-8 single angle quotation marks used as prev/next navigation glyphs.
static constexpr char kPrevGlyph[] = "\xE2\x80\xB9"; // U+2039 ‹
static constexpr char kNextGlyph[] = "\xE2\x80\xBA"; // U+203A ›

} // namespace

// ── static date helpers ───────────────────────────────────────────────────────

/*static*/ void DatePickerView::today(int& year, int& month, int& day)
{
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    year  = lt->tm_year + 1900;
    month = lt->tm_mon  + 1;
    day   = lt->tm_mday;
}

/*static*/ int DatePickerView::days_in_month(int year, int month)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    if (month == 2)
    {
        const bool leap =
            (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

/*static*/ int DatePickerView::first_weekday(int year, int month)
{
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = 1;
    std::mktime(&t);
    return t.tm_wday; // 0=Sun … 6=Sat
}

// ── construction ─────────────────────────────────────────────────────────────

DatePickerView::DatePickerView()
{
    // Initialise max_date to "today" — open_at() will refine this if
    // set_max_date() has not been called before the first show.
    today(max_year_, max_month_, max_day_);
}

// ── public interface ─────────────────────────────────────────────────────────

void DatePickerView::set_max_date(int year, int month, int day)
{
    max_year_  = year;
    max_month_ = month;
    max_day_   = day;
    // A cap that moves while the picker is open (or between opens) must pull
    // the shown month and cursor back inside it; set_cursor_ clamps.
    if (cursor_year_ != 0)
        set_cursor_(cursor_year_, cursor_month_, cursor_day_);
}

void DatePickerView::reset_view()
{
    view_year_ = view_month_ = 0;
    cursor_year_ = cursor_month_ = cursor_day_ = 0;
}

void DatePickerView::open_at(tk::Rect world_rect)
{
    bounds_ = world_rect;
    compute_zones_();

    // Initialise view to the current month on first open.
    if (view_year_ == 0)
    {
        int ty, tm, td;
        today(ty, tm, td);
        view_year_  = ty;
        view_month_ = tm;
        (void)td;
    }

    // Keyboard cursor: today when today's month is shown, else the 1st.
    // Focus starts on the grid; the ring stays hidden until a key press.
    {
        int ty, tm, td;
        today(ty, tm, td);
        if (view_year_ == ty && view_month_ == tm)
            set_cursor_(ty, tm, td);
        else
            set_cursor_(view_year_, view_month_, 1);
    }
    focus_part_ = FocusPart::Grid;
    kbd_active_ = false;

    // Reset interaction state.
    hovered_zone_ = Zone::None;
    hovered_cell_ = -1;
    pressed_zone_ = Zone::None;
    pressed_cell_ = -1;
    wheel_accum_  = 0.0f;
    reveal_.reset(0.0f);
    reveal_.set_target(1.0f);

    // Invalidate per-cell layouts so they're rebuilt on first paint.
    for (auto& l : cell_layouts_)
        l.reset();
    // Header label must be rebuilt since it may reflect a new month.
    layouts_[0].reset();
}

// ── tk::Widget overrides ─────────────────────────────────────────────────────

tk::Size DatePickerView::measure(tk::LayoutCtx&, tk::Size)
{
    return {kWidth, kHeight};
}

void DatePickerView::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    compute_zones_();
}

bool DatePickerView::contains_world(tk::Point world) const
{
    return world.x >= bounds_.x && world.x < bounds_.x + bounds_.w &&
           world.y >= bounds_.y && world.y < bounds_.y + bounds_.h;
}

void DatePickerView::paint_overlay(tk::PaintCtx& ctx)
{
    if (bounds_.w <= 0.0f || bounds_.h <= 0.0f)
        return;

    // Rebuild text layouts if the factory changed (theme switch / DPI change).
    ensure_layouts_(ctx.factory);

    // Populate cells if needed (first paint, or after navigation).
    bool cells_need_build = (cells_[0].year == 0 || cells_[0].month != view_month_);
    if (cells_need_build)
        rebuild_cells_(ctx.factory);

    const auto& pal = ctx.theme.palette;
    auto& c = ctx.canvas;

    constexpr float kRevealMs = 110.0f;
    const float reveal_t = reveal_.step(kRevealMs);
    const bool revealing = reveal_t < 1.0f;
    if (reveal_.still_animating())
    {
        if (auto* h = host()) h->request_repaint();
    }
    std::optional<tk::OpacityScope> fade;
    if (revealing)
    {
        fade.emplace(c, reveal_t);
    }
    tk::ClipScope clip(c, bounds_);

    // ── card background + border ──────────────────────────────────────────────
    c.fill_rounded_rect(bounds_, kCardRadius, pal.chrome_bg);
    c.stroke_rounded_rect(bounds_, kCardRadius, pal.popup_border, 1.0f);

    // ── header: prev button / month-year label / next button ─────────────────
    {
        // Prev button
        const bool can_prev = !(view_year_ == 1970 && view_month_ == 1);
        const bool prev_hov = hovered_zone_ == Zone::PrevBtn;
        const bool prev_prs = pressed_zone_ == Zone::PrevBtn;
        if (can_prev)
        {
            if (prev_prs)
                c.fill_rounded_rect(prev_btn_rect_, 4.0f, pal.subtle_pressed);
            else if (prev_hov)
                c.fill_rounded_rect(prev_btn_rect_, 4.0f, pal.subtle_hover);
        }
        if (nav_prev_layout_)
        {
            const tk::Color nav_col = can_prev ? pal.text_primary : pal.text_muted;
            const tk::Size  gsz     = nav_prev_layout_->measure();
            c.draw_text(*nav_prev_layout_,
                        {prev_btn_rect_.x + (prev_btn_rect_.w - gsz.w) * 0.5f,
                         prev_btn_rect_.y + (prev_btn_rect_.h - nav_prev_layout_->ascent()) * 0.5f},
                        nav_col);
        }

        // Next button
        const bool can_next =
            !(view_year_ == max_year_ && view_month_ == max_month_);
        const bool next_hov = hovered_zone_ == Zone::NextBtn;
        const bool next_prs = pressed_zone_ == Zone::NextBtn;
        if (can_next)
        {
            if (next_prs)
                c.fill_rounded_rect(next_btn_rect_, 4.0f, pal.subtle_pressed);
            else if (next_hov)
                c.fill_rounded_rect(next_btn_rect_, 4.0f, pal.subtle_hover);
        }
        if (nav_next_layout_)
        {
            const tk::Color nav_col = can_next ? pal.text_primary : pal.text_muted;
            const tk::Size  gsz     = nav_next_layout_->measure();
            c.draw_text(*nav_next_layout_,
                        {next_btn_rect_.x + (next_btn_rect_.w - gsz.w) * 0.5f,
                         next_btn_rect_.y + (next_btn_rect_.h - nav_next_layout_->ascent()) * 0.5f},
                        nav_col);
        }

        // Month and year labels centred as a pair, with a small gap between them.
        // Storing year_label_rect_ here lets on_wheel distinguish the two zones.
        if (month_layout_ && year_layout_)
        {
            constexpr float kLabelGap = 4.0f;
            const tk::Size msz = month_layout_->measure();
            const tk::Size ysz = year_layout_->measure();
            const float total_w = msz.w + kLabelGap + ysz.w;
            const float start_x = bounds_.x + (kWidth - total_w) * 0.5f;
            const float text_y  = header_rect_.y +
                                  (kHeaderH - month_layout_->ascent()) * 0.5f;
            c.draw_text(*month_layout_, {start_x, text_y}, pal.text_primary);
            month_label_rect_ = {start_x, header_rect_.y, msz.w, kHeaderH};
            const float year_x = start_x + msz.w + kLabelGap;
            c.draw_text(*year_layout_,  {year_x, text_y}, pal.text_primary);
            year_label_rect_ = {year_x, header_rect_.y, ysz.w, kHeaderH};
        }

        // Thin separator below header
        c.fill_rect({bounds_.x, header_rect_.y + kHeaderH - 1.0f, kWidth, 1.0f},
                    pal.separator);
    }

    // ── day-of-week row ───────────────────────────────────────────────────────
    {
        const float cell_w = kWidth / float(kCols);
        for (int col = 0; col < kCols; ++col)
        {
            auto& layout = layouts_[1 + col]; // slots 1-7
            if (!layout)
                continue;
            const tk::Size lsz = layout->measure();
            const float cx =
                bounds_.x + (float(col) + 0.5f) * cell_w - lsz.w * 0.5f;
            const float cy =
                dow_rect_.y + (kDowH - layout->ascent()) * 0.5f;
            c.draw_text(*layout, {cx, cy}, pal.text_secondary);
        }
    }

    // ── day grid ──────────────────────────────────────────────────────────────
    {
        // Current today's date for is_today comparison.
        int ty = 0, tm = 0, td = 0;
        today(ty, tm, td);

        for (int i = 0; i < kRows * kCols; ++i)
        {
            const auto& cell = cells_[i];
            const tk::Rect wr = cell_world_rect(i);
            const tk::Rect cr = circle_rect_in(wr);

            const bool is_hovered =
                hovered_zone_ == Zone::DayCell && hovered_cell_ == i;
            const bool is_pressed =
                pressed_zone_ == Zone::DayCell && pressed_cell_ == i;

            // Determine if this cell is the selected date — for now we don't
            // maintain a persistent selection, so this is only set when the
            // user presses down on an enabled cell (visual feedback).
            const bool is_selected_press = is_pressed && cell.enabled;

            if (is_selected_press)
            {
                c.fill_rounded_rect(cr, kCellCircleD * 0.5f, pal.accent);
            }
            else if (cell.is_today)
            {
                c.stroke_rounded_rect(cr, kCellCircleD * 0.5f, pal.accent, 1.5f);
                if (is_hovered && cell.enabled)
                    c.fill_rounded_rect(cr, kCellCircleD * 0.5f,
                                        pal.accent.with_alpha(40));
            }
            else if (is_hovered && cell.enabled)
            {
                c.fill_rounded_rect(cr, kCellCircleD * 0.5f, tk::hot_row_fill(ctx.theme));
            }

            // Day number text
            if (cell_layouts_[i])
            {
                tk::Color text_col;
                if (is_selected_press)
                    text_col = pal.text_on_accent;
                else if (!cell.enabled || !cell.in_month)
                    text_col = pal.text_muted;
                else if (is_hovered && ctx.theme.high_contrast &&
                         !cell.is_today)
                    text_col = pal.selected_text; // on the Highlight hover fill
                else
                    text_col = pal.text_primary;

                const tk::Size lsz = cell_layouts_[i]->measure();
                c.draw_text(*cell_layouts_[i],
                            {wr.x + (wr.w - lsz.w) * 0.5f,
                             wr.y + (kDayH - cell_layouts_[i]->ascent()) * 0.5f},
                            text_col);
            }
        }
    }

    // ── footer: Today button ──────────────────────────────────────────────────
    {
        const bool te = today_enabled();
        const bool t_hov = hovered_zone_ == Zone::TodayBtn;
        const bool t_prs = pressed_zone_ == Zone::TodayBtn;
        if (te)
        {
            if (t_prs)
                c.fill_rounded_rect(today_btn_rect_, 4.0f, pal.subtle_pressed);
            else if (t_hov)
                c.fill_rounded_rect(today_btn_rect_, 4.0f, pal.subtle_hover);
        }
        // Thin separator above footer
        c.fill_rect({bounds_.x, footer_rect_.y, kWidth, 1.0f}, pal.separator);
        if (layouts_[8]) // "Today"
        {
            const tk::Color tc = te ? pal.accent : pal.text_muted;
            const tk::Size  lsz = layouts_[8]->measure();
            c.draw_text(*layouts_[8],
                        {today_btn_rect_.x + (today_btn_rect_.w - lsz.w) * 0.5f,
                         today_btn_rect_.y +
                             (today_btn_rect_.h - layouts_[8]->ascent()) * 0.5f},
                        tc);
        }
    }

    if (kbd_active_)
        paint_keyboard_focus_(ctx);
}

void DatePickerView::paint_keyboard_focus_(tk::PaintCtx& ctx)
{
    // Header labels get a ring hugging the text line rather than the full
    // header height.
    const auto label_ring = [this](tk::Rect r)
    {
        constexpr float kRingH = 26.0f;
        return tk::Rect{r.x - 4.0f, header_rect_.y + (kHeaderH - kRingH) * 0.5f,
                        r.w + 8.0f, kRingH};
    };
    switch (focus_part_)
    {
    case FocusPart::PrevBtn: tk::paint_focus_ring(ctx, prev_btn_rect_); break;
    case FocusPart::NextBtn: tk::paint_focus_ring(ctx, next_btn_rect_); break;
    case FocusPart::TodayBtn: tk::paint_focus_ring(ctx, today_btn_rect_); break;
    case FocusPart::Month:
        if (month_label_rect_.w > 0.0f)
            tk::paint_focus_ring(ctx, label_ring(month_label_rect_));
        break;
    case FocusPart::Year:
        if (year_label_rect_.w > 0.0f)
            tk::paint_focus_ring(ctx, label_ring(year_label_rect_));
        break;
    case FocusPart::Grid:
        if (const int cell = cursor_cell_(); cell >= 0)
        {
            const tk::Rect cr = circle_rect_in(cell_world_rect(cell));
            tk::paint_focus_ring(ctx, cr, kCellCircleD * 0.5f);
        }
        break;
    }
}

// ── pointer input ─────────────────────────────────────────────────────────────

bool DatePickerView::on_pointer_down(tk::Point local)
{
    int cell = -1;
    Zone z   = hit_zone(local, &cell);
    pressed_zone_ = z;
    pressed_cell_ = cell;
    return z != Zone::None; // claim if we hit anything
}

void DatePickerView::on_pointer_up(tk::Point local, bool inside_self)
{
    const Zone pz = pressed_zone_;
    const int  pc = pressed_cell_;
    pressed_zone_ = Zone::None;
    pressed_cell_ = -1;

    if (!inside_self)
        return;

    int cell = -1;
    Zone z   = hit_zone(local, &cell);
    if (z != pz || cell != pc)
        return; // released on different target — cancel

    if (z == Zone::PrevBtn)
        step_month_(-1);
    else if (z == Zone::NextBtn)
        step_month_(1);
    else if (z == Zone::DayCell)
        pick_cell_(cell);
    else if (z == Zone::TodayBtn)
        pick_today_();
}

bool DatePickerView::step_month_(int dir)
{
    if (dir < 0)
    {
        if (view_year_ == 1970 && view_month_ == 1)
            return false;
        if (--view_month_ < 1)
        {
            view_month_ = 12;
            --view_year_;
        }
    }
    else
    {
        if (view_year_ == max_year_ && view_month_ == max_month_)
            return false;
        if (++view_month_ > 12)
        {
            view_month_ = 1;
            ++view_year_;
        }
    }
    // No factory here: force a cell rebuild on the next paint_overlay() by
    // zeroing the first cell's month marker.
    layouts_[0].reset();
    cells_[0].month = 0;
    // Keep the keyboard cursor on the month now shown.
    set_cursor_(view_year_, view_month_, cursor_day_);
    if (host())
        host()->request_repaint();
    return true;
}

void DatePickerView::set_cursor_(int y, int m, int d)
{
    m = std::clamp(m, 1, 12);
    d = std::clamp(d, 1, days_in_month(y, m));
    const long key = long(y) * 10000 + m * 100 + d;
    const long max_key = long(max_year_) * 10000 + max_month_ * 100 + max_day_;
    if (key < 19700101L)
    {
        y = 1970; m = 1; d = 1;
    }
    else if (key > max_key)
    {
        y = max_year_; m = max_month_; d = max_day_;
    }
    cursor_year_  = y;
    cursor_month_ = m;
    cursor_day_   = d;
    if (y != view_year_ || m != view_month_)
    {
        view_year_  = y;
        view_month_ = m;
        layouts_[0].reset();
        month_layout_.reset();
        year_layout_.reset();
        cells_[0].month = 0; // force a cell rebuild on the next paint
    }
    if (host())
        host()->request_repaint();
}

void DatePickerView::move_cursor_days_(int days)
{
    // Noon avoids a DST transition nudging mktime() onto the wrong day.
    std::tm t{};
    t.tm_year = cursor_year_ - 1900;
    t.tm_mon  = cursor_month_ - 1;
    t.tm_mday = cursor_day_ + days;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    std::mktime(&t);
    set_cursor_(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

void DatePickerView::move_cursor_months_(int months)
{
    const int total = cursor_year_ * 12 + (cursor_month_ - 1) + months;
    const int y = total / 12;
    const int m = total % 12 + 1;
    set_cursor_(y, m, std::min(cursor_day_, days_in_month(y, m)));
}

int DatePickerView::cursor_cell_() const
{
    if (cursor_year_ != view_year_ || cursor_month_ != view_month_ ||
        view_year_ == 0)
        return -1;
    const int idx = first_weekday(view_year_, view_month_) + cursor_day_ - 1;
    return idx >= 0 && idx < kRows * kCols ? idx : -1;
}

bool DatePickerView::part_enabled_(FocusPart p) const
{
    switch (p)
    {
    case FocusPart::PrevBtn: return !(view_year_ == 1970 && view_month_ == 1);
    case FocusPart::NextBtn:
        return !(view_year_ == max_year_ && view_month_ == max_month_);
    case FocusPart::TodayBtn: return today_enabled();
    default: return true;
    }
}

void DatePickerView::pick_cell_(int cell)
{
    if (cell < 0 || cell >= kRows * kCols || !cells_[cell].enabled)
        return;
    const auto& ci = cells_[cell];
    if (on_date_picked)
        on_date_picked(ci.year, ci.month, ci.day);
}

void DatePickerView::pick_today_()
{
    if (!today_enabled())
        return;
    int ty, tm, td;
    today(ty, tm, td);
    if (on_date_picked)
        on_date_picked(ty, tm, td);
}

// ── Accessibility ─────────────────────────────────────────────────────────────
// Rows: 0 = previous month, 1 = next month, 2 .. 2+42 = day cells (spill
// days from adjacent months report Role::None), last = Today.

namespace
{
constexpr std::size_t kDateRowPrev  = 0;
constexpr std::size_t kDateRowNext  = 1;
constexpr std::size_t kDateRowCell0 = 2;
} // namespace

std::string DatePickerView::access_name() const
{
    std::tm month_tm{};
    month_tm.tm_year = view_year_ - 1900;
    month_tm.tm_mon  = view_month_ - 1;
    month_tm.tm_mday = 1;
    return tk::format_date(month_tm, tk::tr("%B %Y"));
}

std::size_t DatePickerView::access_row_count() const
{
    return kDateRowCell0 + static_cast<std::size_t>(kRows * kCols) + 1;
}

tk::Role DatePickerView::access_role_for_widget_row(std::size_t i) const
{
    if (i == kDateRowPrev || i == kDateRowNext)
        return tk::Role::Button;
    if (i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
        return tk::Role::Button; // Today
    const auto& ci = cells_[i - kDateRowCell0];
    return (ci.year != 0 && ci.in_month) ? tk::Role::GridCell : tk::Role::None;
}

std::string DatePickerView::access_name_for_widget_row(std::size_t i) const
{
    if (i == kDateRowPrev)
        return tk::tr("Previous month");
    if (i == kDateRowNext)
        return tk::tr("Next month");
    if (i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
        return tk::tr("Today");
    const auto& ci = cells_[i - kDateRowCell0];
    std::tm day_tm{};
    day_tm.tm_year = ci.year - 1900;
    day_tm.tm_mon  = ci.month - 1;
    day_tm.tm_mday = ci.day;
    return tk::format_date(day_tm, tk::tr("%B %-d, %Y"));
}

std::string DatePickerView::access_description_for_widget_row(std::size_t i) const
{
    if (i < kDateRowCell0 || i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
        return {};
    return cells_[i - kDateRowCell0].is_today ? tk::tr("Today") : std::string();
}

tk::AccessState DatePickerView::access_state_for_widget_row(std::size_t i) const
{
    tk::AccessState st;
    if (i == kDateRowPrev)
        st.disabled = view_year_ == 1970 && view_month_ == 1;
    else if (i == kDateRowNext)
        st.disabled = view_year_ == max_year_ && view_month_ == max_month_;
    else if (i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
        st.disabled = !today_enabled();
    else
    {
        const int cell = static_cast<int>(i - kDateRowCell0);
        st.disabled = !cells_[cell].enabled;
        st.selected = cursor_cell_() == cell;
    }
    return st;
}

bool DatePickerView::access_activate_widget_row(std::size_t i)
{
    if (i == kDateRowPrev)
        return step_month_(-1);
    if (i == kDateRowNext)
        return step_month_(1);
    if (i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
    {
        if (!today_enabled())
            return false;
        pick_today_();
        return true;
    }
    const int cell = static_cast<int>(i - kDateRowCell0);
    if (!cells_[cell].enabled)
        return false;
    pick_cell_(cell);
    return true;
}

tk::Rect DatePickerView::access_rect_for_widget_row(std::size_t i) const
{
    if (i == kDateRowPrev)
        return prev_btn_rect_;
    if (i == kDateRowNext)
        return next_btn_rect_;
    if (i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
        return today_btn_rect_;
    return cell_world_rect(static_cast<int>(i - kDateRowCell0));
}

std::pair<int, int> DatePickerView::access_grid_cell_for_widget_row(std::size_t i) const
{
    if (i < kDateRowCell0 || i >= kDateRowCell0 + static_cast<std::size_t>(kRows * kCols))
        return {-1, -1};
    const int cell = static_cast<int>(i - kDateRowCell0);
    return {cell / kCols, cell % kCols};
}

bool DatePickerView::on_pointer_move(tk::Point local)
{
    int cell = -1;
    Zone z   = hit_zone(local, &cell);

    const bool changed = (z != hovered_zone_) || (cell != hovered_cell_);
    hovered_zone_ = z;
    hovered_cell_ = cell;
    return changed;
}

void DatePickerView::on_pointer_leave()
{
    const bool changed = hovered_zone_ != Zone::None;
    hovered_zone_ = Zone::None;
    hovered_cell_ = -1;
    (void)changed;
}

bool DatePickerView::on_wheel(tk::Point local, float /*dx*/, float dy, bool /*is_touchpad*/)
{
    if (dy == 0.0f)
        return true;

    wheel_accum_ += dy;

    // Threshold for one step. Win32/Qt/GTK report 90 px per physical notch;
    // macOS reports ~30 px. Capping at 1 step per call and flushing the
    // accumulator on a hit means 90 px fires exactly once rather than 3 times.
    constexpr float kStep = 30.0f;
    int steps = 0;
    if (wheel_accum_ >= kStep)
    {
        steps = 1;
        wheel_accum_ = 0.0f;
    }
    else if (wheel_accum_ <= -kStep)
    {
        steps = -1;
        wheel_accum_ = 0.0f;
    }

    if (steps == 0)
        return true;

    // Positive steps = forward (down) = later date.
    // Negative steps = backward (up)  = earlier date.
    const tk::Point w{bounds_.x + local.x, bounds_.y + local.y};
    const bool over_year = (w.x >= year_label_rect_.x &&
                            w.x <  year_label_rect_.x + year_label_rect_.w &&
                            w.y >= header_rect_.y &&
                            w.y <  header_rect_.y + kHeaderH);

    if (over_year)
    {
        int y = view_year_ + steps;
        if (y < 1970)      y = 1970;
        if (y > max_year_) y = max_year_;
        if (y == view_year_) return true;
        view_year_ = y;
        if (view_year_ == max_year_ && view_month_ > max_month_)
            view_month_ = max_month_;
    }
    else
    {
        view_month_ += steps;
        while (view_month_ > 12) { view_month_ -= 12; ++view_year_; }
        while (view_month_ <  1) { view_month_ += 12; --view_year_; }
        if (view_year_ < 1970) { view_year_ = 1970; view_month_ = 1; }
        if (view_year_ > max_year_ ||
            (view_year_ == max_year_ && view_month_ > max_month_))
        {
            view_year_  = max_year_;
            view_month_ = max_month_;
        }
    }

    month_layout_.reset();
    year_layout_.reset();
    cells_[0].month = 0; // force cell rebuild on next paint
    set_cursor_(view_year_, view_month_, cursor_day_);
    return true;
}

void DatePickerView::on_popup_dismiss()
{
    pressed_zone_ = Zone::None;
    pressed_cell_ = -1;
    hovered_zone_ = Zone::None;
    hovered_cell_ = -1;
    wheel_accum_  = 0.0f;
    if (on_dismiss)
        on_dismiss();
}

bool DatePickerView::on_key_down(const tk::KeyEvent& e)
{
    if (e.key == tk::Key::Escape)
    {
        on_popup_dismiss();
        return true;
    }
    if (e.ctrl || e.alt || e.meta)
        return false;
    if (cursor_year_ == 0)
        set_cursor_(view_year_, view_month_, 1);

    const bool was_active = kbd_active_;
    kbd_active_ = true;

    if (e.key == tk::Key::Tab || e.key == tk::Key::Backtab)
    {
        // The first Tab after a mouse open only reveals the cursor where it
        // already is (the grid), like the first arrow press does.
        if (!was_active)
            return true;
        static constexpr FocusPart kOrder[] = {
            FocusPart::PrevBtn, FocusPart::Month,   FocusPart::Year,
            FocusPart::NextBtn, FocusPart::Grid,    FocusPart::TodayBtn};
        constexpr int n = static_cast<int>(std::size(kOrder));
        int idx = 0;
        for (int i = 0; i < n; ++i)
            if (kOrder[i] == focus_part_)
                idx = i;
        const int dir = e.key == tk::Key::Tab ? 1 : -1;
        for (int step = 0; step < n; ++step)
        {
            idx = (idx + dir + n) % n;
            if (part_enabled_(kOrder[idx]))
                break;
        }
        focus_part_ = kOrder[idx];
        return true;
    }

    if (matches(ShortcutId::DatePrevMonth, e))
    {
        move_cursor_months_(-1);
        return true;
    }
    if (matches(ShortcutId::DateNextMonth, e))
    {
        move_cursor_months_(1);
        return true;
    }
    if (matches(ShortcutId::DatePrevYear, e))
    {
        move_cursor_months_(-12);
        return true;
    }
    if (matches(ShortcutId::DateNextYear, e))
    {
        move_cursor_months_(12);
        return true;
    }

    if (matches(ShortcutId::DateToday, e))
    {
        int ty, tm, td;
        today(ty, tm, td);
        set_cursor_(ty, tm, td);
        focus_part_ = FocusPart::Grid;
        return true;
    }

    if (e.key == tk::Key::Enter || e.key == tk::Key::Space)
    {
        // The first press after a mouse open only reveals the cursor, so a
        // stray Enter can't jump to a date the user never saw highlighted.
        if (!was_active)
            return true;
        switch (focus_part_)
        {
        case FocusPart::Grid:
            if (on_date_picked)
                on_date_picked(cursor_year_, cursor_month_, cursor_day_);
            break;
        case FocusPart::PrevBtn: move_cursor_months_(-1); break;
        case FocusPart::NextBtn: move_cursor_months_(1); break;
        case FocusPart::TodayBtn: pick_today_(); break;
        case FocusPart::Month:
        case FocusPart::Year: focus_part_ = FocusPart::Grid; break;
        }
        return true;
    }

    int step = 0; // +1 = later
    switch (e.key)
    {
    case tk::Key::Left: step = -1; break;
    case tk::Key::Right: step = 1; break;
    case tk::Key::Up: step = focus_part_ == FocusPart::Grid ? -kCols : 1; break;
    case tk::Key::Down: step = focus_part_ == FocusPart::Grid ? kCols : -1; break;
    case tk::Key::Home:
    case tk::Key::End:
        if (focus_part_ == FocusPart::Grid)
        {
            const int col = (first_weekday(cursor_year_, cursor_month_) +
                             cursor_day_ - 1) % kCols;
            move_cursor_days_(e.key == tk::Key::Home ? -col : kCols - 1 - col);
        }
        return true;
    default: return false;
    }
    if (!was_active)
        return true; // first arrow just reveals the cursor
    switch (focus_part_)
    {
    case FocusPart::Grid: move_cursor_days_(step); break;
    case FocusPart::Month: move_cursor_months_(step > 0 ? 1 : -1); break;
    case FocusPart::Year: move_cursor_months_(step > 0 ? 12 : -12); break;
    default: break; // arrows on ‹ › Today are swallowed
    }
    return true;
}

// ── private helpers ───────────────────────────────────────────────────────────

void DatePickerView::compute_zones_()
{
    const float bx = bounds_.x, by = bounds_.y;

    header_rect_ = {bx, by, kWidth, kHeaderH};
    dow_rect_    = {bx, by + kHeaderH, kWidth, kDowH};
    grid_rect_   = {bx, by + kHeaderH + kDowH, kWidth,
                    float(kRows) * kDayH};
    footer_rect_ = {bx, by + kHeaderH + kDowH + float(kRows) * kDayH,
                    kWidth, kFooterH};

    // Navigation buttons: vertically centred in header, 6px from edges.
    const float btn_y = by + (kHeaderH - kNavBtnSz) * 0.5f;
    prev_btn_rect_ = {bx + kNavBtnPad, btn_y, kNavBtnSz, kNavBtnSz};
    next_btn_rect_ = {bx + kWidth - kNavBtnPad - kNavBtnSz, btn_y,
                      kNavBtnSz, kNavBtnSz};

    // Today button: centred in footer (70px wide, 26px tall).
    const float tb_w = 70.0f, tb_h = 26.0f;
    today_btn_rect_ = {bx + (kWidth - tb_w) * 0.5f,
                       footer_rect_.y + (kFooterH - tb_h) * 0.5f,
                       tb_w, tb_h};
}

void DatePickerView::rebuild_cells_(tk::CanvasFactory& factory)
{
    const int fw = first_weekday(view_year_, view_month_);
    const int dim = days_in_month(view_year_, view_month_);

    // Previous month info (for spill cells at the start).
    const int prev_month = (view_month_ == 1) ? 12 : view_month_ - 1;
    const int prev_year  = (view_month_ == 1) ? view_year_ - 1 : view_year_;
    const int prev_dim   = days_in_month(prev_year, prev_month);

    // Next month info (for spill cells at the end).
    const int next_month = (view_month_ == 12) ? 1 : view_month_ + 1;
    const int next_year  = (view_month_ == 12) ? view_year_ + 1 : view_year_;

    int ty = 0, tm = 0, td = 0;
    today(ty, tm, td);

    int next_day_counter = 1;
    for (int i = 0; i < kRows * kCols; ++i)
    {
        auto& cell = cells_[i];
        const int day_offset = i - fw; // 0-based day of current month

        if (day_offset < 0)
        {
            // Spill from previous month.
            cell.year     = prev_year;
            cell.month    = prev_month;
            cell.day      = prev_dim + day_offset + 1; // day_offset is negative
            cell.in_month = false;
            cell.enabled  = false;
            cell.is_today = false;
        }
        else if (day_offset < dim)
        {
            // Current month day.
            cell.year     = view_year_;
            cell.month    = view_month_;
            cell.day      = day_offset + 1;
            cell.in_month = true;

            // Enabled if not in the future (relative to max_date) and >= 1970.
            const bool past_min =
                (view_year_ > 1970) ||
                (view_year_ == 1970 && view_month_ > 1) ||
                (view_year_ == 1970 && view_month_ == 1 && cell.day >= 1);
            const bool before_max =
                (view_year_ < max_year_) ||
                (view_year_ == max_year_ && view_month_ < max_month_) ||
                (view_year_ == max_year_ && view_month_ == max_month_ &&
                 cell.day <= max_day_);
            cell.enabled = past_min && before_max;
            cell.is_today =
                (view_year_ == ty && view_month_ == tm && cell.day == td);
        }
        else
        {
            // Spill into next month.
            cell.year     = next_year;
            cell.month    = next_month;
            cell.day      = next_day_counter++;
            cell.in_month = false;
            cell.enabled  = false;
            cell.is_today = false;
        }

        // Build day-number label.
        cell_layouts_[i].reset();
        const std::string day_str = std::to_string(cell.day);
        tk::TextStyle ts{};
        ts.role = tk::FontRole::Body;
        cell_layouts_[i] = factory.build_text(day_str, ts);
    }

    // Rebuild month and year labels separately (needed for wheel zone detection).
    month_layout_.reset();
    year_layout_.reset();
    tk::TextStyle hdr{};
    hdr.role = tk::FontRole::UiSemibold;
    std::tm month_tm{};
    month_tm.tm_mon = view_month_ - 1;
    month_layout_ = factory.build_text(tk::format_date(month_tm, "%B"), hdr);
    year_layout_  = factory.build_text(std::to_string(view_year_), hdr);
}

void DatePickerView::ensure_layouts_(tk::CanvasFactory& factory)
{
    const bool factory_changed = (&factory != last_factory_);
    if (factory_changed)
    {
        last_factory_ = &factory;
        // Invalidate everything.
        for (auto& l : layouts_)      l.reset();
        for (auto& l : cell_layouts_) l.reset();
        nav_prev_layout_.reset();
        nav_next_layout_.reset();
        month_layout_.reset();
        year_layout_.reset();
        // Force cell rebuild.
        cells_[0].month = 0;
    }

    // DOW labels (slots 1-7): built once.
    for (int i = 0; i < kCols; ++i)
    {
        if (!layouts_[1 + i])
        {
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Small;
            layouts_[1 + i] = factory.build_text(tk::weekday_initials(i), ts);
        }
    }

    // "Today" label (slot 8): built once.
    if (!layouts_[8])
    {
        tk::TextStyle ts{};
        ts.role = tk::FontRole::UiSemibold;
        layouts_[8] = factory.build_text(tk::tr("Today"), ts);
    }

    // Navigation glyphs.
    if (!nav_prev_layout_)
    {
        tk::TextStyle ts{};
        ts.role = tk::FontRole::Title;
        nav_prev_layout_ = factory.build_text(kPrevGlyph, ts);
    }
    if (!nav_next_layout_)
    {
        tk::TextStyle ts{};
        ts.role = tk::FontRole::Title;
        nav_next_layout_ = factory.build_text(kNextGlyph, ts);
    }

    // Header label and cells are rebuilt elsewhere (rebuild_cells_).
    // Trigger cell rebuild if cells are stale.
    if (view_year_ != 0 && cells_[0].month == 0)
        rebuild_cells_(factory);
}

DatePickerView::Zone DatePickerView::hit_zone(tk::Point local,
                                               int* cell_idx_out) const
{
    *cell_idx_out = -1;

    // Convert local (relative to bounds_) to world.
    const tk::Point w{bounds_.x + local.x, bounds_.y + local.y};

    // Today button (check before footer area to reduce false hits).
    if (w.x >= today_btn_rect_.x && w.x < today_btn_rect_.x + today_btn_rect_.w &&
        w.y >= today_btn_rect_.y && w.y < today_btn_rect_.y + today_btn_rect_.h)
    {
        return Zone::TodayBtn;
    }

    // Prev button.
    if (w.x >= prev_btn_rect_.x && w.x < prev_btn_rect_.x + prev_btn_rect_.w &&
        w.y >= prev_btn_rect_.y && w.y < prev_btn_rect_.y + prev_btn_rect_.h)
    {
        return Zone::PrevBtn;
    }

    // Next button.
    if (w.x >= next_btn_rect_.x && w.x < next_btn_rect_.x + next_btn_rect_.w &&
        w.y >= next_btn_rect_.y && w.y < next_btn_rect_.y + next_btn_rect_.h)
    {
        return Zone::NextBtn;
    }

    // Day grid.
    if (w.x >= grid_rect_.x && w.x < grid_rect_.x + grid_rect_.w &&
        w.y >= grid_rect_.y && w.y < grid_rect_.y + grid_rect_.h)
    {
        const float cell_w = kWidth / float(kCols);
        const int col = static_cast<int>((w.x - grid_rect_.x) / cell_w);
        const int row = static_cast<int>((w.y - grid_rect_.y) / kDayH);
        if (col >= 0 && col < kCols && row >= 0 && row < kRows)
        {
            *cell_idx_out = row * kCols + col;
            return Zone::DayCell;
        }
    }

    return Zone::None;
}

tk::Rect DatePickerView::cell_world_rect(int i) const
{
    const float cell_w = kWidth / float(kCols);
    const int col = i % kCols;
    const int row = i / kCols;
    return {grid_rect_.x + float(col) * cell_w,
            grid_rect_.y + float(row) * kDayH,
            cell_w, kDayH};
}

tk::Rect DatePickerView::circle_rect_in(tk::Rect cell) const
{
    // Centre a kCellCircleD × kCellCircleD square inside the cell.
    return {cell.x + (cell.w - kCellCircleD) * 0.5f,
            cell.y + (kDayH - kCellCircleD) * 0.5f,
            kCellCircleD, kCellCircleD};
}

bool DatePickerView::today_enabled() const
{
    int ty, tm, td;
    today(ty, tm, td);
    return (ty < max_year_) ||
           (ty == max_year_ && tm < max_month_) ||
           (ty == max_year_ && tm == max_month_ && td <= max_day_);
}

std::uint64_t local_midnight_ms(int year, int month, int day)
{
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_isdst = -1; // let the zone rules decide whether DST applies
    const std::time_t secs = std::mktime(&t);
    if (secs < 0)
        return 0;
    return static_cast<std::uint64_t>(secs) * 1000ULL;
}

} // namespace tesseract::views
