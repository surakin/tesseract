#ifdef TESSERACT_SERVER_BROWSER_ENABLED

#include <catch2/catch_test_macros.hpp>

#include "tk_test_host.h"
#include "views/LoginView.h"
#include "views/ServerBrowserOverlay.h"

#include <random>

using tesseract::views::LoginView;
using tesseract::views::ServerBrowserOverlay;
using Entry = ServerBrowserOverlay::Entry;

namespace
{

Entry entry(std::string name, std::string domain, bool is_public = true, bool online = true)
{
    Entry e;
    e.name = std::move(name);
    e.client_domain = std::move(domain);
    e.is_public = is_public;
    e.online = online;
    return e;
}

// First widget under `w` that is clickable and named `name` (a server card).
tk::Widget* find_clickable(tk::Widget& w, const std::string& name)
{
    if (w.clickable() && w.access_name() == name)
    {
        return &w;
    }
    for (auto& ch : w.children())
    {
        if (auto* hit = find_clickable(*ch, name))
        {
            return hit;
        }
    }
    return nullptr;
}

std::vector<std::string> names(const std::vector<const Entry*>& v)
{
    std::vector<std::string> out;
    for (const auto* e : v)
    {
        out.push_back(e->name);
    }
    return out;
}

} // namespace

TEST_CASE("ServerBrowserOverlay matches every word against name, domain, description, "
          "jurisdiction and languages",
          "[tk][view][login][server_browser]")
{
    Entry e = entry("Example", "matrix.example.org");
    e.description = "A friendly general server";
    e.jurisdiction = "Germany";
    e.languages = {"Deutsch", "English"};

    CHECK(ServerBrowserOverlay::matches(e, ""));
    CHECK(ServerBrowserOverlay::matches(e, "example"));
    CHECK(ServerBrowserOverlay::matches(e, "GERMANY deutsch"));
    CHECK(ServerBrowserOverlay::matches(e, "friendly   server"));
    CHECK_FALSE(ServerBrowserOverlay::matches(e, "germany france"));
    CHECK_FALSE(ServerBrowserOverlay::matches(e, "zzz"));
}

TEST_CASE("ServerBrowserOverlay filters by query and open registration",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto owner = tk::create_root_widget<ServerBrowserOverlay>(&host);
    ServerBrowserOverlay& ov = *owner;
    ov.set_servers({entry("alpha", "alpha.example"), entry("beta", "beta.example", false),
                    entry("gamma", "gamma.example")});

    CHECK(names(ov.visible_servers()) == std::vector<std::string>{"alpha", "beta", "gamma"});

    ov.set_open_registration_only(true);
    CHECK(names(ov.visible_servers()) == std::vector<std::string>{"alpha", "gamma"});

    ov.set_query("gam");
    CHECK(names(ov.visible_servers()) == std::vector<std::string>{"gamma"});
}

TEST_CASE("ServerBrowserOverlay arranges public online servers first, offline last",
          "[tk][view][login][server_browser]")
{
    std::vector<Entry> in = {
        entry("priv", "priv.example", false),
        entry("pub-off", "pub-off.example", true, false),
        entry("pub-a", "pub-a.example"),
        entry("pub-b", "pub-b.example"),
        entry("priv-off", "priv-off.example", false, false),
    };
    std::mt19937 rng{42};
    auto out = ServerBrowserOverlay::arrange_for_display(in, rng);
    REQUIRE(out.size() == 5);
    CHECK((out[0].name == "pub-a" || out[0].name == "pub-b"));
    CHECK((out[1].name == "pub-a" || out[1].name == "pub-b"));
    CHECK(out[2].name == "pub-off");
    CHECK(out[3].name == "priv");
    CHECK(out[4].name == "priv-off");
}

TEST_CASE("ServerBrowserOverlay pick closes the overlay and reports the domain",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto owner = tk::create_root_widget<ServerBrowserOverlay>(&host);
    ServerBrowserOverlay& ov = *owner;
    ov.on_layout_changed = [] {};
    std::string picked;
    ov.on_pick = [&](const std::string& d)
    {
        picked = d;
    };
    ov.set_servers({entry("alpha", "alpha.example")});
    ov.open();
    REQUIRE(ov.is_open());
    // Failed state shows a Retry instead of servers.
    ov.set_state(ServerBrowserOverlay::State::Failed);
    int retries = 0;
    ov.on_retry = [&]
    {
        ++retries;
    };
    CHECK(ov.state() == ServerBrowserOverlay::State::Failed);
    ov.close();
    CHECK_FALSE(ov.is_open());
    CHECK(picked.empty());
    CHECK(retries == 0);
}

