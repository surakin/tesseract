#pragma once

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

    UserProfilePanel();
    ~UserProfilePanel() override = default;

    void open(std::string user_id, std::string display_name, std::string avatar_url);
    void close();
    bool is_open() const { return open_; }

    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    // Callbacks wired by shell
    std::function<void(std::string user_id)> on_open_dm;
    std::function<void(std::string user_id)> on_ignore;
    std::function<void()>                    on_close;

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

    bool press_backdrop_ = false;

    static constexpr float kAvatarD    = 72.0f;
    static constexpr float kPadX       = 16.0f;
    static constexpr float kPadY       = 12.0f;
    static constexpr float kButtonH    = 36.0f;
    static constexpr float kHeaderH    = 40.0f;
};

} // namespace tesseract::views
