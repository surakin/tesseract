#pragma once

// Settings panel section: read-only display of the connected server, in three
// groups — "Server" (URL, software, spec versions, upload limit, login
// methods), "Features" (the server-advertised features Tesseract uses, each
// Stable / Experimental / Not supported) and "Capabilities" (what the
// account may change). Purely informational; hidden (zero height) until
// set_server_info() supplies a homeserver URL.

#include "SettingsPage.h"

#include "tesseract/client.h"

#include <string>
#include <vector>

namespace tesseract::views
{

/// One label / value line. `dim` renders the value in the secondary text
/// colour (used for "Not supported" and similar).
struct ServerInfoRow
{
    std::string label;
    std::string value;
    bool dim = false;
};

struct ServerInfoRows
{
    std::vector<ServerInfoRow> server;
    std::vector<ServerInfoRow> features;
    std::vector<ServerInfoRow> capabilities;
};

/// Pure mapping from ServerInfo to the rows shown, so it can be tested
/// without painting. Rows whose data is unknown are omitted.
ServerInfoRows build_server_info_rows(const tesseract::ServerInfo& info);

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
    class InfoRow; // defined in ServerSection.cpp
    void fill_group_(SettingsGroup* group, const std::vector<ServerInfoRow>& rows);

    SettingsGroup* server_group_ = nullptr;
    SettingsGroup* features_group_ = nullptr;
    SettingsGroup* capabilities_group_ = nullptr;
};

} // namespace tesseract::views
