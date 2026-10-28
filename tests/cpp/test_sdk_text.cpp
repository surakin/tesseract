#include "views/sdk_text.h"

#include <catch2/catch_test_macros.hpp>

using tesseract::views::sdk_text::is_code;
using tesseract::views::sdk_text::localize;

TEST_CASE("sdk_text passes ordinary text through unchanged", "[sdk_text]")
{
    CHECK(localize("") == "");
    CHECK(localize("hello") == "hello");
    CHECK(localize("(image)") == "(image)"); // a preview cached before codes existed
}

TEST_CASE("sdk_text decodes preview, decryption and pin codes", "[sdk_text]")
{
    CHECK(localize("\x01image") == "(image)");
    CHECK(localize("\x01poll") == "(poll)");
    CHECK(localize("\x01poll\x01Lunch?") == "(poll) Lunch?");
    CHECK(localize("\x01utd_unknown") == "🔒 Unable to decrypt");
    CHECK(localize("\x01pin_added") == "pinned a message");
    CHECK(localize("\x01pin_added_n\x01" "3") == "pinned 3 messages");
    CHECK(localize("\x01pin_removed_n\x01" "2") == "unpinned 2 messages");
}

TEST_CASE("sdk_text shows an unknown code by its name", "[sdk_text]")
{
    CHECK(localize("\x01" "from_the_future") == "from_the_future");
}

TEST_CASE("sdk_text::is_code matches a code by its exact name", "[sdk_text]")
{
    CHECK(is_code("\x01" "deleted", "deleted"));
    CHECK(is_code("\x01" "deleted\x01" "arg", "deleted"));
    CHECK_FALSE(is_code("\x01" "deletedx", "deleted"));
    CHECK_FALSE(is_code("deleted", "deleted"));
    CHECK_FALSE(is_code("", "deleted"));
}
