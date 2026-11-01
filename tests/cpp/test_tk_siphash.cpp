#include <catch2/catch_test_macros.hpp>

#include "tk/siphash.h"

#include <string>

TEST_CASE("siphash128 matches the SipHash-2-4 128-bit reference vectors", "[siphash]")
{
    // Reference key 00 01 .. 0f; digest bytes are read little-endian.
    const tk::SipKey key{0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};

    const auto empty = tk::siphash128(key, "");
    CHECK(empty[0] == 0xe6a825ba047f81a3ULL);
    CHECK(empty[1] == 0x930255c71472f66dULL);

    const auto one = tk::siphash128(key, std::string(1, '\0'));
    CHECK(one[0] == 0x44af996bd8c187daULL);
    CHECK(one[1] == 0x45fc229b11597634ULL);
}

TEST_CASE("siphash128 depends on the key and every input byte", "[siphash]")
{
    const tk::SipKey a{1, 2};
    const tk::SipKey b{1, 3};
    const std::string body = "<table><tr><td>cell</td></tr></table>";

    CHECK(tk::siphash128(a, body) == tk::siphash128(a, body));
    CHECK(tk::siphash128(a, body) != tk::siphash128(b, body));
    CHECK(tk::siphash128(a, body) != tk::siphash128(a, body + " "));
    // Lengths straddling the 8-byte block boundary all differ.
    for (std::size_t n = 0; n < 24; ++n)
    {
        CHECK(tk::siphash128(a, std::string(n, 'x')) != tk::siphash128(a, std::string(n + 1, 'x')));
    }
}
