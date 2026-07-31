#pragma once

// Builds an accessibility tree from a live tk::Widget tree, for whatever
// per-platform accessibility bridge consumes it (see the accessibility
// plan doc). Pure C++, no platform dependencies — this file only knows
// about tk::Widget's access_role()/access_name()/access_state(), not about
// AT-SPI, UI Automation, or any other native accessibility API.

#include "widget.h"

#include <string>
#include <vector>

namespace tk
{

// One node in the accessibility tree. Widgets whose access_role() is
// Role::None never produce a node of their own — see build_access_tree's
// comment for how their accessible descendants attach instead.
struct AccessNode
{
    // Borrowed; valid as long as the underlying widget tree is. Lets a
    // consumer map back to the originating widget (e.g. to invoke its
    // on_pointer_up/on_click equivalent when an AT client activates it).
    // For a synthesized virtualized row/cell (row_index >= 0), this is the
    // owning tk::ListView/tk::GridView itself, not a per-item widget — list
    // rows and grid cells have none (see ListAdapter's/GridAdapter's own
    // top comments in list_view.h) — so a consumer dispatching an action
    // against such a node must go through the ListView/GridView +
    // row_index, not widget-level pointer/click dispatch.
    Widget* widget = nullptr;
    Role role = Role::None;
    std::string name;
    AccessState state;
    std::vector<AccessNode> children;

    // >= 0 for a node synthesized from a ListAdapterAccessibility row, a
    // GridAdapterAccessibility cell (see list_view.h), or a
    // WidgetRowAccessibility row (below) rather than walked from a real
    // Widget child. row_set_size is the count at synthesis time, letting a
    // consumer announce "item N of M" without realizing every row/cell.
    // For a grid this is a flat linear position, not yet a full row/column
    // table model (see GridAdapterAccessibility's own comment).
    int row_index = -1;
    int row_set_size = -1;
};

// Optional interface a Widget may implement directly — unlike
// ListAdapterAccessibility/GridAdapterAccessibility, which live on a
// separate ListAdapter/GridAdapter object retrieved via
// ListView::adapter()/GridView::adapter() — for a widget that paints a
// flat list of rows itself, with no per-row child Widget and no
// tk::ListView underneath (e.g. ui/shared/views/ListPopupBase's shared
// popup scaffolding for MentionPopup/ShortcodePopup/SlashCommandPopup,
// which implements ScrollableBase's hit-test/scroll/paint loop directly
// rather than composing a ListView). Discovered via dynamic_cast in
// access_tree.cpp on the Widget itself, exactly like every other optional
// interface here — kept in this file (not list_view.h) since it isn't
// tied to ListAdapter/GridAdapter or tk::ListView/GridView at all.
class WidgetRowAccessibility
{
public:
    virtual ~WidgetRowAccessibility() = default;

    virtual std::size_t access_row_count() const = 0;
    virtual Role access_role_for_widget_row(std::size_t index) const = 0;
    virtual std::string access_name_for_widget_row(std::size_t index) const = 0;
    virtual AccessState access_state_for_widget_row(std::size_t /*index*/) const
    {
        return {};
    }
};

// Walks `root`'s subtree, producing an AccessNode tree. Mirrors
// next_focusable()'s traversal exactly (same reading_order_less ordering,
// same "skip invisible subtrees entirely" rule) so accessibility reading
// order and Tab order can't silently drift apart.
//
// A widget with access_role() == Role::None contributes no node of its
// own — its accessible descendants (if any) attach directly to the
// nearest accessible ancestor instead, so purely-structural layout widgets
// (VBox, Stack, ...) don't need to opt in just to avoid breaking the tree.
// `root` itself always produces a node (even if its own access_role() is
// None), since callers need a stable entry point to attach to their
// platform's accessibility root.
AccessNode build_access_tree(Widget* root);

} // namespace tk
