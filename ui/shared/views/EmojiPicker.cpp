#include "EmojiPicker.h"

#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/image_pack_order.h"

#include <tesseract/client.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace tesseract::views
{

namespace
{

// Number of Unicode category tabs (kCategories).
constexpr int kCategoryCount =
    static_cast<int>(sizeof(tesseract::emoji::kCategories) /
                     sizeof(tesseract::emoji::kCategories[0]));

const char* frequents_glyph()
{
    return "\xE2\xAD\x90"; // ⭐ Frequents indicator
}

// Extract the first shortcode from a space-delimited list and wrap it in
// colons — e.g. "thumbs_up thumbsup" → ":thumbs_up:". Returns "" when empty.
std::string format_shortcode(std::string_view shortcodes)
{
    if (shortcodes.empty())
    {
        return {};
    }
    auto pos = shortcodes.find(' ');
    auto tok =
        (pos == std::string_view::npos) ? shortcodes : shortcodes.substr(0, pos);
    return ":" + std::string(tok) + ":";
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  EmojiPicker
// ─────────────────────────────────────────────────────────────────────────

EmojiPicker::~EmojiPicker() = default;

EmojiPicker::EmojiPicker()
{
    set_search_placeholder(tk::tr("Search emoji"));
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
        std::vector<tesseract::ImagePack> filtered;
        for (auto& p : client_->list_image_packs())
        {
            if (any(p.usage & tesseract::PackUsage::Emoticon))
            {
                // Packs without their own avatar borrow the first emoji's
                // image so the tab shows a glyph rather than a bare letter.
                if (p.avatar_url.empty())
                {
                    auto imgs = client_->list_pack_images(
                        p.id, tesseract::PackUsageFilter::Emoticon);
                    if (!imgs.empty())
                        p.avatar_url = imgs.front().url;
                }
                filtered.push_back(std::move(p));
            }
        }
        custom_packs_ = order_picker_packs(std::move(filtered), current_room_id_,
                                           current_room_parent_spaces_);
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

// ─────────────────────────────────────────────────────────────────────────
//  Item model
// ─────────────────────────────────────────────────────────────────────────

std::size_t EmojiPicker::item_count() const
{
    if (page_ == Page::CustomPack)
    {
        return current_emoticons_.size();
    }
    return current_glyphs_.size();
}

void EmojiPicker::paint_cell(std::size_t index, tk::PaintCtx& ctx,
                             tk::Rect bounds, bool selected, bool hovered)
{
    bool is_image_page = (page_ == Page::CustomPack);
    if (is_image_page)
    {
        if (index >= current_emoticons_.size())
        {
            return;
        }
    }
    else
    {
        if (index >= current_glyphs_.size())
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
        const auto& entry = current_emoticons_[index];
        const tk::Image* img = nullptr;
        if (image_provider())
        {
            img = image_provider()(entry.url, entry.url);
        }
        if (img)
        {
            float iw = static_cast<float>(img->width());
            float ih = static_cast<float>(img->height());
            float s = std::min(bounds.w / iw, bounds.h / ih);
            float dw = iw * s;
            float dh = ih * s;
            ctx.canvas.draw_image(*img, {bounds.x + (bounds.w - dw) * 0.5f,
                                         bounds.y + (bounds.h - dh) * 0.5f, dw,
                                         dh});
        }
        else
        {
            // Placeholder until the host loads the bitmap.
            tk::Rect ph{bounds.x + 4.0f, bounds.y + 4.0f,
                        std::max(0.0f, bounds.w - 8.0f),
                        std::max(0.0f, bounds.h - 8.0f)};
            ctx.canvas.fill_rounded_rect(ph, 4.0f, ctx.theme.palette.chrome_bg);
        }
        return;
    }

    const std::string& glyph = current_glyphs_[index];

    tk::TextStyle st{};
    st.role = tk::FontRole::EmojiPickerCell;
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

void EmojiPicker::on_item_activated(int idx)
{
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
}

std::string EmojiPicker::cell_tooltip(int index) const
{
    if (index < 0 ||
        static_cast<std::size_t>(index) >= current_shortcodes_.size())
    {
        return {};
    }
    return current_shortcodes_[index];
}

// ─────────────────────────────────────────────────────────────────────────
//  Tab model
// ─────────────────────────────────────────────────────────────────────────

int EmojiPicker::tab_count() const
{
    return frequents_tab_offset() + kCategoryCount +
           static_cast<int>(custom_packs_.size());
}

int EmojiPicker::active_tab_index() const
{
    const int foff = frequents_tab_offset();
    switch (page_)
    {
    case Page::Frequents:
        return has_frequents_tab() ? 0 : -1;
    case Page::Category:
        return foff + static_cast<int>(category_);
    case Page::CustomPack:
        return foff + kCategoryCount + custom_pack_idx_;
    default:
        return -1;
    }
}

void EmojiPicker::paint_tab_content(int i, tk::PaintCtx& ctx, tk::Rect tab)
{
    const int foff = frequents_tab_offset();

    if (has_frequents_tab() && i == 0)
    {
        // Frequents tab — the star indicator.
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        auto layout = ctx.factory.build_text(frequents_glyph(), st);
        if (!layout)
        {
            return;
        }
        tk::Size sz = layout->measure();
        ctx.canvas.draw_text(
            *layout,
            {tab.x + (tab.w - sz.w) * 0.5f, tab.y + (tab.h - sz.h) * 0.5f},
            ctx.theme.palette.text_primary);
    }
    else if (i < foff + kCategoryCount)
    {
        // Builtin category tab (indices foff .. foff+kCategoryCount-1).
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        auto layout = ctx.factory.build_text(
            tesseract::emoji::category_tab_glyph(
                tesseract::emoji::kCategories[i - foff]),
            st);
        if (!layout)
        {
            return;
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
        int pack_idx = i - foff - kCategoryCount;
        const auto& pack = custom_packs_[pack_idx];
        const tk::Image* avatar = nullptr;
        if (image_provider() && !pack.avatar_url.empty())
        {
            avatar = image_provider()(pack.avatar_url, pack.avatar_url);
        }
        if (avatar)
        {
            float side = std::min(tab.h - 6.0f, tab.w - 6.0f);
            ctx.canvas.draw_image(*avatar, {tab.x + (tab.w - side) * 0.5f,
                                            tab.y + (tab.h - side) * 0.5f, side,
                                            side});
        }
        else
        {
            std::string initial =
                pack.display_name.empty()
                    ? std::string("?")
                    : std::string(1, std::toupper(static_cast<unsigned char>(
                                         pack.display_name[0])));
            tk::TextStyle st{};
            st.role = tk::FontRole::Body;
            auto layout = ctx.factory.build_text(initial, st);
            if (!layout)
            {
                return;
            }
            tk::Size sz = layout->measure();
            ctx.canvas.draw_text(
                *layout,
                {tab.x + (tab.w - sz.w) * 0.5f, tab.y + (tab.h - sz.h) * 0.5f},
                ctx.theme.palette.text_secondary);
        }
    }
}

void EmojiPicker::on_tab_clicked(int hit)
{
    const int foff = frequents_tab_offset();
    if (has_frequents_tab() && hit == 0)
    {
        switch_to_frequents();
    }
    else if (hit < foff + kCategoryCount)
    {
        switch_to_category(tesseract::emoji::kCategories[hit - foff]);
    }
    else
    {
        switch_to_custom_pack(hit - foff - kCategoryCount);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Search + page switching
// ─────────────────────────────────────────────────────────────────────────

void EmojiPicker::on_search_query_changed(const std::string& /*query*/,
                                          bool cleared)
{
    if (cleared)
    {
        // Returning to the previously active page (Frequents or Category) when
        // the user clears the search keeps the picker navigable; the most
        // useful default is the category that was showing before they typed.
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
    switch (page_)
    {
    case Page::Frequents:
    {
        current_glyphs_ = frequents_glyphs_;
        // Look up each frequent glyph in the emoji table to get its canonical
        // shortcode.
        const auto& table = tesseract::emoji::all();
        for (const auto& glyph : current_glyphs_)
        {
            std::string sc;
            for (const auto& e : table)
            {
                if (e.glyph == glyph && !e.shortcodes.empty())
                {
                    sc = format_shortcode(e.shortcodes);
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
            current_shortcodes_.push_back(format_shortcode(e->shortcodes));
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
            // Per-image usage filter pulls only emoticon-capable entries —
            // packs that allow both usages will appear in both the
            // StickerPicker and here, but each cell is independently filtered.
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
        // Unicode glyph search. Custom emoticon search would need its own tab;
        // out of scope for this PR.
        auto entries = tesseract::emoji::filter(search_query());
        current_glyphs_.reserve(entries.size());
        current_shortcodes_.reserve(entries.size());
        for (const auto* e : entries)
        {
            current_glyphs_.emplace_back(e->glyph);
            current_shortcodes_.push_back(format_shortcode(e->shortcodes));
        }
        break;
    }
    }
    refresh_grid();
}

} // namespace tesseract::views
