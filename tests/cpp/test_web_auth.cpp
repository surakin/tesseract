#include <catch2/catch_test_macros.hpp>

#include "views/web_auth.h"

TEST_CASE("callback_scheme extracts the private-use scheme", "[views][oauth]")
{
    CHECK(tesseract::views::callback_scheme("io.github.surakin:/callback") == "io.github.surakin");
    CHECK(tesseract::views::callback_scheme("http://127.0.0.1:4321/callback") == "http");
    CHECK(tesseract::views::callback_scheme("no-scheme-here").empty());
    CHECK(tesseract::views::callback_scheme("").empty());
}
