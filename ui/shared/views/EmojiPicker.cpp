#include "EmojiPicker.h"

#include "SkinTonePopover.h"
#include "tk/emoji_segmentation.h"
#include "tk/emoji_support.h"
#include "tk/i18n.h"
#include "tk/text_util.h"
#include "tk/theme.h"
#include "views/image_pack_order.h"

#include <tesseract/client.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>

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

std::vector<tesseract::ImagePackImage> match_pack_emoticons(
    const std::vector<std::vector<tesseract::ImagePackImage>>& pack_images,
    const std::string& query)
{
    std::vector<tesseract::ImagePackImage> out;
    std::unordered_set<std::string> seen_urls;
    for (const auto& images : pack_images)
    {
        for (const auto& img : images)
        {
            if (!tk::ci_contains(img.shortcode, query) &&
                !tk::ci_contains(img.body, query))
                continue;
            if (!img.url.empty() && !seen_urls.insert(img.url).second)
                continue;
            out.push_back(img);
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
//  EmojiPicker
// ─────────────────────────────────────────────────────────────────────────

EmojiPicker::~EmojiPicker() = default;

EmojiPicker::EmojiPicker()
{
    set_search_placeholder(tk::tr("Search emoji"));
    // Added last so it sits above the grid for input as well as paint.
    auto popover = tk::create_widget<SkinTonePopover>(this);
    popover->on_picked = [this](tesseract::emoji::SkinTone tone)
    {
        const std::string glyph = tone_popover_->glyph_for(tone);
        set_skin_tone(tone);
        if (on_skin_tone_changed)
            on_skin_tone_changed(tone);
        activate_glyph_(glyph);
    };
    tone_popover_ = add_child(std::move(popover));
    rebuild_current_items();
}

void EmojiPicker::set_skin_tone(tesseract::emoji::SkinTone tone)
{
    if (tone == skin_tone_)
        return;
    skin_tone_ = tone;
    // Unicode pages only — a custom pack's images have no tones, and
    // rebuilding would needlessly reset its grid.
    if (page_ != Page::CustomPack)
        rebuild_current_items();
}

void EmojiPicker::set_visible(bool v)
{
    if (!v && tone_popover_)
        tone_popover_->close();
    TabbedGridPicker::set_visible(v);
}

void EmojiPicker::paint(tk::PaintCtx& ctx)
{
    TabbedGridPicker::paint(ctx);
    if (tone_popover_ && tone_popover_->is_open())
        tone_popover_->paint(ctx);
}

bool EmojiPicker::on_key_down(const tk::KeyEvent& e)
{
    if (tone_popover_ && tone_popover_->handle_key(e))
        return true;
    return TabbedGridPicker::on_key_down(e);
}

void EmojiPicker::on_popup_dismiss()
{
    if (tone_popover_)
        tone_popover_->close();
    TabbedGridPicker::on_popup_dismiss();
}

bool EmojiPicker::close_tone_popover_if_outside_(tk::Point world)
{
    if (!tone_popover_ || !tone_popover_->is_open() ||
        tone_popover_->contains_world(world))
    {
        return false;
    }
    tone_popover_->close();
    return true;
}

tk::Widget* EmojiPicker::dispatch_pointer_down(tk::Point world)
{
    if (close_tone_popover_if_outside_(world))
        return this;
    return TabbedGridPicker::dispatch_pointer_down(world);
}

tk::Widget* EmojiPicker::dispatch_right_click(tk::Point world)
{
    if (close_tone_popover_if_outside_(world))
        return this;
    return TabbedGridPicker::dispatch_right_click(world);
}

bool EmojiPicker::dispatch_wheel(tk::Point world, float dx, float dy,
                                 bool is_touchpad)
{
    // Scrolling would move the cell out from under the menu.
    if (tone_popover_ && tone_popover_->is_open())
    {
        tone_popover_->close();
        return true;
    }
    return TabbedGridPicker::dispatch_wheel(world, dx, dy, is_touchpad);
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
        // Reactions can be arbitrary text (bots send "👍 ok"); those stay in
        // the synced list but aren't offered here. Over-fetch so filtered
        // entries don't shrink the grid.
        for (auto& glyph : client_->recent_emoji_top(64))
        {
            if (!glyph.empty() && tk::is_emoji_only(glyph) &&
                std::none_of(glyph.begin(), glyph.end(),
                             [](unsigned char c) { return std::isspace(c); }))
            {
                frequents_glyphs_.push_back(std::move(glyph));
            }
        }
        if (frequents_glyphs_.size() > 32)
            frequents_glyphs_.resize(32);
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

void EmojiPicker::show_frequents_page()
{
    if (has_frequents_tab())
    {
        switch_to_frequents();
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
    else if (page_ == Page::CustomPack || page_ == Page::Search)
    {
        // Search results include pack emoticons too.
        rebuild_current_items();
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  Item model
// ─────────────────────────────────────────────────────────────────────────

std::size_t EmojiPicker::item_count() const
{
    return current_glyphs_.size() + current_emoticons_.size();
}

const std::string* EmojiPicker::glyph_at_(std::size_t index) const
{
    return index < current_glyphs_.size() ? &current_glyphs_[index] : nullptr;
}

const tesseract::ImagePackImage* EmojiPicker::emoticon_at_(std::size_t index) const
{
    // Glyphs (if any) come first; current_glyphs_ is empty on CustomPack.
    if (index < current_glyphs_.size())
        return nullptr;
    index -= current_glyphs_.size();
    return index < current_emoticons_.size() ? &current_emoticons_[index]
                                             : nullptr;
}

std::vector<tk::MediaPrefetchKey> EmojiPicker::collect_prefetchable_media_keys() const
{
    std::vector<tk::MediaPrefetchKey> keys;
    if (current_emoticons_.empty())
    {
        return keys; // Unicode/Frequents pages have no image cells at all.
    }
    if (!grid_)
    {
        return keys;
    }
    // Scoped to the viewport + a small lookahead margin — see
    // StickerPicker::collect_prefetchable_media_keys()'s identical pattern
    // and rationale.
    constexpr int kPrefetchLookaheadCells = 8;
    const auto [lo, hi] = tk::grid_prefetch_range(
        *grid_, item_count(), kPrefetchLookaheadCells);
    if (hi < lo)
    {
        return keys;
    }
    keys.reserve(static_cast<std::size_t>(hi - lo + 1));
    for (int i = lo; i <= hi; ++i)
    {
        const auto* entry = emoticon_at_(static_cast<std::size_t>(i));
        if (entry && !entry->url.empty())
        {
            keys.push_back({tk::CacheKey::media(entry->url), tk::MediaKind::MediaImage});
        }
    }
    return keys;
}

void EmojiPicker::paint_cell(std::size_t index, tk::PaintCtx& ctx,
                             tk::Rect bounds, bool selected, bool hovered)
{
    const std::string* glyph = glyph_at_(index);
    const tesseract::ImagePackImage* emoticon = emoticon_at_(index);
    if (!glyph && !emoticon)
    {
        return;
    }

    if (selected)
    {
        // High Contrast: subtle_pressed/hover equal the window colour.
        ctx.canvas.fill_rounded_rect(bounds, 4.0f,
                                     ctx.theme.high_contrast
                                         ? ctx.theme.palette.sidebar_selected
                                         : ctx.theme.palette.subtle_pressed);
    }
    else if (hovered)
    {
        ctx.canvas.fill_rounded_rect(bounds, 4.0f,
                                     tk::hot_row_fill(ctx.theme));
    }

    if (emoticon)
    {
        const auto& entry = *emoticon;
        const tk::Image* img = nullptr;
        if (image_provider())
        {
            img = image_provider()(entry.url, entry.url, hovered);
        }
        if (img)
        {
            float iw = static_cast<float>(img->width());
            float ih = static_cast<float>(img->height());
            float s = std::min(bounds.w / iw, bounds.h / ih);
            float dw = iw * s;
            float dh = ih * s;
            tk::Rect dst{bounds.x + (bounds.w - dw) * 0.5f,
                        bounds.y + (bounds.h - dh) * 0.5f, dw, dh};
            ctx.canvas.draw_image(*img, dst);
            if (ctx.anim_damage)
            {
                ctx.anim_damage->note_image(tk::CacheKey::media(entry.url), dst);
            }
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

    tk::TextStyle st{};
    st.role = tk::FontRole::EmojiPickerCell;
    // build_glyph, not build_text: this cell's entire content is one glyph,
    // so it should render at EmojiPickerCell's own size rather than being
    // silently downgraded to FontRole::InlineEmoji by build_text's emoji-run
    // segmentation (see CanvasFactory::build_glyph's doc comment) — that
    // mismatch was also why the hand-centering below still drifted on Qt6
    // even after the top-left-vs-centered fix.
    auto layout = ctx.factory.build_glyph(*glyph, st);
    if (!layout)
    {
        return;
    }
    // Centered by hand from the layout's own unconstrained measure() rather
    // than via TextStyle::halign/valign — see UserInfo.cpp's status-line
    // comment for why: backends disagree on honoring an alignment request
    // inside a max_width/max_height box.
    tk::Size sz = layout->measure();
    ctx.canvas.draw_text(
        *layout,
        {bounds.x + (bounds.w - sz.w) * 0.5f, bounds.y + (bounds.h - sz.h) * 0.5f},
        ctx.theme.palette.text_primary);
}

void EmojiPicker::on_item_activated(int idx)
{
    if (idx < 0)
    {
        return;
    }
    const auto index = static_cast<std::size_t>(idx);
    if (const auto* emoticon = emoticon_at_(index))
    {
        if (on_emoticon_selected)
        {
            on_emoticon_selected(*emoticon);
        }
        return;
    }
    if (const auto* glyph = glyph_at_(index))
    {
        activate_glyph_(*glyph);
    }
}

void EmojiPicker::activate_glyph_(const std::string& glyph)
{
    if (client_)
    {
        client_->recent_emoji_bump(glyph);
    }
    if (on_selected)
    {
        on_selected(glyph);
    }
}

bool EmojiPicker::on_item_context_requested(int idx, tk::Rect cell)
{
    const std::string* glyph_ptr =
        idx < 0 ? nullptr : glyph_at_(static_cast<std::size_t>(idx));
    if (!tone_popover_ || !glyph_ptr)
    {
        return false;
    }
    const std::string& glyph = *glyph_ptr;
    // Only when the font can draw every tone — a menu with a box in it
    // would let the user send something they can't see.
    if (!tesseract::emoji::supports_skin_tone(glyph) ||
        !tk::all_skin_tones_supported(glyph, supported_emoji_version_()))
    {
        return false;
    }
    // Clear the grid's hover + tooltip under the menu.
    if (grid_)
    {
        grid_->on_pointer_leave();
        if (host())
            host()->hide_tooltip(grid_);
    }
    tone_popover_->open(glyph, skin_tone_, cell, bounds_);
    return true;
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
            // Pack-avatar tab icon: not animated, always play.
            avatar = image_provider()(pack.avatar_url, pack.avatar_url, true);
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
            // Initials, not the first byte: a name in another script starts with a
            // multi-byte character, which one byte would cut in half.
            std::string initial = tk::initials_of(pack.display_name);
            if (initial.empty())
                initial = "?";
            for (char& ch : initial)
                if (ch >= 'a' && ch <= 'z')
                    ch = static_cast<char>(ch - 'a' + 'A');
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

std::string EmojiPicker::tab_label(int i) const
{
    const int foff = frequents_tab_offset();
    if (has_frequents_tab() && i == 0)
    {
        return tk::tr("Frequently Used");
    }
    if (i < foff + kCategoryCount)
    {
        return tk::tr(tesseract::emoji::category_name(
            tesseract::emoji::kCategories[i - foff]));
    }
    int pack_idx = i - foff - kCategoryCount;
    if (pack_idx < 0 || static_cast<std::size_t>(pack_idx) >= custom_packs_.size())
    {
        return {};
    }
    return custom_packs_[pack_idx].display_name; // user data, not tr()'d
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

std::uint16_t EmojiPicker::supported_emoji_version_() const
{
    return host() ? host()->supported_emoji_version() : tk::kAllEmojiVersions;
}

void EmojiPicker::rebuild_current_items()
{
    current_glyphs_.clear();
    current_emoticons_.clear();
    current_shortcodes_.clear();
    // Glyphs newer than the platform's emoji font are never offered — see
    // tk/emoji_support.h.
    const std::uint16_t max_version = supported_emoji_version_();
    switch (page_)
    {
    case Page::Frequents:
    {
        // Entries used untoned follow the default tone; ones used with an
        // explicit tone stay as used (or drop to the base when the font is
        // too old for that tone). Toning can make two entries equal
        // (👍 and 👍🏽), so keep the first.
        std::unordered_set<std::string> seen;
        for (const auto& glyph : frequents_glyphs_)
        {
            const bool untoned = tesseract::emoji::base_glyph(glyph) == glyph;
            auto shown = tk::offered_emoji(
                glyph,
                untoned ? skin_tone_ : tesseract::emoji::SkinTone::None,
                max_version);
            if (!untoned && tesseract::emoji::emoji_version(glyph) <= max_version)
                shown = glyph;
            if (shown && seen.insert(std::string(*shown)).second)
                current_glyphs_.emplace_back(*shown);
        }
        // Look up each frequent glyph in the emoji table to get its canonical
        // shortcode (via its base, since the table holds untoned glyphs).
        const auto& table = tesseract::emoji::all();
        for (const auto& glyph : current_glyphs_)
        {
            const auto base = tesseract::emoji::base_glyph(glyph);
            std::string sc;
            for (const auto& e : table)
            {
                if (e.glyph == base && !e.shortcodes.empty())
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
            auto shown = tk::offered_emoji(e->glyph, skin_tone_, max_version);
            if (!shown)
                continue;
            current_glyphs_.emplace_back(*shown);
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
        // Unicode matches first, then custom emoticons from every pack
        // (in tab order) — same cells as the CustomPack page.
        auto entries = tesseract::emoji::filter(search_query());
        current_glyphs_.reserve(entries.size());
        current_shortcodes_.reserve(entries.size());
        for (const auto* e : entries)
        {
            auto shown = tk::offered_emoji(e->glyph, skin_tone_, max_version);
            if (!shown)
                continue;
            current_glyphs_.emplace_back(*shown);
            current_shortcodes_.push_back(format_shortcode(e->shortcodes));
        }
        if (client_ && !custom_packs_.empty())
        {
            std::vector<std::vector<tesseract::ImagePackImage>> pack_images;
            pack_images.reserve(custom_packs_.size());
            for (const auto& pack : custom_packs_)
            {
                pack_images.push_back(client_->list_pack_images(
                    pack.id, tesseract::PackUsageFilter::Emoticon));
            }
            for (auto& img : match_pack_emoticons(pack_images, search_query()))
            {
                current_shortcodes_.push_back(":" + img.shortcode + ":");
                current_emoticons_.push_back(std::move(img));
            }
        }
        break;
    }
    }
    refresh_grid();
}

} // namespace tesseract::views
