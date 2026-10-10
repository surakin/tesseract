#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/CallOverlayWidget.h"

#include <tesseract/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using tesseract::views::CallOverlayWidget;

namespace
{

tesseract::RtcParticipantInfo cow_part(const std::string& pid, const std::string& uid,
                                       bool amute = false, bool vmute = false,
                                       bool screen = false)
{
    tesseract::RtcParticipantInfo p;
    p.participant_id = pid;
    p.user_id = uid;
    p.is_audio_muted = amute;
    p.is_video_muted = vmute;
    p.is_screen_sharing = screen;
    return p;
}

struct CowStage : vt::Stage
{
    std::unique_ptr<CallOverlayWidget> ov = tk::create_root_widget<CallOverlayWidget>(&host);
    int repaints = 0;
    int relayouts = 0;
    std::vector<std::pair<int, std::function<void()>>> delayed;
    CowStage()
    {
        ov->set_repaint_requester([this] { ++repaints; });
        ov->set_relayout_requester([this] { ++relayouts; });
        ov->set_post_delayed([this](int ms, std::function<void()> f)
                             { delayed.push_back({ms, std::move(f)}); });
        ov->set_local_user_id("@me:x");
        ov->set_display_name_provider([](const std::string& uid)
                                      { return uid == "@bob:x" ? std::string("Bob") : std::string(); });
        ov->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    }
    void run(tk::Rect r = {0, 0, 640, 480})
    {
        layout(*ov, r);
        paint(*ov);
    }
};

} // namespace

TEST_CASE("CallOverlayWidget builds one tile per participant plus screen "
          "shares and removes departed ones",
          "[tk][view][call_overlay]")
{
    CowStage s;
    s.ov->update_participants({cow_part("p1", "@me:x"), cow_part("p2", "@bob:x", true, true),
                               cow_part("p3", "@eve:x", false, false, true)});
    CHECK(s.relayouts >= 1);
    s.run();
    // Eve also gets a ":screen" tile.
    CHECK(vt::find_all<tesseract::views::ParticipantTile>(*s.ov).size() == 4);

    // Eve stops sharing: only her screen tile goes; Bob leaves entirely.
    s.ov->update_participants({cow_part("p1", "@me:x"), cow_part("p3", "@eve:x")});
    s.run();
    CHECK(vt::find_all<tesseract::views::ParticipantTile>(*s.ov).size() == 2);

    // Frames reach tiles by id; unknown ids are dropped.
    auto frame = std::make_shared<std::vector<std::uint8_t>>(4 * 4 * 4, 100);
    s.ov->on_video_frame("p1", 4, 4, frame);
    s.ov->on_video_frame("nope", 4, 4, frame);
    s.ov->on_screen_frame("p3:screen", 4, 4, frame);
    s.ov->on_screen_frame("nope:screen", 4, 4, frame);
    s.run();
}

TEST_CASE("CallOverlayWidget pinning a tile re-lays out with a pinned region",
          "[tk][view][call_overlay]")
{
    CowStage s;
    s.ov->update_participants({cow_part("p1", "@me:x"), cow_part("p2", "@bob:x"),
                               cow_part("p3", "@eve:x")});
    s.run();
    auto tiles = vt::find_all<tesseract::views::ParticipantTile>(*s.ov);
    REQUIRE(tiles.size() == 3);
    const float before = tiles[1]->bounds().w;
    tiles[1]->on_pin_toggled("p2"); // pin
    CHECK(s.relayouts >= 2);
    s.run({0, 0, 900, 400}); // wide: pinned on the left
    CHECK(tiles[1]->bounds().w > before);
    s.run({0, 0, 400, 900}); // tall: pinned on top
    tiles[1]->on_pin_toggled("p2"); // unpin
    s.run();
    CHECK(tiles[1]->bounds().w == before);

    // Without a relayout requester, repaint is requested instead.
    s.ov->set_relayout_requester(nullptr);
    const int rp = s.repaints;
    tiles[0]->on_pin_toggled("p1");
    CHECK(s.repaints > rp);
}

