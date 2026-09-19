#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "tk_test_host.h"
#include "views/CallBanner.h"
#include "views/InviteCard.h"
#include "views/RoomPreviewView.h"

using tesseract::views::CallBanner;
using tesseract::views::InviteCard;
using tesseract::views::RoomPreviewView;

TEST_CASE("CallBanner announces the call type and who is in it",
         "[banner][accessibility]")
{
    auto banner = tk::create_root_widget<CallBanner>(nullptr);
    banner->set_call("video", {{"@a:x.org", "Alice"}, {"@b:x.org", ""}}, [] {}, true);
    CHECK(banner->access_role() == tk::Role::Group);
    CHECK(banner->access_name() == "Video call in progress: Alice, @b:x.org");

    banner->set_call("", {}, [] {}, true);
    CHECK(banner->access_name() == "Call in progress");
}

TEST_CASE("CallBanner shows until cleared and has only a Join button",
         "[banner]")
{
    auto banner = tk::create_root_widget<CallBanner>(nullptr);
    CHECK_FALSE(banner->visible());

    int joined = 0;
    banner->set_call("audio", {{"@a:x.org", "Alice"}}, [&] { ++joined; }, true);
    CHECK(banner->visible());
    CHECK(banner->members().size() == 1);
    // Join is the only button: no Decline/Answer pair, and no auto-dismiss API.
    CHECK(banner->children().size() == 1);

    banner->clear();
    CHECK_FALSE(banner->visible());
    CHECK(banner->members().empty());
}

TEST_CASE("RoomPreviewView summarises the room; None until a summary is set",
         "[banner][accessibility]")
{
    auto view = tk::create_root_widget<RoomPreviewView>(nullptr);
    CHECK(view->access_role() == tk::Role::None);

    tesseract::RoomSummary s;
    s.room_id = "!x:example.org";
    s.name = "Design";
    s.topic = "pixels";
    s.num_joined_members = 3;
    view->set_summary(s);

    CHECK(view->access_role() == tk::Role::Group);
    CHECK(view->access_name() == "Design, 3 members: pixels");
}

TEST_CASE("InviteCard announces who invited you and to what",
         "[banner][accessibility]")
{
    auto card = tk::create_root_widget<InviteCard>(nullptr);
    CHECK(card->access_role() == tk::Role::None);

    tesseract::InviteInfo info;
    info.room_id = "!r:example.org";
    info.room_name = "Book club";
    info.inviter_display_name = "Bob";
    info.is_direct = false;
    card->set_invite(info, {});

    CHECK(card->access_role() == tk::Role::Group);
    CHECK(card->access_name() == "Bob invited you to Book club");
}
