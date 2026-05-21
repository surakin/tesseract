#pragma once

// Settings panel section: "About" tab pinned to the bottom of the sidebar.
// Renders the standard brand splash (app icon, name, version) as the entire
// content — a stable home for future build metadata, license info, and links.

#include "SettingsPage.h"

namespace tesseract::views
{

class AboutSection : public SettingsPage
{
public:
    AboutSection();
    ~AboutSection() override = default;
};

} // namespace tesseract::views
