#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "views/RoomView.h"

#include <tesseract/client.h>

#include <memory>
#include <string>
#include <vector>

using tesseract::RoomPane;

namespace tesseract
{

// Friend of RoomPane (see RoomPane.h): installs a RoomView and wires its
// send callbacks, and flips the pane into pop-out (owned-account) mode.
struct RoomPaneGalleryTestAccess
{
    static void attach(RoomPane& p, views::RoomView* rv, bool has_owner)
    {
        p.room_view_ = rv;
        p.has_owner_ = has_owner;
        p.wire_room_view_();
    }
};

} // namespace tesseract

namespace
{

struct GalleryShell : tesseract::test::TestShellBase
{
    void apply_thread_messages_(const std::string&,
                                std::vector<tesseract::views::MessageRowData>,
                                bool) override {}
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&, std::size_t) override {}
    using ShellBase::client_;
};

std::vector<tesseract::views::ComposeBar::PendingAttachment> two_files()
{
    std::vector<tesseract::views::ComposeBar::PendingAttachment> v(2);
    for (auto& pa : v)
    {
        pa.kind     = tesseract::views::ComposeBar::PendingAttachment::Kind::File;
        pa.bytes    = {1, 2, 3};
        pa.mime     = "application/octet-stream";
        pa.filename = "f.bin";
    }
    return v;
}

} // namespace

TEST_CASE("on_send_gallery uses the pane's own client, not the shell's",
          "[shell][gallery]")
{
    GalleryShell s;
    tesseract::Client shell_client;
    s.client_ = &shell_client; // the main window's active account

    // A pop-out pane whose owning account has no live session: pane_client_()
    // is null. The gallery must NOT fall back to the shell's client (that
    // would post this room's gallery as the wrong account).
    auto pane = std::make_unique<RoomPane>(
        RoomPane::Deps{.shell = &s, .repaint = [] {}, .relayout = [] {}}, "!r:x");
    auto view = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomPaneGalleryTestAccess::attach(*pane, view.get(), /*has_owner=*/true);

    view->compose_bar()->set_current_text("caption draft");
    REQUIRE(view->on_send_gallery);
    view->on_send_gallery(two_files(), "caption draft", "");

    // Early return: composer untouched, nothing was sent.
    CHECK(view->compose_bar()->current_text() == "caption draft");
}

TEST_CASE("on_send_gallery clears the composer when the pane has a client",
          "[shell][gallery]")
{
    GalleryShell s;
    tesseract::Client shell_client;
    s.client_ = &shell_client;

    auto pane = std::make_unique<RoomPane>(
        RoomPane::Deps{.shell = &s, .repaint = [] {}, .relayout = [] {}}, "!r:x");
    auto view = tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    tesseract::RoomPaneGalleryTestAccess::attach(*pane, view.get(), /*has_owner=*/false);

    view->compose_bar()->set_current_text("caption draft");
    view->on_send_gallery(two_files(), "caption draft", "");

    CHECK(view->compose_bar()->current_text().empty());
}
