#pragma once

// Base class for one tab's content in SettingsView. A SettingsPage is a
// vertical flex container with the standard outer settings padding/spacing.
// Subclasses fill themselves in their constructor by calling
//   add_group("Header") -> SettingsGroup*    (a headered chunk), and/or
//   add_widget(...)                          (a bare child widget).
//
// SettingsPage also extends the base VBox layout with vertical scrolling: a
// tab's content may be taller than the viewport SideTabView gives it (e.g. a
// long device list, or a short window), so measure/arrange/paint/on_wheel are
// overridden to lay the VBox out at its natural height, shift it by
// scroll_y_, clip painting to the viewport, and draw a lightweight scrollbar
// thumb when content overflows. Subclasses that override arrange() for their
// own needs (popup-clip rects, etc.) must call SettingsPage::arrange() as
// their base call to keep this working.

#include "tk/layout.h"
#include "tk/widget.h"

#include <memory>
#include <string>

namespace tesseract::views
{

class SettingsGroup;

class SettingsPage : public tk::VBox, public tk::ScrollableRegion
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
        return add_child(std::move(w));
    }

    // tk::Widget overrides — see class comment above for the scroll model.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

    // tk::ScrollableRegion — lets a keyboard focus change on a descendant
    // (e.g. a checkbox near the bottom of a long page) scroll this page so
    // the descendant becomes visible. See Host::request_focus().
    void scroll_into_view(tk::Rect world_rect) override;

    // Test-only inspection of the scroll math.
    float content_height_for_testing() const { return content_height_; }
    float scroll_y_for_testing() const { return scroll_y_; }

protected:
    // Vertical scroll offset, in pixels. 0 = top. Clamped in arrange() once
    // the natural content height is known.
    float scroll_y_ = 0.0f;
    float content_height_ = 0.0f;
};

} // namespace tesseract::views
