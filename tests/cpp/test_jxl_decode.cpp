#include <catch2/catch_test_macros.hpp>
#include <tesseract/jxl.h>

#include "app/jxl_decode.h"
#include "tk/anim_decode_session.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::vector<std::uint8_t> read_fixture(const char* name)
{
    std::ifstream f(std::string(TESSERACT_TEST_DATA_DIR) + "/jxl/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct JxlFakeImage : tk::Image
{
    JxlFakeImage(int w, int h, std::uint8_t p0) : w_(w), h_(h), p0_(p0)
    {
    }
    int width() const override
    {
        return w_;
    }
    int height() const override
    {
        return h_;
    }
    std::size_t memory_bytes() const noexcept override
    {
        return static_cast<std::size_t>(w_) * h_ * 4;
    }
    int w_, h_;
    std::uint8_t p0_; // first pixel's red byte
};

const tesseract::jxl::MakeImage kMake = [](const std::uint8_t* px, int w,
                                           int h) -> std::unique_ptr<tk::Image>
{
    return std::make_unique<JxlFakeImage>(w, h, px[0]);
};

} // namespace

TEST_CASE("JxlDecoder: still decodes to RGBA", "[jxl]")
{
    const auto bytes = read_fixture("still_rgb.jxl");
    REQUIRE(tesseract::is_jxl_data(bytes));
    tesseract::JxlDecoder d(bytes, 4096, 4096);
    REQUIRE(d.valid());
    CHECK(d.width() == 8);
    CHECK(d.height() == 4);
    CHECK(d.frame_count() == 1);
    int delay = -1;
    const std::uint8_t* px = d.next_frame(delay);
    REQUIRE(px != nullptr);
    CHECK(px[3] == 255);
    CHECK(d.next_frame(delay) == nullptr);
}

TEST_CASE("JxlDecoder: animation delivers frames then resets", "[jxl]")
{
    const auto bytes = read_fixture("anim3.jxl");
    tesseract::JxlDecoder d(bytes, 4096, 4096);
    REQUIRE(d.valid());
    int delay = 0;
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(d.next_frame(delay) != nullptr);
        CHECK(delay == 50);
    }
    CHECK_FALSE(d.has_more_frames());
    d.reset();
    CHECK(d.has_more_frames());
}

TEST_CASE("JxlDecoder: garbage is invalid", "[jxl]")
{
    const std::vector<std::uint8_t> junk{1, 2, 3};
    CHECK_FALSE(tesseract::is_jxl_data(junk));
    CHECK_FALSE(tesseract::JxlDecoder(junk, 100, 100).valid());
}

TEST_CASE("jxl::decode_image: still", "[jxl]")
{
    auto d = tesseract::jxl::decode_image(read_fixture("still_rgb.jxl"), 4096, 4096, kMake);
    REQUIRE(d.still);
    CHECK(d.frames.empty());
    CHECK(d.still->width() == 8);
}

TEST_CASE("jxl::decode_image: animation frames and delays", "[jxl]")
{
    auto d = tesseract::jxl::decode_image(read_fixture("anim3.jxl"), 4096, 4096, kMake);
    CHECK_FALSE(d.still);
    REQUIRE(d.frames.size() == 3);
    CHECK(d.delays_ms == std::vector<int>{50, 50, 50});
}

TEST_CASE("jxl::decode_image: delays are normalised", "[jxl]")
{
    auto d = tesseract::jxl::decode_image(read_fixture("anim_delays.jxl"), 4096, 4096, kMake);
    // The 0 ms source frame is a blend layer (JXL: duration 0 is not a keyframe), not a displayed frame.
    REQUIRE(d.frames.size() == 2);
    CHECK(d.delays_ms == std::vector<int>{20, 50}); // 5 ms is floored to 20
}

TEST_CASE("jxl::decode_image: frame cap", "[jxl]")
{
    auto d = tesseract::jxl::decode_image(read_fixture("anim250.jxl"), 4096, 4096, kMake);
    CHECK(d.frames.size() == static_cast<std::size_t>(tk::kAnimDecodeMaxFrames));
}

