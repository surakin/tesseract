#include "PollCardDisplay.h"

#include "MessageListView.h" // MessageRowData / PollAnswerRow (full defs)
#include "poll_logic.h"

#include "tk/i18n.h"
#include "tk/theme.h"
#include "tk/widget.h" // tk::PaintCtx

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kPollCardMaxW = 420.0f;
constexpr float kPad = 12.0f;
constexpr float kGap = 8.0f;
constexpr float kControlW = 22.0f;
constexpr float kMarker = 16.0f;
constexpr float kBarH = 6.0f;
constexpr float kBarGap = 4.0f;
constexpr float kRadius = 8.0f;
constexpr float kBtnPadX = 10.0f;
constexpr float kBtnPadY = 4.0f;

tk::TextStyle style(tk::FontRole role, float max_w)
{
    tk::TextStyle st{};
    st.role = role;
    st.wrap = true;
    st.max_width = std::max(1.0f, max_w);
    return st;
}

bool contains(const tk::Rect& r, tk::Point p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}
} // namespace

float PollCardDisplay::width(float col_w) const
{
    return std::min(col_w, kPollCardMaxW);
}

PollCardDisplay::Layout PollCardDisplay::layout_(const MessageRowData& m,
                                                 tk::CanvasFactory& factory,
                                                 float col_w, bool show_end) const
{
    Layout L;
    L.card_w = width(col_w);
    const float inner_w = std::max(0.0f, L.card_w - 2.0f * kPad);
    float cy = kPad;

    // Question (semibold at body size).
    L.question = factory.build_text(
        m.body.empty() ? tk::tr("Poll") : m.body,
        style(tk::FontRole::SidebarName, inner_w));
    if (L.question)
        cy += L.question->measure().h;
    cy += kGap;

    std::uint32_t top = 0;
    for (const auto& a : m.poll_answers)
        top = std::max(top, a.votes);

    for (const auto& a : m.poll_answers)
    {
        AnswerLayout al;
        al.id = a.id;
        al.votes = a.votes;
        al.mine = a.mine;
        al.leader = top > 0 && a.votes == top;
        float text_w = inner_w - kControlW;
        if (m.poll_results_visible)
        {
            const int pct = poll_percent(a.votes, m.poll_total_votes);
            al.stat = factory.build_text(
                tk::trf(tk::tr("{0}% ({1})"),
                        {std::to_string(pct), std::to_string(a.votes)}),
                style(tk::FontRole::Small, inner_w));
            if (al.stat)
                al.stat_w = al.stat->measure().w;
            text_w -= al.stat_w + kGap;
            al.fill_frac = static_cast<float>(pct) / 100.0f;
        }
        al.text = factory.build_text(a.text, style(tk::FontRole::Body, text_w));
        if (al.text)
        {
            const tk::Size sz = al.text->measure();
            al.text_h = sz.h;
            al.line_h = sz.h / static_cast<float>(std::max(1, al.text->line_count()));
        }
        al.text_h = std::max(al.text_h, kMarker);
        al.y = cy;
        al.h = al.text_h + (m.poll_results_visible ? kBarGap + kBarH : 0.0f);
        cy += al.h + kGap;
        L.answers.push_back(std::move(al));
    }

    const std::string hint = poll_hint_text(m);
    if (!hint.empty())
    {
        L.hint = factory.build_text(hint, style(tk::FontRole::Small, inner_w));
        L.hint_y = cy;
        if (L.hint)
            cy += L.hint->measure().h + 2.0f;
    }

    float btn_w = 0.0f;
    float btn_h = 0.0f;
    // The button's space is reserved for every open poll, whether or not it is
    // shown: whether the viewer may end the poll (power levels, callbacks)
    // can change without the row's cached height being invalidated, and the
    // card must not change height when it does. `show_end` only gates paint.
    (void)show_end;
    if (!m.poll_ended)
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::UiSemibold;
        L.end_label = factory.build_text(tk::tr("End poll"), st);
        if (L.end_label)
        {
            btn_w = L.end_label->measure().w + 2.0f * kBtnPadX;
            btn_h = L.end_label->measure().h + 2.0f * kBtnPadY;
        }
    }
    L.status = factory.build_text(
        poll_status_text(m),
        style(tk::FontRole::Small, inner_w - (btn_w > 0 ? btn_w + kGap : 0.0f)));
    L.status_y = cy;
    const float status_h = L.status ? L.status->measure().h : 0.0f;
    const float line_h = std::max(status_h, btn_h);
    if (btn_w > 0.0f)
        L.end_rect = {kPad + inner_w - btn_w, cy + (line_h - btn_h) * 0.5f, btn_w, btn_h};
    cy += line_h + kPad;
    L.h = cy;
    return L;
}

float PollCardDisplay::height(const MessageRowData& m, tk::CanvasFactory& factory,
                              float col_w, bool show_end) const
{
    return layout_(m, factory, col_w, show_end).h;
}

