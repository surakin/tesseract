#pragma once

// Base class for one tab's content in SettingsView. A SettingsPage is a
// vertical flex container with the standard outer settings padding/spacing.
// Subclasses fill themselves in their constructor by calling
//   add_group("Header") -> SettingsGroup*    (a headered chunk), and/or
//   add_widget(...)                          (a bare child widget).
//
// SettingsPage is a tk::ScrollableBase: a tab's content may be taller than
// the viewport SideTabView gives it (e.g. a long device list, or a short
// window), so it lays its content out at natural height, shifted by
// scroll_y_, clips painting to the viewport, and draws/drags the shared
// scrollbar thumb when content overflows (ScrollableBase::paint_scrollbar()/
// scrollbar_on_pointer_down/drag/up — see dispatch_pointer_down() below for
// why the drag priority needs an explicit override here). The actual flex
// layout (padding/spacing/stacking) is delegated to an owned tk::VBox child
// (content_) rather than SettingsPage inheriting VBox itself — ScrollableBase
// already inherits tk::Widget, so multiply inheriting VBox too would
// double-inherit Widget. Subclasses that override arrange() for their own
// needs (popup-clip rects, etc.) must call SettingsPage::arrange() as their
// base call to keep this working.

#include "tk/layout.h"
#include "tk/scrollable_base.h"

#include <memory>
#include <string>

namespace tesseract::views
{

class SettingsGroup;

class SettingsPage : public tk::ScrollableBase
{
public:
    SettingsPage();

    // Append a headered group to the page. The returned pointer is borrowed;
    // ownership stays with the page. Use the result to add widgets to the group.
    SettingsGroup* add_group(std::string header);

    // Append a bare child widget to the page (no group header).
    // Mirrors tk::Widget::add_child but named for the settings-page API.
    template <typename W>
    W* add_widget(std::unique_ptr<W> w)
    {
        return content_->add_child(std::move(w));
    }

    // Forwarded to content_ (the owned VBox) — subclasses that need custom
    // outer padding/spacing (e.g. a page whose sole child should fill the
    // main axis) call these exactly as they would on a plain tk::VBox.
    SettingsPage& set_padding(tk::Edges e)
    {
        content_->set_padding(e);
        return *this;
    }
    SettingsPage& set_spacing(float v)
    {
        content_->set_spacing(v);
        return *this;
    }

    // tk::Widget overrides — see class comment above for the scroll model.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;
    void paint_after_children(tk::PaintCtx&) override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

    // content_ (the real VBox child holding every group/widget) spans the
    // full viewport width, including the scrollbar thumb's screen column —
    // Widget::dispatch_pointer_down recurses into children before falling
    // back to this widget's own on_pointer_down, so without this override a
    // group/row under the thumb could claim a thumb-drag click first. Checks
    // the thumb first (matching ListView's "thumb wins over content
    // underneath" priority), then falls back to ordinary recursion.
    tk::Widget* dispatch_pointer_down(tk::Point world) override;
    void        on_pointer_drag(tk::Point local) override;
    void        on_pointer_up(tk::Point local, bool inside_self) override;

    // Test-only inspection of the scroll math.
    float content_height_for_testing() const { return content_height_; }
    float scroll_y_for_testing() const { return scroll_y_; }

protected:
    float content_height() const override { return content_height_; }

private:
    // Owns every group/widget added via add_group()/add_widget(); handles
    // the actual flex layout (padding/spacing/stacking). SettingsPage itself
    // only manages scroll offset/clipping/the scrollbar around it.
    tk::VBox* content_ = nullptr;

    // Natural (unclamped-viewport) height of content_, recomputed each
    // arrange()/paint_before_children() re-layout.
    float content_height_ = 0.0f;
};

} // namespace tesseract::views
