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

TEST_CASE("AccountManager - add_account ignores a duplicate user_id",
          "[account_manager]")
{
    // A spawned (secondary) window must never re-add an account that the
    // primary window already restored: doing so duplicates it in every account
    // picker and starts its sync twice. add_account() keeps the first session.
    AccountManager mgr;
    auto first = make_session("@alice:example.org", "Alice");
    mgr.add_account(first);
    mgr.add_account(make_session("@alice:example.org", "Alice (dup)"));

    REQUIRE(mgr.accounts().size() == 1);
    // The original session is retained, not clobbered by the duplicate.
    CHECK(mgr.find("@alice:example.org") == first);
    CHECK(mgr.accounts()[0]->display_name == "Alice");
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

// ---------------------------------------------------------------------------
// Primary-window + tray-owner tests (multi-window: one window per account).
// ---------------------------------------------------------------------------

TEST_CASE("AccountManager registry - primary_window is the first registered",
          "[account_manager][registry]")
{
    AccountManager mgr;
    CHECK(mgr.primary_window() == nullptr);

    int t1, t2;
    mgr.register_window(fake_win(t1));
    mgr.register_window(fake_win(t2));
    // The first window to register is the primary and stays so while it lives.
    CHECK(mgr.primary_window() == fake_win(t1));

    // Unregistering a non-primary window does not change the primary.
    mgr.unregister_window(fake_win(t2));
    CHECK(mgr.primary_window() == fake_win(t1));
}

TEST_CASE("AccountManager registry - primary_window falls back when primary unregisters",
          "[account_manager][registry]")
{
    AccountManager mgr;
    int t1, t2;
    mgr.register_window(fake_win(t1));
    mgr.register_window(fake_win(t2));

    // If the primary ever goes away, fall back to the oldest survivor.
    mgr.unregister_window(fake_win(t1));
    CHECK(mgr.primary_window() == fake_win(t2));

    mgr.unregister_window(fake_win(t2));
    CHECK(mgr.primary_window() == nullptr);
}

TEST_CASE("AccountManager - single tray owner across windows",
          "[account_manager][tray]")
{
    AccountManager mgr;
    int t1, t2;
    auto* w1 = fake_win(t1);
    auto* w2 = fake_win(t2);

    CHECK(mgr.tray_owner() == nullptr);

    // First claimant becomes the owner; everyone else is refused.
    CHECK(mgr.claim_tray_owner(w1) == true);
    CHECK(mgr.is_tray_owner(w1) == true);
    CHECK(mgr.claim_tray_owner(w2) == false);
    CHECK(mgr.is_tray_owner(w2) == false);
    CHECK(mgr.tray_owner() == w1);

    // The owner re-claiming stays the owner (idempotent).
    CHECK(mgr.claim_tray_owner(w1) == true);

    // Releasing a non-owner is a no-op.
    mgr.release_tray_owner(w2);
    CHECK(mgr.tray_owner() == w1);

    // Releasing the owner frees the slot for the next claimant.
    mgr.release_tray_owner(w1);
    CHECK(mgr.tray_owner() == nullptr);
    CHECK(mgr.claim_tray_owner(w2) == true);
    CHECK(mgr.tray_owner() == w2);
}

TEST_CASE("AccountManager - unregister_window releases tray ownership",
          "[account_manager][tray]")
{
    AccountManager mgr;
    int t1, t2;
    auto* w1 = fake_win(t1);
    auto* w2 = fake_win(t2);
    mgr.register_window(w1);
    mgr.register_window(w2);

    REQUIRE(mgr.claim_tray_owner(w1) == true);
    // A window that closes must relinquish the tray so another can take it.
    mgr.unregister_window(w1);
    CHECK(mgr.tray_owner() == nullptr);
    CHECK(mgr.claim_tray_owner(w2) == true);
}
