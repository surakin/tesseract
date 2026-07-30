#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "tk/i18n.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/notifier.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using tesseract::AccountSession;
using tesseract::ShellBase;

namespace
{

// Records every Notification passed to notify(), standing in for a platform
// INotifier so tests can assert on the synthetic failure notification built
// by ShellBase::notify_reply_failed_.
struct RecordingNotifier : tesseract::INotifier
{
    std::vector<tesseract::Notification> notifications;
    void notify(const tesseract::Notification& n) override
    {
        notifications.push_back(n);
    }
};

struct NotificationReplyShellAccountManager { tesseract::AccountManager am_; };

// ShellBase test double exercising send_notification_reply_ end-to-end via a
// REAL mut_pool_ round trip (post_to_ui_ queues into a mutex/condvar-guarded
// vector rather than running inline), mirroring RestoreAsyncShell in
// test_shell_restore_async.cpp.
struct NotificationReplyShell : NotificationReplyShellAccountManager, ShellBase
{
    NotificationReplyShell() : ShellBase(am_) {}

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::function<void()>> queue_;

    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(fn));
        cv_.notify_all();
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(fn));
        cv_.notify_all();
    }

    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override {}
    void repaint_pickers_() override {}
    void navigate_to_room_(const std::string&) override {}
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override {}
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override {}
    std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() override
    {
        return nullptr;
    }
    tesseract::CallWindowBase* create_call_window_() override { return nullptr; }
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void refresh_account_ui_after_switch_() override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override {}
    std::unique_ptr<tesseract::IEventHandler>
    make_account_bridge_(const std::string&) override { return nullptr; }
    void install_account_notifier_(tesseract::AccountSession&) override {}
    void request_relogin_(const std::string&) override {}
    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    using ShellBase::account_manager_;
    using ShellBase::send_notification_reply_;

    // Wait (bounded) for the worker's post_to_ui_alive_ continuation to land
    // in queue_, then run it on this thread, standing in for the UI thread.
    // Returns false if nothing arrived within the timeout (the synchronous
    // no-op guards never touch the pool, so callers of those paths must not
    // call this — it would just time out).
    bool wait_and_drain(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mu_);
        bool got = cv_.wait_for(lk, timeout, [&] { return !queue_.empty(); });
        auto pending = std::move(queue_);
        queue_.clear();
        lk.unlock();
        for (auto& fn : pending)
            if (fn) fn();
        return got;
    }
};

std::shared_ptr<AccountSession> make_session(const std::string& user_id)
{
    auto sess = std::make_shared<AccountSession>();
    sess->user_id = user_id;
    sess->client = std::make_unique<tesseract::Client>();
    sess->notifier = std::make_unique<RecordingNotifier>();
    return sess;
}

} // namespace

TEST_CASE("send_notification_reply_ is a no-op for an unknown user_id",
          "[shell][notification_reply]")
{
    NotificationReplyShell s;
    // No accounts registered at all.
    s.send_notification_reply_("@nobody:example.org", "!r:x", "", "hello");

    // Nothing to drain: there is no session to hang a failure notification
    // off of, so the call must return without ever touching post_to_ui_.
    CHECK_FALSE(s.wait_and_drain(std::chrono::milliseconds(50)));
}

TEST_CASE("send_notification_reply_ is a no-op for empty/whitespace text",
          "[shell][notification_reply]")
{
    NotificationReplyShell s;
    auto sess = make_session("@alice:example.org");
    auto* notifier = static_cast<RecordingNotifier*>(sess->notifier.get());
    s.account_manager_.add_account(sess);

    s.send_notification_reply_("@alice:example.org", "!r:x", "", "   ");

    CHECK_FALSE(s.wait_and_drain(std::chrono::milliseconds(50)));
    CHECK(notifier->notifications.empty());
}

TEST_CASE("send_notification_reply_ reports a failure notification when the "
          "account is logged out",
          "[shell][notification_reply]")
{
    NotificationReplyShell s;
    auto sess = std::make_shared<AccountSession>();
    sess->user_id = "@alice:example.org";
    // client left null: simulates an account that was logged out between
    // the notification firing and the reply being submitted.
    sess->notifier = std::make_unique<RecordingNotifier>();
    auto* notifier = static_cast<RecordingNotifier*>(sess->notifier.get());
    s.account_manager_.add_account(sess);

    s.send_notification_reply_("@alice:example.org", "!r:x", "", "hello");

    // The signed-out guard fires synchronously, before run_async_mut_.
    CHECK_FALSE(s.wait_and_drain(std::chrono::milliseconds(50)));
    REQUIRE(notifier->notifications.size() == 1);
    CHECK(notifier->notifications[0].room_id == "!r:x");
    CHECK(notifier->notifications[0].sender == tk::tr("Message not sent"));
    CHECK(notifier->notifications[0].body ==
          tk::tr("You're signed out of this account"));
}

TEST_CASE("send_notification_reply_ reports a failure notification when the "
          "send fails, as a plain message (no event_id)",
          "[shell][notification_reply]")
{
    NotificationReplyShell s;
    auto sess = make_session("@alice:example.org");
    auto* notifier = static_cast<RecordingNotifier*>(sess->notifier.get());
    s.account_manager_.add_account(sess);

    s.send_notification_reply_("@alice:example.org", "!r:x", "", "hello");

    // client is a fresh, unauthenticated tesseract::Client — send_message
    // deterministically fails ("not logged in"), so the async round trip
    // through mut_pool_ + post_to_ui_ must land here.
    REQUIRE(s.wait_and_drain(std::chrono::seconds(5)));
    REQUIRE(notifier->notifications.size() == 1);
    CHECK(notifier->notifications[0].room_id == "!r:x");
    CHECK(notifier->notifications[0].sender == tk::tr("Message not sent"));
}

TEST_CASE("send_notification_reply_ reports a failure notification when the "
          "send fails, as a threaded reply (event_id set)",
          "[shell][notification_reply]")
{
    NotificationReplyShell s;
    auto sess = make_session("@alice:example.org");
    auto* notifier = static_cast<RecordingNotifier*>(sess->notifier.get());
    s.account_manager_.add_account(sess);

    s.send_notification_reply_("@alice:example.org", "!r:x", "$event:x",
                               "hello");

    REQUIRE(s.wait_and_drain(std::chrono::seconds(5)));
    REQUIRE(notifier->notifications.size() == 1);
    CHECK(notifier->notifications[0].room_id == "!r:x");
    CHECK(notifier->notifications[0].sender == tk::tr("Message not sent"));
}
