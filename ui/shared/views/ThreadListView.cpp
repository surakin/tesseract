#include "ThreadListView.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>

namespace tesseract::views
{

namespace
{

// UTF-8-safe truncation: clip to `max_bytes` bytes, then back off until the
// next byte is a UTF-8 start byte so we never split a code-point. Appends
// "..." when truncation actually trimmed text. Newlines are folded to single
// spaces because the previews are single-line.
std::string truncate_utf8(std::string s, std::size_t max_bytes)
{
    for (char& c : s)
    {
        if (c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }
    if (s.size() <= max_bytes)
    {
        return s;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    s.resize(cut);
    s += "...";
    return s;
}

// Returns a compact human-readable date for the given millisecond timestamp:
//   0         → ""
//   today     → "Today"
//   yesterday → "Yesterday"
//   ≤7 days   → "Mon" / "Tue" / … (3-letter weekday)
//   same year → "May 30"
//   older     → "5/30/24"
std::string format_thread_date(std::uint64_t timestamp_ms)
{
    if (timestamp_ms == 0)
        return {};

    std::time_t now_t = std::time(nullptr);
    std::time_t t     = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm now_tm{}, item_tm{};
#if defined(_WIN32)
    localtime_s(&now_tm, &now_t);
    localtime_s(&item_tm, &t);
#else
    localtime_r(&now_t, &now_tm);
    localtime_r(&t, &item_tm);
#endif

    if (item_tm.tm_year == now_tm.tm_year &&
        item_tm.tm_mon  == now_tm.tm_mon  &&
        item_tm.tm_mday == now_tm.tm_mday)
        return tk::tr("Today");

    std::time_t yest_t = now_t - 86400;
    std::tm yest_tm{};
#if defined(_WIN32)
    localtime_s(&yest_tm, &yest_t);
#else
    localtime_r(&yest_t, &yest_tm);
#endif
    if (item_tm.tm_year == yest_tm.tm_year &&
        item_tm.tm_mon  == yest_tm.tm_mon  &&
        item_tm.tm_mday == yest_tm.tm_mday)
        return tk::tr("Yesterday");

    if (now_t > t && static_cast<std::uint64_t>(now_t - t) < 7u * 86400u)
    {
        constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed",
                                         "Thu", "Fri", "Sat"};
        return tk::tr(kDays[item_tm.tm_wday]);
    }

    constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
    char buf[24];
    if (item_tm.tm_year == now_tm.tm_year)
        std::snprintf(buf, sizeof(buf), "%s %d",
                      tk::tr(kMonths[item_tm.tm_mon]).c_str(), item_tm.tm_mday);
    else
        std::snprintf(buf, sizeof(buf), "%d/%d/%02d",
                      item_tm.tm_mon + 1, item_tm.tm_mday,
                      (item_tm.tm_year + 1900) % 100);
    return std::string(buf);
}

} // namespace

ThreadListView::ThreadListView()
{
    set_adapter(this);

    // Index 0 is the non-selectable header spacer; thread rows start at 1.
    on_row_clicked = [this](int idx)
    {
        if (idx >= 1 && idx <= static_cast<int>(threads_.size()) &&
            on_thread_clicked)
        {
            on_thread_clicked(
                threads_[static_cast<std::size_t>(idx - 1)].root_event_id);
        }
    };

    // Added as a child so dispatch_pointer_down reaches it before the
    // ListView row hit-test, and paint() renders it on top.
    auto close = std::make_unique<tk::Button>(
        "\xC3\x97", // U+00D7 ×
        std::function<void()>{}, tk::Button::Variant::Icon);
    close_btn_ = add_child(std::move(close));
    close_btn_->set_on_click([this] {
        if (on_close) on_close();
    });
}

void ThreadListView::set_threads(std::vector<tesseract::ThreadInfo> threads)
{
    threads_ = std::move(threads);
    // Ascending by most-recent activity: oldest first, newest LAST (at the
    // bottom) to match the message timeline. Older threads load via upward
    // pagination, so they sort in above the current top.
    std::sort(threads_.begin(), threads_.end(),
              [](const tesseract::ThreadInfo& a, const tesseract::ThreadInfo& b) {
                  std::uint64_t ta =
                      a.latest_timestamp > 0 ? a.latest_timestamp : a.root_timestamp;
                  std::uint64_t tb =
                      b.latest_timestamp > 0 ? b.latest_timestamp : b.root_timestamp;
                  return ta < tb;
              });
    // Preserve the user's scroll position across this full-list rebuild so
    // prepended (older) threads don't shove the viewport — mirrors
    // MessageListView::set_messages. No-op while stuck to the bottom (reading
    // the newest), so newest/live additions keep the view pinned to the bottom.
    preserve_top_through([this] { invalidate_data(); });
    reset_near_top_latch();
}

// ── tk::ListView / Widget overrides ──────────────────────────────────────────

tk::Size ThreadListView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void ThreadListView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    // Give ListView the full outer bounds so bounds_ covers both the header
    // strip and the scrollable rows — this keeps hit-testing correct for
    // clicks in the header area and preserves the coordinate system the
    // tests rely on.
    tk::ListView::arrange(lc, bounds);

