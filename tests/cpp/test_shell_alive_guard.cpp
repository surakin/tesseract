#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A ShellBase test double that queues UI-thread callbacks into an EXTERNAL
// vector (owned by the test, not the shell) so the queued continuations can be
// drained *after* the shell is destroyed. This exercises the liveness guard:
// a continuation enqueued via post_to_ui_alive_ must no-op once ~ShellBase has
// run, rather than dereferencing the freed shell.
struct AliveShell : tesseract::test::TestShellBase
{
    explicit AliveShell(std::vector<std::function<void()>>* queue) : queue_(queue)
    {
    }

    std::vector<std::function<void()>>* queue_;

    void post_to_ui_(std::function<void()> fn) override
    {
        queue_->push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue_->push_back(std::move(fn));
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

    // Expose the guarded post helper to the test.
    using ShellBase::post_to_ui_alive_;
    // Expose the worker-pool dispatch + active-session members so the
    // session-capture lifetime fix can be exercised directly.
    using ShellBase::active_account_;
    using ShellBase::run_async_;
};

} // namespace

TEST_CASE("post_to_ui_alive_ continuation no-ops after the shell is destroyed", "[shell][alive]")
{
    std::vector<std::function<void()>> queue;
    bool ran = false;

    {
        AliveShell s(&queue);
        // Enqueue a continuation that captures the shell and would touch it.
        s.post_to_ui_alive_(
            [&s, &ran]
            {
                // Touch the (would-be freed) shell to model a
                // real continuation dereferencing `this`.
                (void)&s;
                ran = true;
            });

        // The guard wraps the fn; it is queued, not yet run.
        REQUIRE(queue.size() == 1);
        REQUIRE_FALSE(ran);
    } // ~AliveShell runs here: sets *alive_ = false.

    // Drain the queue AFTER destruction. The guarded continuation must detect
    // the dead token and skip the body — without the guard this would
    // dereference the freed shell (use-after-free) and set `ran`.
    for (auto& fn : queue)
    {
        if (fn)
        {
            fn();
        }
    }

    CHECK_FALSE(ran);
}

TEST_CASE("post_to_ui_alive_ continuation runs while the shell is alive", "[shell][alive]")
{
    std::vector<std::function<void()>> queue;
    bool ran = false;

    AliveShell s(&queue);
    s.post_to_ui_alive_(
        [&ran]
        {
            ran = true;
        });

    REQUIRE(queue.size() == 1);
    REQUIRE_FALSE(ran);

    // Drain while the shell is still alive: the body runs.
    for (auto& fn : queue)
    {
        if (fn)
        {
            fn();
        }
    }

    CHECK(ran);
}

TEST_CASE("worker body capturing the active AccountSession keeps it alive "
          "across logout",
          "[shell][session-capture]")
{
    std::vector<std::function<void()>> queue;

    AliveShell s(&queue);

    // Install an active account. AccountSession is a pure value type, so the
    // null client/bridge are fine — we only care that the captured shared_ptr
    // keeps the AccountSession object alive past the shell's own reset.
    auto account = std::make_shared<tesseract::AccountSession>();
    account->user_id = "@alice:example.org";
    std::weak_ptr<tesseract::AccountSession> weak = account;
    s.set_initial_account(std::move(account));

    // Gate the worker so it cannot run until after we drop the shell's strong
    // ref, deterministically reproducing the logout/switch race window.
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    bool done = false;
    bool saw_live_session = false;

    // Production pattern: capture a strong ref to the active account at
    // dispatch time and dereference THAT inside the worker body.
    auto sess = s.active_account_;
    s.run_async_(
        [sess, &mu, &cv, &release, &done, &saw_live_session]
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk,
                    [&]
                    {
                        return release;
                    });
            // The shell dropped its active_account_ before this ran; the
            // captured shared_ptr must still observe the live session.
            saw_live_session = (sess != nullptr);
            done = true;
            lk.unlock();
            cv.notify_all();
        });

    // Simulate logout: the shell drops its last strong ref to the session.
    // Without the worker's captured shared_ptr this would destroy the
    // AccountSession (and its Client) — the use-after-free this fix prevents.
    s.active_account_.reset();
    REQUIRE_FALSE(weak.expired()); // worker's captured ref still holds it alive

    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();

    // Wait for the worker to finish before asserting.
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk,
                [&]
                {
                    return done;
                });
    }

    CHECK(saw_live_session);
}
