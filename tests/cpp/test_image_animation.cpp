#include <catch2/catch_test_macros.hpp>

#include "views/image_animation.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace anim_test
{

using Bytes = std::vector<std::uint8_t>;

Bytes anim_fixture(const char* name)
{
    std::ifstream f(std::string(TESSERACT_TEST_DATA_DIR) + "/jxl/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void put(Bytes& b, std::initializer_list<int> v)
{
    for (int x : v)
        b.push_back(static_cast<std::uint8_t>(x));
}

// Minimal GIF: 1x1 canvas, optional global colour table, `frames` image blocks
// (each with an optional graphic-control extension), trailer.
Bytes gif(int frames, bool gct = true, bool lct = false, bool gce = true)
{
    Bytes b;
    put(b, {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, gct ? 0x80 : 0x00, 0, 0});
    if (gct)
        put(b, {0, 0, 0, 255, 255, 255}); // 2 colours
    for (int i = 0; i < frames; ++i)
    {
        if (gce)
            put(b, {0x21, 0xF9, 4, 0, 10, 0, 0, 0});
        put(b, {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, lct ? 0x80 : 0x00});
        if (lct)
            put(b, {0, 0, 0, 255, 255, 255});
        put(b, {2, 2, 0x44, 0x01, 0}); // LZW min code size + one data sub-block + terminator
    }
    put(b, {0x3B});
    return b;
}

Bytes webp_vp8x(std::uint8_t flags)
{
    Bytes b;
    put(b, {'R', 'I', 'F', 'F', 0x1a, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X', 10, 0, 0, 0, flags});
    b.resize(b.size() + 9, 0);
    return b;
}

Bytes png(const std::vector<std::pair<std::string, Bytes>>& chunks)
{
    Bytes b{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    for (const auto& [type, data] : chunks)
    {
        const auto n = static_cast<std::uint32_t>(data.size());
        put(b, {int(n >> 24), int((n >> 16) & 255), int((n >> 8) & 255), int(n & 255)});
        b.insert(b.end(), type.begin(), type.end());
        b.insert(b.end(), data.begin(), data.end());
        put(b, {0, 0, 0, 0}); // crc
    }
    return b;
}

Bytes actl(std::uint8_t frames) { return Bytes{0, 0, 0, frames, 0, 0, 0, 0}; }

Bytes ftyp(const std::string& major, const std::string& compat = "")
{
    Bytes b;
    const auto size = static_cast<int>(16 + compat.size());
    put(b, {0, 0, 0, size, 'f', 't', 'y', 'p'});
    b.insert(b.end(), major.begin(), major.end());
    put(b, {0, 0, 0, 0});
    b.insert(b.end(), compat.begin(), compat.end());
    return b;
}

} // namespace anim_test


TEST_CASE("is_animated_image: GIF counts frames", "[animation]")
{
    CHECK(tesseract::is_animated_image(anim_test::gif(2)));
    CHECK(tesseract::is_animated_image(anim_test::gif(5, true, true)));   // local colour tables
    CHECK(tesseract::is_animated_image(anim_test::gif(3, false, false, false)));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::gif(1)));         // single-frame GIF
    CHECK_FALSE(tesseract::is_animated_image(anim_test::gif(0)));
    auto truncated = anim_test::gif(3);
    truncated.resize(truncated.size() - 12); // cut inside the last frame; two seen already
    CHECK(tesseract::is_animated_image(truncated));
    anim_test::Bytes cut = anim_test::gif(2);
    cut.resize(20);
    CHECK_FALSE(tesseract::is_animated_image(cut));
}

TEST_CASE("is_animated_image: WebP honours the VP8X animation flag", "[animation]")
{
    CHECK(tesseract::is_animated_image(anim_test::webp_vp8x(0x02)));
    CHECK(tesseract::is_animated_image(anim_test::webp_vp8x(0x12)));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::webp_vp8x(0x10)));
}

TEST_CASE("is_animated_image: APNG needs acTL with more than one frame", "[animation]")
{
    CHECK(tesseract::is_animated_image(anim_test::png({{"IHDR", anim_test::Bytes(13)}, {"acTL", anim_test::actl(3)}, {"IDAT", {}}, {"IEND", {}}})));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::png({{"IHDR", anim_test::Bytes(13)}, {"acTL", anim_test::actl(1)}, {"IDAT", {}}, {"IEND", {}}})));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::png({{"IHDR", anim_test::Bytes(13)}, {"IDAT", {}}, {"IEND", {}}})));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::png({{"IHDR", anim_test::Bytes(13)}, {"IDAT", {}}, {"acTL", anim_test::actl(3)}, {"IEND", {}}})));
}

TEST_CASE("is_animated_image: AVIF sequence brand", "[animation]")
{
    CHECK(tesseract::is_animated_image(anim_test::ftyp("avis")));
    CHECK(tesseract::is_animated_image(anim_test::ftyp("avif", "mif1avis")));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::ftyp("avif", "mif1miaf")));
}

TEST_CASE("is_animated_image: JPEG XL header animation", "[animation]")
{
    CHECK(tesseract::is_animated_image(anim_test::anim_fixture("anim3.jxl")));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::anim_fixture("still_rgb.jxl")));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::anim_fixture("still_container.jxl")));
}

TEST_CASE("is_animated_image: other and unrecognised data is false", "[animation]")
{
    CHECK_FALSE(tesseract::is_animated_image(anim_test::Bytes{0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10}));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::Bytes{}));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::Bytes{'G', 'I', 'F'}));
    CHECK_FALSE(tesseract::is_animated_image(anim_test::Bytes(64, 0)));
}

TEST_CASE("is_animated_image: mime is only a pre-filter", "[animation]")
{
    CHECK(tesseract::is_animated_image(anim_test::gif(2), "image/gif"));
    CHECK(tesseract::is_animated_image(anim_test::gif(2), "application/octet-stream"));
    CHECK(tesseract::is_animated_image(anim_test::gif(2), "")); // magic decides
    CHECK_FALSE(tesseract::is_animated_image(anim_test::gif(2), "video/mp4"));
    CHECK(tesseract::is_animated_image(anim_test::gif(2), "image/png")); // wrong mime: magic still wins
}
