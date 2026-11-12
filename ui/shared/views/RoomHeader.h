#pragma once

// Shared room header bar. Renders a 60 px strip with the current room's
// circular avatar, display name, and topic — replacing the native QWidget /
// GtkBox / HWND header that every shell used to build independently.
//
// The avatar is drawn via draw_circle_image (or the initials fallback when
// the image is not yet in the provider's cache). Name and topic are child
// tk::Label widgets laid out in arrange().

#include "DatePickerView.h"
#include "PopupMenu.h"
#include "tk/access_tree.h"
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/keyboard_target.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class RoomHeader : public tk::Widget, public tk::WidgetRowAccessibility
{
public:
    static constexpr float kHeight = 60.0f;

    // The topic layout is rebuilt only when dirty or the width changes.
    void on_fonts_changed() override
    {
        topic_dirty_ = true;
        topic_natural_w_ = -1.0f;
    }

    RoomHeader();
    ~RoomHeader() override = default;

    void set_room(const tesseract::RoomInfo& info);
    void set_avatar_provider(std::function<const tk::Image*(const std::string& mxc_url)> provider);

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
    bool jump_to_date_enabled() const
    {
        return show_calendar_btn_;
    }

    // Show or hide the leading back button. Used in the narrow-window
    // adaptive layout (see MainAppWidget), where the room list and room
    // view aren't both shown at once — this is the affordance back to the
    // list. Hidden by default.
    void set_show_back_button(bool show)
    {
        show_back_btn_ = show;
    }
    bool show_back_button() const
    {
        return show_back_btn_;
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

    // Unread-thread indicator on the threads button (or the overflow button
    // when threads is collapsed there). `any_unread` draws a small neutral
    // dot; `any_mention` promotes it to the accent colour. The caller is
    // expected to trigger a repaint (RoomView::set_threads_unread does).
    void set_threads_unread(bool any_unread, bool any_mention)
    {
        threads_unread_ = any_unread;
        threads_mention_ = any_mention;
    }

    // Show or hide the search button. Hidden by default; the shell enables it
    // when the room supports full-text search.
    void set_show_search_btn(bool show)
    {
        show_search_btn_ = show;
    }
    bool show_search_btn() const
    {
        return show_search_btn_;
    }

    // Show or hide the start-call button. Hidden by default; shown when the
    // room is a DM or the server advertises MatrixRTC support.
    void set_show_call_btn(bool show)
    {
        show_call_btn_ = show;
    }
    bool show_call_btn() const
    {
        return show_call_btn_;
    }

    // Toggle the active-call indicator style on the call button.
    void set_call_active(bool active)
    {
        call_active_ = active;
    }

    // Fired when the user presses the call button, passing the button's
    // world-space rect so the owner can anchor a popup to it.
    std::function<void(tk::Rect)> on_call_requested;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;
    void paint_overlay(tk::PaintCtx&) override;
    void access_detached_children(std::vector<tk::Widget*>& out) const override
    {
        if (date_picker_visible_ && date_picker_)
        {
            out.push_back(date_picker_.get());
        }
    }
    void on_popup_dismiss() override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    tk::Widget* dispatch_pointer_move(tk::Point world, bool* dirty = nullptr) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

    // Fired when a hyperlink in the room topic is clicked.
    std::function<void(const std::string& url)> on_link_clicked;

    // ── Accessibility ─────────────────────────────────────────────────────
    // Painted-only content: the encryption lock, and — when the topic is
    // rich text (HTML, or a plain topic with URLs), so the topic Label is
    // hidden — the topic text plus one Link row per link in it.
    std::size_t access_row_count() const override;
    tk::Role access_role_for_widget_row(std::size_t i) const override;
    std::string access_name_for_widget_row(std::size_t i) const override;
    bool access_activate_widget_row(std::size_t i) override;
    tk::Rect access_rect_for_widget_row(std::size_t i) const override;
    bool access_rows_after_children() const override
    {
        return true;
    }
    // Fired when the pointer enters or leaves a hyperlink in the topic.
    // Passes the URL while hovering, empty string when leaving.
    std::function<void(const std::string& url)> on_link_hovered;

    // Fired when the user confirms a date in the picker (ms since Unix epoch,
    // local midnight on the selected day). Replaces on_jump_to_date_requested.
    std::function<void(std::uint64_t ts_ms)> on_date_jump;

    // Fired each time the date picker opens, so the owner can refresh the
    // cap via set_jump_to_date_max_ts().
    std::function<void()> on_date_picker_opened;

    // Latest day the picker offers: the local day of `ts_ms` (the room's
    // newest content event), never later than today. 0 = no cap beyond today.
    // Applies immediately if the picker is open.
    void set_jump_to_date_max_ts(std::uint64_t ts_ms);

    // On a room switch: drops the previous room's cap and the picker's shown
    // month, so the next open lands on this room's most recent day.
    void reset_jump_to_date();

    // Fired from paint() when the bridge badge is shown but its network logo
    // isn't cached yet, so the shell can fetch it. Safe to fire every paint;
    // the shell dedups in-flight fetches.
    std::function<void(const std::string& mxc_url)> on_bridge_avatar_needed;

    // Fired when the user clicks the threads button.
    std::function<void()> on_threads_requested;

    // Fired when the user clicks the search button.
    std::function<void()> on_search_requested;

    // Fired when the user clicks the room name or avatar area (not on a topic
    // hyperlink or calendar button).
    std::function<void()> on_info_requested;

    // Fired when the user clicks the back button (narrow-window layout only).
    std::function<void()> on_back_requested;

    // Fired when the user clicks the overflow ("more") trigger, with the
    // currently-collapsed items and the trigger button's world-space rect to
    // anchor a popup to. RoomHeader has no popup of its own — it paints too
    // early in RoomView's own paint() order for anything it owned to reliably
    // appear on top (message rows etc. paint after it and would just cover
    // it) — so the owner is expected to open its own PopupMenu, arranged at
    // its own (taller, later-painting) bounds. See RoomView::header_overflow_menu_.
    std::function<void(std::vector<PopupMenu::Item> items, tk::Rect anchor_world)>
        on_overflow_requested;

    // Fired whenever the overflow trigger stops being shown (everything fits
    // again, or the header goes condensed) so the owner can close whatever
    // popup it opened in response to on_overflow_requested, if still open.
    std::function<void()> on_overflow_should_close;

private:
    // Draws a vector padlock icon in `rect` (10×12 logical px), tinted with `tint`.
    static void draw_lock_icon(tk::Canvas& canvas, tk::Rect rect, tk::Color tint);

    // Cached from paint() so on_pointer_move/on_pointer_leave (which don't
    // receive a PaintCtx) can reach Host::show_tooltip/hide_tooltip for the
    // truncated-topic tooltip.
    tk::Host* host_ = nullptr;

    bool condensed_ = false;
    bool show_calendar_btn_ = false;
    bool show_threads_btn_ = false;
    bool threads_unread_ = false;
    bool threads_mention_ = false;
    bool show_search_btn_ = false;
    bool show_call_btn_ = false;
    bool call_active_ = false;
    bool show_back_btn_ = false;

    // Calendar / threads / search / call action buttons. Variant::Icon child
    // buttons own their hover/press background and click dispatch; this view
    // only draws the vector glyph centred inside each button's bounds.
    tk::Button* calendar_btn_ = nullptr;
    tk::Button* threads_btn_ = nullptr;
    tk::Button* search_btn_ = nullptr;
    tk::Button* call_btn_ = nullptr;
    // Leading back button — same Icon-variant pattern as the others above.
    tk::Button* back_btn_ = nullptr;

    // Overflow trigger — shown instead of whichever action buttons don't fit
    // at the current width (lowest-priority ones collapse first; see
    // arrange()). The actual popup is owned by whoever owns
    // on_overflow_requested (RoomView) — see that callback's doc comment.
    tk::Button* more_btn_ = nullptr;
    // Keyboard stand-in for the clickable avatar/name/topic area: Enter
    // opens room info (the same on_info_requested a click fires), Left/
    // Right cycle the topic's links. Pointer-transparent.
    tk::KeyboardTarget* title_target_ = nullptr;

public:
    tk::KeyboardTarget* title_target() const
    {
        return title_target_;
    }

private:
    // Rebuilt each arrange() pass with whatever's currently collapsed;
    // consumed when more_btn_ is clicked.
    std::vector<PopupMenu::Item> overflow_items_;

    // Owned date-picker popup (not a widget-tree child — driven via
    // register_popup / paint_overlay).
    std::unique_ptr<DatePickerView> date_picker_;
    bool date_picker_visible_ = false;

    // Helpers.
    void show_date_picker_();
    void hide_date_picker_();
    void apply_picker_max_();
    std::uint64_t jump_max_ts_ = 0;

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

    // Test-only accessor for the search button's own widget identity — used
    // to assert it's excluded from Tab traversal while a modal overlay is
    // open (bounds alone can't disambiguate since the overlay covers the
    // same screen area).
    tk::Widget* search_btn_for_test() const
    {
        return search_btn_;
    }
    tk::Rect name_label_rect_for_test() const
    {
        return name_label_ ? name_label_->bounds() : tk::Rect{};
    }
    tk::Rect topic_label_rect_for_test() const
    {
        return topic_label_ ? topic_label_->bounds() : tk::Rect{};
    }

private:
    bool encrypted_ = false;
    tk::Rect lock_icon_rect_{};

    // Bridged-room badge next to the title: the network's logo (or a cable
    // glyph) with a tooltip explaining what bridging means.
    bool bridged_ = false;
    std::string bridge_network_name_;
    std::string bridge_network_avatar_url_;
    tk::Rect bridge_icon_rect_{};
    bool hover_bridge_ = false;
    tk::IconCache bridge_icon_cache_;
    std::string bridge_tooltip_() const;

    struct AccessRow
    {
        tk::Role role;
        std::string name;
        std::string url; // Link rows only
        tk::Rect rect;
    };
    mutable std::vector<AccessRow> access_rows_;

    bool press_info_ = false; // a topic hyperlink is pressed (info_area_ declines those)
    bool topic_link_at_(tk::Point world) const;
    // Hoverable avatar+name+topic block that opens room info on click.
    tk::Widget* info_area_ = nullptr;
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

    tk::CachedTextLayout topic_layout_;

    std::function<const tk::Image*(const std::string&)> avatar_provider_;
};

} // namespace tesseract::views
