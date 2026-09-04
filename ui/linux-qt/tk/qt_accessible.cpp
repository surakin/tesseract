#include "qt_accessible.h"
#include "host_qt.h"
#include "tk/access_tree.h"
#include "tk/list_view.h"

#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtWidgets/QAccessibleWidget>
#include <QtWidgets/QApplication>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace tk::qt6
{

namespace
{

// Identifies one AccessNode stably across tree rebuilds: the tk::Widget it
// came from, plus a row/cell index for a synthesized node (-1 for a node
// backed directly by a real Widget — see AccessNode's own doc comment).
// Both fields together are exactly what AccessNode already carries, so this
// is just "the part of an AccessNode that identifies its position" —
// pointer identity survives a relayout as long as the underlying tk::Widget
// (or owning ListView/GridView) isn't itself destroyed, which is the same
// assumption next_focusable()'s Tab order already relies on.
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

QAccessible::Role to_qaccessible_role(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:      return QAccessible::Button;
    case tk::Role::CheckBox:    return QAccessible::CheckBox;
    case tk::Role::Switch:
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
        return QAccessible::Switch;
#else
        return QAccessible::CheckBox;
#endif
    case tk::Role::RadioButton: return QAccessible::RadioButton;
    case tk::Role::ComboBox:    return QAccessible::ComboBox;
    case tk::Role::TextInput:   return QAccessible::EditableText;
    case tk::Role::StaticText:  return QAccessible::StaticText;
    case tk::Role::Image:       return QAccessible::Graphic;
    case tk::Role::Link:        return QAccessible::Link;
    case tk::Role::List:        return QAccessible::List;
    case tk::Role::ListItem:    return QAccessible::ListItem;
    case tk::Role::Grid:        return QAccessible::Table;
    case tk::Role::GridCell:    return QAccessible::Cell;
    case tk::Role::Tab:         return QAccessible::PageTab;
    case tk::Role::TabList:     return QAccessible::PageTabList;
    case tk::Role::TabPanel:    return QAccessible::PropertyPage;
    case tk::Role::Dialog:      return QAccessible::Dialog;
    case tk::Role::MenuItem:    return QAccessible::MenuItem;
    case tk::Role::Group:       return QAccessible::Grouping;
    case tk::Role::None:        return QAccessible::Client;
    }
    return QAccessible::Client;
}

// Roles a screen reader would plausibly invoke a "click" action on.
// Approximate (a ListItem/GridCell that genuinely has no action — e.g. a
// MessageListView row — still advertises pressAction here), but harmless:
// tk::invoke_default_action() on a node with no real action is already a
// documented no-op (mirrors Button::access_default_action() on a disabled
// button). Being conservative here would need a "can activate" dry-run
// query that doesn't exist yet across all three activation mechanisms —
// not worth adding just to avoid an occasional no-op action being offered.
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

class NodeAccessible;

// Per-Surface accessibility state: the cached AccessNode tree, an index
// from AccessKey to the live node, parent lookups, and the registered
// QAccessibleInterface id for every AccessKey ever queried (reused across
// rebuilds so a node's QAccessibleInterface* stays the same instance for as
// long as the node itself keeps existing — required for Qt/AT-SPI to treat
// repeated navigation to "the same" row/cell as the same object).
//
// Rebuilding is lazy (only on the next query after a relayout marks this
// dirty), not eager on every relayout — MessageListView/RoomListView's
// virtualized rows mean a full rebuild walks every row eagerly (see
// build_access_tree's own doc comment on that), and relayouts can fire many
// times/sec during scrolling or typing. This means a structural
// ObjectReorder notification only actually reaches Qt on the next query
// after a change, not the instant the change happens — acceptable for a
// first working bridge; revisit if real screen-reader testing shows this
// staleness actually matters in practice.
class AccessBridge
{
public:
    explicit AccessBridge(Surface* surface) : surface_(surface) {}

    Surface* surface() const
    {
        return surface_;
    }

    void mark_dirty()
    {
        dirty_ = true;
    }

    const AccessNode* root_node()
    {
        rebuild_if_dirty();
        return tree_.widget ? &tree_ : nullptr;
    }

    const AccessNode* find(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : it->second;
    }

    // Returns the (possibly newly created) interface for `key`, or nullptr
    // if `key` doesn't currently resolve to a node. Reuses the previously
    // registered interface for the same key when one exists — required so
    // repeated navigation returns pointer-identical interfaces, not fresh
    // ones each time.
    QAccessibleInterface* interface_for(const AccessKey& key);

    // The parent interface for `key`: the owning NodeAccessible if `key`
    // has a tk-level parent, or the SurfaceAccessible root if `key` is the
    // top-level tk::Widget node (no tk-level parent above it).
    QAccessibleInterface* parent_interface_for(const AccessKey& key);

    QRect to_screen_rect(const tk::Rect& r) const
    {
        QPoint top_left =
            surface_->mapToGlobal(QPoint(static_cast<int>(r.x), static_cast<int>(r.y)));
        return QRect(top_left,
                     QSize(static_cast<int>(r.w), static_cast<int>(r.h)));
    }

private:
    void rebuild_if_dirty()
    {
        if (!dirty_)
            return;
        dirty_ = false;

        Widget* root = surface_->root();
        tree_ = root ? tk::build_access_tree(root) : AccessNode{};

        // Index against tree_ itself (not a temporary later moved into
        // it) — AccessNode's top-level object changes address across a
        // move (it isn't stored in a vector like its children are), so
        // indexing a temporary and then moving it would leave every
        // pointer to the root node dangling.
        std::unordered_map<AccessKey, const AccessNode*, AccessKeyHash> new_index;
        std::unordered_map<AccessKey, AccessKey, AccessKeyHash> new_parent;
        index_tree(tree_, new_index, new_parent);

        // Retire interfaces for keys that no longer resolve to a node
        // (row/cell removed, widget torn down) so a stale interface can't
        // keep being handed to an AT client.
        //
        // The actual QAccessible::deleteAccessibleInterface() is deferred to
        // the next event-loop turn rather than called here inline. Qt's own
        // AT-SPI/D-Bus bridge does not deliver updateAccessibility() events
        // synchronously — it posts them to itself and flushes a batch later
        // (visible in a crash trace as sendPostedEvents -> QObject::event ->
        // libQt6DBus -> operator<<(QDebug, QAccessibleInterface*)). A rapid
        // sequence of rebuilds (e.g. backspacing through a live-filtered
        // search field, one rebuild per keystroke) can retire and delete a
        // row's interface in the very next rebuild after a Focus event was
        // posted for it (via notify_current_row/notify_focus_changed), so
        // the bridge ends up dereferencing an already-freed interface when
        // it finally processes that posted event -> SIGSEGV. Deleting on a
        // singleShot(0, ...) instead lets any already-posted event for this
        // id flush first. `qApp` as the context object ties the callback's
        // lifetime to the application, so it's simply dropped rather than
        // fired into a torn-down Qt if the app is already shutting down.
        for (auto it = ids_.begin(); it != ids_.end();)
        {
            if (new_index.find(it->first) == new_index.end())
            {
                if (auto* iface = QAccessible::accessibleInterface(it->second))
                {
                    QAccessibleEvent ev(iface, QAccessible::ObjectHide);
                    QAccessible::updateAccessibility(&ev);
                }
                QAccessible::Id id = it->second;
                QTimer::singleShot(0, qApp, [id]()
                                    { QAccessible::deleteAccessibleInterface(id); });
                it = ids_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        index_ = std::move(new_index);
        parent_of_ = std::move(new_parent);

        QAccessibleEvent ev(surface_, QAccessible::ObjectReorder);
        QAccessible::updateAccessibility(&ev);
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

    // A ListView/GridView's Up/Down/Left/Right arrow-key navigation moves
    // selected_index_ with no click and no tk-level focus change (real Qt
    // keyboard focus stays on the ListView/GridView itself throughout —
    // see tk::ListView::focusable()'s own doc comment) — so without this,
    // AT-SPI has no way to know the "current row" changed at all. Wired
    // once per distinct ListView/GridView instance encountered while
    // walking the tree (tracked in selection_hooked_, since the same
    // instance reappears in every rebuild); fires QAccessible::Focus on
    // the newly current row/cell's own interface, matching how a real
    // QAbstractItemView reports its current item as an "active descendant"
    // without moving real keyboard focus off the view.
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
        if (idx < 0)
            return; // deselected — nothing to report as "current"
        if (auto* iface = interface_for(AccessKey{owner, idx}))
        {
            QAccessibleEvent ev(iface, QAccessible::Focus);
            QAccessible::updateAccessibility(&ev);
        }
    }

    Surface* surface_;
    bool dirty_ = true;
    AccessNode tree_;
    std::unordered_map<AccessKey, const AccessNode*, AccessKeyHash> index_;
    std::unordered_map<AccessKey, AccessKey, AccessKeyHash> parent_of_;
    std::unordered_map<AccessKey, QAccessible::Id, AccessKeyHash> ids_;
    std::unordered_set<Widget*> selection_hooked_;
};

// Non-QObject-backed accessible object for one AccessNode (mirrors Qt's own
// QAccessibleHyperlink pattern for "virtual" children that have no
// corresponding QObject) — every tk::Widget-tree node is in exactly this
// situation: a real object with real semantics, but not a QObject. Always
// re-resolves its current AccessNode from the bridge on every query rather
// than caching one, since the underlying tree can be rebuilt between
// queries (see AccessBridge's own doc comment) while this object's own
// identity (registered via QAccessible::registerAccessibleInterface) stays
// stable across that.
class NodeAccessible : public QAccessibleInterface, public QAccessibleActionInterface
{
public:
    NodeAccessible(AccessBridge* bridge, AccessKey key) : bridge_(bridge), key_(key) {}

    // ---- QAccessibleInterface ----
    bool isValid() const override
    {
        return node() != nullptr;
    }
    QObject* object() const override
    {
        return nullptr; // no QObject backs this node, like QAccessibleHyperlink
    }
    QWindow* window() const override
    {
        return bridge_->surface()->windowHandle();
    }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        const AccessNode* n = node();
        if (!n)
            return nullptr;
        for (const auto& child : n->children)
        {
            if (bridge_->to_screen_rect(child.rect).contains(x, y))
                return bridge_->interface_for(key_for(child));
        }
        return nullptr;
    }
    QAccessibleInterface* parent() const override
    {
        return bridge_->parent_interface_for(key_);
    }
    QAccessibleInterface* child(int index) const override
    {
        const AccessNode* n = node();
        if (!n || index < 0 || static_cast<std::size_t>(index) >= n->children.size())
            return nullptr;
        return bridge_->interface_for(key_for(n->children[static_cast<std::size_t>(index)]));
    }
    int childCount() const override
    {
        const AccessNode* n = node();
        return n ? static_cast<int>(n->children.size()) : 0;
    }
    int indexOfChild(const QAccessibleInterface* iface) const override
    {
        const AccessNode* n = node();
        if (!n)
            return -1;
        for (std::size_t i = 0; i < n->children.size(); ++i)
        {
            if (bridge_->interface_for(key_for(n->children[i])) == iface)
                return static_cast<int>(i);
        }
        return -1;
    }
    QString text(QAccessible::Text t) const override
    {
        const AccessNode* n = node();
        if (n && t == QAccessible::Name)
            return QString::fromStdString(n->name);
        return {};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override
    {
        const AccessNode* n = node();
        return n ? bridge_->to_screen_rect(n->rect) : QRect();
    }
    QAccessible::Role role() const override
    {
        const AccessNode* n = node();
        return n ? to_qaccessible_role(n->role) : QAccessible::NoRole;
    }
    QAccessible::State state() const override
    {
        QAccessible::State s{};
        const AccessNode* n = node();
        if (!n)
            return s;
        // QAccessibleWidget (the Surface's own top-level interface) already
        // derives "invisible" from the real QWidget's isVisible() — but
        // isVisible() reflects the whole ancestor chain (e.g. a
        // QStackedWidget page switch hiding the Surface without ever
        // touching any tk::Widget's own visible_ flag), so every node
        // *inside* the Surface needs the same check explicitly; nothing
        // else here has a way to know the Surface itself went invisible.
        if (!bridge_->surface()->isVisible())
            s.invisible = true;
        s.focusable = true;
        switch (n->role)
        {
        case tk::Role::CheckBox:
        case tk::Role::Switch:
        case tk::Role::RadioButton:
            s.checkable = true;
            break;
        case tk::Role::ListItem:
        case tk::Role::GridCell:
            s.selectable = true;
            break;
        default:
            break;
        }
        s.checked  = n->state.checked;
        s.expanded = n->state.expanded;
        s.selected = n->state.selected;
        s.busy     = n->state.busy;
        return s;
    }
    void* interface_cast(QAccessible::InterfaceType t) override
    {
        if (t == QAccessible::ActionInterface)
            return static_cast<QAccessibleActionInterface*>(this);
        return nullptr;
    }

    // ---- QAccessibleActionInterface ----
    QStringList actionNames() const override
    {
        const AccessNode* n = node();
        if (n && has_action_role(n->role))
            return {QAccessibleActionInterface::pressAction()};
        return {};
    }
    void doAction(const QString& actionName) override
    {
        if (actionName != QAccessibleActionInterface::pressAction())
            return;
        if (const AccessNode* n = node())
            tk::invoke_default_action(*n);
    }
    QStringList keyBindingsForAction(const QString&) const override
    {
        return {};
    }

private:
    const AccessNode* node() const
    {
        return bridge_->find(key_);
    }

    AccessBridge* bridge_;
    AccessKey key_;
};

QAccessibleInterface* AccessBridge::interface_for(const AccessKey& key)
{
    rebuild_if_dirty();
    if (index_.find(key) == index_.end())
        return nullptr;

    auto it = ids_.find(key);
    if (it != ids_.end())
        return QAccessible::accessibleInterface(it->second);

    auto* iface = new NodeAccessible(this, key);
    // QAccessibleCache owns the registered interface from here on — see its
    // ownership contract — so this class must not delete it; just remember
    // the id to reuse the same instance across repeated queries.
    QAccessible::Id id = QAccessible::registerAccessibleInterface(iface);
    ids_[key] = id;
    return iface;
}

QAccessibleInterface* AccessBridge::parent_interface_for(const AccessKey& key)
{
    rebuild_if_dirty();
    auto it = parent_of_.find(key);
    if (it == parent_of_.end())
        return QAccessible::queryAccessibleInterface(surface_);
    return interface_for(it->second);
}

// Root interface for the tk::qt6::Surface QWidget itself — a real,
// QObject-backed QAccessibleWidget so it merges into Qt's own accessible
// tree (and Qt's own object-to-interface cache) exactly like any other
// widget. Its one child is the root tk::Widget's AccessNode, via the
// Surface's AccessBridge.
class SurfaceAccessible : public QAccessibleWidget
{
public:
    SurfaceAccessible(Surface* surface, AccessBridge* bridge)
        : QAccessibleWidget(surface, QAccessible::Pane), bridge_(bridge)
    {
    }

    int childCount() const override
    {
        return bridge_->root_node() ? 1 : 0;
    }
    QAccessibleInterface* child(int index) const override
    {
        if (index != 0)
            return nullptr;
        const AccessNode* root = bridge_->root_node();
        return root ? bridge_->interface_for(key_for(*root)) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* iface) const override
    {
        const AccessNode* root = bridge_->root_node();
        if (root && bridge_->interface_for(key_for(*root)) == iface)
            return 0;
        return -1;
    }

private:
    AccessBridge* bridge_;
};

// One AccessBridge per Surface, created on first factory() query and kept
// alive (and reused) for the Surface's lifetime; erased when the Surface
// QObject is destroyed.
std::unordered_map<QObject*, std::unique_ptr<AccessBridge>>& bridge_registry()
{
    static std::unordered_map<QObject*, std::unique_ptr<AccessBridge>> registry;
    return registry;
}

AccessBridge* bridge_for(Surface* surface)
{
    auto& registry = bridge_registry();
    auto it = registry.find(surface);
    if (it != registry.end())
        return it->second.get();

    auto owned = std::make_unique<AccessBridge>(surface);
    AccessBridge* bridge = owned.get();
    registry.emplace(surface, std::move(owned));

    surface->add_layout_listener([bridge] { bridge->mark_dirty(); });
    QObject::connect(surface, &QObject::destroyed, [surface]
                     { bridge_registry().erase(surface); });

    return bridge;
}

QAccessibleInterface* factory(const QString&, QObject* object)
{
    // Surface has no Q_OBJECT (it never needed its own signals/slots), so
    // it has no metaObject()/className() of its own — Qt's factory
    // dispatch would call us with classname == "QWidget" (its nearest
    // Q_OBJECT ancestor) for every plain QWidget in the app, not just
    // Surface instances. dynamic_cast works regardless of Q_OBJECT (RTTI
    // only needs a polymorphic type, which QWidget already is), so use it
    // directly instead of trying to match on classname.
    if (auto* surface = dynamic_cast<Surface*>(object))
        return new SurfaceAccessible(surface, bridge_for(surface));
    return nullptr;
}

} // namespace

void install_accessible_factory()
{
    QAccessible::installFactory(factory);
}

void notify_focus_changed(Surface* surface, tk::Widget*, tk::Widget* now)
{
    if (!surface || !now)
        return;
    // Reusing AccessKey/interface_for from the anonymous namespace above is
    // fine here since this function is itself defined in this translation
    // unit, after that namespace closes — the names are still visible via
    // unqualified lookup within the enclosing tk::qt6 namespace (mirrors
    // win32_accessible.cpp's notify_focus_changed, same reasoning).
    AccessBridge* bridge = bridge_for(surface);
    if (auto* iface = bridge->interface_for(AccessKey{now, -1}))
    {
        QAccessibleEvent ev(iface, QAccessible::Focus);
        QAccessible::updateAccessibility(&ev);
    }
}

} // namespace tk::qt6
