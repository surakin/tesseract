#include "EmojiPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <algorithm>
#include <cctype>

namespace tesseract::views
{

namespace
{

constexpr float kPadding = 8.0f;
constexpr float kSearchHeight = 32.0f;
constexpr float kTabHeight = 32.0f;
constexpr float kCellSize = 32.0f;
constexpr float kCellGap = 2.0f;
constexpr float kTabSlotMin = 24.0f; // floor when many custom packs are present

// Built-in tab count: 1 (Frequents) + 8 (Unicode categories) = 9.
constexpr int kBuiltinTabCount =
    1 + static_cast<int>(sizeof(tesseract::emoji::kCategories) /
                         sizeof(tesseract::emoji::kCategories[0]));

const char* builtin_tab_glyph(int idx)
{
    if (idx == 0)
    {
        return "\xE2\xAD\x90"; // ⭐ Frequents indicator
    }
    return tesseract::emoji::category_tab_glyph(
        tesseract::emoji::kCategories[idx - 1]);
}

bool icontains(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
    {
        return true;
    }
    auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Grid adapter — paints each emoji glyph centred in its cell.
// ─────────────────────────────────────────────────────────────────────────

class EmojiPicker::GridAdapter : public tk::GridAdapter
{
public:
    explicit GridAdapter(EmojiPicker& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        if (owner_.page_ == Page::CustomPack)
        {
            return owner_.current_emoticons_.size();
        }
        return owner_.current_glyphs_.size();
    }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override
    {
        bool is_image_page = (owner_.page_ == Page::CustomPack);
        if (is_image_page)
        {
            if (index >= owner_.current_emoticons_.size())
            {
                return;
            }
        }
        else
        {
            if (index >= owner_.current_glyphs_.size())
            {
                return;
            }
        }

        if (selected)
        {
            ctx.canvas.fill_rounded_rect(bounds, 4.0f,
                                         ctx.theme.palette.subtle_pressed);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rounded_rect(bounds, 4.0f,
                                         ctx.theme.palette.subtle_hover);
        }

        if (is_image_page)
        {
            const auto& entry = owner_.current_emoticons_[index];
            const tk::Image* img = nullptr;
            if (owner_.provider_)
            {
                img = owner_.provider_(entry.url, entry.url);
            }
            if (img)
            {
                float iw = static_cast<float>(img->width());
                float ih = static_cast<float>(img->height());
                float s = std::min(bounds.w / iw, bounds.h / ih);
                float dw = iw * s;
                float dh = ih * s;
                ctx.canvas.draw_image(*img, {bounds.x + (bounds.w - dw) * 0.5f,
                                             bounds.y + (bounds.h - dh) * 0.5f,
                                             dw, dh});
            }
            else
            {
                // Placeholder until the host loads the bitmap.
                tk::Rect ph{bounds.x + 4.0f, bounds.y + 4.0f,
                            std::max(0.0f, bounds.w - 8.0f),
                            std::max(0.0f, bounds.h - 8.0f)};
                ctx.canvas.fill_rounded_rect(ph, 4.0f,
                                             ctx.theme.palette.chrome_bg);
            }
            return;
        }

        const std::string& glyph = owner_.current_glyphs_[index];

        tk::TextStyle st{};
        st.role = tk::FontRole::Title; // 15 pt — big enough for emoji
        st.halign = tk::TextHAlign::Center;
        st.valign = tk::TextVAlign::Center;
        st.max_width = bounds.w;
        st.max_height = bounds.h;
        auto layout = ctx.factory.build_text(glyph, st);
        if (!layout)
        {
            return;
        }
        ctx.canvas.draw_text(*layout, {bounds.x, bounds.y},
                             ctx.theme.palette.text_primary);
    }

private:
    EmojiPicker& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  EmojiPicker
// ─────────────────────────────────────────────────────────────────────────

EmojiPicker::~EmojiPicker() = default;

EmojiPicker::EmojiPicker() : grid_adapter_(std::make_unique<GridAdapter>(*this))
{
    auto grid = std::make_unique<tk::GridView>();
    grid->set_cell_size(kCellSize, kCellSize);
    grid->set_spacing(kCellGap, kCellGap);
    grid->set_padding(tk::Edges::all(kPadding * 0.5f));
    grid->set_adapter(grid_adapter_.get());
    grid->on_cell_clicked = [this](int idx)
    {
        if (idx < 0)
        {
            return;
        }
        if (page_ == Page::CustomPack)
        {
            if (static_cast<std::size_t>(idx) >= current_emoticons_.size())
            {
                return;
            }
            if (on_emoticon_selected)
            {
                on_emoticon_selected(current_emoticons_[idx]);
            }
            return;
        }
        if (static_cast<std::size_t>(idx) >= current_glyphs_.size())
        {
            return;
        }
        std::string glyph = current_glyphs_[idx];
        if (client_)
        {
            client_->recent_emoji_bump(glyph);
        }
        if (on_selected)
        {
            on_selected(glyph);
        }
    };
    grid_ = add_child(std::move(grid));

    rebuild_current_items();
}

void EmojiPicker::set_client(tesseract::Client* c)
{
    client_ = c;
    refresh_frequents();
    refresh_emoticon_packs();
}

void EmojiPicker::refresh_frequents()
{
    frequents_glyphs_.clear();
    if (client_)
    {
        frequents_glyphs_ = client_->recent_emoji_top(32);
    }
    if (page_ == Page::Frequents)
    {
        if (frequents_glyphs_.empty())
        {
            switch_to_category(category_);
        }
        else
        {
            rebuild_current_items();
        }
    }
}

void EmojiPicker::refresh_emoticon_packs()
{
    custom_packs_.clear();
    if (client_)
    {
        for (auto& p : client_->list_image_packs())
        {
            if (any(p.usage & tesseract::PackUsage::Emoticon))
            {
                custom_packs_.push_back(std::move(p));
            }
        }
    }
    // If the active CustomPack went away, fall back to a built-in category.
    if (page_ == Page::CustomPack &&
        (custom_pack_idx_ < 0 ||
         static_cast<std::size_t>(custom_pack_idx_) >= custom_packs_.size()))
    {
        switch_to_category(category_);
    }
    else if (page_ == Page::CustomPack)
    {
        rebuild_current_items();
    }
}

void EmojiPicker::set_image_provider(ImageProvider p)
{
    provider_ = std::move(p);
    if (grid_)
    {
        grid_->invalidate_data();
    }
}

void EmojiPicker::invalidate_image_cache()
{
    if (grid_)
    {
        grid_->invalidate_data();
    }
}

int EmojiPicker::builtin_tab_count() const
{
    return kBuiltinTabCount;
}
int EmojiPicker::total_tab_count() const
{
    // kBuiltinTabCount includes 1 slot for Frequents; exclude it when empty.
    return frequents_tab_offset() + (kBuiltinTabCount - 1) +
           static_cast<int>(custom_packs_.size());
}

void EmojiPicker::set_search_query(std::string query)
{
    query_ = std::move(query);
    if (query_.empty())
    {
        // Returning to the previously active page (Frequents or
        // Category) when the user clears the search keeps the picker
        // navigable; the most useful default is the category that was
        // showing before they typed.
        if (page_ == Page::Search)
        {
            if (frequents_glyphs_.empty())
            {
                switch_to_category(category_);
            }
            else
            {
                switch_to_frequents();
            }
        }
        return;
    }
    switch_to_search();
}

void EmojiPicker::switch_to_frequents()
{
    page_ = Page::Frequents;
    rebuild_current_items();
}

void EmojiPicker::switch_to_category(tesseract::emoji::Category c)
{
    page_ = Page::Category;
    category_ = c;
    rebuild_current_items();
}

void EmojiPicker::switch_to_search()
{
    page_ = Page::Search;
    rebuild_current_items();
}

void EmojiPicker::switch_to_custom_pack(int idx)
{
    if (idx < 0 || static_cast<std::size_t>(idx) >= custom_packs_.size())
    {
        return;
    }
    page_ = Page::CustomPack;
    custom_pack_idx_ = idx;
    rebuild_current_items();
}

void EmojiPicker::rebuild_current_items()
{
    current_glyphs_.clear();
    current_emoticons_.clear();
    current_shortcodes_.clear();
    hovered_grid_cell_ = -1;
    switch (page_)
    {
    case Page::Frequents:
    {
        current_glyphs_ = frequents_glyphs_;
        // Look up each frequent glyph in the emoji table to get its
        // canonical shortcode.
        const auto& table = tesseract::emoji::all();
        for (const auto& glyph : current_glyphs_)
        {
            std::string sc;
            for (const auto& e : table)
            {
                if (e.glyph == glyph && !e.shortcodes.empty())
                {
                    // First space-delimited token is the canonical shortcode.
                    auto sv = e.shortcodes;
                    auto pos = sv.find(' ');
                    auto tok = (pos == std::string_view::npos)
                                   ? sv
                                   : sv.substr(0, pos);
                    sc = ":" + std::string(tok) + ":";
                    break;
                }
            }
            current_shortcodes_.push_back(std::move(sc));
        }
        break;
    }
    case Page::Category:
    {
        auto entries = tesseract::emoji::by_category(category_);
        current_glyphs_.reserve(entries.size());
        current_shortcodes_.reserve(entries.size());
        for (const auto* e : entries)
        {
            current_glyphs_.emplace_back(e->glyph);
            std::string sc;
            if (!e->shortcodes.empty())
            {
                auto pos = e->shortcodes.find(' ');
                auto tok = (pos == std::string_view::npos)
                               ? e->shortcodes
                               : e->shortcodes.substr(0, pos);
                sc = ":" + std::string(tok) + ":";
            }
            current_shortcodes_.push_back(std::move(sc));
        }
        break;
    }
    case Page::CustomPack:
    {
        if (custom_pack_idx_ < 0 ||
            static_cast<std::size_t>(custom_pack_idx_) >= custom_packs_.size())
        {
            break;
        }
        const auto& pack = custom_packs_[custom_pack_idx_];
        if (client_)
        {
            // Per-image usage filter pulls only emoticon-capable
            // entries — packs that allow both usages will appear in
            // both the StickerPicker and here, but each cell is
            // independently filtered.
            for (auto& img : client_->list_pack_images(
                     pack.id, tesseract::PackUsageFilter::Emoticon))
            {
                current_shortcodes_.push_back(":" + img.shortcode + ":");
                current_emoticons_.push_back(std::move(img));
            }
        }
        break;
    }
    case Page::Search:
    {
        // Unicode glyph search. Custom emoticon search would need its own
        // tab; out of scope for this PR.
        auto entries = tesseract::emoji::filter(query_);
        current_glyphs_.reserve(entries.size());
        current_shortcodes_.reserve(entries.size());
        for (const auto* e : entries)
        {
            current_glyphs_.emplace_back(e->glyph);
            std::string sc;
            if (!e->shortcodes.empty())
            {
                auto pos = e->shortcodes.find(' ');
                auto tok = (pos == std::string_view::npos)
                               ? e->shortcodes
                               : e->shortcodes.substr(0, pos);
                sc = ":" + std::string(tok) + ":";
            }
            current_shortcodes_.push_back(std::move(sc));
        }
        break;
    }
    }
    if (grid_)
    {
        grid_->set_selected_index(-1);
        grid_->invalidate_data();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Layout
// ─────────────────────────────────────────────────────────────────────────

tk::Size EmojiPicker::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void EmojiPicker::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    search_rect_ = {bounds.x + kPadding, bounds.y + kPadding,
                    bounds.w - kPadding * 2, kSearchHeight};

    tab_rect_ = {bounds.x, bounds.y + bounds.h - kTabHeight, bounds.w,
                 kTabHeight};

    grid_rect_ = {
        bounds.x, bounds.y + kPadding * 2 + kSearchHeight, bounds.w,
        std::max(0.0f, bounds.h - kPadding * 2 - kSearchHeight - kTabHeight)};
    if (grid_)
    {
        grid_->arrange(ctx, grid_rect_);
    }
}

void EmojiPicker::paint(tk::PaintCtx& ctx)
{
    // Backdrop + 1 px border so the picker reads as a card on any host
    // overlay style.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Search-row affordance behind the host's NativeTextField overlay.
    ctx.canvas.fill_rounded_rect(search_rect_, 6.0f,
                                 ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(search_rect_, 6.0f, ctx.theme.palette.border,
                                   1.0f);

    if (grid_)
    {
        grid_->paint(ctx);
    }

    // Shortcode tooltip: shown when a grid cell is hovered.
    if (hovered_grid_cell_ >= 0 &&
        static_cast<std::size_t>(hovered_grid_cell_) <
            current_shortcodes_.size())
    {
        const std::string& sc = current_shortcodes_[hovered_grid_cell_];
        if (!sc.empty())
        {
            tk::TextStyle small{};
            small.role = tk::FontRole::Small;
            auto layout = ctx.factory.build_text(sc, small);
            if (layout)
            {
                tk::Size tsz = layout->measure();
                constexpr float kPad = 4.0f;
                constexpr float kRadius = 4.0f;
                tk::Rect cell_r = grid_->rect_at(hovered_grid_cell_);
                float tx = cell_r.x + (cell_r.w - tsz.w) / 2.0f - kPad;
                float ty = cell_r.y - tsz.h - kPad * 2 - 2.0f;
                if (ty < bounds_.y)
                {
                    ty = cell_r.y + cell_r.h + 2.0f;
                }
                // Clamp horizontally so the tooltip stays within picker bounds.
                tx = std::max(bounds_.x + kPad,
                              std::min(tx, bounds_.x + bounds_.w - tsz.w -
                                               kPad * 2 - kPad));
                tk::Rect bg{tx, ty, tsz.w + kPad * 2, tsz.h + kPad * 2};
                ctx.canvas.push_clip_rect(bounds_);
                ctx.canvas.fill_rounded_rect(bg, kRadius,
                                             ctx.theme.palette.chrome_bg);
                ctx.canvas.stroke_rounded_rect(
                    bg, kRadius, ctx.theme.palette.popup_border, 1.0f);
                ctx.canvas.draw_text(*layout, {bg.x + kPad, bg.y + kPad},
                                     ctx.theme.palette.text_primary);
                ctx.canvas.pop_clip();
            }
        }
    }

    // Tab strip.
    ctx.canvas.fill_rect(tab_rect_, ctx.theme.palette.chrome_bg);
    ctx.canvas.fill_rect({tab_rect_.x, tab_rect_.y, tab_rect_.w, 1.0f},
                         ctx.theme.palette.separator);

    int total = total_tab_count();
    float tab_w =
        std::max(kTabSlotMin, tab_rect_.w / static_cast<float>(total));
    const int foff = frequents_tab_offset(); // 0 or 1
    int active;
    switch (page_)
    {
    case Page::Frequents:
        active = has_frequents_tab() ? 0 : -1;
        break;
    case Page::Category:
        active = foff + static_cast<int>(category_);
        break;
    case Page::CustomPack:
        active = foff + (kBuiltinTabCount - 1) + custom_pack_idx_;
        break;
    default:
        active = -1;
        break;
    }
    ctx.canvas.push_clip_rect(tab_rect_);
    for (int i = 0; i < total; ++i)
    {
        tk::Rect tab{tab_rect_.x + i * tab_w - tab_scroll_offset_, tab_rect_.y,
                     tab_w, tab_rect_.h};
        if (i == active)
        {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_pressed);
            tk::Rect underline{tab.x, tab.y + tab.h - 2.0f, tab.w, 2.0f};
            ctx.canvas.fill_rect(underline, ctx.theme.palette.accent);
        }
        else if (i == hovered_tab_idx_)
        {
            ctx.canvas.fill_rect(tab, ctx.theme.palette.subtle_hover);
        }

        // Tab content: frequents star (when visible), builtin category glyphs,
        // then custom pack avatars/initials.
        if (has_frequents_tab() && i == 0)
        {
            // Frequents tab — reuse builtin_tab_glyph(0) which is the star.
            tk::TextStyle st{};
            st.role = tk::FontRole::Body;
            auto layout = ctx.factory.build_text(builtin_tab_glyph(0), st);
            if (!layout)
            {
                continue;
            }
            tk::Size sz = layout->measure();
            ctx.canvas.draw_text(
                *layout,
                {tab.x + (tab.w - sz.w) * 0.5f, tab.y + (tab.h - sz.h) * 0.5f},
                ctx.theme.palette.text_primary);
        }
        else if (i < foff + (kBuiltinTabCount - 1))
        {
            // Builtin category tab (indices foff .. foff+kCategories-1).
            // builtin_tab_glyph index: 0=frequents,1..8=categories.
            // Our visual index i maps to glyph index (i - foff + 1).
            tk::TextStyle st{};
            st.role = tk::FontRole::Body;
            auto layout =
                ctx.factory.build_text(builtin_tab_glyph(i - foff + 1), st);
            if (!layout)
            {
                continue;
            }
            tk::Size sz = layout->measure();
            ctx.canvas.draw_text(
                *layout,
                {tab.x + (tab.w - sz.w) * 0.5f, tab.y + (tab.h - sz.h) * 0.5f},
                ctx.theme.palette.text_primary);
        }
        else
        {
            // Custom pack tab: avatar bitmap (when cached) or a fallback
            // single-letter initial. Same treatment as StickerPicker.
            int pack_idx = i - foff - (kBuiltinTabCount - 1);
            const auto& pack = custom_packs_[pack_idx];
            const tk::Image* avatar = nullptr;
            if (provider_ && !pack.avatar_url.empty())
            {
                avatar = provider_(pack.avatar_url, pack.avatar_url);
            }
            if (avatar)
            {
                float side = std::min(tab.h - 6.0f, tab.w - 6.0f);
                ctx.canvas.draw_image(*avatar, {tab.x + (tab.w - side) * 0.5f,
                                                tab.y + (tab.h - side) * 0.5f,
                                                side, side});
            }
            else
            {
                std::string initial =
                    pack.display_name.empty()
                        ? std::string("?")
                        : std::string(1,
                                      std::toupper(static_cast<unsigned char>(
                                          pack.display_name[0])));
                tk::TextStyle st{};
                st.role = tk::FontRole::Body;
                auto layout = ctx.factory.build_text(initial, st);
                if (!layout)
                {
                    continue;
                }
                tk::Size sz = layout->measure();
                ctx.canvas.draw_text(*layout,
                                     {tab.x + (tab.w - sz.w) * 0.5f,
                                      tab.y + (tab.h - sz.h) * 0.5f},
                                     ctx.theme.palette.text_secondary);
            }
        }
    }
    ctx.canvas.pop_clip();
    // Outer border drawn last so nothing (grid fill, tab strip) paints over it.
    ctx.canvas.stroke_rect(bounds_, ctx.theme.palette.popup_border, 1.0f);
}

// ─────────────────────────────────────────────────────────────────────────
//  Input
// ─────────────────────────────────────────────────────────────────────────

tk::Rect EmojiPicker::tab_strip_rect() const
{
    return tab_rect_;
}

int EmojiPicker::tab_at(tk::Point local) const
{
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h)
    {
        return -1;
    }
    int total = total_tab_count();
    if (total == 0)
    {
        return -1;
    }
    float tab_w =
        std::max(kTabSlotMin, tab_rect_.w / static_cast<float>(total));
    int idx = static_cast<int>((lx + tab_scroll_offset_) / tab_w);
    if (idx < 0 || idx >= total)
    {
        return -1;
    }
    return idx;
}

bool EmojiPicker::on_pointer_down(tk::Point local)
{
    int t = tab_at(local);
    if (t >= 0)
    {
        pressed_tab_idx_ = t;
        return true;
    }
    return false;
}

void EmojiPicker::on_pointer_up(tk::Point local, bool inside_self)
{
    if (pressed_tab_idx_ < 0)
    {
        return;
    }
    int t = inside_self ? tab_at(local) : -1;
    int hit = pressed_tab_idx_;
    pressed_tab_idx_ = -1;
    if (t != hit)
    {
        return;
    }
    const int foff = frequents_tab_offset();
    if (has_frequents_tab() && hit == 0)
    {
        switch_to_frequents();
    }
    else if (hit < foff + (kBuiltinTabCount - 1))
    {
        switch_to_category(tesseract::emoji::kCategories[hit - foff]);
    }
    else
    {
        switch_to_custom_pack(hit - foff - (kBuiltinTabCount - 1));
    }
}

bool EmojiPicker::on_pointer_move(tk::Point local)
{
    int cell = -1;
    if (grid_)
    {
        float lx = local.x - grid_rect_.x;
        float ly = local.y - grid_rect_.y;
        if (lx >= 0 && ly >= 0 && lx < grid_rect_.w && ly < grid_rect_.h)
        {
            cell = grid_->index_at({lx, ly});
        }
    }
    if (cell == hovered_grid_cell_)
    {
        return false;
    }
    hovered_grid_cell_ = cell;
    if (grid_)
    {
        grid_->invalidate_data();
    }
    return true;
}

void EmojiPicker::on_pointer_leave()
{
    if (hovered_grid_cell_ == -1)
    {
        return;
    }
    hovered_grid_cell_ = -1;
    if (grid_)
    {
        grid_->invalidate_data();
    }
}

bool EmojiPicker::on_wheel(tk::Point local, float dx, float dy)
{
    // Only handle wheel events that land in the tab strip.
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h)
    {
        return false;
    }

    int total = total_tab_count();
    if (total == 0)
    {
        return false;
    }
    float tab_w =
        std::max(kTabSlotMin, tab_rect_.w / static_cast<float>(total));
    float total_content_w = tab_w * static_cast<float>(total);
    float max_offset = std::max(0.0f, total_content_w - tab_rect_.w);
    if (max_offset == 0.0f)
    {
        return false; // all tabs already visible
    }

    // Use horizontal delta when available; fall back to vertical so a
    // plain mouse wheel still scrolls the tab strip left/right.
    float delta = (dx != 0.0f) ? dx : dy;
    tab_scroll_offset_ += delta;
    if (tab_scroll_offset_ < 0.0f)
    {
        tab_scroll_offset_ = 0.0f;
    }
    if (tab_scroll_offset_ > max_offset)
    {
        tab_scroll_offset_ = max_offset;
    }
    return true; // host repaints on true
}

} // namespace tesseract::views
