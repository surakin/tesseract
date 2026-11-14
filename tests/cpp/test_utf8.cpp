#include <tesseract/utf8.h>

#include <catch2/catch_test_macros.hpp>
#include <rust/cxx.h>

#include <string>

using tesseract::utf8_lossy;

TEST_CASE("utf8_lossy returns valid UTF-8 unchanged", "[utf8]")
{
    CHECK(utf8_lossy("").empty());
    CHECK(utf8_lossy("plain ascii") == "plain ascii");
    CHECK(utf8_lossy("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80") ==
          "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
}

TEST_CASE("utf8_lossy replaces invalid bytes with U+FFFD", "[utf8]")
{
    const std::string bad = "\xEF\xBF\xBD";
    CHECK(utf8_lossy("a\xFF"
                     "b") == "a" + bad + "b");
    CHECK(utf8_lossy("\xC3") == bad);                               // truncated sequence
    CHECK(utf8_lossy("\xC0\xAF") == bad + bad);                     // overlong '/'
    CHECK(utf8_lossy("\xED\xA0\x80") == bad + bad + bad);           // a surrogate
    CHECK(utf8_lossy("\xF4\x90\x80\x80") == bad + bad + bad + bad); // above U+10FFFF
    CHECK(utf8_lossy("ok\xE2\x82") == "ok" + bad + bad);            // cut-off euro sign
}

TEST_CASE("cxx rejects invalid UTF-8 and accepts the sanitised string", "[utf8]")
{
    const std::string name = "photo-\xE9.jpg"; // Latin-1 'é', a legal Linux file name
    CHECK_THROWS(rust::Str(name));
    const std::string clean = utf8_lossy(name);
    CHECK_NOTHROW(rust::Str(clean));
}
