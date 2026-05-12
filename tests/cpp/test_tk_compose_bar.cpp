#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "views/ComposeBar.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::ComposeBar;

namespace {

struct Stage {
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 200);
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

Button* find_button(Widget& w, const std::string& label) {
    for (auto& ch : w.children()) {
        if (auto* b = dynamic_cast<Button*>(ch.get())) {
            if (b->label() == label) return b;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("ComposeBar reserves a text-area rect between the emoji and send buttons",
          "[tk][view][compose]") {
    Stage st;
    ComposeBar bar;
    st.run(bar, { 0, 0, 640, ComposeBar::kMinHeight });
    Rect r = bar.text_area_rect();
    CHECK(r.w > 100.0f);
    CHECK(r.h > 0.0f);
    // Sits well inside the bar (not flush with the left/right edges).
    CHECK(r.x > 30.0f);
    CHECK(r.x + r.w < 640.0f - 30.0f);
}

TEST_CASE("ComposeBar starts with the send button disabled until text appears",
          "[tk][view][compose]") {
    Stage st;
    ComposeBar bar;
    st.run(bar, { 0, 0, 640, ComposeBar::kMinHeight });

    Button* send = find_button(bar, "Send");
    REQUIRE(send);
    CHECK_FALSE(send->enabled());

    bar.set_current_text("hello");
    CHECK(send->enabled());

    // Whitespace-only counts as empty.
    bar.set_current_text("   \n\t");
    CHECK_FALSE(send->enabled());
}

TEST_CASE("ComposeBar send-button click fires on_send with the current text",
          "[tk][view][compose]") {
    Stage st;
    ComposeBar bar;
    std::string got;
    bar.on_send = [&](const std::string& t) { got = t; };
    bar.set_current_text("hi there");
    st.run(bar, { 0, 0, 640, ComposeBar::kMinHeight });

    Button* send = find_button(bar, "Send");
    REQUIRE(send);
    REQUIRE(send->enabled());
    send->click();
    CHECK(got == "hi there");
}

TEST_CASE("ComposeBar emoji-button click fires on_emoji",
          "[tk][view][compose]") {
    Stage st;
    ComposeBar bar;
    int hits = 0;
    bar.on_emoji = [&] { ++hits; };
    st.run(bar, { 0, 0, 640, ComposeBar::kMinHeight });

    Button* emoji = nullptr;
    for (auto& ch : bar.children()) {
        if (auto* b = dynamic_cast<Button*>(ch.get())) {
            if (b != find_button(bar, "Send")) { emoji = b; break; }
        }
    }
    REQUIRE(emoji);
    emoji->click();
    CHECK(hits == 1);
}

TEST_CASE("ComposeBar natural_height grows with the text-area content height",
          "[tk][view][compose]") {
    ComposeBar bar;
    CHECK(bar.natural_height() == ComposeBar::kMinHeight);

    bar.set_text_area_natural_height(200.0f);
    // Clamps to kMaxHeight; padding pushes it to the ceiling.
    CHECK(bar.natural_height() == ComposeBar::kMaxHeight);

    bar.set_text_area_natural_height(0.0f);
    CHECK(bar.natural_height() == ComposeBar::kMinHeight);
}

TEST_CASE("ComposeBar set_enabled(false) gates the send button regardless of text",
          "[tk][view][compose]") {
    Stage st;
    ComposeBar bar;
    bar.set_current_text("ready");
    bar.set_enabled(false);
    st.run(bar, { 0, 0, 640, ComposeBar::kMinHeight });

    Button* send = find_button(bar, "Send");
    REQUIRE(send);
    CHECK_FALSE(send->enabled());

    bar.set_enabled(true);
    CHECK(send->enabled());
}
