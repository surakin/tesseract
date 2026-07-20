#pragma once

// One row of "avatar + display name + Matrix ID". Reused by the sidebar
// user-strip on every platform and by every row of the multi-account picker.
// Avatar bytes resolve through an `image_provider` lambda the host wires up;
// missing or unfetched URLs fall back to the canvas's initials disc.

#include "tk/canvas.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class UserInfo : public tk::Widget
{
public:
    UserInfo();
    ~UserInfo() override = default;

    // ----- Content ----------------------------------------------------------

    void set_display_name(std::string name);
    void set_user_id(std::string user_id);    // shown under display_name
    void set_avatar_url(std::string mxc_url); // empty → initials fallback

    // Host hook: given an mxc:// URL, return a decoded image or null. The
    // shell typically reads a `tk_avatars_` cache and kicks off an async
    // fetch on miss; the UserInfo doesn't care which.
    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_image_provider(ImageProvider provider);

    // Visual marker on the right side. Used by AccountPicker to flag the
    // currently-active row.
    void set_active_indicator(bool on);
    bool active_indicator() const
    {
        return active_indicator_;
    }

    const std::string& display_name() const
    {
        return display_name_;
    }
    const std::string& user_id() const
    {
        return user_id_;
    }
    const std::string& avatar_url() const
    {
        return avatar_url_;
    }

    // ----- Callbacks --------------------------------------------------------

    /// Fires on pointer-up that lands back inside the row (matching the
    /// Button click convention). World coords are passed through so account-
    /// picker hosts can position child popovers relative to the row.
    std::function<void(tk::Point world)> on_primary;

    /// Right-click / secondary affordance. tk's Widget API doesn't
    /// differentiate left vs right pointer events natively, so the host
    /// dispatches this callback directly on its platform-native
    /// right-click signal: `if (info.on_secondary) info.on_secondary(p);`.
    std::function<void(tk::Point world)> on_secondary;

    // ----- tk::Widget overrides --------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    void invalidate_text(); // drop cached text layouts when content changes

    std::string display_name_;
    std::string user_id_;
    std::string avatar_url_;
    bool active_indicator_ = false;
    float avatar_size_ = 40.0f;
    ImageProvider image_provider_;

    // Cached layouts — rebuilt lazily inside paint() when the content or
    // surface factory changes.
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> uid_layout_;

    bool hovered_ = false;
    bool pressed_ = false;
};

} // namespace tesseract::views
