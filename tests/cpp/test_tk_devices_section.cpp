#include <catch2/catch_test_macros.hpp>

#include "views/settings/DevicesSection.h"
#include "views/settings/SettingBinder.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::views::DevicesSection;

namespace
{

struct TkDevicesSectionStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(), tk::Theme::light()};
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
    tesseract::Client::DeviceVerification verif = tesseract::Client::DeviceVerification::Unknown)
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

TEST_CASE("DevicesSection: loading state paints without crash", "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_loading(true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: empty list paints without crash", "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices({});
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: populated list with mixed states paints without crash",
          "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_current_device_id("DEVCURR");
    sec.set_devices({
        make_device("DEVCURR", "My Phone", true, tesseract::Client::DeviceVerification::Verified),
        make_device("DEVOLD", "Old Laptop", false,
                    tesseract::Client::DeviceVerification::Unverified),
        make_device("DEVUNK", "", false),
    });
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: set_current_device_id flips is_current on cached rows",
          "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices({
        make_device("DEVA", "A", false),
        make_device("DEVB", "B", true),
    });
    // Switch the current marker; the section should re-render without crashing.
    sec.set_current_device_id("DEVA");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: enter and clear UIA state transitions paint", "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices({
        make_device("DEVA", "Other", false),
    });
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});

    sec.enter_uia_state(
        "DEVA", "https://hs/_matrix/client/v3/auth/m.login.sso/fallback/web?session=xyz", "xyz");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});

    sec.clear_uia_state("DEVA");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: per-device busy + error state paint without crash", "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices({
        make_device("DEVA", "Phone", false),
    });
    sec.set_device_busy("DEVA", true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});

    sec.set_device_busy("DEVA", false);
    sec.set_device_error("DEVA", "Network error");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

TEST_CASE("DevicesSection: state setters on unknown device id are no-ops", "[devices][section]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices({});
    // None of these should crash even though no row matches the id.
    sec.set_device_busy("NOPE", true);
    sec.set_device_error("NOPE", "x");
    sec.enter_uia_state("NOPE", "https://h/fallback", "s");
    sec.clear_uia_state("NOPE");
    st.run(sec, {0.0f, 0.0f, 640.0f, 480.0f});
}

namespace
{

std::vector<tesseract::Client::Device> many_devices(int n)
{
    std::vector<tesseract::Client::Device> devs;
    for (int i = 0; i < n; ++i)
    {
        devs.push_back(
            make_device("DEV" + std::to_string(i), "Device " + std::to_string(i), false));
    }
    return devs;
}

tk::Point list_center(const DevicesSection& sec)
{
    const tk::Rect r = sec.list_bounds_for_testing();
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

} // namespace

TEST_CASE("DevicesSection: device list height is capped at half the viewport",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(20));
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    CHECK(sec.list_bounds_for_testing().h == 400.0f);
    CHECK(sec.list_content_height_for_testing() > 400.0f);
}

TEST_CASE("DevicesSection: device list keeps a three-row minimum in a short viewport",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(20));
    st.run(sec, {0.0f, 0.0f, 640.0f, 240.0f});

    // Half of 240 is below the minimum, so the minimum wins.
    CHECK(sec.list_bounds_for_testing().h == sec.list_max_height_for_testing());
    CHECK(sec.list_bounds_for_testing().h > 120.0f);
    CHECK(sec.list_bounds_for_testing().h >= 3 * 64.0f);
}

TEST_CASE("DevicesSection: a short device list is not padded to the cap",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(1));
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    CHECK(sec.list_bounds_for_testing().h == sec.list_content_height_for_testing());
    CHECK(sec.list_bounds_for_testing().h < 400.0f);
}

