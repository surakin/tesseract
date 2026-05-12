#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/image_view.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/LoginView.h"
#include "tk_test_surface.h"

#include <memory>

using namespace tk;
using tesseract::views::LoginView;

namespace {

// One-shot helper to run measure → arrange → paint against a TestSurface.
struct Stage {
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);

    LayoutCtx layout_ctx() {
        return LayoutCtx{ surface->factory(), Theme::light() };
    }
    PaintCtx paint_ctx() {
        return PaintCtx{ surface->canvas(), surface->factory(),
                          Theme::light() };
    }

    void run(Widget& root, Rect bounds) {
        auto lc = layout_ctx();
        root.measure(lc, { bounds.w, bounds.h });
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

bool nearly(Color a, Color b, int tol = 12) {
    auto delta = [](int x, int y) {
        return x > y ? x - y : y - x;
    };
    return delta(a.r, b.r) <= tol
        && delta(a.g, b.g) <= tol
        && delta(a.b, b.b) <= tol;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Layout: VBox
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("VBox stacks children with spacing", "[tk][widget][layout]") {
    Stage st;
    VBox box;
    box.set_spacing(8).set_padding(Edges::all(10)).set_cross(Cross::Stretch);

    auto* a = box.add_child(std::make_unique<Separator>(Separator::Orientation::Horizontal));
    a->set_thickness(20);
    auto* b = box.add_child(std::make_unique<Separator>(Separator::Orientation::Horizontal));
    b->set_thickness(30);

    auto lc = st.layout_ctx();
    box.arrange(lc, { 0, 0, 200, 100 });

    Rect ra = a->bounds();
    Rect rb = b->bounds();

    // padding.left=10 → cross-axis origin
    CHECK(ra.x == 10.0f);
    CHECK(rb.x == 10.0f);
    // padding.top=10 → main-axis origin for first child
    CHECK(ra.y == 10.0f);
    CHECK(ra.h == 20.0f);
    // spacing=8 + previous height=20 → second child y = 10 + 20 + 8
    CHECK(rb.y == 38.0f);
    CHECK(rb.h == 30.0f);
    // cross stretch means children fill width - padding (10 each side).
    CHECK(ra.w == 180.0f);
    CHECK(rb.w == 180.0f);
}

TEST_CASE("VBox Main::Center centres children with leftover space",
          "[tk][widget][layout]") {
    Stage st;
    VBox box;
    box.set_main(Main::Center).set_cross(Cross::Stretch);

    auto* a = box.add_child(std::make_unique<Separator>());
    a->set_thickness(40);

    auto lc = st.layout_ctx();
    box.arrange(lc, { 0, 0, 100, 100 });
    Rect ra = a->bounds();
    // 100 - 40 = 60 leftover, halved = 30 above.
    CHECK(ra.y == 30.0f);
    CHECK(ra.h == 40.0f);
}

TEST_CASE("HBox lays children horizontally", "[tk][widget][layout]") {
    Stage st;
    HBox row;
    row.set_spacing(4).set_cross(Cross::Stretch);
    auto* a = row.add_child(std::make_unique<Separator>(Separator::Orientation::Vertical));
    a->set_thickness(10);
    auto* b = row.add_child(std::make_unique<Separator>(Separator::Orientation::Vertical));
    b->set_thickness(20);

    auto lc = st.layout_ctx();
    row.arrange(lc, { 0, 0, 100, 50 });

    CHECK(a->bounds().x == 0.0f);
    CHECK(a->bounds().w == 10.0f);
    CHECK(b->bounds().x == 14.0f);    // 0 + 10 + 4 spacing
    CHECK(b->bounds().w == 20.0f);
    // Cross stretch on row's cross axis (height) → both fill 50.
    CHECK(a->bounds().h == 50.0f);
    CHECK(b->bounds().h == 50.0f);
}

// ─────────────────────────────────────────────────────────────────────────
//  Atomic widgets
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Label::measure returns a non-zero size for non-empty text",
          "[tk][widget][label]") {
    Stage st;
    Label lbl("Hello, world", FontRole::Body);
    auto lc = st.layout_ctx();
    Size s = lbl.measure(lc, { -1, -1 });
    CHECK(s.w > 0.0f);
    CHECK(s.h > 0.0f);
}

TEST_CASE("Button::click invokes the on-click callback",
          "[tk][widget][button]") {
    int n_calls = 0;
    Button btn("OK", [&] { ++n_calls; });
    btn.click();
    CHECK(n_calls == 1);

    btn.set_enabled(false);
    btn.click();
    CHECK(n_calls == 1);   // disabled buttons swallow clicks
}

TEST_CASE("Button paints a coloured rect at its bounds",
          "[tk][widget][button]") {
    Stage st;
    auto btn = std::make_unique<Button>("Sign in");

    st.run(*btn, { 10, 10, 120, 36 });

    // Centre of the button should be in the accent colour (default Primary).
    auto accent = Theme::light().palette.accent;
    auto px     = st.surface->read_pixel(70, 28);
    CHECK(nearly(px, accent, /*tol=*/20));
}

TEST_CASE("Separator paints a 1 px line by default",
          "[tk][widget][separator]") {
    Stage st;
    Separator sep;
    sep.set_thickness(1);
    st.surface->canvas().clear(Color::rgb(0xffffff));
    st.run(sep, { 0, 50, 200, 1 });
    // Sample inside the line — should be the theme separator colour.
    auto px = st.surface->read_pixel(50, 50);
    CHECK(nearly(px, Theme::light().palette.separator, /*tol=*/30));
}

TEST_CASE("Avatar paints initials disc when no image is set",
          "[tk][widget][avatar]") {
    Stage st;
    Avatar a("Alice Anders");
    a.set_diameter(48);
    st.surface->canvas().clear(Color::rgb(0xffffff));
    st.run(a, { 100, 100, 48, 48 });
    // Sample to the right of the centred glyph — still well inside the
    // 24 px-radius disc but far enough out to avoid letter strokes.
    auto bg = Theme::light().palette.avatar_initials_bg;
    auto px = st.surface->read_pixel(142, 124);
    CHECK(nearly(px, bg, /*tol=*/30));
}

TEST_CASE("Widget::hit_test descends into children",
          "[tk][widget][hittest]") {
    Stage st;
    VBox box;
    box.set_cross(Cross::Stretch);
    auto* a = box.add_child(std::make_unique<Separator>());
    a->set_thickness(20);
    auto* b = box.add_child(std::make_unique<Separator>());
    b->set_thickness(20);

    auto lc = st.layout_ctx();
    box.arrange(lc, { 0, 0, 100, 100 });

    // hit_test takes local coords. (5, 5) is inside child a.
    Widget* hit = box.hit_test({ 5, 5 });
    CHECK(hit == a);
    // (5, 25) is inside child b (b->y == 20).
    hit = box.hit_test({ 5, 25 });
    CHECK(hit == b);
    // Below all children → no child hit, falls back to the box itself.
    hit = box.hit_test({ 5, 60 });
    CHECK(hit == &box);
    // Outside box entirely.
    hit = box.hit_test({ 5, -1 });
    CHECK(hit == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  Shared LoginView smoke test
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("LoginView lays out + paints onto the offscreen surface",
          "[tk][view][login]") {
    Stage st;
    LoginView view;
    view.set_homeserver_label("matrix.org");

    bool clicked = false;
    view.on_sign_in = [&] { clicked = true; };

    st.run(view, { 0, 0, 400, 600 });

    // The card should have positioned the homeserver field somewhere
    // inside the view bounds.
    Rect hr = view.homeserver_field_rect();
    CHECK(hr.w  >  0.0f);
    CHECK(hr.h >= 30.0f);

    // Pretend a button click. Even without real pointer wiring the
    // callback is reachable via Button::click() through the widget tree.
    auto& children = view.children();
    REQUIRE_FALSE(children.empty());
    // The card is the first (and only) direct child of LoginView.
    Widget* card = children[0].get();
    REQUIRE(card);
    Button* signin = nullptr;
    for (auto& ch : card->children()) {
        if (auto* b = dynamic_cast<Button*>(ch.get())) {
            if (b->visible()) {
                signin = b;
                break;
            }
        }
    }
    REQUIRE(signin);
    signin->click();
    CHECK(clicked);

    // Switching to Waiting hides Sign in, shows Cancel.
    view.set_state(LoginView::State::Waiting);
    int visible_buttons = 0;
    for (auto& ch : card->children()) {
        if (auto* b = dynamic_cast<Button*>(ch.get())) {
            if (b->visible()) ++visible_buttons;
        }
    }
    CHECK(visible_buttons == 1);
}
