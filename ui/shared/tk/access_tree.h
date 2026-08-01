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

    // World-surface (root-widget-coordinate) bounds — a platform bridge maps
    // this through its own window→screen transform for QAccessibleInterface::
    // rect()/UIA's BoundingRectangle/AT-SPI's Component interface. For a real
    // Widget node this is just widget->bounds() (already world-space — see
    // Widget::world_to_local's doc comment); for a synthesized row/cell it
    // comes from the owning ListView/GridView/WidgetRowAccessibility's own
    // world-rect helper, since there's no Widget of its own to ask.
    Rect rect;

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
    // Invoke row `index`'s default action — see
    // ListAdapterAccessibility::access_activate_row's identical rationale
    // (list_view.h). Default: no action available.
    virtual bool access_activate_widget_row(std::size_t /*index*/)
    {
        return false;
    }
    // World-space rect of row `index`, mirroring
    // ListView::row_world_rect()/GridView::rect_at() for a widget that has
    // neither underneath it. Default: empty rect (a bridge should treat
    // this as "unknown," not "zero-size at the origin").
    virtual Rect access_rect_for_widget_row(std::size_t /*index*/) const
    {
        return {};
    }
};

// Invokes `node`'s default action — the single entry point a platform
// bridge calls when an AT client activates a node, regardless of whether
// it's a real Widget (Widget::access_default_action()) or a synthesized
// list row / grid cell / widget row (dispatched through the owning
// ListView's/GridView's adapter, or the widget itself for
// WidgetRowAccessibility). Centralized here rather than duplicated per
// platform bridge, so Qt6/GTK4/future bridges can't drift on how a
// synthesized node's action is resolved. Returns false (no-op) if `node`
// has no widget, or the relevant optional interface isn't implemented.
bool invoke_default_action(const AccessNode& node);

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
