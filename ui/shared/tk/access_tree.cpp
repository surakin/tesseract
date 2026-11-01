#include "access_tree.h"

#include "list_view.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace tk
{

namespace
{

// Subtree nodes (access_subtree_for_row) map to no widget and no row of
// their own, but every bridge identifies a node by {widget, row_index}: give
// each one the owning list plus a synthetic negative index derived from its
// row, role, and name (with a counter for repeats), so siblings don't
// collapse onto one key and a node keeps its key when others are added or
// removed around it (a new link or reaction mustn't re-point an existing
// "Open image" key at a different control). Negative and <= -2, so it can't
// collide with a real row (>= 0) or a real widget node (-1); such nodes
// carry `activate` when actionable, which invoke_default_action consults
// before any row_index dispatch.
void key_subtree(std::vector<AccessNode>& nodes, Widget* owner, int row,
                 std::unordered_map<std::string, int>& seen)
{
    for (auto& n : nodes)
    {
        std::string id = !n.subtree_id.empty()
                             ? n.subtree_id
                             : std::to_string(static_cast<int>(n.role)) + '\x1f' + n.name;
        const int occurrence = seen[id]++;
        std::size_t h = std::hash<std::string>()(id);
        h ^= std::hash<int>()(row) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(occurrence) + 0x9e3779b9u + (h << 6) + (h >> 2);
        n.widget = owner;
        n.row_index = -2 - static_cast<int>(h & 0x3fffffff);
        n.row_set_size = -1;
        n.pos_in_set = -1;
        key_subtree(n.children, owner, row, seen);
    }
}

// Number the nodes one collector appended to `out` from `first` on: their
// position and set size count only the rows actually exposed.
void number_set(std::vector<AccessNode>& out, std::size_t first)
{
    const int n = static_cast<int>(out.size() - first);
    for (int k = 0; k < n; ++k)
    {
        out[first + static_cast<std::size_t>(k)].pos_in_set = k + 1;
        out[first + static_cast<std::size_t>(k)].row_set_size = n;
    }
}

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
    {
        return;
    }

    const std::size_t n = list->adapter()->count();
    const std::size_t first = out.size();
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
        {
            continue;
        }

        AccessNode node;
        node.widget = list;
        node.role = accessible->access_role_for_row(i);
        node.name = accessible->access_name_for_row(i);
        node.state = accessible->access_state_for_row(i);
        node.description = accessible->access_description_for_row(i);
        node.row_index = static_cast<int>(i);
        node.rect = list->row_world_rect(static_cast<int>(i));
        // Optional per-row subtree (reactions / actions / receipts on a
        // virtualized message row). Each returned node carries its own
        // `activate` closure — see AccessNode / invoke_default_action.
        node.children = accessible->access_subtree_for_row(i);
        std::unordered_map<std::string, int> seen;
        key_subtree(node.children, list, static_cast<int>(i), seen);
        out.push_back(std::move(node));
    }
    number_set(out, first);
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
    {
        return;
    }

    const std::size_t n = grid->adapter()->count();
    // Cells flow left-to-right in index order, so the 2-D position falls
    // straight out of the current column count.
    const int cols = std::max(1, grid->column_count());
    const std::size_t first = out.size();
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (accessible->access_role_for_cell(i) == Role::None)
        {
            continue;
        }

        AccessNode node;
        node.widget = grid;
        node.role = accessible->access_role_for_cell(i);
        node.name = accessible->access_name_for_cell(i);
        node.state = accessible->access_state_for_cell(i);
        node.description = accessible->access_description_for_cell(i);
        node.row_index = static_cast<int>(i);
        node.grid_row = static_cast<int>(i) / cols;
        node.grid_col = static_cast<int>(i) % cols;
        node.rect = grid->rect_at(static_cast<int>(i));
        out.push_back(std::move(node));
    }
    number_set(out, first);
}

