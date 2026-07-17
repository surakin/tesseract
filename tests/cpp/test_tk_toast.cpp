#include <catch2/catch_test_macros.hpp>

#include "tk/host.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>

// Exercises Host's toast API: show_toast()'s immediate-visibility +
// generation-counter-guarded auto-hide timer (post_delayed has no
// cancellation, mirroring the tooltip dwell-delay's own guard), and
// paint_toast_overlay()'s no-op-when-nothing-shown behavior. Mirrors the
// style of test_tk_tooltip.cpp, using the same shared TestHost fake.

using namespace tk;

namespace
{

class ToastProbeWidget : public Widget
{
public:
    explicit ToastProbeWidget(Rect rect) { bounds_ = rect; }

    Size measure(LayoutCtx&, Size) override { return {bounds_.w, bounds_.h}; }
    void paint(PaintCtx&) override {}
};

} // namespace

TEST_CASE("show_toast becomes visible immediately and schedules an auto-hide",
          "[tk][host][toast]")
{
    ToastProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);

    host.show_toast("Copied to clipboard");

    CHECK(host.toast_visible_); // unlike a tooltip, no dwell delay
    CHECK(host.toast_message_ == "Copied to clipboard");
    REQUIRE(host.pending_delays_.size() == 1);
    CHECK(host.pending_delays_[0].ms == TestHost::kToastDurationMs);

    host.fire_all_delays();

    CHECK_FALSE(host.toast_visible_);
}

TEST_CASE("a second show_toast call before the first's delay fires "
          "supersedes it",
          "[tk][host][toast]")
{
    ToastProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);

    host.show_toast("first");
    REQUIRE(host.pending_delays_.size() == 1);
    auto stale_first_delay = host.pending_delays_[0].fn;

    host.show_toast("second");
    CHECK(host.toast_visible_);
    CHECK(host.toast_message_ == "second");
    REQUIRE(host.pending_delays_.size() == 2);

    // The first call's now-stale timer firing must not hide the second
    // message's toast.
    stale_first_delay();
    CHECK(host.toast_visible_);
    CHECK(host.toast_message_ == "second");

    // The second call's own timer does hide it.
    host.pending_delays_[1].fn();
    CHECK_FALSE(host.toast_visible_);
}

TEST_CASE("paint_toast_overlay is a no-op when nothing has been shown",
          "[tk][host][toast]")
{
    ToastProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    auto surface = TestSurface::create(400, 400);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};

    REQUIRE_NOTHROW(host.paint_toast_overlay(pc, {0, 0, 400, 400}));
}

TEST_CASE("paint_toast_overlay draws without crashing once shown",
          "[tk][host][toast]")
{
    ToastProbeWidget root({0, 0, 400, 400});
    TestHost host(&root);
    auto surface = TestSurface::create(400, 400);
    PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};

    host.show_toast("Copied to clipboard");
    REQUIRE_NOTHROW(host.paint_toast_overlay(pc, {0, 0, 400, 400}));

    host.fire_all_delays();
    REQUIRE_NOTHROW(host.paint_toast_overlay(pc, {0, 0, 400, 400}));
}
