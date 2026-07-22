#include "PronounsEditor.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <utility>

namespace tesseract::views
{

PronounsEditor::PronounsEditor()
{
    if (!host())
        return;

    rows_.reserve(kMaxRows);
    for (int i = 0; i < kMaxRows; ++i)
    {
        Row row;

        auto lang = tk::create_widget<LanguagePicker>(this);
        row.lang = add_child(std::move(lang));
        row.lang->on_changed = [this, i](std::string code)
        {
            if (static_cast<std::size_t>(i) >= entries_.size())
                return;
            entries_[static_cast<std::size_t>(i)].language = std::move(code);
            notify_changed_();
        };
        auto summary = tk::create_widget<tk::TextField>(this, kRowH);
        summary->set_placeholder(tk::tr("Pronouns (e.g. she/her)"));
        row.summary = add_child(std::move(summary));
        row.summary->set_on_submit(
            [this, i]
            {
                if (static_cast<std::size_t>(i) >= entries_.size())
                    return;
                entries_[static_cast<std::size_t>(i)].summary =
                    rows_[static_cast<std::size_t>(i)].summary->text();
                notify_changed_();
            });

        auto remove = tk::create_widget<tk::Button>(
            this, tk::tr("\xC3\x97"), std::function<void()>{},
            tk::Button::Variant::Icon);
        row.remove = add_child(std::move(remove));
        row.remove->set_on_click([this, i] { remove_row_(static_cast<std::size_t>(i)); });

        rows_.push_back(row);
    }

    auto add_btn = tk::create_widget<tk::Button>(
        this, tk::tr("+ Add language"), [this] { add_row_(); },
        tk::Button::Variant::Subtle);
    add_button_ = add_child(std::move(add_btn));
}

// ---- data -------------------------------------------------------------

void PronounsEditor::set_pronouns(std::vector<tesseract::PronounEntry> entries)
{
    // Preserve any row still mid-edit locally (language or summary not yet
    // both filled in) that this fresh data doesn't know about. Completing
    // *one* row (e.g. picking its language) immediately saves + refetches
    // (see notify_changed_()/ShellBase::handle_profile_field_result_ui_) —
    // that save only ever includes *complete* entries, so the refetch's
    // response naturally omits any *other* row still in progress. Without
    // this, that refetch would silently wipe the in-progress row from the
    // UI the instant a sibling row's edit completes.
    for (auto& local : entries_)
    {
        if (local.language.empty() || local.summary.empty())
            entries.push_back(std::move(local));
    }
    entries_ = std::move(entries);
    refresh_slots_();
    // Data usually arrives asynchronously, after the Settings page's first
    // layout pass (pronouns start empty) — without this, the whole
    // PronounsEditor -> ExtendedFields -> AccountSection height cascade goes
    // stale and everything below (in particular the "Add language" button)
    // stays positioned for an empty list. Mirrors add_row_()/remove_row_().
    if (auto* h = host())
        h->request_relayout();
}

void PronounsEditor::refresh_slots_()
{
    if (rows_.empty())
        return;
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const bool has_entry = i < entries_.size();
        if (has_entry)
        {
            rows_[i].lang->set_value(entries_[i].language);
            rows_[i].summary->set_text(entries_[i].summary);
        }
    }
}

void PronounsEditor::add_row_()
{
    if (entries_.size() >= static_cast<std::size_t>(kMaxRows))
        return;
    entries_.push_back({tk::current_locale(), std::string(), std::string()});
    refresh_slots_();
    if (auto* h = host())
        h->request_relayout();
}

void PronounsEditor::remove_row_(std::size_t index)
{
    if (index >= entries_.size())
        return;
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    refresh_slots_();
    notify_changed_();
    if (auto* h = host())
        h->request_relayout();
}

void PronounsEditor::notify_changed_()
{
    if (!on_changed)
        return;
    std::vector<tesseract::PronounEntry> complete;
    complete.reserve(entries_.size());
    for (const auto& e : entries_)
        if (!e.language.empty() && !e.summary.empty())
            complete.push_back(e);
    on_changed(complete);
}

// ---- state --------------------------------------------------------------

void PronounsEditor::set_editable(bool editable)
{
    editable_ = editable;
    for (auto& row : rows_)
    {
        row.lang->set_enabled(editable && !busy_);
        row.summary->set_enabled(editable && !busy_);
        row.remove->set_enabled(editable && !busy_);
    }
    if (add_button_)
        add_button_->set_enabled(editable && !busy_ &&
                                 entries_.size() < static_cast<std::size_t>(kMaxRows));
}

void PronounsEditor::set_visible(bool v)
{
    Widget::set_visible(v);
    // Re-showing must still respect which rows currently hold an entry —
    // never unconditionally reveal every fixed slot.
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const bool show = v && i < entries_.size();
        rows_[i].summary->set_visible(show);
        rows_[i].lang->set_visible(show);
    }
    if (add_button_)
        add_button_->set_visible(v && entries_.size() < static_cast<std::size_t>(kMaxRows));
}

void PronounsEditor::set_compact(bool compact)
{
    for (auto& row : rows_)
    {
        row.summary->set_compact(compact);
        row.lang->set_compact(compact);
    }
}

void PronounsEditor::on_theme_changed(const tk::Theme& t)
{
    // row.lang (LanguagePicker) themes its own native field via its own
    // on_theme_changed override, invoked automatically since it's a proper
    // child of this widget — only the summary TextField needs it here,
    // mirroring AccountSection::ExtendedFields::on_theme_changed's identical
    // handling of its own tz/bio fields.
    for (auto& row : rows_)
        row.summary->set_text_color(t.palette.text_primary);
}

