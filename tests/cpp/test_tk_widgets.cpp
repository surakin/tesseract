#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/image_view.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/LoginView.h"
#include "tk_test_surface.h"
#include <tesseract/settings.h>

#include <memory>

using namespace tk;
using tesseract::views::LoginView;

namespace
{

// One-shot helper to run measure → arrange → paint against a TestSurface.
struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 600);

    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }

    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

bool nearly(Color a, Color b, int tol = 12)
{
    auto delta = [](int x, int y)
    {
        return x > y ? x - y : y - x;
    };
    return delta(a.r, b.r) <= tol && delta(a.g, b.g) <= tol &&
           delta(a.b, b.b) <= tol;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Layout: VBox
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("VBox stacks children with spacing", "[tk][widget][layout]")
{
    Stage st;
    VBox box;
    box.set_spacing(8).set_padding(Edges::all(10)).set_cross(Cross::Stretch);

    auto* a = box.add_child(
        std::make_unique<Separator>(Separator::Orientation::Horizontal));
    a->set_thickness(20);
    auto* b = box.add_child(
        std::make_unique<Separator>(Separator::Orientation::Horizontal));
    b->set_thickness(30);

    auto lc = st.layout_ctx();
    box.arrange(lc, {0, 0, 200, 100});

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
          "[tk][widget][layout]")
{
    Stage st;
    VBox box;
    box.set_main(Main::Center).set_cross(Cross::Stretch);

    auto* a = box.add_child(std::make_unique<Separator>());
    a->set_thickness(40);

    auto lc = st.layout_ctx();
    box.arrange(lc, {0, 0, 100, 100});
    Rect ra = a->bounds();
    // 100 - 40 = 60 leftover, halved = 30 above.
    CHECK(ra.y == 30.0f);
    CHECK(ra.h == 40.0f);
}

TEST_CASE("HBox lays children horizontally", "[tk][widget][layout]")
{
    Stage st;
    HBox row;
    row.set_spacing(4).set_cross(Cross::Stretch);
    auto* a = row.add_child(
        std::make_unique<Separator>(Separator::Orientation::Vertical));
    a->set_thickness(10);
    auto* b = row.add_child(
        std::make_unique<Separator>(Separator::Orientation::Vertical));
    b->set_thickness(20);

    auto lc = st.layout_ctx();
    row.arrange(lc, {0, 0, 100, 50});

    CHECK(a->bounds().x == 0.0f);
    CHECK(a->bounds().w == 10.0f);
    CHECK(b->bounds().x == 14.0f); // 0 + 10 + 4 spacing
    CHECK(b->bounds().w == 20.0f);
    // Cross stretch on row's cross axis (height) → both fill 50.
    CHECK(a->bounds().h == 50.0f);
    CHECK(b->bounds().h == 50.0f);
}

// ─────────────────────────────────────────────────────────────────────────
//  Atomic widgets
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Label::measure returns a non-zero size for non-empty text",
          "[tk][widget][label]")
{
    Stage st;
    Label lbl("Hello, world", FontRole::Body);
    auto lc = st.layout_ctx();
    Size s = lbl.measure(lc, {-1, -1});
    CHECK(s.w > 0.0f);
    CHECK(s.h > 0.0f);
}

TEST_CASE("Button::click invokes the on-click callback", "[tk][widget][button]")
{
    int n_calls = 0;
    Button btn("OK",
               [&]
               {
                   ++n_calls;
               });
    btn.click();
    CHECK(n_calls == 1);

    btn.set_enabled(false);
    btn.click();
    CHECK(n_calls == 1); // disabled buttons swallow clicks
}

TEST_CASE("Button paints a coloured rect at its bounds", "[tk][widget][button]")
{
    Stage st;
    auto btn = std::make_unique<Button>("Sign in");

    st.run(*btn, {10, 10, 120, 36});

    // A pixel inside the rounded rect but clear of the centred glyph
    // should be the accent fill (default Primary). Sample near the
    // right edge, vertically centred — well past the end of "Sign in".
    auto accent = Theme::light().palette.accent;
    auto px = st.surface->read_pixel(120, 28);
    CHECK(nearly(px, accent, /*tol=*/20));
}

TEST_CASE("ToggleButton toggles checked state and fires on_change",
          "[tk][widget][toggle]")
{
    int  calls = 0;
    bool last  = false;
    ToggleButton tb("\xE2\x98\x86 Favourite"); // ☆ Favourite
    tb.on_change = [&](bool c) { ++calls; last = c; };

    CHECK(tb.checked() == false);

    // Press + release inside toggles on and fires the callback.
    tb.on_pointer_down({1, 1});
    tb.on_pointer_up({1, 1}, /*inside_self=*/true);
    CHECK(tb.checked() == true);
    CHECK(calls == 1);
    CHECK(last == true);

    // Release outside does not toggle and does not fire.
    tb.on_pointer_down({1, 1});
    tb.on_pointer_up({999, 999}, /*inside_self=*/false);
    CHECK(tb.checked() == true);
    CHECK(calls == 1);

    // Disabled swallows the interaction.
    tb.set_enabled(false);
    tb.on_pointer_down({1, 1});
    tb.on_pointer_up({1, 1}, /*inside_self=*/true);
    CHECK(tb.checked() == true);
    CHECK(calls == 1);

    // set_checked() is silent (programmatic, no callback).
    tb.set_enabled(true);
    tb.set_checked(false);
    CHECK(tb.checked() == false);
    CHECK(calls == 1);
}

