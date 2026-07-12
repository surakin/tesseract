#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/image_view.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/LoginView.h"
#include "views/MainAppWidget.h"
#include "tk_test_surface.h"
#include <tesseract/settings.h>

#include <memory>

using namespace tk;
using tesseract::views::LoginView;
using tesseract::views::MainAppWidget;

namespace
{

// One-shot helper to run measure → arrange → paint against a TestSurface.
struct TkWidgetsStage
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

class PaintProbe : public Widget
{
public:
    explicit PaintProbe(int& count) : count_(count)
    {
    }

    Size measure(LayoutCtx&, Size constraints) override
    {
        return constraints;
    }

    void paint(PaintCtx&) override
    {
        ++count_;
    }

private:
    int& count_;
};

class KeyProbe : public FixedBox
{
public:
    KeyProbe(int& count, bool consume)
        : FixedBox({10, 10}), count_(count), consume_(consume)
    {
    }

    bool on_key_down(const KeyEvent& event) override
    {
        if (event.key == Key::Escape)
        {
            ++count_;
        }
        return consume_;
    }

private:
    int& count_;
    bool consume_ = false;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Layout: VBox
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("VBox stacks children with spacing", "[tk][widget][layout]")
{
    TkWidgetsStage st;
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
    TkWidgetsStage st;
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
    TkWidgetsStage st;
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

TEST_CASE("Widget default paint traverses visible children",
          "[tk][widget][layout]")
{
    TkWidgetsStage st;
    FixedBox root({100, 100});
    int painted_a = 0;
    int painted_b = 0;
    root.add_child(std::make_unique<PaintProbe>(painted_a));
    auto* hidden = root.add_child(std::make_unique<PaintProbe>(painted_b));
    hidden->set_visible(false);

    auto pc = st.paint_ctx();
    root.paint(pc);

    CHECK(painted_a == 1);
    CHECK(painted_b == 0);
}

TEST_CASE("Stack arranges visible children to the same bounds",
          "[tk][widget][layout]")
{
    TkWidgetsStage st;
    Stack stack;
    auto* a = stack.add_child(std::make_unique<FixedBox>(Size{20, 30}));
    auto* b = stack.add_child(std::make_unique<FixedBox>(Size{40, 10}));

    auto lc = st.layout_ctx();
    Size measured = stack.measure(lc, {100, 80});
    stack.arrange(lc, {5, 6, 100, 80});

    CHECK(measured.w == 40.0f);
    CHECK(measured.h == 30.0f);
    CHECK(a->bounds().x == 5.0f);
    CHECK(a->bounds().y == 6.0f);
    CHECK(a->bounds().w == 100.0f);
    CHECK(a->bounds().h == 80.0f);
    CHECK(b->bounds().x == 5.0f);
    CHECK(b->bounds().y == 6.0f);
    CHECK(b->bounds().w == 100.0f);
    CHECK(b->bounds().h == 80.0f);
}

TEST_CASE("Keyboard dispatch routes to topmost visible child first",
          "[tk][widget][key]")
{
    FixedBox root({100, 100});
    int lower = 0;
    int upper = 0;
    int hidden = 0;
    root.add_child(std::make_unique<KeyProbe>(lower, true));
    auto* h = root.add_child(std::make_unique<KeyProbe>(hidden, true));
    h->set_visible(false);
    root.add_child(std::make_unique<KeyProbe>(upper, true));

    CHECK(root.dispatch_key_down({Key::Escape}) == true);
    CHECK(lower == 0);
    CHECK(hidden == 0);
    CHECK(upper == 1);
}

TEST_CASE("Keyboard dispatch bubbles to parent when children ignore it",
          "[tk][widget][key]")
{
    class ParentProbe : public FixedBox
    {
    public:
        explicit ParentProbe(int& count) : FixedBox({100, 100}), count_(count)
        {
        }
        bool on_key_down(const KeyEvent& event) override
        {
            if (event.key == Key::Escape)
            {
                ++count_;
                return true;
            }
            return false;
        }

    private:
        int& count_;
    };

    int child = 0;
    int parent = 0;
    ParentProbe root(parent);
    root.add_child(std::make_unique<KeyProbe>(child, false));

    CHECK(root.dispatch_key_down({Key::Escape}) == true);
    CHECK(child == 1);
    CHECK(parent == 1);
}

TEST_CASE("MainAppWidget Escape closes the topmost transient overlay first",
          "[tk][widget][key]")
{
    MainAppWidget app;
    app.image_viewer()->open("mxc://image", "image-key", "image", 100, 100);
    app.show_image_viewer(true);
    app.show_quick_switch(true);

    CHECK(app.dispatch_key_down({Key::Escape}) == true);

    CHECK(app.quick_switcher()->is_open() == false);
    CHECK(app.image_viewer()->is_open() == true);
    CHECK(app.image_viewer()->visible() == true);
}

TEST_CASE("MainAppWidget Escape closes and hides media lightboxes",
          "[tk][widget][key]")
{
    MainAppWidget app;
    app.image_viewer()->open("mxc://image", "image-key", "image", 100, 100);
    app.show_image_viewer(true);

    CHECK(app.dispatch_key_down({Key::Escape}) == true);

    CHECK(app.image_viewer()->is_open() == false);
    CHECK(app.image_viewer()->visible() == false);
}

TEST_CASE("MainAppWidget show_room closes and hides an open video lightbox",
          "[tk][widget]")
{
    MainAppWidget app;
    app.video_viewer()->open("mxc://example.org/v", "", "video/mp4", 0u, 640,
                             360);
    app.show_video_viewer(true);
    REQUIRE(app.video_viewer()->is_open());

    app.show_room();

    CHECK(app.video_viewer()->is_open() == false);
    CHECK(app.video_viewer()->visible() == false);
}

TEST_CASE("MainAppWidget show_room closes and hides an open image lightbox",
          "[tk][widget]")
{
    MainAppWidget app;
    app.image_viewer()->open("mxc://image", "image-key", "image", 100, 100);
    app.show_image_viewer(true);
    REQUIRE(app.image_viewer()->is_open());

    app.show_room();

    CHECK(app.image_viewer()->is_open() == false);
    CHECK(app.image_viewer()->visible() == false);
}

TEST_CASE("MainAppWidget Escape closes in-room search",
          "[tk][widget][key]")
{
    MainAppWidget app;
    app.room_view()->set_room({.id = "!room:example.org", .name = "Room"});
    app.room_view()->open_room_search();

    CHECK(app.room_view()->room_search_open() == true);
    CHECK(app.dispatch_key_down({Key::Escape}) == true);
    CHECK(app.room_view()->room_search_open() == false);
}

TEST_CASE("MainAppWidget routes primary shortcut callbacks",
          "[tk][widget][key]")
{
    MainAppWidget app;
    int quick_switch = 0;
    int message_search = 0;
    int find_in_room = 0;
    app.on_quick_switch_shortcut = [&] { ++quick_switch; };
    app.on_message_search_shortcut = [&] { ++message_search; };
    app.on_find_in_room_shortcut = [&] { ++find_in_room; };

    KeyEvent quick{};
    quick.key = Key::Character;
    quick.text = "k";
    quick.ctrl = true;
    CHECK(app.dispatch_key_down(quick) == true);

    KeyEvent find{};
    find.key = Key::Character;
    find.text = "f";
    find.ctrl = true;
    CHECK(app.dispatch_key_down(find) == true);

    KeyEvent search{};
    search.key = Key::Character;
    search.text = "F";
    search.ctrl = true;
    search.shift = true;
    CHECK(app.dispatch_key_down(search) == true);

    CHECK(quick_switch == 1);
    CHECK(find_in_room == 1);
    CHECK(message_search == 1);
}

TEST_CASE("MainAppWidget accepts Command as the primary shortcut modifier",
          "[tk][widget][key]")
{
    MainAppWidget app;
    int quick_switch = 0;
    app.on_quick_switch_shortcut = [&] { ++quick_switch; };

    KeyEvent quick{};
    quick.key = Key::Character;
    quick.text = "k";
    quick.meta = true;

    CHECK(app.dispatch_key_down(quick) == true);
    CHECK(quick_switch == 1);
}

TEST_CASE("MainAppWidget routes history navigation shortcuts",
          "[tk][widget][key]")
{
    MainAppWidget app;
    int back = 0;
    int forward = 0;
    app.on_history_back_shortcut = [&] { ++back; };
    app.on_history_forward_shortcut = [&] { ++forward; };

    KeyEvent alt_left{};
    alt_left.key = Key::Left;
    alt_left.alt = true;
    CHECK(app.dispatch_key_down(alt_left) == true);

    KeyEvent alt_right{};
    alt_right.key = Key::Right;
    alt_right.alt = true;
    CHECK(app.dispatch_key_down(alt_right) == true);

    KeyEvent cmd_bracket{};
    cmd_bracket.key = Key::Character;
    cmd_bracket.text = "[";
    cmd_bracket.meta = true;
    CHECK(app.dispatch_key_down(cmd_bracket) == true);

    KeyEvent cmd_close_bracket{};
    cmd_close_bracket.key = Key::Character;
    cmd_close_bracket.text = "]";
    cmd_close_bracket.meta = true;
    CHECK(app.dispatch_key_down(cmd_close_bracket) == true);

    CHECK(back == 2);
    CHECK(forward == 2);
}

TEST_CASE("MainAppWidget space nav routes header and back clicks",
          "[tk][widget][pointer]")
{
    MainAppWidget app;
    int back = 0;
    int header = 0;
    app.on_space_back = [&] { ++back; };
    app.on_space_header = [&] { ++header; };
    app.set_space_nav(true, "Space", "");

    TkWidgetsStage st;
    auto lc = st.layout_ctx();
    app.arrange(lc, {0, 0, 400, 600});

    Widget* header_hit = app.dispatch_pointer_down({80, 12});
    REQUIRE(header_hit != nullptr);
    header_hit->on_pointer_up(header_hit->world_to_local({80, 12}), true);

    Widget* back_hit = app.dispatch_pointer_down({12, 12});
    REQUIRE(back_hit != nullptr);
    back_hit->on_pointer_up(back_hit->world_to_local({12, 12}), true);

    CHECK(header == 1);
    CHECK(back == 1);
}

TEST_CASE("MainAppWidget hides verification banner behind encryption setup",
          "[tk][widget]")
{
    MainAppWidget app;

    app.show_verif_banner(true);
    REQUIRE(app.verif_banner()->visible() == true);
    app.show_encryption_setup(true);
    CHECK(app.verif_banner()->visible() == false);
    app.show_encryption_setup(false);
    CHECK(app.verif_banner()->visible() == true);

    app.show_verif_banner(false);
    app.show_encryption_setup(true);
    app.show_verif_banner(true);
    CHECK(app.verif_banner()->visible() == false);
}

TEST_CASE("MainAppWidget offline banner shifts chat content",
          "[tk][widget][layout]")
{
    MainAppWidget app;
    app.room_view()->set_room({.id = "!room:example.org", .name = "Room"});

    TkWidgetsStage st;
    auto lc = st.layout_ctx();
    app.arrange(lc, {0, 0, 400, 600});
    const auto online_bounds = app.room_view()->bounds();

    app.set_offline(true);
    app.arrange(lc, {0, 0, 400, 600});
    const auto offline_bounds = app.room_view()->bounds();

    CHECK(online_bounds.h > 0.0f);
    CHECK(offline_bounds.y == online_bounds.y + 32.0f);
    CHECK(offline_bounds.h == online_bounds.h - 32.0f);
}

TEST_CASE("MainAppWidget publishes native overlay registry entries",
          "[tk][widget][layout]")
{
    MainAppWidget app;
    TkWidgetsStage st;
    auto lc = st.layout_ctx();
    app.arrange(lc, {0, 0, 400, 600});

    auto overlays = app.native_overlays();
    CHECK(overlays.entries().size() == 9);
    REQUIRE(overlays.find(NativeOverlayId::ComposeTextArea) != nullptr);
    REQUIRE(overlays.find(NativeOverlayId::RoomSearchField) != nullptr);
    REQUIRE(overlays.find(NativeOverlayId::QuickSwitchField) != nullptr);
    CHECK(overlays.find(NativeOverlayId::QuickSwitchField)->visible == false);

    app.show_quick_switch(true);
    app.arrange(lc, {0, 0, 400, 600});
    overlays = app.native_overlays();

    const auto* quick = overlays.find(NativeOverlayId::QuickSwitchField);
    REQUIRE(quick != nullptr);
    CHECK(quick->kind == NativeOverlayKind::TextField);
    CHECK(quick->visible == true);
    CHECK(quick->rect.empty() == false);
}

// ─────────────────────────────────────────────────────────────────────────
//  Atomic widgets
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Label::measure returns a non-zero size for non-empty text",
          "[tk][widget][label]")
{
    TkWidgetsStage st;
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
    TkWidgetsStage st;
    auto btn = std::make_unique<Button>("Sign in");

    st.run(*btn, {10, 10, 120, 36});

    // A pixel inside the rounded rect but clear of the centred glyph
    // should be the accent fill (default Primary). Sample near the
    // right edge, vertically centred — well past the end of "Sign in".
    auto accent = Theme::light().palette.accent;
    auto px = st.surface->read_pixel(120, 28);
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
    TkWidgetsStage st;
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
    TkWidgetsStage st;
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
    TkWidgetsStage st;
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
    TkWidgetsStage st;
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
    TkWidgetsStage st;

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
    TkWidgetsStage st;
    LoginView view;

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

TEST_CASE("font_role_pt scales relative to body base", "[font_role]")
{
    // At body=12pt the computed sizes must reproduce the legacy hardcoded values.
    CHECK(tk::font_role_pt(tk::FontRole::Small,           12) ==  8);
    CHECK(tk::font_role_pt(tk::FontRole::Body,            12) == 12);
    CHECK(tk::font_role_pt(tk::FontRole::SenderName,      12) == 11);
    CHECK(tk::font_role_pt(tk::FontRole::Timestamp,       12) ==  9);
    CHECK(tk::font_role_pt(tk::FontRole::SidebarName,     12) == 12);
    CHECK(tk::font_role_pt(tk::FontRole::SidebarPreview,  12) == 10);
    CHECK(tk::font_role_pt(tk::FontRole::UnreadBadge,     12) == 10);
    CHECK(tk::font_role_pt(tk::FontRole::Title,           12) == 14);
    CHECK(tk::font_role_pt(tk::FontRole::UiSemibold,      12) == 10);
    CHECK(tk::font_role_pt(tk::FontRole::BigEmoji,        12) == 24);
    CHECK(tk::font_role_pt(tk::FontRole::EmojiPickerCell, 12) == 17);

    // At body=11pt (typical KDE Noto Sans) every role scales down by 1,
    // except BigEmoji which is 2×body.
    CHECK(tk::font_role_pt(tk::FontRole::Small,           11) ==  7);
    CHECK(tk::font_role_pt(tk::FontRole::Body,            11) == 11);
    CHECK(tk::font_role_pt(tk::FontRole::Title,           11) == 13);
    CHECK(tk::font_role_pt(tk::FontRole::BigEmoji,        11) == 22);

    // Sizes are clamped to at least 6pt.
    CHECK(tk::font_role_pt(tk::FontRole::Small,            6) ==  6); // 6-4=2, clamped

    // InlineEmoji = (base+1)×5/4 — ~125% of body, scales with base.
    CHECK(tk::font_role_pt(tk::FontRole::InlineEmoji,     12) == 16); // (13*5)/4=16
    CHECK(tk::font_role_pt(tk::FontRole::InlineEmoji,     11) == 15); // (12*5)/4=15
    CHECK(tk::font_role_pt(tk::FontRole::InlineEmoji,      3) ==  6); // 4*5/4=5, clamped
}