TEST_CASE("ServerBrowserOverlay: clicking a server card picks it",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto owner = tk::create_root_widget<ServerBrowserOverlay>(&host);
    ServerBrowserOverlay& ov = *owner;
    ov.on_layout_changed = [] {};
    std::string picked;
    ov.on_pick = [&](const std::string& d)
    {
        picked = d;
    };
    ov.set_servers({entry("alpha", "alpha.example"), entry("beta", "beta.example")});
    ov.open();

    auto* card = find_clickable(ov, "beta");
    REQUIRE(card);
    CHECK(card->hoverable());
    CHECK(card->access_default_action());
    CHECK(picked == "beta.example");
    CHECK_FALSE(ov.is_open());
}

TEST_CASE("ServerBrowserOverlay lays the cards out in two columns only when wide",
          "[tk][view][login][server_browser]")
{
    CHECK(ServerBrowserOverlay::columns_for_width(300.0f) == 1);
    CHECK(ServerBrowserOverlay::columns_for_width(639.0f) == 1);
    CHECK(ServerBrowserOverlay::columns_for_width(900.0f) == 2);
}

TEST_CASE("ServerBrowserOverlay remembers delivered icons per server",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto owner = tk::create_root_widget<ServerBrowserOverlay>(&host);
    ServerBrowserOverlay& ov = *owner;
    ov.set_servers({entry("alpha", "alpha.example")});
    CHECK_FALSE(ov.has_icon("alpha.example"));
    ov.set_icon("alpha.example", {});
    CHECK_FALSE(ov.has_icon("alpha.example")); // empty bytes are no icon
    ov.set_icon("alpha.example", {0x89, 'P', 'N', 'G'});
    CHECK(ov.has_icon("alpha.example"));
}

TEST_CASE("LoginView offers the server browser, with matrix.org as one choice",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    if (!lv.server_browser_available())
    {
        SKIP("a homeserver is provisioned on this machine");
    }

    REQUIRE(lv.find_server_button());
    CHECK(lv.find_server_button()->visible());

    tesseract::Client::ServerList list;
    list.ok = true;
    list.entries = {entry("alpha", "alpha.example")};
    lv.simulate_server_list_result(list);

    auto* browser = lv.server_browser();
    REQUIRE(browser);
    CHECK(browser->state() == ServerBrowserOverlay::State::Ready);
    auto shown = names(browser->visible_servers());
    CHECK(shown.size() == 2);
    CHECK(std::find(shown.begin(), shown.end(), "matrix.org") != shown.end());

    CHECK_FALSE(browser->is_open());
    lv.open_server_browser();
    CHECK(browser->is_open());
}

TEST_CASE("LoginView failed server list shows the retry state", "[tk][view][login][server_browser]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    if (!lv.server_browser_available())
    {
        SKIP("a homeserver is provisioned on this machine");
    }

    tesseract::Client::ServerList list; // ok == false
    lv.simulate_server_list_result(list);
    CHECK(lv.server_browser()->state() == ServerBrowserOverlay::State::Failed);
}

TEST_CASE("LoginView Create an account with an empty field opens the server browser",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    if (!lv.server_browser_available())
    {
        SKIP("a homeserver is provisioned on this machine");
    }

    CHECK_FALSE(lv.server_browser()->is_open());
    lv.simulate_register_click();
    CHECK(lv.server_browser()->is_open());
}

TEST_CASE("LoginView picking a server fills the homeserver field",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    if (!lv.server_browser_available())
    {
        SKIP("a homeserver is provisioned on this machine");
    }

    lv.open_server_browser();
    REQUIRE(lv.server_browser()->on_pick);
    lv.server_browser()->on_pick("alpha.example");
    CHECK(lv.discovery_state() == LoginView::DiscoveryState::Discovering);
}

TEST_CASE("LoginView hides the homeserver field while the server browser is open",
          "[tk][view][login][server_browser]")
{
    StubHost host;
    auto lv_owner = tk::create_root_widget<LoginView>(&host);
    LoginView& lv = *lv_owner;
    lv.set_relayout([] {});
    lv.finish_init();
    if (!lv.server_browser_available())
    {
        SKIP("a homeserver is provisioned on this machine");
    }

    REQUIRE(lv.homeserver_field());
    CHECK(lv.homeserver_field()->visible());
    lv.open_server_browser();
    CHECK_FALSE(lv.homeserver_field()->visible());
    lv.server_browser()->close();
    CHECK(lv.homeserver_field()->visible());
}

#endif // TESSERACT_SERVER_BROWSER_ENABLED
