#include "RoomGeneralSection.h"

#include "tk/i18n.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/AvatarEditControl.h"
#include "views/media_utils.h" // rect_contains

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tesseract::views
{

namespace
{

constexpr float kAvatarD   = 96.0f;
constexpr float kRoomGeneralAvatarGap = 24.0f; // avatar -> fields column
constexpr float kRoomGeneralPadX      = 24.0f;
constexpr float kRoomGeneralPadY      = 24.0f;
constexpr float kRoomGeneralFieldGap  = 12.0f; // between one label+field group and the next
constexpr float kRoomGeneralLabelGap  = 4.0f;  // label -> its own field
constexpr float kLabelH    = 16.0f;
constexpr float kRoomGeneralFieldH    = 26.0f; // single-line row (underline sits at its base)
constexpr float kRoomGeneralTopicMaxH = 200.0f; // cap so topic can't swallow the whole tab

} // namespace

// ---------------------------------------------------------------------------
// RoomGeneralSection::Content — the bespoke avatar/name/topic widget. Kept
// verbatim (layout math) from the pre-tabs RoomSettingsView — only the
// surrounding class shape and bounds origin changed (Content no longer sits
// below a title band; that stays on the outer RoomSettingsView shell).
// ---------------------------------------------------------------------------

class RoomGeneralSection::Content : public tk::Widget
{
public:
    using ImageProvider = RoomGeneralSection::ImageProvider;

    // host is nullable: when null, name_field_ is simply not constructed.
    explicit Content(tk::Host* host = nullptr);

    void set_avatar_provider(ImageProvider p);
    void set_name(std::string name);
    void set_topic(std::string topic);
    void set_avatar_url(std::string mxc);
    void set_room_id(std::string room_id);
    void set_canonical_alias(std::string alias);

    void set_field_permissions(bool can_name, bool can_topic, bool can_avatar);
    void set_committing(bool committing);

    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    void set_topic_area_natural_height(float h);

    tk::TextField* name_field() const { return name_field_; }
    tk::TextArea*  topic_field() const { return topic_field_; }

    void reset();

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;
    std::function<void(std::string room_id)> on_room_id_clicked;
    std::function<void()> on_layout_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    std::string staged_name_;
    std::string staged_topic_;
    std::string room_id_;
    std::string canonical_alias_;

    bool can_name_       = false;
    bool can_topic_      = false;
    bool can_avatar_     = false;
    bool committing_     = false;
    bool roomid_hovered_ = false;

    AvatarEditControl avatar_;

    // Borrowed — owned via add_child(). Null when constructed without a Host.
    tk::TextField* name_field_  = nullptr;
    tk::TextArea*  topic_field_ = nullptr;
    tk::Host*      host_        = nullptr;

    // World-space rects, recomputed each arrange().
    tk::Rect name_rect_{};
    tk::Rect topic_rect_{};
    tk::Rect address_rect_{};
    tk::Rect roomid_rect_{};

    std::unique_ptr<tk::TextLayout> name_label_layout_;
    std::unique_ptr<tk::TextLayout> name_static_layout_;
    std::unique_ptr<tk::TextLayout> topic_label_layout_;
    std::unique_ptr<tk::TextLayout> topic_static_layout_;
    std::unique_ptr<tk::TextLayout> address_label_layout_;
    std::unique_ptr<tk::TextLayout> address_value_layout_;
    std::unique_ptr<tk::TextLayout> roomid_label_layout_;
    std::unique_ptr<tk::TextLayout> roomid_value_layout_;

    // Natural (unclamped) content height reported by the topic NativeTextArea;
    // reset to kRoomGeneralFieldH (one line) on reset().
    float topic_natural_h_ = kRoomGeneralFieldH;
};

RoomGeneralSection::Content::Content(tk::Host* host) : host_(host)
{
    if (host)
    {
        auto field = std::make_unique<tk::TextField>(*host, kRoomGeneralFieldH);
        field->set_visible(false);
        name_field_ = add_child(std::move(field));

        auto topic = std::make_unique<tk::TextArea>(*host, kRoomGeneralFieldH);
        topic->set_visible(false);
        // Deferred by one UI-thread tick: set_rect() (called from this
        // widget's own arrange(), below) can synchronously trigger this on
        // some backends as a side effect of the width-driven reflow, and
        // on_layout_changed ultimately reaches Surface::relayout() — calling
        // that synchronously here would re-enter root_->arrange() while the
        // outer arrange() pass that led here is still on the stack.
        topic->set_on_height_changed(
            [this](float h)
            {
                set_topic_area_natural_height(h);
                if (host_)
                    host_->post_to_ui([this] { if (on_layout_changed) on_layout_changed(); });
            });
        topic_field_ = add_child(std::move(topic));
    }
}

void RoomGeneralSection::Content::set_avatar_provider(ImageProvider p)
{
    avatar_.set_image_provider(std::move(p));
}

// Updates the read-only-mode display cache only — never pushes into
// name_field_'s live text. The field's initial text is seeded once by
// RoomSettingsView::open(); after that its content is driven purely by the
// user's own typing (re-pushing text() on every keystroke would fight the
// native control's own cursor/selection state).
void RoomGeneralSection::Content::set_name(std::string name)
{
    if (staged_name_ == name) return;
    staged_name_ = std::move(name);
    name_static_layout_.reset();
}

void RoomGeneralSection::Content::set_topic(std::string topic)
{
    if (staged_topic_ == topic) return;
    staged_topic_ = std::move(topic);
    topic_static_layout_.reset();
}

void RoomGeneralSection::Content::set_avatar_url(std::string mxc)
{
    avatar_.set_avatar_url(std::move(mxc));
}

void RoomGeneralSection::Content::set_room_id(std::string room_id)
{
    if (room_id_ == room_id) return;
    room_id_ = std::move(room_id);
    roomid_value_layout_.reset();
}

void RoomGeneralSection::Content::set_canonical_alias(std::string alias)
{
    if (canonical_alias_ == alias) return;
    canonical_alias_ = std::move(alias);
    address_value_layout_.reset();
}

void RoomGeneralSection::Content::set_field_permissions(bool can_name,
                                                         bool can_topic,
                                                         bool can_avatar)
{
    can_name_   = can_name;
    can_topic_  = can_topic;
    can_avatar_ = can_avatar;
    avatar_.set_editable(can_avatar_ && !committing_);
    name_static_layout_.reset();
    topic_static_layout_.reset();
}

void RoomGeneralSection::Content::set_committing(bool committing)
{
    committing_ = committing;
    avatar_.set_editable(can_avatar_ && !committing_);
}

void RoomGeneralSection::Content::set_avatar_busy(bool busy)
{
    avatar_.set_busy(busy);
}

void RoomGeneralSection::Content::set_avatar_error(std::string error)
{
    avatar_.set_error(std::move(error));
}

void RoomGeneralSection::Content::set_topic_area_natural_height(float h)
{
    topic_natural_h_ = std::clamp(h, kRoomGeneralFieldH, kRoomGeneralTopicMaxH);
}

void RoomGeneralSection::Content::reset()
{
    name_label_layout_.reset();
    name_static_layout_.reset();
    topic_label_layout_.reset();
    topic_static_layout_.reset();
    address_label_layout_.reset();
    address_value_layout_.reset();
    roomid_label_layout_.reset();
    roomid_value_layout_.reset();
    topic_natural_h_ = kRoomGeneralFieldH;
}

tk::Size RoomGeneralSection::Content::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0.0f;
    // Four label+field rows (name, topic-min, address, room ID) plus the
    // gaps between them, or the avatar's own height — whichever is taller.
    constexpr float kRowH = kLabelH + kRoomGeneralLabelGap + kRoomGeneralFieldH;
    const float rows_h = 4.0f * kRowH + 3.0f * kRoomGeneralFieldGap;
    const float h = constraints.h > 0
        ? constraints.h
        : (2.0f * kRoomGeneralPadY + std::max(kAvatarD, rows_h));
    return {w, h};
}

