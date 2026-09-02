#include "gtk_accessible.h"
#include "host_gtk.h"
#include "tk/access_tree.h"
#include "tk/list_view.h"

#include <gtk/gtk.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tk::gtk4
{

namespace
{

// Identifies one AccessNode stably across tree rebuilds — see
// qt_accessible.cpp's identical AccessKey for the full rationale (both
// bridges share the same tk::Widget*/row_index identity scheme, since it's
// exactly what AccessNode itself already carries).
struct AccessKey
{
    tk::Widget* widget = nullptr;
    int row_index = -1;

    bool operator==(const AccessKey& o) const
    {
        return widget == o.widget && row_index == o.row_index;
    }
};

struct AccessKeyHash
{
    std::size_t operator()(const AccessKey& k) const
    {
        return std::hash<void*>()(k.widget) ^
              (std::hash<int>()(k.row_index) << 1);
    }
};

AccessKey key_for(const AccessNode& n)
{
    return {n.widget, n.row_index};
}

GtkAccessibleRole to_gtk_role(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:      return GTK_ACCESSIBLE_ROLE_BUTTON;
    case tk::Role::CheckBox:    return GTK_ACCESSIBLE_ROLE_CHECKBOX;
    case tk::Role::Switch:      return GTK_ACCESSIBLE_ROLE_SWITCH;
    case tk::Role::RadioButton: return GTK_ACCESSIBLE_ROLE_RADIO;
    case tk::Role::ComboBox:    return GTK_ACCESSIBLE_ROLE_COMBO_BOX;
    case tk::Role::TextInput:   return GTK_ACCESSIBLE_ROLE_TEXT_BOX;
    case tk::Role::StaticText:  return GTK_ACCESSIBLE_ROLE_LABEL;
    case tk::Role::Image:       return GTK_ACCESSIBLE_ROLE_IMG;
    case tk::Role::Link:        return GTK_ACCESSIBLE_ROLE_LINK;
    case tk::Role::List:        return GTK_ACCESSIBLE_ROLE_LIST;
    case tk::Role::ListItem:    return GTK_ACCESSIBLE_ROLE_LIST_ITEM;
    case tk::Role::Grid:        return GTK_ACCESSIBLE_ROLE_GRID;
    case tk::Role::GridCell:    return GTK_ACCESSIBLE_ROLE_GRID_CELL;
    case tk::Role::Tab:         return GTK_ACCESSIBLE_ROLE_TAB;
    case tk::Role::TabList:     return GTK_ACCESSIBLE_ROLE_TAB_LIST;
    case tk::Role::TabPanel:    return GTK_ACCESSIBLE_ROLE_TAB_PANEL;
    case tk::Role::Dialog:      return GTK_ACCESSIBLE_ROLE_DIALOG;
    case tk::Role::MenuItem:    return GTK_ACCESSIBLE_ROLE_MENU_ITEM;
    case tk::Role::Group:       return GTK_ACCESSIBLE_ROLE_GROUP;
    case tk::Role::None:        return GTK_ACCESSIBLE_ROLE_GENERIC;
    }
    return GTK_ACCESSIBLE_ROLE_GENERIC;
}

// Roles a screen reader would plausibly invoke a "click" action on — see
// qt_accessible.cpp's identical has_action_role() for the full rationale
// (same conservative, role-based heuristic; a false positive is a harmless
// no-op via tk::invoke_default_action()). Kept in sync with that function
// by hand — small enough that a shared helper isn't worth the cross-
// platform-directory coupling.
bool has_action_role(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:
    case tk::Role::CheckBox:
    case tk::Role::Switch:
    case tk::Role::RadioButton:
    case tk::Role::ComboBox:
    case tk::Role::ListItem:
    case tk::Role::GridCell:
    case tk::Role::MenuItem:
    case tk::Role::Tab:
        return true;
    default:
        return false;
    }
}

class AccessBridge;

