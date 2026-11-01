#include <catch2/catch_test_macros.hpp>

#include "app/image_send.h"

#include <vector>

namespace image_send_test
{

using Bytes = std::vector<std::uint8_t>;

// Records calls; the "host" re-encodes to `compressed_size` bytes of JPEG and
// sniffs originals as `orig_mime` / 100x50.
struct FakeHost
{
    std::size_t compressed_size = 10;
    bool fail = false;
    int compress_calls = 0;
    int original_calls = 0;

    // Whether the SDK can strip the original (image_metadata_strippable).
    bool strippable = true;
    int strippable_calls = 0;
    tesseract::StrippableFn ok()
    {
        return [this](std::span<const std::uint8_t>, std::string_view)
        {
            ++strippable_calls;
            return strippable;
        };
    }

    tesseract::EncodeForSendFn fn()
    {
        return [this](const std::uint8_t* d, std::size_t n, bool compress) -> tk::EncodedImage
        {
            if (fail)
            {
                return {};
            }
            tk::EncodedImage e;
            if (compress)
            {
                ++compress_calls;
                e.bytes.assign(compressed_size, 0xAB);
                e.mime = "image/jpeg";
                e.width = 40;
                e.height = 20;
            }
            else
            {
                ++original_calls;
                e.bytes.assign(d, d + n);
                e.mime = "image/png";
                e.width = 100;
                e.height = 50;
            }
            return e;
        };
    }
};

// A 2-frame GIF so is_animated_image() says yes.
Bytes animated_gif()
{
    Bytes b{'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < 2; ++i)
    {
        for (std::uint8_t v : {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 0x01, 0})
        {
            b.push_back(v);
        }
    }
    b.push_back(0x3B);
    return b;
}

Bytes still(std::size_t n)
{
    return Bytes(n, 0x11);
}

} // namespace image_send_test

using namespace image_send_test;

TEST_CASE("prepare_image_for_send: smaller recompression wins", "[image-send]")
{
    FakeHost host;
    auto p = tesseract::prepare_image_for_send(still(1000), "image/png", "cat.png", 100, 50, false,
                                               true, host.fn(), host.ok());
    CHECK(p.bytes.size() == 10);
    CHECK(p.mime == "image/jpeg");
    CHECK(p.filename == "cat.jpg");
    CHECK(p.width == 40);
    CHECK(p.height == 20);
    CHECK_FALSE(p.is_animated);
    // A recompressed result carries no metadata, so the (UI-thread, full-strip)
    // strippable check is never needed.
    CHECK(host.strippable_calls == 0);
}

TEST_CASE("prepare_image_for_send: a recompression that is not smaller sends the original",
          "[image-send]")
{
    for (std::size_t compressed : {1000u, 1500u})
    {
        FakeHost host;
        host.compressed_size = compressed;
        auto p = tesseract::prepare_image_for_send(still(1000), "image/png", "cat.png", 7, 7, false,
                                                   true, host.fn(), host.ok());
        CHECK(p.bytes.size() == 1000);
        CHECK(p.mime == "image/png");
        CHECK(p.filename == "cat.png");
        CHECK(p.width == 100); // host-sniffed, oriented size
        CHECK(p.height == 50);
    }
}

TEST_CASE("prepare_image_for_send: recompression disabled sends the original", "[image-send]")
{
    FakeHost host;
    auto p = tesseract::prepare_image_for_send(still(1000), "image/png", "cat.png", 7, 7, false,
                                               false, host.fn(), host.ok());
    CHECK(host.compress_calls == 0);
    CHECK(p.bytes.size() == 1000);
    CHECK(p.filename == "cat.png");
}

TEST_CASE("prepare_image_for_send: animated images are never recompressed and are flagged",
          "[image-send]")
{
    FakeHost host;
    // Detected from the bytes even though the composer did not flag it.
    auto g = tesseract::prepare_image_for_send(animated_gif(), "image/gif", "a.gif", 1, 1, false,
                                               true, host.fn(), host.ok());
    CHECK(g.is_animated);
    CHECK(g.mime == "image/gif");
    CHECK(g.width == 1);
    CHECK(host.compress_calls == 0);
    CHECK(host.original_calls == 0); // animations are never decoded here

    // Composer flag alone is enough.
    auto f = tesseract::prepare_image_for_send(still(1000), "image/webp", "a.webp", 5, 6, true,
                                               true, host.fn(), host.ok());
    CHECK(f.is_animated);
    CHECK(f.bytes.size() == 1000);
    CHECK(f.mime == "image/webp");
    CHECK(host.compress_calls == 0);
}

TEST_CASE("prepare_image_for_send: an undecodable image is sent as-is", "[image-send]")
{
    FakeHost host;
    host.fail = true;
    auto p = tesseract::prepare_image_for_send(still(1000), "image/jxl", "a.jxl", 30, 20, false,
                                               true, host.fn(), host.ok());
    CHECK(p.bytes.size() == 1000);
    CHECK(p.mime == "image/jxl");
    CHECK(p.width == 30);
    CHECK(p.height == 20);
    CHECK(p.filename == "a.jxl");
}

TEST_CASE("prepare_image_for_send: an unstrippable still is always recompressed", "[image-send]")
{
    // Recompression switched off: the original would leak metadata, so the
    // privacy rule overrides the setting.
    {
        FakeHost host;
        host.strippable = false;
        auto p = tesseract::prepare_image_for_send(still(1000), "image/x-tga", "a.tga", 5, 5, false,
                                                   false, host.fn(), host.ok());
        CHECK_FALSE(p.refused);
        CHECK(p.mime == "image/jpeg");
        CHECK(p.filename == "a.jpg");
        CHECK(p.bytes.size() == 10);
    }
    // Recompressed result larger than the original: still used.
    {
        FakeHost host;
        host.strippable = false;
        host.compressed_size = 5000;
        auto p = tesseract::prepare_image_for_send(still(1000), "image/x-tga", "a.tga", 5, 5, false,
                                                   true, host.fn(), host.ok());
        CHECK(p.bytes.size() == 5000);
        CHECK(p.mime == "image/jpeg");
    }
}

TEST_CASE("prepare_image_for_send: unstrippable and undecodable is refused", "[image-send]")
{
    FakeHost host;
    host.strippable = false;
    host.fail = true;
    auto p = tesseract::prepare_image_for_send(still(1000), "image/x-raw", "a.raw", 5, 5, false,
                                               true, host.fn(), host.ok());
    CHECK(p.refused);
    CHECK(p.bytes.empty());
}

TEST_CASE(
    "prepare_image_for_send: a strippable original is kept when recompression is off or not smaller",
    "[image-send]")
{
    FakeHost host; // strippable = true
    auto off = tesseract::prepare_image_for_send(still(1000), "image/png", "a.png", 5, 5, false,
                                                 false, host.fn(), host.ok());
    CHECK(off.bytes.size() == 1000);
    CHECK_FALSE(off.refused);
}

TEST_CASE("prepare_image_for_send: an unstrippable animation is refused, a strippable one is sent",
          "[image-send]")
{
    FakeHost host;
    host.strippable = false;
    auto bad = tesseract::prepare_image_for_send(animated_gif(), "image/gif", "a.gif", 1, 1, false,
                                                 true, host.fn(), host.ok());
    CHECK(bad.refused);
    CHECK(bad.bytes.empty());
    CHECK(host.compress_calls == 0); // never flattened by recompression
    // The composer's own animated flag is not an exemption.
    auto flagged = tesseract::prepare_image_for_send(still(1000), "image/webp", "a.webp", 1, 1,
                                                     true, true, host.fn(), host.ok());
    CHECK(flagged.refused);

    host.strippable = true;
    auto ok = tesseract::prepare_image_for_send(animated_gif(), "image/gif", "a.gif", 1, 1, false,
                                                true, host.fn(), host.ok());
    CHECK_FALSE(ok.refused);
    CHECK(ok.is_animated);
    CHECK_FALSE(ok.bytes.empty());
}

TEST_CASE("prepare_image_for_send: empty input yields empty output", "[image-send]")
{
    FakeHost host;
    CHECK(tesseract::prepare_image_for_send({}, "image/png", "x.png", 0, 0, false, true, host.fn(),
                                            host.ok())
              .bytes.empty());
}

TEST_CASE("prepare_image_for_send: filename without extension gets .jpg appended", "[image-send]")
{
    FakeHost host;
    auto p = tesseract::prepare_image_for_send(still(1000), "image/png", "clipboard", 1, 1, false,
                                               true, host.fn(), host.ok());
    CHECK(p.filename == "clipboard.jpg");
}