    // Position the close button in the header-spacer row (index 0).
    if (close_btn_)
    {
        const float cx = bounds.x + bounds.w - kCloseSz - kCloseInset;
        const float cy = bounds.y + (kHeaderH - kCloseSz) * 0.5f;
        close_btn_->arrange(lc, {cx, cy, kCloseSz, kCloseSz});
    }
}

void ThreadListView::paint(tk::PaintCtx& ctx)
{
    // Rows + scrollbar via ListView (fills full bounds with sidebar_bg,
    // then paints each row via paint_row).
    tk::ListView::paint(ctx);

    // Opaque fill for the header strip — ListView clips all row painting to
    // bounds_, so when scrolled, thread rows draw into the header area.
    // Filling here eclipses that content before the separator and button paint.
    ctx.canvas.fill_rect(
        {bounds_.x, bounds_.y, bounds_.w, kHeaderH},
        ctx.theme.palette.bg);

    // Separator line below the header-row area.
    ctx.canvas.fill_rect(
        {bounds_.x, bounds_.y + kHeaderH - 1.f, bounds_.w, 1.f},
        ctx.theme.palette.separator);

    // Close button and glyph — painted last so they sit above the rows.
    if (close_btn_ && close_btn_->visible())
    {
        close_btn_->paint(ctx);
        const tk::Rect cb = close_btn_->bounds();
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        auto glyph = ctx.factory.build_text("\xC3\x97", st); // U+00D7 ×
        if (glyph)
        {
            const tk::Size sz = glyph->measure();
            ctx.canvas.draw_text(*glyph,
                                 {cb.x + (cb.w - sz.w) * 0.5f,
                                  cb.y + (cb.h - sz.h) * 0.5f},
                                 ctx.theme.palette.text_secondary);
        }
    }
}

// ── tk::ListAdapter overrides ─────────────────────────────────────────────────

std::size_t ThreadListView::count() const
{
    // Index 0 is the non-selectable header spacer.
    return threads_.size() + 1;
}

float ThreadListView::measure_row_height(std::size_t index,
                                          tk::LayoutCtx& /*ctx*/,
                                          float /*available_width*/)
{
    return index == 0 ? kHeaderH : kRowH;
}

bool ThreadListView::is_selectable(std::size_t index) const
{
    return index != 0;
}

void ThreadListView::paint_row(std::size_t index, tk::PaintCtx& ctx,
                                tk::Rect r, bool /*selected*/, bool hovered)
{
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    if (index == 0)
    {
        // Header spacer row — fill with panel background so it differs
        // from the sidebar_bg ListView painted over the whole area.
        cv.fill_rect(r, pal.bg);
        return;
    }

    const std::size_t ti = index - 1;
    if (ti >= threads_.size())
        return;
    const auto& t = threads_[ti];

    // Row background.
    cv.fill_rect(r, pal.bg);

    // Hover highlight.
    if (hovered)
        cv.fill_rect(r, pal.subtle_hover);

    // Per-row separator at the bottom.
    cv.fill_rect({r.x, r.bottom() - 1.0f, r.w, 1.0f}, pal.separator);

    // Right column: date at top, reply count at bottom.
    tk::TextStyle cs{};
    cs.role = tk::FontRole::Small;
    cs.wrap = false;

    // Date label — most recent activity timestamp.
    std::uint64_t eff_ts =
        t.latest_timestamp > 0 ? t.latest_timestamp : t.root_timestamp;
    std::string date_str = format_thread_date(eff_ts);
    auto date_layout = ctx.factory.build_text(date_str, cs);
    float date_w = 0.0f;
    if (date_layout)
    {
        const tk::Size sz = date_layout->measure();
        date_w = sz.w;
        cv.draw_text(*date_layout,
                     {r.x + r.w - kPadX - sz.w, r.y + kPadY},
                     pal.text_muted);
    }

    // Reply count — bottom right.
    char count_buf[48];
    std::snprintf(count_buf, sizeof(count_buf),
                  t.num_replies == 1 ? "%llu reply" : "%llu replies",
                  static_cast<unsigned long long>(t.num_replies));
    auto count_layout = ctx.factory.build_text(count_buf, cs);
    float count_w = 0.0f;
    if (count_layout)
    {
        const tk::Size sz = count_layout->measure();
        count_w = sz.w;
        cv.draw_text(*count_layout,
                     {r.x + r.w - kPadX - sz.w, r.y + kRowH - kPadY - sz.h},
                     pal.text_secondary);
    }

    const float right_reserve = std::max(date_w, count_w);
    const float text_left  = r.x + kPadX;
    const float text_right = r.x + r.w - kPadX
                             - right_reserve
                             - (right_reserve > 0.0f ? 8.0f : 0.0f);
    const float text_max_w = std::max(0.0f, text_right - text_left);

    // Top line: "<root_sender_name>: <root_body snippet>".
    {
        std::string preview;
        if (!t.root_sender_name.empty())
            preview = t.root_sender_name + ": ";
        preview += truncate_utf8(t.root_body, 80);

        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        auto layout  = ctx.factory.build_text(preview, st);
        if (layout)
            cv.draw_text(*layout, {text_left, r.y + kPadY}, pal.text_primary);
    }

    // Bottom line: "↳ <latest_sender_name>: <latest_body snippet>" —
    // only when a reply exists.
    if (!t.latest_sender_name.empty())
    {
        std::string preview = "\xE2\x86\xB3 "; // U+21B3 ↳
        preview += t.latest_sender_name;
        preview += ": ";
        preview += truncate_utf8(t.latest_body, 80);

        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        auto layout  = ctx.factory.build_text(preview, st);
        if (layout)
        {
            const tk::Size sz = layout->measure();
            cv.draw_text(*layout,
                         {text_left, r.y + kRowH - kPadY - sz.h},
                         pal.text_secondary);
        }
    }
}

} // namespace tesseract::views
