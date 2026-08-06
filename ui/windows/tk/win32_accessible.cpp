#include "win32_accessible.h"
#include "host_win32.h"
#include "access_tree.h"
#include "list_view.h"

#include <wrl/client.h>
#include <uiautomation.h>
#include <oleauto.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tk::win32
{

namespace
{

using Microsoft::WRL::ComPtr;

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

CONTROLTYPEID to_uia_control_type(tk::Role r)
{
    switch (r)
    {
    case tk::Role::Button:      return UIA_ButtonControlTypeId;
    case tk::Role::CheckBox:    return UIA_CheckBoxControlTypeId;
    case tk::Role::Switch:      return UIA_CheckBoxControlTypeId;
    case tk::Role::RadioButton: return UIA_RadioButtonControlTypeId;
    case tk::Role::ComboBox:    return UIA_ComboBoxControlTypeId;
    case tk::Role::TextInput:   return UIA_EditControlTypeId;
    case tk::Role::StaticText:  return UIA_TextControlTypeId;
    case tk::Role::Image:       return UIA_ImageControlTypeId;
    case tk::Role::Link:        return UIA_HyperlinkControlTypeId;
    case tk::Role::List:        return UIA_ListControlTypeId;
    case tk::Role::ListItem:    return UIA_ListItemControlTypeId;
    case tk::Role::Grid:        return UIA_DataGridControlTypeId;
    case tk::Role::GridCell:    return UIA_DataItemControlTypeId;
    case tk::Role::Tab:         return UIA_TabItemControlTypeId;
    case tk::Role::TabList:     return UIA_TabControlTypeId;
    case tk::Role::TabPanel:    return UIA_PaneControlTypeId;
    case tk::Role::Dialog:      return UIA_PaneControlTypeId;
    case tk::Role::MenuItem:    return UIA_MenuItemControlTypeId;
    case tk::Role::Group:       return UIA_GroupControlTypeId;
    case tk::Role::None:        return UIA_PaneControlTypeId;
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
    return r == tk::Role::ListItem || r == tk::Role::GridCell ||
          r == tk::Role::Tab || r == tk::Role::RadioButton;
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
    explicit AccessBridge(Surface* surface) : surface_(surface) {}

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
    }

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
            return nullptr;
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
            return rc;
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

    // Defined out-of-class (raises the event on a live provider, which
    // needs AccessNodeProvider's complete type).
    void notify_current_row(tk::Widget* owner, int idx);

    Surface* surface_;
    bool dirty_ = true;
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
                                 public IExpandCollapseProvider
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
            return E_POINTER;
        *ppv = nullptr;

        const tk::Role role = node() ? node()->role : tk::Role::None;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *ppv = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *ppv = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot) && bridge_ &&
                bridge_->is_root(key_))
            *ppv = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (riid == __uuidof(IInvokeProvider) && has_action_role(role))
            *ppv = static_cast<IInvokeProvider*>(this);
        else if (riid == __uuidof(IToggleProvider) && supports_toggle(role))
            *ppv = static_cast<IToggleProvider*>(this);
        else if (riid == __uuidof(ISelectionItemProvider) && supports_selection_item(role))
            *ppv = static_cast<ISelectionItemProvider*>(this);
        else if (riid == __uuidof(IExpandCollapseProvider) && supports_expand_collapse(role))
            *ppv = static_cast<IExpandCollapseProvider*>(this);

        if (!*ppv)
            return E_NOINTERFACE;
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
            delete this;
        return n;
    }

    // ---- IRawElementProviderSimple ----
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = nullptr;
        const tk::AccessNode* n = node();
        if (!n)
            return S_OK;
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
        default:
            break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        VariantInit(pRetVal);
        const tk::AccessNode* n = node();
        if (!n)
            return S_OK;
        // A real Widget node (row_index < 0) can read enabled/focus state
        // straight off its Widget; a synthesized row/cell has neither (see
        // AccessState's own doc comment — enabled/focused deliberately
        // aren't part of it) — default both to "not a concern" rather than
        // guessing, matching qt_accessible.cpp's NodeAccessible::state()
        // never setting a "disabled" bit for any node either.
        const bool is_real_widget = n->row_index < 0 && n->widget != nullptr;
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
            pRetVal->boolVal =
                (is_real_widget && !n->widget->enabled()) ? VARIANT_FALSE : VARIANT_TRUE;
            break;
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
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = nullptr;
        if (bridge_ && bridge_->is_root(key_))
            return UiaHostProviderFromHwnd(bridge_->hwnd(), pRetVal);
        return S_OK;
    }

    // ---- IRawElementProviderFragment ----
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment** pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = nullptr;
        if (!bridge_)
            return S_OK;

        switch (direction)
        {
        case NavigateDirection_Parent:
        {
            if (bridge_->is_root(key_))
                return S_OK;
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
                return S_OK;
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
                return S_OK;
            const auto& kids = parent->children;
            for (std::size_t i = 0; i < kids.size(); ++i)
            {
                if (!(key_for(kids[i]) == key_))
                    continue;
                std::size_t j;
                if (direction == NavigateDirection_NextSibling)
                {
                    if (i + 1 >= kids.size())
                        return S_OK;
                    j = i + 1;
                }
                else
                {
                    if (i == 0)
                        return S_OK;
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
            return E_POINTER;
        *pRetVal = nullptr;
        // Null = "this is the fragment root associated with the hwnd" —
        // required by the UIA contract for the element WM_GETOBJECT/
        // UiaReturnRawElementProvider returns for UiaRootObjectId.
        if (bridge_ && bridge_->is_root(key_))
            return S_OK;

        int rt_id[2] = {static_cast<int>(UiaAppendRuntimeId), runtime_id_};
        SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!sa)
            return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i)
            SafeArrayPutElement(sa, &i, &rt_id[i]);
        *pRetVal = sa;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = UiaRect{0, 0, 0, 0};
        const tk::AccessNode* n = node();
        if (!n || !bridge_)
            return S_OK;
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
            return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override
    {
        const tk::AccessNode* n = node();
        if (n && n->widget && bridge_ && bridge_->surface())
            bridge_->surface()->host().request_focus(n->widget);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = nullptr;
        if (!bridge_)
            return S_OK;
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
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                       IRawElementProviderFragment** pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        *pRetVal = nullptr;
        if (!bridge_)
            return S_OK;
        POINT screen{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
        POINT client = screen;
        if (!ScreenToClient(bridge_->hwnd(), &client))
            return S_OK;
        const float scale = bridge_->surface() ? bridge_->surface()->dpi_scale() : 1.f;
        const tk::Point world{client.x / scale, client.y / scale};
        const tk::AccessNode* root = bridge_->root_node();
        if (!root)
            return S_OK;
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
            return E_POINTER;
        *pRetVal = nullptr;
        if (!bridge_ || !bridge_->surface())
            return S_OK;
        tk::Widget* focused = bridge_->surface()->host().focused_widget();
        if (!focused)
            return S_OK;
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
            tk::invoke_default_action(*n);
        return S_OK;
    }

    // ---- IToggleProvider ----
    HRESULT STDMETHODCALLTYPE Toggle() override
    {
        if (const tk::AccessNode* n = node())
            tk::invoke_default_action(*n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        const tk::AccessNode* n = node();
        *pRetVal = (n && n->state.checked) ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // ---- ISelectionItemProvider ----
    HRESULT STDMETHODCALLTYPE Select() override
    {
        if (const tk::AccessNode* n = node())
            tk::invoke_default_action(*n);
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
            return E_POINTER;
        const tk::AccessNode* n = node();
        *pRetVal = (n && n->state.selected) ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
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
            tk::invoke_default_action(*n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Collapse() override
    {
        const tk::AccessNode* n = node();
        if (n && n->state.expanded)
            tk::invoke_default_action(*n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* pRetVal) override
    {
        if (!pRetVal)
            return E_POINTER;
        const tk::AccessNode* n = node();
        *pRetVal =
            (n && n->state.expanded) ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
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
                return hit_test(child, world);
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
        return;
    dirty_ = false;
    if (!surface_)
        return; // detached

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
        if (new_index.find(it->first) == new_index.end())
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

    // Coarse-grained "something changed" notification, same acceptable
    // staleness tradeoff qt_accessible.cpp documents for its own
    // ObjectReorder event (fired once per rebuild, not per individual
    // structural change).
    if (tree_.widget)
    {
        if (auto* rp = provider_for(key_for(tree_)))
            UiaRaiseStructureChangedEvent(rp, StructureChangeType_ChildrenReordered, nullptr, 0);
    }
}

void AccessBridge::notify_current_row(tk::Widget* owner, int idx)
{
    if (idx < 0)
        return; // deselected — nothing to report as "current"
    if (auto* p = provider_for(AccessKey{owner, idx}))
        UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId);
}

AccessNodeProvider* AccessBridge::provider_for(const AccessKey& key)
{
    rebuild_if_dirty();
    if (index_.find(key) == index_.end())
        return nullptr;

    auto it = providers_.find(key);
    if (it != providers_.end())
        return it->second.Get();

    ComPtr<AccessNodeProvider> p;
    p.Attach(new AccessNodeProvider(this, key, next_runtime_id_++));
    AccessNodeProvider* raw = p.Get();
    providers_.emplace(key, std::move(p));
    return raw;
}

AccessNodeProvider* AccessBridge::parent_provider_for(const AccessKey& key)
{
    rebuild_if_dirty();
    auto it = parent_of_.find(key);
    if (it == parent_of_.end())
        return nullptr;
    return provider_for(it->second);
}

AccessNodeProvider* AccessBridge::root_provider()
{
    rebuild_if_dirty();
    return tree_.widget ? provider_for(key_for(tree_)) : nullptr;
}

void AccessBridge::detach()
{
    for (auto& entry : providers_)
    {
        UiaDisconnectProvider(entry.second.Get());
        entry.second->detach();
    }
    providers_.clear();
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

} // namespace

void attach_accessible_bridge(Surface& surface)
{
    HWND hwnd = surface.hwnd();
    if (!hwnd)
        return;
    auto bridge = std::make_unique<AccessBridge>(&surface);
    AccessBridge* raw = bridge.get();
    bridge_registry().emplace(hwnd, std::move(bridge));
    surface.add_layout_listener([raw] { raw->mark_dirty(); });
}

void detach_accessible_bridge(HWND hwnd)
{
    auto& registry = bridge_registry();
    auto it = registry.find(hwnd);
    if (it == registry.end())
        return;
    it->second->detach();
    registry.erase(it);
}

LRESULT handle_get_object(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    if (static_cast<long>(lParam) != UiaRootObjectId)
        return DefWindowProcW(hwnd, WM_GETOBJECT, wParam, lParam);

    AccessBridge* bridge = bridge_for_hwnd(hwnd);
    AccessNodeProvider* root = bridge ? bridge->root_provider() : nullptr;
    if (!root)
        return DefWindowProcW(hwnd, WM_GETOBJECT, wParam, lParam);

    return UiaReturnRawElementProvider(hwnd, wParam, lParam, root);
}

void notify_focus_changed(HWND hwnd, tk::Widget* /*old_widget*/, tk::Widget* now_widget)
{
    AccessBridge* bridge = bridge_for_hwnd(hwnd);
    if (!bridge || !now_widget)
        return;
    // Reusing AccessKey/provider_for from an internal (anonymous-namespace)
    // type outside that namespace is fine here since this function is
    // itself defined in this translation unit, after the anonymous
    // namespace closes — the names are still visible via unqualified
    // lookup within the enclosing tk::win32 namespace.
    if (auto* p = bridge->provider_for(AccessKey{now_widget, -1}))
        UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId);
}

} // namespace tk::win32