// Per-node state for a TkAccessNode instance. Heap-allocated (via `new`,
// freed in tk_access_node_finalize) rather than embedded directly in the
// GObject instance struct: GObject allocates instances via a plain
// zero-filled block (no C++ constructors run), so a non-trivial C++ member
// (std::string below) directly inside a GObject struct would never get
// properly constructed — a well-known GObject/C++ interop gotcha. Only a
// raw `NodeData*` pointer lives in the actual instance struct.
struct NodeData
{
    AccessBridge* bridge = nullptr; // borrowed — outlives every node it owns
    tk::Widget* key_widget = nullptr;
    int key_row_index = -1;
    // True only for the one per-Surface marker created directly by
    // attach_accessible_bridge() (not for any AccessNode-backed node) —
    // see its accessible-parent/first-child special-casing below.
    bool is_root_marker = false;

    // Last-pushed cache, so a rebuild that finds nothing actually changed
    // for this node doesn't re-fire AT-SPI property/state-changed signals
    // (mirrors the Qt bridge's identical "only notify on real change"
    // discipline, and access_tree.cpp's own "not virtualized, but still
    // avoid spam" precedent).
    bool pushed_once = false;
    std::string last_name;
    bool last_checked = false;
    bool last_expanded = false;
    bool last_selected = false;
    bool last_busy = false;
};

// ─────────────────────────────────────────────────────────────────────────
//  TkAccessNode — one real (zero-size, GTK-visible-but-invisually-inert)
//  GtkWidget per AccessNode, flat-parented under the one per-Surface root
//  marker (see attach_accessible_bridge). Real, because GTK4 requires it:
//  - GtkWidget's own default GtkAccessibleInterface vfuncs for
//    get_accessible_parent/get_first_accessible_child/
//    get_next_accessible_sibling just walk the *real* widget tree
//    unconditionally (confirmed against GTK's own gtkwidget.c) — but we
//    override all three anyway, since our *logical* AccessNode tree
//    (already flattened/synthesized by build_access_tree()) doesn't match
//    real widget-tree structure. Flat-parenting everything one level deep
//    under a single already-proven-working marker (rather than mirroring
//    the logical tree's real depth via nested gtk_widget_set_parent calls)
//    sidesteps any risk around unallocated/unrealized nested real widgets
//    — the *only* thing real parenting is needed for here is (a) being
//    discoverable/realized at all, and (b) the point below.
//  - GTK4's built-in AT-SPI Action interface is registered only for a
//    genuine GTK_IS_WIDGET (gtk_atspi_get_action_vtable() returns NULL for
//    anything else — confirmed against gtk/a11y/gtkatspiaction.c) and
//    dispatches via the widget's own action muxer — hence the
//    class-installed "activate" action below, per-instance enabled only
//    for has_action_role() roles (a disabled action is filtered out of
//    GetActions/GetNActions by GTK's own is_valid_action(), so this has
//    the same effect as never installing it, without needing two GTypes).
// ─────────────────────────────────────────────────────────────────────────

#define TK_TYPE_ACCESS_NODE (tk_access_node_get_type())
G_DECLARE_FINAL_TYPE(TkAccessNode, tk_access_node, TK, ACCESS_NODE, GtkWidget)

struct _TkAccessNode
{
    GtkWidget parent_instance;
    NodeData* data;
};

void node_accessible_init(GtkAccessibleInterface* iface);

G_DEFINE_TYPE_WITH_CODE(TkAccessNode, tk_access_node, GTK_TYPE_WIDGET,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_ACCESSIBLE, node_accessible_init))

NodeData* node_data(GtkAccessible* accessible)
{
    return TK_ACCESS_NODE(accessible)->data;
}

void tk_access_node_init(TkAccessNode* self)
{
    self->data = new NodeData();
}

void tk_access_node_finalize(GObject* object)
{
    TkAccessNode* self = TK_ACCESS_NODE(object);
    delete self->data;
    self->data = nullptr;
    G_OBJECT_CLASS(tk_access_node_parent_class)->finalize(object);
}

