#include <catch2/catch_test_macros.hpp>

#include "views/settings/DevicesSection.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::views::DevicesSection;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

tesseract::Client::Device make_device(
    std::string id, std::string name, bool is_current,
    tesseract::Client::DeviceVerification verif =
        tesseract::Client::DeviceVerification::Unknown)
{
    tesseract::Client::Device d;
    d.id = std::move(id);
    d.display_name = std::move(name);
    d.last_seen_ip = "127.0.0.1";
    d.last_seen_ts = 0;
    d.verification = verif;
    d.is_current = is_current;
    return d;
}

} // namespace

TEST_CASE("DevicesSection: loading state paints without crash",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_loading(true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: empty list paints without crash",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_devices({});
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: populated list with mixed states paints without crash",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_current_device_id("DEVCURR");
    sec.set_devices({
        make_device("DEVCURR", "My Phone", true,
                    tesseract::Client::DeviceVerification::Verified),
        make_device("DEVOLD", "Old Laptop", false,
                    tesseract::Client::DeviceVerification::Unverified),
        make_device("DEVUNK", "", false),
    });
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: set_current_device_id flips is_current on cached rows",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_devices({
        make_device("DEVA", "A", false),
        make_device("DEVB", "B", true),
    });
    // Switch the current marker; the section should re-render without crashing.
    sec.set_current_device_id("DEVA");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: enter and clear UIA state transitions paint",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_devices({
        make_device("DEVA", "Other", false),
    });
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});

    sec.enter_uia_state("DEVA", "https://hs/_matrix/client/v3/auth/m.login.sso/fallback/web?session=xyz",
                        "xyz");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});

    sec.clear_uia_state("DEVA");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: per-device busy + error state paint without crash",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_devices({
        make_device("DEVA", "Phone", false),
    });
    sec.set_device_busy("DEVA", true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});

    sec.set_device_busy("DEVA", false);
    sec.set_device_error("DEVA", "Network error");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: state setters on unknown device id are no-ops",
          "[devices][section]")
{
    Stage st;
    DevicesSection sec;
    sec.set_devices({});
    // None of these should crash even though no row matches the id.
    sec.set_device_busy("NOPE", true);
    sec.set_device_error("NOPE", "x");
    sec.enter_uia_state("NOPE", "https://h/fallback", "s");
    sec.clear_uia_state("NOPE");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: wheel scrolls when content exceeds viewport",
          "[devices][section][scroll]")
{
    Stage st;
    DevicesSection sec;
    // Stuff in enough rows to overflow a deliberately-short viewport. Each
    // row is ~64 px + spacing; 20 rows comfortably exceed 240.
    std::vector<tesseract::Client::Device> devs;
    for (int i = 0; i < 20; ++i)
    {
        devs.push_back(
            make_device("DEV" + std::to_string(i),
                        "Device " + std::to_string(i), false));
    }
    sec.set_devices(std::move(devs));
    // Tiny viewport so the content overflows.
    st.run(sec, {0.0f, 0.0f, 640.0f, 240.0f});

    INFO("content_height = " << sec.content_height_for_testing()
                              << ", viewport = 240");
    REQUIRE(sec.content_height_for_testing() > 240.0f);

    // First wheel down should consume the event and move scroll_y_ forward.
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, 60.0f) == true);
    // Repaint with the new scroll position — must not crash.
    st.run(sec, {0.0f, 0.0f, 640.0f, 240.0f});

    // Scrolling further must still consume and not jump past the bottom.
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, 1e6f) == true);
    // Now we are pinned at max_scroll; a further wheel-down is a no-op.
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, 60.0f) == false);
    // Scrolling back up consumes again.
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, -1e6f) == true);
    // Pinned at top now.
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, -60.0f) == false);
}

TEST_CASE("DevicesSection: wheel is a no-op when content fits the viewport",
          "[devices][section][scroll]")
{
    Stage st;
    DevicesSection sec;
    sec.set_devices({
        make_device("DEVA", "One", false),
        make_device("DEVB", "Two", false),
    });
    // Generous viewport — content trivially fits.
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    // Wheel returns false → event bubbles up so a parent scroll container
    // can pick it up.
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, 120.0f) == false);
    REQUIRE(sec.on_wheel({10.0f, 10.0f}, 0.0f, -120.0f) == false);
}
