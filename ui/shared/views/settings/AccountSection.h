#pragma once

// Settings panel section: read-only display of the signed-in account.
// Shows a 64 px avatar disc, the display name in a heavier font, and the
// Matrix ID in muted colour below it. No interactivity — purely informational.

#include "tk/canvas.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class AccountSection : public tk::Widget
{
public:
    AccountSection();
    ~AccountSection() override = default;

    // ----- Content ----------------------------------------------------------

    void set_display_name(std::string name);
    void set_user_id(std::string user_id);
    void set_avatar_url(std::string mxc_url);

    // Given an mxc:// URL, return a decoded image or null. The shell wires
    // this up to the same avatar cache it uses for UserInfo rows.
    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_image_provider(ImageProvider provider);

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    void invalidate_text();

    std::string display_name_;
    std::string user_id_;
    std::string avatar_url_;
    ImageProvider image_provider_;

    // Cached layouts rebuilt lazily in paint() when content changes.
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> uid_layout_;
};

} // namespace tesseract::views
