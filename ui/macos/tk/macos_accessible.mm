#include "macos_accessible.h"
#include "host_macos.h"
#include "access_tree.h"
#include "list_view.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

// Forward declaration only — TKAccessElement's full @interface/@implementation
// live below, after AccessBridge's declaration (it needs AccessBridge* as an
// ivar type), but AccessBridge's own methods that construct/return a
// TKAccessElement* are deferred until after TKAccessElement's complete
// definition — mirrors win32_accessible.cpp's identical AccessNodeProvider
// forward-declare-then-defer structure, for the same reason (a container
// type needing the complete type of the thing it stores/constructs).
@class TKAccessElement;

namespace tk::macos
{

namespace
{

NSAccessibilityRole to_ns_role(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:
        return NSAccessibilityButtonRole;
    case tk::Role::CheckBox:
        return NSAccessibilityCheckBoxRole;
    case tk::Role::Switch:
        return NSAccessibilityCheckBoxRole;
    case tk::Role::RadioButton:
        return NSAccessibilityRadioButtonRole;
    case tk::Role::ComboBox:
        return NSAccessibilityComboBoxRole;
    case tk::Role::TextInput:
        return NSAccessibilityTextFieldRole;
    case tk::Role::StaticText:
        return NSAccessibilityStaticTextRole;
    case tk::Role::Image:
        return NSAccessibilityImageRole;
    case tk::Role::Link:
        return NSAccessibilityLinkRole;
    case tk::Role::List:
        return NSAccessibilityListRole;
    // Apple's own guidance for custom (non-NSTableView) list content maps
    // each item to the row role, not a distinct "list item" role — matches
    // this exact "custom-drawn list" scenario even though it doesn't read
    // as a 1:1 name match with the other three platforms' role constants.
    case tk::Role::ListItem:
        return NSAccessibilityRowRole;
    case tk::Role::Grid:
        return NSAccessibilityGridRole;
    case tk::Role::GridCell:
        return NSAccessibilityCellRole;
    // Individual tabs are NSAccessibilityRadioButtonRole per AppKit's own
    // tab-group convention (also exposed via the tab group's special
    // -accessibilityTabs array — see access_tabs()/-accessibilityTabs
    // below), not a role of their own.
    case tk::Role::Tab:
        return NSAccessibilityRadioButtonRole;
    case tk::Role::TabList:
        return NSAccessibilityTabGroupRole;
    case tk::Role::TabPanel:
        return NSAccessibilityGroupRole;
    case tk::Role::Dialog:
        return NSAccessibilityGroupRole; // + subrole, see to_ns_subrole
    case tk::Role::MenuItem:
        return NSAccessibilityMenuItemRole;
    case tk::Role::Group:
        return NSAccessibilityGroupRole;
    case tk::Role::ProgressBar:
        return NSAccessibilityProgressIndicatorRole;
    case tk::Role::Slider:
        return NSAccessibilitySliderRole;
    case tk::Role::None:
        return NSAccessibilityGroupRole;
    }
    return NSAccessibilityGroupRole;
}

NSString* to_ns_subrole(tk::Role r)
{
    switch (r)
    {
    // NSAccessibilitySwitchSubrole would be the precise match for Switch,
    // deliberately left unused here: it's a less-certain-to-exist constant
    // than the ones used elsewhere in this file, and a wrong/nonexistent
    // symbol is a hard build failure for the whole target — not worth the
    // risk for a role that already reads correctly as a plain checkbox
    // without it. Revisit once this can actually be compiled and checked.
    case tk::Role::Dialog:
        return NSAccessibilityDialogSubrole;
    default:
        return nil;
    }
}

// Roles a screen reader would plausibly invoke a "press" action on. Ported
// verbatim from qt_accessible.cpp's identical has_action_role predicate —
// see its own comment for the rationale.
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

// Identifies one AccessNode stably across tree rebuilds — identical in
// shape and rationale to qt_accessible.cpp's/win32_accessible.cpp's
// AccessKey: the tk::Widget a node came from, plus a row/cell index for a
// synthesized node (-1 for a node backed directly by a real Widget).
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

AccessKey key_for(const tk::AccessNode& n)
{
    return {n.widget, n.row_index};
}

// Real widget nodes report enabled() directly; synthesized ones carry
// state.disabled (see AccessState::disabled).
bool node_disabled(const tk::AccessNode& n)
{
    if (n.state.disabled)
    {
        return true;
    }
    return n.row_index == -1 && n.widget && !n.activate && !n.widget->enabled();
}

// Last values posted to VoiceOver for an element — see AccessBridge::
// post_change_notifications (mirrors qt_accessible.cpp's Snapshot).
struct Snapshot
{
    std::string name;
    tk::AccessState state;
    bool disabled = false;
    double value = 0.0;
};

Snapshot snapshot_of(const tk::AccessNode& n)
{
    return {n.name, n.state, node_disabled(n), n.value.now};
}

// Debounce for the AT-active refresh — see qt_accessible.cpp's
// kRefreshDelayMs.
constexpr int64_t kRefreshDelayMs = 200;

// Depth-first hit test against world-space AccessNode rects (already in the
// same coordinate space as `local` — see AccessNode::rect's own doc
// comment), returning the deepest matching descendant or `node` itself if
// none of its children claim the point.
const tk::AccessNode* hit_test_node(const tk::AccessNode& node, tk::Point local)
{
    for (const auto& child : node.children)
    {
        if (child.rect.x <= local.x && local.x <= child.rect.x + child.rect.w &&
            child.rect.y <= local.y && local.y <= child.rect.y + child.rect.h)
        {
            return hit_test_node(child, local);
        }
    }
    return &node;
}

// Per-Surface accessibility state — same shape and rationale as
// win32_accessible.cpp's AccessBridge (see its doc comment): a cached
// AccessNode tree, an index from AccessKey to the live node, parent
// lookups, and a cached element per AccessKey ever queried, reused across
// rebuilds so a node's TKAccessElement identity stays stable for as long
// as the node itself keeps existing.
//
// Unlike Windows' hand-rolled COM refcounting, TKAccessElement lifetime is
// entirely ARC-managed — VoiceOver holding a strong reference just keeps
// the Objective-C object alive normally, no manual AddRef/Release needed.
// The detach() pattern still applies for safety: an element surviving past
// its node's retirement (or the whole bridge tearing down) must not
// dereference a destroyed AccessBridge/Surface.
class AccessBridge
{
public:
    explicit AccessBridge(Surface* surface) : surface_(surface)
    {
    }

