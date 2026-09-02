#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "tk_test_host.h"
#include "views/IncomingCallBanner.h"
#include "views/InviteCard.h"
#include "views/RoomPreviewView.h"

using tesseract::views::IncomingCallBanner;
using tesseract::views::InviteCard;
using tesseract::views::RoomPreviewView;

TEST_CASE("IncomingCallBanner announces the caller and call type",
         "[banner][accessibility]")
{
    auto banner = tk::create_root_widget<IncomingCallBanner>(nullptr);
    banner->set_call("Alice", "video", [] {}, [] {});
    CHECK(banner->access_role() == tk::Role::Group);
    CHECK(banner->access_name() == "Incoming Video call from Alice");
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
