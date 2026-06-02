#include "StickerPicker.h"

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
constexpr float kTabHeight = 40.0f; // taller than emoji tabs to host avatars
constexpr float kCellSize = 96.0f;
constexpr float kCellGap = 6.0f;
constexpr float kTabSlotMin = 40.0f;

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
//  Grid adapter — paints each sticker thumbnail letterboxed in its cell.
// ─────────────────────────────────────────────────────────────────────────

class StickerPicker::GridAdapter : public tk::GridAdapter
{
public:
    explicit GridAdapter(StickerPicker& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.current_items_.size();
    }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override
    {
        if (index >= owner_.current_items_.size())
        {
            return;
        }

        if (selected)
        {
            ctx.canvas.fill_rounded_rect(bounds, 8.0f,
                                         ctx.theme.palette.subtle_pressed);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rounded_rect(bounds, 8.0f,
                                         ctx.theme.palette.subtle_hover);
        }

        const auto& entry = owner_.current_items_[index];
        const tk::Image* img = nullptr;
        if (owner_.provider_)
        {
            // Encrypted MSC2545 entries carry an opaque source token in
            // `info_json` (the sticker event's MediaSource). Plain entries
            // pass the bare mxc:// url through. The host's
            // fetch_source_bytes is fine with either shape.
            img = owner_.provider_(entry.url, entry.url);
        }
        if (img)
        {
            // Letterbox the bitmap inside the cell, preserving aspect.
            float iw = static_cast<float>(img->width());
            float ih = static_cast<float>(img->height());
            float s = std::min(bounds.w / iw, bounds.h / ih);
            float dw = iw * s;
            float dh = ih * s;
            tk::Rect dst{bounds.x + (bounds.w - dw) * 0.5f,
                         bounds.y + (bounds.h - dh) * 0.5f, dw, dh};
            ctx.canvas.draw_image(*img, dst);
        }
        else
        {
            // Placeholder shimmer: small rounded box, slightly darker than
            // the picker bg. Same idea as message-list image placeholders.
            tk::Rect ph{bounds.x + 8.0f, bounds.y + 8.0f,
                        std::max(0.0f, bounds.w - 16.0f),
                        std::max(0.0f, bounds.h - 16.0f)};
            ctx.canvas.fill_rounded_rect(ph, 6.0f, ctx.theme.palette.chrome_bg);
        }
    }

private:
    StickerPicker& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  StickerPicker
// ─────────────────────────────────────────────────────────────────────────

StickerPicker::~StickerPicker() = default;

StickerPicker::StickerPicker()
    : grid_adapter_(std::make_unique<GridAdapter>(*this))
{
    auto grid = std::make_unique<tk::GridView>();
    grid->set_cell_size(kCellSize, kCellSize);
    grid->set_spacing(kCellGap, kCellGap);
    grid->set_padding(tk::Edges::all(kPadding));
    grid->set_adapter(grid_adapter_.get());
    grid->on_cell_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= current_items_.size())
        {
            return;
        }
        if (on_selected)
        {
            on_selected(current_items_[idx]);
        }
    };
    grid_ = add_child(std::move(grid));

    rebuild_current_items();
}

void StickerPicker::set_client(tesseract::Client* c)
{
    client_ = c;
    refresh_packs();
}

void StickerPicker::set_image_provider(ImageProvider p)
{
    provider_ = std::move(p);
    invalidate_image_cache();
}

void StickerPicker::invalidate_image_cache()
{
    if (grid_)
    {
        grid_->invalidate_data();
    }
}

