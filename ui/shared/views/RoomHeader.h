#pragma once

// Shared room header bar. Renders a 60 px strip with the current room's
// circular avatar, display name, and topic — replacing the native QWidget /
// GtkBox / HWND header that every shell used to build independently.
//
// The avatar is drawn via draw_circle_image (or the initials fallback when
// the image is not yet in the provider's cache). Name and topic are child
// tk::Label widgets laid out in arrange().

#include "DatePickerView.h"
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <cstdint>
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

    // Show or hide the threads button. Hidden by default; the shell flips this
    // on once the SDK reports that the room contains at least one thread.
    void set_show_threads_btn(bool show)
    {
        show_threads_btn_ = show;
    }
    bool show_threads_btn() const
    {
        return show_threads_btn_;
    }

    // Show or hide the search button. Hidden by default; the shell enables it
    // when the room supports full-text search.
    void set_show_search_btn(bool show) { show_search_btn_ = show; }
    bool show_search_btn() const { return show_search_btn_; }

#ifdef TESSERACT_CALLS_ENABLED
    // Show or hide the start-call button. Hidden by default; shown when the
    // room is a DM or the server advertises MatrixRTC support.
    void set_show_call_btn(bool show) { show_call_btn_ = show; }
    bool show_call_btn() const { return show_call_btn_; }

    // Toggle the active-call indicator style on the call button.
    void set_call_active(bool active) { call_active_ = active; }

    // Fired when the user presses the call button, passing the button's
    // world-space rect so the owner can anchor a popup to it.
    std::function<void(tk::Rect)> on_call_requested;
#endif

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    void paint_overlay(tk::PaintCtx&) override;
    void on_popup_dismiss() override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    tk::Widget* dispatch_pointer_move(tk::Point world,
                                      bool* dirty = nullptr) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

    // Fired when a hyperlink in the room topic is clicked.
    std::function<void(const std::string& url)> on_link_clicked;
    // Fired when the pointer enters or leaves a hyperlink in the topic.
    // Passes the URL while hovering, empty string when leaving.
    std::function<void(const std::string& url)> on_link_hovered;

    // Fired when the user confirms a date in the picker (ms since Unix epoch,
    // midnight UTC on the selected day). Replaces on_jump_to_date_requested.
    std::function<void(std::uint64_t ts_ms)> on_date_jump;

    // Fired when the user clicks the threads button.
    std::function<void()> on_threads_requested;

    // Fired when the user clicks the search button.
    std::function<void()> on_search_requested;

    // Fired when the user clicks the room name or avatar area (not on a topic
    // hyperlink or calendar button).
    std::function<void()> on_info_requested;

private:
    // Draws a vector padlock icon in `rect` (10×12 logical px), tinted with `tint`.
    void draw_lock_icon(tk::Canvas& canvas, tk::Rect rect, tk::Color tint);

    // Cached from paint() so on_pointer_move/on_pointer_leave (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip for the
    // truncated-topic tooltip.
    tk::Host* host_ = nullptr;

    bool condensed_ = false;
    bool show_calendar_btn_ = false;
    bool show_threads_btn_ = false;
    bool show_search_btn_ = false;
#ifdef TESSERACT_CALLS_ENABLED
    bool show_call_btn_ = false;
    bool call_active_   = false;
#endif

    // Calendar / threads / search / call action buttons. Variant::Icon child
    // buttons own their hover/press background and click dispatch; this view
    // only draws the vector glyph centred inside each button's bounds.
    tk::Button* calendar_btn_ = nullptr;
    tk::Button* threads_btn_ = nullptr;
    tk::Button* search_btn_ = nullptr;
#ifdef TESSERACT_CALLS_ENABLED
    tk::Button* call_btn_ = nullptr;
#endif

    // Owned date-picker popup (not a widget-tree child — driven via
    // register_popup / paint_overlay).
    std::unique_ptr<DatePickerView> date_picker_;
    bool date_picker_visible_ = false;

    // Helpers.
    void show_date_picker_();
    void hide_date_picker_();
    static std::uint64_t date_to_midnight_utc_ms_(int year, int month, int day);

    // Lucide icons for the action buttons, tinted text_primary.
    tk::IconCache calendar_icon_;
    tk::IconCache threads_icon_;
    tk::IconCache search_icon_;
#ifdef TESSERACT_CALLS_ENABLED
    tk::IconCache call_icon_;
#endif

public:
    // Test-only accessor for the threads button's world-coordinate rect.
    tk::Rect threads_btn_rect_for_test() const
    {
        return threads_btn_ ? threads_btn_->bounds() : tk::Rect{};
    }

    // Test-only accessor for the search button's world-coordinate rect.
    tk::Rect search_btn_rect_for_test() const
    {
        return search_btn_ ? search_btn_->bounds() : tk::Rect{};
    }

private:

    bool encrypted_ = false;
    tk::Rect lock_icon_rect_{};

    bool press_info_ = false; // true when header area (not calendar) is pressed
    bool hover_topic_ = false;
    std::string hover_link_url_; // non-empty while pointer is over a topic link
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
