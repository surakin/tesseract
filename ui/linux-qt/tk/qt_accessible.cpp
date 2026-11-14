#include "qt_accessible.h"
#include "host_qt.h"
#include "tk/access_tree.h"
#include "tk/list_view.h"

#include <QtCore/QLocale>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtWidgets/QAccessibleWidget>
#include <QtWidgets/QApplication>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        return std::hash<void*>()(k.widget) ^ (std::hash<int>()(k.row_index) << 1);
    }
};

AccessKey key_for(const AccessNode& n)
{
    return {n.widget, n.row_index};
}

// The parts of a node an AT caches and must be told about when they change
// (see AccessBridge::fire_change_events). Mirrors gtk_accessible.cpp's
// last-pushed diff.
struct Snapshot
{
    std::string name;
    std::string description;
    AccessState state;
    bool disabled = false;
    double value = 0.0;
};

// Real widget nodes report enabled() directly; synthesized ones carry
// state.disabled.
bool node_disabled(const AccessNode& n)
{
    if (n.state.disabled)
    {
        return true;
    }
    return n.row_index == -1 && n.widget && !n.activate && !n.widget->enabled();
}

Snapshot snapshot_of(const AccessNode& n)
{
    return {n.name, n.description, n.state, node_disabled(n), n.value.now};
}

// Debounce for the AT-active refresh (see AccessBridge::schedule_refresh):
// long enough that a 60 Hz animation doesn't rebuild the tree every frame,
// short enough that a toggled button's new name is spoken promptly.
constexpr int kRefreshDelayMs = 200;

QAccessible::Role to_qaccessible_role(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:
        return QAccessible::Button;
    case tk::Role::CheckBox:
        return QAccessible::CheckBox;
    case tk::Role::Switch:
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
        return QAccessible::Switch;
#else
        return QAccessible::CheckBox;
#endif
    case tk::Role::RadioButton:
        return QAccessible::RadioButton;
    case tk::Role::ComboBox:
        return QAccessible::ComboBox;
    case tk::Role::TextInput:
        return QAccessible::EditableText;
    case tk::Role::StaticText:
        return QAccessible::StaticText;
    case tk::Role::Image:
        return QAccessible::Graphic;
    case tk::Role::Link:
        return QAccessible::Link;
    case tk::Role::List:
        return QAccessible::List;
    case tk::Role::ListItem:
        return QAccessible::ListItem;
    case tk::Role::Grid:
        return QAccessible::Table;
    case tk::Role::GridCell:
        return QAccessible::Cell;
    case tk::Role::Tab:
        return QAccessible::PageTab;
    case tk::Role::TabList:
        return QAccessible::PageTabList;
    case tk::Role::TabPanel:
        return QAccessible::PropertyPage;
    case tk::Role::Dialog:
        return QAccessible::Dialog;
    case tk::Role::MenuItem:
        return QAccessible::MenuItem;
    case tk::Role::Group:
        return QAccessible::Grouping;
    case tk::Role::ProgressBar:
        return QAccessible::ProgressBar;
    case tk::Role::Slider:
        return QAccessible::Slider;
    case tk::Role::None:
        return QAccessible::Client;
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
    explicit AccessBridge(Surface* surface) : surface_(surface)
    {
    }
    ~AccessBridge();

    Surface* surface() const
    {
        return surface_;
    }

    void mark_dirty()
    {
        dirty_ = true;
        schedule_refresh();
    }

    // While an AT client is attached (it has queried at least one node),
    // rebuild shortly after any relayout or repaint so structure, name and
    // state changes are pushed as events instead of waiting for the AT's
    // next query. With no AT attached ids_ is empty and this is free.
    void schedule_refresh()
    {
        if (ids_.empty() || refresh_pending_ || !QAccessible::isActive())
        {
            return;
        }
        refresh_pending_ = true;
        // `surface_` as context: dropped if the Surface (and with it this
        // bridge, see bridge_for) is destroyed first.
        QTimer::singleShot(kRefreshDelayMs, surface_,
                           [this]
                           {
                               refresh_pending_ = false;
                               dirty_ = true;
                               rebuild_if_dirty();
                           });
    }

    void announce(const std::string& text, bool assertive)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QAccessibleAnnouncementEvent ev(surface_, QString::fromStdString(text));
        ev.setPoliteness(assertive ? QAccessible::AnnouncementPoliteness::Assertive
                                   : QAccessible::AnnouncementPoliteness::Polite);
        QAccessible::updateAccessibility(&ev);
#else
        (void)text;
        (void)assertive;
#endif
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
        return QRect(top_left, QSize(static_cast<int>(r.w), static_cast<int>(r.h)));
    }

