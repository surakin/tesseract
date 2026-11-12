#include <catch2/catch_test_macros.hpp>

#include "tk/image_sniff.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace std::string_literals;

namespace
{

std::vector<std::uint8_t> bytes_of(const std::string& s)
{
    return {s.begin(), s.end()};
}

// RIFF....WEBPVP8X<size=10><flags><reserved...>
std::vector<std::uint8_t> webp_vp8x(std::uint8_t flags)
{
    std::string s;
    s.assign("RIFF\x1a\x00\x00\x00WEBPVP8X\x0a\x00\x00\x00", 20);
    s.push_back(static_cast<char>(flags));
    s.append(9, '\0');
    return bytes_of(s);
}

std::vector<std::uint8_t> png_with_chunks(const std::vector<std::string>& types)
{
    std::string s("\x89PNG\r\n\x1a\n", 8);
    for (const auto& t : types)
    {
        s.append("\x00\x00\x00\x04", 4); // 4 data bytes
        s += t;
        s.append(4, '\0'); // data
        s.append(4, '\0'); // crc
    }
    return bytes_of(s);
}

std::vector<std::uint8_t> read_fixture(const char* name)
{
    std::ifstream f(std::string(TESSERACT_TEST_DATA_DIR) + "/jxl/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// LSB-first bit packer for hand-built JXL size headers.
struct BitWriter
{
    std::vector<std::uint8_t> out;
    int nbits = 0;
    void put(std::uint64_t v, int n)
    {
        for (int i = 0; i < n; ++i, ++nbits)
        {
            if (nbits % 8 == 0)
            {
                out.push_back(0);
            }
            out.back() |= static_cast<std::uint8_t>(((v >> i) & 1u) << (nbits % 8));
        }
    }
};

// Bare codestream declaring a `side` x `side` image (div8=0, 1:1 ratio, U32 selector 3 = 30 bits).
std::vector<std::uint8_t> jxl_square_header(std::uint32_t side)
{
    BitWriter w;
    w.put(0, 1); // div8
    w.put(3, 2); // ysize selector: Bits(30)
    w.put(side - 1, 30);
    w.put(1, 3); // ratio 1:1
    std::vector<std::uint8_t> b{0xFF, 0x0A};
    b.insert(b.end(), w.out.begin(), w.out.end());
    b.resize(b.size() + 8, 0);
    return b;
}

void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    for (int s = 24; s >= 0; s -= 8)
    {
        v.push_back(static_cast<std::uint8_t>(x >> s));
    }
}

} // namespace

TEST_CASE("image sniff: JXL", "[image-sniff]")
{
    const auto still = read_fixture("still_rgb.jxl");
    REQUIRE_FALSE(still.empty());
    CHECK(tk::is_jxl_bytes(still));
    CHECK(tk::bytes_may_be_animated(still)); // only the decoder can tell
    const auto d = tk::probe_image_dimensions(still);
    REQUIRE(d.has_value());
    CHECK(d->width == 8);
    CHECK(d->height == 4);
    CHECK(tk::probe_image_dimensions(read_fixture("anim3.jxl"))->width == 16);
    CHECK_FALSE(tk::is_jxl_bytes(bytes_of("GIF89a")));
    CHECK_FALSE(tk::probe_image_dimensions(std::vector<std::uint8_t>{0xFF, 0x0A}).has_value());
}

TEST_CASE("image sniff: JXL in an ISOBMFF container", "[image-sniff]")
{
    const auto cs = read_fixture("still_rgb.jxl");
    std::vector<std::uint8_t> c;
    put_be32(c, 12);
    c.insert(c.end(), {'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A});
    put_be32(c, 20);
    c.insert(c.end(), {'f', 't', 'y', 'p', 'j', 'x', 'l', ' ', 0, 0, 0, 0, 'j', 'x', 'l', ' '});
    put_be32(c, static_cast<std::uint32_t>(8 + cs.size()));
    c.insert(c.end(), {'j', 'x', 'l', 'c'});
    c.insert(c.end(), cs.begin(), cs.end());
    CHECK(tk::is_jxl_bytes(c));
    const auto d = tk::probe_image_dimensions(c);
    REQUIRE(d.has_value());
    CHECK(d->width == 8);
    CHECK(d->height == 4);
}

TEST_CASE("decode_size_allowed: oversize JXL rejected", "[image-sniff]")
{
    CHECK_FALSE(tk::decode_size_allowed(jxl_square_header(60000), false)); // > 32768 side
    CHECK_FALSE(tk::decode_size_allowed(jxl_square_header(20000), false)); // 400 MP still
    CHECK(tk::decode_size_allowed(jxl_square_header(4000), false));
    CHECK_FALSE(tk::decode_size_allowed(jxl_square_header(5000), true)); // > 16 MP animated
    const auto d = tk::probe_image_dimensions(jxl_square_header(1234));
    REQUIRE(d.has_value());
    CHECK(d->width == 1234);
}

TEST_CASE("bytes_may_be_animated: GIF", "[image-sniff]")
{
    CHECK(tk::bytes_may_be_animated(bytes_of("GIF89a\x01\x00\x01\x00"s)));
    CHECK(tk::bytes_may_be_animated(bytes_of("GIF87a")));
}

TEST_CASE("bytes_may_be_animated: WebP honours the VP8X animation flag", "[image-sniff]")
{
    CHECK(tk::bytes_may_be_animated(webp_vp8x(0x02)));
    CHECK(tk::bytes_may_be_animated(webp_vp8x(0x12)));       // animation + alpha
    CHECK_FALSE(tk::bytes_may_be_animated(webp_vp8x(0x10))); // alpha only
    CHECK_FALSE(
        tk::bytes_may_be_animated(bytes_of(std::string("RIFF\x1a\x00\x00\x00WEBPVP8 ", 16))));
}

TEST_CASE("bytes_may_be_animated: APNG needs acTL before IDAT", "[image-sniff]")
{
    CHECK(tk::bytes_may_be_animated(png_with_chunks({"IHDR", "acTL", "IDAT", "IEND"})));
    CHECK_FALSE(tk::bytes_may_be_animated(png_with_chunks({"IHDR", "IDAT", "IEND"})));
    // acTL after the image data does not make it an APNG.
    CHECK_FALSE(tk::bytes_may_be_animated(png_with_chunks({"IHDR", "IDAT", "acTL"})));
}

TEST_CASE("bytes_may_be_animated: other and truncated input is false", "[image-sniff]")
{
    CHECK_FALSE(tk::bytes_may_be_animated({}));
    CHECK_FALSE(tk::bytes_may_be_animated(bytes_of("\xff\xd8\xff\xe0JFIF")));
    CHECK_FALSE(tk::bytes_may_be_animated(bytes_of("GI")));
    CHECK_FALSE(tk::bytes_may_be_animated(bytes_of("RIFF")));
    CHECK_FALSE(tk::bytes_may_be_animated(bytes_of(std::string("\x89PNG\r\n\x1a\n", 8))));
}

namespace
{

std::string be32(std::uint32_t v)
{
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
            static_cast<char>(v)};
}

std::string le16(std::uint32_t v)
{
    return {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
}

std::string le24(std::uint32_t v)
{
    return {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
            static_cast<char>((v >> 16) & 0xff)};
}

std::vector<std::uint8_t> png_ihdr(std::uint32_t w, std::uint32_t h)
{
    std::string s("\x89PNG\r\n\x1a\n", 8);
    s += be32(13) + "IHDR" + be32(w) + be32(h);
    s.append(5, '\0'); // depth, colour type, compression, filter, interlace
    s.append(4, '\0'); // crc
    return bytes_of(s);
}

std::vector<std::uint8_t> gif_screen(std::uint32_t w, std::uint32_t h)
{
    std::string s = "GIF89a" + le16(w) + le16(h);
    s.append(3, '\0');
    return bytes_of(s);
}

std::vector<std::uint8_t> webp_vp8x_canvas(std::uint32_t w, std::uint32_t h)
{
    std::string s("RIFF\x00\x00\x00\x00WEBPVP8X\x0a\x00\x00\x00", 20);
    s.push_back('\x02');            // flags: animation
    s.append(3, '\0');              // reserved
    s += le24(w - 1) + le24(h - 1); // canvas size minus one
    return bytes_of(s);
}

std::vector<std::uint8_t> webp_vp8l(std::uint32_t w, std::uint32_t h)
{
    std::string s("RIFF\x00\x00\x00\x00WEBPVP8L\x00\x00\x00\x00", 20);
    s.push_back('\x2f'); // VP8L signature
    const std::uint32_t bits = (w - 1) | ((h - 1) << 14);
    s += {static_cast<char>(bits & 0xff), static_cast<char>((bits >> 8) & 0xff),
          static_cast<char>((bits >> 16) & 0xff), static_cast<char>((bits >> 24) & 0xff)};
    s.append(8, '\0');
    return bytes_of(s);
}

std::vector<std::uint8_t> webp_vp8(std::uint32_t w, std::uint32_t h)
{
    std::string s("RIFF\x00\x00\x00\x00WEBPVP8 \x00\x00\x00\x00", 20);
    s.append(3, '\0'); // frame tag
    s += std::string("\x9d\x01\x2a", 3);
    s += le16(w) + le16(h);
    s.append(4, '\0');
    return bytes_of(s);
}

// SOI, a large APP1 (EXIF-sized), DQT, then SOFn with the given size.
std::vector<std::uint8_t> jpeg(std::uint8_t sof, std::uint32_t w, std::uint32_t h)
{
    std::string s("\xff\xd8", 2);
    const std::size_t app1_len = 60000;
    s += std::string("\xff\xe1", 2) +
         std::string{static_cast<char>(app1_len >> 8), static_cast<char>(app1_len & 0xff)};
    s.append(app1_len - 2, 'E');
    s += std::string("\xff\xdb\x00\x43", 4);
    s.append(0x43 - 2, '\x01');
    s += std::string{'\xff', static_cast<char>(sof), '\x00', '\x11', '\x08'};
    s += std::string{static_cast<char>(h >> 8), static_cast<char>(h & 0xff),
                     static_cast<char>(w >> 8), static_cast<char>(w & 0xff)};
    s.append(10, '\0');
    return bytes_of(s);
}

} // namespace

TEST_CASE("probe_image_dimensions: PNG IHDR")
{
    auto d = tk::probe_image_dimensions(png_ihdr(1920, 1080));
    REQUIRE(d);
    CHECK(d->width == 1920);
    CHECK(d->height == 1080);
}

TEST_CASE("probe_image_dimensions: GIF logical screen")
{
    auto d = tk::probe_image_dimensions(gif_screen(320, 240));
    REQUIRE(d);
    CHECK(d->width == 320);
    CHECK(d->height == 240);
}

TEST_CASE("probe_image_dimensions: WebP VP8X, VP8L and VP8")
{
    auto x = tk::probe_image_dimensions(webp_vp8x_canvas(16383, 200));
    REQUIRE(x);
    CHECK(x->width == 16383);
    CHECK(x->height == 200);
    auto l = tk::probe_image_dimensions(webp_vp8l(640, 480));
    REQUIRE(l);
    CHECK(l->width == 640);
    CHECK(l->height == 480);
    auto v = tk::probe_image_dimensions(webp_vp8(800, 600));
    REQUIRE(v);
    CHECK(v->width == 800);
    CHECK(v->height == 600);
}

TEST_CASE("probe_image_dimensions: JPEG SOF after large APP1 and DQT")
{
    auto base = tk::probe_image_dimensions(jpeg(0xC0, 8064, 6048));
    REQUIRE(base);
    CHECK(base->width == 8064);
    CHECK(base->height == 6048);
    auto prog = tk::probe_image_dimensions(jpeg(0xC2, 4000, 3000));
    REQUIRE(prog);
    CHECK(prog->width == 4000);
    CHECK(prog->height == 3000);
}

TEST_CASE("probe_image_dimensions: unknown or truncated input")
{
    CHECK_FALSE(tk::probe_image_dimensions({}));
    CHECK_FALSE(tk::probe_image_dimensions(bytes_of("BM6\x00\x00\x00"s)));
    auto png = png_ihdr(10, 10);
    png.resize(20);
    CHECK_FALSE(tk::probe_image_dimensions(png));
    auto j = jpeg(0xC0, 10, 10);
    j.resize(100); // cut inside APP1, before SOF
    CHECK_FALSE(tk::probe_image_dimensions(j));
}

TEST_CASE("decode_size_allowed: budgets")
{
    CHECK(tk::decode_size_allowed(png_ihdr(8064, 6048), false));
    CHECK_FALSE(tk::decode_size_allowed(png_ihdr(10001, 10001), false));
    CHECK_FALSE(tk::decode_size_allowed(png_ihdr(40000, 10), false));
    CHECK(tk::decode_size_allowed(gif_screen(4096, 4096), true));
    CHECK_FALSE(tk::decode_size_allowed(gif_screen(5000, 5000), true));
    CHECK(tk::decode_size_allowed(gif_screen(5000, 5000), false));
    CHECK_FALSE(tk::decode_size_allowed(webp_vp8x_canvas(16383, 16383), true));
    CHECK(tk::decode_size_allowed(bytes_of("not an image"), false));
}

TEST_CASE("decode_dims_allowed: decoder-reported sizes use the still/animated budget")
{
    CHECK(tk::decode_dims_allowed(8064, 6048, false));
    CHECK_FALSE(tk::decode_dims_allowed(10001, 10001, false));
    CHECK_FALSE(tk::decode_dims_allowed(40000, 10, false));
    CHECK_FALSE(tk::decode_dims_allowed(32000, 32000, false));
    CHECK(tk::decode_dims_allowed(4096, 4096, true));
    CHECK_FALSE(tk::decode_dims_allowed(5000, 5000, true));
    CHECK(tk::decode_dims_allowed(5000, 5000, false));
    CHECK_FALSE(tk::decode_dims_allowed(0, 100, false));
    CHECK_FALSE(tk::decode_dims_allowed(100, 0, true));
    CHECK_FALSE(tk::decode_dims_allowed(UINT32_MAX, UINT32_MAX, false));
    CHECK_FALSE(tk::decode_dims_allowed(1ull << 40, 1, false));
    CHECK(tk::decode_dims_allowed(tk::kMaxDecodeSide, 1, false));
    CHECK_FALSE(tk::decode_dims_allowed(tk::kMaxDecodeSide + 1ull, 1, false));
}

TEST_CASE("decode_size_allowed and decode_dims_allowed agree on probed sizes")
{
    const std::pair<std::uint32_t, std::uint32_t> sizes[] = {
        {8064, 6048}, {10001, 10001}, {4096, 4096}, {5000, 5000}};
    for (const auto& [w, h] : sizes)
    {
        for (const bool anim : {false, true})
        {
            CAPTURE(w, h, anim);
            CHECK(tk::decode_size_allowed(png_ihdr(w, h), anim) ==
                  tk::decode_dims_allowed(w, h, anim));
            CHECK(tk::decode_size_allowed(gif_screen(w, h), anim) ==
                  tk::decode_dims_allowed(w, h, anim));
        }
    }
}

TEST_CASE("decode_size_allowed: JPEG stills get the larger budget where it is DCT-scaled")
{
    // Linux only (see image_sniff.h): elsewhere a JPEG has the still budget.
    if (tk::kMaxJpegStillDecodePixels > tk::kMaxStillDecodePixels)
    {
        CHECK(tk::decode_size_allowed(jpeg(0xC0, 16320, 12240), false));
        CHECK(tk::decode_size_allowed(jpeg(0xC0, 11648, 8736), false));
    }
    else
    {
        CHECK_FALSE(tk::decode_size_allowed(jpeg(0xC0, 16320, 12240), false));
        CHECK(tk::decode_size_allowed(jpeg(0xC0, 8064, 6048), false));
    }
    CHECK_FALSE(tk::decode_size_allowed(jpeg(0xC0, 20000, 20000), false));
    CHECK_FALSE(tk::decode_size_allowed(png_ihdr(16320, 12240), false));
    CHECK_FALSE(tk::decode_size_allowed(jpeg(0xC0, 16320, 12240), true));
}

TEST_CASE("probe_image_dimensions: junk byte before SOF does not hide the size")
{
    auto j = jpeg(0xC0, 8064, 6048);
    // SOF follows SOI(2) + APP1(2+60000) + DQT(2+0x43).
    const std::size_t sof_at = 2 + 2 + 60000 + 2 + 0x43;
    REQUIRE(j[sof_at] == 0xFF);
    REQUIRE(j[sof_at + 1] == 0xC0);
    j.insert(j.begin() + static_cast<std::ptrdiff_t>(sof_at), std::uint8_t{0x00});
    auto d = tk::probe_image_dimensions(j);
    REQUIRE(d);
    CHECK(d->width == 8064);
    CHECK(d->height == 6048);

    auto big = jpeg(0xC0, 20000, 20000);
    big.insert(big.begin() + static_cast<std::ptrdiff_t>(sof_at), std::uint8_t{0x00});
    CHECK_FALSE(tk::decode_size_allowed(big, false));
}

TEST_CASE("sniff_image_mime: labels a file by its signature, not its name", "[image-sniff]")
{
    CHECK(tk::sniff_image_mime(bytes_of("\x89PNG\r\n\x1a\n\0\0\0\rIHDR"s)) == "image/png");
    CHECK(tk::sniff_image_mime(bytes_of("\xFF\xD8\xFF\xE0\0\x10JFIF"s)) == "image/jpeg");
    CHECK(tk::sniff_image_mime(bytes_of("GIF89a\x01\0\x01\0"s)) == "image/gif");
    CHECK(tk::sniff_image_mime(webp_vp8x(0)) == "image/webp");
}

TEST_CASE("sniff_image_mime: anything else is refused, not labelled", "[image-sniff]")
{
    CHECK(tk::sniff_image_mime(bytes_of("<html>not an image</html>")).empty());
    // A JPEG XL file is not in the picker's set, so it is refused too.
    CHECK(tk::sniff_image_mime(bytes_of("\xFF\x0A\x00\x00"s)).empty());
    CHECK(tk::sniff_image_mime(bytes_of("\x89PN")).empty()); // truncated signature
    CHECK(tk::sniff_image_mime({}).empty());
}