void PronounsEditor::set_busy(bool busy)
{
    busy_ = busy;
    if (busy)
        error_.clear();
    set_editable(editable_);
}

void PronounsEditor::set_error(std::string error)
{
    error_ = std::move(error);
    error_layout_.reset();
}

// ---- layout ---------------------------------------------------------------

float PronounsEditor::row_y_(int slot) const
{
    return bounds_.y + static_cast<float>(slot) * (kRowH + kRowGap);
}

tk::Size PronounsEditor::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const std::size_t visible =
        std::min(entries_.size(), static_cast<std::size_t>(kMaxRows));
    const float rows_h = visible == 0
                              ? 0.0f
                              : static_cast<float>(visible) * kRowH +
                                    static_cast<float>(visible - 1) * kRowGap +
                                    kRowGap;
    const float error_h = error_.empty() ? 0.0f : kErrorGap + kErrorH;
    const float h = rows_h + kAddButtonH + error_h;
    return {constraints.w > 0 ? constraints.w : 0, h};
}

void PronounsEditor::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const std::size_t visible =
        std::min(entries_.size(), static_cast<std::size_t>(kMaxRows));

    const float lang_x = bounds.x;
    const float summary_x = lang_x + kLangW + kFieldGap;
    // Cap the summary field to a comfortable width instead of stretching it
    // across the panel's full remaining width (unlike the plain tz/bio
    // fields, this row ends in a remove button — stretching would strand it
    // far from the actual fields on a wide settings panel). Still shrinks on
    // a narrower panel rather than overflowing it.
    const float max_summary_w = std::max(
        0.0f, bounds.x + bounds.w - summary_x - kFieldGap - kRemoveW - kCardPadX);
    const float summary_w = std::min(kSummaryW, max_summary_w);
    const float remove_x = summary_x + summary_w + kFieldGap;

    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const bool show = i < visible && visible_in_tree();
        const float ry = row_y_(static_cast<int>(i));

        rows_[i].lang->set_visible(show);
        rows_[i].summary->set_visible(show);
        rows_[i].remove->set_visible(show);
        if (!show)
            continue;

        rows_[i].lang->arrange(ctx, {lang_x, ry, kLangW, kRowH});
        rows_[i].summary->arrange(ctx, {summary_x, ry, summary_w, kRowH});
        rows_[i].remove->arrange(ctx, {remove_x, ry, kRemoveW, kRowH});
    }

    const float add_y = row_y_(static_cast<int>(visible));
    if (add_button_)
    {
        const bool at_cap = entries_.size() >= static_cast<std::size_t>(kMaxRows);
        add_button_->set_visible(visible_in_tree() && !at_cap);
        add_button_->arrange(ctx, {bounds.x, add_y, 140.0f, kAddButtonH});
    }

    error_layout_.reset();
}

// ---- paint ------------------------------------------------------------

void PronounsEditor::paint(tk::PaintCtx& ctx)
{
    // Card background per visible row, grouping its language field, summary
    // field, and remove button into one visible unit — drawn before
    // paint_children() so those controls paint on top of it, not underneath.
    // A thin divider between the language field and summary field keeps the
    // two independently-editable fields from reading as one long field.
    for (const auto& row : rows_)
    {
        if (!row.remove->visible())
            continue;
        const tk::Rect lang_b    = row.lang->bounds();
        const tk::Rect summary_b = row.summary->bounds();
        const tk::Rect remove_b  = row.remove->bounds();
        const tk::Rect card{lang_b.x - kCardPadX, lang_b.y - kCardPadY,
                            (remove_b.x + remove_b.w) - lang_b.x + 2 * kCardPadX,
                            lang_b.h + 2 * kCardPadY};
        ctx.canvas.fill_rounded_rect(card, tesseract::visual::kRadiusSM,
                                     ctx.theme.palette.compose_card_bg);

        const float divider_x = (lang_b.x + lang_b.w + summary_b.x) * 0.5f;
        ctx.canvas.fill_rect({divider_x, lang_b.y, 1.0f, lang_b.h},
                             ctx.theme.palette.border);
    }

    paint_children(ctx); // this override adds error text; children still need their own paint()

    if (!remove_glyph_layout_)
    {
        tk::TextStyle st{};
        st.role   = tk::FontRole::Body;
        st.halign = tk::TextHAlign::Leading;
        remove_glyph_layout_ = ctx.factory.build_text("\xC3\x97", st);
    }
    if (remove_glyph_layout_)
    {
        const tk::Size sz = remove_glyph_layout_->measure();
        for (auto& row : rows_)
        {
            if (!row.remove->visible())
                continue;
            const tk::Rect b = row.remove->bounds();
            const float gx = b.x + (b.w - sz.w) * 0.5f;
            const float gy = b.y + (b.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*remove_glyph_layout_, {gx, gy},
                                 ctx.theme.palette.text_secondary);
        }
    }

    if (!error_.empty())
    {
        const std::size_t visible =
            std::min(entries_.size(), static_cast<std::size_t>(kMaxRows));
        const float err_y = row_y_(static_cast<int>(visible)) + kAddButtonH + kErrorGap;
        if (!error_layout_)
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.max_width = bounds_.w;
            error_layout_ = ctx.factory.build_text(error_, st);
        }
        if (error_layout_)
        {
            ctx.canvas.draw_text(*error_layout_, {bounds_.x, err_y},
                                 tk::Color::rgb(0xcc3333));
        }
    }
}

} // namespace tesseract::views
