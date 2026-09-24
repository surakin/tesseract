#include "RoomInfoPanel.h"
#include "html_spans.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/pill.h" // tk::role_line_metrics
#include "tk/theme.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace tesseract::views
{

// ─────────────────────────────────────────────────────────────────────────
//  RoomInfoPanelBody
// ─────────────────────────────────────────────────────────────────────────

RoomInfoPanelBody::RoomInfoPanelBody()
{
    if (host())
    {
        auto field = tk::create_widget<tk::TextArea>(this, kTopicEditH);
        field->set_visible(false);
        field->set_on_changed([this](const std::string& t) { topic_edit_text_ = t; });
        topic_field_ = add_child(std::move(field));
    }

    edit_topic_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "\xE2\x9C\x8E", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    edit_topic_btn_->set_icon(kEditSvg, 16.0f);
    edit_topic_btn_->set_accessible_name(tk::tr("Edit topic"));
    save_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "Save", std::function<void()>{},
                                     tk::Button::Variant::Primary));
    cancel_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "Cancel", std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    expand_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "Show all \xE2\x96\xBE", std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    invite_btn_ = add_child(
        tk::create_widget<tk::Button>(this, tk::tr("Invite people"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    export_btn_ = add_child(
        tk::create_widget<tk::Button>(this, tk::tr("Export History"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    leave_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "Leave Room", std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    // Favourite / Low-priority tag switches (mutually exclusive in the UI).
    favourite_btn_ = add_child(tk::create_widget<tk::SwitchButton>(this, "Favourite"));
    favourite_btn_->on_change = [this](bool on) {
        if (on && low_priority_btn_) low_priority_btn_->set_checked(false);
        if (on_favourite_changed) on_favourite_changed(room_id_, on);
        if (on_layout_changed) on_layout_changed(); // repaint both switches
    };

    low_priority_btn_ = add_child(tk::create_widget<tk::SwitchButton>(this, "Low priority"));
    low_priority_btn_->on_change = [this](bool on) {
        if (on && favourite_btn_) favourite_btn_->set_checked(false);
        if (on_low_priority_changed) on_low_priority_changed(room_id_, on);
        if (on_layout_changed) on_layout_changed();
    };

    // ComboBox is added last so it is dispatched first in reverse child order,
    // ensuring its expanded dropdown captures pointer events before leave_btn_.
    auto notif_combo = tk::create_widget<tk::ComboBox>(this);
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

    edit_topic_btn_->set_on_click([this]() {
        editing_topic_   = true;
        topic_edit_text_ = topic_;
        if (topic_field_) topic_field_->set_text(topic_edit_text_);
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
    export_btn_->set_on_click([this]() {
        if (on_export_history_requested) on_export_history_requested(room_id_);
    });
    invite_btn_->set_on_click([this]() {
        if (on_invite_requested) on_invite_requested(room_id_);
    });
    // Hidden until the shell confirms the user may invite — see
    // set_invite_visible().
    invite_btn_->set_visible(false);

    // save, cancel, and expand are hidden until needed
    save_btn_->set_visible(false);
    cancel_btn_->set_visible(false);
    expand_btn_->set_visible(false);
}

void RoomInfoPanelBody::open(const tesseract::RoomInfo& info)
{
    room_id_            = info.id;
    display_name_       = info.name;
    avatar_url_         = info.effective_avatar_url();
    topic_              = info.topic;
    topic_html_         = info.topic_html;
    is_encrypted_       = info.is_encrypted;
    history_visibility_ = info.history_visibility;
    is_bridged_         = info.is_bridged && !info.bridge_overridden;
    bridge_network_name_       = info.bridge_network_name;
    bridge_network_avatar_url_ = info.bridge_network_avatar_url;

    open_             = true;
    editing_topic_    = false;
    topic_edit_text_  = {};
    members_expanded_ = false;
    scroll_y_         = 0.0f;

    members_.clear();
    member_layouts_.clear();
    member_rects_.clear();

    name_layout_.reset();
    badge_enc_layout_.reset();
    badge_hist_layout_.reset();
    badge_bridged_layout_.reset();
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
}

void RoomInfoPanelBody::refresh_info(const tesseract::RoomInfo& info)
{
    if (!open_) return;
    display_name_       = info.name;
    avatar_url_         = info.effective_avatar_url();
    is_encrypted_       = info.is_encrypted;
    history_visibility_ = info.history_visibility;
    is_bridged_         = info.is_bridged && !info.bridge_overridden;
    bridge_network_name_       = info.bridge_network_name;
    bridge_network_avatar_url_ = info.bridge_network_avatar_url;
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

void RoomInfoPanelBody::close()
{
    open_ = false;
    // The outer panel becomes invisible on close, which skips this widget in
    // the parent's arrange() traversal — arrange() won't run again to hide
    // topic_field_ itself, so hide it directly here in case of a mid-edit close.
    editing_topic_ = false;
    if (topic_field_) topic_field_->set_visible(false);
}

void RoomInfoPanelBody::set_avatar_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void RoomInfoPanelBody::set_presence_provider(PresenceProvider p)
{
    presence_provider_ = std::move(p);
}

void RoomInfoPanelBody::set_members(std::vector<tesseract::RoomMember> members)
{
    members_ = std::move(members);
    std::sort(members_.begin(), members_.end(),
        [](const tesseract::RoomMember& a, const tesseract::RoomMember& b) {
            if (a.power_level != b.power_level) return a.power_level > b.power_level;
            return std::lexicographical_compare(
                a.display_name.begin(), a.display_name.end(),
                b.display_name.begin(), b.display_name.end(),
                [](unsigned char x, unsigned char y) {
                    return std::tolower(x) < std::tolower(y);
                });
        });
    member_layouts_.clear();
    member_rects_.clear();

    const int total = static_cast<int>(members_.size());
    expand_btn_->set_label(
        std::string("Show all (") + std::to_string(total) + ") \xE2\x96\xBE");

    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanelBody::set_notification_mode(std::string mode)
{
    if (!open_) return;
    if (notification_combo_) notification_combo_->set_selected_value(mode);
}

void RoomInfoPanelBody::set_media_count(int count)
{
    if (count == media_count_) return;
    media_count_ = count;
    media_row_layout_.reset();
    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanelBody::set_knock_requests_visible(bool visible)
{
    if (visible == knock_row_visible_) return;
    knock_row_visible_ = visible;
    knock_row_layout_.reset();
    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanelBody::set_invite_visible(bool visible)
{
    if (!invite_btn_ || visible == invite_btn_->own_visible()) return;
    invite_btn_->set_visible(visible);
    if (on_layout_changed) on_layout_changed();
}

void RoomInfoPanelBody::on_theme_changed(const tk::Theme& t)
{
    // topic_field_ sits directly on this widget's own fill, no separate
    // inset fill behind topic_rect_ — see Widget::background_color()'s doc
    // comment. Must be set here (topic_field_'s direct parent) rather than
    // on the outer RoomInfoPanel for TextArea's ancestor lookup to find it.
    set_background_color(t.palette.chrome_bg);
    if (topic_field_) topic_field_->set_text_color(t.palette.text_primary);
    if (edit_topic_btn_) edit_topic_btn_->set_icon_color_override(t.palette.text_secondary);
}

// ── layout ────────────────────────────────────────────────────────────────

float RoomInfoPanelBody::measure_topic_height_(tk::CanvasFactory& factory, float max_w)
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

tk::Size RoomInfoPanelBody::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void RoomInfoPanelBody::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;
    clamp_scroll(); // clamps scroll_y_ against content_height_ from the previous pass

    const float px = bounds_.x;
    const float iw = kPanelW - kPadX * 2.0f;
    const float origin_y = bounds_.y - scroll_y_;

    // y is a CONTENT-LOCAL cursor (0 == top of unscrolled content). Direct-
    // painted rects are stored content-local (converted to world only at
    // paint/hit-test time); widget children need real WORLD rects for their
    // own arrange(), so `origin_y + y` bakes the current scroll in — a full
    // relayout runs on every scroll change (wheel or scrollbar drag, see
    // on_wheel/on_pointer_drag), so this is never stale.
    float y = 0.0f;

    // Avatar circle (72×72), centred in panel
    avatar_rect_ = {(kPanelW - kAvatarD) * 0.5f, y, kAvatarD, kAvatarD};
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
    topic_rect_ = {kPadX, y, iw, topic_h};
    y += topic_h + 4.0f;

    if (topic_field_)
    {
        topic_field_->set_visible(editing_topic_);
        if (editing_topic_)
            topic_field_->arrange(lc, {px + topic_rect_.x, origin_y + topic_rect_.y,
                                       topic_rect_.w, topic_rect_.h});
    }

    // Edit topic button: 28×28 to the right of the topic header
    if (edit_topic_btn_)
    {
        const float ebx = px + kPanelW - kPadX - kSmallEditH;
        const float eby = topic_rect_.y - 12.0f - kSmallEditH;
        edit_topic_btn_->arrange(lc, {ebx, origin_y + eby, kSmallEditH, kSmallEditH});
        edit_topic_btn_->set_visible(!editing_topic_);
    }

    // Save + Cancel buttons (visible only when editing)
    const bool show_edit_btns = editing_topic_;
    if (save_btn_)
    {
        save_btn_->set_visible(show_edit_btns);
        if (show_edit_btns)
            save_btn_->arrange(lc, {px + kPadX, origin_y + y, iw * 0.5f - 4.0f, kSmallEditH});
    }
    if (cancel_btn_)
    {
        cancel_btn_->set_visible(show_edit_btns);
        if (show_edit_btns)
            cancel_btn_->arrange(lc, {px + kPadX + iw * 0.5f + 4.0f, origin_y + y,
                                      iw * 0.5f - 4.0f, kSmallEditH});
    }
    if (show_edit_btns)
        y += kSmallEditH + kPadY;

    // Separator + Favourite / Low-priority switch rows — between topic & members.
    tags_sep_y_ = y;
    y += 1.0f + kPadY;
    tags_row_y_ = y;
    if (favourite_btn_)
        favourite_btn_->arrange(lc, {px + kPadX, origin_y + y, iw, kButtonH});
    if (low_priority_btn_)
        low_priority_btn_->arrange(lc, {px + kPadX, origin_y + y + kButtonH, iw, kButtonH});
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
        member_rects_.push_back({0.0f, y, kPanelW, kMemberRowH});
        y += kMemberRowH;
    }

    // Expand button (shown when >5 members and not expanded)
    const bool show_expand = !members_expanded_ && total_members > 5;
    if (expand_btn_)
    {
        expand_btn_->set_visible(show_expand);
        if (show_expand)
        {
            expand_btn_->arrange(lc, {px + kPadX, origin_y + y, iw, kButtonH});
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
            notification_combo_->arrange(lc, {px + kPadX, origin_y + y, iw, 32.0f});
            y += 32.0f + kPadY;
        }
    }

    // "Media (N)" row — direct-painted/hit-tested like the member rows
    // (see on_pointer_down/up/move). No popup of its own, so it doesn't
    // participate in the leave/combo paint-order inversion below.
    media_row_rect_ = {0.0f, y, kPanelW, kMediaRowH};
    y += kMediaRowH + kPadY;

    // "Requests to join (N)" row (MSC2403) — same direct-painted/hit-tested
    // treatment, only present when the shell has determined the room is
    // knock/knock_restricted and the current user can moderate it.
    knock_row_rect_ = {};
    if (knock_row_visible_)
    {
        knock_row_rect_ = {0.0f, y, kPanelW, kMediaRowH};
        y += kMediaRowH + kPadY;
    }

    // Invite button: first of the action buttons, only when the user may
    // invite (see set_invite_visible()).
    if (invite_btn_ && invite_btn_->own_visible())
    {
        const float invite_y = y + kPadY;
        invite_btn_->arrange(lc, {px + kPadX, origin_y + invite_y, iw, kButtonH});
        y = invite_y + kButtonH;
    }

    // Export History button: flows after content, above the leave button
    // (export is non-destructive; leave is not — the more dangerous action
    // sits lower/last).
    if (export_btn_)
    {
        const float export_y = y + kPadY;
        export_btn_->arrange(lc, {px + kPadX, origin_y + export_y, iw, kButtonH});
        y = export_y + kButtonH;
    }

    // Leave button: flows after content, never overlaps the member list.
    if (leave_btn_)
    {
        const float leave_y = y + kPadY;
        leave_btn_->arrange(lc, {px + kPadX, origin_y + leave_y, iw, kButtonH});
        y = leave_y + kButtonH + kPadY;
    }

    // content_height_: natural, unscrolled height of the whole content column.
    content_height_ = y;
}

// ── paint ─────────────────────────────────────────────────────────────────

// Genuine override, kept intentionally (see the paint_children()-automation
// work in ui/shared/tk/widget.h): badges, topic text/edit-toggle, member
// rows, and separators are all hand-drawn procedurally and interleaved with
// conditional child-button painting (edit_topic_btn_ only when
// !editing_topic_, etc.) throughout a single clipped region — this is the
// most complex case in the codebase and doesn't reduce to a fixed
// before/after split by itself, which is why it's paint_before_children()
// (direct-painted content) + the base class's own paint_children() (widget
// children, already positioned in world space by arrange() above) +
// paint_after_children() (pop the clip, paint the scrollbar thumb).
void RoomInfoPanelBody::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_) return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(bounds_, pal.chrome_bg);
    cv.push_clip_rect(bounds_);

    const float origin_y = bounds_.y - scroll_y_;
    const auto to_world = [&](tk::Rect r) {
        return tk::Rect{bounds_.x + r.x, origin_y + r.y, r.w, r.h};
    };

    // Avatar
    const tk::Rect avatar_w = to_world(avatar_rect_);
    const tk::Point av_centre{avatar_w.x + kAvatarD * 0.5f, avatar_w.y + kAvatarD * 0.5f};
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

    // Room name (Title, centred, ellipsis)
    const float text_max_w = kPanelW - kPadX * 2.0f;
    const float name_y     = avatar_w.y + kAvatarD + kPadY;

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
        const float tx    = bounds_.x + (kPanelW - sz.w) * 0.5f;
        cv.draw_text(*name_layout_, {tx, name_y}, pal.text_primary);
    }

    // Badge row
    const float badge_y = name_y + 20.0f + 4.0f;
    float badge_x = bounds_.x + kPadX;

    // Every badge is an icon+label pair drawn the same way: a real Lucide
    // glyph (via IconCache, rasterized/tinted/cached) in a box the height of
    // one Small-role text line, then the label immediately after it. Sharing
    // one box height and one icon size across all three is what makes them
    // line up with each other — no per-badge guessing.
    constexpr float kBadgeIconPx  = 14.0f;
    constexpr float kBadgeIconGap = 4.0f;
    const tk::LineMetrics badge_lm =
        tk::role_line_metrics(ctx.factory, tk::FontRole::Small);
    const float badge_row_h =
        (badge_lm.ascent + badge_lm.descent) > 0.0f
            ? (badge_lm.ascent + badge_lm.descent)
            : 16.0f;

    if (is_encrypted_)
    {
        if (!badge_enc_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_max_w;
            badge_enc_layout_ = ctx.factory.build_text(tk::tr("Encrypted"), st);
        }
        badge_enc_icon_.draw(cv, ctx.factory, kLockKeyholeSvg,
                             {badge_x, badge_y, kBadgeIconPx, badge_row_h},
                             kBadgeIconPx, pal.text_muted);
        badge_x += kBadgeIconPx + kBadgeIconGap;
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
            badge_hist_layout_ = ctx.factory.build_text(history_visibility_, st);
        }
        badge_hist_icon_.draw(cv, ctx.factory, kEyeSvg,
                              {badge_x, badge_y, kBadgeIconPx, badge_row_h},
                              kBadgeIconPx, pal.text_muted);
        badge_x += kBadgeIconPx + kBadgeIconGap;
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
            badge_bridged_layout_ = ctx.factory.build_text(
                bridge_network_name_.empty() ? tk::tr("Bridged") : bridge_network_name_, st);
        }
        // Prefer the bridged network's own avatar (e.g. WhatsApp's logo)
        // over the generic cable glyph when one's resolved — drawn in the
        // exact same box the icon would occupy, so it lines up identically
        // either way.
        const tk::Image* network_img =
            (image_provider_ && !bridge_network_avatar_url_.empty())
                ? image_provider_(bridge_network_avatar_url_)
                : nullptr;
        if (network_img)
        {
            const tk::Point icon_centre{badge_x + kBadgeIconPx * 0.5f,
                                        badge_y + badge_row_h * 0.5f};
            cv.draw_circle_image(*network_img, icon_centre, kBadgeIconPx);
        }
        else
        {
            badge_bridged_icon_.draw(cv, ctx.factory, kCableSvg,
                                     {badge_x, badge_y, kBadgeIconPx, badge_row_h},
                                     kBadgeIconPx, pal.text_muted);
        }
        badge_x += kBadgeIconPx + kBadgeIconGap;
        if (badge_bridged_layout_)
            cv.draw_text(*badge_bridged_layout_, {badge_x, badge_y}, pal.text_muted);
    }

    // Separator before Topic section
    const float sep1_y = badge_y + 16.0f + kPadY * 0.5f;
    cv.fill_rect({bounds_.x + kPadX, sep1_y, kPanelW - kPadX * 2.0f, 1.0f},
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
            cv.draw_text(*lbl, {bounds_.x + kPadX, section_topic_y},
                         pal.text_muted);
        }
    }

    // Edit topic button (only when not editing)
    if (edit_topic_btn_ && !editing_topic_)
    {
        edit_topic_btn_->paint(ctx);
    }

    const tk::Rect topic_rect_w = to_world(topic_rect_);

    // Topic text (drawn when not editing)
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
        ctx.canvas.push_clip_rect(topic_rect_w);
        if (topic_layout_)
        {
            cv.draw_text(*topic_layout_,
                         {topic_rect_w.x, topic_rect_w.y},
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
                cv.draw_text(*lbl, {topic_rect_w.x, topic_rect_w.y},
                             pal.text_muted);
            }
        }
        ctx.canvas.pop_clip();
    }
    else if (topic_field_ && topic_field_->visible())
    {
        topic_field_->paint(ctx);
    }

    // Save + Cancel when editing
    if (save_btn_ && editing_topic_)   save_btn_->paint(ctx);
    if (cancel_btn_ && editing_topic_) cancel_btn_->paint(ctx);

    // Separator + Favourite / Low-priority switch rows (between topic and members).
    cv.fill_rect({bounds_.x + kPadX, origin_y + tags_sep_y_, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.separator);
    if (favourite_btn_)    favourite_btn_->paint(ctx);
    if (low_priority_btn_) low_priority_btn_->paint(ctx);

    // Separator before Members section. Mirror arrange(): topic region,
    // then (when editing) the edit buttons, then the switch separator +
    // the two switch rows.
    float cur_y = topic_rect_.y + topic_rect_.h + 4.0f;
    if (editing_topic_)
    {
        cur_y += kSmallEditH + kPadY;
    }
    cur_y += 1.0f + kPadY + 2.0f * kButtonH + kPadY; // switch separator + rows
    cv.fill_rect({bounds_.x + kPadX, origin_y + cur_y, kPanelW - kPadX * 2.0f, 1.0f},
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
            cv.draw_text(*lbl, {bounds_.x + kPadX, origin_y + section_mem_y},
                         pal.text_muted);
        }
    }

    // Member rows
    // Rebuild member layouts lazily as the cache may be empty
    const int visible_count = static_cast<int>(member_rects_.size());
    if (static_cast<int>(member_layouts_.size()) < static_cast<int>(members_.size()))
    {
        member_layouts_.resize(members_.size());
    }

    for (int i = 0; i < visible_count; ++i)
    {
        const tk::Rect row = to_world(member_rects_[static_cast<std::size_t>(i)]);
        const auto& mem    = members_[static_cast<std::size_t>(i)];

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

    // Expand button
    if (expand_btn_ && expand_btn_->visible()) expand_btn_->paint(ctx);

    // Notifications separator + section header
    cv.fill_rect({bounds_.x + kPadX, origin_y + notif_sep_y_, kPanelW - kPadX * 2.0f, 1.0f},
                 pal.separator);
    {
        const float hdr_y = origin_y + notif_sep_y_ + 1.0f + kPadY;
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = kPanelW - kPadX * 2.0f;
        auto lbl = ctx.factory.build_text("Notifications", st);
        if (lbl)
            cv.draw_text(*lbl, {bounds_.x + kPadX, hdr_y}, pal.text_muted);
    }

    // "Media (N)" row — plain clickable text row, no chrome, matching the
    // member rows rather than a bordered tk::Button (it navigates to the
    // gallery rather than performing an in-place action).
    {
        const tk::Rect media_row_w = to_world(media_row_rect_);
        if (hover_media_)
            cv.fill_rect(media_row_w, pal.subtle_hover);
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
            const float ty = media_row_w.y +
                             (kMediaRowH - media_row_layout_->measure().h) * 0.5f;
            cv.draw_text(*media_row_layout_, {bounds_.x + kPadX, ty},
                         pal.text_primary);
        }
    }

    // "Requests to join (N)" row (MSC2403) — same treatment as the
    // "Media (N)" row above; absent entirely when knock_row_visible_ is false.
    if (knock_row_visible_)
    {
        const tk::Rect knock_row_w = to_world(knock_row_rect_);
        if (hover_knock_)
            cv.fill_rect(knock_row_w, pal.subtle_hover);
        if (!knock_row_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.halign    = tk::TextHAlign::Leading;
            st.max_width = kPanelW - kPadX * 2.0f;
            knock_row_layout_ = ctx.factory.build_text(tk::tr("Requests to join"), st);
        }
        if (knock_row_layout_)
        {
            const float ty = knock_row_w.y +
                             (kMediaRowH - knock_row_layout_->measure().h) * 0.5f;
            cv.draw_text(*knock_row_layout_, {bounds_.x + kPadX, ty},
                         pal.text_primary);
        }
    }

    // Export History + Leave buttons (painted before the notification combo
    // so the combo's expanded dropdown overlays them when open)
    if (invite_btn_ && invite_btn_->own_visible()) invite_btn_->paint(ctx);
    if (export_btn_) export_btn_->paint(ctx);
    if (leave_btn_) leave_btn_->paint(ctx);

    // Notification combo — painted last so its dropdown overlays leave btn
    if (notification_combo_) notification_combo_->paint(ctx);
}

void RoomInfoPanelBody::paint_after_children(tk::PaintCtx& ctx)
{
    if (!open_) return;
    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool RoomInfoPanelBody::on_pointer_down(tk::Point local)
{
    if (!open_) return false;

    if (scrollbar_on_pointer_down(local))
        return true;

    // Convert viewport-local `local` into content-space (unscrolled) to
    // compare against the content-local rects computed in arrange().
    const tk::Point c{local.x, local.y + scroll_y_};

    // Avatar click → open lightbox. Falls through when no URL or no
    // callback so initials-only rooms keep the current no-op behaviour.
    if (rect_contains(avatar_rect_, c) && !avatar_url_.empty() && on_avatar_clicked)
    {
        press_avatar_ = true;
        return true;
    }
    // Hit-test topic links before member rows.
    if (topic_layout_ && rect_contains(topic_rect_, c))
    {
        const tk::Point ll{c.x - topic_rect_.x, c.y - topic_rect_.y};
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
        if (rect_contains(member_rects_[static_cast<std::size_t>(i)], c))
        {
            press_member_ = i;
            return true;
        }
    }
    if (rect_contains(media_row_rect_, c))
    {
        press_media_ = true;
        return true;
    }
    if (knock_row_visible_ && rect_contains(knock_row_rect_, c))
    {
        press_knock_ = true;
        return true;
    }
    // Let child dispatch handle button events inside the panel.
    return false;
}

void RoomInfoPanelBody::on_pointer_up(tk::Point local, bool inside_self)
{
    if (scrollbar_on_pointer_up())
        return;

    const tk::Point c{local.x, local.y + scroll_y_};

    if (!press_link_url_.empty())
    {
        std::string url = std::move(press_link_url_);
        press_link_url_.clear();
        if (inside_self && on_link_clicked)
        {
            if (rect_contains(topic_rect_, c) && topic_layout_)
            {
                const tk::Point ll{c.x - topic_rect_.x, c.y - topic_rect_.y};
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
            if (rect_contains(avatar_rect_, c) && on_avatar_clicked)
            {
                on_avatar_clicked(avatar_url_, display_name_);
            }
        }
        return;
    }

    if (press_member_ >= 0)
    {
        const int idx = press_member_;
        press_member_ = -1;
        if (inside_self && idx < static_cast<int>(members_.size()))
        {
            if (idx < static_cast<int>(member_rects_.size()) &&
                rect_contains(member_rects_[static_cast<std::size_t>(idx)], c))
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
            if (rect_contains(media_row_rect_, c) && on_media_view_requested)
            {
                on_media_view_requested(room_id_);
            }
        }
    }

    if (press_knock_)
    {
        press_knock_ = false;
        if (inside_self)
        {
            if (knock_row_visible_ && rect_contains(knock_row_rect_, c) &&
                on_knock_requests_view_requested)
            {
                on_knock_requests_view_requested(room_id_);
            }
        }
    }
}

void RoomInfoPanelBody::on_pointer_drag(tk::Point local)
{
    if (scrollbar_on_pointer_drag(local))
    {
        if (on_layout_changed) on_layout_changed();
    }
}

bool RoomInfoPanelBody::on_pointer_move(tk::Point local)
{
    if (!open_) return false;

    const tk::Point c{local.x, local.y + scroll_y_};

    // Topic tooltip: show the full topic when hovering an over-long (clipped)
    // topic. Mirrors RoomHeader's topic tooltip.
    const bool over_topic =
        !editing_topic_ && topic_truncated_ && rect_contains(topic_rect_, c);
    if (over_topic && !hover_topic_)
    {
        hover_topic_ = true;
        if (host())
        {
            const float origin_y = bounds_.y - scroll_y_;
            const tk::Rect topic_rect_w{bounds_.x + topic_rect_.x, origin_y + topic_rect_.y,
                                        topic_rect_.w, topic_rect_.h};
            host()->show_tooltip(this, topic_, topic_rect_w);
        }
    }
    else if (!over_topic && hover_topic_)
    {
        hover_topic_ = false;
        if (host()) host()->hide_tooltip(this);
    }

    // Cursor: pointer when hovering a link in the topic.
    std::string new_link_url;
    if (!editing_topic_ && topic_layout_ && rect_contains(topic_rect_, c))
    {
        const tk::Point ll{c.x - topic_rect_.x, c.y - topic_rect_.y};
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
        if (rect_contains(member_rects_[static_cast<std::size_t>(i)], c))
        {
            hover_member_ = i;
            break;
        }
    }

    const bool prev_hover_media = hover_media_;
    hover_media_ = rect_contains(media_row_rect_, c);

    const bool prev_hover_knock = hover_knock_;
    hover_knock_ = knock_row_visible_ && rect_contains(knock_row_rect_, c);

    return hover_member_ != prev_hover || link_changed ||
           hover_media_ != prev_hover_media ||
           hover_knock_ != prev_hover_knock;
}

void RoomInfoPanelBody::on_pointer_leave()
{
    press_avatar_   = false;
    hover_member_   = -1;
    press_member_   = -1;
    hover_media_    = false;
    press_media_    = false;
    hover_knock_    = false;
    press_knock_    = false;
    if (hover_topic_ && host()) host()->hide_tooltip(this);
    hover_topic_    = false;
    if (!hover_link_url_.empty())
    {
        hover_link_url_.clear();
        if (on_link_hovered) on_link_hovered({});
    }
}

bool RoomInfoPanelBody::on_wheel(tk::Point /*local*/, float /*dx*/, float dy, bool /*is_touchpad*/)
{
    if (!open_) return false;
    const float prev = scroll_y_;
    scroll_y_ += dy;
    clamp_scroll();
    if (scroll_y_ != prev && on_layout_changed) on_layout_changed();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
//  RoomInfoPanel
// ─────────────────────────────────────────────────────────────────────────

RoomInfoPanel::RoomInfoPanel()
{
    body_ = add_child(tk::create_widget<RoomInfoPanelBody>(this));
    body_->on_layout_changed = [this]() { if (on_layout_changed) on_layout_changed(); };
    body_->on_fetch_notification_mode = [this](std::string room_id) {
        if (on_fetch_notification_mode) on_fetch_notification_mode(std::move(room_id));
    };
    body_->on_notification_mode_changed = [this](std::string room_id, std::string mode) {
        if (on_notification_mode_changed)
            on_notification_mode_changed(std::move(room_id), std::move(mode));
    };
    body_->on_favourite_changed = [this](std::string room_id, bool on) {
        if (on_favourite_changed) on_favourite_changed(std::move(room_id), on);
    };
    body_->on_low_priority_changed = [this](std::string room_id, bool on) {
        if (on_low_priority_changed) on_low_priority_changed(std::move(room_id), on);
    };
    body_->on_fetch_members = [this](std::string room_id) {
        if (on_fetch_members) on_fetch_members(std::move(room_id));
    };
    body_->on_save_topic = [this](std::string room_id, std::string t) {
        if (on_save_topic) on_save_topic(std::move(room_id), std::move(t));
    };
    body_->on_invite_requested = [this](std::string room_id) {
        if (on_invite_requested) on_invite_requested(std::move(room_id));
    };
    body_->on_export_history_requested = [this](std::string room_id) {
        if (on_export_history_requested) on_export_history_requested(std::move(room_id));
    };
    body_->on_media_view_requested = [this](std::string room_id) {
        if (on_media_view_requested) on_media_view_requested(std::move(room_id));
    };
    body_->on_member_clicked = [this](std::string user_id, std::string display_name,
                                      std::string avatar_url) {
        if (on_member_clicked)
            on_member_clicked(std::move(user_id), std::move(display_name),
                             std::move(avatar_url));
    };
    body_->on_member_avatar_needed = [this](const tesseract::RoomMember& m) {
        if (on_member_avatar_needed) on_member_avatar_needed(m);
    };
    body_->on_avatar_clicked = [this](std::string avatar_url, std::string display_name) {
        if (on_avatar_clicked)
            on_avatar_clicked(std::move(avatar_url), std::move(display_name));
    };
    body_->on_link_clicked = [this](std::string url) {
        if (on_link_clicked) on_link_clicked(std::move(url));
    };
    body_->on_link_hovered = [this](std::string url) {
        if (on_link_hovered) on_link_hovered(std::move(url));
    };
    body_->on_knock_requests_view_requested = [this](std::string room_id) {
        if (on_knock_requests_view_requested)
            on_knock_requests_view_requested(std::move(room_id));
    };
    body_->on_leave_room = [this](std::string room_id) {
        if (on_leave_room) on_leave_room(std::move(room_id));
    };

    close_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "\xC3\x97", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    close_btn_->set_icon(kCloseSvg, 16.0f);
    close_btn_->set_accessible_name(tk::tr("Close"));
    settings_btn_ = add_child(
        tk::create_widget<tk::Button>(this, "\xF0\x9F\x94\xA7", std::function<void()>{},
                                     tk::Button::Variant::Icon));
    settings_btn_->set_icon(kWrenchSvg, 16.0f);
    settings_btn_->set_accessible_name(tk::tr("Room settings"));

    close_btn_->set_on_click([this]() {
        if (on_close) on_close();
    });
    settings_btn_->set_on_click([this]() {
        if (on_room_settings_requested) on_room_settings_requested();
    });

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
    open_ = true;
    set_visible(true);
    if (body_) body_->open(info);
    // Fire the layout-changed callback so the shell hides native overlays
    // (compose textarea, room search) while the panel covers the canvas.
    if (!was_open && on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::refresh_info(const tesseract::RoomInfo& info)
{
    if (body_) body_->refresh_info(info);
}

void RoomInfoPanel::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    if (body_) body_->close();
    if (was_open && on_layout_changed) on_layout_changed();
}

void RoomInfoPanel::set_avatar_provider(ImageProvider p)
{
    if (body_) body_->set_avatar_provider(std::move(p));
}

void RoomInfoPanel::set_presence_provider(PresenceProvider p)
{
    if (body_) body_->set_presence_provider(std::move(p));
}

void RoomInfoPanel::set_members(std::vector<tesseract::RoomMember> members)
{
    if (body_) body_->set_members(std::move(members));
}

void RoomInfoPanel::set_notification_mode(std::string mode)
{
    if (body_) body_->set_notification_mode(std::move(mode));
}

void RoomInfoPanel::set_media_count(int count)
{
    if (body_) body_->set_media_count(count);
}

void RoomInfoPanel::set_knock_requests_visible(bool visible)
{
    if (body_) body_->set_knock_requests_visible(visible);
}

void RoomInfoPanel::set_invite_visible(bool visible)
{
    if (body_) body_->set_invite_visible(visible);
}

tk::TextArea* RoomInfoPanel::topic_field() const
{
    return body_ ? body_->topic_field() : nullptr;
}

std::string RoomInfoPanel::access_name() const
{
    if (!body_) return {};
    const std::string& dn = body_->display_name();
    const std::string& tp = body_->topic();
    return tp.empty() ? dn : dn + ": " + tp;
}

void RoomInfoPanel::on_theme_changed(const tk::Theme& t)
{
    // The close/settings icon glyphs are always text_secondary (never the
    // enabled/disabled default Button::paint() would otherwise apply), so
    // re-pin the override whenever the theme (and thus the palette colour
    // behind it) changes.
    if (close_btn_)    close_btn_->set_icon_color_override(t.palette.text_secondary);
    if (settings_btn_) settings_btn_->set_icon_color_override(t.palette.text_secondary);

    // Give them an opaque, theme-matched fill (rather than the default
    // Icon-variant's transparent-at-rest fill), computed as if
    // palette.subtle_hover/subtle_pressed had been alpha-composited over
    // chrome_bg — same visual result those overlays produce elsewhere, just
    // pre-flattened to opaque, since these sit in the fixed header and don't
    // need to let anything show through.
    const auto composite_over_chrome = [&](tk::Color overlay) {
        return tk::Color::lerp(t.palette.chrome_bg, overlay.with_alpha(255),
                               static_cast<float>(overlay.a) / 255.0f);
    };
    const tk::Button::FillOverride header_btn_fill{
        t.palette.chrome_bg,
        composite_over_chrome(t.palette.subtle_hover),
        composite_over_chrome(t.palette.subtle_pressed),
    };
    if (close_btn_)    close_btn_->set_fill_override(header_btn_fill);
    if (settings_btn_) settings_btn_->set_fill_override(header_btn_fill);
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size RoomInfoPanel::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void RoomInfoPanel::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    // Not tk::Widget::arrange(lc, bounds) — it would recursively arrange
    // body_ with the full, un-inset `bounds` first (before the real,
    // header-excluding sub-rect below), corrupting its scroll_y_ clamp.
    // Same fix as ImagePackEditorView::arrange()/RoomSettingsView::arrange().
    bounds_ = bounds;

    backdrop_rect_ = bounds;
    panel_rect_    = {bounds.x + bounds.w - kPanelW, bounds.y,
                      kPanelW, bounds.h};

    const float px = panel_rect_.x;

    // Settings (wrench) and Close buttons: fixed at the top of the panel,
    // never scroll. Settings sits top-left, Close top-right.
    if (settings_btn_)
        settings_btn_->arrange(lc, {px + 8.0f, panel_rect_.y + 8.0f,
                                    kHeaderBtnSz, kHeaderBtnSz});
    if (close_btn_)
        close_btn_->arrange(lc, {px + kPanelW - 8.0f - kHeaderBtnSz, panel_rect_.y + 8.0f,
                                 kHeaderBtnSz, kHeaderBtnSz});

    // Everything below the header strip scrolls — body_ owns and clips it.
    const float body_y = panel_rect_.y + kHeaderBarH;
    const float body_h = std::max(0.0f, panel_rect_.h - kHeaderBarH);
    if (body_) body_->arrange(lc, {px, body_y, kPanelW, body_h});
}

void RoomInfoPanel::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_) return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Semi-transparent backdrop
    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 100});

    // Panel background
    cv.fill_rect(panel_rect_, pal.chrome_bg);

    // 1px left border
    cv.fill_rect({panel_rect_.x, panel_rect_.y, 1.0f, panel_rect_.h},
                 pal.separator);
}

// ── pointer events ────────────────────────────────────────────────────────

bool RoomInfoPanel::on_pointer_down(tk::Point local)
{
    if (!open_) return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (rect_contains(panel_rect_, w))
    {
        // Let child dispatch (body_/settings_btn_/close_btn_) handle it.
        return false;
    }

    // Backdrop click: consume and remember for on_pointer_up
    press_backdrop_ = true;
    return true;
}

void RoomInfoPanel::on_pointer_up(tk::Point local, bool inside_self)
{
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
}

} // namespace tesseract::views