TEST_CASE("ToggleButton paints accent fill when checked",
          "[tk][widget][toggle]")
{
    Stage st;
    auto tb = std::make_unique<ToggleButton>("Favourite", /*checked=*/true);
    st.run(*tb, {10, 10, 140, 36});
    auto accent = Theme::light().palette.accent;
    auto px = st.surface->read_pixel(135, 28); // right edge, clear of glyph
    CHECK(nearly(px, accent, /*tol=*/20));
}

TEST_CASE("SwitchButton toggles checked state and fires on_change",
          "[tk][widget][switch]")
{
    int  calls = 0;
    bool last  = false;
    SwitchButton sw("Favourite");
    sw.on_change = [&](bool c) { ++calls; last = c; };

    CHECK(sw.checked() == false);

    sw.on_pointer_down({1, 1});
    sw.on_pointer_up({1, 1}, /*inside_self=*/true);
    CHECK(sw.checked() == true);
    CHECK(calls == 1);
    CHECK(last == true);

    // Release outside does not toggle.
    sw.on_pointer_down({1, 1});
    sw.on_pointer_up({999, 999}, /*inside_self=*/false);
    CHECK(sw.checked() == true);
    CHECK(calls == 1);

    // Disabled swallows the interaction.
    sw.set_enabled(false);
    sw.on_pointer_down({1, 1});
    sw.on_pointer_up({1, 1}, /*inside_self=*/true);
    CHECK(sw.checked() == true);
    CHECK(calls == 1);

    // set_checked() is silent.
    sw.set_enabled(true);
    sw.set_checked(false);
    CHECK(sw.checked() == false);
    CHECK(calls == 1);
}

TEST_CASE("SwitchButton paints an accent track when on", "[tk][widget][switch]")
{
    Stage st;
    auto sw = std::make_unique<SwitchButton>("Favourite", /*checked=*/true);
    st.run(*sw, {10, 10, 200, 32});
    auto accent = Theme::light().palette.accent;
    // Track sits at the right edge (width 36 → x 174..210); sample its left
    // half, where the knob (parked right when on) does not cover it.
    auto px = st.surface->read_pixel(180, 26);
    CHECK(nearly(px, accent, /*tol=*/20));
}

TEST_CASE("Separator paints a 1 px line by default", "[tk][widget][separator]")
{
    Stage st;
    Separator sep;
    sep.set_thickness(1);
    st.surface->canvas().clear(Color::rgb(0xffffff));
    st.run(sep, {0, 50, 200, 1});
    // Sample inside the line — should be the theme separator colour.
    auto px = st.surface->read_pixel(50, 50);
    CHECK(nearly(px, Theme::light().palette.separator, /*tol=*/30));
}

TEST_CASE("Avatar paints initials disc when no image is set",
          "[tk][widget][avatar]")
{
    Stage st;
    Avatar a("Alice Anders");
    a.set_diameter(48);
    st.surface->canvas().clear(Color::rgb(0xffffff));
    st.run(a, {100, 100, 48, 48});
    // Sample to the right of the centred glyph — still well inside the
    // 24 px-radius disc but far enough out to avoid letter strokes.
    auto bg = Theme::light().palette.avatar_initials_bg;
    auto px = st.surface->read_pixel(142, 124);
    CHECK(nearly(px, bg, /*tol=*/30));
}

TEST_CASE("Widget::hit_test descends into children", "[tk][widget][hittest]")
{
    Stage st;
    VBox box;
    box.set_cross(Cross::Stretch);
    auto* a = box.add_child(std::make_unique<Separator>());
    a->set_thickness(20);
    auto* b = box.add_child(std::make_unique<Separator>());
    b->set_thickness(20);

    auto lc = st.layout_ctx();
    box.arrange(lc, {0, 0, 100, 100});

    // hit_test takes world coords. (5, 5) is inside child a.
    Widget* hit = box.hit_test({5, 5});
    CHECK(hit == a);
    // (5, 25) is inside child b (b->y == 20).
    hit = box.hit_test({5, 25});
    CHECK(hit == b);
    // Below all children → no child hit, falls back to the box itself.
    hit = box.hit_test({5, 60});
    CHECK(hit == &box);
    // Outside box entirely.
    hit = box.hit_test({5, -1});
    CHECK(hit == nullptr);
}