    Surface* surface() const
    {
        return surface_;
    }
    NSView* view() const
    {
        return surface_ ? (__bridge NSView*)surface_->view_handle() : nil;
    }

    void mark_dirty()
    {
        dirty_ = true;
        schedule_refresh();
    }

    // While VoiceOver holds elements (has queried us), rebuild shortly after
    // any relayout/repaint so title/value changes are posted. Defined
    // out-of-class (looks the bridge up again by view when it fires — it
    // may have been detached in between).
    void schedule_refresh();

    void announce(const std::string& text, bool assertive);

    const tk::AccessNode* root_node()
    {
        rebuild_if_dirty();
        return tree_.widget ? &tree_ : nullptr;
    }

    const tk::AccessNode* find(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : it->second;
    }

    bool is_root(const AccessKey& key)
    {
        rebuild_if_dirty();
        return tree_.widget && key_for(tree_) == key;
    }

    NSRect to_screen_rect(const tk::Rect& r) const
    {
        NSView* v = view();
        if (!v || !v.window)
        {
            return NSZeroRect;
        }
        NSRect local = NSMakeRect(r.x, r.y, r.w, r.h);
        NSRect win_rect = [v convertRect:local toView:nil];
        return [v.window convertRectToScreen:win_rect];
    }

    // Index/count of the last row/cell reported "current" for `owner` (a
    // ListView/GridView) via arrow-key navigation — see
    // hook_selection_changed()/notify_current_row() below. -1 if none.
    int current_row_for(tk::Widget* owner) const
    {
        auto it = current_row_.find(owner);
        return it == current_row_.end() ? -1 : it->second;
    }

    // Returns the (possibly newly created) element for `key`, or nil if
    // `key` doesn't currently resolve to a node. Defined out-of-class,
    // after TKAccessElement's complete definition (constructs one).
    TKAccessElement* element_for(const AccessKey& key);

    // The parent element for `key`, or nil if `key` is the root (the root
    // has no tk-level parent — callers of -accessibilityParent special-case
    // that by returning the surface's own NSView instead, since the view
    // plays the "fragment root" role here, not a separate wrapper element).
    TKAccessElement* parent_element_for(const AccessKey& key);