void RoomGeneralSection::Content::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;

    const tk::Point avatar_centre_local{kRoomGeneralPadX + kAvatarD * 0.5f,
                                        kRoomGeneralPadY + kAvatarD * 0.5f};
    avatar_.set_geometry(avatar_centre_local, kAvatarD);

    const float col_x = bounds_.x + kRoomGeneralPadX + kAvatarD + kRoomGeneralAvatarGap;
    const float col_w = std::max(0.0f, bounds_.x + bounds_.w - kRoomGeneralPadX - col_x);

    float cy = bounds_.y + kRoomGeneralPadY;
    cy += kLabelH + kRoomGeneralLabelGap;
    name_rect_ = {col_x, cy, col_w, kRoomGeneralFieldH};
    if (name_field_)
    {
        // SideTabView::arrange() re-arranges every tab's content on each
        // relayout, not just the selected one — visible_in_tree() stops a
        // deselected tab's field from reshowing itself (visibility isn't
        // cascaded down from a hidden ancestor automatically).
        const bool editable = can_name_ && !committing_ && visible_in_tree();
        name_field_->set_visible(editable);
        if (editable)
            name_field_->arrange(lc, name_rect_);
    }
    cy += kRoomGeneralFieldH + kRoomGeneralFieldGap;

    cy += kLabelH + kRoomGeneralLabelGap;
    // One line by default, grows with content up to kRoomGeneralTopicMaxH, but never
    // past the space reserved for the read-only address/ID rows below.
    constexpr float kIdRowH = kLabelH + kRoomGeneralLabelGap + kRoomGeneralFieldH;
    const float reserved_below = kRoomGeneralFieldGap + kIdRowH + kRoomGeneralFieldGap + kIdRowH;
    const float topic_h_cap = std::max(
        kRoomGeneralFieldH, (bounds_.y + bounds_.h) - reserved_below - cy);
    const float topic_h = std::min(topic_natural_h_, topic_h_cap);
    topic_rect_ = {col_x, cy, col_w, topic_h};
    if (topic_field_)
    {
        // See name_field_'s comment above.
        const bool editable = can_topic_ && !committing_ && visible_in_tree();
        topic_field_->set_visible(editable);
        if (editable)
            topic_field_->arrange(lc, topic_rect_);
    }
    cy += topic_h + kRoomGeneralFieldGap;

    // Room address (canonical alias) — read-only, no permission gating.
    cy += kLabelH + kRoomGeneralLabelGap;
    address_rect_ = {col_x, cy, col_w, kRoomGeneralFieldH};
    cy += kRoomGeneralFieldH + kRoomGeneralFieldGap;

    // Room ID — read-only, always present.
    cy += kLabelH + kRoomGeneralLabelGap;
    roomid_rect_ = {col_x, cy, col_w, kRoomGeneralFieldH};
}

