#pragma once

// Settings panel section: signed-in user's profile (avatar + display name +
// Matrix ID) plus a Log Out button at the bottom. The bespoke avatar disc /
// inline name editing / busy & error rendering is kept verbatim in a private
// nested Content widget; this class is a SettingsPage that holds Content as
// a child and forwards its public API to it.

#include "SettingsPage.h"

#include "tk/canvas.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class AccountSection : public SettingsPage
{
public:
    AccountSection();
    ~AccountSection() override;

    // ----- Content ----------------------------------------------------------

    void set_display_name(std::string name);
    void set_user_id(std::string user_id);
    void set_avatar_url(std::string mxc_url);

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_image_provider(ImageProvider provider);

    // ----- Editing capabilities (gated by ServerInfo) -----------------------

    void set_editable(bool editable);
    void set_avatar_editable(bool editable);

    // ----- Busy / error state -----------------------------------------------

    void set_name_busy(bool busy);
    void set_name_error(std::string error);

    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    // ----- NativeTextField overlay rect -------------------------------------

    tk::Rect name_field_rect() const;

    // ----- Callbacks (wired by the shell) -----------------------------------

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;
    std::function<void()> on_logout;

private:
    class Content; // defined in AccountSection.cpp
    Content* content_ = nullptr;
};

} // namespace tesseract::views
