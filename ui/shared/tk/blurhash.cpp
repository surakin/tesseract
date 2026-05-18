#include "blurhash.h"
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace tk
{

namespace
{

// Base83 character set used by BlurHash.
static const char kBase83Chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefg"
                                   "hijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

int base83_decode_char(char c)
{
    for (int i = 0; i < 83; ++i)
    {
        if (kBase83Chars[i] == c)
        {
            return i;
        }
    }
    return -1;
}

// Decode `num_chars` characters from `hash` starting at `start` as a base83 integer.
static uint32_t decode_b83(const std::string& hash, int start, int num_chars)
{
    uint32_t val = 0;
    for (int i = start; i < start + num_chars; ++i)
    {
        int d = base83_decode_char(hash[i]);
        if (d < 0)
        {
            return 0;
        }
        val = val * 83 + static_cast<uint32_t>(d);
    }
    return val;
}

// sRGB gamma expansion (0..255 → 0..1 linear).
static float srgb_to_linear(int value)
{
    float f = value / 255.0f;
    return (f <= 0.04045f) ? (f / 12.92f)
                           : std::pow((f + 0.055f) / 1.055f, 2.4f);
}

// Linear → sRGB, clamped to 0..255.
static uint8_t linear_to_srgb(float value)
{
    float clamped = std::fmax(0.0f, std::fmin(1.0f, value));
    float encoded = (clamped <= 0.0031308f)
                        ? (clamped * 12.92f)
                        : (1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f);
    return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}

// Decode a quantised AC component value into a signed float in [-1, 1].
static float decode_ac_value(int quantised, float max_ac)
{
    float f = static_cast<float>(quantised) / 9.0f - 1.0f;
    return (f < 0.0f ? -1.0f : 1.0f) * f * f * max_ac;
}

} // namespace

bool decode_blurhash(const std::string& hash, int w, int h,
                     std::vector<uint8_t>& out_rgba)
{
    if (hash.size() < 6)
    {
        return false;
    }

    // Char 0: component count.
    uint32_t size_flag = decode_b83(hash, 0, 1);
    int comp_x = static_cast<int>(size_flag % 9) + 1;
    int comp_y = static_cast<int>(size_flag / 9) + 1;
    int total = comp_x * comp_y;

    // Minimum length: 1 (flag) + 1 (quant max) + 4 (DC) + 2*(total-1) (AC).
    int expected = 4 + 2 * (total - 1) + 2;
    if (static_cast<int>(hash.size()) < expected)
    {
        return false;
    }

    // Char 1: quantised maximum AC component magnitude.
    float max_ac = 0.0f;
    if (total > 1)
    {
        int quant_max = static_cast<int>(decode_b83(hash, 1, 1));
        max_ac = (static_cast<float>(quant_max) + 1.0f) / 166.0f;
    }

    // Per-component linear-light RGB triplets.
    struct Color3
    {
        float r, g, b;
    };
    std::vector<Color3> colors(static_cast<size_t>(total));

    // DC component (chars 2..5, 5-char base83 value → packed sRGB).
    {
        uint32_t dc_val = decode_b83(hash, 2, 4);
        int dc_r = static_cast<int>((dc_val >> 16) & 0xFF);
        int dc_g = static_cast<int>((dc_val >> 8) & 0xFF);
        int dc_b = static_cast<int>(dc_val & 0xFF);
        colors[0] = {srgb_to_linear(dc_r), srgb_to_linear(dc_g),
                     srgb_to_linear(dc_b)};
    }

    // AC components (2 chars each, starting at char 6).
    for (int i = 1; i < total; ++i)
    {
        int ac_val = static_cast<int>(decode_b83(hash, 6 + (i - 1) * 2, 2));
        int q_r = ac_val / (19 * 19);
        int q_g = (ac_val / 19) % 19;
        int q_b = ac_val % 19;
        colors[static_cast<size_t>(i)] = {decode_ac_value(q_r, max_ac),
                                          decode_ac_value(q_g, max_ac),
                                          decode_ac_value(q_b, max_ac)};
    }

    out_rgba.resize(static_cast<size_t>(w * h * 4));

    for (int py = 0; py < h; ++py)
    {
        for (int px = 0; px < w; ++px)
        {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int j = 0; j < comp_y; ++j)
            {
                float basis_y = std::cos(
                    (M_PI * static_cast<float>(j) * static_cast<float>(py)) /
                    static_cast<float>(h));
                for (int i = 0; i < comp_x; ++i)
                {
                    float basis =
                        basis_y * std::cos((M_PI * static_cast<float>(i) *
                                            static_cast<float>(px)) /
                                           static_cast<float>(w));
                    const Color3& c =
                        colors[static_cast<size_t>(j * comp_x + i)];
                    r += c.r * basis;
                    g += c.g * basis;
                    b += c.b * basis;
                }
            }
            size_t idx = static_cast<size_t>((py * w + px) * 4);
            out_rgba[idx + 0] = linear_to_srgb(r);
            out_rgba[idx + 1] = linear_to_srgb(g);
            out_rgba[idx + 2] = linear_to_srgb(b);
            out_rgba[idx + 3] = 255;
        }
    }
    return true;
}

} // namespace tk
