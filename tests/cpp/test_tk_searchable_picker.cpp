#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/searchable_picker.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace tk;

namespace
{

// Minimal concrete SearchablePicker over a small fruit list. Rank: 0 for a
// prefix match, 1 for a substring match, -1 otherwise.
class SpFruitPicker : public SearchablePicker
{
protected:
    SpFruitPicker()
    {
        if (!host())
            return;
        init_(28.0f, 24.0f, 200.0f, 3, "Fruit");
    }
    TK_WIDGET_FACTORY_FRIEND(SpFruitPicker)

    std::size_t entry_count_() const override { return kFruit.size(); }
    int match_rank_(std::size_t i, std::string_view q) const override
    {
        const std::string& f = kFruit[i];
        if (q.empty())
            return 1;
        if (f.compare(0, q.size(), q) == 0)
            return 0;
        return f.find(q) != std::string::npos ? 1 : -1;
    }
    std::string entry_key_(std::size_t i) const override { return "k-" + kFruit[i]; }
    std::string entry_label_(std::size_t i) const override { return kFruit[i]; }
    std::string entry_display_(std::size_t i) const override
    {
        return "Fruit " + kFruit[i];
    }

private:
    inline static const std::vector<std::string> kFruit = {
        "apple", "banana", "cherry", "grape", "pineapple"};
};

struct SpStage
{
    PopupCapableStubHost host;
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 300);
    std::unique_ptr<SpFruitPicker> picker =
        create_root_widget<SpFruitPicker>(&host);

    StubTextField& field() { return *host.fields_created.at(0); }
    void layout()
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        picker->measure(lc, {300, 100});
        picker->arrange(lc, {10, 10, 200, 28});
    }
    void type(const std::string& s) { field().on_changed(s); }
    std::vector<std::string> rows()
    {
        std::vector<std::string> out;
        if (host.popups_created.empty())
            return out;
        auto* r = dynamic_cast<WidgetRowAccessibility*>(
            host.popups_created[0]->root());
        for (std::size_t i = 0; r && i < r->access_row_count(); ++i)
            out.push_back(r->access_name_for_widget_row(i));
        return out;
    }
};

} // namespace

TEST_CASE("SearchablePicker measure caps width and uses the field height",
          "[tk][searchable_picker]")
{
    SpStage s;
    LayoutCtx lc{s.surface->factory(), Theme::light()};
    Size a = s.picker->measure(lc, {300, 100});
    CHECK(a.w == 200);
    CHECK(a.h == 28);
    CHECK(s.picker->measure(lc, {120, 100}).w == 120);
    CHECK(s.picker->measure(lc, {0, 0}).w == 200);
}

TEST_CASE("SearchablePicker set_value shows the display text and name",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    s.picker->set_value("k-grape");
    CHECK(s.picker->value() == "k-grape");
    CHECK(s.field().text() == "Fruit grape");
    CHECK(s.picker->access_name() == "Fruit grape");
    s.picker->set_value("unknown-key"); // unknown keys display verbatim
    CHECK(s.field().text() == "unknown-key");
    CHECK(s.picker->access_role() == Role::ComboBox);
}

TEST_CASE("SearchablePicker typing filters, ranks and caps the dropdown",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    s.type("ap");
    // prefix matches first (apple), then substrings (grape, pineapple), cap 3.
    CHECK(s.rows() == std::vector<std::string>{"apple", "grape", "pineapple"});
    CHECK(s.picker->access_state().expanded);
    CHECK(s.host.popups_created[0]->visible());

    s.type("zzz"); // no matches collapses
    CHECK_FALSE(s.picker->access_state().expanded);
    CHECK_FALSE(s.host.popups_created[0]->visible());

    s.type("e"); // empty->tier ranking all contain 'e'? apple, cherry, grape, pineapple
    CHECK(s.rows().size() == 3);
}

TEST_CASE("SearchablePicker keyboard nav wraps and Enter commits the hovered "
          "row",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    std::string committed;
    s.picker->on_changed = [&](std::string v) { committed = v; };

    CHECK_FALSE(s.field().on_popup_nav(NavKey::Down)); // closed: not consumed
    s.type("an"); // banana only
    s.type("a");  // apple, banana, grape (cap 3) via prefix then substring
    REQUIRE(s.rows().size() == 3);
    CHECK(s.field().on_popup_nav(NavKey::Down)); // hovered 1
    CHECK(s.field().on_popup_nav(NavKey::Down)); // 2
    CHECK(s.field().on_popup_nav(NavKey::Down)); // wraps to 0
    CHECK(s.field().on_popup_nav(NavKey::Up));   // wraps to 2
    CHECK_FALSE(s.field().on_popup_nav(NavKey::Left));
    s.field().on_submit();
    CHECK(committed == "k-grape");
    CHECK(s.picker->value() == "k-grape");
    CHECK(s.field().text() == "Fruit grape");
    CHECK_FALSE(s.picker->access_state().expanded);
}