bool RoomGeneralSection::Content::on_pointer_down(tk::Point local)
{
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

    // `local` is widget-local (per Widget::dispatch_pointer_down); roomid_rect_
    // is stored in world coordinates like name_rect_/topic_rect_.
    const tk::Point world_pt{local.x + bounds_.x, local.y + bounds_.y};
    if (!room_id_.empty() && rect_contains(roomid_rect_, world_pt))
    {
        if (on_room_id_clicked) on_room_id_clicked(room_id_);
        return true;
    }
    return false;
}

bool RoomGeneralSection::Content::on_pointer_move(tk::Point local)
{
    const bool avatar_changed = avatar_.on_pointer_move(local);

    const tk::Point world_pt{local.x + bounds_.x, local.y + bounds_.y};
    const bool now_hovered = rect_contains(roomid_rect_, world_pt);
    const bool roomid_changed = (now_hovered != roomid_hovered_);
    roomid_hovered_ = now_hovered;

    return avatar_changed || roomid_changed;
}

void RoomGeneralSection::Content::on_pointer_leave()
{
    avatar_.on_pointer_leave();
    roomid_hovered_ = false;
}

void RoomGeneralSection::Content::paint(tk::PaintCtx& ctx)
{
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Avatar (left)
    {
        std::string_view name_source =
            staged_name_.empty() ? std::string_view("?")
                                 : std::string_view(staged_name_);
        avatar_.paint(ctx, {bounds_.x, bounds_.y}, name_source);
    }

    const float col_x = bounds_.x + kRoomGeneralPadX + kAvatarD + kRoomGeneralAvatarGap;
    const float col_w = std::max(0.0f, bounds_.x + bounds_.w - kRoomGeneralPadX - col_x);
    float cy = bounds_.y + kRoomGeneralPadY;

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
    cy += kLabelH + kRoomGeneralLabelGap;

    if (can_name_ && !committing_)
    {
        // Native overlay draws the live text; we just draw the underline.
        const float uly = cy + kRoomGeneralFieldH - 1.0f;
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
    cy += kRoomGeneralFieldH + kRoomGeneralFieldGap;

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
    cy += kLabelH + kRoomGeneralLabelGap;

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
    // live text itself — nothing to paint here.
    cy = topic_rect_.y + topic_rect_.h + kRoomGeneralFieldGap;

    // Room address (canonical alias) — read-only, no permission gating.
    if (!address_label_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = col_w;
        address_label_layout_ = ctx.factory.build_text(tk::tr("Room Address"), st);
    }
    if (address_label_layout_)
        cv.draw_text(*address_label_layout_, {col_x, cy}, pal.text_muted);
    cy += kLabelH + kRoomGeneralLabelGap;

    if (!address_value_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = col_w;
        address_value_layout_ = ctx.factory.build_text(
            canonical_alias_.empty() ? "—" : canonical_alias_, st);
    }
    if (address_value_layout_)
        cv.draw_text(*address_value_layout_, {col_x, cy},
                     canonical_alias_.empty() ? pal.text_muted : pal.text_primary);
    cy += kRoomGeneralFieldH + kRoomGeneralFieldGap;

    // Room ID — read-only, always present.
    if (!roomid_label_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = col_w;
        roomid_label_layout_ = ctx.factory.build_text(tk::tr("Room ID"), st);
    }
    if (roomid_label_layout_)
        cv.draw_text(*roomid_label_layout_, {col_x, cy}, pal.text_muted);
    cy += kLabelH + kRoomGeneralLabelGap;

    if (roomid_hovered_ && !room_id_.empty())
        cv.fill_rounded_rect(roomid_rect_, 4.0f, pal.sidebar_hover);

    if (!roomid_value_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Body;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = col_w;
        roomid_value_layout_ = ctx.factory.build_text(
            room_id_.empty() ? "—" : room_id_, st);
    }
    if (roomid_value_layout_)
        cv.draw_text(*roomid_value_layout_, {col_x, cy}, pal.text_primary);
}

// ---------------------------------------------------------------------------
// RoomGeneralSection — thin SettingsPage wrapper around Content.
// ---------------------------------------------------------------------------

RoomGeneralSection::RoomGeneralSection(tk::Host* host)
{
    // Content owns its own outer padding and needs the full tab height to
    // size the topic field, so zero out the page inset/spacing (mirrors
    // AccountSection's override) and let Content fill the main axis.
    set_padding(tk::Edges{});
    set_spacing(0.0f);

    auto content = std::make_unique<Content>(host);
    content->set_layout_hints({.fill_main = true});
    content_ = add_widget(std::move(content));

    content_->on_avatar_upload_clicked = [this]
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
    };
    content_->on_avatar_remove_clicked = [this]
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
    };
    content_->on_room_id_clicked = [this](std::string id)
    {
        if (on_room_id_clicked) on_room_id_clicked(std::move(id));
    };
    content_->on_layout_changed = [this]
    {
        if (on_layout_changed) on_layout_changed();
    };
}

