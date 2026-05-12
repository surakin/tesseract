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
constexpr float kTabSlotMin   = 24.0f;     // floor when many custom packs are present

// Built-in tab count: 1 (Frequents) + 8 (Unicode categories) = 9.
constexpr int kBuiltinTabCount = 1 + static_cast<int>(
    sizeof(tesseract::emoji::kCategories) /
    sizeof(tesseract::emoji::kCategories[0]));

const char* builtin_tab_glyph(int idx) {
    if (idx == 0) return "\xE2\xAD\x90";   // ⭐ Frequents indicator
    return tesseract::emoji::category_tab_glyph(
        tesseract::emoji::kCategories[idx - 1]);
}

bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(),  needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a))
                == std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Grid adapter — paints each emoji glyph centred in its cell.
// ─────────────────────────────────────────────────────────────────────────

class EmojiPicker::GridAdapter : public tk::GridAdapter {
public:
    explicit GridAdapter(EmojiPicker& owner) : owner_(owner) {}

    std::size_t count() const override {
        if (owner_.page_ == Page::CustomPack) {
            return owner_.current_emoticons_.size();
        }
        return owner_.current_glyphs_.size();
    }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                     bool selected, bool hovered) override {
        bool is_image_page = (owner_.page_ == Page::CustomPack);
        if (is_image_page) {
            if (index >= owner_.current_emoticons_.size()) return;
        } else {
            if (index >= owner_.current_glyphs_.size()) return;
        }

        if (selected) {
            ctx.canvas.fill_rounded_rect(
                bounds, 4.0f, ctx.theme.palette.subtle_pressed);
        } else if (hovered) {
            ctx.canvas.fill_rounded_rect(
                bounds, 4.0f, ctx.theme.palette.subtle_hover);
        }

        if (is_image_page) {
            const auto& entry = owner_.current_emoticons_[index];
            const tk::Image* img = nullptr;
            if (owner_.provider_) {
                img = owner_.provider_(entry.url, entry.url);
            }
            if (img) {
                float iw = static_cast<float>(img->width());
                float ih = static_cast<float>(img->height());
                float s  = std::min(bounds.w / iw, bounds.h / ih);
                float dw = iw * s;
                float dh = ih * s;
                ctx.canvas.draw_image(*img, {
                    bounds.x + (bounds.w - dw) * 0.5f,
                    bounds.y + (bounds.h - dh) * 0.5f,
                    dw, dh
                });
            } else {
                // Placeholder until the host loads the bitmap.
                tk::Rect ph{
                    bounds.x + 4.0f, bounds.y + 4.0f,
                    std::max(0.0f, bounds.w - 8.0f),
                    std::max(0.0f, bounds.h - 8.0f)
                };
                ctx.canvas.fill_rounded_rect(
                    ph, 4.0f, ctx.theme.palette.chrome_bg);
            }
            return;
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
        if (idx < 0) return;
        if (page_ == Page::CustomPack) {
            if (static_cast<std::size_t>(idx) >= current_emoticons_.size()) return;
            if (on_emoticon_selected) on_emoticon_selected(current_emoticons_[idx]);
            return;
        }
        if (static_cast<std::size_t>(idx) >= current_glyphs_.size()) return;
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
    refresh_emoticon_packs();
}

void EmojiPicker::refresh_frequents() {
    frequents_glyphs_.clear();
    if (client_) frequents_glyphs_ = client_->recent_emoji_top(32);
    if (page_ == Page::Frequents) rebuild_current_items();
}

void EmojiPicker::refresh_emoticon_packs() {
    custom_packs_.clear();
    if (client_) {
        for (auto& p : client_->list_image_packs()) {
            if (any(p.usage & tesseract::PackUsage::Emoticon)) {
                custom_packs_.push_back(std::move(p));
            }
        }
    }
    // If the active CustomPack went away, fall back to a built-in category.
    if (page_ == Page::CustomPack &&
        (custom_pack_idx_ < 0 ||
         static_cast<std::size_t>(custom_pack_idx_) >= custom_packs_.size())) {
        switch_to_category(category_);
    } else if (page_ == Page::CustomPack) {
        rebuild_current_items();
    }
}

void EmojiPicker::set_image_provider(ImageProvider p) {
    provider_ = std::move(p);
    if (grid_) grid_->invalidate_data();
}

int EmojiPicker::builtin_tab_count() const { return kBuiltinTabCount; }
int EmojiPicker::total_tab_count() const {
    return kBuiltinTabCount + static_cast<int>(custom_packs_.size());
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

void EmojiPicker::switch_to_custom_pack(int idx) {
    if (idx < 0 ||
        static_cast<std::size_t>(idx) >= custom_packs_.size()) return;
    page_            = Page::CustomPack;
    custom_pack_idx_ = idx;
    rebuild_current_items();
}

void EmojiPicker::rebuild_current_items() {
    current_glyphs_.clear();
    current_emoticons_.clear();
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
        case Page::CustomPack: {
            if (custom_pack_idx_ < 0 ||
                static_cast<std::size_t>(custom_pack_idx_) >= custom_packs_.size()) break;
            const auto& pack = custom_packs_[custom_pack_idx_];
            if (client_) {
                // Per-image usage filter pulls only emoticon-capable
                // entries — packs that allow both usages will appear in
                // both the StickerPicker and here, but each cell is
                // independently filtered.
                for (auto& img : client_->list_pack_images(
                        pack.id, tesseract::PackUsageFilter::Emoticon)) {
                    current_emoticons_.push_back(std::move(img));
                }
            }
            break;
        }
        case Page::Search: {
            // Unicode glyph search. Custom emoticon search would need its own
            // tab; out of scope for this PR.
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

    int total = total_tab_count();
    float tab_w = std::max(kTabSlotMin,
                            tab_rect_.w / static_cast<float>(total));
    int active;
    switch (page_) {
        case Page::Frequents:  active = 0; break;
        case Page::Category:   active = 1 + static_cast<int>(category_); break;
        case Page::CustomPack: active = kBuiltinTabCount + custom_pack_idx_; break;
        default:               active = -1; break;
    }
    for (int i = 0; i < total; ++i) {
        tk::Rect tab{
            tab_rect_.x + i * tab_w,
            tab_rect_.y,
            tab_w,
            tab_rect_.h
        };
        if (i == active) {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_pressed);
            tk::Rect underline{ tab.x, tab.y + tab.h - 2.0f, tab.w, 2.0f };
            ctx.canvas.fill_rect(underline, ctx.theme.palette.accent);
        } else if (i == hovered_tab_idx_) {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_hover);
        }

        if (i < kBuiltinTabCount) {
            tk::TextStyle st{};
            st.role   = tk::FontRole::Body;
            st.halign = tk::TextHAlign::Center;
            st.valign = tk::TextVAlign::Center;
            st.max_width  = tab.w;
            st.max_height = tab.h;
            auto layout = ctx.factory.build_text(builtin_tab_glyph(i), st);
            if (!layout) continue;
            tk::Size sz = layout->measure();
            ctx.canvas.draw_text(*layout,
                { tab.x + (tab.w - sz.w) * 0.5f,
                  tab.y + (tab.h - sz.h) * 0.5f },
                ctx.theme.palette.text_primary);
        } else {
            // Custom pack tab: avatar bitmap (when cached) or a fallback
            // single-letter initial. Same treatment as StickerPicker.
            int pack_idx = i - kBuiltinTabCount;
            const auto& pack = custom_packs_[pack_idx];
            const tk::Image* avatar = nullptr;
            if (provider_ && !pack.avatar_url.empty()) {
                avatar = provider_(pack.avatar_url, pack.avatar_url);
            }
            if (avatar) {
                float side = std::min(tab.h - 6.0f, tab.w - 6.0f);
                ctx.canvas.draw_image(*avatar, {
                    tab.x + (tab.w - side) * 0.5f,
                    tab.y + (tab.h - side) * 0.5f,
                    side, side
                });
            } else {
                std::string initial = pack.display_name.empty()
                    ? std::string("?")
                    : std::string(1, std::toupper(static_cast<unsigned char>(
                        pack.display_name[0])));
                tk::TextStyle st{};
                st.role   = tk::FontRole::Body;
                st.halign = tk::TextHAlign::Center;
                st.valign = tk::TextVAlign::Center;
                st.max_width  = tab.w;
                st.max_height = tab.h;
                auto layout = ctx.factory.build_text(initial, st);
                if (!layout) continue;
                tk::Size sz = layout->measure();
                ctx.canvas.draw_text(*layout,
                    { tab.x + (tab.w - sz.w) * 0.5f,
                      tab.y + (tab.h - sz.h) * 0.5f },
                    ctx.theme.palette.text_secondary);
            }
        }
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
    int total = total_tab_count();
    if (total == 0) return -1;
    float tab_w = std::max(kTabSlotMin,
                            tab_rect_.w / static_cast<float>(total));
    int idx = static_cast<int>(lx / tab_w);
    if (idx < 0 || idx >= total) return -1;
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
    } else if (hit < kBuiltinTabCount) {
        switch_to_category(
            tesseract::emoji::kCategories[hit - 1]);
    } else {
        switch_to_custom_pack(hit - kBuiltinTabCount);
    }
}

} // namespace tesseract::views
