#pragma once

// AvatarEditControl — the editable-avatar-disc logic shared by
// AccountSection (the user's own avatar) and RoomSettingsView (a room's
// avatar). Extracted from AccountSection::Content, which originally had
// this baked in with hardcoded geometry; here the disc's centre/diameter
// are parameters, so any owning widget can position it.
//
// Geometry (`set_geometry`) is in the OWNER WIDGET'S LOCAL coordinate
// space — the same space `on_pointer_down`/`on_pointer_move`'s `local`
// argument uses. `paint()` takes the owner's world-space origin (typically
// `bounds_.x/y`) to translate when drawing, since `tk::Canvas` draws in
// world space.
//
// The owner is responsible for: constructing/positioning this control in
// its own `arrange()`, forwarding `hit_test`/`on_pointer_move`/
// `on_pointer_leave` from its own pointer overrides, and calling `paint()`
// from its own `paint()`.

#include "tk/canvas.h"
#include "tk/access_tree.h"
#include "tk/row_cursor.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <string_view>

namespace tesseract::views
{

class AvatarEditControl
{
public:
    enum class HitZone
    {
        None,
        Disc,
        RemoveChip,
    };

    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;

    // `centre`/`diameter` are in the owner's local coordinate space.
    void set_geometry(tk::Point centre, float diameter);

    void set_avatar_url(std::string mxc_url);
    void set_image_provider(ImageProvider provider);

    // Optimistic local decode of a just-picked (not-yet-resolvable-via-mxc)
    // image, preferred over image_provider_(avatar_url_) in paint() when
    // non-null. Cleared automatically by set_avatar_url("") (the shared
    // "removed" signal for both RoomSettingsView and AccountSection).
    void set_local_preview(std::shared_ptr<tk::Image> image);

    // When false, the disc is never clickable/hoverable and no "+"/"x"
    // affordances are drawn — just the plain avatar.
    void set_editable(bool editable);

    // While busy, hover/click are suppressed and an ellipsis overlay is
    // drawn instead. Clears any pending error.
    void set_busy(bool busy);

    // Inline error text drawn just below the disc. Cleared automatically
    // by set_busy(true).
    void set_error(std::string error);

    bool editable() const
    {
        return editable_;
    }
    bool busy() const
    {
        return busy_;
    }
    bool has_avatar() const
    {
        return !avatar_url_.empty() || local_preview_ != nullptr;
    }
    bool has_error() const
    {
        return !error_.empty();
    }

    // `local` uses the owner's local coordinate space.
    HitZone hit_test(tk::Point local) const;

    // What an owning widget exposes to assistive technology on this
    // control's behalf (it isn't a tk::Widget itself): "Change avatar" and,
    // when there is one, "Remove avatar" while editable, then the error
    // text. Rects are world-space given the owner's world origin.
    struct AccessItem
    {
        enum class Kind
        {
            Change,
            Remove,
            Error,
        };
        Kind kind;
        std::string name;
        tk::Rect rect;
    };
    std::vector<AccessItem> access_items(tk::Point world_origin) const;

    // Returns true when the hover state changed (caller should repaint).
    // No-op (always returns false) when not editable.
    bool on_pointer_move(tk::Point local);
    void on_pointer_leave();

    // `world_origin` is the owner's paint-space origin (typically
    // `bounds_.x`/`bounds_.y`) added to the local geometry set via
    // set_geometry(). `initials_source` picks the fallback-initials text
    // when no avatar image is available (e.g. a display name or room name).
    void paint(tk::PaintCtx& ctx, tk::Point world_origin, std::string_view initials_source) const;

private:
    tk::Point centre_{};
    float diameter_ = 0.0f;

    std::string avatar_url_;
    ImageProvider image_provider_;
    std::shared_ptr<tk::Image> local_preview_;

    bool editable_ = false;
    bool busy_ = false;
    bool hovered_ = false;
    std::string error_;
    mutable tk::CachedTextLayout error_layout_;
};

// Mixin for a widget hosting an AvatarEditControl: exposes the control's
// access_items() as rows and routes activation back through the owner's
// existing upload / remove callbacks.
class AvatarAccessRows : public tk::WidgetRowAccessibility
{
public:
    std::size_t access_row_count() const override
    {
        items_ = avatar_control_().access_items(avatar_world_origin_());
        return items_.size();
    }
    tk::Role access_role_for_widget_row(std::size_t i) const override
    {
        if (i >= items_.size())
        {
            return tk::Role::None;
        }
        return items_[i].kind == AvatarEditControl::AccessItem::Kind::Error ? tk::Role::StaticText
                                                                            : tk::Role::Button;
    }
    std::string access_name_for_widget_row(std::size_t i) const override
    {
        return i < items_.size() ? items_[i].name : std::string();
    }
    tk::Rect access_rect_for_widget_row(std::size_t i) const override
    {
        return i < items_.size() ? items_[i].rect : tk::Rect{};
    }
    bool access_activate_widget_row(std::size_t i) override
    {
        if (i >= items_.size() || items_[i].kind == AvatarEditControl::AccessItem::Kind::Error)
        {
            return false;
        }
        avatar_activate_(items_[i].kind == AvatarEditControl::AccessItem::Kind::Remove);
        return true;
    }

protected:
    // Keyboard operation of the same rows (Change / Remove avatar) — the
    // owning widget forwards its focus/key/ring hooks here (see
    // AVATAR_ACCESS_ROWS_KEYBOARD below).
    tk::RowKeyboardCursor avatar_kbd_;

    virtual const AvatarEditControl& avatar_control_() const = 0;
    virtual tk::Point avatar_world_origin_() const = 0;
    // remove == false: upload / change.
    virtual void avatar_activate_(bool remove) = 0;

private:
    mutable std::vector<AvatarEditControl::AccessItem> items_;
};

// Drop into the public section of a tk::Widget that also derives from
// AvatarAccessRows: makes the editable avatar a Tab stop whose arrow keys
// pick Change / Remove and Enter runs it. Clicks keep their behaviour.
#define AVATAR_ACCESS_ROWS_KEYBOARD                                                                \
    bool focusable() const override                                                                \
    {                                                                                              \
        return enabled() && avatar_control_().editable() && !avatar_control_().busy();             \
    }                                                                                              \
    bool focus_on_click() const override                                                           \
    {                                                                                              \
        return false;                                                                              \
    }                                                                                              \
    bool on_key_down(const tk::KeyEvent& e) override                                               \
    {                                                                                              \
        return avatar_kbd_.handle_key(*this, *this, e);                                            \
    }                                                                                              \
    void paint_own_focus_ring(tk::PaintCtx& ctx) override                                          \
    {                                                                                              \
        avatar_kbd_.paint_ring(ctx, *this, *this);                                                 \
    }                                                                                              \
    void on_focus_lost() override                                                                  \
    {                                                                                              \
        avatar_kbd_.reset();                                                                       \
    }

} // namespace tesseract::views
