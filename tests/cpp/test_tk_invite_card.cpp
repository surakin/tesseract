#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/InviteCard.h"

#include <tesseract/types.h>

#include <string>

using tesseract::views::InviteCard;

namespace
{

tesseract::InviteInfo ic_invite(bool direct)
{
    tesseract::InviteInfo i;
    i.room_id = "!r:x";
    i.room_name = direct ? "" : "Cool Room";
    i.room_topic = "a topic";
    i.is_direct = direct;
    i.inviter_user_id = "@bob:x";
    i.inviter_display_name = direct ? "" : "Bob";
    i.room_avatar_url = "mxc://x/room";
    i.inviter_avatar_url = "mxc://x/bob";
    return i;
}

struct IcStage : vt::Stage
{
    std::unique_ptr<InviteCard> card = tk::create_root_widget<InviteCard>(&host);
    void run() { mount(*card, {0, 0, 500, 500}); }
};

} // namespace

TEST_CASE("InviteCard starts hidden and empty; clear hides again",
          "[tk][view][invite_card]")
{
    IcStage s;
    CHECK_FALSE(s.card->visible());
    CHECK_FALSE(s.card->has_invite());
    CHECK(s.card->access_role() == tk::Role::None);
    CHECK(s.card->access_name().empty());
    s.run(); // no invite: arrange/paint are harmless

    s.card->set_invite(ic_invite(false), nullptr);
    CHECK(s.card->has_invite());
    CHECK(s.card->visible());
    s.card->clear();
    CHECK_FALSE(s.card->has_invite());
    CHECK_FALSE(s.card->visible());
}

TEST_CASE("InviteCard group invite: name, Accept/Decline, no Block",
          "[tk][view][invite_card]")
{
    IcStage s;
    s.card->set_invite(ic_invite(false), nullptr);
    s.run();
    CHECK(s.card->access_role() == tk::Role::Group);
    CHECK(s.card->access_name() == "Bob invited you to Cool Room");

    int accepted = 0, declined = 0, blocked = 0;
    s.card->on_accept = [&] { ++accepted; };
    s.card->on_decline = [&] { ++declined; };
    s.card->on_block = [&] { ++blocked; };

    REQUIRE(vt::press(*s.card, "Accept"));
    CHECK(accepted == 1);
    // Accept disables itself so a double-click can't send twice.
    CHECK_FALSE(vt::press(*s.card, "Accept"));
    CHECK(accepted == 1);
    REQUIRE(vt::press(*s.card, "Decline"));
    CHECK(declined == 1);
    CHECK_FALSE(vt::has_name(*s.card, "Block")); // hidden outside DMs
    CHECK(blocked == 0);

    // A fresh invite re-enables Accept.
    s.card->set_invite(ic_invite(false), nullptr);
    s.run();
    CHECK(vt::press(*s.card, "Accept"));
    CHECK(accepted == 2);
}

TEST_CASE("InviteCard direct-message invite shows Block and a DM name",
          "[tk][view][invite_card]")
{
    IcStage s;
    s.card->set_invite(ic_invite(true), nullptr);
    s.run();
    CHECK(s.card->access_name() == "@bob:x invited you to a direct message");
    int blocked = 0;
    s.card->on_block = [&] { ++blocked; };
    REQUIRE(vt::press(*s.card, "Block"));
    CHECK(blocked == 1);
}

TEST_CASE("InviteCard fetches avatars via the provider and shows a reason",
          "[tk][view][invite_card]")
{
    IcStage s;
    std::vector<std::string> asked;
    auto info = ic_invite(false);
    info.reason = "come join";
    s.card->set_invite(info, [&](const std::string& mxc) -> const tk::Image*
                       { asked.push_back(mxc); return nullptr; });
    s.run();
    CHECK_FALSE(asked.empty());

    auto dm = ic_invite(true);
    dm.reason = "hi there";
    s.card->set_invite(dm, nullptr);
    s.run();
    // Unnamed room falls back to the room id in the group variant.
    auto g = ic_invite(false);
    g.room_name.clear();
    g.inviter_display_name.clear();
    s.card->set_invite(g, nullptr);
    CHECK(s.card->access_name() == "@bob:x invited you to !r:x");
    s.run();
}
