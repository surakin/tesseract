#include <catch2/catch_test_macros.hpp>

#include "tk/host.h"
#include "tk/widget.h"
#include "tk_test_host.h"

#include <memory>

// Exercises the shared Host::dispatch_file_drop tree walk (mirrors
// test_tk_host_pointer.cpp's coverage of dispatch_pointer_down) and the
// FileDropPayload ownership contract: a rejecting on_file_drop must leave
// the payload untouched so an ancestor/sibling can still claim it; an
// accepting one moves out of it as its last action.

using namespace tk;

namespace
{

class DropProbeWidget : public Widget
{
public:
    DropProbeWidget(Rect rect, bool claim_drop = false) : claim_drop_(claim_drop)
    {
        bounds_ = rect; // world bounds drive contains_world / the tree walk
    }

    Size measure(LayoutCtx&, Size) override
    {
        return {bounds_.w, bounds_.h};
    }
    void paint(PaintCtx&) override {}

    bool on_file_drop(Point local, FileDropPayload& payload) override
    {
        ++drop_count;
        last_local = local;
        if (!claim_drop_)
            return false; // must not touch `payload`
        last_bytes = payload.bytes;
        last_mime = payload.mime;
        last_filename = payload.filename;
        payload.bytes.clear(); // simulate taking ownership via std::move
        payload.mime.clear();
        payload.filename.clear();
        return true;
    }

    bool on_drag_hover(Point local) override
    {
        ++hover_count;
        last_hover_local = local;
        return claim_drop_;
    }
    void on_drag_leave() override { ++leave_count; }

    int drop_count = 0;
    Point last_local{};
    std::vector<std::uint8_t> last_bytes;
    std::string last_mime;
    std::string last_filename;

    int hover_count = 0;
    int leave_count = 0;
    Point last_hover_local{};

private:
    bool claim_drop_;
};

FileDropPayload make_payload()
{
    return FileDropPayload{{1, 2, 3}, "image/png", "sticker.png"};
}

} // namespace

