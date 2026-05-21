#pragma once

// Settings panel section: read-only display of the connected server. Shows a
// single "Homeserver" / URL row under a "Server" header. Purely informational;
// the inner HomeserverRow widget hides itself (zero height) when no server
// info has been provided yet.

#include "SettingsPage.h"

#include "tesseract/client.h"

namespace tesseract::views
{

class ServerSection : public SettingsPage
{
public:
    ServerSection();
    ~ServerSection() override;

    void set_server_info(const tesseract::ServerInfo& info);

    // Reports zero height before set_server_info() supplies a URL — the
    // page's own outer padding is suppressed in that state.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;

private:
    class HomeserverRow; // defined in ServerSection.cpp
    SettingsGroup* group_ = nullptr;
    HomeserverRow* row_ = nullptr;
};

} // namespace tesseract::views
