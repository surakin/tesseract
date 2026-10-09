#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include "views/MainAppWidget.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// ShellBase test double that records whether the native avatar picker was
// opened, so we can assert that the /myroomavatar branch of the unified
// dispatch_room_send_ ladder routed there.
struct SendShell : tesseract::test::TestShellBase
{
    // Like the real shells' destructors: finish queued work (e.g. the plain-
    // text send's pipeline job) while post_to_ui_ is still overridden. Left
    // to ~ShellBase, a job completing mid-join calls the now pure-virtual
    // post_to_ui_ and aborts.
    ~SendShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }

    bool avatar_picker_opened = false;
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override
    {
        // Record but never invoke the callback (i.e. simulate a still-open
        // picker), so no async upload work is queued during the test.
        avatar_picker_opened = true;
    }
    void apply_thread_messages_(
        const std::string&,
        std::vector<tesseract::views::MessageRowData>, bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    void on_show_status_message_ui_(const std::string& msg) override
    {
        status_messages.push_back(msg);
    }
    std::vector<std::string> status_messages;

    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::main_app_;
    using ShellBase::dispatch_room_send_;
    using ShellBase::report_unsent_message_;
    using ShellBase::pending_room_actions_;
    using ShellBase::RoomActionKind;
};

} // namespace

TEST_CASE("dispatch_room_send_ with no client is treated as handled",
          "[shell][dispatch_room_send]")
{
    SendShell s;
    // client_ defaults to nullptr.
    auto out = s.dispatch_room_send_("!r:x", "hello", "");
    CHECK(out.handled_as_command);
}

TEST_CASE("dispatch_room_send_ routes /myroomavatar to the avatar picker",
          "[shell][dispatch_room_send]")
{
    // Declared first so it outlives the shell's queued work, which uses it.
    tesseract::Client client;
    SendShell s;
    s.client_ = &client;

    auto out = s.dispatch_room_send_("!r:x", "/myroomavatar", "");
    CHECK(out.handled_as_command);
    CHECK(s.avatar_picker_opened);
    // A command must NOT enqueue a normal send.
    CHECK(s.pending_room_actions_.empty());
}

TEST_CASE("dispatch_room_send_ routes /leave to a Leave room action",
          "[shell][dispatch_room_send]")
{
    // Declared first so it outlives the shell's queued work, which uses it.
    tesseract::Client client;
    SendShell s;
    s.client_ = &client;

    auto out = s.dispatch_room_send_("!r:x", "/leave", "");
    CHECK(out.handled_as_command);
    // leave_room_command_ records a pending Leave action synchronously before
    // dispatching the async SDK call.
    REQUIRE(s.pending_room_actions_.size() == 1);
    const auto& action = s.pending_room_actions_.begin()->second;
    CHECK(action.room_id == "!r:x");
    CHECK(action.kind == SendShell::RoomActionKind::Leave);
}

TEST_CASE("dispatch_room_send_ falls through to a normal send for plain text",
          "[shell][dispatch_room_send]")
{
    // Declared first so it outlives the shell's queued work, which uses it.
    tesseract::Client client;
    SendShell s;
    s.client_ = &client;

    auto out = s.dispatch_room_send_("!r:x", "just a message", "");
    // Not a recognized native command: it is a normal compose send, so the
    // caller (not the command ladder) owns the result.
    CHECK_FALSE(out.handled_as_command);
    CHECK(s.avatar_picker_opened == false);
    CHECK(s.pending_room_actions_.empty());
}

TEST_CASE("report_unsent_message_ surfaces a failed send and ignores success "
          "and cancellation",
          "[shell][dispatch_room_send]")
{
    SendShell s;

    s.report_unsent_message_("@a:x", "!r:x", "hello", tesseract::Result{true, ""});
    s.report_unsent_message_("@a:x", "!r:x", "hello",
                             tesseract::Result{false, "cancelled"});
    CHECK(s.status_messages.empty());

    s.report_unsent_message_("@a:x", "!r:x", "hello",
                             tesseract::Result{false, "boom"});
    REQUIRE_FALSE(s.status_messages.empty());
    CHECK(s.status_messages.front() == "Message not sent: boom");
}

TEST_CASE("dispatch_room_send_ routes /poll to the create-poll dialog",
          "[shell][dispatch_room_send][poll]")
{
    // Declared first so it outlives the shell's queued work.
    auto app = tk::create_root_widget<tesseract::views::MainAppWidget>(nullptr);
    SendShell s;
    auto sess = std::make_shared<tesseract::AccountSession>();
    sess->user_id = "@a:x";
    sess->client  = std::make_unique<tesseract::Client>();
    s.active_account_ = sess;
    s.client_         = sess->client.get();
    s.main_app_       = app.get();

    REQUIRE(app->create_poll_dialog());
    CHECK_FALSE(app->create_poll_dialog()->is_open());
    auto out = s.dispatch_room_send_("!r:x", "/poll", "");
    CHECK(out.handled_as_command);
    CHECK(app->create_poll_dialog()->is_open());
    CHECK(s.pending_room_actions_.empty());
    s.main_app_ = nullptr;
}

TEST_CASE("dispatch_room_send_ /poll without a main app is a safe no-op",
          "[shell][dispatch_room_send][poll]")
{
    tesseract::Client client;
    SendShell s;
    s.client_ = &client;
    auto out = s.dispatch_room_send_("!r:x", "/poll", "");
    CHECK(out.handled_as_command);
    CHECK(s.pending_room_actions_.empty());
}