// Zero size unconditionally: every TkAccessNode exists only to carry an
// accessible identity, never to be painted or affect layout (mirrors the
// original spike's TkAccessRoot::root_measure).
void node_measure(GtkWidget*, GtkOrientation, int, int* minimum, int* natural,
                  int* minimum_baseline, int* natural_baseline)
{
    *minimum = *natural = 0;
    *minimum_baseline = *natural_baseline = -1;
}

// Defined after AccessBridge's full definition, below — needs to call into
// it. Forward-declared here so class_init (which needs a real function
// pointer, not just a later definition) can reference it.
void node_activate_action(GtkWidget* widget, const char* action_name, GVariant* parameter);

void tk_access_node_class_init(TkAccessNodeClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = tk_access_node_finalize;
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = node_measure;
    gtk_widget_class_install_action(widget_class, "activate", nullptr,
                                    node_activate_action);
}

gboolean node_get_platform_state(GtkAccessible*, GtkAccessiblePlatformState state)
{
    return state == GTK_ACCESSIBLE_PLATFORM_STATE_FOCUSABLE ? TRUE : FALSE;
}

// Defined after AccessBridge, below.
GtkAccessible* node_get_accessible_parent(GtkAccessible* self);
GtkAccessible* node_get_first_accessible_child(GtkAccessible* self);
GtkAccessible* node_get_next_accessible_sibling(GtkAccessible* self);

// get_at_context / get_bounds / get_accessible_id are deliberately left
// untouched here: GObject interface inheritance means this iface struct
// starts as a *copy* of GtkWidget's own (already-registered) vtable before
// this function runs, so any vfunc we don't explicitly overwrite keeps
// GtkWidget's real default — exactly what we want for those three (a real
// per-instance "accessible-role" property is already provided by
// GtkWidget itself; see attach_accessible_bridge's construction call).
void node_accessible_init(GtkAccessibleInterface* iface)
{
    iface->get_platform_state          = node_get_platform_state;
    iface->get_accessible_parent       = node_get_accessible_parent;
    iface->get_first_accessible_child  = node_get_first_accessible_child;
    iface->get_next_accessible_sibling = node_get_next_accessible_sibling;
}

// ─────────────────────────────────────────────────────────────────────────
//  AccessBridge — one per Surface, owns the one root-marker TkAccessNode
//  and every content TkAccessNode (keyed by AccessKey), rebuilding the
//  cached AccessNode tree lazily on the next query after a relayout marks
//  it dirty (same tradeoff as the Qt bridge — see its own doc comment).
// ─────────────────────────────────────────────────────────────────────────

class AccessBridge
{
public:
    AccessBridge(Surface& surface, GtkWidget* root_marker)
        : surface_(surface), root_marker_(root_marker)
    {
    }

    void mark_dirty()
    {
        dirty_ = true;
    }

    GtkAccessible* root_child()
    {
        rebuild_if_dirty();
        if (!tree_.widget)
            return nullptr;
        auto it = nodes_.find(key_for(tree_));
        return it != nodes_.end() ? GTK_ACCESSIBLE(g_object_ref(it->second)) : nullptr;
    }

    GtkAccessible* accessible_parent_for(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto it = parent_of_.find(key);
        if (it == parent_of_.end())
        {
            // No logical parent recorded: `key` is the top-level
            // AccessNode (build_access_tree()'s own root, which always
            // produces a node — see its doc comment) — its accessible
            // parent is our root marker.
            return GTK_ACCESSIBLE(g_object_ref(root_marker_));
        }
        auto w = nodes_.find(it->second);
        return w != nodes_.end() ? GTK_ACCESSIBLE(g_object_ref(w->second)) : nullptr;
    }

    GtkAccessible* first_child_for(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto it = index_.find(key);
        if (it == index_.end() || it->second->children.empty())
            return nullptr;
        auto w = nodes_.find(key_for(it->second->children.front()));
        return w != nodes_.end() ? GTK_ACCESSIBLE(g_object_ref(w->second)) : nullptr;
    }

