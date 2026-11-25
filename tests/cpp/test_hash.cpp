#include <catch2/catch_test_macros.hpp>
#include "tesseract/hash.h"

#include <cstring>

TEST_CASE("sha256 known vectors", "[hash]")
{
    CHECK(tesseract::to_hex(tesseract::sha256("", 0)) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(tesseract::to_hex(tesseract::sha256("abc", 3)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("to_hex is 64 lower-case hex characters", "[hash]")
{
    const auto hex = tesseract::to_hex(tesseract::sha256("abc", 3));
    REQUIRE(hex.size() == 64);
    for (char c : hex)
    {
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}

TEST_CASE("sha256 of zero bytes with a null pointer is the empty digest", "[hash]")
{
    CHECK(tesseract::to_hex(tesseract::sha256(nullptr, 0)) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