float PollCardDisplay::paint(const MessageRowData& m, tk::PaintCtx& ctx, float x,
                             float y, float col_w, bool interactive, bool show_end)
{
    const Layout L = layout_(m, ctx.factory, col_w, show_end);
    const auto& pal = ctx.theme.palette;
    const tk::Rect card{x, y, L.card_w, L.h};
    const float inner_w = std::max(0.0f, L.card_w - 2.0f * kPad);
    const bool radio = m.poll_max_selections <= 1;

    ctx.canvas.fill_rounded_rect(card, kRadius, pal.chrome_bg);
    ctx.canvas.stroke_rounded_rect(card, kRadius, pal.border, 1.0f);

    if (L.question)
        ctx.canvas.draw_text(*L.question, {x + kPad, y + kPad}, pal.text_primary);

    for (const auto& a : L.answers)
    {
        const float ry = y + a.y;
        const tk::Rect marker{x + kPad + 1.0f, ry + (a.line_h - kMarker) * 0.5f + 1.0f,
                              kMarker - 2.0f, kMarker - 2.0f};
        if (radio)
        {
            ctx.canvas.stroke_rounded_rect(marker, marker.w * 0.5f, pal.accent, 1.5f);
            if (a.mine)
                ctx.canvas.fill_rounded_rect(
                    {marker.x + 3.5f, marker.y + 3.5f, marker.w - 7.0f, marker.h - 7.0f},
                    (marker.w - 7.0f) * 0.5f, pal.accent);
        }
        else if (a.mine)
        {
            ctx.canvas.fill_rounded_rect(marker, 3.0f, pal.accent);
            ctx.canvas.draw_line({marker.x + 3.0f, marker.y + marker.h * 0.55f},
                                 {marker.x + marker.w * 0.42f, marker.y + marker.h - 3.5f},
                                 pal.text_on_accent, 1.8f);
            ctx.canvas.draw_line({marker.x + marker.w * 0.42f, marker.y + marker.h - 3.5f},
                                 {marker.x + marker.w - 3.0f, marker.y + 3.5f},
                                 pal.text_on_accent, 1.8f);
        }
        else
        {
            ctx.canvas.stroke_rounded_rect(marker, 3.0f, pal.accent, 1.5f);
        }

        if (a.text)
            ctx.canvas.draw_text(*a.text, {x + kPad + kControlW, ry}, pal.text_primary);
        if (a.stat)
            ctx.canvas.draw_text(*a.stat, {x + kPad + inner_w - a.stat_w, ry},
                                 pal.text_secondary);

        if (m.poll_results_visible)
        {
            const tk::Rect track{x + kPad + kControlW, ry + a.text_h + kBarGap,
                                 std::max(0.0f, inner_w - kControlW), kBarH};
            ctx.canvas.fill_rounded_rect(track, kBarH * 0.5f, pal.border);
            const float fw = std::clamp(a.fill_frac, 0.0f, 1.0f) * track.w;
            if (fw > 0.0f)
            {
                const tk::Rect fill{track.x, track.y, fw, kBarH};
                if (m.poll_ended && !a.leader)
                    ctx.canvas.stroke_rounded_rect(fill, kBarH * 0.5f, pal.accent, 1.0f);
                else
                    ctx.canvas.fill_rounded_rect(fill, kBarH * 0.5f, pal.accent);
            }
        }

        if (interactive)
            geom_.push_back({Hit::Type::Answer, m.event_id, a.id,
                             {x + kPad, ry, inner_w, a.h}});
    }

    if (L.hint)
        ctx.canvas.draw_text(*L.hint, {x + kPad, y + L.hint_y}, pal.text_muted);
    if (L.status)
        ctx.canvas.draw_text(*L.status, {x + kPad, y + L.status_y}, pal.text_muted);

    if (show_end && L.end_label && L.end_rect.w > 0.0f)
    {
        const tk::Rect b{x + L.end_rect.x, y + L.end_rect.y, L.end_rect.w, L.end_rect.h};
        ctx.canvas.fill_rounded_rect(b, b.h * 0.5f, pal.chrome_bg);
        ctx.canvas.stroke_rounded_rect(b, b.h * 0.5f, pal.accent, 1.0f);
        ctx.canvas.draw_text(*L.end_label, {b.x + kBtnPadX, b.y + kBtnPadY}, pal.accent);
        if (interactive)
            geom_.push_back({Hit::Type::EndButton, m.event_id, {}, b});
    }

    return y + L.h;
}

const PollCardDisplay::Hit* PollCardDisplay::hit_test(tk::Point world) const
{
    // End button first: it never overlaps an answer row, but be explicit.
    for (const auto& h : geom_)
        if (h.type == Hit::Type::EndButton && contains(h.rect, world))
            return &h;
    for (const auto& h : geom_)
        if (contains(h.rect, world))
            return &h;
    return nullptr;
}

} // namespace tesseract::views