    GtkAccessible* next_sibling_for(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto pit = parent_of_.find(key);
        if (pit == parent_of_.end())
            return nullptr; // top-level node: no siblings, there's only one root
        auto parent_it = index_.find(pit->second);
        if (parent_it == index_.end())
            return nullptr;
        const auto& siblings = parent_it->second->children;
        for (std::size_t i = 0; i < siblings.size(); ++i)
        {
            if (!(key_for(siblings[i]) == key))
                continue;
            if (i + 1 >= siblings.size())
                return nullptr;
            auto w = nodes_.find(key_for(siblings[i + 1]));
            return w != nodes_.end() ? GTK_ACCESSIBLE(g_object_ref(w->second)) : nullptr;
        }
        return nullptr;
    }

    void activate(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto it = index_.find(key);
        if (it != index_.end())
            tk::invoke_default_action(*it->second);
    }

    // Generalizes notify_current_row()'s active-descendant technique (below)
    // to the whole surface's root marker rather than a specific list — for
    // ordinary tk-level Tab-focus moves (see the free function
    // tk::gtk4::notify_focus_changed, called from Host::on_focus_changed_).
    // Unlike notify_current_row (only ever invoked from a hook installed
    // during a tree walk, so its target is guaranteed already indexed), this
    // can fire before any rebuild has ever indexed `now` — force one first.
    void notify_focus_changed(Widget* now)
    {
        rebuild_if_dirty();
        if (!now)
        {
            gtk_accessible_reset_relation(GTK_ACCESSIBLE(root_marker_),
                                          GTK_ACCESSIBLE_RELATION_ACTIVE_DESCENDANT);
            return;
        }
        auto it = nodes_.find(AccessKey{now, -1});
        if (it == nodes_.end())
            return;
        gtk_accessible_update_relation(GTK_ACCESSIBLE(root_marker_),
                                       GTK_ACCESSIBLE_RELATION_ACTIVE_DESCENDANT,
                                       it->second, -1);
    }

private:
    void rebuild_if_dirty()
    {
        if (!dirty_)
            return;
        dirty_ = false;

        Widget* root = surface_.root();
        // Indexed against tree_ itself (not a temporary later moved into
        // it) — AccessNode's top-level object changes address across a
        // move (unlike its children, stored in a vector whose move just
        // swaps the buffer pointer), so indexing a temporary and then
        // moving it would leave every pointer to the root node dangling.
        tree_ = root ? tk::build_access_tree(root) : AccessNode{};

        std::unordered_map<AccessKey, const AccessNode*, AccessKeyHash> new_index;
        std::unordered_map<AccessKey, AccessKey, AccessKeyHash> new_parent;
        index_tree(tree_, new_index, new_parent);

        // Retire widgets for keys no longer present.
        for (auto it = nodes_.begin(); it != nodes_.end();)
        {
            if (new_index.find(it->first) == new_index.end())
            {
                gtk_widget_unparent(it->second);
                it = nodes_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        index_ = std::move(new_index);
        parent_of_ = std::move(new_parent);

        for (auto& [key, node_ptr] : index_)
            ensure_node_widget(key, *node_ptr);
    }

    void index_tree(
        AccessNode& node,
        std::unordered_map<AccessKey, const AccessNode*, AccessKeyHash>& index,
        std::unordered_map<AccessKey, AccessKey, AccessKeyHash>& parent)
    {
        AccessKey key = key_for(node);
        index[key] = &node;
        hook_selection_changed(node.widget);
        for (auto& child : node.children)
        {
            parent[key_for(child)] = key;
            index_tree(child, index, parent);
        }
    }

    // Real GTK-parents `widget` flatly under root_marker_ (creating it on
    // first sight) and pushes fresh role/name/state — see this file's top
    // comment for why flat parenting (not mirroring the logical tree's
    // real depth) is deliberate.
    void ensure_node_widget(const AccessKey& key, const AccessNode& node)
    {
        auto it = nodes_.find(key);
        GtkWidget* widget;
        NodeData* data;
        if (it != nodes_.end())
        {
            widget = it->second;
            data = node_data(GTK_ACCESSIBLE(widget));
        }
        else
        {
            // The accessible-role property is GtkWidget's own construct-
            // time-only storage (cannot be changed post-construction — see
            // gtkwidget.c's create_at_context()), so it must be passed to
            // g_object_new() directly rather than set afterward.
            widget = GTK_WIDGET(g_object_new(TK_TYPE_ACCESS_NODE, "accessible-role",
                                             to_gtk_role(node.role), nullptr));
            data = node_data(GTK_ACCESSIBLE(widget));
            data->bridge = this;
            data->key_widget = key.widget;
            data->key_row_index = key.row_index;
            gtk_widget_set_parent(widget, root_marker_);
            gtk_widget_action_set_enabled(widget, "activate", has_action_role(node.role));
            nodes_[key] = widget;
        }
        push_if_changed(data, widget, node);
    }

    void push_if_changed(NodeData* data, GtkWidget* widget, const AccessNode& node)
    {
        bool name_changed = !data->pushed_once || data->last_name != node.name;
        bool state_changed = !data->pushed_once ||
            data->last_checked != node.state.checked ||
            data->last_expanded != node.state.expanded ||
            data->last_selected != node.state.selected ||
            data->last_busy != node.state.busy;

        if (name_changed)
        {
            gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
                                           GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           node.name.c_str(), -1);
            data->last_name = node.name;
        }
        if (state_changed)
        {
            gtk_accessible_update_state(
                GTK_ACCESSIBLE(widget),
                GTK_ACCESSIBLE_STATE_CHECKED,
                node.state.checked ? GTK_ACCESSIBLE_TRISTATE_TRUE
                                   : GTK_ACCESSIBLE_TRISTATE_FALSE,
                GTK_ACCESSIBLE_STATE_EXPANDED, static_cast<int>(node.state.expanded),
                GTK_ACCESSIBLE_STATE_SELECTED, static_cast<int>(node.state.selected),
                GTK_ACCESSIBLE_STATE_BUSY, static_cast<int>(node.state.busy),
                -1);
            data->last_checked  = node.state.checked;
            data->last_expanded = node.state.expanded;
            data->last_selected = node.state.selected;
            data->last_busy     = node.state.busy;
        }
        data->pushed_once = true;
    }

    // A ListView/GridView's arrow-key navigation moves selected_index_
    // with no click and no tk-level focus change (real Qt/GTK keyboard
    // focus stays on the ListView/GridView itself throughout). Wired once
    // per distinct instance encountered while walking the tree (tracked in
    // selection_hooked_, since the same instance reappears in every
    // rebuild) — mirrors qt_accessible.cpp's identical hook, but reports
    // via GTK4's own ARIA-style active-descendant relation rather than a
    // Focus event fired directly on the child, matching how GTK4 itself
    // models "current item without moving real focus".
    void hook_selection_changed(Widget* widget)
    {
        if (!widget || selection_hooked_.count(widget))
            return;
        selection_hooked_.insert(widget);

        if (auto* list = dynamic_cast<tk::ListView*>(widget))
        {
            list->on_selection_changed = [this, list](int idx)
            { notify_current_row(list, idx); };
        }
        else if (auto* grid = dynamic_cast<tk::GridView*>(widget))
        {
            grid->on_selection_changed = [this, grid](int idx)
            { notify_current_row(grid, idx); };
        }
    }

    void notify_current_row(Widget* owner, int idx)
    {
        auto owner_it = nodes_.find(AccessKey{owner, -1});
        if (owner_it == nodes_.end())
            return;
        if (idx < 0)
        {
            gtk_accessible_reset_relation(GTK_ACCESSIBLE(owner_it->second),
                                          GTK_ACCESSIBLE_RELATION_ACTIVE_DESCENDANT);
            return;
        }
        auto row_it = nodes_.find(AccessKey{owner, idx});
        if (row_it == nodes_.end())
            return;
        gtk_accessible_update_relation(GTK_ACCESSIBLE(owner_it->second),
                                       GTK_ACCESSIBLE_RELATION_ACTIVE_DESCENDANT,
                                       row_it->second, -1);
    }

    Surface& surface_;
    GtkWidget* root_marker_; // borrowed — owned by the real GtkOverlay it's added to
    bool dirty_ = true;
    AccessNode tree_;
    std::unordered_map<AccessKey, const AccessNode*, AccessKeyHash> index_;
    std::unordered_map<AccessKey, AccessKey, AccessKeyHash> parent_of_;
    std::unordered_map<AccessKey, GtkWidget*, AccessKeyHash> nodes_; // borrowed
    std::unordered_set<Widget*> selection_hooked_;
};

GtkAccessible* node_get_accessible_parent(GtkAccessible* self)
{
    NodeData* data = node_data(self);
    if (data->is_root_marker)
    {
        GtkWidget* real_parent = gtk_widget_get_parent(GTK_WIDGET(self));
        return real_parent ? GTK_ACCESSIBLE(g_object_ref(real_parent)) : nullptr;
    }
    if (!data->bridge)
        return nullptr;
    return data->bridge->accessible_parent_for({data->key_widget, data->key_row_index});
}

GtkAccessible* node_get_first_accessible_child(GtkAccessible* self)
{
    NodeData* data = node_data(self);
    if (!data->bridge)
        return nullptr;
    if (data->is_root_marker)
        return data->bridge->root_child();
    return data->bridge->first_child_for({data->key_widget, data->key_row_index});
}

GtkAccessible* node_get_next_accessible_sibling(GtkAccessible* self)
{
    NodeData* data = node_data(self);
    if (data->is_root_marker || !data->bridge)
        return nullptr; // only ever one root marker per Surface
    return data->bridge->next_sibling_for({data->key_widget, data->key_row_index});
}

void node_activate_action(GtkWidget* widget, const char*, GVariant*)
{
    NodeData* data = TK_ACCESS_NODE(widget)->data;
    if (data->bridge)
        data->bridge->activate({data->key_widget, data->key_row_index});
}

} // namespace

