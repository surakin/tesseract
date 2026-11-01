#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>

namespace tk
{

using SipKey = std::array<std::uint64_t, 2>;
using SipDigest = std::array<std::uint64_t, 2>;

// A fresh random 128-bit key. Keep one per process so digests can't be
// predicted, and therefore can't be collided on purpose, by whoever supplies
// the data.
inline SipKey random_sip_key()
{
    std::random_device rd;
    auto r64 = [&rd]
    {
        return (static_cast<std::uint64_t>(rd()) << 32) | rd();
    };
    return SipKey{r64(), r64()};
}

// SipHash-2-4 with 128-bit output. Keyed, so with a secret `key` it is a
// collision-resistant fingerprint for cache keys that must not retain the data.
inline SipDigest siphash128(const SipKey& key, std::string_view data)
{
    auto rotl = [](std::uint64_t x, int b)
    {
        return (x << b) | (x >> (64 - b));
    };
    std::uint64_t v0 = key[0] ^ 0x736f6d6570736575ULL;
    std::uint64_t v1 = key[1] ^ 0x646f72616e646f6dULL ^ 0xeeULL;
    std::uint64_t v2 = key[0] ^ 0x6c7967656e657261ULL;
    std::uint64_t v3 = key[1] ^ 0x7465646279746573ULL;
    auto round = [&]
    {
        v0 += v1;
        v1 = rotl(v1, 13);
        v1 ^= v0;
        v0 = rotl(v0, 32);
        v2 += v3;
        v3 = rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotl(v1, 17);
        v1 ^= v2;
        v2 = rotl(v2, 32);
    };
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t n = data.size();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        std::uint64_t m = 0;
        for (int j = 7; j >= 0; --j)
        {
            m = (m << 8) | p[i + j];
        }
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    std::uint64_t last = static_cast<std::uint64_t>(n) << 56;
    for (std::size_t j = 0; i + j < n; ++j)
    {
        last |= static_cast<std::uint64_t>(p[i + j]) << (8 * j);
    }
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xeeULL;
    for (int k = 0; k < 4; ++k)
    {
        round();
    }
    const std::uint64_t h0 = v0 ^ v1 ^ v2 ^ v3;
    v1 ^= 0xddULL;
    for (int k = 0; k < 4; ++k)
    {
        round();
    }
    return SipDigest{h0, v0 ^ v1 ^ v2 ^ v3};
}

struct SipDigestHash
{
    std::size_t operator()(const SipDigest& d) const noexcept
    {
        return static_cast<std::size_t>(d[0]);
    }
};

} // namespace tk
