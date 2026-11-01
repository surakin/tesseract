#pragma once

// User font configuration: a master base size, a UI + monospace family, and
// per-category size deltas / family overrides. tk stays independent of
// tesseract::Settings — the shell converts Settings::FontPrefs into a
// FontConfig and pushes it in with set_font_config().
//
// font_role_pt() (canvas.h) reads the active config, so every backend picks up
// the new sizes the next time it builds a font. Anything that caches a font or
// shaped text records font_config_generation() and drops itself when it moves.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tk
{

enum class FontRole;
class TextLayout;

// User-facing groups of FontRoles. Emoji is size-only (its face is always the
// colour-emoji font).
enum class FontCategory
{
    Messages,  // Body, SenderName
    Secondary, // Caption, Small, Timestamp
    RoomList,  // SidebarName, SidebarPreview, UnreadBadge
    Headings,  // Title
    Buttons,   // UiSemibold
    Emoji,     // BigEmoji, Inline*, EmojiPickerCell, Reaction*
};
inline constexpr std::size_t kFontCategoryCount = 6;

inline constexpr int kFontBaseMinPt = 6;
inline constexpr int kFontBaseMaxPt = 36;
inline constexpr int kFontDeltaMinPt = -6;
inline constexpr int kFontDeltaMaxPt = 12;

struct FontConfig
{
    int base_pt = 0;         // 0 = follow the system base
    std::string ui_family;   // "" = platform default face
    std::string mono_family; // "" = platform default monospace
    std::array<int, kFontCategoryCount> delta_pt{};
    std::array<std::string, kFontCategoryCount> family{}; // "" = use ui_family

    bool is_default() const;
    bool operator==(const FontConfig&) const = default;
};

// The active config (defaults until the shell pushes one). UI thread only.
const FontConfig& font_config();

// Replaces the active config (values are clamped) and bumps the generation if
// anything changed.
void set_font_config(FontConfig cfg);

// Increments whenever the effective config changes. Never 0 after the first
// change; caches initialised to 0 are therefore valid for the default config.
std::uint64_t font_config_generation();

FontCategory font_category(FontRole role);

// base_pt ? base_pt : system_base_pt.
int effective_base_pt(int system_base_pt);

// font_role_pt() (canvas.h) evaluated against an explicit config instead of
// the active one.
int font_role_pt_with(const FontConfig& cfg, FontRole role, int system_base_pt);

// Remembers the platform's system body size; font_role_pt() calls this on
// every use, so it tracks whatever base the backends pass.
void note_system_base_pt(int system_base_pt);

// The system body size last passed to font_role_pt(); 0 until a backend has
// asked for any font.
int system_base_pt();

// How much larger `role`'s text is than at the default config, >= 1.0 (1.0
// when it is the same or smaller, or the system base isn't known yet). For
// scaling fixed pixel heights that were sized for the default fonts.
float font_height_scale(FontRole role);

// Family for `role` ("" = backend default): the category override, else the
// global UI family. Always "" for the Emoji category.
std::string_view font_role_family(FontRole role);

// Monospace family ("" = backend default).
std::string_view font_mono_family();

// An installed font family, as enumerated by Host::list_font_families().
struct FontFamilyInfo
{
    std::string name;
    bool monospace = false;
};

// A TextLayout cache slot that empties itself when the font config changes, so
// widgets rebuild their layout without a per-widget flush hook.
class CachedTextLayout
{
public:
    CachedTextLayout();
    ~CachedTextLayout();
    CachedTextLayout(CachedTextLayout&&) noexcept;
    CachedTextLayout& operator=(CachedTextLayout&&) noexcept;

    // Null while empty or stale.
    TextLayout* get() const;
    TextLayout* operator->() const
    {
        return get();
    }
    TextLayout& operator*() const
    {
        return *get();
    }
    explicit operator bool() const
    {
        return get() != nullptr;
    }

    // Out of line: the by-value parameter's destructor needs TextLayout complete.
    CachedTextLayout& operator=(std::unique_ptr<TextLayout> layout);

    void reset();
    void reset(std::unique_ptr<TextLayout> layout);

private:
    std::unique_ptr<TextLayout> layout_;
    std::uint64_t generation_ = 0;
};

} // namespace tk
