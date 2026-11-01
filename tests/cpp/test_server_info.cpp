#include <catch2/catch_test_macros.hpp>
#include "ffi_convert.h"
#include "tesseract/client.h"
#include "views/settings/ServerSection.h"
#include "tk_test_surface.h"
#include <memory>
#include <string>

using tesseract::ServerInfo;

namespace
{

struct ServerInfoStage
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
};

} // namespace

namespace
{

// An `ok` ServerInfoFfi with the capability bools at their permissive values,
// as the Rust side reports a server that doesn't restrict anything.
tesseract_ffi::ServerInfoFfi ok_info()
{
    tesseract_ffi::ServerInfoFfi f{};
    f.ok = true;
    f.can_change_password = true;
    f.can_set_displayname = true;
    f.can_set_avatar = true;
    f.profile_fields_enabled = true;
    f.preview_url_enabled = true;
    return f;
}

void add_version(tesseract_ffi::ServerInfoFfi& f, std::uint32_t major, std::uint32_t minor)
{
    f.spec_versions.push_back({major, minor});
}

} // namespace

TEST_CASE("from_ffi(ServerInfoFfi): full blob", "[server][info]")
{
    auto f = ok_info();
    f.homeserver_url = "https://matrix.org";
    add_version(f, 1, 1);
    add_version(f, 1, 2);
    add_version(f, 1, 3);
    f.supports_msc3030 = true;
    f.can_change_password = false;
    f.default_room_version = "10";

    ServerInfo info = tesseract::from_ffi(f);
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

TEST_CASE("from_ffi(ServerInfoFfi): msc3030 false without flag or v1.6", "[server][info]")
{
    auto f = ok_info();
    add_version(f, 1, 5);
    ServerInfo info = tesseract::from_ffi(f);
    CHECK(info.supports_msc3030 == false);
    CHECK(info.supports_profile_fields == false);
    CHECK(info.profile_fields_enabled == true);
    CHECK(info.preview_url_enabled == true);
    CHECK(info.default_room_version.empty());
}

TEST_CASE("from_ffi(ServerInfoFfi): preview_url_enabled (MSC4452)", "[server][info]")
{
    auto f = ok_info();
    f.preview_url_enabled = false;
    CHECK(tesseract::from_ffi(f).preview_url_enabled == false);
    f.preview_url_enabled = true;
    CHECK(tesseract::from_ffi(f).preview_url_enabled == true);
}

TEST_CASE("from_ffi(ServerInfoFfi): !ok returns default-constructed", "[server][info]")
{
    tesseract_ffi::ServerInfoFfi f{}; // ok == false, every bool false
    f.homeserver_url = "ignored";
    ServerInfo info = tesseract::from_ffi(f);
    CHECK(info.homeserver_url.empty());
    CHECK(info.spec_versions.empty());
    CHECK(info.supports_msc3030 == false);
    CHECK(info.can_change_password == true);
    CHECK(info.profile_fields_enabled == true);
}

TEST_CASE("from_ffi(ServerInfoFfi): spec_versions >= v1.6 enables MSC3030", "[server][info]")
{
    auto f = ok_info();
    add_version(f, 1, 4);
    add_version(f, 1, 5);
    add_version(f, 1, 6);
    ServerInfo info = tesseract::from_ffi(f);
    REQUIRE(info.spec_versions.size() == 3);
    CHECK(info.spec_versions[0].minor == 4);
    CHECK(info.spec_versions[1].minor == 5);
    CHECK(info.spec_versions[2].minor == 6);
    // v1.6 in the list should enable MSC3030 even without the unstable flag
    CHECK(info.supports_msc3030 == true);
}

TEST_CASE("from_ffi(ServerInfoFfi): profile field flags pass through", "[server][info]")
{
    auto f = ok_info();
    f.supports_profile_fields = true;
    f.profile_fields_enabled = false;
    ServerInfo info = tesseract::from_ffi(f);
    CHECK(info.supports_profile_fields == true);
    CHECK(info.profile_fields_enabled == false);
}

TEST_CASE("from_ffi(ServerInfoFfi): software, upload limit, login flows, feature flags",
          "[server][info]")
{
    auto f = ok_info();
    f.server_name = "Synapse";
    f.server_version = "1.120.0";
    f.upload_size_limit = 52428800;
    f.login_flows.push_back("password");
    f.login_flows.push_back("sso");
    f.profile_fields_stable = true;
    f.supports_invite_reason = true;
    f.supports_mutual_rooms = true;

    ServerInfo info = tesseract::from_ffi(f);
    CHECK(info.server_name == "Synapse");
    CHECK(info.server_version == "1.120.0");
    CHECK(info.upload_size_limit == 52428800);
    REQUIRE(info.login_flows.size() == 2);
    CHECK(info.login_flows[1] == "sso");
    CHECK(info.profile_fields_stable == true);
    CHECK(info.supports_invite_reason == true);
    CHECK(info.supports_mutual_rooms == true);
}

namespace
{
const tesseract::views::ServerInfoRow*
find_row(const std::vector<tesseract::views::ServerInfoRow>& rows, const std::string& label)
{
    for (const auto& r : rows)
    {
        if (r.label == label)
        {
            return &r;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("build_server_info_rows: server group omits unknown data", "[server][ui]")
{
    ServerInfo info;
    info.homeserver_url = "https://matrix.org";
    auto rows = tesseract::views::build_server_info_rows(info);
    REQUIRE(rows.server.size() == 1);
    CHECK(rows.server[0].value == "https://matrix.org");
    CHECK(find_row(rows.server, "Software") == nullptr);
    CHECK(find_row(rows.server, "Matrix versions") == nullptr);
    CHECK(find_row(rows.server, "Maximum upload size") == nullptr);
    CHECK(find_row(rows.server, "Login methods") == nullptr);
}

TEST_CASE("build_server_info_rows: server group fills from info", "[server][ui]")
{
    ServerInfo info;
    info.homeserver_url = "https://matrix.org";
    info.server_name = "Synapse";
    info.server_version = "1.120.0";
    info.spec_versions = {{1, 12}, {1, 1}, {1, 5}};
    info.upload_size_limit = 1024;
    info.login_flows = {"sso", "password", "m.login.unknown"};

    auto rows = tesseract::views::build_server_info_rows(info);
    REQUIRE(find_row(rows.server, "Software"));
    CHECK(find_row(rows.server, "Software")->value == "Synapse 1.120.0");
    REQUIRE(find_row(rows.server, "Matrix versions"));
    CHECK(find_row(rows.server, "Matrix versions")->value == "v1.1 – v1.12");
    CHECK(find_row(rows.server, "Maximum upload size") != nullptr);
    REQUIRE(find_row(rows.server, "Login methods"));
    // Unknown flows are dropped; known ones keep the server's order.
    CHECK(find_row(rows.server, "Login methods")->value == "Single sign-on, Password");
}

TEST_CASE("build_server_info_rows: feature stability", "[server][ui]")
{
    ServerInfo info;
    info.homeserver_url = "https://example.org";
    info.spec_versions = {{1, 6}};
    info.supports_msc3030 = true;
    info.supports_profile_fields = true;
    info.profile_fields_stable = false;
    info.supports_qr_grant = true;
    info.supports_invite_reason = false;
    info.supports_calls = true;

    auto rows = tesseract::views::build_server_info_rows(info);
    CHECK(find_row(rows.features, "Jump to date")->value == "Stable");
    CHECK(find_row(rows.features, "Profile fields")->value == "Experimental");
    CHECK(find_row(rows.features, "QR code login")->value == "Experimental");
    CHECK(find_row(rows.features, "Invite reasons")->value == "Not supported");
    CHECK(find_row(rows.features, "Invite reasons")->dim);
    CHECK(find_row(rows.features, "Voice and video calls")->value == "Experimental");

    info.profile_fields_stable = true;
    rows = tesseract::views::build_server_info_rows(info);
    CHECK(find_row(rows.features, "Profile fields")->value == "Stable");

    info.profile_fields_enabled = false;
    rows = tesseract::views::build_server_info_rows(info);
    CHECK(find_row(rows.features, "Profile fields")->value == "Disabled by server");

    // MSC3030 advertised on a pre-1.6 server is experimental.
    info.spec_versions = {{1, 5}};
    rows = tesseract::views::build_server_info_rows(info);
    CHECK(find_row(rows.features, "Jump to date")->value == "Experimental");
}

TEST_CASE("build_server_info_rows: capabilities", "[server][ui]")
{
    ServerInfo info;
    info.homeserver_url = "https://example.org";
    info.can_change_password = false;
    info.default_room_version = "11";

    auto rows = tesseract::views::build_server_info_rows(info);
    CHECK(find_row(rows.capabilities, "Change password")->value == "Not allowed");
    CHECK(find_row(rows.capabilities, "Change display name")->value == "Allowed");
    CHECK(find_row(rows.capabilities, "Default room version")->value == "11");
}

TEST_CASE("ServerSection: empty before set_server_info", "[server][ui]")
{
    ServerInfoStage st;
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
    ServerInfoStage st;
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