// Same situation as collect_list_rows/collect_grid_cells above, but for a
// widget that implements WidgetRowAccessibility directly on itself rather
// than through a separate adapter object — e.g. ListPopupBase, which
// paints its rows via ScrollableBase's own hit-test/scroll/paint loop, not
// a tk::ListView.
void collect_widget_rows(Widget* w, WidgetRowAccessibility* rows, std::vector<AccessNode>& out)
{
    const std::size_t n = rows->access_row_count();
    const std::size_t first = out.size();
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (rows->access_role_for_widget_row(i) == Role::None)
        {
            continue;
        }

        AccessNode node;
        node.widget = w;
        node.role = rows->access_role_for_widget_row(i);
        node.name = rows->access_name_for_widget_row(i);
        node.state = rows->access_state_for_widget_row(i);
        node.row_index = static_cast<int>(i);
        node.rect = rows->access_rect_for_widget_row(i);
        node.description = rows->access_description_for_widget_row(i);
        const auto [gr, gc] = rows->access_grid_cell_for_widget_row(i);
        node.grid_row = gr;
        node.grid_col = gc;
        out.push_back(std::move(node));
    }
    number_set(out, first);
}

void append_child_node(Widget* ch, std::vector<AccessNode>& out);

// The node for a real widget, minus its children.
AccessNode widget_node(Widget* w)
{
    AccessNode node;
    node.widget = w;
    node.role = w->access_role();
    node.name = w->access_name();
    node.description = w->access_description();
    node.language = w->access_language();
    node.state = w->access_state();
    node.value = w->access_value();
    node.modal = w->access_modal();
    node.rect = w->bounds();
    if (auto* grid = dynamic_cast<GridView*>(w))
    {
        if (grid->adapter())
        {
            const int cols = std::max(1, grid->column_count());
            const int n = static_cast<int>(grid->adapter()->count());
            node.grid_col_count = cols;
            node.grid_row_count = (n + cols - 1) / cols;
        }
    }
    else if (auto* rows = dynamic_cast<WidgetRowAccessibility*>(w))
    {
        const auto [r, c] = rows->access_grid_size();
        node.grid_row_count = r;
        node.grid_col_count = c;
    }
    return node;
}

// The topmost visible widget reporting access_modal(), or nullptr. Later
// siblings paint over earlier ones (and detached popups over everything),
// so search in reverse paint order: when two dialogs are open — e.g. a
// verification request opening the encryption dialog under an open
// forward picker — the one the user sees on top wins.
Widget* find_modal(Widget* w)
{
    if (!w->visible())
    {
        return nullptr;
    }
    std::vector<Widget*> detached;
    w->access_detached_children(detached);
    for (auto it = detached.rbegin(); it != detached.rend(); ++it)
    {
        if (*it)
        {
            if (Widget* m = find_modal(*it))
            {
                return m;
            }
        }
    }
    const auto& kids = w->children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it)
    {
        if (Widget* m = find_modal(it->get()))
        {
            return m;
        }
    }
    if (w->access_modal())
    {
        return w;
    }
    return nullptr;
}

// Appends AccessNodes for w's accessible descendants into `out`. w itself
// never gets a node here — the caller (build_access_tree, or a recursive
// call below for an accessible widget) already decided whether w gets one.
void collect_access_children(Widget* w, std::vector<AccessNode>& out)
{
    if (!w->visible())
    {
        return;
    }

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
    auto* rows = dynamic_cast<WidgetRowAccessibility*>(w);
    if (rows && !rows->access_rows_after_children())
    {
        collect_widget_rows(w, rows, out);
        // No early return: unlike ListView/GridView (whose row/cell model IS
        // the whole content), a WidgetRowAccessibility widget can ALSO have
        // real child widgets — e.g. SideTabView, a self-painted tab strip
        // plus the active tab's content panel. Fall through so those are
        // walked too. The synthesized rows are appended first (reading order:
        // the strip precedes its panel in every current case).
    }

    std::vector<Widget*> kids;
    kids.reserve(w->children().size());
    for (auto& ch : w->children())
    {
        kids.push_back(ch.get());
    }
    // Same reading-order sort as collect_focus_order in widget.cpp, reused
    // via reading_order_less so the two traversals can't silently diverge.
    std::stable_sort(kids.begin(), kids.end(),
                     [](Widget* a, Widget* b)
                     {
                         return reading_order_less(a->bounds(), b->bounds());
                     });

    for (Widget* ch : kids)
    {
        append_child_node(ch, out);
    }
    if (rows && rows->access_rows_after_children())
    {
        collect_widget_rows(w, rows, out);
    }

    // Owned-but-not-parented popups (see Widget::access_detached_children)
    // read after the real children: they float above them visually and are
    // opened from them.
    std::vector<Widget*> detached;
    w->access_detached_children(detached);
    for (Widget* d : detached)
    {
        if (d)
        {
            append_child_node(d, out);
        }
    }
}