TEST_CASE("File drop inside a claiming child reaches it and moves the "
          "payload out",
          "[tk][host][drop]")
{
    DropProbeWidget root({0, 0, 400, 400});
    auto* child =
        root.add_child(std::make_unique<DropProbeWidget>(
            Rect{100, 100, 100, 100}, /*claim_drop=*/true));
    TestHost host(&root);

    FileDropPayload payload = make_payload();
    Widget* target = host.dispatch_file_drop({150, 150}, payload);

    REQUIRE(target == child);
    CHECK(child->drop_count == 1);
    CHECK(child->last_local.x == 50);
    CHECK(child->last_local.y == 50);
    CHECK(child->last_bytes == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(child->last_mime == "image/png");
    CHECK(child->last_filename == "sticker.png");
    CHECK(root.drop_count == 0); // child claimed it before self was tried

    // Accepting moved out of the payload — caller no longer owns the bytes.
    CHECK(payload.bytes.empty());
}

TEST_CASE("File drop on a non-claiming widget leaves the payload untouched",
          "[tk][host][drop]")
{
    DropProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);

    FileDropPayload payload = make_payload();
    Widget* target = host.dispatch_file_drop({10, 10}, payload);

    REQUIRE(target == nullptr);
    CHECK(root.drop_count == 1); // tried and rejected
    CHECK(payload.bytes == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(payload.mime == "image/png");
    CHECK(payload.filename == "sticker.png");
}

TEST_CASE("File drop outside every widget's bounds returns nullptr and "
          "leaves the payload untouched",
          "[tk][host][drop]")
{
    DropProbeWidget root({100, 100, 100, 100});
    TestHost host(&root);

    FileDropPayload payload = make_payload();
    Widget* target = host.dispatch_file_drop({10, 10}, payload);

    REQUIRE(target == nullptr);
    CHECK(root.drop_count == 0); // never reached — contains_world failed first
    CHECK(payload.bytes == std::vector<std::uint8_t>{1, 2, 3});
}

TEST_CASE("An invisible ancestor skips the whole subtree even though a "
          "descendant would otherwise claim",
          "[tk][host][drop]")
{
    DropProbeWidget root({0, 0, 400, 400});
    auto* hidden_container =
        root.add_child(std::make_unique<DropProbeWidget>(Rect{100, 100, 200, 200}));
    auto* grandchild =
        hidden_container->add_child(std::make_unique<DropProbeWidget>(
            Rect{150, 150, 50, 50}, /*claim_drop=*/true));
    hidden_container->set_visible(false);
    TestHost host(&root);

    FileDropPayload payload = make_payload();
    Widget* target = host.dispatch_file_drop({160, 160}, payload);

    REQUIRE(target == nullptr);
    CHECK(grandchild->drop_count == 0);   // whole subtree skipped
    CHECK(hidden_container->drop_count == 0);
    CHECK(root.drop_count == 1);          // falls back to root, which rejects
    CHECK(payload.bytes == std::vector<std::uint8_t>{1, 2, 3});
}

// Host::dispatch_drag_hover / dispatch_drag_leave — the per-widget
// replacement for the old whole-surface "Drop to attach" overlay. Mirrors
// dispatch_file_drop's claim-based shape (children topmost-first, self
// last) but tracks a persistent claimant across calls instead of resolving
// once per event, firing on_drag_leave when the claim changes.

TEST_CASE("Drag hover claiming a widget requests no extra leave on the "
          "very first call",
          "[tk][host][drag_hover]")
{
    DropProbeWidget root({0, 0, 400, 400}, /*claim_drop=*/true);
    TestHost host(&root);

    Widget* target = host.dispatch_drag_hover({10, 10});

    REQUIRE(target == &root);
    CHECK(root.hover_count == 1);
    CHECK(root.leave_count == 0);
    CHECK(host.drag_hovered_widget_.lock().get() == &root);
}

TEST_CASE("Drag hover moving between two claiming widgets fires "
          "on_drag_leave on the previous claimant",
          "[tk][host][drag_hover]")
{
    DropProbeWidget root({0, 0, 400, 400});
    auto* a = root.add_child(
        std::make_unique<DropProbeWidget>(Rect{0, 0, 100, 100}, true));
    auto* b = root.add_child(
        std::make_unique<DropProbeWidget>(Rect{200, 0, 100, 100}, true));
    TestHost host(&root);

    REQUIRE(host.dispatch_drag_hover({50, 50}) == a);
    CHECK(a->hover_count == 1);
    CHECK(a->leave_count == 0);

    REQUIRE(host.dispatch_drag_hover({250, 50}) == b);
    CHECK(a->leave_count == 1);  // a lost the claim
    CHECK(b->hover_count == 1);
    CHECK(b->leave_count == 0);
}

TEST_CASE("Drag hover moving off every widget clears the claim and fires "
          "on_drag_leave",
          "[tk][host][drag_hover]")
{
    DropProbeWidget root({0, 0, 400, 400});
    auto* a = root.add_child(
        std::make_unique<DropProbeWidget>(Rect{0, 0, 100, 100}, true));
    TestHost host(&root);

    REQUIRE(host.dispatch_drag_hover({50, 50}) == a);
    REQUIRE(host.dispatch_drag_hover({300, 300}) == nullptr);

    CHECK(a->leave_count == 1);
    CHECK(host.drag_hovered_widget_.expired());
}

TEST_CASE("dispatch_drag_leave clears an active claim", "[tk][host][drag_hover]")
{
    DropProbeWidget root({0, 0, 400, 400}, /*claim_drop=*/true);
    TestHost host(&root);

    REQUIRE(host.dispatch_drag_hover({10, 10}) == &root);
    host.dispatch_drag_leave();

    CHECK(root.leave_count == 1);
    CHECK(host.drag_hovered_widget_.expired());

    // Idempotent — a second leave with no active claim is a no-op.
    host.dispatch_drag_leave();
    CHECK(root.leave_count == 1);
}

TEST_CASE("A non-claiming widget still receives on_drag_hover but never "
          "becomes the claimant",
          "[tk][host][drag_hover]")
{
    DropProbeWidget root({0, 0, 400, 400}, /*claim_drop=*/false);
    TestHost host(&root);

    Widget* target = host.dispatch_drag_hover({10, 10});

    REQUIRE(target == nullptr);
    CHECK(root.hover_count == 1);
    CHECK(root.leave_count == 0);
}
