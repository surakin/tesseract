#pragma once

// Settings panel section: read-only display of the connected server.
// Shows a single "Homeserver" label / URL row.  No interactivity — purely
// informational.  Returns height 0 (and skips paint) when no server info has
// been provided yet.

#include "tk/widget.h"
#include "tesseract/client.h"

#include <memory>
#include <string>

namespace tk
{
class TextLayout;
}

namespace tesseract::views
{

class ServerSection : public tk::Widget
{
public:
    ServerSection() = default;
    ~ServerSection() override = default;

    void set_server_info(const tesseract::ServerInfo& info);

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    std::string homeserver_url_;
    std::unique_ptr<tk::TextLayout> label_layout_;
    std::unique_ptr<tk::TextLayout> value_layout_;
};

} // namespace tesseract::views
