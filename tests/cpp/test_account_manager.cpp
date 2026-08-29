#include <catch2/catch_test_macros.hpp>
#include "app/AccountManager.h"
#include <tesseract/account_session.h>

#include <chrono>
#include <thread>

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

TEST_CASE("AccountManager - upload request IDs are process-wide and nonzero",
          "[account_manager][upload]")
{
    AccountManager mgr;
    CHECK(mgr.next_upload_request_id() == 1);
    CHECK(mgr.next_upload_request_id() == 2);
}

// ---------------------------------------------------------------------------
// Draining registry — see ShellBase::logout_active_account_impl_(), which
// uses this to bound-wait for a just-logged-out session's mut_pool_ teardown
// barrier before considering logout (or a same-user_id re-login) safe.
// ---------------------------------------------------------------------------

TEST_CASE("AccountManager draining - mark/is/clear round trip",
          "[account_manager][draining]")
{
    AccountManager mgr;
    CHECK_FALSE(mgr.is_draining("@alice:example.org"));

    mgr.mark_draining("@alice:example.org");
    CHECK(mgr.is_draining("@alice:example.org"));
    // Unrelated user_ids are unaffected.
    CHECK_FALSE(mgr.is_draining("@bob:example.org"));

    mgr.clear_draining("@alice:example.org");
    CHECK_FALSE(mgr.is_draining("@alice:example.org"));
}

TEST_CASE("AccountManager draining - clear_draining for an unmarked id is a no-op",
          "[account_manager][draining]")
{
    AccountManager mgr;
    mgr.clear_draining("@nobody:example.org"); // must not crash
    CHECK_FALSE(mgr.is_draining("@nobody:example.org"));
}

TEST_CASE("AccountManager draining - wait_until_drained returns immediately when not draining",
          "[account_manager][draining]")
{
    AccountManager mgr;
    const auto start = std::chrono::steady_clock::now();
    CHECK(mgr.wait_until_drained("@alice:example.org", std::chrono::milliseconds(500)));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(200));
}

TEST_CASE("AccountManager draining - wait_until_drained blocks until another thread clears it",
          "[account_manager][draining]")
{
    AccountManager mgr;
    mgr.mark_draining("@alice:example.org");

    std::thread clearer([&mgr] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        mgr.clear_draining("@alice:example.org");
    });

    CHECK(mgr.wait_until_drained("@alice:example.org", std::chrono::milliseconds(2000)));
    CHECK_FALSE(mgr.is_draining("@alice:example.org"));
    clearer.join();
}

TEST_CASE("AccountManager draining - wait_until_drained times out and leaves the flag set",
          "[account_manager][draining]")
{
    AccountManager mgr;
    mgr.mark_draining("@alice:example.org");

    CHECK_FALSE(mgr.wait_until_drained("@alice:example.org", std::chrono::milliseconds(50)));
    // Not force-cleared on timeout — a later caller must still see the truth.
    CHECK(mgr.is_draining("@alice:example.org"));
}