private:
    void rebuild_if_dirty()
    {
        if (!dirty_)
        {
            return;
        }
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
            if (!new_index.contains(it->first))
            {
                if (auto* iface = QAccessible::accessibleInterface(it->second))
                {
                    QAccessibleEvent ev(iface, QAccessible::ObjectHide);
                    QAccessible::updateAccessibility(&ev);
                }
                QAccessible::Id id = it->second;
                QTimer::singleShot(0, qApp,
                                   [id]()
                                   {
                                       QAccessible::deleteAccessibleInterface(id);
                                   });
                it = ids_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        bool structure_changed = new_index.size() != index_.size();
        if (!structure_changed)
        {
            for (const auto& [key, node] : new_index)
            {
                if (!index_.contains(key))
                {
                    structure_changed = true;
                    break;
                }
            }
        }

        index_ = std::move(new_index);
        parent_of_ = std::move(new_parent);

        if (structure_changed)
        {
            QAccessibleEvent ev(surface_, QAccessible::ObjectReorder);
            QAccessible::updateAccessibility(&ev);
        }
        fire_change_events();
    }

    // Name/description/state/value events for every node an AT has seen
    // (has a registered interface) whose cached values changed since the
    // last rebuild.
    void fire_change_events()
    {
        for (auto it = snapshots_.begin(); it != snapshots_.end();)
        {
            auto node_it = index_.find(it->first);
            auto id_it = ids_.find(it->first);
            if (node_it == index_.end() || id_it == ids_.end())
            {
                it = snapshots_.erase(it);
                continue;
            }
            QAccessibleInterface* iface = QAccessible::accessibleInterface(id_it->second);
            Snapshot now = snapshot_of(*node_it->second);
            Snapshot& was = it->second;
            if (iface)
            {
                if (now.name != was.name)
                {
                    QAccessibleEvent ev(iface, QAccessible::NameChanged);
                    QAccessible::updateAccessibility(&ev);
                }
                if (now.description != was.description)
                {
                    QAccessibleEvent ev(iface, QAccessible::DescriptionChanged);
                    QAccessible::updateAccessibility(&ev);
                }
                QAccessible::State changed{};
                changed.checked = now.state.checked != was.state.checked;
                changed.expanded = now.state.expanded != was.state.expanded;
                changed.collapsed = changed.expanded;
                changed.selected = now.state.selected != was.state.selected;
                changed.busy = now.state.busy != was.state.busy;
                changed.disabled = now.disabled != was.disabled;
                if (changed.checked || changed.expanded || changed.selected || changed.busy ||
                    changed.disabled)
                {
                    QAccessibleStateChangeEvent ev(iface, changed);
                    QAccessible::updateAccessibility(&ev);
                }
                if (node_it->second->value.present && now.value != was.value)
                {
                    QAccessibleValueChangeEvent ev(iface, QVariant(now.value));
                    QAccessible::updateAccessibility(&ev);
                }
            }
            was = std::move(now);
            ++it;
        }
    }

    void index_tree(AccessNode& node,
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
        if (!widget || selection_hooked_.contains(widget))
        {
            return;
        }
        selection_hooked_.insert(widget);

        if (auto* list = dynamic_cast<tk::ListView*>(widget))
        {
            list->on_selection_changed = [this, list](int idx)
            {
                notify_current_row(list, idx);
            };
        }
        else if (auto* grid = dynamic_cast<tk::GridView*>(widget))
        {
            grid->on_selection_changed = [this, grid](int idx)
            {
                notify_current_row(grid, idx);
            };
        }
    }

    void notify_current_row(Widget* owner, int idx)
    {
        if (idx < 0)
        {
            return; // deselected — nothing to report as "current"
        }
        if (auto* iface = interface_for(AccessKey{owner, idx}))
        {
            QAccessibleEvent ev(iface, QAccessible::Focus);
            QAccessible::updateAccessibility(&ev);
        }
    }

    Surface* surface_;
    bool dirty_ = true;
    bool refresh_pending_ = false;
    AccessNode tree_;
    std::unordered_map<AccessKey, const AccessNode*, AccessKeyHash> index_;
    std::unordered_map<AccessKey, AccessKey, AccessKeyHash> parent_of_;
    std::unordered_map<AccessKey, QAccessible::Id, AccessKeyHash> ids_;
    std::unordered_set<Widget*> selection_hooked_;
    // Last values pushed to the AT, per node it has an interface for.
    std::unordered_map<AccessKey, Snapshot, AccessKeyHash> snapshots_;

public:
    void remember(const AccessKey& key)
    {
        auto it = index_.find(key);
        if (it != index_.end())
        {
            snapshots_[key] = snapshot_of(*it->second);
        }
    }
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
class NodeAccessible : public QAccessibleInterface,
                       public QAccessibleActionInterface,
                       public QAccessibleValueInterface,
                       public QAccessibleTableInterface,
                       public QAccessibleTableCellInterface
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    ,
                       public QAccessibleAttributesInterface
#endif
{
public:
    NodeAccessible(AccessBridge* bridge, AccessKey key) : bridge_(bridge), key_(key)
    {
    }

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
        if (!bridge_)
        {
            return nullptr;
        }
        return bridge_->surface()->windowHandle();
    }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        const AccessNode* n = node();
        if (!n)
        {
            return nullptr;
        }
        for (const auto& child : n->children)
        {
            if (bridge_->to_screen_rect(child.rect).contains(x, y))
            {
                return bridge_->interface_for(key_for(child));
            }
        }
        return nullptr;
    }
    QAccessibleInterface* parent() const override
    {
        if (!bridge_)
        {
            return nullptr;
        }
        return bridge_->parent_interface_for(key_);
    }
    QAccessibleInterface* child(int index) const override
    {
        const AccessNode* n = node();
        if (!n || index < 0 || static_cast<std::size_t>(index) >= n->children.size())
        {
            return nullptr;
        }
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
        {
            return -1;
        }
        for (std::size_t i = 0; i < n->children.size(); ++i)
        {
            if (bridge_->interface_for(key_for(n->children[i])) == iface)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    QString text(QAccessible::Text t) const override
    {
        const AccessNode* n = node();
        if (n && t == QAccessible::Name)
        {
            return QString::fromStdString(n->name);
        }
        if (n && t == QAccessible::Description)
        {
            return QString::fromStdString(n->description);
        }
        return {};
    }
    void setText(QAccessible::Text, const QString&) override
    {
    }
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
        {
            return s;
        }
        // QAccessibleWidget (the Surface's own top-level interface) already
        // derives "invisible" from the real QWidget's isVisible() — but
        // isVisible() reflects the whole ancestor chain (e.g. a
        // QStackedWidget page switch hiding the Surface without ever
        // touching any tk::Widget's own visible_ flag), so every node
        // *inside* the Surface needs the same check explicitly; nothing
        // else here has a way to know the Surface itself went invisible.
        if (!bridge_->surface()->isVisible())
        {
            s.invisible = true;
        }
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
        s.checked = n->state.checked;
        s.expanded = n->state.expanded;
        s.collapsed = (n->role == tk::Role::ComboBox) && !n->state.expanded;
        s.selected = n->state.selected;
        s.busy = n->state.busy;
        s.modal = n->modal;
        s.disabled = node_disabled(*n);
        if (n->row_index == -1 && n->widget && !n->activate)
        {
            s.focused = n->widget->has_focus();
        }
        return s;
    }
    void* interface_cast(QAccessible::InterfaceType t) override
    {
        const AccessNode* n = node();
        if (t == QAccessible::ActionInterface)
        {
            return static_cast<QAccessibleActionInterface*>(this);
        }
        if (t == QAccessible::ValueInterface && n && n->value.present)
        {
            return static_cast<QAccessibleValueInterface*>(this);
        }
        if (t == QAccessible::TableInterface && n && n->grid_col_count > 0)
        {
            return static_cast<QAccessibleTableInterface*>(this);
        }
        if (t == QAccessible::TableCellInterface && n && n->grid_row >= 0)
        {
            return static_cast<QAccessibleTableCellInterface*>(this);
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (t == QAccessible::AttributesInterface && n && !n->language.empty())
        {
            return static_cast<QAccessibleAttributesInterface*>(this);
        }
#endif
        return nullptr;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // ---- QAccessibleAttributesInterface (content language; Qt 6.8+) ----
    QList<QAccessible::Attribute> attributeKeys() const override
    {
        return {QAccessible::Attribute::Locale};
    }
    QVariant attributeValue(QAccessible::Attribute key) const override
    {
        const AccessNode* n = node();
        if (key == QAccessible::Attribute::Locale && n && !n->language.empty())
        {
            return QVariant::fromValue(QLocale(QString::fromStdString(n->language)));
        }
        return {};
    }
#endif

    // ---- QAccessibleValueInterface (Role::ProgressBar, Role::Slider) ----
    QVariant currentValue() const override
    {
        const AccessNode* n = node();
        return n ? QVariant(n->value.now) : QVariant();
    }
    void setCurrentValue(const QVariant& v) override
    {
        const AccessNode* n = node();
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (n && ok)
        {
            tk::set_node_value(*n, d);
        }
    }
    QVariant maximumValue() const override
    {
        const AccessNode* n = node();
        return n ? QVariant(n->value.max) : QVariant();
    }
    QVariant minimumValue() const override
    {
        const AccessNode* n = node();
        return n ? QVariant(n->value.min) : QVariant();
    }
    QVariant minimumStepSize() const override
    {
        return {};
    }

    // ---- QAccessibleTableInterface (grid container) ----
    QAccessibleInterface* caption() const override
    {
        return nullptr;
    }
    QAccessibleInterface* summary() const override
    {
        return nullptr;
    }
    QAccessibleInterface* cellAt(int row, int column) const override
    {
        const AccessNode* n = node();
        if (!n)
        {
            return nullptr;
        }
        for (const auto& child : n->children)
        {
            if (child.grid_row == row && child.grid_col == column)
            {
                return bridge_->interface_for(key_for(child));
            }
        }
        return nullptr;
    }
    int selectedCellCount() const override
    {
        return static_cast<int>(selectedCells().size());
    }
    QList<QAccessibleInterface*> selectedCells() const override
    {
        QList<QAccessibleInterface*> out;
        if (const AccessNode* n = node())
        {
            for (const auto& child : n->children)
            {
                if (child.grid_row >= 0 && child.state.selected)
                {
                    if (auto* iface = bridge_->interface_for(key_for(child)))
                    {
                        out.push_back(iface);
                    }
                }
            }
        }
        return out;
    }
    QString columnDescription(int) const override
    {
        return {};
    }
    QString rowDescription(int) const override
    {
        return {};
    }
    int selectedColumnCount() const override
    {
        return 0;
    }
    int selectedRowCount() const override
    {
        return 0;
    }
    int columnCount() const override
    {
        const AccessNode* n = node();
        return n ? n->grid_col_count : 0;
    }
    int rowCount() const override
    {
        const AccessNode* n = node();
        return n ? n->grid_row_count : 0;
    }
    QList<int> selectedColumns() const override
    {
        return {};
    }
    QList<int> selectedRows() const override
    {
        return {};
    }
    bool isColumnSelected(int) const override
    {
        return false;
    }
    bool isRowSelected(int) const override
    {
        return false;
    }
    bool selectRow(int) override
    {
        return false;
    }
    bool selectColumn(int) override
    {
        return false;
    }
    bool unselectRow(int) override
    {
        return false;
    }
    bool unselectColumn(int) override
    {
        return false;
    }
    void modelChange(QAccessibleTableModelChangeEvent*) override
    {
    }

    // ---- QAccessibleTableCellInterface (grid cell) ----
    bool isSelected() const override
    {
        const AccessNode* n = node();
        return n && n->state.selected;
    }
    QList<QAccessibleInterface*> columnHeaderCells() const override
    {
        return {};
    }
    QList<QAccessibleInterface*> rowHeaderCells() const override
    {
        return {};
    }
    int columnIndex() const override
    {
        const AccessNode* n = node();
        return n ? n->grid_col : -1;
    }
    int rowIndex() const override
    {
        const AccessNode* n = node();
        return n ? n->grid_row : -1;
    }
    int columnExtent() const override
    {
        return 1;
    }
    int rowExtent() const override
    {
        return 1;
    }
    QAccessibleInterface* table() const override
    {
        return parent();
    }

    // ---- QAccessibleActionInterface ----
    QStringList actionNames() const override
    {
        const AccessNode* n = node();
        if (n && has_action_role(n->role))
        {
            return {QAccessibleActionInterface::pressAction()};
        }
        return {};
    }
    void doAction(const QString& actionName) override
    {
        if (actionName != QAccessibleActionInterface::pressAction())
        {
            return;
        }
        if (const AccessNode* n = node())
        {
            tk::invoke_default_action(*n);
        }
    }
    QStringList keyBindingsForAction(const QString&) const override
    {
        return {};
    }

    // Called when the bridge is destroyed while Qt still holds this interface.
    // Every query then answers "nothing" rather than reaching freed memory.
    void detach()
    {
        bridge_ = nullptr;
    }

private:
    const AccessNode* node() const
    {
        if (!bridge_)
        {
            return nullptr;
        }
        return bridge_->find(key_);
    }

    AccessBridge* bridge_;
    AccessKey key_;
};

// Interfaces are registered with Qt, so they outlive the bridge; an AT that
// still holds an object path reaches them. Detach them now, so any call from
// here on answers "nothing", and retire them on the next event-loop turn, after
// posted accessibility events for them have been delivered (see
// rebuild_if_dirty for why deletion is deferred).
AccessBridge::~AccessBridge()
{
    std::vector<QAccessible::Id> ids;
    ids.reserve(ids_.size());
    for (const auto& entry : ids_)
    {
        ids.push_back(entry.second);
        if (auto* node =
                dynamic_cast<NodeAccessible*>(QAccessible::accessibleInterface(entry.second)))
        {
            node->detach();
        }
    }
    ids_.clear();
    QTimer::singleShot(0, qApp,
                       [ids]()
                       {
                           for (QAccessible::Id id : ids)
                           {
                               QAccessible::deleteAccessibleInterface(id);
                           }
                       });
}

QAccessibleInterface* AccessBridge::interface_for(const AccessKey& key)
{
    rebuild_if_dirty();
    if (!index_.contains(key))
    {
        return nullptr;
    }

    auto it = ids_.find(key);
    if (it != ids_.end())
    {
        return QAccessible::accessibleInterface(it->second);
    }

    auto* iface = new NodeAccessible(this, key);
    // QAccessibleCache owns the registered interface from here on — see its
    // ownership contract — so this class must not delete it; just remember
    // the id to reuse the same instance across repeated queries.
    QAccessible::Id id = QAccessible::registerAccessibleInterface(iface);
    ids_[key] = id;
    remember(key);
    return iface;
}

QAccessibleInterface* AccessBridge::parent_interface_for(const AccessKey& key)
{
    rebuild_if_dirty();
    auto it = parent_of_.find(key);
    if (it == parent_of_.end())
    {
        return QAccessible::queryAccessibleInterface(surface_);
    }
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
        {
            return nullptr;
        }
        const AccessNode* root = bridge_->root_node();
        return root ? bridge_->interface_for(key_for(*root)) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* iface) const override
    {
        const AccessNode* root = bridge_->root_node();
        if (root && bridge_->interface_for(key_for(*root)) == iface)
        {
            return 0;
        }
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
    {
        return it->second.get();
    }

    auto owned = std::make_unique<AccessBridge>(surface);
    AccessBridge* bridge = owned.get();
    registry.emplace(surface, std::move(owned));

    surface->add_layout_listener(
        [bridge]
        {
            bridge->mark_dirty();
        });
    surface->host().add_paint_listener(
        [bridge]
        {
            bridge->schedule_refresh();
        });
    QObject::connect(surface, &QObject::destroyed,
                     [surface]
                     {
                         bridge_registry().erase(surface);
                     });

    return bridge;
}

// Live Surfaces, keyed by their QObject address. GUI thread only.
std::unordered_set<const QObject*>& live_surfaces()
{
    static std::unordered_set<const QObject*> surfaces;
    return surfaces;
}

QAccessibleInterface* factory(const QString&, QObject* object)
{
    // Surface has no Q_OBJECT (it never needed its own signals/slots), so
    // Qt's factory dispatch calls us with classname == "QWidget" for every
    // plain QWidget in the app, not just Surface instances. Identify ours by
    // address instead of dynamic_cast: Qt queries objects while they are
    // still being constructed or destroyed, where RTTI on them can crash.
    if (!object || !live_surfaces().contains(object))
    {
        return nullptr;
    }
    auto* surface = static_cast<Surface*>(static_cast<QWidget*>(object));
    return new SurfaceAccessible(surface, bridge_for(surface));
}

} // namespace

void install_accessible_factory()
{
    QAccessible::installFactory(factory);
}

void register_accessible_surface(Surface* surface)
{
    live_surfaces().insert(static_cast<QWidget*>(surface));
}

void unregister_accessible_surface(Surface* surface)
{
    live_surfaces().erase(static_cast<QWidget*>(surface));
}

void announce(Surface* surface, const std::string& text, bool assertive)
{
    if (surface)
    {
        bridge_for(surface)->announce(text, assertive);
    }
}

void notify_focus_changed(Surface* surface, tk::Widget*, tk::Widget* now)
{
    if (!surface || !now)
    {
        return;
    }
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
