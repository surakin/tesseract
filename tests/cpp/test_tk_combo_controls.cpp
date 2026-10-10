#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/combo_button.h"
#include "tk/combobox.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;

namespace
{

struct ComboStage
{
    PopupCapableStubHost host;
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 300);

    LayoutCtx lc() { return LayoutCtx{surface->factory(), Theme::light()}; }
    PaintCtx pc()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void layout(Widget& w, Rect r = {10, 10, 200, 40})
    {
        auto l = lc();
        w.measure(l, {r.w, r.h});
        w.arrange(l, r);
    }
    void paint(Widget& w)
    {
        auto p = pc();
        w.paint(p);
    }
};

KeyEvent combo_key(Key k)
{
    KeyEvent e;
    e.key = k;
    return e;
}

const std::vector<ComboBox::Option> kBoxOpts = {
    {"One", "1"}, {"Two", "2"}, {"Three", "3"}};
const std::vector<ComboButton::Option> kBtnOpts = {
    {"Room", "room"}, {"Space", "space"}};

} // namespace

// ── ComboBox ───────────────────────────────────────────────────────────────

TEST_CASE("ComboBox measures to the constraint width and a fixed height",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    auto l = s.lc();
    Size sz = cb->measure(l, {180, 100});
    CHECK(sz.w == 180);
    CHECK(sz.h > 0);
    CHECK(cb->measure(l, {0, 100}).w == 0);
}

TEST_CASE("ComboBox click opens the popup and a row commit fires on_changed",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    cb->set_selected_value("2");
    s.layout(*cb);
    s.paint(*cb);

    std::string got;
    cb->on_changed = [&](std::string v) { got = v; };

    CHECK(cb->access_name() == "Two");
    CHECK(cb->on_pointer_down({5, 5}));
    CHECK_FALSE(cb->is_expanded());
    cb->on_pointer_up({5, 5}, true);
    REQUIRE(cb->is_expanded());
    REQUIRE(s.host.popups_created.size() == 1);
    CHECK(s.host.popups_created[0]->visible());
    CHECK(cb->access_state().expanded);

    // Keyboard drive: the selected row is pre-hovered; Down moves to "Three".
    CHECK(cb->on_key_down(combo_key(Key::Down)));
    CHECK(cb->on_key_down(combo_key(Key::Down))); // clamped at the last row
    CHECK(cb->on_key_down(combo_key(Key::Enter)));
    CHECK(got == "3");
    CHECK(cb->selected_value() == "3");
    CHECK_FALSE(cb->is_expanded());
    CHECK_FALSE(s.host.popups_created[0]->visible());
}

TEST_CASE("ComboBox second click collapses; Escape collapses without a change",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    s.layout(*cb);
    int changes = 0;
    cb->on_changed = [&](std::string) { ++changes; };

    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true);
    REQUIRE(cb->is_expanded());
    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true);
    CHECK_FALSE(cb->is_expanded());

    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true);
    REQUIRE(cb->is_expanded());
    CHECK(cb->on_key_down(combo_key(Key::Escape)));
    CHECK_FALSE(cb->is_expanded());
    CHECK(changes == 0);
}

TEST_CASE("ComboBox Enter with no hovered row just collapses; other keys are "
          "swallowed while open; Tab collapses and passes through",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    s.layout(*cb);
    int changes = 0;
    cb->on_changed = [&](std::string) { ++changes; };

    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true); // no selected value => no hover
    REQUIRE(cb->is_expanded());
    CHECK(cb->on_key_down(combo_key(Key::Left)));  // swallowed
    CHECK(cb->on_key_down(combo_key(Key::Enter))); // nothing hovered
    CHECK_FALSE(cb->is_expanded());
    CHECK(changes == 0);

    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true);
    REQUIRE(cb->is_expanded());
    CHECK_FALSE(cb->on_key_down(combo_key(Key::Tab)));
    CHECK_FALSE(cb->is_expanded());
}

