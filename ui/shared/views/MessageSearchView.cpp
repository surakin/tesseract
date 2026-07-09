#include "MessageSearchView.h"

#include "tk/i18n.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace tesseract::views
{

namespace
{
constexpr float kMessageSearchPadX = 16.0f;
constexpr float kMessageSearchFieldH = 32.0f;

// Compact relative-time label for a result row ("now", "5m", "3h", "2d",
// "10w", else a y-count). Best-effort; uses wall-clock now().
std::string relative_time(std::uint64_t ts_ms)
{
    if (ts_ms == 0)
        return std::string();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (static_cast<std::uint64_t>(now) <= ts_ms)
        return "now";
    const std::uint64_t secs = (static_cast<std::uint64_t>(now) - ts_ms) / 1000;
    if (secs < 60)
        return "now";
    const std::uint64_t mins = secs / 60;
    if (mins < 60)
        return std::to_string(mins) + "m";
    const std::uint64_t hours = mins / 60;
    if (hours < 24)
        return std::to_string(hours) + "h";
    const std::uint64_t days = hours / 24;
    if (days < 7)
        return std::to_string(days) + "d";
    // Show weeks up to a full year, then years. Guard on `days` (not `weeks`)
    // so a ~52-week-old hit renders "52w" rather than rounding to "0y".
    if (days < 365)
        return std::to_string(days / 7) + "w";
    return std::to_string(days / 365) + "y";
}

// Collapse newlines/runs of whitespace into single spaces so a multi-line
// message renders as a one-line snippet.
std::string one_line(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s)
    {
        const bool is_space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (is_space)
        {
            if (!prev_space && !out.empty())
                out.push_back(' ');
            prev_space = true;
        }
        else
        {
            out.push_back(c);
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────

class MessageSearchView::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(MessageSearchView& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.result_count_();
    }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override
    {
        if (index >= owner_.results_.size())
            return;
        const auto& hit = owner_.results_[index];
        const auto& pal = ctx.theme.palette;

        if (selected)
            ctx.canvas.fill_rect(bounds, pal.sidebar_selected);
        else if (hovered)
            ctx.canvas.fill_rect(bounds, pal.sidebar_hover);

        const float text_x = bounds.x + kMessageSearchPadX;
        const float full_w = std::max(0.0f, bounds.w - 2 * kMessageSearchPadX);

        // ── Top line: room name (primary, left) + relative time (muted, right).
        const std::string time_str = relative_time(hit.timestamp_ms);
        float time_w = 0.0f;
        std::unique_ptr<tk::TextLayout> time_lo;
        if (!time_str.empty())
        {
            tk::TextStyle ts{};
            ts.role = tk::FontRole::Small;
            time_lo = ctx.factory.build_text(time_str, ts);
            if (time_lo)
                time_w = time_lo->measure().w + 8.0f;
        }

        tk::TextStyle ns{};
        ns.role = tk::FontRole::SidebarName;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = std::max(0.0f, full_w - time_w);
        const std::string room =
            hit.room_name.empty() ? tk::tr("Unknown room") : hit.room_name;
        auto room_lo = ctx.factory.build_text(room, ns);

        const float pad_y = 8.0f;
        const float line_gap = 3.0f;
        const float top_y = bounds.y + pad_y;
        if (room_lo)
        {
            ctx.canvas.draw_text(*room_lo, {text_x, top_y}, pal.text_primary);
        }
        if (time_lo)
        {
            const tk::Size tsz = time_lo->measure();
            ctx.canvas.draw_text(*time_lo,
                                 {bounds.x + bounds.w - kMessageSearchPadX - tsz.w, top_y},
                                 pal.text_muted);
        }

        // ── Bottom line: "sender: snippet" (muted), ellipsised to full width.
        const float room_h = room_lo ? room_lo->measure().h : 14.0f;
        const float bot_y = top_y + room_h + line_gap;

        std::string snippet = one_line(hit.body);
        if (!hit.sender_name.empty())
            snippet = hit.sender_name + ": " + snippet;
        tk::TextStyle ss{};
        ss.role = tk::FontRole::SidebarPreview;
        ss.trim = tk::TextTrim::Ellipsis;
        ss.max_width = full_w;
        auto snip_lo = ctx.factory.build_text(snippet, ss);
        if (snip_lo)
        {
            ctx.canvas.draw_text(*snip_lo, {text_x, bot_y}, pal.text_muted);
        }
    }

private:
    MessageSearchView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

MessageSearchView::MessageSearchView()
    : adapter_(std::make_unique<Adapter>(*this))
{
    set_visible(false);

    auto list = std::make_unique<tk::ListView>();
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= result_count_())
            return;
        list_->set_selected_index(idx);
        activate_selected();
    };
    list_ = add_child(std::move(list));
}

MessageSearchView::~MessageSearchView() = default;

void MessageSearchView::open()
{
    query_.clear();
    results_.clear();
    have_searched_ = false;
    press_outside_ = false;
    max_card_h_ = 0.0f;
    is_open_ = true;
    set_visible(true);
    if (list_)
    {
        list_->invalidate_data();
        list_->set_selected_index(-1);
        list_->scroll_to_top();
    }
}

void MessageSearchView::close()
{
    if (!is_open_)
        return;
    is_open_ = false;
    set_visible(false);
    if (on_close)
        on_close();
}

void MessageSearchView::set_query(const std::string& q)
{
    if (q == query_)
        return;
    query_ = q;
    // A new query invalidates the prior results until the next response lands.
    results_.clear();
    have_searched_ = false;
    if (list_)
    {
        list_->invalidate_data();
        list_->set_selected_index(-1);
        list_->scroll_to_top();
    }
    if (on_query_changed)
        on_query_changed(query_);
}

void MessageSearchView::set_results(std::vector<tesseract::SearchHit> results,
                                    const std::string& for_query)
{
    // Drop stale responses: only apply results computed for the current query.
    if (for_query != query_)
        return;
    results_ = std::move(results);
    have_searched_ = true;
    if (list_)
    {
        list_->invalidate_data();
        list_->set_selected_index(results_.empty() ? -1 : 0);
        list_->scroll_to_top();
    }
}

void MessageSearchView::move_selection(int delta)
{
    if (!list_ || result_count_() == 0)
        return;
    const int n = static_cast<int>(result_count_());
    int cur = list_->selected_index();
    if (cur < 0)
        cur = delta > 0 ? -1 : 0;
    int next = cur + delta;
    if (next < 0)
        next = 0;
    if (next >= n)
        next = n - 1;
    list_->set_selected_index(next);
    list_->scroll_to_index(next);
}

void MessageSearchView::activate_selected()
{
    const int sel = list_ ? list_->selected_index() : -1;
    if (sel < 0 || static_cast<std::size_t>(sel) >= results_.size())
        return;
    const auto hit = results_[static_cast<std::size_t>(sel)];
    if (on_result_activated)
        on_result_activated(hit.room_id, hit.event_id);
    close();
}

// ── Layout + paint ────────────────────────────────────────────────────────

tk::Size MessageSearchView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void MessageSearchView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!list_)
        return;

    const float margin = 40.0f;
    const float cw = std::min(kCardW, std::max(0.0f, bounds.w - 2 * margin));
    const float chrome_h = kHeaderH;

    const float list_content =
        result_count_() == 0 ? kRowH
                             : static_cast<float>(result_count_()) * kRowH;
    const float max_h = std::min(kCardMaxH, std::max(0.0f, bounds.h - 2 * margin));
    float ch =
        chrome_h + std::min(list_content, std::max(0.0f, max_h - chrome_h));
    ch = std::max(ch, chrome_h + kRowH);
    ch = std::min(ch, max_h);

    max_card_h_ = std::max(max_card_h_, ch);
    ch = max_card_h_;

    const float cx = bounds.x + (bounds.w - cw) * 0.5f;
    float cy = bounds.y + (bounds.h - ch) * 0.38f; // bias above centre
    cy = std::max(cy, bounds.y + margin);

    card_rect_ = {cx, cy, cw, ch};
    search_field_rect_ = {cx + kMessageSearchPadX, cy + (kHeaderH - kMessageSearchFieldH) * 0.5f,
                          std::max(0.0f, cw - 2 * kMessageSearchPadX), kMessageSearchFieldH};

    const tk::Rect list_bounds{cx, cy + chrome_h, cw,
                               std::max(0.0f, ch - chrome_h)};
    list_->arrange(ctx, list_bounds);
}

