#include "win32_accessible.h"
#include "host_win32.h"
#include "access_tree.h"
#include "list_view.h"

#include <wrl/client.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tk::win32
{

namespace
{

using Microsoft::WRL::ComPtr;

// utf8_to_wide() is declared in host_win32.h and defined once in
// host_win32.cpp — shared rather than duplicated here, since a unity build
// merges every .cpp's anonymous namespace into one and would otherwise
// duplicate-define it.

CONTROLTYPEID to_uia_control_type(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:
        return UIA_ButtonControlTypeId;
    case tk::Role::CheckBox:
        return UIA_CheckBoxControlTypeId;
    case tk::Role::Switch:
        return UIA_CheckBoxControlTypeId;
    case tk::Role::RadioButton:
        return UIA_RadioButtonControlTypeId;
    case tk::Role::ComboBox:
        return UIA_ComboBoxControlTypeId;
    case tk::Role::TextInput:
        return UIA_EditControlTypeId;
    case tk::Role::StaticText:
        return UIA_TextControlTypeId;
    case tk::Role::Image:
        return UIA_ImageControlTypeId;
    case tk::Role::Link:
        return UIA_HyperlinkControlTypeId;
    case tk::Role::List:
        return UIA_ListControlTypeId;
    case tk::Role::ListItem:
        return UIA_ListItemControlTypeId;
    case tk::Role::Grid:
        return UIA_DataGridControlTypeId;
    case tk::Role::GridCell:
        return UIA_DataItemControlTypeId;
    case tk::Role::Tab:
        return UIA_TabItemControlTypeId;
    case tk::Role::TabList:
        return UIA_TabControlTypeId;
    case tk::Role::TabPanel:
        return UIA_PaneControlTypeId;
    case tk::Role::Dialog:
        return UIA_PaneControlTypeId;
    case tk::Role::MenuItem:
        return UIA_MenuItemControlTypeId;
    case tk::Role::Group:
        return UIA_GroupControlTypeId;
    case tk::Role::ProgressBar:
        return UIA_ProgressBarControlTypeId;
    case tk::Role::Slider:
        return UIA_SliderControlTypeId;
    case tk::Role::None:
        return UIA_PaneControlTypeId;
    }
    return UIA_PaneControlTypeId;
}

// Roles a screen reader would plausibly invoke a "click" action on. Ported
// verbatim from qt_accessible.cpp's identical predicate — see its own
// comment for the rationale (conservative approximation, harmless since
// tk::invoke_default_action() on a node with no real action is already a
// documented no-op).
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

bool supports_toggle(tk::Role r)
{
    return r == tk::Role::CheckBox || r == tk::Role::Switch;
}

bool supports_selection_item(tk::Role r)
{
    return r == tk::Role::ListItem || r == tk::Role::GridCell || r == tk::Role::Tab ||
           r == tk::Role::RadioButton;
}

bool supports_expand_collapse(tk::Role r)
{
    return r == tk::Role::ComboBox;
}

// Identifies one AccessNode stably across tree rebuilds — identical in
// shape and rationale to qt_accessible.cpp's AccessKey (see its own doc
// comment): the tk::Widget a node came from, plus a row/cell index for a
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

// Last values raised to UIA for a provider — see AccessBridge::
// raise_change_events (mirrors qt_accessible.cpp's Snapshot).
struct Snapshot
{
    std::string name;
    std::string description;
    tk::AccessState state;
    bool disabled = false;
    double value = 0.0;
    tk::Rect rect;
};

Snapshot snapshot_of(const tk::AccessNode& n)
{
    return {n.name, n.description, n.state, node_disabled(n), n.value.now, n.rect};
}

// Debounce for the AT-active refresh — see qt_accessible.cpp's
// kRefreshDelayMs.
constexpr int kRefreshDelayMs = 200;

VARIANT bstr_variant(const std::string& s)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(utf8_to_wide(s).c_str());
    return v;
}

VARIANT i4_variant(int i)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = i;
    return v;
}

VARIANT bool_variant(bool b)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    return v;
}

class AccessNodeProvider;

