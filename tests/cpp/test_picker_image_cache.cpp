#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace {

struct FakeImage : tk::Image {
    int width()  const override { return 1; }
    int height() const override { return 1; }
};

// Minimal concrete ShellBase. Implements every pure virtual; re-exposes
// the protected members the tests assert on.
struct TestShell : ShellBase {
    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&,
                               int, int) override { return {}; }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override { ++anim_tick_starts; }
    void repaint_pickers_() override { ++repaints; }

    int anim_tick_starts = 0;
    int repaints         = 0;

    using ShellBase::finalize_picker_image_;
    using ShellBase::DecodedImage;
    using ShellBase::tk_images_;
    using ShellBase::anim_cache_;
    using ShellBase::emoji_fetches_in_flight_;
    using ShellBase::sticker_fetches_in_flight_;
};

TestShell::DecodedImage make_still() {
    TestShell::DecodedImage d;
    d.still = std::make_unique<FakeImage>();
    return d;
}

TestShell::DecodedImage make_anim(int n) {
    TestShell::DecodedImage d;
    for (int i = 0; i < n; ++i) {
        d.frames.push_back(std::make_unique<FakeImage>());
        d.delays_ms.push_back(50);
    }
    return d;
}

} // namespace

TEST_CASE("finalize routes a still image into tk_images_", "[picker-cache]") {
    TestShell s;
    s.emoji_fetches_in_flight_.insert("mxc://e/1");
    s.finalize_picker_image_("mxc://e/1", /*is_sticker=*/false, make_still());

    CHECK(s.tk_images_.count("mxc://e/1") == 1);
    CHECK(s.anim_cache_.has("mxc://e/1") == false);
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/1") == 0);
    CHECK(s.repaints == 1);
    CHECK(s.anim_tick_starts == 0);
}

TEST_CASE("finalize routes animated frames into anim_cache_", "[picker-cache]") {
    TestShell s;
    s.sticker_fetches_in_flight_.insert("mxc://s/1");
    s.finalize_picker_image_("mxc://s/1", /*is_sticker=*/true, make_anim(3));

    CHECK(s.anim_cache_.has("mxc://s/1") == true);
    CHECK(s.tk_images_.count("mxc://s/1") == 0);
    CHECK(s.sticker_fetches_in_flight_.count("mxc://s/1") == 0);
    CHECK(s.anim_tick_starts == 1);
    CHECK(s.repaints == 1);
}

TEST_CASE("finalize does not overwrite an existing cache entry",
          "[picker-cache]") {
    TestShell s;
    s.tk_images_.emplace("mxc://e/2", std::make_unique<FakeImage>());
    const tk::Image* original = s.tk_images_.at("mxc://e/2").get();
    s.emoji_fetches_in_flight_.insert("mxc://e/2");

    s.finalize_picker_image_("mxc://e/2", false, make_still());

    CHECK(s.tk_images_.at("mxc://e/2").get() == original);   // unchanged
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/2") == 0); // still cleared
    CHECK(s.repaints == 0);
}

TEST_CASE("finalize with an empty decode result caches nothing",
          "[picker-cache]") {
    TestShell s;
    s.emoji_fetches_in_flight_.insert("mxc://e/3");

    s.finalize_picker_image_("mxc://e/3", false,
                             TestShell::DecodedImage{});

    CHECK(s.tk_images_.count("mxc://e/3") == 0);
    CHECK(s.anim_cache_.has("mxc://e/3") == false);
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/3") == 0);
    CHECK(s.repaints == 0);
}
