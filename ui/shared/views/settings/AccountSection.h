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

namespace tesseract
{
struct ExtendedProfile;
} // namespace tesseract

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

    // ----- Extended profile fields (MSC4133) --------------------------------

    void set_extended_profile(const tesseract::ExtendedProfile& profile);
    void set_profile_fields_editable(bool editable);

    // key is the MSC unstable key string (e.g. "io.fsky.nyx.pronouns")
    void set_profile_field_busy(const std::string& key, bool busy);
    void set_profile_field_error(const std::string& key, std::string error);

    // Rect accessors for NativeTextField overlays (empty when not editable/busy)
    tk::Rect pronouns_field_rect() const;
    tk::Rect tz_field_rect() const;
    tk::Rect bio_field_rect() const;

    // ----- Callbacks (wired by the shell) -----------------------------------

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;
    std::function<void()> on_logout;

    // key = MSC unstable key string, value_json = serialised JSON or "null"
    std::function<void(std::string key, std::string value_json)>
        on_profile_field_changed;

private:
    class Content;         // defined in AccountSection.cpp
    class ExtendedFields;  // defined in AccountSection.cpp
    Content*        content_    = nullptr;
    ExtendedFields* ext_fields_ = nullptr;
};

} // namespace tesseract::views
