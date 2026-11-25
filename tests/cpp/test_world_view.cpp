// WorldView: the 3D engine's picture on the canvas, and its walking input.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tk/widget.h"
#include "views/world/WorldView.h"
#include "views/world/world_room.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <cmath>

using Catch::Approx;
using tesseract::views::world::EngineScene;
using tesseract::views::world::MoveInput;
using tesseract::views::world::WorldView;

namespace
{

tk::KeyEvent physical(tk::PhysicalKey k, bool shift = false)
{
    tk::KeyEvent e;
    e.physical = k;
    e.shift = shift;
    return e;
}

// A click: with no host here the platform lock is unavailable, so the view falls back to the old
// controls (keys while focused, look by dragging) from the first click on.
void click(WorldView& v)
{
    v.on_pointer_down({1, 1});
    v.on_pointer_up({1, 1}, true);
}

// A WorldView whose host can (or cannot) lock the pointer, as a real shell's can.
struct LockingView
{
    explicit LockingView(bool supported)
    {
        host.lock_supported = supported;
        tk::detail::pending_host_stack().push_back(&host);
        view = std::make_unique<WorldView>();
        tk::detail::pending_host_stack().pop_back();
    }
    TestHost host{nullptr};
    std::unique_ptr<WorldView> view;
};

} // namespace

TEST_CASE("WorldView names every engine failure", "[world]")
{
    using F = EngineScene::Failure;
    CHECK(WorldView::failure_text(F::None).empty());
    for (F f : {F::LoadFailed, F::NoOffscreen, F::NoDevice, F::ImportFailed, F::SceneFailed})
    {
        CHECK_FALSE(WorldView::failure_text(f).empty());
    }
}

TEST_CASE("WorldView starts inactive and stops on deactivate", "[world]")
{
    WorldView v;
    CHECK_FALSE(v.active());
    v.set_active(true);
    CHECK(v.active());
    v.set_active(false); // no paint happened: nothing started, nothing to join
    CHECK_FALSE(v.active());
}

TEST_CASE("WorldView takes keyboard focus", "[world]")
{
    WorldView v;
    CHECK(v.focusable());
    CHECK(v.focus_on_click());
}

TEST_CASE("WorldView asks the player to walk from held physical keys", "[world]")
{
    WorldView v;
    click(v);
    auto in = v.take_input_for_test();
    CHECK_FALSE(
        (in.forward || in.back || in.left || in.right || in.strafe_left || in.strafe_right));

    CHECK(v.on_key_down(physical(tk::PhysicalKey::W)));
    CHECK(v.take_input_for_test().forward);
    CHECK(v.on_key_up(physical(tk::PhysicalKey::W)));
    CHECK_FALSE(v.take_input_for_test().forward);

    v.on_key_down(physical(tk::PhysicalKey::Up));
    CHECK(v.take_input_for_test().forward);
    v.on_key_up(physical(tk::PhysicalKey::Up));
    v.on_key_down(physical(tk::PhysicalKey::S));
    CHECK(v.take_input_for_test().back);
    v.on_key_up(physical(tk::PhysicalKey::S));
    v.on_key_down(physical(tk::PhysicalKey::Down));
    CHECK(v.take_input_for_test().back);
}

TEST_CASE("WorldView stops on keys released and on focus lost", "[world]")
{
    WorldView v;
    click(v);
    v.on_key_down(physical(tk::PhysicalKey::Up));
    v.on_keys_released();
    CHECK_FALSE(v.take_input_for_test().forward);

    v.on_key_down(physical(tk::PhysicalKey::S));
    v.on_focus_lost();
    CHECK_FALSE(v.take_input_for_test().back);
}

