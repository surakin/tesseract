#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace tesseract::emoji
{

/// Picker categories — combines Unicode's "Smileys & Emotion" and
/// "People & Body" into a single tab for the standard mobile-keyboard
/// layout. Order is the order tabs appear in the picker.
enum class Category : std::uint8_t
{
    SmileysPeople,
    AnimalsNature,
    FoodDrink,
    Activities,
    TravelPlaces,
    Objects,
    Symbols,
    Flags,
};

/// Single emoji entry. All fields are static string views into the
/// embedded data table — never owned, never freed.
struct Entry
{
    std::string_view glyph;    ///< UTF-8 glyph (may be a multi-codepoint sequence).
    std::string_view name;     ///< CLDR short name, e.g. "grinning face".
    std::string_view keywords; ///< Space-separated search terms, lowercase.
    Category category;
    std::string_view
        shortcodes;        ///< Space-separated; first token is canonical (snake_case CLDR name).
    std::uint16_t version; ///< Emoji version, major*10 + minor (E12.1 → 121).
};

/// All bundled emoji, in Unicode-CLDR display order grouped by category.
const std::vector<Entry>& all();

/// Entries whose `name` or `keywords` contain `query` (case-insensitive
/// substring). Returns pointers into the static table.
std::vector<const Entry*> filter(std::string_view query);

/// Entries belonging to category `c`, in declaration order.
std::vector<const Entry*> by_category(Category c);

/// Entries whose shortcode tokens (canonical + aliases) contain `prefix`
/// (case-insensitive substring). Returns pairs of (entry, matched_shortcode)
/// so callers can rank by match quality. Pointers into the static table.
std::vector<std::pair<const Entry*, std::string_view>> by_shortcode_prefix(std::string_view prefix);

/// Human-readable category label, e.g. "Smileys & People".
const char* category_name(Category c);

/// One representative glyph used as the category tab icon.
const char* category_tab_glyph(Category c);

/// Fitzpatrick skin tone applied to emoji that support one. `None` is the
/// default (yellow) presentation. Order matches the picker's tone menu.
enum class SkinTone : std::uint8_t
{
    None,
    Light,
    MediumLight,
    Medium,
    MediumDark,
    Dark,
};

/// Every tone, in menu order (None first).
constexpr SkinTone kSkinTones[] = {
    SkinTone::None,   SkinTone::Light,      SkinTone::MediumLight,
    SkinTone::Medium, SkinTone::MediumDark, SkinTone::Dark,
};

/// True when `glyph` (a base emoji or one of its toned variants) has the
/// five uniform skin-tone variants.
bool supports_skin_tone(std::string_view glyph);

/// `glyph` with any skin tone removed — the base emoji for a toned variant,
/// `glyph` itself otherwise. Points into the static table or at `glyph`.
std::string_view base_glyph(std::string_view glyph);

/// The `tone` variant of `glyph` (base or already-toned). Returns the base
/// for `SkinTone::None`, and `glyph` unchanged when it has no tones.
std::string_view with_skin_tone(std::string_view glyph, SkinTone tone);

/// Persisted key for `tone` ("light", "medium_light", "medium",
/// "medium_dark", "dark"); empty for `None`.
const char* skin_tone_key(SkinTone tone);

/// Inverse of `skin_tone_key`; empty or unknown keys map to `None`.
SkinTone skin_tone_from_key(std::string_view key);

/// Emoji version (major*10 + minor, E12.1 → 121) that introduced `glyph` —
/// a picker entry or one of its tone variants, which can be newer than the
/// base. 0 for glyphs not in the table.
std::uint16_t emoji_version(std::string_view glyph);

/// Every Emoji version present in the table, newest first.
const std::vector<std::uint16_t>& emoji_versions();

/// The picker entries and uniform tone variants introduced in `version`.
std::vector<std::string_view> glyphs_introduced_in(std::uint16_t version);

/// Iteration helper — categories in tab order, suitable for building the
/// tab strip.
constexpr Category kCategories[] = {
    Category::SmileysPeople, Category::AnimalsNature, Category::FoodDrink, Category::Activities,
    Category::TravelPlaces,  Category::Objects,       Category::Symbols,   Category::Flags,
};

} // namespace tesseract::emoji
