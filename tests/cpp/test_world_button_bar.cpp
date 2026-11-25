// The world view's button bar: hidden at first, opened by its handle or the WorldBar shortcut,
// real buttons that Tab reaches, disabled entries that stay quiet.

#include <catch2/catch_test_macros.hpp>

#include "access_test_util.h"
#include "tk/access_tree.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/world/WorldButtonBar.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::world::WorldButtonBar;

namespace world_bar_test
{

struct BarStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 400);
    StubHost host;
    std::unique_ptr<WorldButtonBar> bar;
    std::vector<std::string> fired;

    explicit BarStage(std::vector<WorldButtonBar::Entry> entries = {{"import", "Import", true},
                                                                    {"export", "Export", true}})
    {
        bar = tk::create_root_widget<WorldButtonBar>(&host, std::move(entries));
        host.set_root(bar.get());
        bar->on_activate = [this](const std::string& id)
        {
            fired.push_back(id);
        };
        run();
    }

    void run()
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        const Size s = bar->measure(lc, {400.0f, 400.0f});
        bar->arrange(lc, {10, 10, s.w, s.h});
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        pc.host = &host;
        bar->paint(pc);
    }
};

KeyEvent bar_key()
{
    KeyEvent e{};
    e.key = Key::Character;
    e.text = "b";
    return e;
}

} // namespace world_bar_test

using namespace world_bar_test;

TEST_CASE("the button bar starts hidden", "[world][button bar]")
{
    BarStage st;
    CHECK_FALSE(st.bar->expanded());
    CHECK_FALSE(st.bar->entry_button("import")->visible());
    CHECK_FALSE(st.bar->entry_button("export")->visible());
    CHECK(st.bar->handle_button()->visible());
}

TEST_CASE("the button bar's handle click expands and collapses", "[world][button bar]")
{
    BarStage st;
    st.bar->handle_button()->click();
    st.run();
    CHECK(st.bar->expanded());
    CHECK(st.bar->entry_button("import")->visible());
    CHECK(st.bar->entry_button("export")->visible());

    st.bar->handle_button()->click();
    st.run();
    CHECK_FALSE(st.bar->expanded());
    CHECK_FALSE(st.bar->entry_button("import")->visible());
}

TEST_CASE("a disabled button bar entry does not fire", "[world][button bar]")
{
    BarStage st({{"import", "Import", false}, {"export", "Export", true}});
    st.bar->set_expanded(true);
    st.run();
    st.bar->entry_button("import")->click();
    CHECK(st.fired.empty());
    st.bar->entry_button("export")->click();
    REQUIRE(st.fired.size() == 1);
    CHECK(st.fired[0] == "export");

    st.bar->set_entry_enabled("import", true);
    st.bar->entry_button("import")->click();
    CHECK(st.fired.back() == "import");
    st.bar->set_entry_enabled("import", false);
    CHECK_FALSE(st.bar->entry_enabled("import"));
    st.bar->entry_button("import")->click();
    CHECK(st.fired.size() == 2);
}

TEST_CASE("the button bar keyboard toggles and focuses the first entry", "[world][button bar]")
{
    BarStage st({{"import", "Import", false}, {"export", "Export", true}});
    // A "B" while the keyboard is elsewhere is not the bar's.
    CHECK_FALSE(st.bar->on_key_down(bar_key()));
    CHECK_FALSE(st.bar->expanded());
    st.host.request_focus(st.bar->handle_button());
    CHECK(st.bar->on_key_down(bar_key()));
    st.run();
    CHECK(st.bar->expanded());
    // The first enabled entry, not the disabled one before it.
    CHECK(st.host.focused_widget() == st.bar->entry_button("export"));

    CHECK(st.bar->on_key_down(bar_key()));
    CHECK_FALSE(st.bar->expanded());
    st.run();

    bool closed_by_key = false;
    st.bar->on_closed_by_key = [&]
    {
        closed_by_key = true;
    };
    st.host.request_focus(st.bar->handle_button());
    st.bar->on_key_down(bar_key());
    st.run();
    KeyEvent esc{};
    esc.key = Key::Escape;
    CHECK(st.bar->on_key_down(esc));
    CHECK_FALSE(st.bar->expanded());
    CHECK(closed_by_key);
    CHECK_FALSE(st.bar->on_key_down(esc)); // closed: nothing to do
}

TEST_CASE("the button bar entries are reachable by Tab and have accessible names",
          "[world][button bar]")
{
    BarStage st;
    st.bar->set_expanded(true);
    st.run();
    st.host.request_focus(st.bar->handle_button());
    CHECK(st.host.focused_widget() == st.bar->handle_button());
    st.host.dispatch_key_down(KeyEvent{Key::Tab});
    CHECK(st.host.focused_widget() == st.bar->entry_button("import"));
    st.host.dispatch_key_down(KeyEvent{Key::Tab});
    CHECK(st.host.focused_widget() == st.bar->entry_button("export"));

    const AccessNode tree = build_access_tree(st.bar.get());
    CHECK(tree.name == "World actions");
    CHECK(tree.state.expanded);
    // The handle says what pressing it does, not the group's name again.
    CHECK(access_test::find_named(tree, "Hide world actions") != nullptr);
    st.bar->set_expanded(false);
    CHECK_FALSE(build_access_tree(st.bar.get()).state.expanded);
    CHECK(access_test::find_named(build_access_tree(st.bar.get()), "Show world actions") !=
          nullptr);
    st.bar->set_expanded(true);
    CHECK(access_test::find_named(tree, "World actions") != nullptr);
    CHECK(access_test::find_named(tree, "Import") != nullptr);
    CHECK(access_test::find_named(tree, "Export") != nullptr);
}