TEST_CASE("CallOverlayWidget controls fire callbacks via accessible names",
          "[tk][view][call_overlay]")
{
    CowStage s;
    s.ov->update_participants({cow_part("p1", "@me:x")});
    s.run();
    bool muted = false, vmuted = false, sharing = false;
    int hangups = 0;
    s.ov->on_toggle_audio = [&](bool m) { muted = m; };
    s.ov->on_toggle_video = [&](bool m) { vmuted = m; };
    s.ov->on_toggle_screen_share = [&](bool v) { sharing = v; };
    s.ov->on_hang_up = [&] { ++hangups; };

    REQUIRE(vt::press(*s.ov, "Mute microphone"));
    CHECK(muted);
    s.run();
    CHECK(vt::has_name(*s.ov, "Unmute microphone"));
    REQUIRE(vt::press(*s.ov, "Turn camera off"));
    CHECK(vmuted);
    s.run();
    CHECK(vt::has_name(*s.ov, "Turn camera on"));
    REQUIRE(vt::press(*s.ov, "Share screen"));
    CHECK(sharing);
    s.run();
    CHECK(vt::has_name(*s.ov, "Stop sharing screen"));
    REQUIRE(vt::press(*s.ov, "Leave call"));
    CHECK(hangups == 1);

    s.ov->set_show_video_button(false);
    s.run();
    CHECK_FALSE(vt::has_name(*s.ov, "Turn camera on"));
    s.ov->set_audio_muted(false);
    s.ov->set_video_muted(false);
    s.ov->set_screen_sharing(false);
}

TEST_CASE("CallOverlayWidget mode buttons request docked/floating/popout "
          "transitions",
          "[tk][view][call_overlay]")
{
    CowStage s;
    std::vector<CallOverlayWidget::Mode> req;
    s.ov->on_mode_change_requested = [&](CallOverlayWidget::Mode m) { req.push_back(m); };

    s.ov->set_mode(CallOverlayWidget::Mode::Docked);
    s.run();
    REQUIRE(vt::press(*s.ov, "Expand call"));
    s.ov->set_mode(CallOverlayWidget::Mode::DockedExpanded);
    s.run();
    REQUIRE(vt::press(*s.ov, "Collapse call"));
    s.ov->click_pip_button_for_test(); // expanded -> floating
    s.ov->set_mode(CallOverlayWidget::Mode::Floating);
    s.run();
    s.ov->click_pip_button_for_test(); // floating -> popout
    s.ov->set_mode(CallOverlayWidget::Mode::Popout);
    s.run();
    REQUIRE(vt::press(*s.ov, "Dock call"));
    using M = CallOverlayWidget::Mode;
    CHECK(req == std::vector<M>{M::DockedExpanded, M::Docked, M::Floating, M::Popout, M::Docked});

    // Measure per mode.
    auto lc = s.lc();
    s.ov->set_mode(M::Docked);
    CHECK(s.ov->measure(lc, {640, 480}).h == 220);
    s.ov->set_mode(M::DockedExpanded);
    CHECK(s.ov->measure(lc, {640, 480}).h == 480);
    s.ov->set_mode(M::Popout);
    CHECK(s.ov->measure(lc, {640, 480}).w == 640);
    s.ov->set_mode(M::Floating);
    CHECK(s.ov->measure(lc, {640, 480}).w == 320);
    CHECK(s.ov->mode() == M::Floating);
}