    // Array of child elements for `key`'s node, or an empty array.
    NSArray* children_for(const AccessKey& key);

    // Disconnects and drops every cached element — see
    // win32_accessible.h-equivalent detach_accessible_bridge doc comment in
    // macos_accessible.h for when/why this runs.
    void detach();

private:
    void rebuild_if_dirty();

    void index_tree(tk::AccessNode& node,
                    std::unordered_map<AccessKey, const tk::AccessNode*, AccessKeyHash>& index,
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

    // A ListView/GridView's arrow-key navigation moves selected_index_ with
    // no tk-level focus change (real focus stays on the ListView/GridView
    // itself) — mirrors qt_accessible.cpp's/win32_accessible.cpp's
    // identical hook_selection_changed, wired once per distinct instance
    // encountered while walking the tree.
    void hook_selection_changed(tk::Widget* widget)
    {
        if (!widget || selection_hooked_.count(widget))
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

    // Defined out-of-class (constructs/posts against a live element, which
    // needs TKAccessElement's complete type).
    void notify_current_row(tk::Widget* owner, int idx);

    void post_change_notifications();

public:
    void remember(const AccessKey& key)
    {
        auto it = index_.find(key);
        if (it != index_.end())
        {
            snapshots_[key] = snapshot_of(*it->second);
        }
    }

private:
    Surface* surface_;
    bool dirty_ = true;
    bool refresh_pending_ = false;
    std::unordered_map<AccessKey, Snapshot, AccessKeyHash> snapshots_;
    tk::AccessNode tree_;
    std::unordered_map<AccessKey, const tk::AccessNode*, AccessKeyHash> index_;
    std::unordered_map<AccessKey, AccessKey, AccessKeyHash> parent_of_;
    std::unordered_map<AccessKey, TKAccessElement * __strong, AccessKeyHash> elements_;
    std::unordered_set<tk::Widget*> selection_hooked_;
    std::unordered_map<tk::Widget*, int> current_row_;
};

} // namespace

} // namespace tk::macos

// ─────────────────────────────────────────────────────────────────────────
//  TKAccessElement — one per AccessNode (identified by AccessKey)
// ─────────────────────────────────────────────────────────────────────────
//
// Always re-resolves its current AccessNode from the bridge on every query
// (via -tkNode) rather than caching one, since the underlying tree can be
// rebuilt between queries — mirrors qt_accessible.cpp's NodeAccessible /
// win32_accessible.cpp's AccessNodeProvider.

@interface TKAccessElement : NSAccessibilityElement
- (instancetype)initWithBridge:(tk::macos::AccessBridge*)bridge key:(tk::macos::AccessKey)key;
// Called by AccessBridge::detach()/rebuild_if_dirty() when this node's
// bridge is going away (Surface teardown) or this key no longer resolves
// to a node (retired). After this, -tkNode always returns nullptr and
// every accessor below returns its documented safe default.
- (void)detach;
@end

@implementation TKAccessElement
{
    tk::macos::AccessBridge* _bridge; // nulled by -detach
    tk::macos::AccessKey _key;
}

- (instancetype)initWithBridge:(tk::macos::AccessBridge*)bridge key:(tk::macos::AccessKey)key
{
    self = [super init];
    if (self)
    {
        _bridge = bridge;
        _key = key;
    }
    return self;
}

- (void)detach
{
    _bridge = nullptr;
}

- (const tk::AccessNode*)tkNode
{
    return _bridge ? _bridge->find(_key) : nullptr;
}

- (BOOL)isAccessibilityElement
{
    return YES;
}

- (NSAccessibilityRole)accessibilityRole
{
    const tk::AccessNode* n = self.tkNode;
    return n ? tk::macos::to_ns_role(n->role) : NSAccessibilityUnknownRole;
}

- (NSString*)accessibilitySubrole
{
    const tk::AccessNode* n = self.tkNode;
    return n ? tk::macos::to_ns_subrole(n->role) : nil;
}

- (NSString*)accessibilityLabel
{
    const tk::AccessNode* n = self.tkNode;
    return n ? [NSString stringWithUTF8String:n->name.c_str()] : @"";
}

- (id)accessibilityValue
{
    const tk::AccessNode* n = self.tkNode;
    if (!n)
    {
        return nil;
    }
    if (n->value.present)
    {
        return @(n->value.now);
    }
    switch (n->role)
    {
    case tk::Role::CheckBox:
    case tk::Role::Switch:
        return @(n->state.checked);
    case tk::Role::RadioButton:
        return @(n->state.selected);
    default:
        return nil;
    }
}

- (void)setAccessibilityValue:(id)value
{
    const tk::AccessNode* n = self.tkNode;
    if (n && [value isKindOfClass:[NSNumber class]])
    {
        tk::set_node_value(*n, [(NSNumber*)value doubleValue]);
    }
}

- (BOOL)isAccessibilityEnabled
{
    const tk::AccessNode* n = self.tkNode;
    return (n && tk::macos::node_disabled(*n)) ? NO : YES;
}

- (id)accessibilityMinValue
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->value.present) ? @(n->value.min) : nil;
}