TEST_CASE("WorldView consumes only its movement keys", "[world]")
{
    WorldView v;
    tk::KeyEvent ctrl_f;
    ctrl_f.key = tk::Key::Character;
    ctrl_f.text = "f";
    ctrl_f.ctrl = true;
    CHECK_FALSE(v.on_key_down(ctrl_f));
    CHECK_FALSE(v.on_key_up(ctrl_f));

    tk::KeyEvent ctrl_w = physical(tk::PhysicalKey::W);
    ctrl_w.ctrl = true;
    CHECK_FALSE(v.on_key_down(ctrl_w)); // a shortcut, not a step

    click(v);
    CHECK(v.on_key_down(physical(tk::PhysicalKey::A)));
    CHECK(v.on_key_up(physical(tk::PhysicalKey::D))); // a release it never saw: harmless
}

TEST_CASE("WorldView ignores a character key without a physical position", "[world]")
{
    WorldView v;
    click(v);
    tk::KeyEvent w;
    w.key = tk::Key::Character;
    w.text = "w"; // another layout's "w": physical Unknown
    CHECK_FALSE(v.on_key_down(w));
    CHECK_FALSE(v.take_input_for_test().forward);
}

TEST_CASE("WorldView jumps while Space is held", "[world]")
{
    WorldView v;
    click(v);
    CHECK_FALSE(v.take_input_for_test().jump);
    v.on_key_down(physical(tk::PhysicalKey::Space));
    CHECK(v.take_input_for_test().jump);
    CHECK(v.take_input_for_test().jump); // held, not a one-shot
    v.on_key_up(physical(tk::PhysicalKey::Space));
    CHECK_FALSE(v.take_input_for_test().jump);
}

TEST_CASE("WorldView turns, strafes and looks", "[world]")
{
    WorldView v;
    click(v);

    // Turn: the arrow keys. Strafe: A and D.
    v.on_key_down(physical(tk::PhysicalKey::Left));
    CHECK(v.take_input_for_test().left);
    v.on_key_up(physical(tk::PhysicalKey::Left));
    v.on_key_down(physical(tk::PhysicalKey::Right));
    CHECK(v.take_input_for_test().right);
    v.on_keys_released();
    v.on_key_down(physical(tk::PhysicalKey::A));
    CHECK(v.take_input_for_test().strafe_left);
    v.on_keys_released();
    v.on_key_down(physical(tk::PhysicalKey::D));
    CHECK(v.take_input_for_test().strafe_right);
    v.on_keys_released();

    // Look: a drag right and down is 0.005 rad per logical pixel, handed over once.
    CHECK(v.on_pointer_down({10, 10}));
    v.on_pointer_drag({30, 20});
    v.on_pointer_up({30, 20}, true);
    const auto look = v.take_input_for_test();
    CHECK(look.look_dx == Approx(0.1f).margin(0.001f));
    CHECK(look.look_dy == Approx(0.05f).margin(0.001f));
    CHECK(v.take_input_for_test().look_dx == 0.0f);

    // A drag after release is nothing.
    v.on_pointer_drag({100, 100});
    CHECK(v.take_input_for_test().look_dx == 0.0f);
}

TEST_CASE("WorldView hiding the world forgets the held keys", "[world]")
{
    WorldView v;
    v.set_active(true);

    click(v);
    v.on_key_down(physical(tk::PhysicalKey::W));
    v.set_active(false); // hidden while W is held
    v.set_active(true);
    CHECK_FALSE(v.take_input_for_test().forward); // the held key was forgotten
}

TEST_CASE("WorldView ignores keys until it is clicked", "[world]")
{
    WorldView v;
    CHECK_FALSE(v.on_key_down(physical(tk::PhysicalKey::W)));
    CHECK_FALSE(v.take_input_for_test().forward);
}

