#include <catch2/catch_test_macros.hpp>
#include "tesseract/emoji.h"

#include <set>

using tesseract::emoji::Category;

TEST_CASE("emoji::all() has a sensible number of entries", "[emoji]")
{
    const auto& all = tesseract::emoji::all();
    // Curated catalog from Unicode 17 fully-qualified entries (skin tones
    // dropped). Anywhere from ~1500 to ~2500 is plausible across Unicode
    // version bumps:assert a broad-but-defensive range.
    CHECK(all.size() >= 1500);
    CHECK(all.size() <= 3000);
}

TEST_CASE("every emoji entry has glyph + name + valid category", "[emoji]")
{
    for (const auto& e : tesseract::emoji::all())
    {
        CHECK_FALSE(e.glyph.empty());
        CHECK_FALSE(e.name.empty());
        const int idx = static_cast<int>(e.category);
        CHECK(idx >= 0);
        CHECK(idx <= static_cast<int>(Category::Flags));
    }
}

TEST_CASE("emoji::filter is case-insensitive substring", "[emoji]")
{
    auto hits = tesseract::emoji::filter("smile");
    REQUIRE_FALSE(hits.empty());

    // At least one hit's name must contain the search term (case-insensitive).
    bool found_in_name = false;
    for (const auto* e : hits)
    {
        for (std::size_t i = 0; i + 5 <= e->name.size(); ++i)
        {
            auto sub = e->name.substr(i, 5);
            if (sub == "smile" || sub == "Smile" || sub == "SMILE")
            {
                found_in_name = true;
                break;
            }
        }
        if (found_in_name)
        {
            break;
        }
    }
    CHECK(found_in_name);

    // Mixed case search yields the same result set.
    auto hits_upper = tesseract::emoji::filter("SMILE");
    CHECK(hits.size() == hits_upper.size());
}

TEST_CASE("emoji::filter empty query matches everything", "[emoji]")
{
    auto all_hits = tesseract::emoji::filter("");
    CHECK(all_hits.size() == tesseract::emoji::all().size());
}

TEST_CASE("emoji::by_category returns entries with matching tag", "[emoji]")
{
    for (Category c : tesseract::emoji::kCategories)
    {
        auto entries = tesseract::emoji::by_category(c);
        // Every category in the bundled catalog should be non-empty.
        REQUIRE_FALSE(entries.empty());
        for (const auto* e : entries)
        {
            CHECK(e->category == c);
        }
    }
}

TEST_CASE("category metadata is populated for every category", "[emoji]")
{
    for (Category c : tesseract::emoji::kCategories)
    {
        const char* name = tesseract::emoji::category_name(c);
        const char* tab = tesseract::emoji::category_tab_glyph(c);
        REQUIRE(name != nullptr);
        REQUIRE(tab != nullptr);
        CHECK(*name != '\0');
        CHECK(*tab != '\0');
    }
}

TEST_CASE("known emoji can be located by exact name", "[emoji]")
{
    bool found_grinning = false;
    for (const auto& e : tesseract::emoji::all())
    {
        if (e.name == "grinning face")
        {
            CHECK(e.category == Category::SmileysPeople);
            CHECK_FALSE(e.glyph.empty());
            found_grinning = true;
            break;
        }
    }
    CHECK(found_grinning);
}

TEST_CASE("by_shortcode_prefix:canonical match")
{
    // "grinning_face" should surface 😀 which has CLDR name "grinning face"
    auto results = tesseract::emoji::by_shortcode_prefix("grinning_face");
    REQUIRE(!results.empty());
    bool found = false;
    for (auto [entry, shortcode] : results)
    {
        if (entry->glyph == "😀")
        {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("by_shortcode_prefix:alias match")
{
    // "grinning" is a gemoji alias for 😀:must surface via alias table
    auto results = tesseract::emoji::by_shortcode_prefix("grinning");
    REQUIRE(!results.empty());
    bool found = false;
    for (auto [entry, shortcode] : results)
    {
        if (entry->glyph == "😀")
        {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("by_shortcode_prefix:no partial match below 1 char")
{
    // empty prefix returns everything; just verify it doesn't crash
    auto all = tesseract::emoji::by_shortcode_prefix("");
    REQUIRE(all.size() > 100);
}

TEST_CASE("by_shortcode_prefix:returns empty for unknown prefix")
{
    auto none = tesseract::emoji::by_shortcode_prefix("xyzzy_not_an_emoji");
    REQUIRE(none.empty());
}

TEST_CASE("by_shortcode_prefix:Entry shortcodes field is non-empty")
{
    const auto& table = tesseract::emoji::all();
    REQUIRE(!table.empty());
    for (const auto& e : table)
    {
        REQUIRE(!e.shortcodes.empty());
    }
}