TEST_CASE("jxl::decode_image: bad input is empty", "[jxl]")
{
    const auto good = read_fixture("still_rgb.jxl");
    const std::vector<std::uint8_t> truncated(good.begin(), good.begin() + good.size() / 2);
    CHECK(
        tesseract::jxl::decode_image(std::vector<std::uint8_t>{1, 2, 3}, 100, 100, kMake).empty());
    CHECK(tesseract::jxl::decode_image(truncated, 100, 100, kMake).empty());
    // Header declaring 60000 x 60000: refused before any decode.
    const std::vector<std::uint8_t> huge{0xFF, 0x0A, 0xFE, 0x52, 0x07, 0x00, 0x02, 0, 0, 0, 0, 0};
    CHECK(tesseract::jxl::decode_image(huge, 100, 100, kMake).empty());
}

TEST_CASE("jxl::decode_streamed_windowed: short animation decodes whole", "[jxl]")
{
    int firsts = 0;
    std::vector<int> idx;
    const bool ok = tesseract::jxl::decode_streamed_windowed(
        read_fixture("anim3.jxl"), 4096, 4096, kMake,
        [&](std::unique_ptr<tk::Image>, int, std::shared_ptr<tk::AnimDecodeSession> s,
            std::size_t total)
        {
            ++firsts;
            CHECK(s == nullptr);
            CHECK(total == 0);
        },
        [&](int i, std::unique_ptr<tk::Image>, int)
        {
            idx.push_back(i);
        });
    CHECK(ok);
    CHECK(firsts == 1);
    CHECK(idx == std::vector<int>{1, 2});
}

TEST_CASE("jxl::decode_streamed_windowed: still returns false", "[jxl]")
{
    bool called = false;
    const bool ok = tesseract::jxl::decode_streamed_windowed(
        read_fixture("still_rgb.jxl"), 4096, 4096, kMake,
        [&](std::unique_ptr<tk::Image>, int, std::shared_ptr<tk::AnimDecodeSession>, std::size_t)
        {
            called = true;
        },
        [&](int, std::unique_ptr<tk::Image>, int)
        {
            called = true;
        });
    CHECK_FALSE(ok);
    CHECK_FALSE(called);
}

TEST_CASE("jxl::decode_streamed_windowed: long animation yields a session", "[jxl]")
{
    std::shared_ptr<tk::AnimDecodeSession> session;
    std::size_t total = 0;
    int delivered = 0;
    const bool ok = tesseract::jxl::decode_streamed_windowed(
        read_fixture("anim250.jxl"), 4096, 4096, kMake,
        [&](std::unique_ptr<tk::Image>, int, std::shared_ptr<tk::AnimDecodeSession> s,
            std::size_t t)
        {
            session = std::move(s);
            total = t;
            ++delivered;
        },
        [&](int, std::unique_ptr<tk::Image>, int)
        {
            ++delivered;
        });
    REQUIRE(ok);
    REQUIRE(session);
    CHECK(total == static_cast<std::size_t>(tk::kAnimDecodeMaxFrames));
    CHECK(delivered == tk::kAnimDecodeInitialBatchFrames);
    CHECK_FALSE(session->exhausted());

    int produced = 0;
    int last_idx = -1;
    while (!session->exhausted())
    {
        produced += session->decode_next_batch(50,
                                               [&](int i, std::unique_ptr<tk::Image>, int)
                                               {
                                                   last_idx = i;
                                               });
    }
    CHECK(produced == tk::kAnimDecodeMaxFrames - tk::kAnimDecodeInitialBatchFrames);
    CHECK(last_idx == tk::kAnimDecodeMaxFrames - 1);

    session->restart();
    CHECK_FALSE(session->exhausted());
    int first_idx = -1;
    session->decode_next_batch(1,
                               [&](int i, std::unique_ptr<tk::Image>, int)
                               {
                                   first_idx = i;
                               });
    CHECK(first_idx == 0);
}

TEST_CASE("jxl::decode_streamed: delivers every frame", "[jxl]")
{
    int firsts = 0, rest = 0;
    const bool ok = tesseract::jxl::decode_streamed(
        read_fixture("anim3.jxl"), 4096, 4096, kMake,
        [&](std::unique_ptr<tk::Image>, int)
        {
            ++firsts;
        },
        [&](int, std::unique_ptr<tk::Image>, int)
        {
            ++rest;
        });
    CHECK(ok);
    CHECK(firsts == 1);
    CHECK(rest == 2);
}
