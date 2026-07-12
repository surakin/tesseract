#include "RoomInfoPanel.h"
#include "html_spans.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

// ── constructor ───────────────────────────────────────────────────────────

RoomInfoPanel::RoomInfoPanel()
{
    close_btn_ = add_child(
        std::make_unique<tk::Button>("\xC3\x97", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    settings_btn_ = add_child(
        std::make_unique<tk::Button>("\xF0\x9F\x94\xA7", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    edit_topic_btn_ = add_child(
        std::make_unique<tk::Button>("\xE2\x9C\x8E", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    save_btn_ = add_child(
        std::make_unique<tk::Button>("Save", std::function<void()>{},
                                     tk::Button::Variant::Primary));
    cancel_btn_ = add_child(
        std::make_unique<tk::Button>("Cancel", std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    expand_btn_ = add_child(
        std::make_unique<tk::Button>("Show all \xE2\x96\xBE", std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    leave_btn_ = add_child(
        std::make_unique<tk::Button>("Leave Room", std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    // Favourite / Low-priority tag switches (mutually exclusive in the UI).
    favourite_btn_ = add_child(std::make_unique<tk::SwitchButton>("Favourite"));
    favourite_btn_->on_change = [this](bool on) {
        if (on && low_priority_btn_) low_priority_btn_->set_checked(false);
        if (on_favourite_changed) on_favourite_changed(room_id_, on);
        if (on_layout_changed) on_layout_changed(); // repaint both switches
    };

    low_priority_btn_ = add_child(std::make_unique<tk::SwitchButton>("Low priority"));
    low_priority_btn_->on_change = [this](bool on) {
        if (on && favourite_btn_) favourite_btn_->set_checked(false);
        if (on_low_priority_changed) on_low_priority_changed(room_id_, on);
        if (on_layout_changed) on_layout_changed();
    };

    // ComboBox is added last so it is dispatched first in reverse child order,
    // ensuring its expanded dropdown captures pointer events before leave_btn_.
    auto notif_combo = std::make_unique<tk::ComboBox>();
    notif_combo->set_options({
        {.label = "Default",      .value = "default"},
        {.label = "All messages", .value = "all"},
        {.label = "Mentions",     .value = "mentions"},
        {.label = "Off",          .value = "off"},
    });
    notif_combo->set_selected_value("default");
    notif_combo->on_changed = [this](std::string value) {
        if (on_notification_mode_changed)
            on_notification_mode_changed(room_id_, std::move(value));
    };
    notification_combo_ = add_child(std::move(notif_combo));

    close_btn_->set_on_click([this]() {
        if (on_close) on_close();
    });
    settings_btn_->set_on_click([this]() {
        if (on_room_settings_requested) on_room_settings_requested();
    });
    edit_topic_btn_->set_on_click([this]() {
        editing_topic_   = true;
        topic_edit_text_ = topic_;
        if (on_layout_changed) on_layout_changed();
    });
    save_btn_->set_on_click([this]() {
        // Optimistically reflect the new topic immediately; refresh_info()
        // will reconcile when the SDK echoes the state event back.
        if (topic_ != topic_edit_text_)
        {
            topic_      = topic_edit_text_;
            topic_html_ = {};
            topic_spans_ = autolink_plain_to_spans(topic_);
            topic_layout_.reset();
        }
        if (on_save_topic) on_save_topic(room_id_, topic_edit_text_);
        editing_topic_ = false;
        if (on_layout_changed) on_layout_changed();
    });
    cancel_btn_->set_on_click([this]() {
        editing_topic_   = false;
        topic_edit_text_ = topic_;
        if (on_layout_changed) on_layout_changed();
    });
    expand_btn_->set_on_click([this]() {
        members_expanded_ = true;
        if (on_layout_changed) on_layout_changed();
    });
    leave_btn_->set_on_click([this]() {
        if (on_leave_room) on_leave_room(room_id_);
    });

    // save, cancel, and expand are hidden until needed
    save_btn_->set_visible(false);
    cancel_btn_->set_visible(false);
    expand_btn_->set_visible(false);

    // The panel is a closed-by-default overlay. Tie its widget visibility
    // to the open state so the Widget tree's hit-test walks past us entirely
    // when closed — otherwise leave_btn_ (positioned at the bottom-right of
    // panel_rect_) overlaps the compose-bar buttons and silently captures
    // their clicks. Children are skipped because their parent is invisible.
    set_visible(false);
}

// ── public API ────────────────────────────────────────────────────────────

void RoomInfoPanel::open(const tesseract::RoomInfo& info)
{
    const bool was_open = open_;

    room_id_            = info.id;
    display_name_       = info.name;
    avatar_url_         = info.effective_avatar_url();
    topic_              = info.topic;
    topic_html_         = info.topic_html;
    is_encrypted_       = info.is_encrypted;
    history_visibility_ = info.history_visibility;
    is_bridged_         = info.is_bridged;

    open_            = true;
    set_visible(true);
    editing_topic_   = false;
    topic_edit_text_ = {};
    members_expanded_ = false;
    scroll_offset_   = 0.0f;

    members_.clear();
    member_layouts_.clear();
    member_rects_.clear();

    name_layout_.reset();
    badge_enc_layout_.reset();
    badge_hist_layout_.reset();
    topic_layout_.reset();
    topic_spans_ = topic_html_.empty() ? autolink_plain_to_spans(topic_) :
                                         std::vector<tk::TextSpan>{};

    expand_btn_->set_label("Show all \xE2\x96\xBE");

    if (notification_combo_)
    {
        notification_combo_->collapse();
        notification_combo_->set_selected_value("default");
    }
    if (favourite_btn_)    favourite_btn_->set_checked(info.is_favorite);
    if (low_priority_btn_) low_priority_btn_->set_checked(info.is_low_priority);
    if (on_fetch_notification_mode) on_fetch_notification_mode(room_id_);

    if (on_fetch_members) on_fetch_members(room_id_);

    // Fire the layout-changed callback so the shell hides native overlays
    // (compose textarea, room search) while the panel covers the canvas.
    if (!was_open && on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::refresh_info(const tesseract::RoomInfo& info)
{
    if (!open_) return;
    display_name_       = info.name;
    avatar_url_         = info.effective_avatar_url();
    is_encrypted_       = info.is_encrypted;
    history_visibility_ = info.history_visibility;
    is_bridged_         = info.is_bridged;
    // Authoritative re-sync after a server-side tag change.
    if (favourite_btn_)    favourite_btn_->set_checked(info.is_favorite);
    if (low_priority_btn_) low_priority_btn_->set_checked(info.is_low_priority);
    if (topic_ != info.topic || topic_html_ != info.topic_html)
    {
        topic_      = info.topic;
        topic_html_ = info.topic_html;
        topic_layout_.reset();
        topic_spans_ = topic_html_.empty() ? autolink_plain_to_spans(topic_) :
                                             std::vector<tk::TextSpan>{};
    }
    name_layout_.reset();
    badge_enc_layout_.reset();
    badge_hist_layout_.reset();
    badge_bridged_layout_.reset();
    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    if (was_open && on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::set_avatar_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void RoomInfoPanel::set_presence_provider(PresenceProvider p)
{
    presence_provider_ = std::move(p);
}

void RoomInfoPanel::set_members(std::vector<tesseract::RoomMember> members)
{
    members_       = std::move(members);
    member_layouts_.clear();
    member_rects_.clear();

    const int total = static_cast<int>(members_.size());
    expand_btn_->set_label(
        std::string("Show all (") + std::to_string(total) + ") \xE2\x96\xBE");

    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::set_notification_mode(std::string mode)
{
    if (!open_) return;
    if (notification_combo_) notification_combo_->set_selected_value(mode);
}

void RoomInfoPanel::set_media_count(int count)
{
    if (count == media_count_) return;
    media_count_ = count;
    media_row_layout_.reset();
    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::set_topic_edit_text(std::string t)
{
    topic_edit_text_ = std::move(t);
}

tk::Rect RoomInfoPanel::topic_edit_rect() const
{
    if (!editing_topic_) return {};
    return topic_edit_rect_;
}

// ── layout ────────────────────────────────────────────────────────────────

float RoomInfoPanel::measure_topic_height_(tk::CanvasFactory& factory, float max_w)
{
    topic_truncated_ = false;

    tk::TextStyle st{};
    st.role      = tk::FontRole::Body;
    st.halign    = tk::TextHAlign::Leading;
    st.wrap      = true;
    st.max_width = max_w;

    if (!topic_layout_)
    {
        if (!topic_html_.empty())
            topic_layout_ = factory.build_rich_text(html_to_spans(topic_html_), st);
        else if (!topic_spans_.empty())
            topic_layout_ = factory.build_rich_text(topic_spans_, st);
        else if (!topic_.empty())
            topic_layout_ = factory.build_text(topic_, st);
    }

    // Empty topic → a single "No topic set." placeholder line.
    const tk::TextLayout* lo = topic_layout_.get();
    std::unique_ptr<tk::TextLayout> placeholder;
    if (!lo)
    {
        placeholder = factory.build_text("No topic set.", st);
        lo = placeholder.get();
    }
    if (!lo) return 20.0f; // defensive fallback (~1 Body line)

    const int   lines  = std::max(1, lo->line_count());
    const float line_h = lo->measure().h / static_cast<float>(lines);
    const int   shown  = std::min(lines, kTopicMaxLines);
    topic_truncated_   = lines > kTopicMaxLines;
    return line_h * static_cast<float>(shown);
}

tk::Size RoomInfoPanel::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void RoomInfoPanel::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    backdrop_rect_ = bounds;
    panel_rect_    = {bounds.x + bounds.w - kPanelW, bounds.y,
                      kPanelW, bounds.h};

    const float px = panel_rect_.x;
    const float iw = kPanelW - kPadX * 2.0f;

    // Settings (wrench) and Close buttons: fixed at the top of the panel,
    // never scroll. Settings sits top-left, Close top-right.
    constexpr float kCloseSz = 32.0f;
    if (settings_btn_)
        settings_btn_->arrange(lc, {px + 8.0f, panel_rect_.y + 8.0f, kCloseSz, kCloseSz});
    if (close_btn_)
        close_btn_->arrange(lc, {px + kPanelW - 8.0f - kCloseSz, panel_rect_.y + 8.0f,
                                 kCloseSz, kCloseSz});

    // Everything below the close button scrolls. Compute the y origin of the
    // scrollable viewport and clamp scroll_offset_ to valid range.
    const float scroll_top = panel_rect_.y + 8.0f + kCloseSz + 4.0f;
    const float viewport_h = panel_rect_.h - (scroll_top - panel_rect_.y);
    const float max_scroll = std::max(0.0f, content_height_ - viewport_h);
    scroll_offset_ = std::clamp(scroll_offset_, 0.0f, max_scroll);

    // y is the running cursor in world-space with scroll applied.
    float y = scroll_top - scroll_offset_;

    // Avatar circle (72×72), centred in panel
    const float av_x = px + (kPanelW - kAvatarD) * 0.5f;
    avatar_rect_ = {av_x, y, kAvatarD, kAvatarD};
    y += kAvatarD + kPadY;

    // Room name row height estimate: Title = ~20px
    constexpr float kNameH  = 20.0f;
    y += kNameH + 4.0f;

    // Badge row (~16px) — encrypted + history visibility
    y += 16.0f + 4.0f;

    // Separator + "Topic" section header (Small ~12px)
    y += 1.0f + kPadY + 12.0f + 4.0f;

    // Topic text region — fixed 80px tall (NativeTextArea height when editing)
    // Topic height adapts to the wrapped line count (capped at kTopicMaxLines);
    // when editing, a fixed editable area is used instead.
    const float topic_h =
        editing_topic_ ? kTopicEditH : measure_topic_height_(lc.factory, iw);
    if (editing_topic_) topic_truncated_ = false;
    topic_rect_ = {px + kPadX, y, iw, topic_h};
    y += topic_h + 4.0f;

    topic_edit_rect_ = topic_rect_;

    // Edit topic button: 28×28 to the right of the topic header
    if (edit_topic_btn_)
    {
        const float ebx = px + kPanelW - kPadX - kSmallEditH;
        const float eby = topic_rect_.y - 12.0f - kSmallEditH;
        edit_topic_btn_->arrange(lc, {ebx, eby, kSmallEditH, kSmallEditH});
        edit_topic_btn_->set_visible(!editing_topic_);
    }

    // Save + Cancel buttons (visible only when editing)
    const bool show_edit_btns = editing_topic_;
    if (save_btn_)
    {
        save_btn_->set_visible(show_edit_btns);
        if (show_edit_btns)
            save_btn_->arrange(lc, {px + kPadX, y, iw * 0.5f - 4.0f, kSmallEditH});
    }
    if (cancel_btn_)
    {
        cancel_btn_->set_visible(show_edit_btns);
        if (show_edit_btns)
            cancel_btn_->arrange(lc, {px + kPadX + iw * 0.5f + 4.0f, y,
                                      iw * 0.5f - 4.0f, kSmallEditH});
    }
    if (show_edit_btns)
        y += kSmallEditH + kPadY;

    // Separator + Favourite / Low-priority switch rows — between topic & members.
    tags_sep_y_ = y;
    y += 1.0f + kPadY;
    tags_row_y_ = y;
    if (favourite_btn_)
        favourite_btn_->arrange(lc, {px + kPadX, y, iw, kButtonH});
    if (low_priority_btn_)
        low_priority_btn_->arrange(lc, {px + kPadX, y + kButtonH, iw, kButtonH});
    y += 2.0f * kButtonH + kPadY;

    // Separator + "Members (N)" section header
    y += 1.0f + kPadY + 12.0f + 4.0f;

    // Member rows
    const int total_members   = static_cast<int>(members_.size());
    const int visible_members = (members_expanded_ || total_members <= 5)
                                    ? total_members : 5;

    member_rects_.clear();
    for (int i = 0; i < visible_members; ++i)
    {
        member_rects_.push_back({px, y, kPanelW, kMemberRowH});
        y += kMemberRowH;
    }

    // Expand button (shown when >5 members and not expanded)
    const bool show_expand = !members_expanded_ && total_members > 5;
    if (expand_btn_)
    {
        expand_btn_->set_visible(show_expand);
        if (show_expand)
        {
            expand_btn_->arrange(lc, {px + kPadX, y, iw, kButtonH});
            y += kButtonH + kPadY;
        }
    }

    // Notifications section — before Leave Room
    {
        notif_sep_y_ = y;
        y += 1.0f + kPadY;
        y += 12.0f + 6.0f; // section header
        if (notification_combo_)
        {
            notification_combo_->arrange(lc, {px + kPadX, y, iw, 32.0f});
            notification_combo_->set_popup_clip(panel_rect_);
            y += 32.0f + kPadY;
        }
    }

    // "Media (N)" row — direct-painted/hit-tested like the member rows
    // (see on_pointer_down/up/move). No popup of its own, so it doesn't
    // participate in the leave/combo paint-order inversion below.
    media_row_rect_ = {px, y, kPanelW, kMediaRowH};
    y += kMediaRowH + kPadY;

    // Leave button: flows after content, never overlaps the member list.
    if (leave_btn_)
    {
        const float leave_y = y + kPadY;
        leave_btn_->arrange(lc, {px + kPadX, leave_y, iw, kButtonH});
        y = leave_y + kButtonH + kPadY;
    }

    // content_height_: natural full height of scrollable content.
    // y is in world-space with scroll applied, so natural_y = y + scroll_offset_.
    content_height_ = (y + scroll_offset_) - scroll_top;
}

// ── paint ─────────────────────────────────────────────────────────────────

void RoomInfoPanel::paint(tk::PaintCtx& ctx)
{
    host_ = ctx.host;
    if (!open_) return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // 1. Semi-transparent backdrop
    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 100});

    // 2. Panel background
    cv.fill_rect(panel_rect_, pal.chrome_bg);

    // 3. 1px left border
    cv.fill_rect({panel_rect_.x, panel_rect_.y, 1.0f, panel_rect_.h},
                 pal.separator);

    // 4. Clip all scrollable content to the panel rect, then paint the close
    //    button last so it always renders above scrolled content.
    ctx.canvas.push_clip_rect(panel_rect_);

    // 5. Avatar
    const tk::Point av_centre{avatar_rect_.x + kAvatarD * 0.5f,
                               avatar_rect_.y + kAvatarD * 0.5f};
    const tk::Image* av_img = nullptr;
    if (image_provider_ && !avatar_url_.empty())
    {
        av_img = image_provider_(avatar_url_);
    }
    {
        std::string_view disp =
            display_name_.empty() ? std::string_view("?")
                                  : std::string_view(display_name_);
        draw_avatar(cv, av_img, av_centre, kAvatarD, disp, pal.accent,
                    tk::Color{255, 255, 255, 255});
    }

    // 6. Room name (Title, centred, ellipsis)
    const float text_max_w = kPanelW - kPadX * 2.0f;
    const float name_y     = avatar_rect_.y + kAvatarD + kPadY;

    if (!name_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_max_w;
        name_layout_ = ctx.factory.build_text(
            display_name_.empty() ? room_id_ : display_name_, st);
    }
    if (name_layout_)
    {
        const tk::Size sz = name_layout_->measure();
        const float tx    = panel_rect_.x + (kPanelW - sz.w) * 0.5f;
        cv.draw_text(*name_layout_, {tx, name_y}, pal.text_primary);
    }

    // 7. Badge row
    const float badge_y = name_y + 20.0f + 4.0f;
    float badge_x = panel_rect_.x + kPadX;

    if (is_encrypted_)
    {
        if (!badge_enc_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_max_w;
            badge_enc_layout_ =
                ctx.factory.build_text(tk::tr("\xF0\x9F\x94\x92 Encrypted"), st);
        }
        if (badge_enc_layout_)
        {
            cv.draw_text(*badge_enc_layout_, {badge_x, badge_y}, pal.text_muted);
            badge_x += badge_enc_layout_->measure().w + 12.0f;
        }
    }

    if (!history_visibility_.empty())
    {
        if (!badge_hist_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_max_w;
            const std::string hist_text =
                "\xF0\x9F\x91\x81 " + history_visibility_;
            badge_hist_layout_ = ctx.factory.build_text(hist_text, st);
        }
        if (badge_hist_layout_)
        {
            cv.draw_text(*badge_hist_layout_, {badge_x, badge_y}, pal.text_muted);
            badge_x += badge_hist_layout_->measure().w + 12.0f;
        }
    }

    if (is_bridged_)
    {
        if (!badge_bridged_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_max_w;
            badge_bridged_layout_ =
                ctx.factory.build_text(tk::tr("\xF0\x9F\x8C\x89 Bridged"), st);
        }
        if (badge_bridged_layout_)
            cv.draw_text(*badge_bridged_layout_, {badge_x, badge_y}, pal.text_muted);
    }

    // 8. Separator before Topic section
    const float sep1_y = badge_y + 16.0f + kPadY * 0.5f;
    cv.fill_rect({panel_rect_.x + kPadX, sep1_y, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.separator);

    // "Topic" section header (Small, muted)
    const float section_topic_y = sep1_y + 1.0f + 4.0f;
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = text_max_w;
        auto lbl = ctx.factory.build_text("Topic", st);
        if (lbl)
        {
            cv.draw_text(*lbl, {panel_rect_.x + kPadX, section_topic_y},
                         pal.text_muted);
        }
    }

    // 9. Edit topic button (only when not editing)
    if (edit_topic_btn_ && !editing_topic_)
    {
        edit_topic_btn_->paint(ctx);
        edit_topic_icon_.draw(ctx.canvas, ctx.factory, kEditSvg,
                              edit_topic_btn_->bounds(), 16.0f,
                              ctx.theme.palette.text_secondary);
    }

    // 10. Topic text (drawn when not editing)
    if (!editing_topic_)
    {
        if (!topic_layout_)
        {
            tk::TextStyle st{};
            st.role       = tk::FontRole::Body;
            st.halign     = tk::TextHAlign::Leading;
            st.wrap       = true;
            st.max_width  = text_max_w;

            if (!topic_html_.empty())
            {
                const auto spans = html_to_spans(topic_html_);
                topic_layout_ = ctx.factory.build_rich_text(spans, st);
            }
            else if (!topic_spans_.empty())
            {
                topic_layout_ = ctx.factory.build_rich_text(topic_spans_, st);
            }
            else if (!topic_.empty())
            {
                topic_layout_ = ctx.factory.build_text(topic_, st);
            }
        }
        // topic_rect_ is sized to the wrapped topic up to kTopicMaxLines lines
        // (see measure_topic_height_); clip to it so a longer topic is cut at
        // the cap (the full text is then available via the hover tooltip).
        ctx.canvas.push_clip_rect(topic_rect_);
        if (topic_layout_)
        {
            cv.draw_text(*topic_layout_,
                         {topic_rect_.x, topic_rect_.y},
                         pal.text_primary);
        }
        else if (topic_.empty())
        {
            // Placeholder
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.max_width = text_max_w;
            auto lbl = ctx.factory.build_text("No topic set.", st);
            if (lbl)
            {
                cv.draw_text(*lbl, {topic_rect_.x, topic_rect_.y},
                             pal.text_muted);
            }
        }
        ctx.canvas.pop_clip();
    }

    // 11. Save + Cancel when editing
    if (save_btn_ && editing_topic_)   save_btn_->paint(ctx);
    if (cancel_btn_ && editing_topic_) cancel_btn_->paint(ctx);

    // 11b. Separator + Favourite / Low-priority switch rows (between topic
    //      and members).
    cv.fill_rect({panel_rect_.x + kPadX, tags_sep_y_, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.separator);
    if (favourite_btn_)    favourite_btn_->paint(ctx);
    if (low_priority_btn_) low_priority_btn_->paint(ctx);

    // 12. Separator before Members section. Mirror arrange(): topic region,
    //     then (when editing) the edit buttons, then the switch separator +
    //     the two switch rows.
    float cur_y = topic_rect_.y + topic_rect_.h + 4.0f;
    if (editing_topic_)
    {
        cur_y += kSmallEditH + kPadY;
    }
    cur_y += 1.0f + kPadY + 2.0f * kButtonH + kPadY; // switch separator + rows
    cv.fill_rect({panel_rect_.x + kPadX, cur_y, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.separator);

    const float section_mem_y = cur_y + 1.0f + 4.0f;

    // "Members (N)" section header
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.max_width = text_max_w;
        const std::string mem_hdr =
            "Members (" + std::to_string(static_cast<int>(members_.size())) + ")";
        auto lbl = ctx.factory.build_text(mem_hdr, st);
        if (lbl)
        {
            cv.draw_text(*lbl, {panel_rect_.x + kPadX, section_mem_y},
                         pal.text_muted);
        }
    }

    // 13. Member rows
    // Rebuild member layouts lazily as the cache may be empty
    const int visible_count = static_cast<int>(member_rects_.size());
    if (static_cast<int>(member_layouts_.size()) < static_cast<int>(members_.size()))
    {
        member_layouts_.resize(members_.size());
    }

    for (int i = 0; i < visible_count; ++i)
    {
        const tk::Rect& row = member_rects_[i];
        const auto& mem     = members_[static_cast<std::size_t>(i)];

        // Hover highlight
        if (i == hover_member_)
        {
            cv.fill_rect(row, pal.subtle_hover);
        }

        // Small avatar (32×32), 6px from left of panel
        const float sm_av_x = row.x + kPadX;
        const float sm_av_y = row.y + (kMemberRowH - kAvatarSmall) * 0.5f;
        const tk::Point sm_centre{sm_av_x + kAvatarSmall * 0.5f,
                                   sm_av_y + kAvatarSmall * 0.5f};

        const tk::Image* mem_img = nullptr;
        if (image_provider_ && !mem.avatar_url.empty())
        {
            mem_img = image_provider_(mem.avatar_url);
            if (!mem_img && on_member_avatar_needed)
                on_member_avatar_needed(mem);
        }
        {
            std::string_view disp_sv =
                mem.display_name.empty()
                    ? std::string_view("?")
                    : std::string_view(mem.display_name);
            draw_avatar(cv, mem_img, sm_centre, kAvatarSmall, disp_sv,
                        pal.avatar_initials_bg, pal.avatar_initials_text);
        }

        // Presence dot — bottom-right of member avatar.
        if (presence_provider_)
        {
            const auto ps = presence_provider_(mem.user_id);
            tk::Color dot_color{};
            bool show_dot = true;
            if (ps == tesseract::PresenceState::Online)
            {
                dot_color = pal.presence_online;
            }
            else if (ps == tesseract::PresenceState::Unavailable)
            {
                dot_color = pal.presence_unavailable;
            }
            else
            {
                show_dot = false;
            }
            if (show_dot)
            {
                constexpr float kDotD = 7.0f; // slightly smaller for 32dp avatar
                constexpr float kRing = 1.5f;
                const float outer_d   = kDotD + kRing * 2.0f;
                // Centre on avatar's bottom-right edge: half inside, half outside.
                const float dot_cx    = sm_centre.x + kAvatarSmall * 0.5f;
                const float dot_cy    = sm_centre.y + kAvatarSmall * 0.5f;
                cv.fill_rounded_rect(
                    {dot_cx - outer_d * 0.5f, dot_cy - outer_d * 0.5f,
                     outer_d, outer_d},
                    outer_d * 0.5f, pal.bg);
                cv.fill_rounded_rect(
                    {dot_cx - kDotD * 0.5f, dot_cy - kDotD * 0.5f, kDotD, kDotD},
                    kDotD * 0.5f, dot_color);
            }
        }

        // Text column: display_name + user_id
        const float txt_x     = sm_av_x + kAvatarSmall + 8.0f;
        const float txt_max_w = row.x + row.w - kPadX - txt_x;
        const float name_ty   = row.y + (kMemberRowH * 0.5f) - 14.0f;
        const float uid_ty    = name_ty + 14.0f + 2.0f;

        auto& ml = member_layouts_[static_cast<std::size_t>(i)];

        if (!ml.name)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::SenderName;
            st.halign    = tk::TextHAlign::Leading;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = txt_max_w;
            ml.name = ctx.factory.build_text(
                mem.display_name.empty() ? mem.user_id : mem.display_name, st);
        }
        if (!ml.uid)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = txt_max_w;
            ml.uid = ctx.factory.build_text(mem.user_id, st);
        }

        if (ml.name)
        {
            cv.draw_text(*ml.name, {txt_x, name_ty}, pal.text_primary);
        }
        if (ml.uid)
        {
            cv.draw_text(*ml.uid, {txt_x, uid_ty}, pal.text_muted);
        }
    }

    // 14. Expand button
    if (expand_btn_ && expand_btn_->visible()) expand_btn_->paint(ctx);

    // 15. Notifications separator + section header
    cv.fill_rect({panel_rect_.x + kPadX, notif_sep_y_, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.separator);
    {
        const float hdr_y = notif_sep_y_ + 1.0f + kPadY;
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = kPanelW - kPadX * 2.0f;
        auto lbl = ctx.factory.build_text("Notifications", st);
        if (lbl)
            cv.draw_text(*lbl, {panel_rect_.x + kPadX, hdr_y}, pal.text_muted);
    }

    // 15b. "Media (N)" row — plain clickable text row, no chrome, matching
    // the member rows rather than a bordered tk::Button (it navigates to the
    // gallery rather than performing an in-place action).
    {
        if (hover_media_)
            cv.fill_rect(media_row_rect_, pal.subtle_hover);
        if (!media_row_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.halign    = tk::TextHAlign::Leading;
            st.max_width = kPanelW - kPadX * 2.0f;
            media_row_layout_ = ctx.factory.build_text(
                tk::trf(tk::tr("Media ({0})"), {std::to_string(media_count_)}),
                st);
        }
        if (media_row_layout_)
        {
            const float ty = media_row_rect_.y +
                             (kMediaRowH - media_row_layout_->measure().h) * 0.5f;
            cv.draw_text(*media_row_layout_, {panel_rect_.x + kPadX, ty},
                         pal.text_primary);
        }
    }

    // 16. Leave button (painted before the notification combo so the combo's
    //     expanded dropdown overlays it when open)
    if (leave_btn_) leave_btn_->paint(ctx);

    // 17. Notification combo — painted last so its dropdown overlays leave btn
    if (notification_combo_) notification_combo_->paint(ctx);

    ctx.canvas.pop_clip();

    // 16. Settings + Close buttons — painted outside the clip so they're
    //     always visible regardless of scroll position.
    if (settings_btn_)
    {
        settings_btn_->paint(ctx);
        settings_icon_.draw(ctx.canvas, ctx.factory, kWrenchSvg,
                            settings_btn_->bounds(), 16.0f,
                            ctx.theme.palette.text_secondary);
    }
    if (close_btn_)
    {
        close_btn_->paint(ctx);
        close_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg,
                         close_btn_->bounds(), 16.0f,
                         ctx.theme.palette.text_secondary);
    }
}

// ── pointer events ────────────────────────────────────────────────────────

bool RoomInfoPanel::on_pointer_down(tk::Point local)
{
    if (!open_) return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (rect_contains(panel_rect_, w))
    {
        // Avatar click → open lightbox. Falls through when no URL or no
        // callback so initials-only rooms keep the current no-op behaviour.
        if (rect_contains(avatar_rect_, w) && !avatar_url_.empty()
            && on_avatar_clicked)
        {
            press_avatar_ = true;
            return true;
        }
        // Hit-test topic links before member rows.
        if (topic_layout_ && rect_contains(topic_rect_, w))
        {
            const tk::Point ll{w.x - topic_rect_.x, w.y - topic_rect_.y};
            std::string url = topic_layout_->link_at(ll);
            if (!url.empty())
            {
                press_link_url_ = std::move(url);
                return true;
            }
        }

        // Hit-test direct-painted member rows first (not child widgets).
        for (int i = 0; i < static_cast<int>(member_rects_.size()); ++i)
        {
            if (rect_contains(member_rects_[static_cast<std::size_t>(i)], w))
            {
                press_member_ = i;
                return true;
            }
        }
        if (rect_contains(media_row_rect_, w))
        {
            press_media_ = true;
            return true;
        }
        // Let child dispatch handle button events inside the panel.
        return false;
    }

    // Backdrop click: consume and remember for on_pointer_up
    press_backdrop_ = true;
    return true;
}

void RoomInfoPanel::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_link_url_.empty())
    {
        std::string url = std::move(press_link_url_);
        press_link_url_.clear();
        if (inside_self && on_link_clicked)
        {
            const tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (rect_contains(topic_rect_, w) && topic_layout_)
            {
                const tk::Point ll{w.x - topic_rect_.x, w.y - topic_rect_.y};
                if (topic_layout_->link_at(ll) == url)
                    on_link_clicked(url);
            }
        }
        return;
    }

    if (press_avatar_)
    {
        press_avatar_ = false;
        if (inside_self)
        {
            const tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (rect_contains(avatar_rect_, w) && on_avatar_clicked)
            {
                on_avatar_clicked(avatar_url_, display_name_);
            }
        }
        return;
    }

    if (press_backdrop_)
    {
        press_backdrop_ = false;
        if (inside_self)
        {
            const tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (!rect_contains(panel_rect_, w))
            {
                if (on_close) on_close();
            }
        }
    }

    if (press_member_ >= 0)
    {
        const int idx = press_member_;
        press_member_ = -1;
        if (inside_self && idx < static_cast<int>(members_.size()))
        {
            const tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (idx < static_cast<int>(member_rects_.size()) &&
                rect_contains(member_rects_[static_cast<std::size_t>(idx)], w))
            {
                const auto& mem = members_[static_cast<std::size_t>(idx)];
                if (on_member_clicked)
                {
                    on_member_clicked(mem.user_id, mem.display_name,
                                      mem.avatar_url);
                }
            }
        }
    }

    if (press_media_)
    {
        press_media_ = false;
        if (inside_self)
        {
            const tk::Point w{local.x + bounds().x, local.y + bounds().y};
            if (rect_contains(media_row_rect_, w) && on_media_view_requested)
            {
                on_media_view_requested(room_id_);
            }
        }
    }
}

bool RoomInfoPanel::on_pointer_move(tk::Point local)
{
    if (!open_) return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    // Topic tooltip: show the full topic when hovering an over-long (clipped)
    // topic. Mirrors RoomHeader's topic tooltip.
    const bool over_topic =
        !editing_topic_ && topic_truncated_ && rect_contains(topic_rect_, w);
    if (over_topic && !hover_topic_)
    {
        hover_topic_ = true;
        if (host_) host_->show_tooltip(this, topic_, topic_rect_);
    }
    else if (!over_topic && hover_topic_)
    {
        hover_topic_ = false;
        if (host_) host_->hide_tooltip(this);
    }

    // Cursor: pointer when hovering a link in the topic.
    std::string new_link_url;
    if (!editing_topic_ && topic_layout_ && rect_contains(topic_rect_, w))
    {
        const tk::Point ll{w.x - topic_rect_.x, w.y - topic_rect_.y};
        new_link_url = topic_layout_->link_at(ll);
    }
    const bool link_changed = (new_link_url != hover_link_url_);
    if (link_changed)
    {
        hover_link_url_ = new_link_url;
        if (on_link_hovered) on_link_hovered(hover_link_url_);
    }

    int prev_hover = hover_member_;
    hover_member_  = -1;

    for (int i = 0; i < static_cast<int>(member_rects_.size()); ++i)
    {
        if (rect_contains(member_rects_[static_cast<std::size_t>(i)], w))
        {
            hover_member_ = i;
            break;
        }
    }

    const bool prev_hover_media = hover_media_;
    hover_media_ = rect_contains(media_row_rect_, w);

    return hover_member_ != prev_hover || link_changed ||
           hover_media_ != prev_hover_media;
}

void RoomInfoPanel::on_pointer_leave()
{
    press_backdrop_ = false;
    press_avatar_   = false;
    hover_member_   = -1;
    press_member_   = -1;
    hover_media_    = false;
    press_media_    = false;
    if (hover_topic_ && host_) host_->hide_tooltip(this);
    hover_topic_    = false;
    if (!hover_link_url_.empty())
    {
        hover_link_url_.clear();
        if (on_link_hovered) on_link_hovered({});
    }
}

bool RoomInfoPanel::on_wheel(tk::Point local, float /*dx*/, float dy)
{
    if (!open_) return false;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(panel_rect_, w)) return false;

    scroll_offset_ += dy * 20.0f;
    scroll_offset_ = std::max(0.0f, scroll_offset_);
    if (on_layout_changed) on_layout_changed();
    return true;
}

} // namespace tesseract::views