// Per-Surface accessibility state: the cached AccessNode tree, an index from
// AccessKey to the live node, parent lookups, and a cached COM provider per
// AccessKey ever queried — reused across rebuilds so a node's provider
// identity stays stable for as long as the node itself keeps existing
// (required: a UIA client treats provider pointer identity as object
// identity, and may cache it).
//
// Rebuilding is lazy (only on the next query after a relayout marks this
// dirty), matching qt_accessible.cpp's AccessBridge exactly — see its doc
// comment for why (virtualized-row cost, relayout frequency during
// scrolling/typing).
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
    HWND hwnd() const
    {
        return surface_ ? surface_->hwnd() : nullptr;
    }

    void mark_dirty()
    {
        dirty_ = true;
        schedule_refresh();
    }

    // While a UIA client holds providers (has queried us), rebuild shortly
    // after any relayout/repaint so property/structure changes are raised as
    // events. Looked up again by HWND when the timer fires — the bridge may
    // have been detached and destroyed in between.
    void schedule_refresh();

    // UiaRaiseNotificationEvent on the root provider (Windows 10 1709+).
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

    const tk::AccessNode* parent_node_for(const AccessKey& key)
    {
        rebuild_if_dirty();
        auto it = parent_of_.find(key);
        if (it == parent_of_.end())
        {
            return nullptr;
        }
        auto it2 = index_.find(it->second);
        return it2 == index_.end() ? nullptr : it2->second;
    }

    bool is_root(const AccessKey& key)
    {
        rebuild_if_dirty();
        return tree_.widget && key_for(tree_) == key;
    }

    RECT to_screen_rect(const tk::Rect& r) const
    {
        RECT rc{0, 0, 0, 0};
        if (!surface_)
        {
            return rc;
        }
        const float scale = surface_->dpi_scale();
        POINT top_left{static_cast<LONG>(std::lround(r.x * scale)),
                       static_cast<LONG>(std::lround(r.y * scale))};
        ClientToScreen(surface_->hwnd(), &top_left);
        rc.left = top_left.x;
        rc.top = top_left.y;
        rc.right = rc.left + static_cast<LONG>(std::lround(r.w * scale));
        rc.bottom = rc.top + static_cast<LONG>(std::lround(r.h * scale));
        return rc;
    }

    // Returns the (possibly newly created) provider for `key`, or nullptr if
    // `key` doesn't currently resolve to a node. Defined out-of-class, after
    // AccessNodeProvider's complete definition (constructs one).
    AccessNodeProvider* provider_for(const AccessKey& key);

    // The parent provider for `key`: the owning node's provider if `key`
    // has a tk-level parent, or nullptr if `key` is the root (Navigate's
    // caller checks is_root() separately for that case).
    AccessNodeProvider* parent_provider_for(const AccessKey& key);

    AccessNodeProvider* root_provider();

    // Disconnects and drops every cached provider — see win32_accessible.h's
    // detach_accessible_bridge doc comment for when/why this runs.
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
    // itself) — mirrors qt_accessible.cpp's identical hook_selection_changed,
    // wired once per distinct instance encountered while walking the tree.
    void hook_selection_changed(tk::Widget* widget)
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

    // Defined out-of-class (raises the event on a live provider, which
    // needs AccessNodeProvider's complete type).
    void notify_current_row(tk::Widget* owner, int idx);

    // Defined out-of-class (needs AccessNodeProvider's complete type).
    void raise_change_events();
    // UIA_BoundingRectanglePropertyId change for `key`'s provider, if any.
    void raise_rect_changed_(const AccessKey& key);
    // Keys whose on-screen rect a UIA client is tracking: the focused widget
    // and each list/grid's current row (Narrator's highlight follows those).
    // Rect-change events go to these only — a scroll moves every row.
    std::vector<AccessKey> tracked_keys_() const;

public:
    // The top-level window moved: tracked elements' screen rects changed
    // even though nothing in the tk tree did.
    void on_window_moved();
    // Subclasses the surface's top-level window (once) to hear about moves —
    // the surface is a child window and gets no WM_MOVE of its own.
    void ensure_root_hooked();
    void unhook_root();
    void schedule_window_moved_();
    void forget_root_()
    {
        hooked_root_ = nullptr;
    }

private:
    std::unordered_map<AccessKey, std::vector<AccessKey>, AccessKeyHash> children_of_;
    std::unordered_map<tk::Widget*, int> current_row_;
    HWND hooked_root_ = nullptr;
    bool move_pending_ = false;

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
    int next_runtime_id_ = 1;
    std::unordered_map<AccessKey, const tk::AccessNode*, AccessKeyHash> index_;
    std::unordered_map<AccessKey, AccessKey, AccessKeyHash> parent_of_;
    std::unordered_map<AccessKey, ComPtr<AccessNodeProvider>, AccessKeyHash> providers_;
    std::unordered_set<tk::Widget*> selection_hooked_;
};