RoomGeneralSection::~RoomGeneralSection() = default;

void RoomGeneralSection::set_avatar_provider(ImageProvider p)
{
    content_->set_avatar_provider(std::move(p));
}

void RoomGeneralSection::set_room_id(std::string room_id)
{
    content_->set_room_id(std::move(room_id));
}

void RoomGeneralSection::set_canonical_alias(std::string alias)
{
    content_->set_canonical_alias(std::move(alias));
}

void RoomGeneralSection::set_name(std::string name)
{
    content_->set_name(std::move(name));
}

void RoomGeneralSection::set_topic(std::string topic)
{
    content_->set_topic(std::move(topic));
}

void RoomGeneralSection::set_avatar_url(std::string mxc)
{
    content_->set_avatar_url(std::move(mxc));
}

void RoomGeneralSection::set_field_permissions(bool can_name, bool can_topic,
                                               bool can_avatar)
{
    content_->set_field_permissions(can_name, can_topic, can_avatar);
}

void RoomGeneralSection::set_committing(bool committing)
{
    content_->set_committing(committing);
}

void RoomGeneralSection::set_avatar_busy(bool busy)
{
    content_->set_avatar_busy(busy);
}

void RoomGeneralSection::set_avatar_error(std::string error)
{
    content_->set_avatar_error(std::move(error));
}

void RoomGeneralSection::set_topic_area_natural_height(float h)
{
    content_->set_topic_area_natural_height(h);
}

tk::TextField* RoomGeneralSection::name_field() const
{
    return content_->name_field();
}

tk::TextArea* RoomGeneralSection::topic_field() const
{
    return content_->topic_field();
}

void RoomGeneralSection::reset()
{
    content_->reset();
}

} // namespace tesseract::views
