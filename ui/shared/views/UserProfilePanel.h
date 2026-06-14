#pragma once

#include <tesseract/client.h>
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class UserProfilePanel : public tk::Widget
{
public:
    static constexpr float kCardW = 240.0f;

    enum class DmButtonState { Normal, HasDM, Sending };

    void        set_dm_button_state(DmButtonState state);
    std::string dm_button_label() const;
    bool        dm_button_enabled() const;

    UserProfilePanel();
    ~UserProfilePanel() override = default;

    void open(std::string user_id, std::string display_name, std::string avatar_url);
    void close();
    bool is_open() const { return open_; }

    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    // Set extended profile fields (called async after open())
    void set_extended_profile(const tesseract::ExtendedProfile& profile);

    // Fired from open() so the shell can fetch extended fields async
    // and call set_extended_profile() when done
    std::function<void(std::string user_id)> on_extended_profile_requested;

    // Callbacks wired by shell
    std::function<void(std::string user_id)> on_open_dm;
    std::function<void(std::string user_id)> on_ignore;

    // Called on open() to determine the initial DM button state. Return true
    // if a DM room already exists for the given user_id (→ HasDM), false for
    // Normal. Leave unset to default to Normal.
    std::function<bool(const std::string& user_id)> on_check_has_dm;
    std::function<void()>                    on_close;
    std::function<void(std::string avatar_url, std::string display_name)>
                                             on_avatar_clicked;

    // Fires on open/close. RoomView routes this into the shared layout-
    // changed chain so the shells re-query rect accessors (specifically:
    // hide the compose textarea + room-search NativeTextField while the
    // panel covers the canvas).
    std::function<void()>                    on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

private:
    bool open_ = false;

    std::string user_id_;
    std::string display_name_;
    std::string avatar_url_;

    ImageProvider image_provider_;

    // Child widgets (borrowed pointers)
    tk::Button* close_btn_  = nullptr;
    tk::Button* dm_btn_     = nullptr;
    tk::Button* ignore_btn_ = nullptr;

    // Layout rects (world-space, updated each arrange)
    tk::Rect card_rect_{};
    tk::Rect backdrop_rect_{};
    tk::Rect avatar_rect_{};

    // Cached text layouts (rebuilt lazily in paint)
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> uid_layout_;

    // Extended profile (MSC4133) — set async after open()
    tesseract::ExtendedProfile ext_profile_;
    std::unique_ptr<tk::TextLayout> pronouns_label_layout_;
    std::unique_ptr<tk::TextLayout> pronouns_value_layout_;
    std::unique_ptr<tk::TextLayout> tz_label_layout_;
    std::unique_ptr<tk::TextLayout> tz_value_layout_;
    std::unique_ptr<tk::TextLayout> bio_label_layout_;
    std::unique_ptr<tk::TextLayout> bio_value_layout_;

    bool press_backdrop_ = false;
    bool press_avatar_   = false;

    static constexpr float kAvatarD    = 72.0f;
    static constexpr float kPadX       = 16.0f;
    static constexpr float kPadY       = 12.0f;
    static constexpr float kButtonH    = 36.0f;
    static constexpr float kHeaderH    = 40.0f;
};

} // namespace tesseract::views
