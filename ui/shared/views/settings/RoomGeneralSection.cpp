#include "RoomGeneralSection.h"

#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/AvatarEditControl.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tesseract::views
{

namespace
{

constexpr float kAvatarD              = 96.0f;
constexpr float kRoomGeneralAvatarGap = 24.0f; // avatar -> fields column
constexpr float kRoomGeneralFieldGap  = 12.0f; // between one label+field row and the next
constexpr float kRoomGeneralLabelGap  = 4.0f;  // label -> its own field
// TextField/TextArea::arrange() insets the native overlay by overlay_inset_
// (2px, on all 4 sides) inside whatever rect it's given, so the *visible*
// native content area ends up 2*overlay_inset_ shorter (top+bottom) than
// whatever height it's handed — applies both to the single-line fields'
// fixed reserved height below and to TopicAreaCell's dynamic natural_h_ (see
// its set_on_height_changed handler), so neither ever has to scroll inside
// its own field for content that otherwise fits. Includes a little headroom
// beyond the bare 2*overlay_inset_ for sub-pixel DPI rounding.
constexpr float kFieldOverlayPad      = 6.0f;
constexpr float kRoomGeneralFieldH    = 26.0f + kFieldOverlayPad; // single-line row height
constexpr float kRoomGeneralTopicMaxH = 200.0f; // cap so topic can't swallow the whole tab

} // namespace

// ---------------------------------------------------------------------------
// AvatarCell — bridges AvatarEditControl (a plain geometry-agnostic helper,
// not a tk::Widget) into a FlexBox cell: measure() reports a fixed square,
// arrange()/paint()/pointer overrides forward to the control using this
// cell's own bounds_ for geometry/origin, exactly as AvatarEditControl's own
// doc comment describes an owner doing.
// ---------------------------------------------------------------------------

class RoomGeneralSection::AvatarCell : public tk::Widget
{
protected:
    AvatarCell() = default;
    TK_WIDGET_FACTORY_FRIEND(AvatarCell)

public:
    void set_avatar_provider(RoomGeneralSection::ImageProvider p)
    {
        avatar_.set_image_provider(std::move(p));
    }
    void set_avatar_url(std::string mxc) { avatar_.set_avatar_url(std::move(mxc)); }
    void set_local_preview(std::shared_ptr<tk::Image> image)
    {
        avatar_.set_local_preview(std::move(image));
    }
    void set_editable(bool editable) { avatar_.set_editable(editable); }
    void set_busy(bool busy) { avatar_.set_busy(busy); }
    void set_error(std::string error) { avatar_.set_error(std::move(error)); }
    // Initials fallback source when there's no avatar image — kept in sync
    // by RoomGeneralSection::set_name().
    void set_name_source(std::string name) { name_source_ = std::move(name); }

    std::function<void()> on_upload_clicked;
    std::function<void()> on_remove_clicked;

    tk::Size measure(tk::LayoutCtx&, tk::Size) override { return {kAvatarD, kAvatarD}; }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        bounds_ = bounds;
        avatar_.set_geometry({kAvatarD * 0.5f, kAvatarD * 0.5f}, kAvatarD);
    }

    void paint(tk::PaintCtx& ctx) override
    {
        std::string_view src = name_source_.empty() ? std::string_view("?")
                                                     : std::string_view(name_source_);
        avatar_.paint(ctx, {bounds_.x, bounds_.y}, src);
    }

    bool on_pointer_down(tk::Point local) override
    {
        switch (avatar_.hit_test(local))
        {
        case AvatarEditControl::HitZone::RemoveChip:
            if (on_remove_clicked) on_remove_clicked();
            return true;
        case AvatarEditControl::HitZone::Disc:
            if (on_upload_clicked) on_upload_clicked();
            return true;
        case AvatarEditControl::HitZone::None:
            return false;
        }
        return false;
    }
    bool on_pointer_move(tk::Point local) override { return avatar_.on_pointer_move(local); }
    void on_pointer_leave() override { avatar_.on_pointer_leave(); }

private:
    AvatarEditControl avatar_;
    std::string       name_source_;
};

// ---------------------------------------------------------------------------
// NameFieldCell — a fixed-height cell around the Name tk::TextField, drawing
// the underline affordance beneath it. Kept as its own tiny widget (rather
// than drawing the underline from RoomGeneralSection itself) so the
// decoration and the field it belongs to move/hide together automatically.
// ---------------------------------------------------------------------------