void attach_accessible_bridge(Surface& surface)
{
    GtkWidget* overlay = surface.widget();

    GtkWidget* root_marker = GTK_WIDGET(g_object_new(
        TK_TYPE_ACCESS_NODE, "accessible-role", GTK_ACCESSIBLE_ROLE_GROUP, nullptr));
    NodeData* root_data = node_data(GTK_ACCESSIBLE(root_marker));
    root_data->is_root_marker = true;

    auto* bridge = new AccessBridge(surface, root_marker);
    root_data->bridge = bridge;
    // Ties the bridge's lifetime to the root marker's own GObject lifetime
    // (destroyed when unparented with no other refs, i.e. when the
    // GtkOverlay itself is torn down as part of the Surface's own
    // destruction) — avoids needing a separate registry/destroy-signal
    // mechanism the way qt_accessible.cpp's QObject-keyed one does, since
    // tk::gtk4::Surface has no equivalent lifecycle signal of its own.
    g_object_set_data_full(G_OBJECT(root_marker), "tk-access-bridge", bridge,
                           [](gpointer p) { delete static_cast<AccessBridge*>(p); });
    // Second, non-owning stash of the same pointer directly on `overlay` —
    // the root marker above owns the real (destroy-notify'd) reference;
    // this is just so notify_focus_changed(GtkWidget* overlay, ...) can find
    // the bridge in O(1) given only the overlay Host already holds, with no
    // separate Surface*-keyed registry (mirrors qt_accessible.cpp's
    // bridge_registry, minus the registry — GTK ties lifetime to the root
    // marker's GObject instead, see the comment above).
    g_object_set_data(G_OBJECT(overlay), "tk-access-bridge", bridge);

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), root_marker);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(overlay), root_marker, FALSE);

    surface.add_layout_listener([bridge] { bridge->mark_dirty(); });
}

void notify_focus_changed(GtkWidget* overlay, tk::Widget* now)
{
    if (!overlay)
        return;
    auto* bridge = static_cast<AccessBridge*>(
        g_object_get_data(G_OBJECT(overlay), "tk-access-bridge"));
    if (bridge)
        bridge->notify_focus_changed(now);
}

} // namespace tk::gtk4
