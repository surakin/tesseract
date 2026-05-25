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
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class RoomHeader : public tk::Widget
{
public:
    static constexpr float kHeight = 60.0f;

    RoomHeader();
    ~RoomHeader() override = default;

    void set_room(const tesseract::RoomInfo& info);
    void set_avatar_provider(
        std::function<const tk::Image*(const std::string& mxc_url)> provider);

    // When condensed, the header shows only the room topic at a reduced height.
    // Used by the multi-tab layout when a TabBar is visible above the header.
    void set_condensed(bool condensed);
    bool is_condensed() const
    {
        return condensed_;
    }

    // Show or hide the calendar / jump-to-date button. Hidden by default until
    // the shell confirms the homeserver supports MSC3030 via set_jump_to_date_enabled(true).
    void set_jump_to_date_enabled(bool enabled)
    {
        show_calendar_btn_ = enabled;
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    tk::Widget* dispatch_pointer_move(tk::Point world,
                                      bool* dirty = nullptr) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

    // Fired when a hyperlink in the room topic is clicked.
    std::function<void(const std::string& url)> on_link_clicked;

    // Fired when the user clicks the calendar/jump-to-date button.
    std::function<void()> on_jump_to_date_requested;

    // Fired when the user clicks the room name or avatar area (not on a topic
    // hyperlink or calendar button).
    std::function<void()> on_info_requested;

    // Tooltip for truncated topic text. on_show_tooltip fires when the pointer
    // enters a topic that did not fit in the available width; on_hide_tooltip
    // fires when the pointer leaves or the topic is not truncated.
    std::function<void(std::string text, tk::Rect anchor)> on_show_tooltip;
    std::function<void()> on_hide_tooltip;

private:
    // Draws a vector calendar icon centred in `button`, tinted with `tint`.
    void draw_calendar_icon(tk::Canvas& canvas, tk::Rect button,
                            tk::Color tint);
    // Draws a vector padlock icon in `rect` (10×12 logical px), tinted with `tint`.
    void draw_lock_icon(tk::Canvas& canvas, tk::Rect rect, tk::Color tint);

    bool condensed_ = false;
    bool show_calendar_btn_ = false;
    bool hover_calendar_ = false;
    bool press_calendar_ = false;
    tk::Rect calendar_btn_rect_{}; // updated each paint pass

    bool encrypted_ = false;
    tk::Rect lock_icon_rect_{};

    bool press_info_ = false; // true when header area (not calendar) is pressed
    bool hover_topic_ = false;
    bool topic_truncated_ = false;
    bool topic_dirty_ = true;
    float last_topic_w_ = -1.0f;
    float topic_natural_w_ = -1.0f;

    tk::Label* name_label_ = nullptr;
    tk::Label* topic_label_ = nullptr;

    std::string display_name_;
    std::string topic_;
    std::string topic_html_;
    std::vector<tk::TextSpan> topic_spans_;
    // First-line-only spans used for painting (equals topic_spans_ when
    // the topic is single-line; truncated at the first \n + "…" otherwise).
    std::vector<tk::TextSpan> topic_display_spans_;
    bool topic_multiline_ = false;
    tk::Rect topic_rect_{};
    std::string avatar_url_;

    std::unique_ptr<tk::TextLayout> topic_layout_;

    std::function<const tk::Image*(const std::string&)> avatar_provider_;
};

} // namespace tesseract::views
