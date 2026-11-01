#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "app/ThreadPanelController.h"
#include "views/RoomView.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::RoomPane;
using tesseract::ShellBase;

namespace tesseract
{

// Exposes exactly the private RoomPane thread-panel state this test suite
// pokes directly, mirroring RoomPaneMediaViewTestAccess in
// test_shell_media_view_pagination.cpp (RoomPane is held by composition, not
// inherited, so the `using ShellBase::field;` trick the ShellBase test
// double below uses for its own protected members isn't available here).
struct RoomPaneStickerTestAccess
{
    static void set_thread_open(RoomPane& p, std::string thread_root)
    {
        p.thread_panel_ = ThreadPanelController::ThreadPanel::Open;
        p.thread_root_ = std::move(thread_root);
    }
    static views::RoomView*& room_view(RoomPane& p)
    {
        return p.room_view_;
    }
};

} // namespace tesseract

using tesseract::RoomPaneStickerTestAccess;

namespace
{

// Minimal ShellBase test double providing the client_ a RoomPane's
// send_sticker_ reads via shell_->client_. A default-constructed
// tesseract::Client has no live FFI (SH_FFI short-circuits before reaching
// the network), so send_sticker_/send_thread_sticker_ are safe no-ops here —
// these tests assert on compose-bar reply-state side effects, not on FFI
// results (mirrors SendShell in test_shell_dispatch_room_send.cpp).
struct SendStickerShell : tesseract::test::TestShellBase
{

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

    using ShellBase::client_;
};

std::unique_ptr<RoomPane> make_pane(SendStickerShell& s, const std::string& room_id)
{
    return std::make_unique<RoomPane>(
        RoomPane::Deps{.shell = &s, .repaint = [] {}, .relayout = [] {}}, room_id);
}

} // namespace

TEST_CASE("send_sticker_ clears an active reply after sending", "[shell][sticker][reply]")
{
    SendStickerShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto pane = make_pane(s, "!r:x");
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    RoomPaneStickerTestAccess::room_view(*pane) = &view;

    view.compose_bar()->set_reply_to("$evt1", "Alice", "hi");
    REQUIRE(view.compose_bar()->has_reply());

    pane->send_sticker_("sticker body", "mxc://example.org/abc", "{}");

    CHECK_FALSE(view.compose_bar()->has_reply());
}

TEST_CASE("send_sticker_ is a no-op on reply state when no reply is pending",
          "[shell][sticker][reply]")
{
    SendStickerShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto pane = make_pane(s, "!r:x");
    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    RoomPaneStickerTestAccess::room_view(*pane) = &view;

    REQUIRE_FALSE(view.compose_bar()->has_reply());

    pane->send_sticker_("sticker body", "mxc://example.org/abc", "{}");

    CHECK_FALSE(view.compose_bar()->has_reply());
}

TEST_CASE("send_sticker_ clears an active reply in the thread-open branch too",
          "[shell][sticker][reply]")
{
    SendStickerShell s;
    tesseract::Client client;
    s.client_ = &client;

    auto pane = make_pane(s, "!r:x");
    RoomPaneStickerTestAccess::set_thread_open(*pane, "$root:x");

    auto view_owner = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::views::RoomView& view = *view_owner;
    RoomPaneStickerTestAccess::room_view(*pane) = &view;

    view.compose_bar()->set_reply_to("$evt2", "Bob", "hello");
    REQUIRE(view.compose_bar()->has_reply());

    pane->send_sticker_("sticker body", "mxc://example.org/abc", "{}");

    CHECK_FALSE(view.compose_bar()->has_reply());
}