void StickerPicker::refresh_packs()
{
    tab_scroll_offset_ = 0.0f;
    packs_.clear();
    favorites_.clear();
    if (client_)
    {
        // Keep only packs whose usage allows stickers (per MSC2545; missing
        // usage means any, which the SDK already normalised to PackUsage::Any).
        for (auto& p : client_->list_image_packs())
        {
            if (any(p.usage & PackUsage::Sticker))
            {
                packs_.push_back(std::move(p));
            }
        }
        favorites_ = client_->list_favorite_stickers();
    }
    // If we were on Favorites and there are no longer any, fall back to pack 0.
    if (page_ == Page::Favorites && favorites_.empty())
    {
        if (!packs_.empty())
        {
            page_ = Page::Pack;
            active_tab_ =
                favorites_tab_offset(); // == 0 now that favorites gone
        }
        else
        {
            active_tab_ = 0;
        }
    }
    // Reclamp active tab so it points at a valid index.
    // Skip the clamp on the Search page — active_tab_ is intentionally -1
    // there and the search results should survive a pack reload.
    if (page_ != Page::Search && (active_tab_ < 0 || active_tab_ > tab_count() - 1))
    {
        if (!packs_.empty())
        {
            page_ = Page::Pack;
            active_tab_ = favorites_tab_offset();
        }
        else
        {
            active_tab_ = 0;
            page_ = Page::Favorites;
        }
    }
    rebuild_current_items();
}

void StickerPicker::set_search_query(std::string query)
{
    query_ = std::move(query);
    if (query_.empty())
    {
        if (page_ == Page::Search)
        {
            if (!favorites_.empty())
            {
                switch_to_favorites();
            }
            else if (!packs_.empty())
            {
                switch_to_pack(0);
            }
            else
            {
                rebuild_current_items();
            }
        }
        return;
    }
    switch_to_search();
}

void StickerPicker::switch_to_favorites()
{
    page_ = Page::Favorites;
    active_tab_ = 0;
    rebuild_current_items();
}

void StickerPicker::switch_to_pack(int idx)
{
    if (idx < 0 || static_cast<std::size_t>(idx) >= packs_.size())
    {
        return;
    }
    page_ = Page::Pack;
    active_tab_ = favorites_tab_offset() + idx;
    rebuild_current_items();
}

void StickerPicker::switch_to_search()
{
    page_ = Page::Search;
    active_tab_ = -1;
    rebuild_current_items();
}

int StickerPicker::tab_count() const
{
    return favorites_tab_offset() + static_cast<int>(packs_.size());
}