- (id)accessibilityMaxValue
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->value.present) ? @(n->value.max) : nil;
}

- (NSString*)accessibilityHelp
{
    const tk::AccessNode* n = self.tkNode;
    if (!n || n->description.empty())
    {
        return nil;
    }
    return [NSString stringWithUTF8String:n->description.c_str()];
}

// Content language (BCP 47, AccessNode::language) as "AXLanguage", so
// VoiceOver can pick a matching voice. NSAccessibilityProtocols has no
// element-level property for it (only NSAccessibilityLanguageTextAttribute
// inside attributed strings), so this goes through the legacy attribute API
// under the same name WebKit and Chromium expose on their elements; every
// other attribute still resolves through the property methods via super.
static NSString* const kTKLanguageAttribute = @"AXLanguage";

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
- (NSArray<NSAccessibilityAttributeName>*)accessibilityAttributeNames
{
    NSArray<NSAccessibilityAttributeName>* names = [super accessibilityAttributeNames];
    const tk::AccessNode* n = self.tkNode;
    if (n && !n->language.empty() && ![names containsObject:kTKLanguageAttribute])
    {
        return [names arrayByAddingObject:kTKLanguageAttribute];
    }
    return names;
}

- (id)accessibilityAttributeValue:(NSAccessibilityAttributeName)attribute
{
    if ([attribute isEqualToString:kTKLanguageAttribute])
    {
        const tk::AccessNode* n = self.tkNode;
        if (!n || n->language.empty())
        {
            return nil;
        }
        return [NSString stringWithUTF8String:n->language.c_str()];
    }
    return [super accessibilityAttributeValue:attribute];
}
#pragma clang diagnostic pop

- (BOOL)isAccessibilityModal
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->modal) ? YES : NO;
}

// "Item N" for list rows / grid cells.
- (NSInteger)accessibilityIndex
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->pos_in_set > 0) ? n->pos_in_set - 1 : 0;
}

- (NSInteger)accessibilityRowCount
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->grid_row_count > 0) ? n->grid_row_count : 0;
}

- (NSInteger)accessibilityColumnCount
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->grid_col_count > 0) ? n->grid_col_count : 0;
}

- (NSRange)accessibilityRowIndexRange
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->grid_row >= 0) ? NSMakeRange(n->grid_row, 1) : NSMakeRange(NSNotFound, 0);
}

- (NSRange)accessibilityColumnIndexRange
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->grid_col >= 0) ? NSMakeRange(n->grid_col, 1) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)isAccessibilityFocused
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->row_index == -1 && n->widget && !n->activate)
               ? (n->widget->has_focus() ? YES : NO)
               : NO;
}

- (BOOL)isAccessibilityExpanded
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->state.expanded) ? YES : NO;
}

- (BOOL)isAccessibilitySelected
{
    const tk::AccessNode* n = self.tkNode;
    return (n && n->state.selected) ? YES : NO;
}

- (id)accessibilityParent
{
    if (!_bridge)
    {
        return nil;
    }
    if (TKAccessElement* parent = _bridge->parent_element_for(_key))
    {
        return parent;
    }
    // No tk-level parent — this key is the root; the surface's own NSView
    // plays the "fragment root" role (see the doc comment at the top of
    // this file), not a separate wrapper element.
    return _bridge->view();
}

