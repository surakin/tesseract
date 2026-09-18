#include "ActivityMonitorView.h"

#include "tk/canvas.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

std::string translated_kind(const std::string& kind)
{
    if (kind == "loop")
        return tk::tr("Runs continuously");
    if (kind == "periodic")
        return tk::tr("Periodic");
    if (kind == "one-shot")
        return tk::tr("On demand");
    if (kind == "pool")
        return tk::tr("Thread pool");
    return {};
}

} // namespace

ActivityMonitorView::ActivityMonitorView()
{
    set_adapter(this);
}

std::string ActivityMonitorView::format_duration(std::int64_t ms)
{
    const std::int64_t s = std::max<std::int64_t>(ms, 0) / 1000;
    if (s < 60)
        return tk::trf(tk::tr("{0}s"), {std::to_string(s)});
    if (s < 3600)
        return tk::trf(tk::tr("{0}m"), {std::to_string(s / 60)});
    if (s < 86400)
        return tk::trf(tk::tr("{0}h"), {std::to_string(s / 3600)});
    return tk::trf(tk::tr("{0}d"), {std::to_string(s / 86400)});
}

void ActivityMonitorView::set_snapshot(std::vector<tesseract::ActivityEntry> entries,
                                       std::int64_t now_ms)
{
    now_ms_ = now_ms;
    rows_.clear();
    rows_.reserve(entries.size() + 8);

    std::size_t running = 0;
    std::size_t errors  = 0;
    for (const auto& e : entries)
    {
        if (e.state == tesseract::ActivityState::Running)
            ++running;
        else if (e.state == tesseract::ActivityState::Error)
            ++errors;
    }
    job_count_ = entries.size();

    Row summary;
    summary.kind = Row::Kind::Summary;
    summary.text = tk::trf(tk::tr("{0} jobs \xC2\xB7 {1} running \xC2\xB7 {2} with errors"),
                           {std::to_string(entries.size()), std::to_string(running),
                            std::to_string(errors)});
    rows_.push_back(std::move(summary));

    std::size_t i = 0;
    while (i < entries.size())
    {
        std::size_t j = i;
        while (j < entries.size() && entries[j].group == entries[i].group)
            ++j;
        Row header;
        header.kind = Row::Kind::Group;
        header.text = entries[i].group;
        rows_.push_back(std::move(header));
        for (std::size_t k = i; k < j; ++k)
        {
            Row r;
            r.kind = Row::Kind::Job;
            r.job  = std::move(entries[k]);
            rows_.push_back(std::move(r));
        }
        i = j;
    }
    invalidate_data();
}

std::size_t ActivityMonitorView::count() const
{
    return rows_.size();
}

float ActivityMonitorView::measure_row_height(std::size_t index, tk::LayoutCtx&, float)
{
    if (index >= rows_.size())
        return 0.0f;
    switch (rows_[index].kind)
    {
    case Row::Kind::Summary:
        return kSummaryH;
    case Row::Kind::Group:
        return kGroupH;
    case Row::Kind::Job:
        return kJobH;
    }
    return kJobH;
}

std::string ActivityMonitorView::row_key(std::size_t index) const
{
    if (index >= rows_.size())
        return {};
    const Row& r = rows_[index];
    switch (r.kind)
    {
    case Row::Kind::Summary:
        return "summary";
    case Row::Kind::Group:
        return "group:" + r.text;
    case Row::Kind::Job:
        return "job:" + r.job.group + "/" + r.job.name;
    }
    return {};
}

std::string ActivityMonitorView::status_text_(const tesseract::ActivityEntry& e) const
{
    switch (e.state)
    {
    case tesseract::ActivityState::Running:
        if (e.kind == "loop" || e.kind == "pool")
            return tk::tr("Running");
        return tk::trf(tk::tr("Running for {0}"), {format_duration(now_ms_ - e.last_started_ms)});
    case tesseract::ActivityState::Error:
        return tk::tr("Error");
    case tesseract::ActivityState::Idle:
        break;
    }
    if (e.last_finished_ms <= 0)
        return tk::tr("Not run yet");
    return tk::trf(tk::tr("{0} ago"), {format_duration(now_ms_ - e.last_finished_ms)});
}