TEST_CASE("ComboBox closed keyboard opening requires focus", "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    cb->set_selected_value("1");
    s.layout(*cb);

    CHECK_FALSE(cb->on_key_down(combo_key(Key::Down))); // unfocused: ignored
    CHECK_FALSE(cb->is_expanded());

    s.host.request_focus(cb.get());
    REQUIRE(cb->has_focus());
    CHECK_FALSE(cb->on_key_down(combo_key(Key::Left)));
    CHECK(cb->on_key_down(combo_key(Key::Space)));
    CHECK(cb->is_expanded());
}

TEST_CASE("ComboBox wheel over the closed button cycles the selection",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    cb->set_selected_value("1");
    s.layout(*cb);
    std::vector<std::string> seen;
    cb->on_changed = [&](std::string v) { seen.push_back(v); };

    CHECK_FALSE(cb->on_wheel({5, 5}, 0, 0));
    CHECK_FALSE(cb->on_wheel({5, 5}, 0, -1)); // already first: clamped, no-op
    CHECK(cb->on_wheel({5, 5}, 0, 1));
    CHECK(cb->on_wheel({5, 5}, 0, 1));
    CHECK_FALSE(cb->on_wheel({5, 5}, 0, 1)); // already last
    CHECK(seen == std::vector<std::string>{"2", "3"});

    // While expanded the wheel is not handled.
    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true);
    REQUIRE(cb->is_expanded());
    CHECK_FALSE(cb->on_wheel({5, 5}, 0, -1));
}

TEST_CASE("ComboBox disabled ignores input and collapses an open dropdown",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    s.layout(*cb);
    cb->on_pointer_down({5, 5});
    cb->on_pointer_up({5, 5}, true);
    REQUIRE(cb->is_expanded());

    cb->set_enabled(false);
    CHECK_FALSE(cb->is_expanded());
    CHECK_FALSE(cb->focusable());
    CHECK_FALSE(cb->on_pointer_down({5, 5}));
    CHECK_FALSE(cb->on_pointer_move({5, 5}));
    CHECK_FALSE(cb->on_key_down(combo_key(Key::Down)));
    CHECK_FALSE(cb->on_wheel({5, 5}, 0, 1));
    CHECK_FALSE(cb->access_default_action());
}

TEST_CASE("ComboBox pointer move hover shows/hides its tooltip",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    cb->set_tooltip("Pick one");
    s.layout(*cb);

    CHECK(cb->on_pointer_move({5, 5}));
    CHECK(s.host.tooltip_owner_ == cb.get());
    CHECK_FALSE(cb->on_pointer_move({6, 6})); // no change
    CHECK(cb->on_pointer_move({500, 500}));   // moved off
    CHECK(s.host.tooltip_owner_ == nullptr);

    cb->on_pointer_move({5, 5});
    cb->on_pointer_leave();
    CHECK(s.host.tooltip_owner_ == nullptr);
}

TEST_CASE("ComboBox access default action toggles; popup dismiss collapses; "
          "theme change and relayout while open are safe",
          "[tk][combobox]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    s.layout(*cb);
    CHECK(cb->access_default_action());
    REQUIRE(cb->is_expanded());
    cb->on_theme_changed(Theme::dark());
    s.layout(*cb, {10, 10, 260, 40}); // new width -> reposition popup
    cb->set_options({{"Only", "o"}});
    CHECK(cb->access_default_action());
    CHECK_FALSE(cb->is_expanded());
    cb->access_default_action();
    cb->on_popup_dismiss();
    CHECK_FALSE(cb->is_expanded());
    s.paint(*cb);
}