void MessageSearchView::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
        return;
    const auto& pal = ctx.theme.palette;

    ctx.canvas.fill_rect(bounds_, tk::Color::rgba(0, 0, 0, 160));

    ctx.canvas.fill_rounded_rect(card_rect_, 10.0f, pal.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card_rect_, 10.0f, pal.popup_border, 1.0f);

    tk::Rect sep{card_rect_.x, card_rect_.y + kHeaderH - 1.0f, card_rect_.w,
                 1.0f};
    ctx.canvas.fill_rect(sep, pal.separator);
    if (!search_field_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(search_field_rect_, 6.0f,
                                     pal.compose_card_bg);
        ctx.canvas.stroke_rounded_rect(search_field_rect_, 6.0f, pal.border,
                                       1.0f);
    }

    if (result_count_() == 0)
    {
        tk::TextStyle es{};
        es.role = tk::FontRole::Body;
        const std::string msg =
            query_.empty()
                ? tk::tr("Type to search your messages")
                : (have_searched_ ? tk::tr("No matches")
                                  : tk::tr("Searching…"));
        auto empty_lo = ctx.factory.build_text(msg, es);
        if (empty_lo)
        {
            const tk::Size sz = empty_lo->measure();
            ctx.canvas.draw_text(
                *empty_lo,
                {card_rect_.x + (card_rect_.w - sz.w) * 0.5f,
                 card_rect_.y + kHeaderH +
                     (card_rect_.h - kHeaderH - sz.h) * 0.5f},
                pal.text_muted);
        }
        return;
    }

    if (list_ && list_->visible())
    {
        ctx.canvas.push_clip_rounded_rect(card_rect_, 10.0f);
        list_->paint(ctx);
        ctx.canvas.pop_clip();
    }
}

bool MessageSearchView::on_pointer_down(tk::Point local)
{
    if (!is_open_)
        return false;
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    auto contains = [](const tk::Rect& r, tk::Point p)
    {
        return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
    };
    press_outside_ = !contains(card_rect_, world);
    // Always consume so the press never falls through to widgets behind the
    // modal backdrop.
    return true;
}

void MessageSearchView::on_pointer_up(tk::Point /*local*/, bool inside_self)
{
    if (press_outside_)
    {
        press_outside_ = false;
        if (inside_self)
            close();
    }
}

bool MessageSearchView::on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/)
{
    // Consume wheel over the modal backdrop; the inner ListView handles its own
    // scrolling via the normal child-dispatch path before reaching here.
    return is_open_;
}

} // namespace tesseract::views
