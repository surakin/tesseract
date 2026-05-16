#include "views/ShortcodePopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include <algorithm>

namespace tesseract::views {

void ShortcodePopup::set_suggestions(std::vector<ShortcodeSuggestion> s) {
    suggestions_ = std::move(s);
    selected_index_ = -1;
    hovered_index_  = -1;
    pressed_index_  = -1;
}

void ShortcodePopup::set_selected_index(int index) {
    selected_index_ = index;
}

tk::Size ShortcodePopup::measure(tk::LayoutCtx&, tk::Size) {
    return { kWidth, kRowHeight * float(visible_rows()) };
}

void ShortcodePopup::arrange(tk::LayoutCtx&, tk::Rect bounds) {
    bounds_ = bounds;
}

void ShortcodePopup::paint(tk::PaintCtx& ctx) {
    const auto& pal = ctx.theme.palette;
    int n = visible_rows();
    for (int i = 0; i < n; ++i) {
        tk::Rect row{ bounds_.x, bounds_.y + float(i) * kRowHeight, bounds_.w, kRowHeight };

        // Background: selected > hovered > normal
        tk::Color bg = (i == selected_index_) ? pal.sidebar_selected
                     : (i == hovered_index_)  ? pal.subtle_hover
                     :                          pal.bg;
        ctx.canvas.fill_rect(row, bg);

        // 28×28 glyph cell left-aligned with 4px margin
        tk::Rect cell{
            row.x + 4.0f,
            row.y + (kRowHeight - 28.0f) * 0.5f,
            28.0f, 28.0f
        };
        const auto& s = suggestions_[std::size_t(i)];
        if (!s.glyph.empty()) {
            tk::TextStyle st{};
            st.role       = tk::FontRole::BigEmoji;
            st.halign     = tk::TextHAlign::Center;
            st.valign     = tk::TextVAlign::Center;
            st.max_width  = cell.w;
            st.max_height = cell.h;
            auto layout = ctx.factory.build_text(s.glyph, st);
            if (layout) {
                tk::Size sz = layout->measure();
                tk::Point origin{
                    cell.x + (cell.w - sz.w) * 0.5f,
                    cell.y + (cell.h - sz.h) * 0.5f
                };
                ctx.canvas.draw_text(*layout, origin, pal.text_primary);
            }
        } else {
            ctx.canvas.fill_rect(cell, pal.chrome_bg);
        }

        // Shortcode label
        std::string label = ":" + s.shortcode + ":";
        tk::TextStyle tst{};
        tst.role   = tk::FontRole::Body;
        tst.halign = tk::TextHAlign::Leading;
        tst.valign = tk::TextVAlign::Center;
        auto tl = ctx.factory.build_text(label, tst);
        if (tl) {
            tk::Size tsz = tl->measure();
            float lx = cell.x + cell.w + 8.0f;
            float ly = row.y + (kRowHeight - tsz.h) * 0.5f;
            ctx.canvas.draw_text(*tl, tk::Point{ lx, ly }, pal.text_primary);
        }

        // Row separator (except after last row)
        if (i < n - 1) {
            tk::Rect sep{ row.x, row.y + row.h - 1.0f, row.w, 1.0f };
            ctx.canvas.fill_rect(sep, pal.separator);
        }
    }
}

bool ShortcodePopup::on_pointer_down(tk::Point local) {
    pressed_index_ = row_at(local.y);
    return pressed_index_ >= 0;
}

void ShortcodePopup::on_pointer_up(tk::Point local, bool inside_self) {
    if (!inside_self) { pressed_index_ = -1; return; }
    int r = row_at(local.y);
    if (r >= 0 && r == pressed_index_ && r < (int)suggestions_.size()) {
        if (on_accepted) on_accepted(suggestions_[std::size_t(r)]);
    }
    pressed_index_ = -1;
}

void ShortcodePopup::on_pointer_move(tk::Point local) {
    hovered_index_ = row_at(local.y);
}

void ShortcodePopup::on_pointer_leave() {
    hovered_index_ = -1;
}

} // namespace tesseract::views