// One COM object per AccessNode (identified by AccessKey), implementing
// every provider interface this bridge ever exposes on a single class —
// deliberately not split into a separate "root" subclass (see the plan doc
// for the reasoning): IRawElementProviderFragmentRoot is implemented
// unconditionally here too, but QueryInterface only ever hands out that
// interface pointer when bridge_->is_root(key_) is true, so it's only ever
// reachable for the one node that legitimately is the fragment root.
//
// Always re-resolves its current AccessNode from the bridge on every query
// (via node()) rather than caching one, since the underlying tree can be
// rebuilt between queries — mirrors qt_accessible.cpp's NodeAccessible.
//
// A UIA client can hold a reference to this object past its node's
// retirement, or past the whole bridge being torn down (window close) — see
// detach()/bridge_ handling throughout: every accessor degrades to a safe
// default once bridge_ is null or node() returns nullptr, rather than
// assuming either is still valid. Mirrors host_win32.cpp's existing
// DropTarget::detach_host() idiom for the identical class of problem.
class AccessNodeProvider final : public IRawElementProviderSimple,
                                 public IRawElementProviderFragment,
                                 public IRawElementProviderFragmentRoot,
                                 public IInvokeProvider,
                                 public IToggleProvider,
                                 public ISelectionItemProvider,
                                 public IExpandCollapseProvider,
                                 public IRangeValueProvider,
                                 public IGridProvider,
                                 public IGridItemProvider
{
public:
    AccessNodeProvider(AccessBridge* bridge, AccessKey key, int runtime_id)
        : bridge_(bridge), key_(key), runtime_id_(runtime_id)
    {
    }

    // Called by AccessBridge::detach()/rebuild_if_dirty() when this node's
    // bridge is going away (window teardown) or this key no longer resolves
    // to a node (retired). After this, node() always returns nullptr and
    // every accessor below returns its documented safe default.
    void detach()
    {
        bridge_ = nullptr;
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        *ppv = nullptr;

        const tk::Role role = node() ? node()->role : tk::Role::None;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
        {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        }
        else if (riid == __uuidof(IRawElementProviderFragment))
        {
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        }
        else if (riid == __uuidof(IRawElementProviderFragmentRoot) && bridge_ &&
                 bridge_->is_root(key_))
        {
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        }
        else if (riid == __uuidof(IInvokeProvider) && has_action_role(role))
        {
            *ppv = static_cast<IInvokeProvider*>(this);
        }
        else if (riid == __uuidof(IToggleProvider) && supports_toggle(role))
        {
            *ppv = static_cast<IToggleProvider*>(this);
        }
        else if (riid == __uuidof(ISelectionItemProvider) && supports_selection_item(role))
        {
            *ppv = static_cast<ISelectionItemProvider*>(this);
        }
        else if (riid == __uuidof(IExpandCollapseProvider) && supports_expand_collapse(role))
        {
            *ppv = static_cast<IExpandCollapseProvider*>(this);
        }
        else if (riid == __uuidof(IRangeValueProvider) && node() && node()->value.present)
        {
            *ppv = static_cast<IRangeValueProvider*>(this);
        }
        else if (riid == __uuidof(IGridProvider) && node() && node()->grid_col_count > 0)
        {
            *ppv = static_cast<IGridProvider*>(this);
        }
        else if (riid == __uuidof(IGridItemProvider) && node() && node()->grid_row >= 0)
        {
            *ppv = static_cast<IGridItemProvider*>(this);
        }

        if (!*ppv)
        {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs_;
        if (n == 0)
        {
            delete this;
        }
        return n;
    }

    // ---- IRawElementProviderSimple ----
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        // UseComThreading: UI Automation calls the provider on the window's STA
        // thread, so the tree walk and the row callbacks stay on the UI thread.
        *pRetVal = ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        const tk::AccessNode* n = node();
        if (!n)
        {
            return S_OK;
        }
        switch (patternId)
        {
        case UIA_InvokePatternId:
            if (has_action_role(n->role))
            {
                AddRef();
                *pRetVal = static_cast<IInvokeProvider*>(this);
            }
            break;
        case UIA_TogglePatternId:
            if (supports_toggle(n->role))
            {
                AddRef();
                *pRetVal = static_cast<IToggleProvider*>(this);
            }
            break;
        case UIA_SelectionItemPatternId:
            if (supports_selection_item(n->role))
            {
                AddRef();
                *pRetVal = static_cast<ISelectionItemProvider*>(this);
            }
            break;
        case UIA_ExpandCollapsePatternId:
            if (supports_expand_collapse(n->role))
            {
                AddRef();
                *pRetVal = static_cast<IExpandCollapseProvider*>(this);
            }
            break;
        case UIA_RangeValuePatternId:
            if (n->value.present)
            {
                AddRef();
                *pRetVal = static_cast<IRangeValueProvider*>(this);
            }
            break;
        case UIA_GridPatternId:
            if (n->grid_col_count > 0)
            {
                AddRef();
                *pRetVal = static_cast<IGridProvider*>(this);
            }
            break;
        case UIA_GridItemPatternId:
            if (n->grid_row >= 0)
            {
                AddRef();
                *pRetVal = static_cast<IGridItemProvider*>(this);
            }
            break;
        default:
            break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        VariantInit(pRetVal);
        const tk::AccessNode* n = node();
        if (!n)
        {
            return S_OK;
        }
        // A real Widget node (row_index == -1) can read enabled/focus state
        // straight off its Widget; a synthesized row/cell has neither (see
        // AccessState's own doc comment — enabled/focused deliberately
        // aren't part of it) — default both to "not a concern" rather than
        // guessing, matching qt_accessible.cpp's NodeAccessible::state()
        // never setting a "disabled" bit for any node either.
        const bool is_real_widget = n->row_index == -1 && n->widget != nullptr && !n->activate;
        switch (propertyId)
        {
        case UIA_ControlTypePropertyId:
            pRetVal->vt = VT_I4;
            pRetVal->lVal = to_uia_control_type(n->role);
            break;
        case UIA_NamePropertyId:
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(utf8_to_wide(n->name).c_str());
            break;
        case UIA_IsEnabledPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = node_disabled(*n) ? VARIANT_FALSE : VARIANT_TRUE;
            break;
        case UIA_CulturePropertyId:
            // Content language (UIA wants an LCID); an unknown tag maps to 0
            // and leaves the property unset.
            if (!n->language.empty())
            {
                const LCID lcid = LocaleNameToLCID(utf8_to_wide(n->language).c_str(), 0);
                if (lcid != 0)
                {
                    *pRetVal = i4_variant(static_cast<int>(lcid));
                }
            }
            break;
        case UIA_HelpTextPropertyId:
            if (!n->description.empty())
            {
                *pRetVal = bstr_variant(n->description);
            }
            break;
#ifdef UIA_FullDescriptionPropertyId
        case UIA_FullDescriptionPropertyId:
            if (!n->description.empty())
            {
                *pRetVal = bstr_variant(n->description);
            }
            break;
#endif
#ifdef UIA_PositionInSetPropertyId
        case UIA_PositionInSetPropertyId:
            if (n->pos_in_set > 0)
            {
                *pRetVal = i4_variant(n->pos_in_set);
            }
            break;
        case UIA_SizeOfSetPropertyId:
            if (n->row_set_size > 0)
            {
                *pRetVal = i4_variant(n->row_set_size);
            }
            break;
#endif
#ifdef UIA_IsDialogPropertyId
        // UIA has no non-HWND dialog control type; Narrator/NVDA treat any
        // element with IsDialog as a dialog (and build_access_tree already
        // prunes the inert content behind a modal one).
        case UIA_IsDialogPropertyId:
            *pRetVal = bool_variant(n->role == tk::Role::Dialog);
            break;
#endif
        case UIA_IsKeyboardFocusablePropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal =
                (is_real_widget && n->widget->focusable()) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_HasKeyboardFocusPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal =
                (is_real_widget && n->widget->has_focus()) ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case UIA_IsContentElementPropertyId:
        case UIA_IsControlElementPropertyId:
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = VARIANT_TRUE;
            break;
        default:
            break; // VT_EMPTY (already set by VariantInit) — "not implemented", per contract
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (bridge_ && bridge_->is_root(key_))
        {
            return UiaHostProviderFromHwnd(bridge_->hwnd(), pRetVal);
        }
        return S_OK;
    }

    // ---- IRawElementProviderFragment ----
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (!bridge_)
        {
            return S_OK;
        }

        switch (direction)
        {
        case NavigateDirection_Parent:
        {
            if (bridge_->is_root(key_))
            {
                return S_OK;
            }
            if (auto* p = bridge_->parent_provider_for(key_))
            {
                p->AddRef();
                *pRetVal = p;
            }
            return S_OK;
        }
        case NavigateDirection_FirstChild:
        case NavigateDirection_LastChild:
        {
            const tk::AccessNode* n = node();
            if (!n || n->children.empty())
            {
                return S_OK;
            }
            const tk::AccessNode& child = (direction == NavigateDirection_FirstChild)
                                              ? n->children.front()
                                              : n->children.back();
            if (auto* p = bridge_->provider_for(key_for(child)))
            {
                p->AddRef();
                *pRetVal = p;
            }
            return S_OK;
        }
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling:
        {
            const tk::AccessNode* parent = bridge_->parent_node_for(key_);
            if (!parent)
            {
                return S_OK;
            }
            const auto& kids = parent->children;
            for (std::size_t i = 0; i < kids.size(); ++i)
            {
                if (!(key_for(kids[i]) == key_))
                {
                    continue;
                }
                std::size_t j;
                if (direction == NavigateDirection_NextSibling)
                {
                    if (i + 1 >= kids.size())
                    {
                        return S_OK;
                    }
                    j = i + 1;
                }
                else
                {
                    if (i == 0)
                    {
                        return S_OK;
                    }
                    j = i - 1;
                }
                if (auto* p = bridge_->provider_for(key_for(kids[j])))
                {
                    p->AddRef();
                    *pRetVal = p;
                }
                return S_OK;
            }
            return S_OK;
        }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        // Null = "this is the fragment root associated with the hwnd" —
        // required by the UIA contract for the element WM_GETOBJECT/
        // UiaReturnRawElementProvider returns for UiaRootObjectId.
        if (bridge_ && bridge_->is_root(key_))
        {
            return S_OK;
        }

        int rt_id[2] = {UiaAppendRuntimeId, runtime_id_};
        SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!sa)
        {
            return E_OUTOFMEMORY;
        }
        for (LONG i = 0; i < 2; ++i)
        {
            SafeArrayPutElement(sa, &i, &rt_id[i]);
        }
        *pRetVal = sa;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = UiaRect{0, 0, 0, 0};
        const tk::AccessNode* n = node();
        if (!n || !bridge_)
        {
            return S_OK;
        }
        RECT rc = bridge_->to_screen_rect(n->rect);
        pRetVal->left = rc.left;
        pRetVal->top = rc.top;
        pRetVal->width = rc.right - rc.left;
        pRetVal->height = rc.bottom - rc.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override
    {
        const tk::AccessNode* n = node();
        if (n && n->widget && bridge_ && bridge_->surface())
        {
            bridge_->surface()->host().request_focus(n->widget);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (!bridge_)
        {
            return S_OK;
        }
        if (auto* root = bridge_->root_provider())
        {
            root->AddRef();
            *pRetVal = root;
        }
        return S_OK;
    }

    // ---- IRawElementProviderFragmentRoot ----
    // Only ever reachable via QueryInterface on the actual root node (see
    // QueryInterface's is_root() gate above), so no extra guard is needed
    // here beyond the usual node()/bridge_ null checks.
    HRESULT STDMETHODCALLTYPE
    ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (!bridge_)
        {
            return S_OK;
        }
        POINT screen{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
        POINT client = screen;
        if (!ScreenToClient(bridge_->hwnd(), &client))
        {
            return S_OK;
        }
        const float scale = bridge_->surface() ? bridge_->surface()->dpi_scale() : 1.f;
        const tk::Point world{client.x / scale, client.y / scale};
        const tk::AccessNode* root = bridge_->root_node();
        if (!root)
        {
            return S_OK;
        }
        const tk::AccessNode* hit = hit_test(*root, world);
        if (hit == root)
        {
            AddRef();
            *pRetVal = this;
            return S_OK;
        }
        if (auto* p = bridge_->provider_for(key_for(*hit)))
        {
            p->AddRef();
            *pRetVal = p;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (!bridge_ || !bridge_->surface())
        {
            return S_OK;
        }
        tk::Widget* focused = bridge_->surface()->host().focused_widget();
        if (!focused)
        {
            return S_OK;
        }
        if (auto* p = bridge_->provider_for(AccessKey{focused, -1}))
        {
            p->AddRef();
            *pRetVal = p;
        }
        return S_OK;
    }

    // ---- IInvokeProvider ----
    HRESULT STDMETHODCALLTYPE Invoke() override
    {
        if (const tk::AccessNode* n = node())
        {
            tk::invoke_default_action(*n);
        }
        return S_OK;
    }

    // ---- IToggleProvider ----
    HRESULT STDMETHODCALLTYPE Toggle() override
    {
        if (const tk::AccessNode* n = node())
        {
            tk::invoke_default_action(*n);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = (n && n->state.checked) ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // ---- ISelectionItemProvider ----
    HRESULT STDMETHODCALLTYPE Select() override
    {
        if (const tk::AccessNode* n = node())
        {
            tk::invoke_default_action(*n);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override
    {
        return Select();
    }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
    {
        // No per-item deselect action exists in the shared model (a
        // ListItem/GridCell's only action is "activate" — see
        // tk::invoke_default_action) — no-op rather than guessing at one.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = (n && n->state.selected) ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        // Left null: full multi/single-selection container semantics
        // (ISelectionProvider/IGridProvider on the owning List/Grid) are an
        // explicitly accepted v1 gap — see the plan doc's note on
        // GridAdapterAccessibility's flat (non-row/column) model. A null
        // container is a spec-permitted response, not an error.
        *pRetVal = nullptr;
        return S_OK;
    }

    // ---- IExpandCollapseProvider ----
    HRESULT STDMETHODCALLTYPE Expand() override
    {
        const tk::AccessNode* n = node();
        if (n && !n->state.expanded)
        {
            tk::invoke_default_action(*n);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Collapse() override
    {
        const tk::AccessNode* n = node();
        if (n && n->state.expanded)
        {
            tk::invoke_default_action(*n);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal =
            (n && n->state.expanded) ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
        return S_OK;
    }

    // ---- IRangeValueProvider (Role::ProgressBar read-only, Role::Slider settable) ----
    HRESULT STDMETHODCALLTYPE SetValue(double v) override
    {
        const tk::AccessNode* n = node();
        if (!n || n->role != tk::Role::Slider || !tk::set_node_value(*n, v))
        {
            return UIA_E_ELEMENTNOTENABLED;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->value.now : 0.0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal =
            (n && n->role == tk::Role::Slider && n->widget && n->widget->enabled()) ? FALSE : TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->value.max : 0.0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->value.min : 0.0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = 0.0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = 0.0;
        return S_OK;
    }

    // ---- IGridProvider (grid container) ----
    HRESULT STDMETHODCALLTYPE GetItem(int row, int column,
                                      IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        const tk::AccessNode* n = node();
        if (!n || !bridge_)
        {
            return E_INVALIDARG;
        }
        for (const auto& child : n->children)
        {
            if (child.grid_row == row && child.grid_col == column)
            {
                if (auto* p = bridge_->provider_for(key_for(child)))
                {
                    p->AddRef();
                    *pRetVal = static_cast<IRawElementProviderSimple*>(p);
                }
                return S_OK;
            }
        }
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE get_RowCount(int* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->grid_row_count : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ColumnCount(int* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->grid_col_count : 0;
        return S_OK;
    }

    // ---- IGridItemProvider (grid cell) ----
    HRESULT STDMETHODCALLTYPE get_Row(int* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->grid_row : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Column(int* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        const tk::AccessNode* n = node();
        *pRetVal = n ? n->grid_col : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_RowSpan(int* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ColumnSpan(int* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ContainingGrid(IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (bridge_)
        {
            if (auto* p = bridge_->parent_provider_for(key_))
            {
                p->AddRef();
                *pRetVal = static_cast<IRawElementProviderSimple*>(p);
            }
        }
        return S_OK;
    }

private:
    const tk::AccessNode* node() const
    {
        return bridge_ ? bridge_->find(key_) : nullptr;
    }

    // Depth-first hit test against world-space AccessNode rects (already in
    // the same coordinate space as `world` — see AccessNode::rect's own doc
    // comment), returning the deepest matching descendant or `node` itself
    // if none of its children claim the point.
    static const tk::AccessNode* hit_test(const tk::AccessNode& node, tk::Point world)
    {
        for (const auto& child : node.children)
        {
            if (child.rect.x <= world.x && world.x <= child.rect.x + child.rect.w &&
                child.rect.y <= world.y && world.y <= child.rect.y + child.rect.h)
            {
                return hit_test(child, world);
            }
        }
        return &node;
    }

    AccessBridge* bridge_; // nulled by detach()
    AccessKey key_;
    int runtime_id_;
    std::atomic<ULONG> refs_{1};
};

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

    // Retire providers for keys that no longer resolve to a node (row/cell
    // removed, widget torn down) — disconnect first (a UIA client may still
    // hold a reference; see the class's own doc comment), then drop our
    // cached ref. Mirrors qt_accessible.cpp's identical retire loop, using
    // UiaDisconnectProvider/AccessNodeProvider::detach() in place of Qt's
    // QAccessible::deleteAccessibleInterface().
    for (auto it = providers_.begin(); it != providers_.end();)
    {
        if (!new_index.contains(it->first))
        {
            UiaDisconnectProvider(it->second.Get());
            it->second->detach();
            it = providers_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    index_ = std::move(new_index);
    parent_of_ = std::move(new_parent);

    // ChildrenInvalidated on each container (that a UIA client has seen)
    // whose child list changed — never on the root: it is a nameless Pane,
    // and Narrator speaks the event's element ("pane" on every room switch).
    std::unordered_map<AccessKey, std::vector<AccessKey>, AccessKeyHash> new_children;
    for (const auto& [key, node] : index_)
    {
        if (node->children.empty())
        {
            continue;
        }
        auto& kids = new_children[key];
        kids.reserve(node->children.size());
        for (const auto& ch : node->children)
        {
            kids.push_back(key_for(ch));
        }
    }
    const AccessKey root_key = tree_.widget ? key_for(tree_) : AccessKey{};
    for (const auto& [key, kids] : new_children)
    {
        if (key == root_key)
        {
            continue;
        }
        auto old = children_of_.find(key);
        if (old != children_of_.end() && old->second == kids)
        {
            continue;
        }
        auto prov = providers_.find(key);
        if (prov != providers_.end())
        {
            UiaRaiseStructureChangedEvent(prov->second.Get(),
                                          StructureChangeType_ChildrenInvalidated, nullptr, 0);
        }
    }
    for (const auto& [key, kids] : children_of_)
    {
        if (key == root_key || new_children.contains(key))
        {
            continue;
        }
        auto prov = providers_.find(key);
        if (prov != providers_.end()) // all children removed
        {
            UiaRaiseStructureChangedEvent(prov->second.Get(),
                                          StructureChangeType_ChildrenInvalidated, nullptr, 0);
        }
    }
    children_of_ = std::move(new_children);

    raise_change_events();
}

void AccessBridge::raise_change_events()
{
    std::vector<AccessKey> moved_keys;
    for (auto it = snapshots_.begin(); it != snapshots_.end();)
    {
        auto node_it = index_.find(it->first);
        auto prov_it = providers_.find(it->first);
        if (node_it == index_.end() || prov_it == providers_.end())
        {
            it = snapshots_.erase(it);
            continue;
        }
        const tk::AccessNode& node = *node_it->second;
        AccessNodeProvider* p = prov_it->second.Get();
        Snapshot now = snapshot_of(node);
        Snapshot& was = it->second;

        auto raise = [p](PROPERTYID id, VARIANT oldv, VARIANT newv)
        {
            UiaRaiseAutomationPropertyChangedEvent(p, id, oldv, newv);
            VariantClear(&oldv);
            VariantClear(&newv);
        };
        if (now.name != was.name)
        {
            raise(UIA_NamePropertyId, bstr_variant(was.name), bstr_variant(now.name));
        }
        if (now.description != was.description)
        {
            raise(UIA_HelpTextPropertyId, bstr_variant(was.description),
                  bstr_variant(now.description));
        }
        if (now.disabled != was.disabled)
        {
            raise(UIA_IsEnabledPropertyId, bool_variant(!was.disabled),
                  bool_variant(!now.disabled));
        }
        if (now.state.checked != was.state.checked && supports_toggle(node.role))
        {
            raise(UIA_ToggleToggleStatePropertyId,
                  i4_variant(was.state.checked ? ToggleState_On : ToggleState_Off),
                  i4_variant(now.state.checked ? ToggleState_On : ToggleState_Off));
        }
        if (now.state.expanded != was.state.expanded && supports_expand_collapse(node.role))
        {
            raise(UIA_ExpandCollapseExpandCollapseStatePropertyId,
                  i4_variant(was.state.expanded ? ExpandCollapseState_Expanded
                                                : ExpandCollapseState_Collapsed),
                  i4_variant(now.state.expanded ? ExpandCollapseState_Expanded
                                                : ExpandCollapseState_Collapsed));
        }
        if (now.state.selected != was.state.selected && supports_selection_item(node.role))
        {
            raise(UIA_SelectionItemIsSelectedPropertyId, bool_variant(was.state.selected),
                  bool_variant(now.state.selected));
        }
        if (node.value.present && now.value != was.value)
        {
            VARIANT oldv;
            VariantInit(&oldv);
            oldv.vt = VT_R8;
            oldv.dblVal = was.value;
            VARIANT newv;
            VariantInit(&newv);
            newv.vt = VT_R8;
            newv.dblVal = now.value;
            raise(UIA_RangeValueValuePropertyId, oldv, newv);
        }
        const bool moved = now.rect.x != was.rect.x || now.rect.y != was.rect.y ||
                           now.rect.w != was.rect.w || now.rect.h != was.rect.h;
        was = std::move(now);
        if (moved)
        {
            moved_keys.push_back(it->first);
        }
        ++it;
    }
    // Scrolling moves every row; only tell UIA about what it is tracking.
    for (const AccessKey& k : tracked_keys_())
    {
        if (std::find(moved_keys.begin(), moved_keys.end(), k) != moved_keys.end())
        {
            raise_rect_changed_(k);
        }
    }
}

std::vector<AccessKey> AccessBridge::tracked_keys_() const
{
    std::vector<AccessKey> keys;
    if (surface_)
    {
        if (tk::Widget* f = surface_->host().focused_widget())
        {
            keys.push_back(AccessKey{f, -1});
        }
    }
    for (const auto& [owner, idx] : current_row_)
    {
        if (idx >= 0)
        {
            keys.push_back(AccessKey{owner, idx});
        }
    }
    return keys;
}

void AccessBridge::raise_rect_changed_(const AccessKey& key)
{
    auto prov = providers_.find(key);
    auto node = index_.find(key);
    if (prov == providers_.end() || node == index_.end())
    {
        return;
    }
    const RECT rc = to_screen_rect(node->second->rect);
    VARIANT newv;
    VariantInit(&newv);
    newv.vt = VT_R8 | VT_ARRAY;
    newv.parray = SafeArrayCreateVector(VT_R8, 0, 4);
    if (newv.parray)
    {
        double vals[4] = {static_cast<double>(rc.left), static_cast<double>(rc.top),
                          static_cast<double>(rc.right - rc.left),
                          static_cast<double>(rc.bottom - rc.top)};
        for (LONG i = 0; i < 4; ++i)
        {
            SafeArrayPutElement(newv.parray, &i, &vals[i]);
        }
    }
    VARIANT oldv;
    VariantInit(&oldv); // VT_EMPTY: unknown — clients re-query
    UiaRaiseAutomationPropertyChangedEvent(prov->second.Get(), UIA_BoundingRectanglePropertyId,
                                           oldv, newv);
    VariantClear(&newv);
}

void AccessBridge::on_window_moved()
{
    for (const AccessKey& k : tracked_keys_())
    {
        raise_rect_changed_(k);
    }
}

void AccessBridge::notify_current_row(tk::Widget* owner, int idx)
{
    current_row_[owner] = idx;
    if (idx < 0)
    {
        return; // deselected — nothing to report as "current"
    }
    if (auto* p = provider_for(AccessKey{owner, idx}))
    {
        UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId);
    }
}

AccessNodeProvider* AccessBridge::provider_for(const AccessKey& key)
{
    rebuild_if_dirty();
    if (!index_.contains(key))
    {
        return nullptr;
    }

    auto it = providers_.find(key);
    if (it != providers_.end())
    {
        return it->second.Get();
    }

    ComPtr<AccessNodeProvider> p;
    p.Attach(new AccessNodeProvider(this, key, next_runtime_id_++));
    AccessNodeProvider* raw = p.Get();
    providers_.emplace(key, std::move(p));
    remember(key);
    ensure_root_hooked();
    return raw;
}

AccessNodeProvider* AccessBridge::parent_provider_for(const AccessKey& key)
{
    rebuild_if_dirty();
    auto it = parent_of_.find(key);
    if (it == parent_of_.end())
    {
        return nullptr;
    }
    return provider_for(it->second);
}

AccessNodeProvider* AccessBridge::root_provider()
{
    rebuild_if_dirty();
    return tree_.widget ? provider_for(key_for(tree_)) : nullptr;
}

void AccessBridge::detach()
{
    // Iterate a local, moved-out copy rather than providers_ itself:
    // UiaDisconnectProvider() can synchronize with an out-of-process UIA
    // client and pump this thread's sent-message queue while blocked, so a
    // reentrant WM_GETOBJECT could otherwise insert into providers_ mid-loop
    // and invalidate this range-for's iterator (see detach_accessible_bridge()
    // for the other half of this defense — closing the registry lookup that
    // such a reentrant call would otherwise still find).
    auto providers = std::move(providers_);
    for (auto& entry : providers)
    {
        UiaDisconnectProvider(entry.second.Get());
        entry.second->detach();
    }
    providers_.clear();
    unhook_root();
    surface_ = nullptr;
}

// One AccessBridge per Surface, keyed by HWND (not by Surface* or the
// opaque tk::win32::Host*, which surface_wnd_proc doesn't have direct
// access to as a Surface — mirrors host_win32.cpp's own
// drop_targets_by_hwnd() registry, same rationale: the wndproc only ever
// has an HWND and the internal Host* to work with).
std::unordered_map<HWND, std::unique_ptr<AccessBridge>>& bridge_registry()
{
    static std::unordered_map<HWND, std::unique_ptr<AccessBridge>> registry;
    return registry;
}

AccessBridge* bridge_for_hwnd(HWND hwnd)
{
    auto& registry = bridge_registry();
    auto it = registry.find(hwnd);
    return it == registry.end() ? nullptr : it->second.get();
}

// Debounce for window-move rect events: one burst after the drag settles
// rather than one per WM_WINDOWPOSCHANGED.
constexpr int kMoveSettleMs = 150;

LRESULT CALLBACK root_move_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                         DWORD_PTR ref)
{
    const HWND surface_hwnd = reinterpret_cast<HWND>(ref);
    switch (msg)
    {
    case WM_WINDOWPOSCHANGED:
    {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (pos && (pos->flags & SWP_NOMOVE))
        {
            break;
        }
        if (AccessBridge* b = bridge_for_hwnd(surface_hwnd))
        {
            b->schedule_window_moved_();
        }
        break;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, root_move_subclass_proc, id);
        if (AccessBridge* b = bridge_for_hwnd(surface_hwnd))
        {
            b->forget_root_();
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void AccessBridge::ensure_root_hooked()
{
    if (hooked_root_ || !surface_)
    {
        return;
    }
    const HWND self = surface_->hwnd();
    const HWND root = self ? GetAncestor(self, GA_ROOT) : nullptr;
    if (!root || root == self)
    {
        return; // a top-level surface gets WM_WINDOWPOSCHANGED itself
    }
    if (SetWindowSubclass(root, root_move_subclass_proc, reinterpret_cast<UINT_PTR>(self),
                          reinterpret_cast<DWORD_PTR>(self)))
    {
        hooked_root_ = root;
    }
}

void AccessBridge::unhook_root()
{
    if (hooked_root_ && surface_ && IsWindow(hooked_root_))
    {
        RemoveWindowSubclass(hooked_root_, root_move_subclass_proc,
                             reinterpret_cast<UINT_PTR>(surface_->hwnd()));
    }
    hooked_root_ = nullptr;
}

void AccessBridge::schedule_window_moved_()
{
    if (move_pending_ || !surface_ || providers_.empty())
    {
        return;
    }
    move_pending_ = true;
    HWND hwnd = surface_->hwnd();
    surface_->host().post_delayed(kMoveSettleMs,
                                  [hwnd]
                                  {
                                      if (AccessBridge* b = bridge_for_hwnd(hwnd))
                                      {
                                          b->move_pending_ = false;
                                          b->on_window_moved();
                                      }
                                  });
}

void AccessBridge::schedule_refresh()
{
    if (providers_.empty() || refresh_pending_ || !surface_)
    {
        return;
    }
    refresh_pending_ = true;
    HWND hwnd = surface_->hwnd();
    surface_->host().post_delayed(kRefreshDelayMs,
                                  [hwnd]
                                  {
                                      if (AccessBridge* b = bridge_for_hwnd(hwnd))
                                      {
                                          b->refresh_pending_ = false;
                                          b->dirty_ = true;
                                          b->rebuild_if_dirty();
                                      }
                                  });
}

void AccessBridge::announce(const std::string& text, bool assertive)
{
    AccessNodeProvider* root = root_provider();
    if (!root)
    {
        return;
    }
    BSTR display = SysAllocString(utf8_to_wide(text).c_str());
    BSTR activity = SysAllocString(L"tesseract.announce");
    UiaRaiseNotificationEvent(root, NotificationKind_Other,
                              assertive ? NotificationProcessing_ImportantMostRecent
                                        : NotificationProcessing_MostRecent,
                              display, activity);
    SysFreeString(display);
    SysFreeString(activity);
}

} // namespace

namespace
{

// The COM object behind NativeEditAccessible. Callbacks are nulled by
// detach() when the owner goes away; a UIA client still holding a
// reference then gets safe defaults.
class NativeEditProvider final : public IRawElementProviderSimple, public IValueProvider
{
public:
    NativeEditProvider(HWND hwnd, NativeEditAccessible::Callbacks cb)
        : hwnd_(hwnd), cb_(std::move(cb))
    {
    }
    void detach()
    {
        live_ = false;
        cb_ = {};
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
        {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        }
        else if (riid == __uuidof(IValueProvider))
        {
            *ppv = static_cast<IValueProvider*>(this);
        }
        if (!*ppv)
        {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = --refs_;
        if (n == 0)
        {
            delete this;
        }
        return n;
    }

    // ---- IRawElementProviderSimple ----
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        // UseComThreading: UI Automation calls the provider on the window's STA
        // thread, so the tree walk and the row callbacks stay on the UI thread.
        *pRetVal = ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = nullptr;
        if (id == UIA_ValuePatternId && live_)
        {
            AddRef();
            *pRetVal = static_cast<IValueProvider*>(this);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        VariantInit(pRetVal);
        if (!live_)
        {
            return S_OK;
        }
        switch (id)
        {
        case UIA_ControlTypePropertyId:
            *pRetVal = i4_variant(UIA_EditControlTypeId);
            break;
        case UIA_NamePropertyId:
            *pRetVal = bstr_variant(cb_.name ? cb_.name() : std::string());
            break;
        case UIA_IsPasswordPropertyId:
            *pRetVal = bool_variant(is_password_());
            break;
        case UIA_IsKeyboardFocusablePropertyId:
        case UIA_IsContentElementPropertyId:
        case UIA_IsControlElementPropertyId:
            *pRetVal = bool_variant(true);
            break;
        default:
            break; // the HWND's own provider supplies focus, bounds, enabled
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        return UiaHostProviderFromHwnd(hwnd_, pRetVal);
    }

    // ---- IValueProvider ----
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR val) override
    {
        if (!live_ || !cb_.set_text)
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }
        cb_.set_text(wide_to_utf8(val ? std::wstring(val) : std::wstring()));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        // A password field's content is never exposed.
        const std::string t = (live_ && !is_password_() && cb_.text) ? cb_.text() : std::string();
        *pRetVal = SysAllocString(utf8_to_wide(t).c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* pRetVal) override
    {
        if (!pRetVal)
        {
            return E_POINTER;
        }
        *pRetVal = FALSE;
        return S_OK;
    }

private:
    bool is_password_() const
    {
        return cb_.password && cb_.password();
    }

    HWND hwnd_;
    NativeEditAccessible::Callbacks cb_;
    bool live_ = true;
    std::atomic<ULONG> refs_{1};
};

} // namespace

struct NativeEditAccessible::Impl
{
    HWND hwnd = nullptr;
    ComPtr<NativeEditProvider> provider;
};

NativeEditAccessible::NativeEditAccessible(HWND hwnd, Callbacks cb)
    : impl_(std::make_unique<Impl>())
{
    impl_->hwnd = hwnd;
    impl_->provider.Attach(new NativeEditProvider(hwnd, std::move(cb)));
}

NativeEditAccessible::~NativeEditAccessible()
{
    if (impl_->provider)
    {
        UiaDisconnectProvider(impl_->provider.Get());
        impl_->provider->detach();
    }
}

bool NativeEditAccessible::handle_get_object(WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    if (static_cast<long>(lParam) != UiaRootObjectId || !impl_->provider)
    {
        return false;
    }
    *result = UiaReturnRawElementProvider(impl_->hwnd, wParam, lParam, impl_->provider.Get());
    return true;
}

void NativeEditAccessible::name_changed(const std::string& old_name, const std::string& new_name)
{
    if (!impl_->provider || old_name == new_name || !UiaClientsAreListening())
    {
        return;
    }
    VARIANT oldv = bstr_variant(old_name);
    VARIANT newv = bstr_variant(new_name);
    UiaRaiseAutomationPropertyChangedEvent(impl_->provider.Get(), UIA_NamePropertyId, oldv, newv);
    VariantClear(&oldv);
    VariantClear(&newv);
}

void attach_accessible_bridge(Surface& surface)
{
    HWND hwnd = surface.hwnd();
    if (!hwnd)
    {
        return;
    }
    auto bridge = std::make_unique<AccessBridge>(&surface);
    AccessBridge* raw = bridge.get();
    bridge_registry().emplace(hwnd, std::move(bridge));
    surface.add_layout_listener(
        [raw]
        {
            raw->mark_dirty();
        });
    surface.host().add_paint_listener(
        [hwnd]
        {
            if (AccessBridge* b = bridge_for_hwnd(hwnd))
            {
                b->schedule_refresh();
            }
        });
}

void announce(HWND hwnd, const std::string& text, bool assertive)
{
    if (AccessBridge* b = bridge_for_hwnd(hwnd))
    {
        b->announce(text, assertive);
    }
}

void detach_accessible_bridge(HWND hwnd)
{
    auto& registry = bridge_registry();
    auto it = registry.find(hwnd);
    if (it == registry.end())
    {
        return;
    }
    // Erase from the registry before detach() runs, not after: detach()'s
    // UiaDisconnectProvider() call can pump a reentrant WM_GETOBJECT to this
    // still-live hwnd, and handle_get_object()/notify_focus_changed() must
    // find no bridge for hwnd at that point rather than reaching back into
    // the one being torn down (see AccessBridge::detach()'s doc comment).
    std::unique_ptr<AccessBridge> bridge = std::move(it->second);
    registry.erase(it);
    bridge->detach();
}

LRESULT handle_get_object(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    if (static_cast<long>(lParam) != UiaRootObjectId)
    {
        return DefWindowProcW(hwnd, WM_GETOBJECT, wParam, lParam);
    }

    AccessBridge* bridge = bridge_for_hwnd(hwnd);
    AccessNodeProvider* root = bridge ? bridge->root_provider() : nullptr;
    if (!root)
    {
        return DefWindowProcW(hwnd, WM_GETOBJECT, wParam, lParam);
    }

    return UiaReturnRawElementProvider(hwnd, wParam, lParam, root);
}

void notify_focus_changed(HWND hwnd, tk::Widget* /*old_widget*/, tk::Widget* now_widget)
{
    AccessBridge* bridge = bridge_for_hwnd(hwnd);
    if (!bridge || !now_widget)
    {
        return;
    }
    // Reusing AccessKey/provider_for from an internal (anonymous-namespace)
    // type outside that namespace is fine here since this function is
    // itself defined in this translation unit, after the anonymous
    // namespace closes — the names are still visible via unqualified
    // lookup within the enclosing tk::win32 namespace.
    if (auto* p = bridge->provider_for(AccessKey{now_widget, -1}))
    {
        UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId);
    }
}

} // namespace tk::win32