TEST_CASE(
    "WorldView captures the mouse and keyboard on a click and Esc or Left Ctrl gives them back",
    "[world]")
{
    for (const auto release : {tk::Key::Escape, tk::Key::Unknown})
    {
        LockingView lv(true);
        WorldView& v = *lv.view;
        CHECK_FALSE(v.captured());
        CHECK_FALSE(v.on_key_down(physical(tk::PhysicalKey::W))); // not yet: a shortcut may use it

        click(v);
        REQUIRE(v.captured());
        CHECK(lv.host.pointer_locked());

        // Captured: movement keys move, and every other key is swallowed, ctrl chords included.
        CHECK(v.on_key_down(physical(tk::PhysicalKey::W)));
        CHECK(v.take_input_for_test().forward);
        tk::KeyEvent chord = physical(tk::PhysicalKey::A);
        chord.ctrl = true;
        CHECK(v.on_key_down(chord));
        tk::KeyEvent other;
        other.key = tk::Key::Character;
        other.text = "q";
        CHECK(v.on_key_down(other));

        // The mouse looks without a button: 0.005 rad per pixel.
        v.on_pointer_motion(20, 10);
        const auto look = v.take_input_for_test();
        CHECK(look.look_dx == Approx(0.1f).margin(0.001f));
        CHECK(look.look_dy == Approx(0.05f).margin(0.001f));

        // The release key lets go and is consumed; the held key is forgotten.
        tk::KeyEvent r;
        if (release == tk::Key::Escape)
        {
            r.key = tk::Key::Escape;
        }
        else
        {
            r = physical(tk::PhysicalKey::ControlLeft);
        }
        CHECK(v.on_key_down(r));
        CHECK_FALSE(v.captured());
        CHECK_FALSE(lv.host.pointer_locked());
        CHECK_FALSE(v.take_input_for_test().forward);
        CHECK_FALSE(v.on_key_down(physical(tk::PhysicalKey::W))); // inert again
    }
}

TEST_CASE("WorldView lets go when the platform ends the lock or the window loses focus", "[world]")
{
    LockingView lv(true);
    WorldView& v = *lv.view;
    click(v);
    REQUIRE(v.captured());
    v.on_pointer_lock_lost();
    CHECK_FALSE(v.captured());

    click(v);
    REQUIRE(v.captured());
    v.on_keys_released(); // the window lost the keyboard
    CHECK_FALSE(v.captured());
    CHECK_FALSE(lv.host.pointer_locked());
}

TEST_CASE("WorldView keeps the old controls when the platform cannot lock", "[world]")
{
    LockingView lv(false);
    WorldView& v = *lv.view;
    click(v);
    CHECK_FALSE(v.captured());
    CHECK(v.on_key_down(physical(tk::PhysicalKey::W))); // keys work while focused
    CHECK(v.take_input_for_test().forward);
    v.on_pointer_down({10, 10}); // and a drag looks
    v.on_pointer_drag({30, 10});
    CHECK(v.take_input_for_test().look_dx == Approx(0.1f).margin(0.001f));
}

TEST_CASE("WorldView drops the picture of a participant whose camera is off", "[world]")
{
    WorldView v;
    const auto bgra = std::make_shared<std::vector<std::uint8_t>>(4 * 4 * 4, 90);
    tesseract::RtcParticipantInfo remote;
    remote.participant_id = "remote";
    v.set_participants({remote}, "me");
    v.on_video_frame("remote", 4, 4, bgra);
    CHECK(v.has_video_for_test("remote"));

    remote.is_video_muted = true;
    v.set_participants({remote}, "me");
    CHECK_FALSE(v.has_video_for_test("remote"));
    v.on_video_frame("remote", 4, 4, bgra); // a late frame does not bring it back
    CHECK_FALSE(v.has_video_for_test("remote"));

    remote.is_video_muted = false;
    v.set_participants({remote}, "me");
    v.on_video_frame("remote", 4, 4, bgra);
    CHECK(v.has_video_for_test("remote"));
}

namespace
{
tk::KeyEvent bar_key_event()
{
    tk::KeyEvent e;
    e.key = tk::Key::Character;
    e.text = "b";
    return e;
}
} // namespace