- (NSRect)accessibilityFrame
{
    const tk::AccessNode* n = self.tkNode;
    return (n && _bridge) ? _bridge->to_screen_rect(n->rect) : NSZeroRect;
}

- (NSArray*)accessibilityChildren
{
    return _bridge ? _bridge->children_for(_key) : @[];
}

// AppKit's tab-group convention: individual tabs are exposed both as
// regular children AND via this special property, which VoiceOver
// consults specifically for "tab N of M" navigation on a tab group.
- (NSArray*)accessibilityTabs
{
    const tk::AccessNode* n = self.tkNode;
    if (n && n->role == tk::Role::TabList)
    {
        return self.accessibilityChildren;
    }
    return nil;
}

// For a List/Grid container node: the row/cell last reported "current" via
// arrow-key navigation (see AccessBridge::notify_current_row) — lets
// VoiceOver announce the current item without real keyboard focus ever
// leaving the ListView/GridView, mirroring the other platforms' identical
// non-focus-stealing "current item" model.
- (NSArray*)accessibilitySelectedChildren
{
    const tk::AccessNode* n = self.tkNode;
    if (!n || !_bridge)
    {
        return @[];
    }
    if (n->role != tk::Role::List && n->role != tk::Role::Grid)
    {
        return @[];
    }
    int idx = _bridge->current_row_for(n->widget);
    if (idx < 0)
    {
        return @[];
    }
    if (TKAccessElement* el = _bridge->element_for(tk::macos::AccessKey{n->widget, idx}))
    {
        return @[ el ];
    }
    return @[];
}

- (BOOL)accessibilityPerformPress
{
    const tk::AccessNode* n = self.tkNode;
    if (n)
    {
        tk::invoke_default_action(*n);
    }
    return n != nullptr;
}

@end

