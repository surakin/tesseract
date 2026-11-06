#include <catch2/catch_test_macros.hpp>
#include "tk/emoji_support.h"
#include "views/EmoticonReplacer.h"

#include <string>

using tesseract::views::find_emoticon;

namespace
{

constexpr std::string_view kSmile = "\xF0\x9F\x99\x82"; // 🙂
constexpr std::string_view kFrown = "\xF0\x9F\x99\x81"; // 🙁

std::optional<tesseract::views::EmoticonMatch> at_end(std::string_view text)
{
    return find_emoticon(text, static_cast<int>(text.size()), tk::kAllEmojiVersions);
}

} // namespace

TEST_CASE("find_emoticon: an emoticon followed by a space matches", "[emoticon]")
{
    auto m = at_end("hi :-) ");
    REQUIRE(m.has_value());
    CHECK(m->start == 3);
    CHECK(m->end == 6); // the trailing space stays
    CHECK(m->glyph == kSmile);
}

TEST_CASE("find_emoticon: a closed emoticon at end of text matches at once", "[emoticon]")
{
    auto m = at_end(":(");
    REQUIRE(m.has_value());
    CHECK(m->start == 0);
    CHECK(m->end == 2);
    CHECK(m->glyph == kFrown);
    CHECK(at_end("<3 ").has_value());
}

TEST_CASE("find_emoticon: tokens that may still grow wait for a delimiter", "[emoticon]")
{
    // ":p" may be the start of ":pizza:", "D:" of "D:\path".
    CHECK_FALSE(at_end(":p").has_value());
    CHECK_FALSE(at_end("D:").has_value());
    CHECK_FALSE(at_end("<3").has_value());
    CHECK(at_end(":p ").has_value());
    CHECK(at_end("D: ").has_value());
}

TEST_CASE("find_emoticon: only whole whitespace-delimited tokens match", "[emoticon]")
{
    CHECK_FALSE(at_end("a:) ").has_value());
    CHECK_FALSE(at_end("at 12:30 ").has_value());
    CHECK_FALSE(at_end("http://x ").has_value());
    CHECK_FALSE(at_end("(:)) ").has_value());
    CHECK_FALSE(at_end(":)x").has_value());
    CHECK_FALSE(at_end("plain ").has_value());
    // "B)" is a list marker, not an emoticon.
    CHECK_FALSE(at_end("option B) ").has_value());
    CHECK(at_end("line\n:) ").has_value());
}

TEST_CASE("find_emoticon: the caret must sit right after the token", "[emoticon]")
{
    std::string text = ":) and more";
    CHECK(find_emoticon(text, 3, tk::kAllEmojiVersions).has_value());       // after ":) "
    CHECK_FALSE(find_emoticon(text, 2, tk::kAllEmojiVersions).has_value()); // mid-text, no space
    CHECK_FALSE(find_emoticon(text, 7, tk::kAllEmojiVersions).has_value()); // after "and "
}

TEST_CASE("find_emoticon: code spans and fences are left alone", "[emoticon]")
{
    CHECK_FALSE(at_end("`:) ").has_value());
    CHECK(at_end("`x` :) ").has_value());
    CHECK_FALSE(at_end("``a ` :) ").has_value());
    CHECK_FALSE(at_end("```\ncode :) ").has_value());
    CHECK(at_end("```\ncode\n``` :) ").has_value());
    CHECK(at_end("\\` :) ").has_value()); // escaped backtick opens nothing
}

TEST_CASE("find_emoticon: offsets are bytes after multibyte text", "[emoticon]")
{
    std::string text = "\xC3\xA9t\xC3\xA9 :) "; // "été :) "
    auto m = at_end(text);
    REQUIRE(m.has_value());
    CHECK(m->start == 6);
    CHECK(m->end == 8);
}

TEST_CASE("find_emoticon: glyphs newer than the font are skipped", "[emoticon]")
{
    // ":')" is 🥲 (Emoji 13); ":)" is Emoji 1.
    CHECK(find_emoticon(":') ", 4, tk::kAllEmojiVersions).has_value());
    CHECK_FALSE(find_emoticon(":') ", 4, 120).has_value());
    CHECK(find_emoticon(":) ", 3, 120).has_value());
}
