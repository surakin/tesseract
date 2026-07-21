#pragma once

// tk::TabView — a fixed-size horizontal N-segment tab header, styled like an
// actual tab strip rather than a segmented button group: no filled/bordered
// pill per segment, just an accent-coloured underline beneath the active
// label (Material/GitHub-style), so it reads as "which page am I on," not
// "which button did I press." Pure "N labeled segments, pick one": no owned
// content widgets (unlike SideTabView), no string-keyed identity (unlike
// TabBar, which is keyed by room_id for the open-room strip). All segments
// are equal width, sized from the widest label and stretched to fill the
// arranged bounds.
//
// Usage:
//   auto tv = tk::create_widget<tk::TabView>(this);
//   tv->set_items({tk::tr("Join"), tk::tr("Create")});
//   tv->on_selected = [this](int idx) { ... };
//   tab_view_ = add_child(std::move(tv));

#include "widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{

class TabView : public Widget
{
protected:
    TabView() = default;
    TK_WIDGET_FACTORY_FRIEND(TabView)

public:
    ~TabView() override = default;

    // Replaces the full segment set. Resets selected_idx_ to 0 (clamped
    // into range). Does NOT fire on_selected — a bulk content reset, not a
    // user action, mirrors ComboBox::set_options.
    void set_items(std::vector<std::string> labels);

    // Single commit path — pointer-up and the arrow-key handler both funnel
    // through this. No-op (does not fire on_selected) if idx is out of
    // range or equal to the current selection, mirrors
    // SideTabView::select()'s "unchanged is silent" convention.
    void set_selected_index(int idx);
    int selected_index() const
    {
        return selected_idx_;
    }

    // Fires whenever the selection changes, from any source (pointer,
    // keyboard, or a direct set_selected_index() call).
    std::function<void(int)> on_selected;

    Size measure(LayoutCtx&, Size constraints) override;
    void arrange(LayoutCtx&, Rect bounds) override;
    void paint(PaintCtx&) override;

    bool on_pointer_down(Point local) override;
    void on_pointer_up(Point local, bool inside_self) override;
    bool on_pointer_move(Point local) override;
    void on_pointer_leave() override;

    // Requires more than one item — a transiently empty/single-item
    // TabView has nothing to Tab-stop for (matches TabBar/SideTabView's
    // convention over AppearanceSection::ThemePicker's unconditional
    // `true`, since ThemePicker's 3 options are a compile-time-fixed
    // array while TabView's item count is caller-supplied).
    bool focusable() const override
    {
        return static_cast<int>(items_.size()) > 1;
    }

    // Gated on has_focus() first — mandatory: without it an unfocused
    // TabView could react to a stray Left/Right via
    // Host::dispatch_key_down's root-wide fallback broadcast. Left/Right
    // only (horizontal layout); wraps around and commits immediately via
    // set_selected_index(), mirroring AppearanceSection::ThemePicker's
    // arrow-key handling.
    bool on_key_down(const KeyEvent& e) override;

    // Rings just the segment span, not the widget's full bounds() — this
    // is a small header strip, potentially inside a much larger container
    // (e.g. AddRoomView's card), matching SideTabView/ThemePicker's
    // identical paint_own_focus_ring override rationale.
    void paint_own_focus_ring(PaintCtx& ctx) override;

private:
    static constexpr float kGap = 8.0f;
    static constexpr float kMinH = 32.0f;
    static constexpr float kHPad = 16.0f;
    static constexpr float kUnderlineH = 3.0f; // active-tab indicator thickness
    static constexpr float kFocusRingInset = 4.0f;

    struct Item
    {
        std::string label;
        Rect bounds{};
        std::unique_ptr<TextLayout> layout;
    };

    std::vector<Item> items_;
    int selected_idx_ = 0;
    int hovered_idx_ = -1;
    int pressed_idx_ = -1;

    int hit_test_(Point local) const;
};

} // namespace tk
