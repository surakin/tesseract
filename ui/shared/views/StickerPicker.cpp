#include "StickerPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <algorithm>
#include <cctype>

namespace tesseract::views
{

namespace
{

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
//  StickerPicker
// ─────────────────────────────────────────────────────────────────────────

StickerPicker::~StickerPicker() = default;

StickerPicker::StickerPicker()
{
    rebuild_current_items();
}

void StickerPicker::set_client(tesseract::Client* c)
{
    client_ = c;
    refresh_packs();
}

void StickerPicker::refresh_packs()
{
    reset_tab_scroll();
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
    if (page_ != Page::Search &&
        (active_tab_ < 0 || active_tab_ > tab_count() - 1))
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

// ─────────────────────────────────────────────────────────────────────────
//  Item model
// ─────────────────────────────────────────────────────────────────────────

std::size_t StickerPicker::item_count() const
{
    return current_items_.size();
}

void StickerPicker::paint_cell(std::size_t index, tk::PaintCtx& ctx,
                               tk::Rect bounds, bool selected, bool hovered)
{
    if (index >= current_items_.size())
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

    const auto& entry = current_items_[index];
    const tk::Image* img = nullptr;
    if (image_provider())
    {
        // Encrypted MSC2545 entries carry an opaque source token in
        // `info_json` (the sticker event's MediaSource). Plain entries pass the
        // bare mxc:// url through. The host's fetch_source_bytes is fine with
        // either shape.
        img = image_provider()(entry.url, entry.url);
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
        // Placeholder shimmer: small rounded box, slightly darker than the
        // picker bg. Same idea as message-list image placeholders.
        tk::Rect ph{bounds.x + 8.0f, bounds.y + 8.0f,
                    std::max(0.0f, bounds.w - 16.0f),
                    std::max(0.0f, bounds.h - 16.0f)};
        ctx.canvas.fill_rounded_rect(ph, 6.0f, ctx.theme.palette.chrome_bg);
    }
}

void StickerPicker::on_item_activated(int idx)
{
    if (idx < 0 || static_cast<std::size_t>(idx) >= current_items_.size())
    {
        return;
    }
    if (on_selected)
    {
        on_selected(current_items_[idx]);
    }
}

std::string StickerPicker::cell_tooltip(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= current_items_.size())
    {
        return {};
    }
    const std::string& shortcode = current_items_[index].shortcode;
    if (shortcode.empty())
    {
        return {};
    }
    return ":" + shortcode + ":";
}

// ─────────────────────────────────────────────────────────────────────────
//  Tab model
// ─────────────────────────────────────────────────────────────────────────

int StickerPicker::tab_count() const
{
    return favorites_tab_offset() + static_cast<int>(packs_.size());
}

int StickerPicker::active_tab_index() const
{
    return (page_ == Page::Search) ? -1 : active_tab_;
}

void StickerPicker::paint_tab_content(int i, tk::PaintCtx& ctx, tk::Rect tab)
{
    // Favorites = ⭐ glyph (only when has_favorites_tab()).
    // Pack tabs = avatar bitmap when cached; otherwise first-letter fallback.
    if (has_favorites_tab() && i == 0)
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        auto layout = ctx.factory.build_text(std::string("\xE2\xAD\x90"), st);
        if (layout)
        {
            tk::Size sz = layout->measure();
            ctx.canvas.draw_text(
                *layout,
                {tab.x + (tab.w - sz.w) * 0.5f, tab.y + (tab.h - sz.h) * 0.5f},
                ctx.theme.palette.text_primary);
        }
        return;
    }

    const auto& pack =
        packs_[static_cast<std::size_t>(i - favorites_tab_offset())];
    const tk::Image* avatar = nullptr;
    if (image_provider() && !pack.avatar_url.empty())
    {
        avatar = image_provider()(pack.avatar_url, pack.avatar_url);
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
            ctx.canvas.draw_text(
                *layout,
                {tab.x + (tab.w - sz.w) * 0.5f, tab.y + (tab.h - sz.h) * 0.5f},
                ctx.theme.palette.text_secondary);
        }
    }
}

void StickerPicker::on_tab_clicked(int hit)
{
    if (has_favorites_tab() && hit == 0)
    {
        switch_to_favorites();
    }
    else
    {
        switch_to_pack(hit - favorites_tab_offset());
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Search + page switching
// ─────────────────────────────────────────────────────────────────────────

void StickerPicker::on_search_query_changed(const std::string& /*query*/,
                                            bool cleared)
{
    if (cleared)
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

void StickerPicker::rebuild_current_items()
{
    current_items_.clear();
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
        // refresh_packs(); but individual images can still be emoticon-only via
        // per-image usage override, so filter again here.
        const auto& pack = packs_[pack_idx];
        for (const auto& img :
             (client_ ? client_->list_pack_images(pack.id,
                                                  PackUsageFilter::Sticker)
                      : std::vector<ImagePackImage>{}))
        {
            current_items_.push_back(img);
        }
        break;
    }
    case Page::Search:
    {
        // Cross-pack search across every entry whose body or shortcode matches
        // the query (case-insensitive). Favorites are already included via
        // their pack of origin.
        if (client_)
        {
            const std::string& query = search_query();
            for (const auto& pack : packs_)
            {
                for (const auto& img : client_->list_pack_images(
                         pack.id, PackUsageFilter::Sticker))
                {
                    if (icontains(img.body, query) ||
                        icontains(img.shortcode, query))
                    {
                        current_items_.push_back(img);
                    }
                }
            }
        }
        break;
    }
    }
    refresh_grid();
}

} // namespace tesseract::views