namespace tk::macos
{

namespace
{

void AccessBridge::rebuild_if_dirty()
{
    if (!dirty_)
    {
        return;
    }
    dirty_ = false;
    if (!surface_)
    {
        return; // detached
    }

    tk::Widget* root = surface_->root();
    tree_ = root ? tk::build_access_tree(root) : tk::AccessNode{};

    std::unordered_map<AccessKey, const tk::AccessNode*, AccessKeyHash> new_index;
    std::unordered_map<AccessKey, AccessKey, AccessKeyHash> new_parent;
    index_tree(tree_, new_index, new_parent);

    // Retire elements for keys that no longer resolve to a node (row/cell
    // removed, widget torn down) — mirrors qt_accessible.cpp's/
    // win32_accessible.cpp's identical retire loop. ARC releases the
    // element once both this map's reference and any VoiceOver-held
    // reference are gone; detach() just prevents it from touching this
    // bridge again in the meantime.
    for (auto it = elements_.begin(); it != elements_.end();)
    {
        if (new_index.find(it->first) == new_index.end())
        {
            [it->second detach];
            it = elements_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    bool structure_changed = new_index.size() != index_.size();
    if (!structure_changed)
    {
        for (const auto& entry : new_index)
        {
            if (index_.find(entry.first) == index_.end())
            {
                structure_changed = true;
                break;
            }
        }
    }

    index_ = std::move(new_index);
    parent_of_ = std::move(new_parent);

    // Coarse-grained "something changed" notification (once per rebuild
    // that actually added/removed nodes).
    if (structure_changed)
    {
        if (NSView* v = view())
        {
            NSAccessibilityPostNotification(v, NSAccessibilityLayoutChangedNotification);
        }
    }
    post_change_notifications();
}

void AccessBridge::post_change_notifications()
{
    for (auto it = snapshots_.begin(); it != snapshots_.end();)
    {
        auto node_it = index_.find(it->first);
        auto el_it = elements_.find(it->first);
        if (node_it == index_.end() || el_it == elements_.end())
        {
            it = snapshots_.erase(it);
            continue;
        }
        Snapshot now = snapshot_of(*node_it->second);
        Snapshot& was = it->second;
        TKAccessElement* el = el_it->second;
        if (now.name != was.name)
        {
            NSAccessibilityPostNotification(el, NSAccessibilityTitleChangedNotification);
        }
        if (now.state.checked != was.state.checked || now.state.selected != was.state.selected ||
            now.state.expanded != was.state.expanded || now.disabled != was.disabled ||
            now.value != was.value)
        {
            NSAccessibilityPostNotification(el, NSAccessibilityValueChangedNotification);
        }
        was = std::move(now);
        ++it;
    }
}

void AccessBridge::notify_current_row(tk::Widget* owner, int idx)
{
    current_row_[owner] = idx;
    if (idx < 0)
    {
        return; // deselected — nothing to report as "current"
    }
    if (TKAccessElement* container = element_for(AccessKey{owner, -1}))
    {
        NSAccessibilityPostNotification(container, NSAccessibilitySelectedRowsChangedNotification);
    }
}

TKAccessElement* AccessBridge::element_for(const AccessKey& key)
{
    rebuild_if_dirty();
    if (index_.find(key) == index_.end())
    {
        return nil;
    }

    auto it = elements_.find(key);
    if (it != elements_.end())
    {
        return it->second;
    }

    TKAccessElement* el = [[TKAccessElement alloc] initWithBridge:this key:key];
    elements_.emplace(key, el);
    remember(key);
    return el;
}

TKAccessElement* AccessBridge::parent_element_for(const AccessKey& key)
{
    rebuild_if_dirty();
    auto it = parent_of_.find(key);
    if (it == parent_of_.end())
    {
        return nil;
    }
    return element_for(it->second);
}

NSArray* AccessBridge::children_for(const AccessKey& key)
{
    rebuild_if_dirty();
    const tk::AccessNode* n = find(key);
    if (!n || n->children.empty())
    {
        return @[];
    }
    NSMutableArray* out = [NSMutableArray arrayWithCapacity:n->children.size()];
    for (auto& child : n->children)
    {
        if (TKAccessElement* el = element_for(key_for(child)))
        {
            [out addObject:el];
        }
    }
    return out;
}

void AccessBridge::detach()
{
    for (auto& entry : elements_)
    {
        [entry.second detach];
    }
    elements_.clear();
    surface_ = nullptr;
}

// One AccessBridge per Surface, keyed by the surface's NSView pointer
// value — mirrors win32_accessible.cpp's HWND-keyed registry (same
// rationale: TKSurfaceView's own accessibility overrides only have `self`
// to work with, not a Surface&).
std::unordered_map<const void*, std::unique_ptr<AccessBridge>>& bridge_registry()
{
    static std::unordered_map<const void*, std::unique_ptr<AccessBridge>> registry;
    return registry;
}

AccessBridge* bridge_for_view(id view)
{
    if (!view)
    {
        return nullptr;
    }
    auto& registry = bridge_registry();
    auto it = registry.find((__bridge const void*)view);
    return it == registry.end() ? nullptr : it->second.get();
}

AccessBridge* bridge_for_key(const void* key)
{
    auto& registry = bridge_registry();
    auto it = registry.find(key);
    return it == registry.end() ? nullptr : it->second.get();
}

void AccessBridge::schedule_refresh()
{
    if (elements_.empty() || refresh_pending_ || !surface_)
    {
        return;
    }
    NSView* v = view();
    if (!v)
    {
        return;
    }
    refresh_pending_ = true;
    const void* key = (__bridge const void*)v;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRefreshDelayMs * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
                       if (AccessBridge* b = bridge_for_key(key))
                       {
                           b->refresh_pending_ = false;
                           b->dirty_ = true;
                           b->rebuild_if_dirty();
                       }
                   });
}

void AccessBridge::announce(const std::string& text, bool assertive)
{
    NSView* v = view();
    if (!v)
    {
        return;
    }
    NSString* msg = [NSString stringWithUTF8String:text.c_str()];
    if (!msg)
    {
        return;
    }
    id target = v.window ? (id)v.window : (id)v;
    NSAccessibilityPostNotificationWithUserInfo(
        target, NSAccessibilityAnnouncementRequestedNotification, @{
            NSAccessibilityAnnouncementKey : msg,
            NSAccessibilityPriorityKey :
                @(assertive ? NSAccessibilityPriorityHigh : NSAccessibilityPriorityMedium),
        });
}

} // namespace

void attach_accessible_bridge(Surface& surface)
{
    NSView* view = (__bridge NSView*)surface.view_handle();
    if (!view)
    {
        return;
    }
    auto bridge = std::make_unique<AccessBridge>(&surface);
    AccessBridge* raw = bridge.get();
    bridge_registry().emplace((__bridge const void*)view, std::move(bridge));
    surface.add_layout_listener(
        [raw]
        {
            raw->mark_dirty();
        });
    const void* key = (__bridge const void*)view;
    surface.host().add_paint_listener(
        [key]
        {
            if (AccessBridge* b = bridge_for_key(key))
            {
                b->schedule_refresh();
            }
        });
}

void announce(id view, const std::string& text, bool assertive)
{
    if (AccessBridge* b = bridge_for_view(view))
    {
        b->announce(text, assertive);
    }
}

void detach_accessible_bridge(Surface& surface)
{
    NSView* view = (__bridge NSView*)surface.view_handle();
    if (!view)
    {
        return;
    }
    auto& registry = bridge_registry();
    auto it = registry.find((__bridge const void*)view);
    if (it == registry.end())
    {
        return;
    }
    it->second->detach();
    registry.erase(it);
}

BOOL access_is_element(id /*view*/)
{
    // The view itself is a pure container (its role/label reflect the root
    // AccessNode via the overrides below, but it should not additionally
    // claim to BE a leaf element distinct from its children) — matches
    // Windows' RootProvider design, where the fragment root and the
    // window-level container are the same conceptual object.
    return NO;
}

NSAccessibilityRole access_role(id view)
{
    AccessBridge* bridge = bridge_for_view(view);
    const tk::AccessNode* root = bridge ? bridge->root_node() : nullptr;
    return root ? to_ns_role(root->role) : NSAccessibilityGroupRole;
}

NSString* access_subrole(id view)
{
    AccessBridge* bridge = bridge_for_view(view);
    const tk::AccessNode* root = bridge ? bridge->root_node() : nullptr;
    return root ? to_ns_subrole(root->role) : nil;
}

NSString* access_label(id view)
{
    AccessBridge* bridge = bridge_for_view(view);
    const tk::AccessNode* root = bridge ? bridge->root_node() : nullptr;
    return root ? [NSString stringWithUTF8String:root->name.c_str()] : @"";
}

NSArray* access_children(id view)
{
    AccessBridge* bridge = bridge_for_view(view);
    if (!bridge)
    {
        return @[];
    }
    const tk::AccessNode* root = bridge->root_node();
    if (!root)
    {
        return @[];
    }
    return bridge->children_for(key_for(*root));
}

NSArray* access_tabs(id view)
{
    AccessBridge* bridge = bridge_for_view(view);
    const tk::AccessNode* root = bridge ? bridge->root_node() : nullptr;
    if (root && root->role == tk::Role::TabList)
    {
        return access_children(view);
    }
    return nil;
}

id access_hit_test(id view, NSPoint screen_point)
{
    AccessBridge* bridge = bridge_for_view(view);
    if (!bridge)
    {
        return nil;
    }
    NSView* v = bridge->view();
    if (!v || !v.window)
    {
        return nil;
    }
    NSRect screen_rect = NSMakeRect(screen_point.x, screen_point.y, 0, 0);
    NSRect win_rect = [v.window convertRectFromScreen:screen_rect];
    NSPoint local = [v convertPoint:win_rect.origin fromView:nil];
    const tk::AccessNode* root = bridge->root_node();
    if (!root)
    {
        return nil;
    }
    const tk::AccessNode* hit =
        hit_test_node(*root, tk::Point{static_cast<float>(local.x), static_cast<float>(local.y)});
    if (hit == root)
    {
        return view; // the view itself represents the root
    }
    return bridge->element_for(key_for(*hit));
}

id access_focused_element(id view)
{
    AccessBridge* bridge = bridge_for_view(view);
    if (!bridge || !bridge->surface())
    {
        return nil;
    }
    tk::Widget* focused = bridge->surface()->host().focused_widget();
    if (!focused)
    {
        return nil;
    }
    return bridge->element_for(AccessKey{focused, -1});
}

void notify_focus_changed(id view, tk::Widget* /*old_widget*/, tk::Widget* now_widget)
{
    AccessBridge* bridge = bridge_for_view(view);
    if (!bridge || !now_widget)
    {
        return;
    }
    if (TKAccessElement* el = bridge->element_for(AccessKey{now_widget, -1}))
    {
        NSAccessibilityPostNotification(el, NSAccessibilityFocusedUIElementChangedNotification);
    }
}

} // namespace tk::macos
