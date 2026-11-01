#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <tesseract/account_session.h>

#include <memory>
#include <string>
#include <vector>

using tesseract::AccountManager;
using tesseract::AccountSession;
using tesseract::ShellBase;

namespace
{

struct ClickShell : tesseract::test::TestShellBase
{
    explicit ClickShell(AccountManager& am) : TestShellBase(am)
    {
        am.register_window(this);
    }
    ~ClickShell() override
    {
        account_manager_.unregister_window(this);
    }

    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }

    void switch_active_account_(const std::string& uid) override
    {
        switched_to.push_back(uid);
    }
    void activate_for_notification_(const std::string& token) override
    {
        tokens.push_back(token);
    }
    void navigate_to_room_(const std::string& room) override
    {
        navigated.push_back(room);
    }

    void show(const std::shared_ptr<AccountSession>& s)
    {
        active_account_ = s;
    }

    using ShellBase::on_notification_clicked_;
    using ShellBase::notification_target_window_;

    std::vector<std::string> switched_to, tokens, navigated;
};

std::shared_ptr<AccountSession> session(const std::string& uid)
{
    auto s = std::make_shared<AccountSession>();
    s->user_id = uid;
    return s;
}

} // namespace

TEST_CASE("notification click acts on the window owning the account", "[shell][notification]")
{
    AccountManager am;
    ClickShell a(am), b(am);
    auto x = session("@x:example.org");
    auto y = session("@y:example.org");
    am.add_account(x);
    am.add_account(y);
    a.show(x);
    b.show(y);
    am.set_dedicated(x->user_id, &a);
    am.set_dedicated(y->user_id, &b);

    // Notifier was installed by window A, but the click is for account Y.
    a.on_notification_clicked_(y->user_id, "!room:example.org", "", "tok");

    CHECK(a.switched_to.empty());
    CHECK(a.navigated.empty());
    CHECK(b.switched_to == std::vector<std::string>{y->user_id});
    CHECK(b.tokens == std::vector<std::string>{"tok"});
    CHECK(b.navigated == std::vector<std::string>{"!room:example.org"});
}

TEST_CASE("notification click falls back to a window already showing the account",
          "[shell][notification]")
{
    AccountManager am;
    ClickShell a(am), b(am);
    auto y = session("@y:example.org");
    am.add_account(y);
    b.show(y); // no dedicated mapping recorded

    CHECK(a.notification_target_window_(y->user_id) == &b);
}

TEST_CASE("notification click with no owner switches the calling window", "[shell][notification]")
{
    AccountManager am;
    ClickShell a(am), b(am);
    auto y = session("@y:example.org");
    am.add_account(y);

    a.on_notification_clicked_(y->user_id, "!r:example.org", "", "");

    CHECK(a.switched_to == std::vector<std::string>{y->user_id});
    CHECK(a.navigated == std::vector<std::string>{"!r:example.org"});
    CHECK(b.navigated.empty());
}

TEST_CASE("notification click with an event id still navigates when the thread "
          "cannot be resolved",
          "[shell][notification]")
{
    AccountManager am;
    ClickShell a(am);
    auto y = session("@y:example.org"); // no client: the thread lookup is skipped
    am.add_account(y);

    a.on_notification_clicked_(y->user_id, "!r:example.org", "$evt:example.org", "tok");

    CHECK(a.navigated == std::vector<std::string>{"!r:example.org"});
    CHECK(a.tokens == std::vector<std::string>{"tok"});
}