TEST_CASE("SearchablePicker Escape collapses and reverts uncommitted typing",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    s.picker->set_value("k-cherry");
    s.type("ban");
    REQUIRE(s.picker->access_state().expanded);
    CHECK(s.field().on_popup_nav(NavKey::Escape));
    CHECK_FALSE(s.picker->access_state().expanded);
    CHECK(s.field().text() == "Fruit cherry");
    CHECK(s.picker->value() == "k-cherry");
}

TEST_CASE("SearchablePicker Enter with no hover commits the first row; with "
          "no matches it just collapses",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    int changes = 0;
    s.picker->on_changed = [&](std::string) { ++changes; };

    s.type("zzz"); // filtered empty
    s.field().on_submit();
    CHECK(changes == 0);

    s.type("pine");
    s.field().on_submit();
    CHECK(changes == 1);
    CHECK(s.picker->value() == "k-pineapple");
}

TEST_CASE("SearchablePicker dropdown rows commit via pointer and a11y",
          "[tk][searchable_picker][accessibility]")
{
    SpStage s;
    s.layout();
    std::vector<std::string> got;
    s.picker->on_changed = [&](std::string v) { got.push_back(v); };

    s.type("a");
    REQUIRE(s.host.popups_created.size() == 1);
    Widget* list = s.host.popups_created[0]->root();
    auto* rows = dynamic_cast<WidgetRowAccessibility*>(list);
    REQUIRE(rows != nullptr);
    CHECK(rows->access_name_for_widget_row(99).empty());
    CHECK(rows->access_rect_for_widget_row(99).w == 0);
    CHECK_FALSE(rows->access_activate_widget_row(99));
    CHECK(rows->access_rect_for_widget_row(1).h == 24);

    LayoutCtx lc{s.surface->factory(), Theme::light()};
    Size sz = list->measure(lc, {200, 0});
    CHECK(sz.h == 3 * 24);
    CHECK(list->measure(lc, {0, 0}).w == 200);
    list->arrange(lc, {0, 0, 200, 72});
    PaintCtx pc{s.surface->canvas(), s.surface->factory(), Theme::light()};
    list->paint(pc);

    CHECK(list->on_pointer_move({10, 30}));
    CHECK_FALSE(list->on_pointer_move({12, 31})); // same row
    CHECK_FALSE(list->on_pointer_down({500, 5})); // outside
    CHECK(list->on_pointer_down({10, 30}));
    list->on_pointer_up({10, 30}, true);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == "k-banana");

    s.type("a");
    CHECK(rows->access_activate_widget_row(0));
    CHECK(got.size() == 2);
}

TEST_CASE("SearchablePicker losing focus collapses and reverts; focus "
          "callback forwards",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    std::vector<bool> focus_events;
    s.picker->set_on_focus_changed([&](bool f) { focus_events.push_back(f); });
    s.picker->set_value("k-apple");

    s.field().on_focus_changed(true);
    s.type("gr");
    REQUIRE(s.picker->access_state().expanded);
    s.field().on_focus_changed(false);
    CHECK_FALSE(s.picker->access_state().expanded);
    CHECK(s.field().text() == "Fruit apple");
    CHECK(focus_events == std::vector<bool>{true, false});
}

TEST_CASE("SearchablePicker disabled/hidden collapse; default action toggles; "
          "theme/layout while open are safe",
          "[tk][searchable_picker]")
{
    SpStage s;
    s.layout();
    CHECK(s.picker->access_default_action());
    // Toggling open with an empty filter list: nothing to show but state flips.
    CHECK(s.picker->access_state().expanded);
    CHECK(s.picker->access_default_action());
    CHECK_FALSE(s.picker->access_state().expanded);

    s.type("a");
    REQUIRE(s.picker->access_state().expanded);
    s.picker->on_theme_changed(Theme::dark());
    s.picker->set_compact(true);
    s.layout();
    s.picker->set_visible(false);
    CHECK_FALSE(s.picker->access_state().expanded);

    s.picker->set_visible(true);
    s.type("a");
    s.picker->set_enabled(false);
    CHECK_FALSE(s.picker->access_state().expanded);
    CHECK_FALSE(s.picker->access_default_action());
}

TEST_CASE("SearchablePicker without a host stays a plain spacer",
          "[tk][searchable_picker]")
{
    auto p = create_root_widget<SpFruitPicker>(nullptr);
    p->set_value("k-apple"); // no field: no crash
    p->set_compact(true);
    p->set_enabled(false);
    p->set_visible(false);
    CHECK(p->value() == "k-apple");
}
