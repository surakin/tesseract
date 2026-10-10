#pragma once

// Shared helpers for the shared-view tests (a header, not per-file copies:
// the unity build merges every test file's anonymous namespace). Everything
// lives in namespace `vt`.

#include "tk/access_tree.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

namespace vt
{

// A PopupCapableStubHost plus an offscreen canvas for layout/paint passes.
struct Stage
{
    PopupCapableStubHost host;
    std::unique_ptr<TestSurface> surface;

    explicit Stage(int w = 800, int h = 600) : surface(TestSurface::create(w, h)) {}

    tk::LayoutCtx lc() { return tk::LayoutCtx{surface->factory(), tk::Theme::light()}; }
    tk::PaintCtx pc()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(), tk::Theme::light()};
    }
    void layout(tk::Widget& w, tk::Rect r)
    {
        auto l = lc();
        w.measure(l, {r.w, r.h});
        w.arrange(l, r);
    }
    void paint(tk::Widget& w)
    {
        auto p = pc();
        w.paint(p);
    }
    // Layout + paint, and make `w` the host's input root.
    void mount(tk::Widget& w, tk::Rect r)
    {
        host.set_root(&w);
        layout(w, r);
        paint(w);
    }
    void relayout_paint(tk::Widget& w, tk::Rect r)
    {
        layout(w, r);
        paint(w);
    }
};

inline tk::KeyEvent key(tk::Key k, bool ctrl = false, bool shift = false)
{
    tk::KeyEvent e;
    e.key = k;
    e.ctrl = ctrl;
    e.shift = shift;
    return e;
}

// First widget (depth-first, including `root`) of type T, or nullptr.
template <typename T>
T* find(tk::Widget& root)
{
    if (auto* t = dynamic_cast<T*>(&root))
        return t;
    for (const auto& c : root.children())
        if (T* f = find<T>(*c))
            return f;
    return nullptr;
}

template <typename T>
void collect(tk::Widget& root, std::vector<T*>& out)
{
    if (auto* t = dynamic_cast<T*>(&root))
        out.push_back(t);
    for (const auto& c : root.children())
        collect<T>(*c, out);
}

template <typename T>
std::vector<T*> find_all(tk::Widget& root)
{
    std::vector<T*> out;
    collect<T>(root, out);
    return out;
}

// First access node with this name (exact), or nullptr.
inline const tk::AccessNode* node_named(const tk::AccessNode& n, const std::string& name)
{
    if (n.name == name)
        return &n;
    for (const auto& c : n.children)
        if (const auto* f = node_named(c, name))
            return f;
    return nullptr;
}

inline void collect_nodes_named(const tk::AccessNode& n, const std::string& name,
                                std::vector<tk::AccessNode>& out)
{
    if (n.name == name)
        out.push_back(n);
    for (const auto& c : n.children)
        collect_nodes_named(c, name, out);
}

// Every access node with this name (copies; their activate closures stay valid).
inline std::vector<tk::AccessNode> nodes_named(tk::Widget& root, const std::string& name)
{
    std::vector<tk::AccessNode> out;
    auto tree = tk::build_access_tree(&root);
    collect_nodes_named(tree, name, out);
    return out;
}

inline const tk::AccessNode* node_role(const tk::AccessNode& n, tk::Role role)
{
    if (n.role == role)
        return &n;
    for (const auto& c : n.children)
        if (const auto* f = node_role(c, role))
            return f;
    return nullptr;
}

inline void collect_names(const tk::AccessNode& n, std::vector<std::string>& out)
{
    if (!n.name.empty())
        out.push_back(n.name);
    for (const auto& c : n.children)
        collect_names(c, out);
}

inline std::vector<std::string> all_names(tk::Widget& root)
{
    std::vector<std::string> out;
    collect_names(tk::build_access_tree(&root), out);
    return out;
}

inline bool has_name(tk::Widget& root, const std::string& name)
{
    auto tree = tk::build_access_tree(&root);
    return node_named(tree, name) != nullptr;
}

// Invoke the default action of the accessible node called `name`.
inline bool press(tk::Widget& root, const std::string& name)
{
    auto tree = tk::build_access_tree(&root);
    const tk::AccessNode* n = node_named(tree, name);
    return n && tk::invoke_default_action(*n);
}

// Left-click a widget through the real host pointer pipeline at its centre.
inline void click(Stage& s, tk::Widget& w)
{
    const tk::Rect b = w.bounds();
    const tk::Point p{b.x + b.w / 2, b.y + b.h / 2};
    s.host.dispatch_pointer_down(p);
    s.host.dispatch_pointer_up(p);
}

} // namespace vt
