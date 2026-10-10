#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/KnockRequestsPanel.h"

#include <tesseract/types.h>

#include <string>
#include <vector>

using tesseract::views::KnockRequestsPanel;

namespace
{

tesseract::KnockRequestInfo knock(const std::string& uid, const std::string& name,
                                  const std::string& reason = "")
{
    tesseract::KnockRequestInfo k;
    k.room_id = "!r:x";
    k.user_id = uid;
    k.display_name = name;
    k.reason = reason;
    k.avatar_url = "mxc://x/" + uid;
    return k;
}

struct KnockStage : vt::Stage
{
    std::unique_ptr<KnockRequestsPanel> panel =
        tk::create_root_widget<KnockRequestsPanel>(&host);
    void run() { mount(*panel, {0, 0, 800, 600}); }
};

} // namespace

TEST_CASE("KnockRequestsPanel starts closed and hidden; open/close toggles",
          "[tk][view][knock]")
{
    KnockStage s;
    CHECK_FALSE(s.panel->is_open());
    CHECK_FALSE(s.panel->visible());
    s.panel->open("!r:x");
    CHECK(s.panel->is_open());
    CHECK(s.panel->visible());
    CHECK(s.panel->room_id() == "!r:x");
    s.panel->set_requests({knock("@a:x", "Alice")});
    s.panel->close();
    CHECK_FALSE(s.panel->is_open());
    CHECK(s.panel->room_id().empty());
    CHECK_FALSE(s.panel->visible());
}

TEST_CASE("KnockRequestsPanel exposes count in its name and an empty "
          "description",
          "[tk][view][knock][accessibility]")
{
    KnockStage s;
    s.panel->open("!r:x");
    CHECK(s.panel->access_role() == tk::Role::Dialog);
    CHECK(s.panel->access_description() == "No pending requests");
    s.panel->set_requests({knock("@a:x", "Alice"), knock("@b:x", "")});
    CHECK(s.panel->access_name().find("2") != std::string::npos);
    CHECK(s.panel->access_description().empty());
    s.run();
    // Row groups: name falls back to user id; description to the reason.
    CHECK(vt::has_name(*s.panel, "Alice"));
    CHECK(vt::has_name(*s.panel, "@b:x"));
}

TEST_CASE("KnockRequestsPanel row buttons fire per-user callbacks",
          "[tk][view][knock]")
{
    KnockStage s;
    s.panel->open("!r:x");
    s.panel->set_can_ban(true);
    s.panel->set_requests({knock("@a:x", "Alice", "let me in"), knock("@b:x", "Bob")});
    s.run();

    std::vector<std::string> accepted, declined, banned;
    s.panel->on_accept = [&](std::string u) { accepted.push_back(u); };
    s.panel->on_decline = [&](std::string u) { declined.push_back(u); };
    s.panel->on_decline_and_ban = [&](std::string u, std::string r)
    {
        banned.push_back(u + "|" + r);
    };

    auto accepts = vt::nodes_named(*s.panel, "Accept");
    REQUIRE(accepts.size() == 2);
    CHECK(tk::invoke_default_action(accepts[1]));
    auto denies = vt::nodes_named(*s.panel, "Deny");
    REQUIRE(denies.size() == 2);
    CHECK(tk::invoke_default_action(denies[0]));
    auto bans = vt::nodes_named(*s.panel, "Deny & Ban");
    REQUIRE(bans.size() == 2);
    CHECK(tk::invoke_default_action(bans[0]));

    CHECK(accepted == std::vector<std::string>{"@b:x"});
    CHECK(declined == std::vector<std::string>{"@a:x"});
    CHECK(banned == std::vector<std::string>{"@a:x|"});
}

TEST_CASE("KnockRequestsPanel hides Deny & Ban without ban permission",
          "[tk][view][knock]")
{
    KnockStage s;
    s.panel->open("!r:x");
    int layout_changed = 0;
    s.panel->on_layout_changed = [&] { ++layout_changed; };
    s.panel->set_requests({knock("@a:x", "Alice")});
    s.run();
    CHECK(vt::nodes_named(*s.panel, "Deny & Ban").empty());
    s.panel->set_can_ban(true);
    s.run();
    CHECK(vt::nodes_named(*s.panel, "Deny & Ban").size() == 1);
    const int before = layout_changed;
    s.panel->set_can_ban(true); // unchanged: no rebuild
    CHECK(layout_changed == before);
    CHECK(layout_changed >= 2);
}

TEST_CASE("KnockRequestsPanel close button and backdrop click close it",
          "[tk][view][knock]")
{
    KnockStage s;
    s.panel->open("!r:x");
    s.run();
    int closed = 0;
    s.panel->on_close = [&] { ++closed; };

    REQUIRE(vt::press(*s.panel, "Close"));
    CHECK(closed == 1);

    // Backdrop (left of the 280px panel) press+release closes.
    CHECK(s.panel->on_pointer_down({10, 300}));
    s.panel->on_pointer_up({10, 300}, true);
    CHECK(closed == 2);
    // A click inside the panel is consumed but does not close.
    CHECK(s.panel->on_pointer_down({700, 300}));
    s.panel->on_pointer_up({700, 300}, true);
    CHECK(closed == 2);
    // Release outside after pressing the backdrop: no close.
    s.panel->on_pointer_down({10, 300});
    s.panel->on_pointer_up({10, 300}, false);
    CHECK(closed == 2);
}

TEST_CASE("KnockRequestsPanel input is ignored while closed; wheel scrolls "
          "and clamps",
          "[tk][view][knock]")
{
    KnockStage s;
    s.run();
    CHECK_FALSE(s.panel->on_pointer_down({10, 10}));
    CHECK_FALSE(s.panel->on_wheel({0, 0}, 0, 1));

    s.panel->open("!r:x");
    std::vector<tesseract::KnockRequestInfo> many;
    for (int i = 0; i < 12; ++i)
        many.push_back(knock("@u" + std::to_string(i) + ":x", "User " + std::to_string(i)));
    s.panel->set_requests(many);
    s.run();
    CHECK(s.panel->on_wheel({0, 0}, 0, 5));
    s.run();
    CHECK(s.panel->on_wheel({0, 0}, 0, -50)); // clamps at the top
    s.run();
    s.panel->on_theme_changed(tk::Theme::dark());
    s.panel->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.run();
}
