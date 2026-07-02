#include "RoomSettingsView.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

RoomSettingsChanges compute_room_settings_changes(
    const std::string& original_name, const std::string& staged_name,
    const std::string& original_topic, const std::string& staged_topic,
    const std::string& original_avatar_mxc,
    const std::string& staged_avatar_mxc)
{
    RoomSettingsChanges changes;
    if (staged_name != original_name)
        changes.name = staged_name;
    if (staged_topic != original_topic)
        changes.topic = staged_topic;
    if (staged_avatar_mxc != original_avatar_mxc)
        changes.avatar_mxc = staged_avatar_mxc;
    return changes;
}

RoomSettingsView::RoomSettingsView()
{
    accept_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Accept"), std::function<void()>{},
                                     tk::Button::Variant::Primary));
    cancel_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Cancel"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    accept_btn_->set_on_click([this]() {
        if (committing_) return;
        RoomSettingsChanges changes = compute_room_settings_changes(
            original_name_, staged_name_, original_topic_, staged_topic_,
            original_avatar_mxc_, staged_avatar_mxc_);
        committing_ = true;
        commit_error_.clear();
        commit_error_layout_.reset();
        accept_btn_->set_enabled(false);
        cancel_btn_->set_enabled(false);
        if (on_accept) on_accept(room_id_, std::move(changes));
    });
    cancel_btn_->set_on_click([this]() {
        if (committing_) return;
        if (on_cancel) on_cancel();
    });

    // Closed-by-default; same idiom as RoomInfoPanel/ConfirmDialog.
    set_visible(false);
}

void RoomSettingsView::open(const tesseract::RoomInfo& info)
{
    const bool was_open = open_;

    room_id_ = info.id;
    original_name_        = info.name;
    original_topic_       = info.topic;
    original_avatar_mxc_  = info.avatar_url;
    staged_name_          = original_name_;
    staged_topic_         = original_topic_;
    staged_avatar_mxc_    = original_avatar_mxc_;

    avatar_.set_avatar_url(staged_avatar_mxc_);
    avatar_.set_busy(false);
    avatar_.set_error("");
    avatar_.set_editable(false);

    can_name_ = can_topic_ = can_avatar_ = false;
    committing_ = false;
    commit_error_.clear();
    commit_error_layout_.reset();
    accept_btn_->set_enabled(true);
    cancel_btn_->set_enabled(true);

    title_layout_.reset();
    name_label_layout_.reset();
    name_static_layout_.reset();
    topic_label_layout_.reset();
    topic_static_layout_.reset();
    topic_natural_h_ = kFieldH;

    open_ = true;
    set_visible(true);

    if (!was_open && on_layout_changed) on_layout_changed();
}