TEST_CASE("DevicesSection: wheel over the device list scrolls the list, not the page",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(20));
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    REQUIRE(sec.dispatch_wheel(list_center(sec), 0.0f, 60.0f) == true);
    CHECK(sec.list_scroll_y_for_testing() > 0.0f);
    CHECK(sec.scroll_y_for_testing() == 0.0f);
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f}); // re-layout and paint at the new offset

    // Scroll to the bottom; the list consumes while it can move.
    REQUIRE(sec.dispatch_wheel(list_center(sec), 0.0f, 1e6f) == true);
    CHECK(sec.list_scroll_y_for_testing() ==
          sec.list_content_height_for_testing() - sec.list_bounds_for_testing().h);
}

TEST_CASE("DevicesSection: wheel at the list edge falls through to the page",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(20));
    // Short viewport: the page overflows as well as the list.
    st.run(sec, {0.0f, 0.0f, 640.0f, 300.0f});
    REQUIRE(sec.content_height_for_testing() > 300.0f);

    REQUIRE(sec.dispatch_wheel(list_center(sec), 0.0f, 1e6f) == true); // list to bottom
    const float list_bottom = sec.list_scroll_y_for_testing();
    REQUIRE(sec.scroll_y_for_testing() == 0.0f);

    // At the list's bottom edge the wheel is not swallowed: the page scrolls.
    REQUIRE(sec.dispatch_wheel(list_center(sec), 0.0f, 60.0f) == true);
    CHECK(sec.list_scroll_y_for_testing() == list_bottom);
    CHECK(sec.scroll_y_for_testing() > 0.0f);
}

TEST_CASE("DevicesSection: wheel over the list is not consumed when nothing can scroll",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices({
        make_device("DEVA", "One", false),
        make_device("DEVB", "Two", false),
    });
    // Generous viewport: neither the list nor the page overflows.
    st.run(sec, {0.0f, 0.0f, 640.0f, 2000.0f});

    CHECK(sec.dispatch_wheel(list_center(sec), 0.0f, 120.0f) == false);
    CHECK(sec.dispatch_wheel(list_center(sec), 0.0f, -120.0f) == false);
    // The page-level handler also reports that it has nothing to scroll.
    CHECK(sec.on_wheel({10.0f, 10.0f}, 0.0f, 120.0f) == false);
}

TEST_CASE("DevicesSection: fewer devices re-clamps the list scroll offset",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(20));
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});
    REQUIRE(sec.dispatch_wheel(list_center(sec), 0.0f, 1e6f) == true);
    REQUIRE(sec.list_scroll_y_for_testing() > 0.0f);

    sec.set_devices(many_devices(2));
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    CHECK(sec.list_scroll_y_for_testing() == 0.0f);
    CHECK(sec.list_bounds_for_testing().h == sec.list_content_height_for_testing());
}

TEST_CASE("DevicesSection: busy and UIA rows paint inside the scrolled list",
          "[devices][section][scroll]")
{
    TkDevicesSectionStage st;
    tesseract::views::SettingBinder binder;
    DevicesSection sec{binder};
    sec.set_devices(many_devices(20));
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    sec.set_device_busy("DEV1", true);
    sec.set_device_error("DEV2", "Network error");
    sec.enter_uia_state("DEV0", "https://hs/_matrix/client/v3/auth/m.login.sso/fallback/web", "s");
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});

    REQUIRE(sec.dispatch_wheel(list_center(sec), 0.0f, 200.0f) == true);
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});
    sec.clear_uia_state("DEV0");
    st.run(sec, {0.0f, 0.0f, 640.0f, 800.0f});
}

TEST_CASE("url_destination_host names the host a sign-in page opens on", "[devices][uia]")
{
    using tesseract::views::url_destination_host;
    CHECK(url_destination_host("https://matrix.example.org/_matrix/client/r0/login") ==
          "matrix.example.org");
    CHECK(url_destination_host("https://matrix.example.org:8448/auth?x=1") ==
          "matrix.example.org:8448");
    CHECK(url_destination_host("https://user@evil.example/#/x") == "evil.example");
    CHECK(url_destination_host("https://example.org") == "example.org");
    CHECK(url_destination_host("example.org/path").empty());
    CHECK(url_destination_host("").empty());
}
