#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/ParticipantTile.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using tesseract::views::ParticipantTile;

namespace
{

struct PtStage : vt::Stage
{
    std::unique_ptr<ParticipantTile> tile = tk::create_root_widget<ParticipantTile>(&host);
    int repaints = 0;
    PtStage()
    {
        tile->set_repaint_requester([this] { ++repaints; });
    }
    void run() { mount(*tile, {0, 0, 320, 240}); }
};

ParticipantTile::State pt_state(const std::string& id)
{
    ParticipantTile::State s;
    s.participant_id = id;
    s.user_id = "@" + id + ":x";
    s.display_name = "Person " + id;
    return s;
}

} // namespace

TEST_CASE("ParticipantTile paints avatar, video, mute badges and pin states",
          "[tk][view][participant_tile]")
{
    PtStage s;
    s.tile->set_avatar_provider([](const std::string&) -> const tk::Image* { return nullptr; });
    s.tile->set_state(pt_state("p1"));
    CHECK(s.repaints >= 1);
    s.run(); // no video yet: avatar placeholder

    auto muted = pt_state("p1");
    muted.audio_muted = true;
    muted.video_muted = true;
    s.tile->set_state(muted);
    s.run();

    auto st = pt_state("p1");
    st.is_self = true;
    s.tile->set_state(st);
    s.tile->push_video_frame(4, 2, std::make_shared<std::vector<std::uint8_t>>(4 * 2 * 4, 80));
    s.run(); // wide frame
    s.tile->push_video_frame(2, 4, std::make_shared<std::vector<std::uint8_t>>(2 * 4 * 4, 80));
    s.run(); // tall frame

    auto screen = pt_state("p1:screen");
    screen.is_screen_share_tile = true;
    s.tile->set_state(screen);
    s.run();

    s.tile->set_pinned(true);
    s.run(); // pinned badge when not hovered
    CHECK(s.tile->pin_target() != nullptr);

    // Zero-size tile paints nothing.
    s.relayout_paint(*s.tile, {0, 0, 0, 0});
}

TEST_CASE("ParticipantTile hover reveals the pin button; clicking it toggles",
          "[tk][view][participant_tile]")
{
    PtStage s;
    s.tile->set_state(pt_state("p9"));
    s.run();
    std::vector<std::string> pins;
    s.tile->on_pin_toggled = [&](const std::string& id) { pins.push_back(id); };

    s.tile->on_pointer_down({4, 4}); // before any layout of the pin: no crash
    s.tile->on_pointer_up({4, 4}, false);
    CHECK(s.tile->on_pointer_move({100, 100}));
    CHECK_FALSE(s.tile->on_pointer_move({101, 100})); // same state
    s.run();

    // Locate the pin button by probing (its exact spot depends on letterboxing).
    tk::Point pin{-1, -1};
    for (float y = 0; y < 240 && pin.x < 0; y += 2)
        for (float x = 0; x < 320; x += 2)
            if (s.tile->on_pointer_down({x, y}))
            {
                pin = {x, y};
                s.tile->on_pointer_up({x, y}, false);
                break;
            }
    REQUIRE(pin.x >= 0);
    pin.x += 2;
    pin.y += 2;
    CHECK(s.tile->on_pointer_down(pin));
    s.tile->on_pointer_up(pin, true);
    CHECK(pins == std::vector<std::string>{"p9"});

    // Press on the pin, release elsewhere / outside: no toggle.
    s.tile->on_pointer_down(pin);
    s.tile->on_pointer_up({200, 150}, true);
    s.tile->on_pointer_down(pin);
    s.tile->on_pointer_up(pin, false);
    CHECK(pins.size() == 1);
    CHECK_FALSE(s.tile->on_pointer_down({200, 150}));

    s.tile->on_pointer_leave();
    s.run();
    s.tile->on_pointer_up({1, 1}, true); // nothing pressed: no-op

    // Keyboard activation of the pin target toggles too.
    s.tile->pin_target()->on_activate();
    CHECK(pins.size() == 2);
}