TEST_CASE(
    "WorldView's bar shortcut lets go of the mouse, shows the buttons and takes the keys back",
    "[world][button bar]")
{
    auto surface = TestSurface::create(400, 300);
    LockingView lv(true);
    WorldView& v = *lv.view;
    v.set_actions({{"import", "Import", true}, {"export", "Export", true}},
                  [](const std::string&) {});
    v.set_active(true);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    v.arrange(lc, {0, 0, 400, 300});
    click(v);
    REQUIRE(v.captured());

    CHECK(v.on_key_down(bar_key_event()));
    CHECK_FALSE(v.captured());
    CHECK(v.button_bar()->expanded());
    CHECK(lv.host.focused_widget() == v.button_bar()->entry_button("import"));

    // Closing it hands the keyboard back to the world.
    CHECK(v.on_key_down(bar_key_event()));
    CHECK_FALSE(v.button_bar()->expanded());
    CHECK(lv.host.focused_widget() == &v);
}

TEST_CASE("WorldView's bar shortcut does nothing while the world is not on screen",
          "[world][button bar]")
{
    auto surface = TestSurface::create(400, 300);
    LockingView lv(true);
    WorldView& v = *lv.view;
    v.set_actions({{"import", "Import", true}}, [](const std::string&) {});
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    v.arrange(lc, {0, 0, 400, 300});
    CHECK_FALSE(v.on_key_down(bar_key_event())); // parked: not active
    CHECK_FALSE(v.button_bar()->expanded());
    CHECK(lv.host.focused_widget() == nullptr);

    v.set_active(true);
    v.arrange(lc, {0, 0, 0, 0}); // no room on screen
    CHECK_FALSE(v.on_key_down(bar_key_event()));
    CHECK_FALSE(v.button_bar()->expanded());
}

TEST_CASE("WorldView paints the bar handle over the picture, not just its glyph",
          "[world][button bar]")
{
    auto surface = TestSurface::create(400, 300);
    LockingView lv(true);
    WorldView& v = *lv.view;
    v.set_actions({{"import", "Import", true}}, [](const std::string&) {});
    v.set_active(true);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    v.arrange(lc, {0, 0, 400, 300});
    tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
    pc.host = &lv.host;
    v.paint(pc);
    const tk::Rect hb = v.button_bar()->handle_button()->bounds();
    const tk::Color bg = surface->read_pixel(300, 150);
    int differing = 0;
    for (int y = static_cast<int>(hb.y); y < static_cast<int>(hb.y + hb.h); ++y)
    {
        for (int x = static_cast<int>(hb.x); x < static_cast<int>(hb.x + hb.w); ++x)
        {
            const tk::Color c = surface->read_pixel(x, y);
            if (c.r != bg.r || c.g != bg.g || c.b != bg.b)
            {
                ++differing;
            }
        }
    }
    INFO("differing pixels in the handle rect: " << differing << " of " << hb.w * hb.h);
    CHECK(differing > 600);
}

TEST_CASE("WorldView puts the bar in the corner and hides it, closed, while asked to",
          "[world][button bar]")
{
    auto surface = TestSurface::create(400, 300);
    LockingView lv(true);
    WorldView& v = *lv.view;
    v.set_actions({{"import", "Import", true}}, [](const std::string&) {});
    v.set_active(true);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    v.arrange(lc, {0, 0, 400, 300});
    const tk::Rect hb = v.button_bar()->handle_button()->bounds();
    CHECK(hb.x == 8.0f);
    CHECK(hb.y == 8.0f);

    CHECK(v.on_key_down(bar_key_event()));
    REQUIRE(v.button_bar()->expanded());
    v.set_bar_hidden(true);
    CHECK_FALSE(v.button_bar()->expanded());
    CHECK_FALSE(v.button_bar()->visible());
    CHECK_FALSE(v.on_key_down(bar_key_event())); // no shortcut while hidden
    CHECK_FALSE(v.button_bar()->expanded());

    v.set_bar_hidden(false);
    CHECK(v.button_bar()->visible());
    CHECK(v.on_key_down(bar_key_event()));
    CHECK(v.button_bar()->expanded());
}
