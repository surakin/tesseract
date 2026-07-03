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
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>
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

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;

    // `centre`/`diameter` are in the owner's local coordinate space.
    void set_geometry(tk::Point centre, float diameter);

    void set_avatar_url(std::string mxc_url);
    void set_image_provider(ImageProvider provider);

    // When false, the disc is never clickable/hoverable and no "+"/"x"
    // affordances are drawn — just the plain avatar.
    void set_editable(bool editable);

    // While busy, hover/click are suppressed and an ellipsis overlay is
    // drawn instead. Clears any pending error.
    void set_busy(bool busy);

    // Inline error text drawn just below the disc. Cleared automatically
    // by set_busy(true).
    void set_error(std::string error);

    bool editable() const { return editable_; }
    bool busy() const { return busy_; }
    bool has_avatar() const { return !avatar_url_.empty(); }
    bool has_error() const { return !error_.empty(); }

    // `local` uses the owner's local coordinate space.
    HitZone hit_test(tk::Point local) const;

    // Returns true when the hover state changed (caller should repaint).
    // No-op (always returns false) when not editable.
    bool on_pointer_move(tk::Point local);
    void on_pointer_leave();

    // `world_origin` is the owner's paint-space origin (typically
    // `bounds_.x`/`bounds_.y`) added to the local geometry set via
    // set_geometry(). `initials_source` picks the fallback-initials text
    // when no avatar image is available (e.g. a display name or room name).
    void paint(tk::PaintCtx& ctx, tk::Point world_origin,
              std::string_view initials_source) const;

private:
    tk::Point centre_{};
    float diameter_ = 0.0f;

    std::string avatar_url_;
    ImageProvider image_provider_;

    bool editable_ = false;
    bool busy_     = false;
    bool hovered_  = false;
    std::string error_;
    mutable std::unique_ptr<tk::TextLayout> error_layout_;
};

} // namespace tesseract::views
