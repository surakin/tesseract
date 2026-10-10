// ShellBase_accounts.cpp pieces that need no network: the sync-error state
// machine, delayed sync restarts, the account picker routing and the
// dedicated-window bookkeeping.

#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct AccountsShell : tesseract::test::TestShellBase
{
    ~AccountsShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    void post_to_ui_after_(int ms, std::function<void()> fn) override
    {
        delayed.emplace_back(ms, std::move(fn));
    }
    std::vector<std::pair<int, std::function<void()>>> delayed;
    // The status line also schedules its own auto-clear timer; count only
    // posts with the given delay.
    int restarts_scheduled(int ms) const
    {
        int n = 0;
        for (const auto& d : delayed)
        {
            n += d.first == ms ? 1 : 0;
        }
        return n;
    }

    void on_show_status_message_ui_(const std::string& m) override
    {
        status.push_back(m);
    }
    std::vector<std::string> status;

    void request_relogin_(const std::string& uid) override
    {
        relogins.push_back(uid);
    }
    std::vector<std::string> relogins;

    void switch_active_account_(const std::string& uid) override
    {
        switched.push_back(uid);
    }
    std::vector<std::string> switched;

    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession> s) override
    {
        spawned.push_back(s->user_id);
    }
    std::vector<std::string> spawned;

    bool is_ctrl_held_() const override { return ctrl; }
    bool ctrl = false;

    void raise_and_activate_() override { ++raised; }
    int raised = 0;

    using ShellBase::account_manager_;
    using ShellBase::active_account_;
    using ShellBase::claim_dedicated_for_active_;
    using ShellBase::client_;
    using ShellBase::handle_sync_error_impl_;
    using ShellBase::is_pinned_window_;
    using ShellBase::is_secondary_window_startup_;
    using ShellBase::my_user_id_;
    using ShellBase::on_account_picker_select_;
    using ShellBase::release_dedicated_for_active_;
    using ShellBase::restart_account_sync_;
    using ShellBase::schedule_sync_restart_;
    using ShellBase::set_initial_account;
};

std::shared_ptr<tesseract::AccountSession> acct(const std::string& uid,
                                                bool with_client)
{
    auto s = std::make_shared<tesseract::AccountSession>();
    s->user_id = uid;
    if (with_client)
    {
        s->client = std::make_unique<tesseract::Client>();
    }
    return s;
}

} // namespace

TEST_CASE("a transient reconnect stops sync and schedules a delayed restart",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    auto a = acct("@a:x", true);
    a->sync_started = true;
    s.account_manager_.add_account(a);

    s.handle_sync_error_impl_("sync_reconnect", "@a:x", "", false);
    REQUIRE(s.status.size() == 1);
    CHECK_FALSE(a->sync_started);
    CHECK(s.restarts_scheduled(5000) == 1);
    CHECK(s.relogins.empty());
}

TEST_CASE("a reconnect for an unknown account only shows the status",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    s.handle_sync_error_impl_("sync_reconnect", "@ghost:x", "", false);
    CHECK(s.status.size() == 1);
    CHECK(s.restarts_scheduled(5000) == 0);
}

TEST_CASE("an unrecoverable auth error clears the account and asks to log in again",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    auto a = acct("@a:x", true);
    a->sync_started = true;
    s.account_manager_.add_account(a);

    // Not a soft logout: no restore attempt.
    s.handle_sync_error_impl_("sync_auth_error", "@a:x", "", false);
    CHECK_FALSE(a->sync_started);
    CHECK(s.relogins == std::vector<std::string>{"@a:x"});
    REQUIRE(s.status.size() == 1);
    CHECK(s.status[0].find("log in again") != std::string::npos);
}

TEST_CASE("a soft logout with no stored session falls through to relogin",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    auto a = acct("@nostored:x", true);
    s.account_manager_.add_account(a);
    s.handle_sync_error_impl_("sync_auth_error", "@nostored:x", "", true);
    CHECK(s.relogins == std::vector<std::string>{"@nostored:x"});
}

TEST_CASE("an auth error for an unknown account still requests relogin",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    s.handle_sync_error_impl_("sync_auth_error", "@ghost:x", "", true);
    CHECK(s.relogins == std::vector<std::string>{"@ghost:x"});
}

TEST_CASE("other sync errors surface their description", "[shell][accounts][sync_error]")
{
    AccountsShell s;
    s.handle_sync_error_impl_("something_else", "@a:x", "server on fire", false);
    CHECK(s.status == std::vector<std::string>{"server on fire"});
    CHECK(s.relogins.empty());
}

