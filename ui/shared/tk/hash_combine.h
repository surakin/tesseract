#pragma once

#include <cstddef>

namespace tk
{

// Fold `value` into the running hash `seed` (boost-style mix with the 64-bit
// golden-ratio constant). Order-dependent: call repeatedly to combine several
// fields into one hash, or XOR per-element results for an order-independent
// digest of a set.
inline std::size_t hash_combine(std::size_t seed, std::size_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

} // namespace tk