void RoomSettingsView::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    if (was_open && on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_avatar_provider(ImageProvider p)
{
    avatar_.set_image_provider(std::move(p));
}

void RoomSettingsView::set_field_permissions(bool can_name, bool can_topic,
                                             bool can_avatar)
{
    can_name_   = can_name;
    can_topic_  = can_topic;
    can_avatar_ = can_avatar;
    avatar_.set_editable(can_avatar_ && !committing_);
    name_static_layout_.reset();
    topic_static_layout_.reset();
    if (on_layout_changed) on_layout_changed();
}

tk::Rect RoomSettingsView::name_field_rect() const
{
    if (!open_ || !can_name_ || committing_) return {};
    return name_rect_;
}

void RoomSettingsView::set_name_edit_text(std::string t)
{
    staged_name_ = std::move(t);
}

tk::Rect RoomSettingsView::topic_edit_rect() const
{
    if (!open_ || !can_topic_ || committing_) return {};
    return topic_rect_;
}

void RoomSettingsView::set_topic_edit_text(std::string t)
{
    staged_topic_ = std::move(t);
}

void RoomSettingsView::set_topic_area_natural_height(float h)
{
    topic_natural_h_ = std::clamp(h, kFieldH, kTopicMaxH);
    if (on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_staged_avatar(std::string mxc)
{
    staged_avatar_mxc_ = std::move(mxc);
    avatar_.set_avatar_url(staged_avatar_mxc_);
}

void RoomSettingsView::set_avatar_busy(bool busy)
{
    avatar_.set_busy(busy);
}

void RoomSettingsView::set_avatar_error(std::string error)
{
    avatar_.set_error(std::move(error));
}

void RoomSettingsView::set_commit_result(bool ok, std::string error)
{
    committing_ = false;
    if (ok)
    {
        close();
        return;
    }
    commit_error_ = std::move(error);
    commit_error_layout_.reset();
    accept_btn_->set_enabled(true);
    cancel_btn_->set_enabled(true);
    avatar_.set_editable(can_avatar_);
    if (on_layout_changed) on_layout_changed();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size RoomSettingsView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire replaced content area
}

void RoomSettingsView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    // Uses the full available width (minus side padding) rather than a
    // fixed, centered column.
    const float content_x = bounds.x + kPadX;
    const float content_w = std::max(0.0f, bounds.w - 2.0f * kPadX);

    float y = bounds.y + kPadY;

    // Title
    y += 28.0f + kPadY;

    // Avatar on the left; name/topic fields stacked in a column to its
    // right, both starting level with the top of this row.
    const tk::Point avatar_centre_local{
        (content_x - bounds.x) + kAvatarD * 0.5f, y - bounds.y + kAvatarD * 0.5f};
    avatar_.set_geometry(avatar_centre_local, kAvatarD);

    const float col_x = content_x + kAvatarD + kAvatarGap;
    const float col_w = std::max(0.0f, content_x + content_w - col_x);

    float cy = y;
    cy += kLabelH + kLabelGap;
    name_rect_ = {col_x, cy, col_w, kFieldH};
    cy += kFieldH + kFieldGap;

    cy += kLabelH + kLabelGap;
    // One line by default, grows with content up to kTopicMaxH, but never
    // past the space available above the button row.
    const float btns_y = bounds.y + bounds.h - kPadY - kBtnH;
    const float topic_h_cap = std::max(kFieldH, btns_y - kFieldGap - cy);
    const float topic_h = std::min(topic_natural_h_, topic_h_cap);
    topic_rect_ = {col_x, cy, col_w, topic_h};
    cy += topic_h;

    // Accept/Cancel — pinned to the bottom of the replaced content area,
    // right-aligned within the full content width (mirrors ConfirmDialog's
    // bottom-right footer).
    const float btn_w_min = 88.0f;
    tk::Size accept_sz = accept_btn_ ? accept_btn_->measure(lc, {-1.0f, kBtnH})
                                     : tk::Size{btn_w_min, kBtnH};
    tk::Size cancel_sz = cancel_btn_ ? cancel_btn_->measure(lc, {-1.0f, kBtnH})
                                     : tk::Size{btn_w_min, kBtnH};
    const float accept_w = std::max(accept_sz.w, btn_w_min);
    const float cancel_w = std::max(cancel_sz.w, btn_w_min);
    const float accept_x = content_x + content_w - accept_w;
    const float cancel_x = accept_x - kBtnGap - cancel_w;

    if (cancel_btn_)
        cancel_btn_->arrange(lc, {cancel_x, btns_y, cancel_w, kBtnH});
    if (accept_btn_)
        accept_btn_->arrange(lc, {accept_x, btns_y, accept_w, kBtnH});
}

// ── paint ─────────────────────────────────────────────────────────────────

void RoomSettingsView::paint(tk::PaintCtx& ctx)
{
    if (!open_) return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Opaque background — this view fully replaces the header/timeline/
    // composer, so (unlike RoomInfoPanel/ConfirmDialog) there is nothing
    // underneath to dim.
    cv.fill_rect(bounds_, pal.bg);

    const float content_x = bounds_.x + kPadX;
    const float content_w = std::max(0.0f, bounds_.w - 2.0f * kPadX);
    float y = bounds_.y + kPadY;

    // Title
    if (!title_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = content_w;
        title_layout_ = ctx.factory.build_text(tk::tr("Room Settings"), st);
    }
    if (title_layout_)
        cv.draw_text(*title_layout_, {content_x, y}, pal.text_primary);
    y += 28.0f + kPadY;

    // Avatar (left)
    {
        std::string_view name_source =
            staged_name_.empty() ? std::string_view("?")
                                 : std::string_view(staged_name_);
        avatar_.paint(ctx, {bounds_.x, bounds_.y}, name_source);
    }

    // Name/topic fields (right column, level with the top of the avatar).
    const float col_x = content_x + kAvatarD + kAvatarGap;
    const float col_w = std::max(0.0f, content_x + content_w - col_x);
    float cy = y;

    // Name label
    if (!name_label_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = col_w;
        name_label_layout_ = ctx.factory.build_text(tk::tr("Name"), st);
    }
    if (name_label_layout_)
        cv.draw_text(*name_label_layout_, {col_x, cy}, pal.text_muted);
    cy += kLabelH + kLabelGap;

    if (can_name_ && !committing_)
    {
        // Native overlay draws the live text; we just draw the underline.
        const float uly = cy + kFieldH - 1.0f;
        cv.fill_rect({col_x, uly, col_w, 1.0f}, pal.text_secondary.with_alpha(80));
    }
    else
    {
        if (!name_static_layout_ && !staged_name_.empty())
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = col_w;
            name_static_layout_ = ctx.factory.build_text(staged_name_, st);
        }
        if (name_static_layout_)
            cv.draw_text(*name_static_layout_, {col_x, cy}, pal.text_primary);
    }
    cy += kFieldH + kFieldGap;

    // Topic label
    if (!topic_label_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = col_w;
        topic_label_layout_ = ctx.factory.build_text(tk::tr("Topic"), st);
    }
    if (topic_label_layout_)
        cv.draw_text(*topic_label_layout_, {col_x, cy}, pal.text_muted);
    cy += kLabelH + kLabelGap;

    if (!can_topic_ || committing_)
    {
        if (!topic_static_layout_ && !staged_topic_.empty())
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.wrap      = true;
            st.max_width = col_w;
            topic_static_layout_ = ctx.factory.build_text(staged_topic_, st);
        }
        cv.push_clip_rect(topic_rect_);
        if (topic_static_layout_)
            cv.draw_text(*topic_static_layout_, {topic_rect_.x, topic_rect_.y},
                         pal.text_primary);
        cv.pop_clip();
    }
    // else: native NativeTextArea overlay covers topic_rect_ and draws the
    // live text itself — nothing to paint here (mirrors RoomInfoPanel's
    // editing_topic_ branch).

    // Commit error, shown just above the button row.
    if (!commit_error_.empty())
    {
        if (!commit_error_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.max_width = content_w;
            commit_error_layout_ = ctx.factory.build_text(commit_error_, st);
        }
        if (commit_error_layout_)
        {
            const float err_y = bounds_.y + bounds_.h - kPadY - kBtnH - 4.0f -
                                commit_error_layout_->measure().h;
            cv.draw_text(*commit_error_layout_, {content_x, err_y},
                         tk::Color::rgb(0xcc3333));
        }
    }

    if (cancel_btn_) cancel_btn_->paint(ctx);
    if (accept_btn_) accept_btn_->paint(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool RoomSettingsView::on_pointer_down(tk::Point local)
{
    if (!open_) return false;

    switch (avatar_.hit_test(local))
    {
    case AvatarEditControl::HitZone::RemoveChip:
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
        return true;
    case AvatarEditControl::HitZone::Disc:
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
        return true;
    case AvatarEditControl::HitZone::None:
        break;
    }
    // Let child dispatch handle the Accept/Cancel buttons.
    return false;
}

bool RoomSettingsView::on_pointer_move(tk::Point local)
{
    if (!open_) return false;
    return avatar_.on_pointer_move(local);
}

void RoomSettingsView::on_pointer_leave()
{
    avatar_.on_pointer_leave();
}

} // namespace tesseract::views