TEST_CASE("Widget pointer dispatch routes clicks to a button in an offset "
          "container",
          "[tk][widget][dispatch]")
{
    // Regression: previously the recursive subtraction of bounds_.x in
    // dispatch_pointer_down over-corrected once it descended past a
    // parent that was itself laid out at a non-zero world origin — which
    // is what LoginView does when it centres its card. Build that exact
    // shape (VBox padded inside a non-zero-origin frame, button inside)
    // and assert the dispatch reaches the button and fires on_click.
    Stage st;

    VBox card;
    card.set_padding(Edges::all(24)).set_cross(Cross::Stretch);

    bool clicked = false;
    auto* btn = card.add_child(std::make_unique<Button>(
        "Sign in",
        [&]
        {
            clicked = true;
        },
        Button::Variant::Primary));
    btn->set_min_size({0, 36});

    auto lc = st.layout_ctx();
    // Place the card well away from (0, 0) so the bug would manifest.
    Rect card_bounds{20, 250, 360, 100};
    card.measure(lc, {card_bounds.w, card_bounds.h});
    card.arrange(lc, card_bounds);

    Rect bb = btn->bounds();
    REQUIRE(bb.w > 0);
    REQUIRE(bb.h > 0);

    // Click in the geometric centre of the button (world coords).
    Point centre{bb.x + bb.w * 0.5f, bb.y + bb.h * 0.5f};

    Widget* hit = card.hit_test(centre);
    CHECK(hit == btn);

    Widget* claimer = card.dispatch_pointer_down(centre);
    REQUIRE(claimer == btn);

    // Release on the same spot — the Button should treat it as inside
    // and fire its on_click.
    Point local = btn->world_to_local(centre);
    CHECK(local.x >= 0);
    CHECK(local.y >= 0);
    CHECK(local.x < bb.w);
    CHECK(local.y < bb.h);
    btn->on_pointer_up(local,
                       /*inside_self=*/local.x >= 0 && local.y >= 0 &&
                           local.x < bb.w && local.y < bb.h);
    CHECK(clicked);

    // And a click clearly outside the button should miss it.
    clicked = false;
    Widget* miss =
        card.dispatch_pointer_down({card_bounds.x + 2, card_bounds.y + 2});
    CHECK(miss != btn);
}

// ─────────────────────────────────────────────────────────────────────────
//  Shared LoginView smoke test
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("LoginView lays out + paints onto the offscreen surface",
          "[tk][view][login]")
{
    Stage st;
    LoginView view;
    view.set_homeserver_label("matrix.org");

    st.run(view, {0, 0, 400, 600});

    // The card should have positioned the homeserver field somewhere
    // inside the view bounds.
    Rect hr = view.homeserver_field_rect();
    CHECK(hr.w > 0.0f);
    CHECK(hr.h >= 30.0f);

    // Verify the Sign In button exists and is clickable (no-ops without a
    // client, but must not crash). Controller logic lives in LoginView itself
    // now so there is no external callback to hook at this layer.
    auto& children = view.children();
    REQUIRE_FALSE(children.empty());
    // The card is the first (and only) direct child of LoginView.
    Widget* card = children[0].get();
    REQUIRE(card);
    Button* signin = nullptr;
    for (auto& ch : card->children())
    {
        if (auto* b = dynamic_cast<Button*>(ch.get()))
        {
            if (b->visible())
            {
                signin = b;
                break;
            }
        }
    }
    REQUIRE(signin);
    signin->click(); // no-op: client_ is null, sign_in_() returns immediately

    // Switching to Waiting hides Sign in. Cancel visibility is now gated
    // on Mode (added for multi-account): Initial keeps Cancel hidden,
    // AddAccount surfaces it.
    view.set_state(LoginView::State::Waiting);
    auto count_visible_buttons = [&]()
    {
        int n = 0;
        for (auto& ch : card->children())
        {
            if (auto* b = dynamic_cast<Button*>(ch.get()))
            {
                if (b->visible())
                {
                    ++n;
                }
            }
        }
        return n;
    };
    CHECK(count_visible_buttons() == 0); // Initial + Waiting → both hidden

    view.set_mode(LoginView::Mode::AddAccount);
    CHECK(count_visible_buttons() == 1); // AddAccount + Waiting → Cancel only
    CHECK(view.cancel_visible());
}

TEST_CASE("Settings has expected defaults", "[settings]")
{
    const auto& s = tesseract::Settings::instance();

    // Font sizes — one per tk::FontRole.
    CHECK(s.font_small == 8);
    CHECK(s.font_body == 12);
    CHECK(s.font_sender_name == 11);
    CHECK(s.font_timestamp == 9);
    CHECK(s.font_sidebar_name == 12);
    CHECK(s.font_sidebar_preview == 10);
    CHECK(s.font_unread_badge == 10);
    CHECK(s.font_title == 14);
    CHECK(s.font_ui_semibold == 10);

    // Reaction chip.
    CHECK(s.reaction_chip_height == 28);
    CHECK(s.reaction_chip_gap == 6);
}
