#pragma once

// The "General" tab of RoomSettingsView: room avatar, display name, and
// topic editing, built from tk::HBox/VBox composition (an avatar cell beside
// a column of label+field rows) like every other settings section, instead
// of one widget doing its own manual position math. AvatarCell and
// TopicAreaCell are small private widgets bridging two things FlexBox can't
// measure on its own: AvatarEditControl (a plain geometry-agnostic helper,
// not a tk::Widget) and TextArea's auto-grow height (see TopicAreaCell's
// own comment). Everything else (TextField, Label) already has a working
// measure(), so it's plain FlexBox children.

#include "SettingsPage.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/text_area.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class RoomGeneralSection : public SettingsPage
{
protected:
    // host() is nullable: when null, the name field is simply not
    // constructed (name_field() stays nullptr) — lets tests that don't care
    // about the native field default-construct without a Host.
    RoomGeneralSection();
    TK_WIDGET_FACTORY_FRIEND(RoomGeneralSection)

public:
    ~RoomGeneralSection() override;

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    // Display copies kept in sync by RoomSettingsView whenever its own
    // staged_name_/staged_topic_ change; set_name also drives the avatar's
    // initials fallback and its own static-text cache.
    void set_name(std::string name);
    void set_topic(std::string topic);
    void set_avatar_url(std::string mxc);

    // Optimistic local preview of a just-picked, not-yet-uploaded avatar
    // image — see RoomSettingsView::set_staged_avatar_pending.
    void set_staged_avatar_preview(std::shared_ptr<tk::Image> image);

    // Read-only identity rows shown below the topic field — never editable,
    // regardless of the field-permission gating below (a room's canonical
    // alias/ID aren't set via this dialog). canonical_alias may be empty
    // (rendered as "—").
    void set_room_id(std::string room_id);
    void set_canonical_alias(std::string alias);

    void set_field_permissions(bool can_name, bool can_topic, bool can_avatar);
    void set_committing(bool committing);

    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    // Borrowed — owned via add_child(). Null when constructed without a
    // Host. Shown only while can_name_ && !committing_ (a static Label
    // takes its place otherwise).
    tk::TextField* name_field() const { return name_field_; }

    // Borrowed — owned via add_child(). Null when constructed without a
    // Host. Shown only while can_topic_ && !committing_. Auto-grows like
    // the compose bar's text area (see TopicAreaCell in the .cpp).
    tk::TextArea* topic_field() const;

    // Clears cached layouts and resets the topic's natural height back to
    // one line. Called by RoomSettingsView::open() on every open.
    void reset();

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;

    // Fired when the user clicks the Room ID row (see RoomSettingsView,
    // which copies it to the clipboard and shows a toast).
    std::function<void(std::string room_id)> on_room_id_clicked;

    // Fired whenever the name/topic field's visibility flips at runtime —
    // FlexBox skips invisible children entirely, so a widget that just
    // became visible has stale bounds until the next arrange() pass. Bubbles
    // up to RoomSettingsView's own on_layout_changed, mirroring
    // security_/permissions_/image_packs_. NOT fired for ordinary topic
    // auto-grow (TopicAreaCell's measure() picks that up on its own, no
    // native-overlay reposition needed beyond the normal relayout).
    std::function<void()> on_layout_changed;

    void on_theme_changed(const tk::Theme& t) override;

private:
    class AvatarCell;
    class NameFieldCell;
    class TopicAreaCell;
    class RoomIdRow;

    void refresh_name_display_();
    void refresh_topic_display_();
    void refresh_address_display_();
    void relayout_and_notify_();

    AvatarCell* avatar_cell_ = nullptr;

    tk::Label*     name_label_  = nullptr; // caption, always muted
    tk::TextField* name_field_  = nullptr; // editable
    tk::Label*     name_static_ = nullptr; // read-only display

    tk::Label*      topic_label_  = nullptr; // caption, always muted
    TopicAreaCell*  topic_cell_   = nullptr; // editable, auto-grows
    tk::Label*      topic_static_ = nullptr; // read-only display, wraps

    tk::Label* address_label_ = nullptr; // caption, always muted
    tk::Label* address_value_ = nullptr; // muted when empty, else primary

    RoomIdRow* roomid_row_ = nullptr;

    std::string staged_name_;
    std::string staged_topic_;
    std::string canonical_alias_;

    bool can_name_   = false;
    bool can_topic_  = false;
    bool can_avatar_ = false;
    bool committing_ = false;

    // Cached from the last on_theme_changed() so a content-only change
    // (e.g. set_canonical_alias between theme events) can still pick the
    // right colour immediately instead of waiting for the next theme push.
    tk::Color cached_muted_{0x80, 0x80, 0x80, 0xff};
    tk::Color cached_primary_{0, 0, 0, 0xff};
};

} // namespace tesseract::views