class RoomGeneralSection::NameFieldCell : public tk::Widget
{
protected:
    NameFieldCell();
    TK_WIDGET_FACTORY_FRIEND(NameFieldCell)

public:
    tk::TextField* text_field() const { return text_field_; }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w > 0 ? constraints.w : 0.0f, kRoomGeneralFieldH};
    }

    void paint_before_children(tk::PaintCtx& ctx) override
    {
        const float uly = bounds_.y + kRoomGeneralFieldH - 1.0f;
        ctx.canvas.fill_rect({bounds_.x, uly, bounds_.w, 1.0f},
                             ctx.theme.palette.text_secondary.with_alpha(80));
    }

private:
    tk::TextField* text_field_ = nullptr;
};

RoomGeneralSection::NameFieldCell::NameFieldCell()
{
    if (host())
    {
        auto field = tk::create_widget<tk::TextField>(this, kRoomGeneralFieldH);
        field->set_visible(false); // shown once refresh_name_display_ knows can_name_
        text_field_ = add_child(std::move(field));
    }
}

// ---------------------------------------------------------------------------
// TopicAreaCell — bridges tk::TextArea's auto-grow height into FlexBox:
// TextArea's own measure() doesn't reflect its grown height (only its static
// construction-time minimum), so this cell tracks the clamped natural height
// via TextArea::set_on_height_changed and reports it from measure() instead.
// TextArea already requests its own relayout when its height changes, so no
// extra bubbling is needed here — the next measure() pass just sees the new
// clamped value.
// ---------------------------------------------------------------------------

class RoomGeneralSection::TopicAreaCell : public tk::Widget
{
protected:
    TopicAreaCell();
    TK_WIDGET_FACTORY_FRIEND(TopicAreaCell)

public:
    tk::TextArea* text_area() const { return text_area_; }

    // Called by RoomSettingsView::open() (via RoomGeneralSection::reset())
    // so a room with a short topic doesn't briefly inherit the previous
    // room's grown height before its own TextArea recomputes one.
    void reset_natural_height() { natural_h_ = kRoomGeneralFieldH; }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w > 0 ? constraints.w : 0.0f, natural_h_};
    }

private:
    tk::TextArea* text_area_ = nullptr;
    float         natural_h_ = kRoomGeneralFieldH;
};

RoomGeneralSection::TopicAreaCell::TopicAreaCell()
{
    if (host())
    {
        auto area = tk::create_widget<tk::TextArea>(this, kRoomGeneralFieldH);
        area->set_visible(false); // shown once refresh_topic_display_ knows can_topic_
        area->set_on_height_changed([this](float h)
        {
            // `h` is the native area's own reported content height; pad it
            // by the same overlay-inset compensation kRoomGeneralFieldH
            // bakes in for the static case, since arrange() will subtract
            // that same inset back off before handing space to the native
            // control — see kFieldOverlayPad's doc comment.
            natural_h_ = std::clamp(h + kFieldOverlayPad, kRoomGeneralFieldH,
                                    kRoomGeneralTopicMaxH);
        });
        text_area_ = add_child(std::move(area));
    }
}

// ---------------------------------------------------------------------------
// RoomIdRow — the Room ID caption+value pair, plus its hover/click-to-copy
// behavior. A plain tk::VBox subclass: the two Labels stack via ordinary
// FlexBox layout, and this only adds the pointer handling + hover fill on
// top (dispatch already only calls these when the pointer is within
// bounds_, so no manual rect_contains bookkeeping is needed any more).
// ---------------------------------------------------------------------------

class RoomGeneralSection::RoomIdRow : public tk::VBox
{
protected:
    RoomIdRow();
    TK_WIDGET_FACTORY_FRIEND(RoomIdRow)

public:
    void set_room_id(std::string room_id)
    {
        room_id_ = std::move(room_id);
        value_->set_text(room_id_.empty() ? "\xE2\x80\x94" : room_id_); // "—"
    }

    std::function<void(std::string room_id)> on_clicked;

    void on_theme_changed(const tk::Theme& t) override
    {
        label_->set_colour(t.palette.text_muted);
    }

    void paint_before_children(tk::PaintCtx& ctx) override
    {
        if (hovered_ && !room_id_.empty())
            ctx.canvas.fill_rounded_rect(bounds_, 4.0f, ctx.theme.palette.sidebar_hover);
    }