void StickerPicker::rebuild_current_items()
{
    current_items_.clear();
    hovered_grid_cell_ = -1;
    switch (page_)
    {
    case Page::Favorites:
        current_items_ = favorites_;
        break;
    case Page::Pack:
    {
        int pack_idx = active_tab_ - favorites_tab_offset();
        if (pack_idx < 0 || static_cast<std::size_t>(pack_idx) >= packs_.size())
        {
            break;
        }
        // We already filtered the pack list to sticker-capable packs in
        // refresh_packs(); but individual images can still be emoticon-
        // only via per-image usage override, so filter again here.
        const auto& pack = packs_[pack_idx];
        for (const auto& img : (client_ ? client_->list_pack_images(
                                              pack.id, PackUsageFilter::Sticker)
                                        : std::vector<ImagePackImage>{}))
        {
            current_items_.push_back(img);
        }
        break;
    }
    case Page::Search:
    {
        // Cross-pack search across every entry whose body or shortcode
        // matches the query (case-insensitive). Favorites are already
        // included via their pack of origin.
        if (client_)
        {
            for (const auto& pack : packs_)
            {
                for (const auto& img : client_->list_pack_images(
                         pack.id, PackUsageFilter::Sticker))
                {
                    if (icontains(img.body, query_) ||
                        icontains(img.shortcode, query_))
                    {
                        current_items_.push_back(img);
                    }
                }
            }
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

tk::Size StickerPicker::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void StickerPicker::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    search_rect_ = {bounds.x + kPadding, bounds.y + kPadding,
                    std::max(0.0f, bounds.w - kPadding * 2), kSearchHeight};

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

void StickerPicker::paint(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Search affordance behind the host's NativeTextField overlay.
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
        static_cast<std::size_t>(hovered_grid_cell_) < current_items_.size())
    {
        const std::string& shortcode =
            current_items_[hovered_grid_cell_].shortcode;
        if (!shortcode.empty())
        {
            std::string sc = ":" + shortcode + ":";
            tk::TextStyle small_style{};
            small_style.role = tk::FontRole::Small;
            auto layout = ctx.factory.build_text(sc, small_style);
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

    // ─── Tab strip ──────────────────────────────────────────────────────
    ctx.canvas.fill_rect(tab_rect_, ctx.theme.palette.chrome_bg);
    ctx.canvas.fill_rect({tab_rect_.x, tab_rect_.y, tab_rect_.w, 1.0f},
                         ctx.theme.palette.separator);

    int total_tabs = tab_count();
    if (total_tabs == 0)
    {
        return;
    }
    float tab_w =
        std::max(kTabSlotMin, tab_rect_.w / static_cast<float>(total_tabs));
    int active = (page_ == Page::Search) ? -1 : active_tab_;
    ctx.canvas.push_clip_rect(tab_rect_);
    for (int i = 0; i < total_tabs; ++i)
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

        // Tab content. Favorites = ⭐ glyph (only when has_favorites_tab()).
        // Pack tabs = avatar bitmap when cached; otherwise first-letter fallback.
        if (has_favorites_tab() && i == 0)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Title;
            auto layout =
                ctx.factory.build_text(std::string("\xE2\xAD\x90"), st);
            if (layout)
            {
                tk::Size sz = layout->measure();
                ctx.canvas.draw_text(*layout,
                                     {tab.x + (tab.w - sz.w) * 0.5f,
                                      tab.y + (tab.h - sz.h) * 0.5f},
                                     ctx.theme.palette.text_primary);
            }
            continue;
        }

        const auto& pack =
            packs_[static_cast<std::size_t>(i - favorites_tab_offset())];
        const tk::Image* avatar = nullptr;
        if (provider_ && !pack.avatar_url.empty())
        {
            avatar = provider_(pack.avatar_url, pack.avatar_url);
        }
        if (avatar)
        {
            float side = std::min(tab.h - 8.0f, tab.w - 8.0f);
            tk::Rect dst{tab.x + (tab.w - side) * 0.5f,
                         tab.y + (tab.h - side) * 0.5f, side, side};
            ctx.canvas.draw_image(*avatar, dst);
        }
        else
        {
            std::string initial =
                pack.display_name.empty()
                    ? std::string("?")
                    : std::string(1, std::toupper(static_cast<unsigned char>(
                                         pack.display_name[0])));
            tk::TextStyle st{};
            st.role = tk::FontRole::Title;
            auto layout = ctx.factory.build_text(initial, st);
            if (layout)
            {
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

tk::Rect StickerPicker::tab_strip_rect() const
{
    return tab_rect_;
}

int StickerPicker::tab_at(tk::Point local) const
{
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h)
    {
        return -1;
    }
    int total = tab_count();
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

bool StickerPicker::on_pointer_down(tk::Point local)
{
    int t = tab_at(local);
    if (t >= 0)
    {
        pressed_tab_idx_ = t;
        return true;
    }
    return false;
}

void StickerPicker::on_pointer_up(tk::Point local, bool inside_self)
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
    if (has_favorites_tab() && hit == 0)
    {
        switch_to_favorites();
    }
    else
    {
        switch_to_pack(hit - favorites_tab_offset());
    }
}

bool StickerPicker::on_pointer_move(tk::Point local)
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

void StickerPicker::on_pointer_leave()
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

bool StickerPicker::on_wheel(tk::Point local, float dx, float dy)
{
    float lx = local.x - (tab_rect_.x - bounds_.x);
    float ly = local.y - (tab_rect_.y - bounds_.y);
    if (lx < 0 || ly < 0 || lx >= tab_rect_.w || ly >= tab_rect_.h)
    {
        return false;
    }
    int total = tab_count();
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
        return false;
    }
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
    return true;
}

} // namespace tesseract::views
