#pragma once

// Settings panel section: "About" tab pinned to the bottom of the sidebar.
// Renders the standard brand splash (app icon, name, version) as the upper
// fill, then the read-only server info (Server / Features / Capabilities
// groups, hidden until the homeserver details are known) and a right-aligned
// "Advanced" button that reveals the hidden Advanced tab.

#include "SettingsPage.h"

#include "tesseract/client.h"

#include <functional>

namespace tk
{
class Widget;
} // namespace tk

namespace tesseract::views
{

class ServerInfoPanel;

class AboutSection : public SettingsPage
{
public:
    AboutSection();
    ~AboutSection() override = default;

    // Fill the Server / Features / Capabilities groups from the connected
    // homeserver's details. An empty homeserver URL hides them again.
    void set_server_info(const tesseract::ServerInfo& info);

    // Fired when the user presses "Advanced". SettingsView wires this to
    // reveal and navigate to the hidden Advanced tab.
    std::function<void()> on_advanced_clicked;

    // Hide the "Advanced" button — it acts on a signed-in account. Used while
    // Settings is open from the login screen.
    void set_account_actions_visible(bool visible);

private:
    class AboutBrand;
    AboutBrand* brand_ = nullptr;            // borrowed
    ServerInfoPanel* server_info_ = nullptr; // borrowed
    tk::Widget* advanced_row_ = nullptr;     // borrowed
};

} // namespace tesseract::views