std::string ActivityMonitorView::secondary_text_(const tesseract::ActivityEntry& e) const
{
    std::string out = translated_kind(e.kind);
    if (e.run_count > 0)
    {
        if (!out.empty())
            out += " \xC2\xB7 ";
        out += tk::trf(tk::trn("{0} run", "{0} runs", static_cast<long>(e.run_count)),
                       {std::to_string(e.run_count)});
    }
    if (!e.detail.empty())
    {
        if (!out.empty())
            out += " \xC2\xB7 ";
        out += e.detail;
    }
    return out;
}

void ActivityMonitorView::paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect r,
                                    bool /*selected*/, bool hovered)
{
    if (index >= rows_.size())
        return;
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;
    const Row& row  = rows_[index];

    if (row.kind == Row::Kind::Summary || row.kind == Row::Kind::Group)
    {
        const bool summary = row.kind == Row::Kind::Summary;
        cv.fill_rect(r, summary ? pal.chrome_bg : pal.section_header_bg);
        tk::TextStyle st{};
        st.role      = summary ? tk::FontRole::Body : tk::FontRole::Small;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, r.w - 2 * kPadX);
        if (auto layout = ctx.factory.build_text(row.text, st))
        {
            const tk::Size sz = layout->measure();
            cv.draw_text(*layout, {r.x + kPadX, r.y + (r.h - sz.h) * 0.5f},
                         summary ? pal.text_primary : pal.text_secondary);
        }
        return;
    }

    const auto& e = row.job;
    cv.fill_rect(r, pal.bg);
    if (hovered)
        cv.fill_rect(r, pal.subtle_hover);
    cv.fill_rect({r.x, r.bottom() - 1.0f, r.w, 1.0f}, pal.separator);

    const tk::Color dot = e.state == tesseract::ActivityState::Running ? pal.success
                          : e.state == tesseract::ActivityState::Error ? pal.destructive
                                                                       : pal.text_muted;
    cv.fill_rounded_rect({r.x + kPadX, r.y + 12.0f, kDotD, kDotD}, kDotD * 0.5f, dot);

    // Right column: status / relative time.
    float status_w = 0.0f;
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Small;
        st.wrap = false;
        if (auto layout = ctx.factory.build_text(status_text_(e), st))
        {
            const tk::Size sz = layout->measure();
            status_w          = sz.w;
            cv.draw_text(*layout, {r.x + r.w - kPadX - sz.w, r.y + 10.0f},
                         e.state == tesseract::ActivityState::Error ? pal.destructive
                                                                    : pal.text_secondary);
        }
    }

    const float text_left = r.x + kPadX + kDotD + 10.0f;
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, r.x + r.w - kPadX - status_w - 12.0f - text_left);
        if (auto layout = ctx.factory.build_text(e.name, st))
            cv.draw_text(*layout, {text_left, r.y + 8.0f}, pal.text_primary);
    }
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.wrap      = false;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = std::max(0.0f, r.x + r.w - kPadX - text_left);
        if (auto layout = ctx.factory.build_text(secondary_text_(e), st))
            cv.draw_text(*layout, {text_left, r.y + 29.0f},
                         e.state == tesseract::ActivityState::Error ? pal.destructive
                                                                    : pal.text_muted);
    }
}

tk::Role ActivityMonitorView::access_role_for_row(std::size_t index) const
{
    if (index >= rows_.size() || rows_[index].kind != Row::Kind::Job)
        return tk::Role::None;
    return tk::Role::ListItem;
}

std::string ActivityMonitorView::access_name_for_row(std::size_t index) const
{
    if (index >= rows_.size())
        return {};
    const Row& r = rows_[index];
    if (r.kind != Row::Kind::Job)
        return r.text;
    return r.job.name + ", " + status_text_(r.job);
}

} // namespace tesseract::views
