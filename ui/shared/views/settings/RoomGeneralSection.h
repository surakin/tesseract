#pragma once

// The "General" tab of RoomSettingsView: room avatar, display name, and
// topic editing. The bespoke avatar-disc / inline field editing / busy-error
// rendering is kept in a private nested Content widget — mirrors
// AccountSection's Content split, just for a room's identity fields instead
// of the signed-in user's.

#include "SettingsPage.h"

#include "tk/canvas.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class RoomGeneralSection : public SettingsPage
{
public:
    RoomGeneralSection();
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

    // The topic field starts one line tall and grows with its content, like
    // the compose bar's text area; clamped to [kFieldH, kTopicMaxH] internally.
    void set_topic_area_natural_height(float h);

    // NativeTextField/NativeTextArea overlay rects (world space); empty when
    // that field's permission is denied or a commit is in flight.
    tk::Rect name_field_rect() const;
    tk::Rect topic_edit_rect() const;

    // Clears cached layouts and resets topic_natural_h_ back to one line.
    // Called by RoomSettingsView::open() on every open.
    void reset();

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;

    // Fired when the user clicks the Room ID row (see RoomSettingsView,
    // which copies it to the clipboard and shows a toast).
    std::function<void(std::string room_id)> on_room_id_clicked;

private:
    class Content;
    Content* content_ = nullptr;
};

} // namespace tesseract::views
