#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/LoginView.h"
#include "tk_test_surface.h"

#include <memory>

using namespace tk;
using tesseract::views::LoginView;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
    }
};

} // namespace

TEST_CASE("LoginView Mode::Initial hides Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    Stage st;
    LoginView lv; // default: Mode::Initial, State::Form
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.mode() == LoginView::Mode::Initial);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::Initial keeps Cancel hidden in Waiting state",
          "[tk][view][login][multi_account]")
{
    Stage st;
    LoginView lv;
    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});

    CHECK_FALSE(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::AddAccount shows Cancel in Form state",
          "[tk][view][login][multi_account]")
{
    Stage st;
    LoginView lv;
    lv.set_mode(LoginView::Mode::AddAccount);
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.mode() == LoginView::Mode::AddAccount);
    CHECK(lv.state() == LoginView::State::Form);
    CHECK(lv.cancel_visible());
}

TEST_CASE("LoginView Mode::AddAccount keeps Cancel visible in Waiting state",
          "[tk][view][login][multi_account]")
{
    Stage st;
    LoginView lv;
    lv.set_mode(LoginView::Mode::AddAccount);
    lv.set_state(LoginView::State::Waiting);
    st.run(lv, {0, 0, 640, 480});

    CHECK(lv.cancel_visible());
}

TEST_CASE("LoginView toggling Mode flips Cancel visibility on its own",
          "[tk][view][login][multi_account]")
{
    Stage st;
    LoginView lv;
    st.run(lv, {0, 0, 640, 480});
    REQUIRE_FALSE(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::AddAccount);
    CHECK(lv.cancel_visible());

    lv.set_mode(LoginView::Mode::Initial);
    CHECK_FALSE(lv.cancel_visible());
}