TEST_CASE("restart_account_sync_ only restarts an idle account with a client",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    auto already = acct("@running:x", true);
    already->sync_started = true;
    auto no_client = acct("@noclient:x", false);
    s.account_manager_.add_account(already);
    s.account_manager_.add_account(no_client);

    s.restart_account_sync_("@running:x");
    s.restart_account_sync_("@noclient:x");
    s.restart_account_sync_("@ghost:x");
    CHECK(already->sync_started);
    CHECK_FALSE(no_client->sync_started);
}

TEST_CASE("schedule_sync_restart_ posts the restart after the given delay",
          "[shell][accounts][sync_error]")
{
    AccountsShell s;
    s.schedule_sync_restart_("@a:x", 1234);
    REQUIRE(s.delayed.size() == 1);
    CHECK(s.delayed[0].first == 1234);
    // Running it for an unknown account is harmless.
    CHECK_NOTHROW(s.delayed[0].second());
}

TEST_CASE("the account picker raises a window that already shows the account",
          "[shell][accounts][picker]")
{
    AccountsShell owner;
    AccountsShell s;
    // Same AccountManager would be needed for a shared registry; each double
    // owns its own, so register the owner window in s's manager.
    s.account_manager_.add_account(acct("@a:x", false));
    s.account_manager_.set_dedicated("@a:x", &owner);
    s.on_account_picker_select_("@a:x");
    CHECK(owner.raised == 1);
    CHECK(s.switched.empty());
    s.account_manager_.clear_dedicated("@a:x");
}

TEST_CASE("the account picker does not raise itself", "[shell][accounts][picker]")
{
    AccountsShell s;
    s.account_manager_.add_account(acct("@a:x", false));
    s.account_manager_.set_dedicated("@a:x", &s);
    s.on_account_picker_select_("@a:x");
    CHECK(s.raised == 0);
    CHECK(s.switched.empty());
    s.account_manager_.clear_dedicated("@a:x");
}

TEST_CASE("Ctrl-click opens the account in a new window",
          "[shell][accounts][picker]")
{
    AccountsShell s;
    s.account_manager_.add_account(acct("@a:x", false));
    s.ctrl = true;
    s.on_account_picker_select_("@a:x");
    CHECK(s.spawned == std::vector<std::string>{"@a:x"});
    CHECK(s.switched.empty());
    // Unknown account under Ctrl: nothing.
    s.on_account_picker_select_("@ghost:x");
    CHECK(s.spawned.size() == 1);
}

TEST_CASE("a pinned window opens the chosen account elsewhere",
          "[shell][accounts][picker]")
{
    AccountsShell s;
    s.account_manager_.add_account(acct("@a:x", false));
    s.is_pinned_window_ = true;
    s.on_account_picker_select_("@a:x");
    CHECK(s.spawned == std::vector<std::string>{"@a:x"});
    CHECK(s.switched.empty());
}

TEST_CASE("a normal window switches to the chosen account",
          "[shell][accounts][picker]")
{
    AccountsShell s;
    s.account_manager_.add_account(acct("@a:x", false));
    s.on_account_picker_select_("@a:x");
    CHECK(s.switched == std::vector<std::string>{"@a:x"});
}

TEST_CASE("secondary-window startup reuses restored accounts",
          "[shell][accounts]")
{
    AccountsShell s;
    CHECK_FALSE(s.is_secondary_window_startup_()); // empty manager
    auto a = acct("@a:x", false);
    s.account_manager_.add_account(a);
    CHECK_FALSE(s.is_secondary_window_startup_()); // nothing pinned yet
    s.set_initial_account(a);
    CHECK(s.is_secondary_window_startup_());
    tesseract::Client c;
    s.client_ = &c;
    CHECK_FALSE(s.is_secondary_window_startup_()); // already bound
}

TEST_CASE("dedicated-window claim never steals another live window's account",
          "[shell][accounts]")
{
    AccountsShell mine;
    AccountsShell other;
    mine.account_manager_.add_account(acct("@a:x", false));
    mine.set_initial_account(mine.account_manager_.find("@a:x"));

    mine.claim_dedicated_for_active_();
    CHECK(mine.account_manager_.dedicated_window("@a:x") == &mine);

    // Someone else owns it: a claim is a no-op.
    mine.account_manager_.set_dedicated("@a:x", &other);
    mine.claim_dedicated_for_active_();
    CHECK(mine.account_manager_.dedicated_window("@a:x") == &other);

    // Releasing something we don't own leaves it alone.
    mine.release_dedicated_for_active_();
    CHECK(mine.account_manager_.dedicated_window("@a:x") == &other);

    mine.account_manager_.set_dedicated("@a:x", &mine);
    mine.release_dedicated_for_active_();
    CHECK(mine.account_manager_.dedicated_window("@a:x") == nullptr);
}
