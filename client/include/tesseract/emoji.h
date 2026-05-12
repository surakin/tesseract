#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace tesseract::emoji {

/// Picker categories — combines Unicode's "Smileys & Emotion" and
/// "People & Body" into a single tab for the standard mobile-keyboard
/// layout. Order is the order tabs appear in the picker.
enum class Category : std::uint8_t {
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
struct Entry {
    std::string_view glyph;     ///< UTF-8 glyph (may be a multi-codepoint sequence).
    std::string_view name;      ///< CLDR short name, e.g. "grinning face".
    std::string_view keywords;  ///< Space-separated search terms, lowercase.
    Category         category;
};

/// All bundled emoji, in Unicode-CLDR display order grouped by category.
const std::vector<Entry>& all();

/// Entries whose `name` or `keywords` contain `query` (case-insensitive
/// substring). Returns pointers into the static table.
std::vector<const Entry*> filter(std::string_view query);

/// Entries belonging to category `c`, in declaration order.
std::vector<const Entry*> by_category(Category c);

/// Human-readable category label, e.g. "Smileys & People".
const char* category_name(Category c);

/// One representative glyph used as the category tab icon.
const char* category_tab_glyph(Category c);

/// Iteration helper — categories in tab order, suitable for building the
/// tab strip.
constexpr Category kCategories[] = {
    Category::SmileysPeople, Category::AnimalsNature, Category::FoodDrink,
    Category::Activities,    Category::TravelPlaces,  Category::Objects,
    Category::Symbols,       Category::Flags,
};

} // namespace tesseract::emoji
