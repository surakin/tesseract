#include "DatePickerView.h"

#include "tk/host.h"
#include "tk/theme.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <sstream>

namespace tesseract::views
{

namespace
{

static const char* const kMonthNames[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
static const char* const kDowLabels[7] = {"Su", "Mo", "Tu", "We",
                                          "Th", "Fr", "Sa"};

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

    // Reset interaction state.
    hovered_zone_ = Zone::None;
    hovered_cell_ = -1;
    pressed_zone_ = Zone::None;
    pressed_cell_ = -1;
    wheel_accum_  = 0.0f;

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

    c.push_clip_rect(bounds_);

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
                c.fill_rounded_rect(cr, kCellCircleD * 0.5f, pal.subtle_hover);
            }

            // Day number text
            if (cell_layouts_[i])
            {
                tk::Color text_col;
                if (is_selected_press)
                    text_col = pal.text_on_accent;
                else if (!cell.enabled || !cell.in_month)
                    text_col = pal.text_muted;
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

    c.pop_clip();
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
    {
        if (!(view_year_ == 1970 && view_month_ == 1))
        {
            // We don't have a factory here; schedule a re-navigate flag and
            // let paint_overlay() pick it up.  To keep it simple, force a
            // cell rebuild by zeroing the first cell's month marker.
            if (--view_month_ < 1)
            {
                view_month_ = 12;
                --view_year_;
            }
            layouts_[0].reset();
            cells_[0].month = 0; // force rebuild on next paint
        }
    }
    else if (z == Zone::NextBtn)
    {
        if (!(view_year_ == max_year_ && view_month_ == max_month_))
        {
            if (++view_month_ > 12)
            {
                view_month_ = 1;
                ++view_year_;
            }
            layouts_[0].reset();
            cells_[0].month = 0;
        }
    }
    else if (z == Zone::DayCell)
    {
        if (cell >= 0 && cell < kRows * kCols && cells_[cell].enabled)
        {
            const auto& ci = cells_[cell];
            if (on_date_picked)
                on_date_picked(ci.year, ci.month, ci.day);
        }
    }
    else if (z == Zone::TodayBtn && today_enabled())
    {
        int ty, tm, td;
        today(ty, tm, td);
        if (on_date_picked)
            on_date_picked(ty, tm, td);
    }
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

bool DatePickerView::on_wheel(tk::Point local, float /*dx*/, float dy)
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
    month_layout_ = factory.build_text(kMonthNames[view_month_ - 1], hdr);
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
            layouts_[1 + i] = factory.build_text(kDowLabels[i], ts);
        }
    }

    // "Today" label (slot 8): built once.
    if (!layouts_[8])
    {
        tk::TextStyle ts{};
        ts.role = tk::FontRole::UiSemibold;
        layouts_[8] = factory.build_text("Today", ts);
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

} // namespace tesseract::views
