#include <catch2/catch_test_macros.hpp>

#include "tk/layout.h"
#include "tk/text_area.h"
#include "tk/text_field.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>

// Widget visibility cascades: a widget is effectively visible only if its own
// visible_ flag is set AND every ancestor is visible. Hiding a container must
// therefore also hide the native OS control (QLineEdit / GtkEntry) backing a
// tk::TextField/TextArea nested inside it — tk visibility used not to cascade,
// so a dismissed panel left its native text control painting a stale rectangle
// (visible once the field was disabled, since a disabled QLineEdit paints an
// opaque background). See RoomSearchBar / ThreadListView.

using namespace tk;

namespace
{
struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(200, 120);
    LayoutCtx lc() { return LayoutCtx{surface->factory(), Theme::light()}; }
};
} // namespace

TEST_CASE("visible() reflects a hidden ancestor", "[tk][widget][visibility]")
{
    auto box_owner = create_root_widget<VBox>(nullptr);
    VBox& box = *box_owner;
    auto* a = box.add_child(create_root_widget<VBox>(nullptr));
    auto* leaf = a->add_child(create_root_widget<VBox>(nullptr));

    CHECK(leaf->visible());
    CHECK(a->visible());

    box.set_visible(false);
    CHECK_FALSE(box.visible());
    CHECK_FALSE(a->visible());   // cascaded down
    CHECK_FALSE(leaf->visible());

    // The leaf's own flag is untouched — it re-emerges when the ancestor does.
    a->set_visible(false); // hide the middle node too
    box.set_visible(true);
    CHECK(box.visible());
    CHECK_FALSE(a->visible());    // its own flag is now false
    CHECK_FALSE(leaf->visible()); // still hidden via `a`
    a->set_visible(true);
    CHECK(a->visible());
    CHECK(leaf->visible());
}

TEST_CASE("a child added under a hidden parent inherits the hidden state",
          "[tk][widget][visibility]")
{
    auto box_owner = create_root_widget<VBox>(nullptr);
    VBox& box = *box_owner;
    box.set_visible(false);

    auto* late = box.add_child(create_root_widget<VBox>(nullptr));
    CHECK_FALSE(late->visible());

    box.set_visible(true);
    CHECK(late->visible());
}

TEST_CASE("hiding an ancestor hides a nested TextField's native control",
          "[tk][widget][visibility][text_field]")
{
    StubHost host;
    auto box_owner = create_root_widget<VBox>(&host);
    VBox& box = *box_owner;
    host.set_root(&box);
    auto* field = box.add_child(create_root_widget<TextField>(&host, 30.0f));

    Stage st;
    auto lc = st.lc();
    box.arrange(lc, {0, 0, 200, 120});
    REQUIRE(host.fields_created.size() == 1);
    StubTextField* native = host.fields_created[0];
    REQUIRE(native->visible_); // created shown

    box.set_visible(false);
    CHECK_FALSE(field->visible());
    CHECK_FALSE(native->visible_); // native control pushed hidden by the cascade

    box.set_visible(true);
    CHECK(field->visible());
    CHECK(native->visible_);
}

TEST_CASE("hiding an ancestor hides a nested TextArea's native control",
          "[tk][widget][visibility][text_area]")
{
    StubHost host;
    auto box_owner = create_root_widget<VBox>(&host);
    VBox& box = *box_owner;
    host.set_root(&box);
    auto* area = box.add_child(create_root_widget<TextArea>(&host, 40.0f));

    Stage st;
    auto lc = st.lc();
    box.arrange(lc, {0, 0, 200, 120});
    REQUIRE(host.areas_created.size() == 1);
    StubTextArea* native = host.areas_created[0];
    REQUIRE(native->visible_);

    box.set_visible(false);
    CHECK_FALSE(native->visible_);

    box.set_visible(true);
    CHECK(native->visible_);
}
