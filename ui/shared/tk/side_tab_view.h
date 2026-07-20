#pragma once

// Left-sidebar navigation panel. A fixed-width left column of labelled tab
// buttons sits beside a flex right content area that shows exactly one child
// widget at a time. The selected tab button is highlighted with
// palette.sidebar_selected; hover uses palette.sidebar_hover.
//
// Tabs may be added in two groups: regular tabs stack from the top of the
// sidebar, and "bottom" tabs added via add_bottom_tab() pin to the bottom of
// the sidebar with a separator line drawn above them. Bottom tabs must be
// added after all regular tabs.
//
// Usage:
//   auto tabs = std::make_unique<tk::SideTabView>();
//   tabs->add_tab("General", std::make_unique<MyGeneralWidget>());
//   tabs->add_tab("Privacy", std::make_unique<MyPrivacyWidget>());
//   tabs->add_bottom_tab("About", std::make_unique<MyAboutWidget>());
//   tabs->on_tab_selected = [](int idx) { … };
//   tabs->select(0);

#include "widget.h"

#include <tesseract/visual.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{

class SideTabView : public Widget
{
protected:
    SideTabView();
    TK_WIDGET_FACTORY_FRIEND(SideTabView)

public:
    ~SideTabView() override = default;

    // Append a tab. The content widget is owned by SideTabView. This may be
    // called at any time; the first tab added automatically becomes selected.
    void add_tab(std::string label, std::unique_ptr<Widget> content);

    // Append a tab that pins to the bottom of the sidebar. Bottom tabs are
    // drawn anchored to the bottom of the column with a separator line above
    // the group. Must be called after all add_tab() calls so that internal
    // indices remain stable (top tabs occupy [0..num_top), bottom tabs occupy
    // [num_top..size)).
    void add_bottom_tab(std::string label, std::unique_ptr<Widget> content);

    // Show/hide the tab at `idx` without removing it — its button and its
    // slot in the sidebar column disappear entirely (not just disabled).
    // Hiding the currently-selected tab moves selection to the first
    // remaining visible tab (top-to-bottom, top group before bottom group),
    // firing on_tab_selected. No-op if `idx` is out of range or already in
    // the requested state.
    void set_tab_visible(int idx, bool visible);

    // Whether the tab at `idx` currently has a visible slot/button. False for
    // an out-of-range `idx`.
    bool tab_visible(int idx) const;

    // Switch the visible content to the tab at `idx`. No-op if `idx` is out
    // of range. Fires `on_tab_selected` when the selection actually changes.
    void select(int idx);

    // Current selected index, or -1 when no tabs have been added.
    int selected_idx() const
    {
        return selected_idx_;
    }

    // Optional callback fired when the selection changes.
    std::function<void(int)> on_tab_selected;

    // Widget interface.
    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;

    // Scrolls the top tab group (regular tabs) when there isn't enough
    // vertical room for every tab. Bottom-pinned tabs never scroll. Only
    // reacts when `local.x` is over the sidebar column, so wheel events over
    // the content pane fall through to it untouched.
    bool on_wheel(Point local, float dx, float dy) override;

    // Test-only inspection of the scroll math.
    float scroll_y_for_testing() const { return scroll_y_; }

private:
    // Visual constants.
    static constexpr float kSidebarWidth = 200.0f;
    static constexpr float kTabHeight = 36.0f;
    static constexpr float kTabHPad = 14.0f;
    static constexpr float kTabVPad = 8.0f;
    static constexpr float kTabRadius = tesseract::visual::kRadiusSM;
    static constexpr float kTabInset = 6.0f; // horizontal inset inside column

    struct Tab
    {
        std::string label;
        std::unique_ptr<TextLayout> layout; // built lazily in measure/paint
        float layout_max_w = -2.0f;
        Widget* content = nullptr; // borrowed; owned via add_child
        bool hovered = false;
        bool bottom = false; // true when added via add_bottom_tab
        bool visible = true; // false skips this tab's slot entirely (set_tab_visible)
    };

