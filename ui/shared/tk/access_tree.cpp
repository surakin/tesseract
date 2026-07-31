#include "access_tree.h"

#include "list_view.h"

#include <algorithm>

namespace tk
{

namespace
{

// A ListView's rows have no per-row Widget (see ListAdapter's own top
// comment in list_view.h) — paint_row() draws directly from adapter data.
// When the adapter also implements ListAdapterAccessibility, synthesize
// one ListItem-shaped AccessNode per row directly from it instead of the
// normal Widget-child walk, which would find nothing. Eagerly covers every
// row (0..count()) for correctness first; trimming this to just the
// currently-realized/visible range (to avoid touching every row of a huge
// list on each tree rebuild) is deferred until a real platform consumer
// exists to design the partial-update path against — see the plan doc's
// Phase 1 note on virtualized lists.
void collect_list_rows(ListView* list, std::vector<AccessNode>& out)
{
    auto* accessible = dynamic_cast<ListAdapterAccessibility*>(list->adapter());
    if (!accessible)
        return;

    const std::size_t n = list->adapter()->count();
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
    {
        // Role::None means "excluded from the tree" everywhere else in
        // this file (a suppressed row — a header spacer, a day separator
        // with nothing after it — matching paint_row's own suppression
        // logic). List rows have no children to flatten through, unlike a
        // Role::None Widget elsewhere in the tree, so skip entirely rather
        // than push an empty node.
        if (accessible->access_role_for_row(i) == Role::None)
            continue;

        AccessNode node;
        node.widget        = list;
        node.role          = accessible->access_role_for_row(i);
        node.name          = accessible->access_name_for_row(i);
        node.state         = accessible->access_state_for_row(i);
        node.row_index     = static_cast<int>(i);
        node.row_set_size  = static_cast<int>(n);
        out.push_back(std::move(node));
    }
}

// GridView cells have no per-cell tk::Widget either (see GridAdapter's own
// top comment in list_view.h) — same situation and same fix as
// collect_list_rows above, via GridAdapterAccessibility instead. Reuses
// AccessNode's row_index/row_set_size fields for linear position-in-set
// (a flat "item N of M", not yet a full 2-D row/column table model — see
// the plan doc's note on EmojiPicker/StickerPicker being the harder case;
// this ships the achievable slice now).
void collect_grid_cells(GridView* grid, std::vector<AccessNode>& out)
{
    auto* accessible = dynamic_cast<GridAdapterAccessibility*>(grid->adapter());
    if (!accessible)
        return;

    const std::size_t n = grid->adapter()->count();
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (accessible->access_role_for_cell(i) == Role::None)
            continue;

        AccessNode node;
        node.widget        = grid;
        node.role          = accessible->access_role_for_cell(i);
        node.name          = accessible->access_name_for_cell(i);
        node.state         = accessible->access_state_for_cell(i);
        node.row_index     = static_cast<int>(i);
        node.row_set_size  = static_cast<int>(n);
        out.push_back(std::move(node));
    }
}

// Same situation as collect_list_rows/collect_grid_cells above, but for a
// widget that implements WidgetRowAccessibility directly on itself rather
// than through a separate adapter object — e.g. ListPopupBase, which
// paints its rows via ScrollableBase's own hit-test/scroll/paint loop, not
// a tk::ListView.
void collect_widget_rows(Widget* w, WidgetRowAccessibility* rows,
                         std::vector<AccessNode>& out)
{
    const std::size_t n = rows->access_row_count();
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (rows->access_role_for_widget_row(i) == Role::None)
            continue;

        AccessNode node;
        node.widget        = w;
        node.role          = rows->access_role_for_widget_row(i);
        node.name          = rows->access_name_for_widget_row(i);
        node.state         = rows->access_state_for_widget_row(i);
        node.row_index     = static_cast<int>(i);
        node.row_set_size  = static_cast<int>(n);
        out.push_back(std::move(node));
    }
}

// Appends AccessNodes for w's accessible descendants into `out`. w itself
// never gets a node here — the caller (build_access_tree, or a recursive
// call below for an accessible widget) already decided whether w gets one.
void collect_access_children(Widget* w, std::vector<AccessNode>& out)
{
    if (!w->visible())
        return;

    if (auto* list = dynamic_cast<ListView*>(w))
    {
        collect_list_rows(list, out);
        return;
    }
    if (auto* grid = dynamic_cast<GridView*>(w))
    {
        collect_grid_cells(grid, out);
        return;
    }
    if (auto* rows = dynamic_cast<WidgetRowAccessibility*>(w))
    {
        collect_widget_rows(w, rows, out);
        return;
    }

    std::vector<Widget*> kids;
    kids.reserve(w->children().size());
    for (auto& ch : w->children())
        kids.push_back(ch.get());
    // Same reading-order sort as collect_focus_order in widget.cpp, reused
    // via reading_order_less so the two traversals can't silently diverge.
    std::stable_sort(kids.begin(), kids.end(),
                     [](Widget* a, Widget* b)
                     { return reading_order_less(a->bounds(), b->bounds()); });

    for (Widget* ch : kids)
    {
        if (!ch->visible())
            continue;
        if (ch->access_role() != Role::None)
        {
            AccessNode node;
            node.widget = ch;
            node.role   = ch->access_role();
            node.name   = ch->access_name();
            node.state  = ch->access_state();
            collect_access_children(ch, node.children);
            out.push_back(std::move(node));
        }
        else
        {
            // Flatten through: ch contributes no node of its own, so its
            // accessible descendants attach directly to `out` instead.
            collect_access_children(ch, out);
        }
    }
}

} // namespace

AccessNode build_access_tree(Widget* root)
{
    AccessNode top;
    if (!root || !root->visible())
        return top;

    top.widget = root;
    top.role   = root->access_role();
    top.name   = root->access_name();
    top.state  = root->access_state();
    collect_access_children(root, top.children);
    return top;
}

} // namespace tk