TEST_CASE("CallOverlayWidget floating window drags by its header and moves "
          "with the arrow keys",
          "[tk][view][call_overlay]")
{
    CowStage s;
    s.ov->set_mode(CallOverlayWidget::Mode::Floating);
    s.ov->set_float_position(100, 50);
    CHECK(s.ov->float_position().x == 100);
    s.run({100, 50, 320, 240});
    std::vector<tk::Point> pos;
    s.ov->on_float_position_changed = [&](float x, float y) { pos.push_back({x, y}); };

    CHECK_FALSE(s.ov->on_pointer_down({10, 100})); // below the header
    s.ov->on_pointer_drag({20, 20});               // not dragging: ignored
    CHECK(pos.empty());
    CHECK(s.ov->on_pointer_down({10, 10}));
    s.ov->on_pointer_drag({40, 30});
    REQUIRE(pos.size() == 1);
    CHECK(pos[0].x == 130);
    CHECK(pos[0].y == 70);
    s.ov->on_pointer_up({40, 30}, false);
    s.ov->on_pointer_drag({60, 60});
    CHECK(pos.size() == 1);

    // Keyboard move via the header's focus target.
    auto* target = vt::find<tk::KeyboardTarget>(*s.ov);
    REQUIRE(target != nullptr);
    CHECK(target->on_key(vt::key(tk::Key::Right)));
    CHECK(target->on_key(vt::key(tk::Key::Down, false, true)));
    CHECK_FALSE(target->on_key(vt::key(tk::Key::Space)));
    CHECK_FALSE(target->on_key(vt::key(tk::Key::Left, true)));
    CHECK(pos.size() == 3);
    CHECK(pos[1].x == 100 + 16);
    s.ov->set_mode(CallOverlayWidget::Mode::Docked);
    CHECK_FALSE(target->on_key(vt::key(tk::Key::Left)));
}

TEST_CASE("CallOverlayWidget timer ticks via post_delayed, snapshot/restore "
          "round-trips state",
          "[tk][view][call_overlay]")
{
    CowStage s;
    s.ov->start_timer(3661.0); // 1:01:01
    CHECK(s.ov->elapsed_seconds() >= 3661.0);
    REQUIRE(s.delayed.size() == 1);
    CHECK(s.delayed[0].first == 1000);
    const int rp = s.repaints;
    auto first = std::move(s.delayed[0].second);
    s.delayed.clear();
    first();
    CHECK(s.repaints > rp);
    CHECK(s.delayed.size() == 1); // re-armed

    s.run();
    s.ov->stop_timer();
    auto stale = std::move(s.delayed[0].second);
    s.delayed.clear();
    stale(); // generation changed: no re-arm
    CHECK(s.delayed.empty());

    s.ov->set_audio_muted(true);
    s.ov->set_video_muted(true);
    s.ov->set_show_video_button(false);
    s.ov->set_screen_sharing(true);
    const auto snap = s.ov->snapshot();
    CHECK(snap.audio_muted);
    CHECK(snap.screen_sharing);
    CHECK_FALSE(snap.show_video_button);
    CHECK(snap.local_user_id == "@me:x");

    CowStage other;
    other.ov->restore(snap);
    const auto got = other.ov->snapshot();
    CHECK(got.audio_muted);
    CHECK(got.video_muted);
    CHECK(got.screen_sharing);
    CHECK(got.elapsed_seconds >= snap.elapsed_seconds);
    other.run();
}

TEST_CASE("CallOverlayWidget announces joins/leaves only after the roster "
          "settles; room_active toggles the expand button",
          "[tk][view][call_overlay]")
{
    CowStage s;
    s.ov->update_participants({cow_part("p1", "@me:x")}); // baseline
    s.ov->update_participants({cow_part("p1", "@me:x"), cow_part("p2", "@bob:x")});
    // Within the settle window nothing is announced; just no crash.
    s.ov->update_participants({cow_part("p1", "@me:x")});
    s.ov->set_room_active(false);
    CHECK_FALSE(s.ov->expand_button_visible_for_test());
    s.ov->set_room_active(true);
    CHECK(s.ov->room_active());
    s.ov->set_mode(CallOverlayWidget::Mode::Docked);
    CHECK(s.ov->expand_button_visible_for_test());
}