TEST_CASE("ComboBox dropdown rows are exposed and activatable",
          "[tk][combobox][accessibility]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboBox>(&s.host);
    cb->set_options(kBoxOpts);
    cb->set_selected_value("2");
    s.layout(*cb);
    std::string got;
    cb->on_changed = [&](std::string v) { got = v; };
    cb->access_default_action();
    REQUIRE(s.host.popups_created.size() == 1);
    Widget* list = s.host.popups_created[0]->root();
    REQUIRE(list != nullptr);

    auto* rows = dynamic_cast<WidgetRowAccessibility*>(list);
    REQUIRE(rows != nullptr);
    REQUIRE(rows->access_row_count() == 3);
    CHECK(rows->access_name_for_widget_row(0) == "One");
    CHECK(rows->access_state_for_widget_row(1).selected);
    CHECK_FALSE(rows->access_state_for_widget_row(0).selected);
    CHECK(rows->access_rect_for_widget_row(2).h > 0);
    CHECK_FALSE(rows->access_activate_widget_row(9));
    CHECK(rows->access_activate_widget_row(2));
    CHECK(got == "3");

    // Paint the list and drive its pointer path (row hover + click).
    cb->access_default_action();
    auto l = s.lc();
    list->measure(l, {200, 96});
    list->arrange(l, {0, 0, 200, 96});
    s.paint(*list);
    list->on_pointer_move({10, 40});
    CHECK(list->on_pointer_down({10, 40}));
    list->on_pointer_up({10, 40}, true);
    CHECK(got == "2");
}

// ── ComboButton ────────────────────────────────────────────────────────────

TEST_CASE("ComboButton main click activates with the selected value, chevron "
          "opens the dropdown",
          "[tk][combo_button]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    cb->set_selected_value("space");
    s.layout(*cb);
    s.paint(*cb);

    std::string activated, changed;
    cb->on_activate = [&](std::string v) { activated = v; };
    cb->on_selection_changed = [&](std::string v) { changed = v; };

    CHECK(cb->access_name() == "Space");
    CHECK(cb->on_pointer_down({10, 10}));
    cb->on_pointer_up({10, 10}, true);
    CHECK(activated == "space");
    CHECK_FALSE(cb->is_expanded());

    // Press on main, release outside: no activation.
    activated.clear();
    cb->on_pointer_down({10, 10});
    cb->on_pointer_up({10, 10}, false);
    CHECK(activated.empty());

    // Chevron zone is at the right edge.
    CHECK(cb->on_pointer_down({195, 10}));
    cb->on_pointer_up({195, 10}, true);
    REQUIRE(cb->is_expanded());
    CHECK(cb->access_state().expanded);
    CHECK(changed.empty());
    s.paint(*cb);

    // Dropdown pick changes the mode but does not activate.
    cb->on_key_down(combo_key(Key::Up));
    cb->on_key_down(combo_key(Key::Enter));
    CHECK(changed == "room");
    CHECK(activated.empty());
    CHECK(cb->selected_value() == "room");
    CHECK_FALSE(cb->is_expanded());

    // Pressing outside both zones does nothing.
    CHECK_FALSE(cb->on_pointer_down({500, 500}));
}

TEST_CASE("ComboButton chevron click while open collapses", "[tk][combo_button]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    s.layout(*cb);
    cb->on_pointer_down({195, 10});
    cb->on_pointer_up({195, 10}, true);
    REQUIRE(cb->is_expanded());
    cb->on_pointer_down({195, 10});
    cb->on_pointer_up({195, 10}, true);
    CHECK_FALSE(cb->is_expanded());
}

TEST_CASE("ComboButton keyboard: Enter activates, Down opens, Escape/Tab "
          "close",
          "[tk][combo_button]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    cb->set_selected_value("room");
    s.layout(*cb);
    std::vector<std::string> acts;
    cb->on_activate = [&](std::string v) { acts.push_back(v); };

    CHECK_FALSE(cb->on_key_down(combo_key(Key::Enter))); // unfocused
    s.host.request_focus(cb.get());
    REQUIRE(cb->has_focus());
    CHECK(cb->on_key_down(combo_key(Key::Enter)));
    CHECK(cb->on_key_down(combo_key(Key::Space)));
    CHECK(acts == std::vector<std::string>{"room", "room"});
    CHECK_FALSE(cb->on_key_down(combo_key(Key::Left)));

    CHECK(cb->on_key_down(combo_key(Key::Down)));
    REQUIRE(cb->is_expanded());
    CHECK(cb->on_key_down(combo_key(Key::Left))); // swallowed
    CHECK(cb->on_key_down(combo_key(Key::Escape)));
    CHECK_FALSE(cb->is_expanded());

    cb->on_key_down(combo_key(Key::Down));
    REQUIRE(cb->is_expanded());
    CHECK_FALSE(cb->on_key_down(combo_key(Key::Tab)));
    CHECK_FALSE(cb->is_expanded());

    // Enter with nothing hovered while open just collapses.
    cb->set_selected_value("nonexistent");
    cb->on_key_down(combo_key(Key::Down));
    REQUIRE(cb->is_expanded());
    CHECK(cb->on_key_down(combo_key(Key::Enter)));
    CHECK_FALSE(cb->is_expanded());
}

