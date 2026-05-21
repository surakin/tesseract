#pragma once

// Base class for one tab's content in SettingsView. A SettingsPage is a
// vertical flex container with the standard outer settings padding/spacing.
// Subclasses fill themselves in their constructor by calling
//   add_group("Header") -> SettingsGroup*    (a headered chunk), and/or
//   add_widget(...)                          (a bare child widget).

#include "tk/layout.h"

#include <memory>
#include <string>

namespace tesseract::views
{

class SettingsGroup;

class SettingsPage : public tk::VBox
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
};

} // namespace tesseract::views
