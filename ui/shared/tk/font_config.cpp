#include "tk/font_config.h"

#include "tk/canvas.h"

#include <algorithm>

namespace tk
{

namespace
{

FontConfig g_config;
std::uint64_t g_generation = 0;

constexpr std::size_t idx(FontCategory c)
{
    return static_cast<std::size_t>(c);
}

} // namespace

bool FontConfig::is_default() const
{
    return *this == FontConfig{};
}

const FontConfig& font_config()
{
    return g_config;
}

void set_font_config(FontConfig cfg)
{
    if (cfg.base_pt != 0)
    {
        cfg.base_pt = std::clamp(cfg.base_pt, kFontBaseMinPt, kFontBaseMaxPt);
    }
    for (int& d : cfg.delta_pt)
    {
        d = std::clamp(d, kFontDeltaMinPt, kFontDeltaMaxPt);
    }
    cfg.family[idx(FontCategory::Emoji)].clear();
    if (cfg == g_config)
    {
        return;
    }
    g_config = std::move(cfg);
    ++g_generation;
}

std::uint64_t font_config_generation()
{
    return g_generation;
}

FontCategory font_category(FontRole role)
{
    switch (role)
    {
    case FontRole::Body:
    case FontRole::SenderName:
        return FontCategory::Messages;
    case FontRole::Small:
    case FontRole::Caption:
    case FontRole::Timestamp:
        return FontCategory::Secondary;
    case FontRole::SidebarName:
    case FontRole::SidebarPreview:
    case FontRole::UnreadBadge:
        return FontCategory::RoomList;
    case FontRole::Title:
        return FontCategory::Headings;
    case FontRole::UiSemibold:
        return FontCategory::Buttons;
    case FontRole::BigEmoji:
    case FontRole::InlineEmoji:
    case FontRole::InlineCustomEmoji:
    case FontRole::EmojiPickerCell:
    case FontRole::ReactionEmoji:
    case FontRole::ReactionText:
        return FontCategory::Emoji;
    }
    return FontCategory::Messages;
}

int effective_base_pt(int system_base_pt)
{
    return g_config.base_pt ? g_config.base_pt : system_base_pt;
}

std::string_view font_role_family(FontRole role)
{
    const FontCategory cat = font_category(role);
    if (cat == FontCategory::Emoji)
    {
        return {};
    }
    const std::string& own = g_config.family[idx(cat)];
    return own.empty() ? std::string_view(g_config.ui_family) : std::string_view(own);
}

std::string_view font_mono_family()
{
    return g_config.mono_family;
}

namespace
{
int g_system_base_pt = 0;
}

void note_system_base_pt(int system_base_pt)
{
    g_system_base_pt = system_base_pt;
}

int system_base_pt()
{
    return g_system_base_pt;
}

float font_height_scale(FontRole role)
{
    // Called per row on layout and paint paths: memoise per role until the
    // config or the system base changes.
    struct Entry
    {
        std::uint64_t generation = ~std::uint64_t{0};
        int system_base = 0;
        float scale = 1.0f;
    };
    static std::array<Entry, 16> cache;
    const auto idx = static_cast<std::size_t>(role);
    Entry* e = idx < cache.size() ? &cache[idx] : nullptr;
    if (e && e->generation == g_generation && e->system_base == g_system_base_pt)
    {
        return e->scale;
    }

    float scale = 1.0f;
    if (g_system_base_pt > 0 && !g_config.is_default())
    {
        const int now = font_role_pt_with(g_config, role, g_system_base_pt);
        const int stock = font_role_pt_with(FontConfig{}, role, g_system_base_pt);
        if (stock > 0)
        {
            scale = std::max(1.0f, static_cast<float>(now) / stock);
        }
    }
    if (e)
    {
        *e = {g_generation, g_system_base_pt, scale};
    }
    return scale;
}

CachedTextLayout::CachedTextLayout() = default;
CachedTextLayout::~CachedTextLayout() = default;
CachedTextLayout::CachedTextLayout(CachedTextLayout&&) noexcept = default;
CachedTextLayout& CachedTextLayout::operator=(CachedTextLayout&&) noexcept = default;

TextLayout* CachedTextLayout::get() const
{
    return generation_ == g_generation ? layout_.get() : nullptr;
}

CachedTextLayout& CachedTextLayout::operator=(std::unique_ptr<TextLayout> layout)
{
    reset(std::move(layout));
    return *this;
}

void CachedTextLayout::reset()
{
    layout_.reset();
}

void CachedTextLayout::reset(std::unique_ptr<TextLayout> layout)
{
    layout_ = std::move(layout);
    generation_ = g_generation;
}

} // namespace tk
