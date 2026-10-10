#include <catch2/catch_test_macros.hpp>

#include "tk/blurhash.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
const char kB83[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

std::string b83(unsigned v, int n)
{
    std::string s(static_cast<std::size_t>(n), '0');
    for (int i = n - 1; i >= 0; --i)
    {
        s[static_cast<std::size_t>(i)] = kB83[v % 83];
        v /= 83;
    }
    return s;
}
} // namespace

TEST_CASE("decode_blurhash rejects empty and too-short hashes", "[tk][blurhash]")
{
    std::vector<uint8_t> px;
    CHECK_FALSE(tk::decode_blurhash("", 4, 4, px));
    CHECK_FALSE(tk::decode_blurhash("LEHV", 4, 4, px));
}

TEST_CASE("decode_blurhash rejects a hash shorter than its component count "
          "requires",
          "[tk][blurhash]")
{
    std::vector<uint8_t> px;
    // Size flag 'L' = 4x3 components needs 4 + 2*11 + 2 = 28 chars.
    CHECK_FALSE(tk::decode_blurhash("LEHV6nWB2yk8py", 4, 4, px));
}

TEST_CASE("decode_blurhash of a 1x1-component hash yields a flat DC colour",
          "[tk][blurhash]")
{
    // flag 0 (1x1), quant-max 0, DC = pure red 0xFF0000.
    const std::string hash = "0" + b83(0, 1) + b83(0xFF0000u, 4);
    REQUIRE(hash.size() == 6);
    std::vector<uint8_t> px;
    REQUIRE(tk::decode_blurhash(hash, 3, 2, px));
    REQUIRE(px.size() == 3u * 2u * 4u);
    for (std::size_t i = 0; i < 6; ++i)
    {
        CHECK(px[i * 4 + 0] == 255);
        CHECK(px[i * 4 + 1] == 0);
        CHECK(px[i * 4 + 2] == 0);
        CHECK(px[i * 4 + 3] == 255);
    }
}

TEST_CASE("decode_blurhash decodes a real multi-component hash "
          "deterministically",
          "[tk][blurhash]")
{
    const std::string hash = "LEHV6nWB2yk8pyo0adR*.7kCMdnj";
    std::vector<uint8_t> a, b;
    REQUIRE(tk::decode_blurhash(hash, 32, 24, a));
    REQUIRE(tk::decode_blurhash(hash, 32, 24, b));
    CHECK(a == b);
    REQUIRE(a.size() == 32u * 24u * 4u);
    bool varies = false;
    for (std::size_t i = 0; i < a.size(); i += 4)
    {
        CHECK(a[i + 3] == 255);
        if (a[i] != a[0] || a[i + 1] != a[1] || a[i + 2] != a[2])
            varies = true;
    }
    CHECK(varies); // AC components give a gradient, not a flat colour
}

TEST_CASE("decode_blurhash treats invalid base83 characters as zero",
          "[tk][blurhash]")
{
    std::vector<uint8_t> px;
    // Spaces are not in the alphabet: decode_b83 returns 0, still "valid".
    REQUIRE(tk::decode_blurhash("0     ", 1, 1, px));
    CHECK(px.size() == 4);
}
