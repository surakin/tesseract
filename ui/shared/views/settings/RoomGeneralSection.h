#pragma once

// The "General" tab of RoomSettingsView: room avatar, display name, and
// topic editing. The bespoke avatar-disc / inline field editing / busy-error
// rendering is kept in a private nested Content widget — mirrors
// AccountSection's Content split, just for a room's identity fields instead
// of the signed-in user's.

#include "SettingsPage.h"

#include "tk/canvas.h"
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

    // The topic field starts one line tall and grows with its content, like
    // the compose bar's text area; clamped to [kFieldH, kTopicMaxH] internally.
    void set_topic_area_natural_height(float h);

    // Borrowed — owned via Content's add_child(). Null when constructed
    // without a Host. Positions/shows itself during arrange(); visibility
    // additionally requires can_name_ && !committing_.
    tk::TextField* name_field() const;

    // Borrowed — owned via Content's add_child(). Null when constructed
    // without a Host. Positions/shows itself during arrange(); visibility
    // additionally requires can_topic_ && !committing_. Auto-grows via
    // set_topic_area_natural_height() above, mirroring the compose bar.
    tk::TextArea* topic_field() const;

    // Clears cached layouts and resets topic_natural_h_ back to one line.
    // Called by RoomSettingsView::open() on every open.
    void reset();

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;

    // Fired when the user clicks the Room ID row (see RoomSettingsView,
    // which copies it to the clipboard and shows a toast).
    std::function<void(std::string room_id)> on_room_id_clicked;

    // Fired when the topic field's auto-grow height changes, deferred by
    // one UI-thread tick past the arrange() pass that triggered it (see
    // Content's constructor) — bubbles up to RoomSettingsView's own
    // on_layout_changed, mirroring security_/permissions_/image_packs_.
    std::function<void()> on_layout_changed;

private:
    class Content;
    Content* content_ = nullptr;
};

} // namespace tesseract::views
