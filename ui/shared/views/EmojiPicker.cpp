#include "EmojiPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <algorithm>
#include <cctype>

namespace tesseract::views {

namespace {

constexpr float kPadding      = 8.0f;
constexpr float kSearchHeight = 32.0f;
constexpr float kTabHeight    = 32.0f;
constexpr float kCellSize     = 32.0f;
constexpr float kCellGap      = 2.0f;

// Tab strip layout: nine equally-wide cells (Frequents + 8 categories).
constexpr int kTabCount = 1 + static_cast<int>(
    sizeof(tesseract::emoji::kCategories) /
    sizeof(tesseract::emoji::kCategories[0]));   // = 9

const char* tab_glyph(int idx) {
    if (idx == 0) return "\xE2\xAD\x90";   // ⭐ Frequents indicator
    return tesseract::emoji::category_tab_glyph(
        tesseract::emoji::kCategories[idx - 1]);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Grid adapter — paints each emoji glyph centred in its cell.
// ─────────────────────────────────────────────────────────────────────────

class EmojiPicker::GridAdapter : public tk::GridAdapter {
public:
    explicit GridAdapter(EmojiPicker& owner) : owner_(owner) {}

    std::size_t count() const override {
        return owner_.current_glyphs_.size();
    }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                     bool selected, bool hovered) override {
        if (index >= owner_.current_glyphs_.size()) return;

        if (selected) {
            ctx.canvas.fill_rounded_rect(
                bounds, 4.0f, ctx.theme.palette.subtle_pressed);
        } else if (hovered) {
            ctx.canvas.fill_rounded_rect(
                bounds, 4.0f, ctx.theme.palette.subtle_hover);
        }

        const std::string& glyph = owner_.current_glyphs_[index];

        tk::TextStyle st{};
        st.role   = tk::FontRole::Title;          // 15 pt — big enough for emoji
        st.halign = tk::TextHAlign::Center;
        st.valign = tk::TextVAlign::Center;
        st.max_width  = bounds.w;
        st.max_height = bounds.h;
        auto layout = ctx.factory.build_text(glyph, st);
        if (!layout) return;
        tk::Size sz = layout->measure();
        tk::Point origin{
            bounds.x + (bounds.w - sz.w) * 0.5f,
            bounds.y + (bounds.h - sz.h) * 0.5f
        };
        ctx.canvas.draw_text(*layout, origin, ctx.theme.palette.text_primary);
    }

private:
    EmojiPicker& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  EmojiPicker
// ─────────────────────────────────────────────────────────────────────────

EmojiPicker::~EmojiPicker() = default;

EmojiPicker::EmojiPicker()
    : grid_adapter_(std::make_unique<GridAdapter>(*this)) {
    auto grid = std::make_unique<tk::GridView>();
    grid->set_cell_size(kCellSize, kCellSize);
    grid->set_spacing(kCellGap, kCellGap);
    grid->set_padding(tk::Edges::all(kPadding * 0.5f));
    grid->set_adapter(grid_adapter_.get());
    grid->on_cell_clicked = [this](int idx) {
        if (idx < 0 ||
            static_cast<std::size_t>(idx) >= current_glyphs_.size()) return;
        std::string glyph = current_glyphs_[idx];
        if (client_) client_->recent_emoji_bump(glyph);
        if (on_selected) on_selected(glyph);
    };
    grid_ = add_child(std::move(grid));

    rebuild_current_items();
}

void EmojiPicker::set_client(tesseract::Client* c) {
    client_ = c;
    refresh_frequents();
}

void EmojiPicker::refresh_frequents() {
    frequents_glyphs_.clear();
    if (client_) frequents_glyphs_ = client_->recent_emoji_top(32);
    if (page_ == Page::Frequents) rebuild_current_items();
}

void EmojiPicker::set_search_query(std::string query) {
    query_ = std::move(query);
    if (query_.empty()) {
        // Returning to the previously active page (Frequents or
        // Category) when the user clears the search keeps the picker
        // navigable; the most useful default is the category that was
        // showing before they typed.
        if (page_ == Page::Search) {
            if (frequents_glyphs_.empty()) switch_to_category(category_);
            else                            switch_to_frequents();
        }
        return;
    }
    switch_to_search();
}

void EmojiPicker::switch_to_frequents() {
    page_ = Page::Frequents;
    rebuild_current_items();
}

void EmojiPicker::switch_to_category(tesseract::emoji::Category c) {
    page_     = Page::Category;
    category_ = c;
    rebuild_current_items();
}

void EmojiPicker::switch_to_search() {
    page_ = Page::Search;
    rebuild_current_items();
}

void EmojiPicker::rebuild_current_items() {
    current_glyphs_.clear();
    switch (page_) {
        case Page::Frequents:
            current_glyphs_ = frequents_glyphs_;
            break;
        case Page::Category: {
            auto entries = tesseract::emoji::by_category(category_);
            current_glyphs_.reserve(entries.size());
            for (const auto* e : entries) current_glyphs_.emplace_back(e->glyph);
            break;
        }
        case Page::Search: {
            auto entries = tesseract::emoji::filter(query_);
            current_glyphs_.reserve(entries.size());
            for (const auto* e : entries) current_glyphs_.emplace_back(e->glyph);
            break;
        }
    }
    if (grid_) {
        grid_->set_selected_index(-1);
        grid_->invalidate_data();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Layout
// ─────────────────────────────────────────────────────────────────────────

tk::Size EmojiPicker::measure(tk::LayoutCtx&, tk::Size constraints) {
    return constraints;
}

void EmojiPicker::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    search_rect_ = {
        bounds.x + kPadding,
        bounds.y + kPadding,
        bounds.w - kPadding * 2,
        kSearchHeight
    };

    tab_rect_ = {
        bounds.x,
        bounds.y + bounds.h - kTabHeight,
        bounds.w,
        kTabHeight
    };

    grid_rect_ = {
        bounds.x,
        bounds.y + kPadding * 2 + kSearchHeight,
        bounds.w,
        std::max(0.0f, bounds.h - kPadding * 2 - kSearchHeight - kTabHeight)
    };
    if (grid_) grid_->arrange(ctx, grid_rect_);
}

void EmojiPicker::paint(tk::PaintCtx& ctx) {
    // Backdrop + 1 px border so the picker reads as a card on any host
    // overlay style.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);
    ctx.canvas.stroke_rect(bounds_, ctx.theme.palette.border, 1.0f);

    // Search-row affordance behind the host's NativeTextField overlay.
    ctx.canvas.fill_rounded_rect(search_rect_, 6.0f,
                                   ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(search_rect_, 6.0f,
                                     ctx.theme.palette.border, 1.0f);

    if (grid_) grid_->paint(ctx);

    // Tab strip.
    ctx.canvas.fill_rect(tab_rect_, ctx.theme.palette.chrome_bg);
    ctx.canvas.fill_rect(
        { tab_rect_.x, tab_rect_.y, tab_rect_.w, 1.0f },
        ctx.theme.palette.separator);

    float tab_w = tab_rect_.w / static_cast<float>(kTabCount);
    int   active = (page_ == Page::Frequents) ? 0
                  : (page_ == Page::Category)
                        ? 1 + static_cast<int>(category_)
                        : -1;
    for (int i = 0; i < kTabCount; ++i) {
        tk::Rect tab{
            tab_rect_.x + i * tab_w,
            tab_rect_.y,
            tab_w,
            tab_rect_.h
        };
        if (i == active) {
            // Selected tab: subtle highlight + accent underline.
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_pressed);
            tk::Rect underline{ tab.x, tab.y + tab.h - 2.0f, tab.w, 2.0f };
            ctx.canvas.fill_rect(underline, ctx.theme.palette.accent);
        } else if (i == hovered_tab_idx_) {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_hover);
        }

        tk::TextStyle st{};
        st.role   = tk::FontRole::Body;
        st.halign = tk::TextHAlign::Center;
        st.valign = tk::TextVAlign::Center;
        st.max_width  = tab.w;
        st.max_height = tab.h;
        auto layout = ctx.factory.build_text(tab_glyph(i), st);
        if (!layout) continue;
        tk::Size sz = layout->measure();
        tk::Point origin{
            tab.x + (tab.w - sz.w) * 0.5f,
            tab.y + (tab.h - sz.h) * 0.5f
        };
        ctx.canvas.draw_text(*layout, origin, ctx.theme.palette.text_primary);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Input
// ─────────────────────────────────────────────────────────────────────────

tk::Rect EmojiPicker::tab_strip_rect() const { return tab_rect_; }

int EmojiPicker::tab_at(tk::Point local) const {
    // Translate widget-local to tab_rect_-local.
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h) return -1;
    float tab_w = tab_rect_.w / static_cast<float>(kTabCount);
    int idx = static_cast<int>(lx / tab_w);
    if (idx < 0 || idx >= kTabCount) return -1;
    return idx;
}

bool EmojiPicker::on_pointer_down(tk::Point local) {
    int t = tab_at(local);
    if (t >= 0) {
        pressed_tab_idx_ = t;
        return true;
    }
    return false;
}

void EmojiPicker::on_pointer_up(tk::Point local, bool inside_self) {
    if (pressed_tab_idx_ < 0) return;
    int t = inside_self ? tab_at(local) : -1;
    int hit = pressed_tab_idx_;
    pressed_tab_idx_ = -1;
    if (t != hit) return;
    if (hit == 0) {
        switch_to_frequents();
    } else {
        switch_to_category(
            tesseract::emoji::kCategories[hit - 1]);
    }
}

} // namespace tesseract::views