TEST_CASE("ComboButton disabled never activates and collapses",
          "[tk][combo_button]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    cb->set_variant(Button::Variant::Subtle);
    s.layout(*cb);
    int acts = 0;
    cb->on_activate = [&](std::string) { ++acts; };
    CHECK(cb->access_default_action());
    CHECK(acts == 1);

    cb->on_pointer_down({195, 10});
    cb->on_pointer_up({195, 10}, true);
    REQUIRE(cb->is_expanded());
    cb->set_enabled(false);
    CHECK_FALSE(cb->is_expanded());
    CHECK_FALSE(cb->focusable());
    CHECK_FALSE(cb->access_default_action());
    CHECK_FALSE(cb->on_pointer_down({10, 10}));
    CHECK_FALSE(cb->on_pointer_move({10, 10}));
    CHECK_FALSE(cb->on_key_down(combo_key(Key::Enter)));
    CHECK(acts == 1);
    s.paint(*cb);
}

TEST_CASE("ComboButton hover state tracks zones and clears on leave",
          "[tk][combo_button]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    s.layout(*cb);
    CHECK(cb->on_pointer_move({10, 10}));
    CHECK_FALSE(cb->on_pointer_move({12, 10}));
    CHECK(cb->on_pointer_move({195, 10})); // main -> chevron
    CHECK(cb->on_pointer_move({500, 500}));
    cb->on_pointer_move({10, 10});
    cb->on_pointer_leave();
    CHECK(cb->on_pointer_move({10, 10})); // hover re-registers after leave
}

TEST_CASE("ComboButton option/selection updates while open and relayout are "
          "safe; dismiss collapses",
          "[tk][combo_button]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    s.layout(*cb);
    cb->on_key_down(combo_key(Key::Down));
    cb->access_default_action(); // activate path, no handler
    cb->on_pointer_down({195, 10});
    cb->on_pointer_up({195, 10}, true);
    REQUIRE(cb->is_expanded());
    cb->set_options({{"A", "a"}, {"B", "b"}, {"C", "c"}});
    cb->set_selected_value("b");
    cb->on_theme_changed(Theme::dark());
    s.layout(*cb, {10, 10, 300, 40});
    s.paint(*cb);
    cb->on_popup_dismiss();
    CHECK_FALSE(cb->is_expanded());
    CHECK(cb->access_name() == "B");
}

TEST_CASE("ComboButton dropdown rows are exposed and activatable",
          "[tk][combo_button][accessibility]")
{
    ComboStage s;
    auto cb = create_root_widget<ComboButton>(&s.host);
    cb->set_options(kBtnOpts);
    s.layout(*cb);
    std::string changed;
    cb->on_selection_changed = [&](std::string v) { changed = v; };
    cb->on_pointer_down({195, 10});
    cb->on_pointer_up({195, 10}, true);
    REQUIRE(s.host.popups_created.size() == 1);
    Widget* list = s.host.popups_created[0]->root();
    auto* rows = dynamic_cast<WidgetRowAccessibility*>(list);
    REQUIRE(rows != nullptr);
    CHECK(rows->access_row_count() == 2);
    CHECK(rows->access_name_for_widget_row(1) == "Space");
    CHECK_FALSE(rows->access_activate_widget_row(5));
    auto l = s.lc();
    list->measure(l, {200, 64});
    list->arrange(l, {0, 0, 200, 64});
    s.paint(*list);
    list->on_pointer_move({10, 40});
    list->on_pointer_down({10, 40});
    list->on_pointer_up({10, 40}, true);
    CHECK(changed == "space");
}
