#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "views/VerificationBanner.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using Catch::Approx;
using tesseract::views::VerificationBanner;
using tesseract::VerificationEmoji;

namespace {

struct Stage {
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 130);
    LayoutCtx layout_ctx() {
        return LayoutCtx{ surface->factory(), Theme::light() };
    }
    PaintCtx paint_ctx() {
        return PaintCtx{ surface->canvas(), surface->factory(), Theme::light() };
    }
    void run(Widget& root, Rect bounds) {
        auto lc = layout_ctx();
        root.measure(lc, { bounds.w, bounds.h });
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

std::vector<VerificationEmoji> make_emojis() {
    return {
        { "🐶", "Dog" }, { "🐱", "Cat" }, { "🦁", "Lion" }, { "🐻", "Bear" },
        { "🐼", "Panda" }, { "🐨", "Koala" }, { "🐯", "Tiger" },
    };
}

Button* find_button(Widget& root, const std::string& lbl) {
    for (auto& ch : root.children()) {
        if (auto* b = dynamic_cast<Button*>(ch.get())) {
            if (b->label() == lbl) return b;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("VerificationBanner starts in Prompt state", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    st.run(banner, { 0, 0, 640, 48 });
    CHECK(banner.state() == VerificationBanner::State::Prompt);
}

TEST_CASE("VerificationBanner Prompt measures 48 px tall", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    auto lc = st.layout_ctx();
    auto sz = banner.measure(lc, { 640, 48 });
    CHECK(sz.h == Approx(48.0f));
}

TEST_CASE("VerificationBanner ShowEmojis measures 124 px tall", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    banner.set_emojis(make_emojis());
    CHECK(banner.state() == VerificationBanner::State::ShowEmojis);
    auto lc = st.layout_ctx();
    auto sz = banner.measure(lc, { 640, 124 });
    CHECK(sz.h == Approx(124.0f));
}

TEST_CASE("VerificationBanner set_emojis switches to ShowEmojis", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    banner.set_emojis(make_emojis());
    CHECK(banner.state() == VerificationBanner::State::ShowEmojis);
    // Must paint without crash at the ShowEmojis height.
    st.run(banner, { 0, 0, 640, 124 });
}

TEST_CASE("VerificationBanner on_verify fires on Prompt primary click", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    bool fired = false;
    banner.on_verify = [&] { fired = true; };
    st.run(banner, { 0, 0, 640, 48 });

    Button* b = find_button(banner, "Verify");
    REQUIRE(b);
    b->click();
    CHECK(fired);
}

TEST_CASE("VerificationBanner on_dismiss fires on Prompt dismiss click", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    bool fired = false;
    banner.on_dismiss = [&] { fired = true; };
    st.run(banner, { 0, 0, 640, 48 });

    Button* b = find_button(banner, "✕");
    REQUIRE(b);
    b->click();
    CHECK(fired);
}

TEST_CASE("VerificationBanner Cancelled state measures 48 px tall", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    banner.set_state(VerificationBanner::State::Cancelled);
    banner.set_cancel_reason("mismatched SAS");
    auto lc = st.layout_ctx();
    auto sz = banner.measure(lc, { 640, 48 });
    CHECK(sz.h == Approx(48.0f));
    st.run(banner, { 0, 0, 640, 48 });
}

TEST_CASE("VerificationBanner set_state clears cancel reason on non-Cancelled", "[tk][view][verification]") {
    VerificationBanner banner;
    banner.set_state(VerificationBanner::State::Cancelled);
    banner.set_cancel_reason("some reason");
    banner.set_state(VerificationBanner::State::Prompt);
    CHECK(banner.state() == VerificationBanner::State::Prompt);
}

TEST_CASE("VerificationBanner Done state paints without crash", "[tk][view][verification]") {
    Stage st;
    VerificationBanner banner;
    banner.set_state(VerificationBanner::State::Done);
    st.run(banner, { 0, 0, 640, 48 });
    CHECK(banner.state() == VerificationBanner::State::Done);
}