void append_child_node(Widget* ch, std::vector<AccessNode>& out)
{
    if (!ch->visible())
    {
        return;
    }
    if (ch->access_role() != Role::None)
    {
        AccessNode node = widget_node(ch);
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

// Fills each node's unset language from its parent, so a widget declaring a
// language covers its synthesized rows/cells and subtree nodes too.
void inherit_language(AccessNode& node, const std::string& inherited)
{
    if (node.language.empty())
    {
        node.language = inherited;
    }
    for (auto& child : node.children)
    {
        inherit_language(child, node.language);
    }
}

} // namespace

AccessNode build_access_tree(Widget* root)
{
    AccessNode top;
    if (!root || !root->visible())
    {
        return top;
    }

    top = widget_node(root);
    // While a modal is open, everything behind it is inert for pointer and
    // keyboard already; expose only the modal so an AT can't wander there
    // either.
    Widget* modal = root->access_modal() ? nullptr : find_topmost_modal(root);
    if (modal)
    {
        append_child_node(modal, top.children);
    }
    else
    {
        collect_access_children(root, top.children);
    }
    inherit_language(top, {});
    return top;
}

bool set_node_value(const AccessNode& node, double value)
{
    if (!node.widget || node.row_index != -1)
    {
        return false;
    }
    return node.widget->access_set_value(value);
}

bool invoke_default_action(const AccessNode& node)
{
    // A subtree node (reaction toggle, row action button, …) carries its own
    // closure — it maps to no widget and no plain row index.
    if (node.activate)
    {
        return node.activate();
    }

    if (!node.widget)
    {
        return false;
    }

    // Real widget node (not a synthesized row/cell) — dispatch directly.
    if (node.row_index == -1)
    {
        return node.widget->access_default_action();
    }
    // A subtree node with no closure (a group, a text node — see
    // key_subtree): nothing to invoke.
    if (node.row_index < -1)
    {
        return false;
    }

    // Synthesized node: node.widget is the owning ListView/GridView (or,
    // for WidgetRowAccessibility, the widget itself), never a per-item
    // Widget — mirrors collect_list_rows/collect_grid_cells/
    // collect_widget_rows' identical dispatch-by-type above.
    const auto index = static_cast<std::size_t>(node.row_index);
    if (auto* list = dynamic_cast<ListView*>(node.widget))
    {
        if (auto* accessible = dynamic_cast<ListAdapterAccessibility*>(list->adapter()))
        {
            return accessible->access_activate_row(index);
        }
        return false;
    }
    if (auto* grid = dynamic_cast<GridView*>(node.widget))
    {
        if (auto* accessible = dynamic_cast<GridAdapterAccessibility*>(grid->adapter()))
        {
            return accessible->access_activate_cell(index);
        }
        return false;
    }
    if (auto* rows = dynamic_cast<WidgetRowAccessibility*>(node.widget))
    {
        return rows->access_activate_widget_row(index);
    }
    return false;
}

Widget* find_topmost_modal(Widget* root)
{
    if (!root)
    {
        return nullptr;
    }
    Widget* m = find_modal(root);
    return m == root ? nullptr : m;
}

} // namespace tk
