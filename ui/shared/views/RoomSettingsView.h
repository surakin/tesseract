#pragma once

// RoomSettingsView — a full-bounds widget that replaces the room header,
// timeline, and compose bar while open (see RoomView::arrange()/paint()'s
// early-return guard), letting the user edit a room's avatar, display name,
// and topic. Every field is editable immediately (no separate view/edit
// toggle like RoomInfoPanel's topic) but nothing is sent to the server
// until Accept is clicked — Cancel discards all staged edits.
//
// Per-field permission gating (set_field_permissions): a field the current
// user lacks power level for renders as plain static text instead of an
// editable control, mirroring AccountSection::Content's !name_editable_
// branch.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"
#include "views/AvatarEditControl.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace tesseract::views
{

// Pure diff logic, deliberately free of any widget/paint dependency so it
// is trivially unit-testable. Returns only the fields that actually
// changed; an unset optional means "leave this field alone" (no state
// event should be sent for it).
struct RoomSettingsChanges
{
    std::optional<std::string> name;
    std::optional<std::string> topic;
    std::optional<std::string> avatar_mxc;
};

RoomSettingsChanges compute_room_settings_changes(
    const std::string& original_name, const std::string& staged_name,
    const std::string& original_topic, const std::string& staged_topic,
    const std::string& original_avatar_mxc,
    const std::string& staged_avatar_mxc);

class RoomSettingsView : public tk::Widget
{
public:
    RoomSettingsView();
    ~RoomSettingsView() override = default;

    // Seeds original_*/staged_* from `info` (avatar from info.avatar_url —
    // the room's own state — NOT effective_avatar_url(), which falls back
    // to a DM counterpart's avatar the user doesn't own and shouldn't be
    // able to "clear").
    void open(const tesseract::RoomInfo& info);
    void close();
    bool is_open() const { return open_; }

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    // Independent per-field permission gating.
    void set_field_permissions(bool can_name, bool can_topic, bool can_avatar);

    // NativeTextField/NativeTextArea overlay rects (empty when that field's
    // permission is denied, the view is closed, or a commit is in flight).
    tk::Rect    name_field_rect() const;
    void        set_name_edit_text(std::string t);
    std::string name_edit_initial_text() const { return staged_name_; }
    tk::Rect    topic_edit_rect() const;
    void        set_topic_edit_text(std::string t);
    std::string topic_edit_initial_text() const { return staged_topic_; }

    // The topic field starts one line tall and grows with its content, like
    // the compose bar's text area. The shell wires the NativeTextArea's
    // set_on_height_changed to this (mirrors ComposeBar::set_text_area_
    // natural_height), reporting the control's natural (unclamped) content
    // height on every change; clamped to [kFieldH, kTopicMaxH] internally.
    void set_topic_area_natural_height(float h);

    // Avatar staging — the shell calls this once Client::upload_media
    // succeeds. Never touches server room state. "" clears the staged
    // avatar (shown via the AvatarEditControl "x" remove chip / upload
    // flow — see on_avatar_remove_clicked / on_avatar_upload_clicked).
    void set_staged_avatar(std::string mxc);
    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    // Called by the shell once the Accept commit resolves. ok=false keeps
    // the view open with `error` shown inline and the buttons re-enabled
    // (mirrors AccountSection's on_avatar_result/on_name_result
    // precedent); ok=true closes the view.
    void set_commit_result(bool ok, std::string error);

    // Fired when the view's own layout-affecting state changes (open/close,
    // permission changes affecting field rects) so the shell can relayout
    // native overlays.
    std::function<void()> on_layout_changed;
    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;
    std::function<void()> on_cancel;
    // Only the fields that actually changed are populated (see
    // compute_room_settings_changes); avatar_mxc == "" means clear.
    std::function<void(std::string room_id, RoomSettingsChanges changes)>
        on_accept;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

private:
    bool open_       = false;
    bool committing_ = false;

    std::string room_id_;
    std::string original_name_;
    std::string original_topic_;
    std::string original_avatar_mxc_;
    std::string staged_name_;
    std::string staged_topic_;
    std::string staged_avatar_mxc_;

    bool can_name_   = false;
    bool can_topic_  = false;
    bool can_avatar_ = false;

    AvatarEditControl avatar_;

    tk::Button* accept_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;

    std::string commit_error_;

    // World-space rects, recomputed each arrange().
    tk::Rect name_rect_{};
    tk::Rect topic_rect_{};

    std::unique_ptr<tk::TextLayout> title_layout_;
    std::unique_ptr<tk::TextLayout> name_label_layout_;
    std::unique_ptr<tk::TextLayout> name_static_layout_;
    std::unique_ptr<tk::TextLayout> topic_label_layout_;
    std::unique_ptr<tk::TextLayout> topic_static_layout_;
    std::unique_ptr<tk::TextLayout> commit_error_layout_;

    // Natural (unclamped) content height reported by the topic NativeTextArea;
    // reset to kFieldH (one line) on open().
    float topic_natural_h_ = kFieldH;

    static constexpr float kAvatarD      = 96.0f;
    static constexpr float kAvatarGap    = 24.0f; // avatar -> fields column
    static constexpr float kPadX         = 24.0f;
    static constexpr float kPadY         = 24.0f;
    static constexpr float kFieldGap     = 12.0f; // between one label+field group and the next
    static constexpr float kLabelGap     = 4.0f;  // label -> its own field
    static constexpr float kLabelH       = 16.0f;
    static constexpr float kFieldH       = 26.0f; // single-line row (underline sits at its base)
    static constexpr float kTopicMaxH    = 200.0f; // cap so topic can't swallow the whole view
    static constexpr float kBtnH         = 36.0f;
    static constexpr float kBtnGap       = 8.0f;
};

} // namespace tesseract::views