    bool on_pointer_down(tk::Point) override
    {
        if (room_id_.empty())
            return false;
        if (on_clicked) on_clicked(room_id_);
        return true;
    }
    bool on_pointer_move(tk::Point) override
    {
        if (hovered_) return false;
        hovered_ = true;
        return true;
    }
    void on_pointer_leave() override { hovered_ = false; }

private:
    tk::Label*  label_ = nullptr; // "Room ID" caption, muted
    tk::Label*  value_ = nullptr;
    std::string room_id_;
    bool        hovered_ = false;
};

RoomGeneralSection::RoomIdRow::RoomIdRow()
{
    set_spacing(kRoomGeneralLabelGap);
    label_ = add_child(tk::create_widget<tk::Label>(this, tk::tr("Room ID"), tk::FontRole::Small));
    auto value = tk::create_widget<tk::Label>(this, "\xE2\x80\x94", tk::FontRole::Body);
    value->set_trim(tk::TextTrim::Ellipsis);
    value_ = add_child(std::move(value));
}

// ---------------------------------------------------------------------------
// RoomGeneralSection
// ---------------------------------------------------------------------------

RoomGeneralSection::RoomGeneralSection()
{
    // Left at SettingsPage's own default padding/spacing (unlike the
    // pre-FlexBox Content, which zeroed it and applied its own instead) so
    // the avatar/fields row gets the same left/right margin every other
    // settings tab's groups do.
    auto row = tk::create_widget<tk::HBox>(this);
    row->set_spacing(kRoomGeneralAvatarGap);
    row->set_cross(tk::Cross::Start);

    auto avatar_cell = tk::create_widget<AvatarCell>(this);
    avatar_cell_ = row->add_child(std::move(avatar_cell));
    avatar_cell_->on_upload_clicked = [this]
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
    };
    avatar_cell_->on_remove_clicked = [this]
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
    };

    auto column = tk::create_widget<tk::VBox>(this);
    column->set_layout_hints({.fill_main = true});
    column->set_spacing(kRoomGeneralFieldGap);

    // Name row.
    {
        auto name_row = tk::create_widget<tk::VBox>(this);
        name_row->set_spacing(kRoomGeneralLabelGap);
        name_label_ = name_row->add_child(
            tk::create_widget<tk::Label>(this, tk::tr("Name"), tk::FontRole::Small));

        auto name_cell = tk::create_widget<NameFieldCell>(this);
        name_field_    = name_cell->text_field();
        name_row->add_child(std::move(name_cell));

        auto name_static = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
        name_static->set_trim(tk::TextTrim::Ellipsis);
        name_static_ = name_row->add_child(std::move(name_static));

        column->add_child(std::move(name_row));
    }

    // Topic row.
    {
        auto topic_row = tk::create_widget<tk::VBox>(this);
        topic_row->set_spacing(kRoomGeneralLabelGap);
        topic_label_ = topic_row->add_child(
            tk::create_widget<tk::Label>(this, tk::tr("Topic"), tk::FontRole::Small));

        auto topic_cell = tk::create_widget<TopicAreaCell>(this);
        topic_cell_     = topic_row->add_child(std::move(topic_cell));

        auto topic_static = tk::create_widget<tk::Label>(this, "", tk::FontRole::Body);
        topic_static->set_wrap(true);
        topic_static_ = topic_row->add_child(std::move(topic_static));

        column->add_child(std::move(topic_row));
    }

    // Room Address row — read-only, no permission gating, no interaction.
    {
        auto addr_row = tk::create_widget<tk::VBox>(this);
        addr_row->set_spacing(kRoomGeneralLabelGap);
        address_label_ = addr_row->add_child(
            tk::create_widget<tk::Label>(this, tk::tr("Room Address"), tk::FontRole::Small));
        auto addr_value = tk::create_widget<tk::Label>(this, "\xE2\x80\x94", tk::FontRole::Body);
        addr_value->set_trim(tk::TextTrim::Ellipsis);
        address_value_ = addr_row->add_child(std::move(addr_value));
        column->add_child(std::move(addr_row));
    }

    // Room ID row — read-only, always present, hover/click to copy.
    {
        auto roomid_row = tk::create_widget<RoomIdRow>(this);
        roomid_row->on_clicked = [this](std::string id)
        {
            if (on_room_id_clicked) on_room_id_clicked(std::move(id));
        };
        roomid_row_ = column->add_child(std::move(roomid_row));
    }

    row->add_child(std::move(column));
    add_widget(std::move(row));
}

RoomGeneralSection::~RoomGeneralSection() = default;

