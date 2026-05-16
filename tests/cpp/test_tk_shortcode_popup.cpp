#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "views/ShortcodePopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"

#include <optional>
#include <string>

using namespace tk;
using Catch::Approx;
using tesseract::views::ShortcodePopup;
using tesseract::views::ShortcodeSuggestion;

namespace {

struct Stage {
    std::unique_ptr<TestSurface> surface = TestSurface::create(300, 400);
    LayoutCtx lc() { return LayoutCtx{ surface->factory(), Theme::light() }; }
    PaintCtx  pc() { return PaintCtx{ surface->canvas(), surface->factory(), Theme::light() }; }
    void run(Widget& w, Rect bounds) {
        auto l = lc(); w.measure(l, {bounds.w, bounds.h}); w.arrange(l, bounds);
        auto p = pc(); w.paint(p);
    }
};

ShortcodeSuggestion make_unicode(const char* sc, const char* glyph) {
    ShortcodeSuggestion s;
    s.shortcode = sc;
    s.glyph     = glyph;
    return s;
}

} // namespace

TEST_CASE("ShortcodePopup — empty suggestions measures zero height") {
    Stage st;
    ShortcodePopup popup;
    auto lc = st.lc();
    auto sz = popup.measure(lc, {280.0f, 400.0f});
    CHECK(sz.h == 0.0f);
}

TEST_CASE("ShortcodePopup — 3 suggestions measures 3 row heights") {
    Stage st;
    ShortcodePopup popup;
    popup.set_suggestions({
        make_unicode("smile",    "😄"),
        make_unicode("grinning", "😀"),
        make_unicode("joy",      "😂"),
    });
    auto lc = st.lc();
    auto sz = popup.measure(lc, {280.0f, 400.0f});
    CHECK(sz.h > 0.0f);
    CHECK(sz.h == Approx(3.0f * ShortcodePopup::kRowHeight));
}

TEST_CASE("ShortcodePopup — capped at 8 rows even with 10 suggestions") {
    Stage st;
    ShortcodePopup popup;
    std::vector<ShortcodeSuggestion> many;
    for (int i = 0; i < 10; ++i) {
        ShortcodeSuggestion s;
        s.shortcode = "sc" + std::to_string(i);
        s.glyph = "X";
        many.push_back(s);
    }
    popup.set_suggestions(std::move(many));
    auto lc = st.lc();
    auto sz = popup.measure(lc, {280.0f, 400.0f});
    CHECK(sz.h == Approx(8.0f * ShortcodePopup::kRowHeight));
}

TEST_CASE("ShortcodePopup — on_accepted fires with correct suggestion") {
    Stage st;
    ShortcodePopup popup;
    popup.set_suggestions({
        make_unicode("smile",    "😄"),
        make_unicode("grinning", "😀"),
    });
    std::optional<ShortcodeSuggestion> accepted;
    popup.on_accepted = [&](ShortcodeSuggestion s) { accepted = std::move(s); };

    auto lc = st.lc();
    Rect bounds{0, 0, 280, 80};
    popup.measure(lc, {bounds.w, bounds.h});
    popup.arrange(lc, bounds);

    float row_h = ShortcodePopup::kRowHeight;
    popup.on_pointer_down({ 100.0f, row_h * 0.5f });
    popup.on_pointer_up  ({ 100.0f, row_h * 0.5f }, true);

    REQUIRE(accepted.has_value());
    CHECK(accepted->shortcode == "smile");
}

TEST_CASE("ShortcodePopup — set_selected_index highlights selected row") {
    Stage st;
    ShortcodePopup popup;
    popup.set_suggestions({
        make_unicode("smile",    "😄"),
        make_unicode("grinning", "😀"),
    });
    popup.set_selected_index(1);
    CHECK(popup.selected_index() == 1);
    st.run(popup, {0, 0, 280, 80});  // must paint without crash
}
