#pragma once

// AddRoomView — the combined Join/Create "Add Room" modal overlay. Owns a
// JoinRoomView, a CreateRoomView, and a tk::TabView (the Join/Create
// segmented header) as children. Draws the shared modal backdrop and
// centred card itself; the header is a real tk::TabView child, not
// hand-painted, so it participates in normal pointer/keyboard dispatch
// (Tab-focusable, Left/Right switches tabs, proper focus ring) for free.
// Each content child renders only its own area below the header.
//
// Mounted once as the topmost child of MainAppWidget (in overlay_stack_) —
// set_visible(false) by default, arranged at full bounds, painted last
// (highest z-order). Replaces the old per-platform native Join Room
// dialog/sheet/window entirely: no shell constructs its own window for
// either flow anymore.

#include "CreateRoomView.h"
#include "JoinRoomView.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/tab_view.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class AddRoomView : public tk::Widget
{
protected:
    AddRoomView();
    TK_WIDGET_FACTORY_FRIEND(AddRoomView)

public:
    ~AddRoomView() override = default;

    enum class Tab
    {
        Join,
        Create,
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────
    void open(Tab initial_tab = Tab::Join);
    // Opens directly on the Join tab, prefilled with an alias/room ID —
    // used when following a matrix: link to a room the user isn't in yet.
    void open_join_with_prefill(const std::string& prefill);
    void close();
    bool is_open() const
    {
        return is_open_;
    }

    // Switches the active tab without closing the dialog (header click).
    void set_active_tab(Tab t);
    Tab active_tab() const
    {
        return active_tab_;
    }

    JoinRoomView* join_view() const
    {
        return join_view_;
    }
    CreateRoomView* create_view() const
    {
        return create_view_;
    }

    // Hiding (close()) doesn't cascade to the children's native fields —
    // tk::Widget::set_visible is deliberately non-virtual/non-cascading —
    // so this shadow hides them explicitly, mirroring ForwardRoomPicker's
    // idiom.
    void set_visible(bool v);

    // Fired when the overlay should be dismissed (Cancel, Escape, outside click).
    std::function<void()> on_close;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

    static constexpr float kCardW = 440.0f;
    static constexpr float kCardH = 460.0f;
    static constexpr float kHeaderH = 48.0f;

private:
    Tab active_tab_ = Tab::Join;
    bool is_open_ = false;
    // Set by open()/set_active_tab(); consumed by the next paint() — the
    // active child's native field overlay isn't positioned until arrange()
    // runs, mirroring JoinRoomView::pending_focus_'s identical rationale.
    bool pending_focus_ = false;

    JoinRoomView* join_view_ = nullptr;
    CreateRoomView* create_view_ = nullptr;
    tk::TabView* tab_view_ = nullptr;

    tk::Rect card_rect_{};

    bool press_outside_ = false;
};

} // namespace tesseract::views
