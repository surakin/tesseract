#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "tk/list_view.h"
#include "tk/theme.h"
#include "views/EmojiPicker.h"
#include "views/StickerPicker.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

// Exercises the generic GridAdapterAccessibility mechanism (mirrors
// ListAdapterAccessibility for tk::GridView) and its real consumer,
// TabbedGridPicker::GridAdapter — shared by EmojiPicker and StickerPicker,
// so mapping it once in the base class covers both subclasses.

using namespace tk;
using tesseract::views::EmojiPicker;
using tesseract::views::StickerPicker;

namespace
{

struct GridPickerAccessibilityStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 360);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
    }
};

const AccessNode* find_role_grid(const AccessNode& node, Role role)
{
    if (node.role == role)
        return &node;
    for (const auto& ch : node.children)
        if (const AccessNode* found = find_role_grid(ch, role))
            return found;
    return nullptr;
}

} // namespace

TEST_CASE("EmojiPicker's grid cells are accessible GridCells named after "
         "their shortcode, under a Grid node",
         "[tk][view][emoji][accessibility]")
{
    GridPickerAccessibilityStage st;
    auto picker_owner = tk::create_root_widget<EmojiPicker>(nullptr);
    EmojiPicker& picker = *picker_owner;
    // TabbedGridPicker's constructor ends with set_visible(false) — hidden
    // until the owning RoomView actually opens it (see its own doc
    // comment). build_access_tree() correctly refuses to walk an
    // invisible root, so show it first to test the tree as a screen
    // reader would actually see it while the picker is open.
    picker.set_visible(true);
    st.run(picker, {0, 0, 320, 360});

    AccessNode tree = build_access_tree(&picker);
    const AccessNode* grid = find_role_grid(tree, Role::Grid);
    REQUIRE(grid != nullptr);

    // The default category has built-in emoji, each with a shortcode —
    // at least one real GridCell should be present, non-empty-named, with
    // a valid row_index/row_set_size (set-size is the adapter's raw
    // count(), which can exceed grid->children.size() since cells with an
    // empty tooltip are filtered to Role::None and excluded — see
    // access_role_for_cell()).
    bool found_named_cell = false;
    for (const auto& cell : grid->children)
    {
        CHECK(cell.role == Role::GridCell);
        CHECK(cell.row_index >= 0);
        CHECK(cell.row_set_size > cell.row_index);
        if (!cell.name.empty())
        {
            found_named_cell = true;
            // cell_tooltip() returns the colon-wrapped chat-typing form
            // (":thumbs_up:") — the accessible name must strip both, so a
            // screen reader doesn't announce "colon thumbs underscore up
            // colon".
            CHECK(cell.name.front() != ':');
            CHECK(cell.name.back() != ':');
        }
    }
    CHECK(found_named_cell);
}

TEST_CASE("StickerPicker's grid cells are accessible GridCells under a "
         "Grid node (empty when no shortcode, matching cell_tooltip's own "
         "suppression)",
         "[tk][view][sticker][accessibility]")
{
    GridPickerAccessibilityStage st;
    auto picker_owner = tk::create_root_widget<StickerPicker>(nullptr);
    StickerPicker& picker = *picker_owner;
    // See the EmojiPicker test's identical comment on set_visible(true).
    picker.set_visible(true);
    st.run(picker, {0, 0, 320, 360});

    AccessNode tree = build_access_tree(&picker);
    const AccessNode* grid = find_role_grid(tree, Role::Grid);
    REQUIRE(grid != nullptr);
    // No packs loaded in this unit test — zero cells is a valid, crash-free
    // outcome; the real assertion is that the Grid node itself exists and
    // any cells that do appear are well-formed.
    for (const auto& cell : grid->children)
        CHECK(cell.role == Role::GridCell);
}

TEST_CASE("invoking an EmojiPicker grid cell's default action fires "
         "on_selected with that cell's glyph, the same as a real click",
         "[tk][view][emoji][accessibility][action]")
{
    GridPickerAccessibilityStage st;
    auto picker_owner = tk::create_root_widget<EmojiPicker>(nullptr);
    EmojiPicker& picker = *picker_owner;
    picker.set_visible(true);
    st.run(picker, {0, 0, 320, 360});

    std::string selected;
    picker.on_selected = [&](const std::string& glyph) { selected = glyph; };

    AccessNode tree = build_access_tree(&picker);
    const AccessNode* grid = find_role_grid(tree, Role::Grid);
    REQUIRE(grid != nullptr);
    REQUIRE_FALSE(grid->children.empty());
    const AccessNode& cell = grid->children.front();

    CHECK(invoke_default_action(cell));
    CHECK_FALSE(selected.empty());
}
