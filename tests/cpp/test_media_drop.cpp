#include <catch2/catch_test_macros.hpp>

#include "views/ComposeBar.h"
#include "views/media_drop.h"

#include <string>
#include <vector>

using tesseract::views::ComposeBar;
using tesseract::views::route_file_drop_to_compose_bar;
using tesseract::views::FileDropOutcome;
using Kind = ComposeBar::PendingAttachment::Kind;

namespace
{
// Records the arguments of the per-shell media-info probe so tests can assert
// it ran (gif/webp/video/audio) or stayed idle (plain image / file).
struct ProbeSpy
{
    int calls = 0;
    std::uint32_t gen = 0;
    std::string mime;
    std::size_t bytes = 0;

    tesseract::views::MediaInfoExtractor fn()
    {
        return [this](std::uint32_t g, std::vector<std::uint8_t> b,
                      std::string m)
        {
            ++calls;
            gen = g;
            mime = std::move(m);
            bytes = b.size();
        };
    }
};
} // namespace

TEST_CASE("route_file_drop_to_compose_bar rejects an empty payload", "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    auto out = route_file_drop_to_compose_bar(bar, {}, "image/png", "x.png", 0, spy.fn());
    CHECK(out == FileDropOutcome::Empty);
    CHECK(bar.pending_for_test() == nullptr);
    CHECK(spy.calls == 0);
}

TEST_CASE("route_file_drop_to_compose_bar rejects a payload over the upload limit",
          "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    std::vector<std::uint8_t> bytes(100, 0x00);
    auto out =
        route_file_drop_to_compose_bar(bar, bytes, "image/png", "x.png", 50, spy.fn());
    CHECK(out == FileDropOutcome::TooLarge);
    CHECK(bar.pending_for_test() == nullptr);
    CHECK(spy.calls == 0);
}

TEST_CASE("route_file_drop_to_compose_bar with a zero limit imposes no ceiling",
          "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    std::vector<std::uint8_t> bytes(100, 0x00);
    auto out =
        route_file_drop_to_compose_bar(bar, bytes, "image/png", "x.png", 0, spy.fn());
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::Image);
}

TEST_CASE("route_file_drop_to_compose_bar queues a still image without probing",
          "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    auto out = route_file_drop_to_compose_bar(bar, {0x01, 0x02}, "image/png", "x.png", 0,
                                  spy.fn());
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::Image);
    CHECK(bar.pending_for_test()->is_animated == false);
    CHECK(spy.calls == 0); // plain images need no background probe
}

TEST_CASE("route_file_drop_to_compose_bar probes a gif for animation", "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    std::vector<std::uint8_t> bytes{0x47, 0x49, 0x46}; // "GIF"
    auto out =
        route_file_drop_to_compose_bar(bar, bytes, "image/gif", "a.gif", 0, spy.fn());
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::Image);
    REQUIRE(spy.calls == 1);
    CHECK(spy.mime == "image/gif");
    CHECK(spy.bytes == bytes.size());
    CHECK(spy.gen == bar.pending_gen()); // probe keyed to the queued generation
}

TEST_CASE("route_file_drop_to_compose_bar queues a video and probes it",
          "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    auto out = route_file_drop_to_compose_bar(bar, {0x00, 0x01}, "video/mp4", "c.mp4", 0,
                                  spy.fn());
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::Video);
    CHECK(spy.calls == 1);
    CHECK(spy.mime == "video/mp4");
}

TEST_CASE("route_file_drop_to_compose_bar queues audio and probes it", "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    auto out = route_file_drop_to_compose_bar(bar, {0x00, 0x01}, "audio/ogg", "s.ogg", 0,
                                  spy.fn());
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::Audio);
    CHECK(spy.calls == 1);
    CHECK(spy.mime == "audio/ogg");
}

TEST_CASE("route_file_drop_to_compose_bar queues a generic file without probing",
          "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    auto out = route_file_drop_to_compose_bar(bar, {0x25, 0x50}, "application/pdf",
                                  "doc.pdf", 0, spy.fn());
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::File);
    CHECK(spy.calls == 0);
}

TEST_CASE("route_file_drop_to_compose_bar tolerates a null probe", "[view][media_drop]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    // gif path would call the probe; null must be a safe no-op.
    auto out = route_file_drop_to_compose_bar(bar, {0x47, 0x49, 0x46}, "image/gif", "a.gif",
                                  0, nullptr);
    CHECK(out == FileDropOutcome::Accepted);
    REQUIRE(bar.pending_for_test() != nullptr);
    CHECK(bar.pending_for_test()->kind == Kind::Image);
}

TEST_CASE("route_file_drop_to_compose_bar dropping multiple files appends, "
          "not replaces",
          "[view][media_drop][gallery]")
{
    // Regression test for the pre-gallery bug: every platform's native drop
    // handler already loops over all dropped files and calls this router
    // once per file (see host_win32.cpp/host_macos.mm/host_gtk.cpp/
    // host_qt.cpp) — with the old set_pending_* (replace) semantics, only
    // the last dropped file survived. add_pending_* (append) fixes this for
    // all four shells with no shell-specific code.
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;

    CHECK(route_file_drop_to_compose_bar(bar, {0x01}, "image/png", "one.png", 0,
                                         spy.fn()) == FileDropOutcome::Accepted);
    CHECK(route_file_drop_to_compose_bar(bar, {0x02}, "image/png", "two.png", 0,
                                         spy.fn()) == FileDropOutcome::Accepted);
    CHECK(route_file_drop_to_compose_bar(bar, {0x03}, "application/pdf",
                                         "three.pdf", 0,
                                         spy.fn()) == FileDropOutcome::Accepted);

    REQUIRE(bar.pending_count() == 3);
    CHECK(bar.pending_for_test(0)->filename == "one.png");
    CHECK(bar.pending_for_test(1)->filename == "two.png");
    CHECK(bar.pending_for_test(2)->filename == "three.pdf");
}

TEST_CASE("route_file_drop_to_compose_bar rejects a drop once kMaxAttachments "
          "is queued",
          "[view][media_drop][gallery]")
{
    auto bar_owner = tk::create_root_widget<ComposeBar>(nullptr);
    ComposeBar& bar = *bar_owner;
    ProbeSpy spy;
    for (std::size_t i = 0; i < ComposeBar::kMaxAttachments; ++i)
    {
        REQUIRE(route_file_drop_to_compose_bar(bar, {0x00}, "application/pdf",
                                               "f.pdf", 0,
                                               spy.fn()) == FileDropOutcome::Accepted);
    }
    REQUIRE(bar.pending_count() == ComposeBar::kMaxAttachments);

    auto out = route_file_drop_to_compose_bar(bar, {0x00}, "application/pdf",
                                              "overflow.pdf", 0, spy.fn());
    CHECK(out == FileDropOutcome::TooMany);
    CHECK(bar.pending_count() == ComposeBar::kMaxAttachments);
}
