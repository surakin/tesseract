#pragma once

// Settings panel section: signed-in user's profile (avatar + display name +
// Matrix ID) plus a Log Out button at the bottom. The bespoke avatar disc /
// inline name editing / busy & error rendering is kept verbatim in a private
// nested Content widget; this class is a SettingsPage that holds Content as
// a child and forwards its public API to it.

#include "SettingsPage.h"

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/text_field.h"
#include "tk/widget.h"

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
protected:
    // host() is nullable: when null (e.g. unit tests constructing the
    // section directly), the four editable fields are skipped — their
    // accessors stay null, and name_field_rect() (still test-observed
    // geometry) is unaffected since it's computed independently of the
    // field.
    AccountSection();
    TK_WIDGET_FACTORY_FRIEND(AccountSection)

public:
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

    // ----- Self-owned name field --------------------------------------------

    // Test-observed geometry (empty when not editable/busy) — kept even
    // though the field positions itself via this same computation.
    tk::Rect name_field_rect() const;

    // The self-owned display-name field, or null when constructed without
    // a Host. SettingsView::set_controller() wires its on_submit once the
    // SettingsController is available (not sooner — the field can't act on
    // a controller that doesn't exist yet).
    tk::TextField* name_field() const;

    // ----- Extended profile fields (MSC4133) --------------------------------

    void set_extended_profile(const tesseract::ExtendedProfile& profile);
    void set_profile_fields_editable(bool editable);

    // key is the MSC unstable key string (e.g. "io.fsky.nyx.pronouns")
    void set_profile_field_busy(const std::string& key, bool busy);
    void set_profile_field_error(const std::string& key, std::string error);

    // The three self-owned extended-profile fields, or null when
    // constructed without a Host. Wired the same way as name_field().
    tk::TextField* pronouns_field() const;
    tk::TextField* tz_field() const;
    tk::TextField* bio_field() const;

    // ----- Callbacks (wired by the shell) -----------------------------------

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;
    std::function<void()> on_logout;

private:
    class Content;         // defined in AccountSection.cpp
    class ExtendedFields;  // defined in AccountSection.cpp
    Content*        content_    = nullptr;
    ExtendedFields* ext_fields_ = nullptr;
};

} // namespace tesseract::views