    // Pointer hit-testing for the sidebar column.
    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    // Keyboard-focusable whenever there's more than one reachable tab.
    // Up/Down cycles the selection via select() (which already no-ops on an
    // unchanged index and fires on_tab_selected itself). Flat scan over
    // tabs_ in storage order — top tabs are always added before bottom
    // tabs, so storage order already matches top-to-bottom document order;
    // does not special-case the top/bottom visual grouping.
    bool focusable() const override
    {
        return num_visible_top_tabs_() + num_visible_bottom_tabs_() > 1;
    }
    // Gated on has_focus(): without it, this is reachable not only as the
    // genuinely tk-focused widget but also via Host::dispatch_key_down's
    // root-wide broadcast fallback (fired whenever the ACTUALLY focused
    // widget doesn't consume a key) — an unfocused side-tab column would
    // silently switch its selection on a stray Up/Down meant elsewhere.
    bool on_key_down(const KeyEvent& e) override
    {
        if (!has_focus()) return false;
        if (e.key != Key::Up && e.key != Key::Down) return false;
        const int n = static_cast<int>(tabs_.size());
        if (n == 0) return false;
        const int delta = (e.key == Key::Down) ? 1 : -1;
        int idx = selected_idx_;
        for (int step = 0; step < n; ++step)
        {
            idx = (idx < 0) ? (delta > 0 ? 0 : n - 1) : (idx + delta + n) % n;
            if (tabs_[idx].visible)
            {
                select(idx);
                return true;
            }
        }
        return false;
    }

    // Default paint_own_focus_ring() would ring bounds_ — the whole widget,
    // which spans both the sidebar column AND the flex content pane beside
    // it. Ring just the sidebar column instead (the same rect paint()
    // already fills as the sidebar background), inset slightly so the ring
    // doesn't sit flush against the sidebar/content separator line.
    void paint_own_focus_ring(PaintCtx& ctx) override
    {
        constexpr float kFocusRingInset = 2.0f;
        const Rect ring{bounds_.x + kFocusRingInset, bounds_.y + kFocusRingInset,
                        kSidebarWidth - kFocusRingInset * 2.0f,
                        bounds_.h - kFocusRingInset * 2.0f};
        paint_focus_ring(ctx, ring);
    }

    // Return the tab index whose button spans `local_y` (widget-local),
    // or -1 when outside any tab.
    int tab_at_y(float local_y) const;

    // Count of regular (top-stacked) tabs. Bottom tabs always sit after.
    int num_top_tabs_() const;
    int num_bottom_tabs_() const;
    // Same, but excluding hidden tabs — these are what layout/hit-test use.
    int num_visible_top_tabs_() const;
    int num_visible_bottom_tabs_() const;

    // Position of tab `idx` among visible tabs in its own group (top or
    // bottom), or -1 if `idx` is hidden.
    int visible_slot_(int idx) const;
    // Reverse of visible_slot_: the tab index at visible position `slot`
    // within the top (bottom=false) or bottom (bottom=true) group, or -1.
    int tab_at_visible_slot_(int slot, bool bottom) const;

    // Scrolls the top tab group (scroll_y_) so tab `idx`'s button is fully
    // visible, if it isn't already. No-op for a bottom-group tab (always
    // pinned/visible by design) or a hidden tab.
    void scroll_tab_into_view_(int idx);

    // Maximum scroll_y_ so the top group never scrolls past its last tab
    // (i.e. 0 when every tab already fits in the sidebar's height).
    float max_top_scroll_() const;

    // Ensure the TextLayout for tab `i` is built for the given max width.
    // Called from both arrange (LayoutCtx) and paint (PaintCtx) paths;
    // both expose `factory` under the same name so the implementation is
    // shared via this template.
    template <typename Ctx>
    void ensure_layout_(Ctx& ctx, int i, float max_w);

    std::vector<Tab> tabs_;
    int selected_idx_ = -1;
    int pressed_idx_ = -1; // index of button currently held down
    int hovered_idx_ = -1; // index of button under the pointer, or -1

    // Vertical scroll offset applied to the top tab group only, in pixels.
    // 0 = top. Clamped in on_wheel() via max_top_scroll_().
    float scroll_y_ = 0.0f;
};

} // namespace tk
