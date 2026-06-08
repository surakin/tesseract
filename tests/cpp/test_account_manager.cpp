#include <catch2/catch_test_macros.hpp>
#include "app/AccountManager.h"
#include <tesseract/account_session.h>

using tesseract::AccountManager;
using tesseract::AccountSession;

namespace
{
std::shared_ptr<AccountSession> make_session(std::string user_id,
                                              std::string display_name = "")
{
    auto s          = std::make_shared<AccountSession>();
    s->user_id      = std::move(user_id);
    s->display_name = std::move(display_name);
    return s;
}
} // namespace

TEST_CASE("AccountManager - add and find", "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org", "Alice"));

    auto found = mgr.find("@alice:example.org");
    REQUIRE(found != nullptr);
    CHECK(found->display_name == "Alice");
}

TEST_CASE("AccountManager - find unknown returns nullptr", "[account_manager]")
{
    AccountManager mgr;
    CHECK(mgr.find("@nobody:example.org") == nullptr);
}

TEST_CASE("AccountManager - remove account", "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org"));
    mgr.remove_account("@alice:example.org");

    CHECK(mgr.find("@alice:example.org") == nullptr);
    CHECK(mgr.accounts().empty());
}

TEST_CASE("AccountManager - accounts span contains all added sessions",
          "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org"));
    mgr.add_account(make_session("@bob:matrix.org"));

    REQUIRE(mgr.accounts().size() == 2);
    CHECK(mgr.accounts()[0]->user_id == "@alice:example.org");
    CHECK(mgr.accounts()[1]->user_id == "@bob:matrix.org");
}

TEST_CASE("AccountManager - remove non-existent is a no-op", "[account_manager]")
{
    AccountManager mgr;
    mgr.add_account(make_session("@alice:example.org"));
    mgr.remove_account("@nobody:example.org");

    REQUIRE(mgr.accounts().size() == 1);
}

TEST_CASE("AccountManager - shared_ptr identity preserved", "[account_manager]")
{
    AccountManager mgr;
    auto session = make_session("@alice:example.org");
    mgr.add_account(session);

    CHECK(mgr.find("@alice:example.org").get() == session.get());
}

// ---------------------------------------------------------------------------
// Window registry tests
// ShellBase is forward-declared in AccountManager.h; we only need a pointer
// type here, so we cast local int addresses to ShellBase* (never dereferenced).
// ---------------------------------------------------------------------------

namespace
{
tesseract::ShellBase* fake_win(int& tag)
{
    return reinterpret_cast<tesseract::ShellBase*>(&tag);
}
} // namespace

TEST_CASE("AccountManager registry - window_count", "[account_manager][registry]")
{
    AccountManager mgr;
    CHECK(mgr.window_count() == 0);

    int t1, t2;
    mgr.register_window(fake_win(t1));
    CHECK(mgr.window_count() == 1);

    mgr.register_window(fake_win(t2));
    CHECK(mgr.window_count() == 2);

    mgr.unregister_window(fake_win(t1));
    CHECK(mgr.window_count() == 1);
}

TEST_CASE("AccountManager registry - dedicated_window round-trip",
          "[account_manager][registry]")
{
    AccountManager mgr;
    int t1;
    tesseract::ShellBase* w = fake_win(t1);

    CHECK(mgr.dedicated_window("@alice:example.org") == nullptr);
    mgr.set_dedicated("@alice:example.org", w);
    CHECK(mgr.dedicated_window("@alice:example.org") == w);
    mgr.clear_dedicated("@alice:example.org");
    CHECK(mgr.dedicated_window("@alice:example.org") == nullptr);
}

TEST_CASE("AccountManager registry - all_windows span", "[account_manager][registry]")
{
    AccountManager mgr;
    int t1, t2;
    mgr.register_window(fake_win(t1));
    mgr.register_window(fake_win(t2));

    REQUIRE(mgr.all_windows().size() == 2);
    CHECK(mgr.all_windows()[0] == fake_win(t1));
    CHECK(mgr.all_windows()[1] == fake_win(t2));
}

TEST_CASE("AccountManager registry - unregister_window is idempotent for unknown ptr",
          "[account_manager][registry]")
{
    AccountManager mgr;
    int t1;
    mgr.unregister_window(fake_win(t1));  // must not crash
    CHECK(mgr.window_count() == 0);
}
