#pragma once

// Shared room header bar. Renders a 60 px strip with the current room's
// circular avatar, display name, and topic — replacing the native QWidget /
// GtkBox / HWND header that every shell used to build independently.
//
// The avatar is drawn via draw_circle_image (or the initials fallback when
// the image is not yet in the provider's cache). Name and topic are child
// tk::Label widgets laid out in arrange().

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <string>

namespace tesseract::views {

class RoomHeader : public tk::Widget {
public:
    static constexpr float kHeight = 60.0f;

    RoomHeader();
    ~RoomHeader() override = default;

    void set_room(const tesseract::RoomInfo& info);
    void set_avatar_provider(
        std::function<const tk::Image*(const std::string& mxc_url)> provider);

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

    bool on_pointer_down(tk::Point local)                  override;
    void on_pointer_up  (tk::Point local, bool inside_self) override;
    void on_pointer_move(tk::Point local)                  override;
    void on_pointer_leave()                                override;

    // Fired when the user clicks the calendar/jump-to-date button.
    std::function<void()> on_jump_to_date_requested;

private:
    bool     hover_calendar_ = false;
    bool     press_calendar_ = false;
    tk::Rect calendar_btn_rect_{};  // updated each paint pass
    tk::Label* name_label_  = nullptr;
    tk::Label* topic_label_ = nullptr;

    std::string display_name_;
    std::string topic_;
    std::string avatar_url_;

    std::function<const tk::Image*(const std::string&)> avatar_provider_;
};

} // namespace tesseract::views
