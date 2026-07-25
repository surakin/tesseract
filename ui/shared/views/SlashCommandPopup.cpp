#include "views/SlashCommandPopup.h"
#include "tk/canvas.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include <algorithm>

namespace tesseract::views
{

void SlashCommandPopup::set_suggestions(std::vector<SlashCommandSuggestion> s)
{
    hint_mode_ = false;
    suggestions_ = std::move(s);
    // Preselect the first match so Tab/Enter accepts the top result without
    // the user having to press Down first. Stays -1 when the list is empty.
    selected_index_ = suggestions_.empty() ? -1 : 0;
    scroll_y_ = 0; // a new filter always starts the view at the top
    reset_transient_state_();
}

void SlashCommandPopup::show_hint(std::string text, bool is_error)
{
    hint_mode_ = true;
    hint_text_ = std::move(text);
    hint_is_error_ = is_error;
    selected_index_ = -1; // nothing to navigate/accept in hint mode
    scroll_y_ = 0;
    reset_transient_state_();
}

void SlashCommandPopup::clear_hint()
{
    hint_mode_ = false;
    hint_text_.clear();
    hint_is_error_ = false;
}

void SlashCommandPopup::set_selected_index(int index)
{
    if (suggestions_.empty())
    {
        selected_index_ = -1;
        return;
    }
    // -1 clears selection (matching ShortcodePopup); otherwise clamp into range.
    if (index < 0)
    {
        selected_index_ = -1;
    }
    else
    {
        selected_index_ = std::clamp(index, 0, (int)total_rows() - 1);
        ensure_row_visible(selected_index_);
    }
}

void SlashCommandPopup::paint_hint_row(tk::PaintCtx& ctx, const tk::Rect& row)
{
    const auto& pal = ctx.theme.palette;
    tk::TextStyle st{};
    st.role   = tk::FontRole::Body;
    st.halign = tk::TextHAlign::Leading;
    st.valign = tk::TextVAlign::Top;
    auto layout = ctx.factory.build_text(hint_text_, st);
    if (!layout)
    {
        return;
    }
    tk::Size sz = layout->measure();
    float ly = row.y + (row.h - sz.h) * 0.5f;
    ctx.canvas.draw_text(*layout, {row.x + 6.0f, ly},
                         hint_is_error_ ? pal.destructive : pal.text_muted);
}

void SlashCommandPopup::paint_row(tk::PaintCtx& ctx, const tk::Rect& row,
                                  size_t index, bool /*selected*/,
                                  bool /*hovered*/)
{
    if (hint_mode_)
    {
        paint_hint_row(ctx, row);
        return;
    }

    const auto& pal = ctx.theme.palette;
    const auto& s   = suggestions_[index];

    // Primary text: "/name <args_hint>" — top half of row with 6px left margin
    std::string primary = "/" + s.name;
    if (!s.args_hint.empty())
    {
        primary += " " + s.args_hint;
    }
    tk::TextStyle pst{};
    pst.role   = tk::FontRole::Body;
    pst.halign = tk::TextHAlign::Leading;
    pst.valign = tk::TextVAlign::Top;
    auto pl = ctx.factory.build_text(primary, pst);
    if (pl)
    {
        tk::Size psz = pl->measure();
        // Place primary text in the upper half of the row, vertically
        // centred within that half (kRowHeight/2 = 22px per half).
        float ly = row.y + (kRowHeight * 0.5f - psz.h) * 0.5f;
        ctx.canvas.draw_text(*pl, {row.x + 6.0f, ly}, pal.text_primary);
    }

    // Secondary text: description, plus a "via <bot>" disambiguation suffix
    // for bot-advertised commands (needed when two bots expose the same
    // command name, or just to make it clear this isn't a built-in) —
    // bottom half of row, muted colour.
    std::string secondary = s.description;
    if (!s.is_builtin)
    {
        const std::string& who =
            s.bot_display_name.empty() ? s.bot_sender_id : s.bot_display_name;
        std::string via = tk::trf(tk::tr("via {0}"), {who});
        secondary = secondary.empty() ? via : (secondary + "  \xC2\xB7  " + via);
    }
    if (!secondary.empty())
    {
        tk::TextStyle sst{};
        sst.role   = tk::FontRole::Small;
        sst.halign = tk::TextHAlign::Leading;
        sst.valign = tk::TextVAlign::Top;
        auto sl = ctx.factory.build_text(secondary, sst);
        if (sl)
        {
            tk::Size ssz = sl->measure();
            float half   = kRowHeight * 0.5f;
            float ly     = row.y + half + (half - ssz.h) * 0.5f;
            ctx.canvas.draw_text(*sl, {row.x + 6.0f, ly}, pal.text_muted);
        }
    }
}

} // namespace tesseract::views
