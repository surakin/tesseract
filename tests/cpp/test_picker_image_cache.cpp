#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct FakeImage : tk::Image
{
    int width() const override
    {
        return 1;
    }
    int height() const override
    {
        return 1;
    }
    std::size_t memory_bytes() const override
    {
        return 4;
    }
};

// Minimal concrete ShellBase. Implements every pure virtual; re-exposes
// the protected members the tests assert on.
struct WithAccountManager { tesseract::AccountManager am_; };

struct TestShell : WithAccountManager, ShellBase
{
    TestShell() : ShellBase(am_) {}

    void post_to_ui_(std::function<void()> fn) override
    {
        fn();
    }
    void post_to_ui_after_(int /*ms*/, std::function<void()> fn) override
    {
        fn();
    }
    void request_relayout_() override
    {
    }
    void request_repaint_() override
    {
    }
    void on_rooms_updated_() override
    {
    }
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override
    {
    }
    void on_tab_state_changed_ui_() override
    {
    }
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override
    {
        return 1000;
    }
    void start_anim_tick_() override
    {
        ++anim_tick_starts;
    }
    void repaint_pickers_() override
    {
        ++repaints;
    }
    void navigate_to_room_(const std::string&) override
    {
    }
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override
    {
    }
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override {}
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    void request_relogin_(const std::string&) override {}

    int anim_tick_starts = 0;
    int repaints = 0;

    // Accessors so tests can inspect the caches without reaching through
    // account_manager_ each time.
    tk::PixmapCache&    image_cache() { return am_.image_cache(); }
    tk::AnimImageCache& anim_cache()  { return am_.anim_cache(); }

    using ShellBase::finalize_picker_image_;
    // DecodedImage is protected in ShellBase; re-export so tests can name it.
    using ShellBase::DecodedImage;
    using ShellBase::emoji_fetches_in_flight_;
    using ShellBase::sticker_fetches_in_flight_;
};

TestShell::DecodedImage make_still()
{
    TestShell::DecodedImage d;
    d.still = std::make_unique<FakeImage>();
    return d;
}

TestShell::DecodedImage make_anim(int n)
{
    TestShell::DecodedImage d;
    for (int i = 0; i < n; ++i)
    {
        d.frames.push_back(std::make_unique<FakeImage>());
        d.delays_ms.push_back(50);
    }
    return d;
}

} // namespace

TEST_CASE("finalize routes a still image into image_cache_", "[picker-cache]")
{
    TestShell s;
    s.emoji_fetches_in_flight_.insert("mxc://e/1");
    s.finalize_picker_image_("mxc://e/1", /*is_sticker=*/false, make_still());

    CHECK(s.image_cache().contains("mxc://e/1"));
    CHECK(s.anim_cache().has("mxc://e/1") == false);
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/1") == 0);
    CHECK(s.repaints == 1);
    CHECK(s.anim_tick_starts == 0);
}

TEST_CASE("finalize routes animated frames into anim_cache_", "[picker-cache]")
{
    TestShell s;
    s.sticker_fetches_in_flight_.insert("mxc://s/1");
    s.finalize_picker_image_("mxc://s/1", /*is_sticker=*/true, make_anim(3));

    CHECK(s.anim_cache().has("mxc://s/1") == true);
    CHECK_FALSE(s.image_cache().contains("mxc://s/1"));
    CHECK(s.sticker_fetches_in_flight_.count("mxc://s/1") == 0);
    CHECK(s.anim_tick_starts == 1);
    CHECK(s.repaints == 1);
}

TEST_CASE("finalize does not overwrite an existing cache entry",
          "[picker-cache]")
{
    TestShell s;
    s.image_cache().store("mxc://e/2", std::make_unique<FakeImage>());
    const tk::Image* original = s.image_cache().peek("mxc://e/2");
    s.emoji_fetches_in_flight_.insert("mxc://e/2");

    s.finalize_picker_image_("mxc://e/2", false, make_still());

    CHECK(s.image_cache().peek("mxc://e/2") == original);       // unchanged
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/2") == 0); // still cleared
    CHECK(s.repaints == 0);
}

TEST_CASE("finalize with an empty decode result caches nothing",
          "[picker-cache]")
{
    TestShell s;
    s.emoji_fetches_in_flight_.insert("mxc://e/3");

    s.finalize_picker_image_("mxc://e/3", false, TestShell::DecodedImage{});

    CHECK_FALSE(s.image_cache().contains("mxc://e/3"));
    CHECK(s.anim_cache().has("mxc://e/3") == false);
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/3") == 0);
    CHECK(s.repaints == 0);
}