void RoomGeneralSection::on_theme_changed(const tk::Theme& t)
{
    cached_muted_   = t.palette.text_muted;
    cached_primary_ = t.palette.text_primary;

    if (name_label_) name_label_->set_colour(cached_muted_);
    if (topic_label_) topic_label_->set_colour(cached_muted_);
    if (address_label_) address_label_->set_colour(cached_muted_);

    refresh_address_display_();
}

void RoomGeneralSection::set_avatar_provider(ImageProvider p)
{
    avatar_cell_->set_avatar_provider(std::move(p));
}

void RoomGeneralSection::set_room_id(std::string room_id)
{
    roomid_row_->set_room_id(std::move(room_id));
}

void RoomGeneralSection::set_canonical_alias(std::string alias)
{
    canonical_alias_ = std::move(alias);
    refresh_address_display_();
}

// Updates the read-only-mode display cache only — never pushes into
// name_field_'s live text. The field's initial text is seeded once by
// RoomSettingsView::open(); after that its content is driven purely by the
// user's own typing (re-pushing text() on every keystroke would fight the
// native control's own cursor/selection state).
void RoomGeneralSection::set_name(std::string name)
{
    if (staged_name_ == name) return;
    staged_name_ = std::move(name);
    name_static_->set_text(staged_name_);
    avatar_cell_->set_name_source(staged_name_);
}

void RoomGeneralSection::set_topic(std::string topic)
{
    if (staged_topic_ == topic) return;
    staged_topic_ = std::move(topic);
    topic_static_->set_text(staged_topic_);
}

void RoomGeneralSection::set_avatar_url(std::string mxc)
{
    avatar_cell_->set_avatar_url(std::move(mxc));
}

void RoomGeneralSection::set_staged_avatar_preview(std::shared_ptr<tk::Image> image)
{
    avatar_cell_->set_local_preview(std::move(image));
}

void RoomGeneralSection::refresh_name_display_()
{
    const bool editable    = can_name_ && !committing_;
    const bool was_visible = name_field_ && name_field_->visible();
    if (name_field_) name_field_->set_visible(editable);
    if (name_static_) name_static_->set_visible(!editable);
    if (was_visible != editable)
        relayout_and_notify_();
}

void RoomGeneralSection::refresh_topic_display_()
{
    tk::TextArea* area        = topic_cell_->text_area();
    const bool    editable    = can_topic_ && !committing_;
    const bool    was_visible = area && area->visible();
    // Toggle the TextArea's own visibility directly (mirrors
    // refresh_name_display_) rather than the TopicAreaCell wrapper's: the
    // wrapper stays always-visible so FlexBox keeps arranging it (and, in
    // turn, the TextArea whenever the TextArea's own flag allows) even while
    // the field itself is hidden — hiding the wrapper instead would stop
    // FlexBox from ever arranging the TextArea while not editable, since it
    // skips invisible children entirely.
    if (area) area->set_visible(editable);
    if (topic_static_) topic_static_->set_visible(!editable);
    if (was_visible != editable)
        relayout_and_notify_();
}

void RoomGeneralSection::refresh_address_display_()
{
    const bool muted = canonical_alias_.empty();
    address_value_->set_text(muted ? "\xE2\x80\x94" : canonical_alias_); // "—"
    address_value_->set_colour(muted ? cached_muted_ : cached_primary_);
}

void RoomGeneralSection::relayout_and_notify_()
{
    if (on_layout_changed) on_layout_changed();
}

void RoomGeneralSection::set_field_permissions(bool can_name, bool can_topic,
                                               bool can_avatar)
{
    can_name_   = can_name;
    can_topic_  = can_topic;
    can_avatar_ = can_avatar;
    avatar_cell_->set_editable(can_avatar_ && !committing_);
    refresh_name_display_();
    refresh_topic_display_();
}

void RoomGeneralSection::set_committing(bool committing)
{
    committing_ = committing;
    avatar_cell_->set_editable(can_avatar_ && !committing_);
    refresh_name_display_();
    refresh_topic_display_();
}

void RoomGeneralSection::set_avatar_busy(bool busy)
{
    avatar_cell_->set_busy(busy);
}

void RoomGeneralSection::set_avatar_error(std::string error)
{
    avatar_cell_->set_error(std::move(error));
}

tk::TextArea* RoomGeneralSection::topic_field() const
{
    return topic_cell_->text_area();
}

void RoomGeneralSection::reset()
{
    // tk::Label owns its own text-layout cache and rebuilds on demand — the
    // only state left to reset is the topic's grown height, so a room with
    // a short topic doesn't briefly show at the previous room's height.
    topic_cell_->reset_natural_height();
}

} // namespace tesseract::views
