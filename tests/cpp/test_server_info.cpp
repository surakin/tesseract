#include <catch2/catch_test_macros.hpp>
#include "tesseract/client.h"
#include "views/settings/ServerSection.h"
#include "tk_test_surface.h"
#include <memory>
#include <string>

using tesseract::ServerInfo;

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
};

} // namespace

TEST_CASE("ServerInfo::from_json: full blob", "[server][info]")
{
    const std::string json = R"({
        "homeserver": "https://matrix.org",
        "spec_versions": ["v1.1", "v1.2", "v1.3"],
        "supports_msc3030": true,
        "can_change_password": false,
        "can_set_displayname": true,
        "can_set_avatar": true,
        "default_room_version": "10"
    })";

    ServerInfo info = ServerInfo::from_json(json);
    CHECK(info.homeserver_url == "https://matrix.org");
    REQUIRE(info.spec_versions.size() == 3);
    CHECK(info.spec_versions[0].major == 1);
    CHECK(info.spec_versions[0].minor == 1);
    CHECK(info.spec_versions[2].major == 1);
    CHECK(info.spec_versions[2].minor == 3);
    CHECK(info.supports_msc3030 == true);
    CHECK(info.can_change_password == false);
    CHECK(info.can_set_displayname == true);
    CHECK(info.can_set_avatar == true);
    CHECK(info.default_room_version == "10");
}

TEST_CASE("ServerInfo::from_json: missing caps default to true, msc3030 defaults false",
          "[server][info]")
{
    const std::string json = R"({"homeserver":"https://example.com","spec_versions":[]})";
    ServerInfo info = ServerInfo::from_json(json);
    CHECK(info.supports_msc3030 == false);
    CHECK(info.can_change_password == true);
    CHECK(info.can_set_displayname == true);
    CHECK(info.can_set_avatar == true);
    CHECK(info.default_room_version.empty());
}

TEST_CASE("ServerInfo::from_json: empty string returns default-constructed", "[server][info]")
{
    ServerInfo info = ServerInfo::from_json("");
    CHECK(info.homeserver_url.empty());
    CHECK(info.spec_versions.empty());
    CHECK(info.supports_msc3030 == false);
    CHECK(info.can_change_password == true);
}

TEST_CASE("ServerInfo::from_json: spec_versions array", "[server][info]")
{
    const std::string json =
        R"({"spec_versions":["v1.4","v1.5","v1.6"],"homeserver":"h"})";
    ServerInfo info = ServerInfo::from_json(json);
    REQUIRE(info.spec_versions.size() == 3);
    CHECK(info.spec_versions[0].major == 1);
    CHECK(info.spec_versions[0].minor == 4);
    CHECK(info.spec_versions[1].major == 1);
    CHECK(info.spec_versions[1].minor == 5);
    CHECK(info.spec_versions[2].major == 1);
    CHECK(info.spec_versions[2].minor == 6);
    // v1.6 in the list should enable MSC3030 even without the unstable flag
    CHECK(info.supports_msc3030 == true);
}

TEST_CASE("ServerSection: empty before set_server_info", "[server][ui]")
{
    Stage st;
    tesseract::views::ServerSection sec;
    auto lc = st.layout_ctx();
    auto sz = sec.measure(lc, {640.0f, 480.0f});
    CHECK(sz.h == 0.0f);
    sec.arrange(lc, {0.0f, 0.0f, sz.w, sz.h});
    auto pc1 = st.paint_ctx();
    sec.paint(pc1); // must not crash
}

TEST_CASE("ServerSection: non-zero height and paints after set_server_info", "[server][ui]")
{
    Stage st;
    tesseract::views::ServerSection sec;

    tesseract::ServerInfo info;
    info.homeserver_url = "https://matrix.org";
    sec.set_server_info(info);

    auto lc = st.layout_ctx();
    auto sz = sec.measure(lc, {640.0f, 480.0f});
    CHECK(sz.h > 0.0f);

    sec.arrange(lc, {0.0f, 0.0f, sz.w, sz.h});
    auto pc2 = st.paint_ctx();
    sec.paint(pc2); // must not crash
}
