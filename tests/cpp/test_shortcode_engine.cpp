#include <catch2/catch_test_macros.hpp>
#include "views/ShortcodeEngine.h"
#include <tesseract/image_pack.h>

using tesseract::views::ShortcodeEngine;
using tesseract::views::ShortcodeMatch;
using tesseract::views::ShortcodeSuggestion;

// ── find_prefix ────────────────────────────────────────────────────────────

TEST_CASE("find_prefix:detects open shortcode at end of text")
{
    ShortcodeEngine eng;
    std::string text = "hello :gri";
    auto m = eng.find_prefix(text, (int)text.size());
    REQUIRE(m.has_value());
    CHECK(m->prefix == "gri");
    CHECK(m->start == 6); // byte offset of ':'
    CHECK(m->end == 10);  // one past 'i'
}

TEST_CASE("find_prefix:detects open shortcode with cursor inside")
{
    ShortcodeEngine eng;
    std::string text = "hello :grinning face";
    // cursor after "grin" (before "ning face")
    auto m = eng.find_prefix(text, 11); // ":grin" → start=6, end=11
    REQUIRE(m.has_value());
    CHECK(m->prefix == "grin");
    CHECK(m->start == 6);
}

TEST_CASE("find_prefix:no match when colon followed by space")
{
    ShortcodeEngine eng;
    std::string text = ": )";
    auto m = eng.find_prefix(text, (int)text.size());
    CHECK(!m.has_value());
}

TEST_CASE("find_prefix:no match in bare text")
{
    ShortcodeEngine eng;
    std::string text = "hello world";
    CHECK(!eng.find_prefix(text, (int)text.size()).has_value());
}

TEST_CASE("find_prefix:no match when colon is closed (full shortcode)")
{
    ShortcodeEngine eng;
    std::string text = ":smile:";
    // Cursor after the closing colon:this is find_complete territory.
    // The prefix between the closing ':' and cursor is empty; we require at
    // least one shortcode character after the opening ':'.
    CHECK(!eng.find_prefix(text, (int)text.size()).has_value());
}

TEST_CASE("find_prefix:no match when cursor is outside the colon sequence")
{
    ShortcodeEngine eng;
    std::string text = ":gri hello";
    // cursor=5 lands after the space (text[4]==' '), past the shortcode word.
    // cursor=4 would be the end of ":gri" which IS a valid open shortcode;
    // cursor=5 is one step beyond the space, so no shortcode is active.
    CHECK(!eng.find_prefix(text, 5).has_value());
}

// ── find_complete ──────────────────────────────────────────────────────────

TEST_CASE("find_complete:detects complete shortcode before space")
{
    ShortcodeEngine eng;
    std::string text = ":smile: world";
    // Cursor just after the space (index 8)
    auto m = eng.find_complete(text, 8);
    REQUIRE(m.has_value());
    CHECK(m->prefix == "smile");
    CHECK(m->start == 0);
    CHECK(m->end == 7); // one past the closing ':'
}

TEST_CASE("find_complete:detects complete shortcode at end of text")
{
    ShortcodeEngine eng;
    std::string text = "hi :smile:";
    auto m = eng.find_complete(text, (int)text.size());
    REQUIRE(m.has_value());
    CHECK(m->prefix == "smile");
    CHECK(m->start == 3);
}

TEST_CASE("find_complete:no match for unclosed shortcode")
{
    ShortcodeEngine eng;
    std::string text = ":smile";
    CHECK(!eng.find_complete(text, (int)text.size()).has_value());
}

TEST_CASE("find_complete:no match when closing colon not immediately before "
          "cursor/space")
{
    ShortcodeEngine eng;
    std::string text = ":smile: abc";
    // Cursor at end (not immediately after closing colon)
    CHECK(!eng.find_complete(text, (int)text.size()).has_value());
}

// ── lookup ─────────────────────────────────────────────────────────────────

TEST_CASE("lookup:returns Unicode emoji matches")
{
    ShortcodeEngine eng;
    std::vector<tesseract::ImagePackImage> no_packs;
    auto results = eng.lookup("grinning", no_packs);
    REQUIRE(!results.empty());
    bool found_unicode = false;
    for (const auto& s : results)
    {
        if (!s.glyph.empty())
        {
            found_unicode = true;
            break;
        }
    }
    CHECK(found_unicode);
}

TEST_CASE("lookup:respects max_results cap")
{
    ShortcodeEngine eng;
    std::vector<tesseract::ImagePackImage> no_packs;
    auto results = eng.lookup("a", no_packs, 3);
    CHECK(results.size() <= 3);
}

TEST_CASE("lookup:exact prefix ranked before substring")
{
    ShortcodeEngine eng;
    std::vector<tesseract::ImagePackImage> no_packs;
    auto results = eng.lookup("smile", no_packs);
    REQUIRE(!results.empty());
    CHECK(results.front().shortcode.substr(0, 5) == "smile");
}

TEST_CASE("lookup:returns custom emoticon matches")
{
    ShortcodeEngine eng;
    tesseract::ImagePackImage img;
    img.shortcode = "happy_cat";
    img.url = "mxc://example.org/abc";
    img.usage = tesseract::PackUsage::Emoticon;
    std::vector<tesseract::ImagePackImage> packs{img};

    auto results = eng.lookup("happy", packs);
    REQUIRE(!results.empty());
    bool found_custom = false;
    for (const auto& s : results)
    {
        if (s.glyph.empty() && s.shortcode == "happy_cat")
        {
            found_custom = true;
            break;
        }
    }
    CHECK(found_custom);
}

TEST_CASE("lookup:empty prefix returns empty result")
{
    ShortcodeEngine eng;
    std::vector<tesseract::ImagePackImage> no_packs;
    auto results = eng.lookup("", no_packs, 8);
    CHECK(results.empty());
}
