#include <catch2/catch_test_macros.hpp>

#include "tk/audio.h"
#include "tk/audio_capture.h"
#include "tk/host.h"
#include "tk/video.h"
#include "tk/widget.h"

#include <memory>

// Exercises the shared Host::dispatch_pointer_* state machine + popup routing
// (Phase 4.1: the logic was previously copy-pasted across all four host
// backends, including the Phase-1.7 popup branch with no test coverage). A
// minimal concrete Host stubs the platform-specific virtuals and exposes the
// protected dispatch entry points so we can drive them directly. ProbeWidget
// relies on the base Widget tree-walk (hit_test / dispatch_*) and only fixes
// its world bounds + records the leaf callbacks it receives.

using namespace tk;

namespace
{

class ProbeWidget : public Widget
{
public:
    ProbeWidget(Rect rect, bool claim_down = false) : claim_down_(claim_down)
    {
        bounds_ = rect; // world bounds drive contains_world / hit_test descent
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}

    bool on_pointer_down(Point) override
    {
        ++down_count;
        return claim_down_;
    }
    void on_pointer_up(Point, bool inside) override
    {
        ++up_count;
        last_up_inside = inside;
    }
    void on_pointer_drag(Point) override { ++drag_count; }
    bool on_pointer_move(Point) override
    {
        ++move_count;
        return false;
    }
    void on_pointer_leave() override { ++leave_count; }
    void on_popup_dismiss() override { ++dismiss_count; }

    int down_count = 0;
    int up_count = 0;
    int drag_count = 0;
    int move_count = 0;
    int leave_count = 0;
    int dismiss_count = 0;
    bool last_up_inside = false;

private:
    bool claim_down_;
};

// Minimal concrete Host: stubs every platform virtual, points input_root_ at a
// supplied root, and re-exposes the protected dispatch_pointer_* methods.
class TestHost : public Host
{
public:
    explicit TestHost(Widget* root) : root_(root) {}

    void request_repaint() override { ++repaint_count; }
    void post_to_ui(std::function<void()>) override {}
    void post_delayed(int, std::function<void()>) override {}
    std::unique_ptr<NativeTextField> make_text_field() override
    {
        return nullptr;
    }
    std::unique_ptr<NativeTextArea> make_text_area() override
    {
        return nullptr;
    }
    std::unique_ptr<AudioPlayer> make_audio_player() override
    {
        return nullptr;
    }
    std::unique_ptr<AudioCapture> make_audio_capture() override
    {
        return nullptr;
    }
    std::unique_ptr<VideoPlayer> make_video_player() override
    {
        return nullptr;
    }
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<AudioPlayback> make_audio_playback() override
    {
        return nullptr;
    }
#endif
    EncodedImage encode_for_send(const std::uint8_t*, std::size_t,
                                 bool) override
    {
        return {};
    }
    void set_clipboard_text(std::string_view) override {}
    bool set_clipboard_image(std::span<const std::uint8_t>) override
    {
        return false;
    }

    // Re-expose the protected shared dispatch + tracked state for the test.
    using Host::dispatch_pointer_down;
    using Host::dispatch_pointer_leave;
    using Host::dispatch_pointer_move;
    using Host::dispatch_pointer_up;
    using Host::hovered_widget_;
    using Host::pressed_widget_;

    // Drive the popup the way a paint pass would: register then promote.
    void set_active_popup(Widget* w)
    {
        register_popup(w);
        popup_ = pending_popup_;
    }

    int repaint_count = 0;

protected:
    Widget* input_root_() const override { return root_; }

private:
    Widget* root_;
};

} // namespace

TEST_CASE("Click outside an open popup dismisses it and falls through",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_down({10, 10}); // outside popup, inside root

    REQUIRE(host.popup() == nullptr);  // popup cleared
    REQUIRE(popup.dismiss_count == 1); // dismiss fired exactly once
    REQUIRE(popup.down_count == 0);    // popup never saw the press
    REQUIRE(root.down_count == 1);     // press fell through to the tree
}

TEST_CASE("Click inside an open popup routes to it and does not dismiss",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget popup({100, 100, 100, 100}, /*claim_down=*/true);
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_down({120, 120}); // inside the popup

    REQUIRE(popup.down_count == 1);
    REQUIRE(popup.dismiss_count == 0);
    REQUIRE(host.popup() == &popup);         // still open
    REQUIRE(root.down_count == 0);           // tree never saw it
    REQUIRE(host.pressed_widget_ == &popup); // popup captured the press
}

TEST_CASE("Pointer-move inside an open popup routes hover to it, not the tree",
          "[tk][host][popup]")
{
    ProbeWidget root({0, 0, 400, 400});
    ProbeWidget popup({100, 100, 100, 100});
    TestHost host(&root);
    host.set_active_popup(&popup);

    host.dispatch_pointer_move({130, 130}); // inside the popup

    REQUIRE(popup.move_count == 1);
    REQUIRE(root.move_count == 0);
    REQUIRE(host.hovered_widget_ == &popup);
}

TEST_CASE("Captured widget gets drag on move and inside-release on up",
          "[tk][host][popup]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* child =
        root->add_child(std::make_unique<ProbeWidget>(
            Rect{50, 50, 100, 100}, /*claim_down=*/true));
    TestHost host(root.get());

    host.dispatch_pointer_down({60, 60}); // press inside the claiming child
    REQUIRE(host.pressed_widget_ == child);

    host.dispatch_pointer_move({70, 70}); // captured → drag, no hover
    REQUIRE(child->drag_count == 1);
    REQUIRE(child->move_count == 0);

    host.dispatch_pointer_up({80, 80}); // inside child's bounds
    REQUIRE(child->up_count == 1);
    REQUIRE(child->last_up_inside == true);
    REQUIRE(host.pressed_widget_ == nullptr);
}

TEST_CASE("Pointer-leave clears hover and synthesises an outside release",
          "[tk][host][popup]")
{
    auto root = std::make_unique<ProbeWidget>(Rect{0, 0, 400, 400});
    ProbeWidget* child =
        root->add_child(std::make_unique<ProbeWidget>(
            Rect{50, 50, 100, 100}, /*claim_down=*/true));
    TestHost host(root.get());

    host.dispatch_pointer_down({60, 60});
    REQUIRE(host.pressed_widget_ == child);

    host.dispatch_pointer_leave();
    REQUIRE(child->up_count == 1);
    REQUIRE(child->last_up_inside == false); // synthetic outside release
    REQUIRE(host.pressed_widget_ == nullptr);
}
