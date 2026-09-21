#include "SpaceAddRoomList.h"
#include "media_utils.h"
#include "text_util.h"

#include "tk/drag_gesture.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/list_view.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kListRowAvatarSize = 28.0f;
constexpr float kFieldH = 34.0f;
constexpr float kPad = 8.0f;
} // namespace

// ── Inner list view: layers drag-source initiation and hover tooltips on
// top of tk::ListView's own selection/scroll behavior. Must be a real
// subclass (not a container wrapping a plain ListView) because pointer
// dispatch walks to the deepest hit widget — only this instance's own
// on_pointer_down/on_pointer_drag/on_pointer_move ever fire for a press
// landing on a row. ──────────────────────────────────────────────────────
class SpaceAddRoomList::InnerListView : public tk::ListView
{
protected:
    InnerListView() = default;
    TK_WIDGET_FACTORY_FRIEND(InnerListView)

public:
    SpaceAddRoomList* owner = nullptr;

    bool on_pointer_down(tk::Point local) override
    {
        // scrollbar_on_pointer_down() runs first, inside ListView::
        // on_pointer_down() — check scrollbar_dragging() *after* the base
        // call so a press on the thumb never arms the drag tracker (it
        // would otherwise still resolve to a valid row via index_at(), and
        // dragging the thumb would start an item drag instead of scrolling).
        const bool base = tk::ListView::on_pointer_down(local);
        press_index_ = -1;
        if (owner && owner->can_manage_ && !scrollbar_dragging())
        {
            const int idx = index_at(local);
            if (idx >= 0)
            {
                press_index_ = idx;
                drag_tracker_.begin(local);
            }
        }
        return base;
    }

    void on_pointer_drag(tk::Point local) override
    {
        tk::ListView::on_pointer_drag(local);
        if (!owner || !owner->can_manage_ || press_index_ < 0 || scrollbar_dragging())
            return;
        if (!drag_tracker_.crossed_threshold(local))
            return;
        const int idx = press_index_;
        press_index_ = -1;
        const auto* room = owner->room_at_(static_cast<std::size_t>(idx));
        if (!room || !host())
            return;
        tk::DragVisual visual = owner->build_drag_visual_(*room);
        host()->begin_drag(tk::DragPayload("space_room_id", room->id),
                           std::move(visual), local);
    }

    bool on_pointer_move(tk::Point local) override
    {
        const bool base = tk::ListView::on_pointer_move(local);
        if (owner && host())
        {
            const int idx = index_at(local);
            if (idx != last_tooltip_index_)
            {
                last_tooltip_index_ = idx;
                const auto* room =
                    idx >= 0 ? owner->room_at_(static_cast<std::size_t>(idx))
                             : nullptr;
                if (room)
                    host()->show_tooltip(this, owner->tooltip_text_(*room),
                                         row_world_rect(idx));
                else
                    host()->hide_tooltip(this);
            }
        }
        return base;
    }

    void on_pointer_leave() override
    {
        tk::ListView::on_pointer_leave();
        press_index_ = -1;
        drag_tracker_.cancel();
        if (last_tooltip_index_ >= 0 && host())
            host()->hide_tooltip(this);
        last_tooltip_index_ = -1;
    }

protected:
    // Match the container's own fill (paint()'s fill_rounded_rect below) —
    // ListView::paint() otherwise fills with the base class default
    // (sidebar_bg), which visibly mismatched the rest of this section.
    tk::Color background_color(const tk::Theme& theme) const override
    {
        return theme.palette.compose_card_bg;
    }

private:
    int press_index_ = -1;
    int last_tooltip_index_ = -1;
    tk::DragGestureTracker drag_tracker_;
};

// ── Row painter: avatar + single-line name only — deliberately simpler
// than RoomListView's row (no unread badge/preview/call icon, which don't
// apply to "am I a candidate to add" rows). RoomListView's own Adapter is a
// private nested class (opaque in its header), so this reimplements the
// look from the same shared pieces (draw_avatar() + <visual.h> metrics)
// rather than reusing it. ────────────────────────────────────────────────
class SpaceAddRoomList::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(SpaceAddRoomList& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.filtered_.size(); }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override
    {
        const auto* room = owner_.room_at_(index);
        if (!room)
            return;
        const auto& pal = ctx.theme.palette;

        if (selected)
            ctx.canvas.fill_rect(bounds, pal.sidebar_selected);
        else if (hovered)
            ctx.canvas.fill_rect(bounds, pal.sidebar_hover);

        const float avatar_cx =
            bounds.x + tesseract::visual::kSpaceSM + kListRowAvatarSize * 0.5f;
        const float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        const std::string& av_mxc = room->effective_avatar_url();
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
            if (!avatar && owner_.on_room_avatar_needed)
                owner_.on_room_avatar_needed(*room);
        }
        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kListRowAvatarSize,
                    room->name.empty() ? room->id : room->name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);

        const float text_x = bounds.x + tesseract::visual::kSpaceSM +
                             kListRowAvatarSize + tesseract::visual::kSpaceMD;
        const float text_w = std::max(
            0.0f, bounds.x + bounds.w - text_x - tesseract::visual::kSpaceSM);
        tk::TextStyle ns{};
        ns.role = tk::FontRole::SidebarName;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        auto name_lo = ctx.factory.build_text(
            room->name.empty() ? room->id : room->name, ns);
        if (name_lo)
        {
            const tk::Size sz = name_lo->measure();
            ctx.canvas.draw_text(
                *name_lo, {text_x, bounds.y + (bounds.h - sz.h) * 0.5f},
                owner_.can_manage_ ? pal.text_primary : pal.text_muted);
        }
    }

private:
    SpaceAddRoomList& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

SpaceAddRoomList::SpaceAddRoomList() : adapter_(std::make_unique<Adapter>(*this))
{
    if (host())
    {
        auto field = tk::create_widget<tk::TextField>(this, kFieldH);
        field->set_placeholder(tk::tr("Search your rooms\xe2\x80\xa6"));
        field->set_on_changed([this](const std::string& q) { set_query(q); });
        search_field_ = add_child(std::move(field));
    }

    auto list = tk::create_widget<InnerListView>(this);
    list->owner = this;
    list->set_adapter(adapter_.get());
    list->set_focus_on_click(false);
    list->on_row_clicked = [this](int idx)
    {
        if (!can_manage_ || idx < 0)
            return;
        const auto* room = room_at_(static_cast<std::size_t>(idx));
        if (room && on_add_requested)
            on_add_requested(room->id);
    };
    list_ = add_child(std::move(list));
}

SpaceAddRoomList::~SpaceAddRoomList() = default;

void SpaceAddRoomList::set_rooms_provider(RoomsProvider p)
{
    rooms_provider_ = std::move(p);
}

void SpaceAddRoomList::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void SpaceAddRoomList::set_excluded_room_ids(std::vector<std::string> ids)
{
    excluded_ids_.clear();
    for (auto& id : ids)
        excluded_ids_.insert(std::move(id));
}

void SpaceAddRoomList::refresh()
{
    all_candidates_.clear();
    if (rooms_provider_)
    {
        for (auto& r : rooms_provider_())
        {
            // Spaces (including this one) aren't offered as addable
            // candidates — nesting a space as a child is out of scope for
            // this management UI.
            if (!r.is_space && !excluded_ids_.count(r.id))
                all_candidates_.push_back(std::move(r));
        }
    }
    refilter_();
}

void SpaceAddRoomList::set_query(const std::string& q)
{
    query_ = q;
    refilter_();
}

void SpaceAddRoomList::refilter_()
{
    filtered_.clear();
    for (const auto& r : all_candidates_)
        if (tesseract::text::name_matches(r.name, query_))
            filtered_.push_back(r);
    if (list_)
    {
        list_->invalidate_data();
        // No auto-select — this is an always-visible section, not a modal
        // confirm flow (ForwardRoomPicker's pattern this was modeled on),
        // so index 0 being permanently pre-selected would just tint the
        // first row with the selection highlight for no reason. Keyboard
        // Down still moves from -1 to the first row on its own.
        list_->set_selected_index(-1);
    }
}

const tesseract::RoomInfo* SpaceAddRoomList::room_at_(std::size_t index) const
{
    if (index >= filtered_.size())
        return nullptr;
    return &filtered_[index];
}

void SpaceAddRoomList::set_can_manage(bool v)
{
    can_manage_ = v;
}

tk::Size SpaceAddRoomList::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void SpaceAddRoomList::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    search_field_rect_ = {bounds.x + kPad, bounds.y + kPad,
                          std::max(0.0f, bounds.w - 2.0f * kPad), kFieldH};
    if (search_field_)
        search_field_->arrange(ctx, search_field_rect_);

    const float list_y = bounds.y + kPad + kFieldH + kPad;
    if (list_)
        list_->arrange(ctx, {bounds.x, list_y, bounds.w,
                             std::max(0.0f, bounds.y + bounds.h - list_y)});
}

void SpaceAddRoomList::paint(tk::PaintCtx& ctx)
{
    factory_ = &ctx.factory;
    theme_ = &ctx.theme;
    scale_factor_ = ctx.canvas.scale_factor();

    const auto& pal = ctx.theme.palette;
    ctx.canvas.fill_rounded_rect(bounds_, tesseract::visual::kRadiusSM,
                                 pal.compose_card_bg);
    if (drag_hover_)
        ctx.canvas.fill_rect(
            bounds_, tk::Color::rgba(pal.accent.r, pal.accent.g, pal.accent.b, 40));

    if (search_field_)
    {
        ctx.canvas.fill_rounded_rect(search_field_rect_,
                                     tesseract::visual::kRadiusSM, pal.bg);
        ctx.canvas.stroke_rounded_rect(search_field_rect_,
                                       tesseract::visual::kRadiusSM,
                                       pal.border, 1.0f);
        search_field_->paint(ctx);
    }

    if (filtered_.empty())
    {
        tk::TextStyle es{};
        es.role = tk::FontRole::Body;
        es.halign = tk::TextHAlign::Center;
        es.max_width = std::max(0.0f, bounds_.w - 2.0f * kPad);
        if (auto lo = ctx.factory.build_text(
                tk::tr("No more rooms to add."), es))
        {
            const tk::Size sz = lo->measure();
            const float list_y = bounds_.y + kPad + kFieldH + kPad;
            ctx.canvas.draw_text(
                *lo, {bounds_.x + (bounds_.w - sz.w) * 0.5f, list_y + 16.0f},
                pal.text_muted);
        }
    }
    else if (list_)
    {
        list_->paint(ctx);
    }

    if (!can_manage_)
    {
        tk::TextStyle ns{};
        ns.role = tk::FontRole::Small;
        ns.wrap = true;
        ns.max_width = std::max(0.0f, bounds_.w - 2.0f * kPad);
        if (auto lo = ctx.factory.build_text(
                tk::tr("You don't have permission to add rooms to this space."),
                ns))
        {
            ctx.canvas.draw_text(
                *lo,
                {bounds_.x + kPad,
                 bounds_.y + bounds_.h - lo->measure().h - 4.0f},
                pal.destructive);
        }
    }
}

bool SpaceAddRoomList::on_drag_enter(tk::Point, const tk::DragPayload& payload)
{
    if (!can_manage_ || payload.kind() != "space_room_id")
        return false;
    drag_hover_ = true;
    return true;
}

void SpaceAddRoomList::on_drag_over(tk::Point, const tk::DragPayload&)
{
}

void SpaceAddRoomList::on_drag_leave_target()
{
    drag_hover_ = false;
}

bool SpaceAddRoomList::on_drop(tk::Point, tk::DragPayload payload)
{
    drag_hover_ = false;
    const auto* id = payload.get_if<std::string>("space_room_id");
    if (!id)
        return false;
    if (on_room_dropped_for_remove)
        on_room_dropped_for_remove(*id);
    return true;
}

tk::DragVisual SpaceAddRoomList::build_drag_visual_(const tesseract::RoomInfo& room)
{
    tk::DragVisual visual;
    if (!factory_ || !theme_)
        return visual;

    constexpr float kW = 160.0f;
    constexpr float kH = 36.0f;
    constexpr float kInnerAvatar = kH - 8.0f;

    auto surface = factory_->create_offscreen({kW, kH}, scale_factor_);
    if (!surface)
        return visual;

    tk::Canvas& canvas = surface->canvas();
    canvas.clear(tk::Color::rgba(0, 0, 0, 0));
    const auto& pal = theme_->palette;
    canvas.fill_rounded_rect({0, 0, kW, kH}, kH * 0.5f, pal.chrome_bg);

    const tk::Image* avatar = (avatar_provider_ && !room.effective_avatar_url().empty())
                                  ? avatar_provider_(room.effective_avatar_url())
                                  : nullptr;
    draw_avatar(canvas, avatar, {kH * 0.5f, kH * 0.5f}, kInnerAvatar,
                room.name.empty() ? room.id : room.name,
                pal.avatar_initials_bg, pal.avatar_initials_text);

    tk::TextStyle ns{};
    ns.role = tk::FontRole::SidebarName;
    ns.trim = tk::TextTrim::Ellipsis;
    ns.max_width = kW - kH - 8.0f;
    if (auto lo = factory_->build_text(room.name.empty() ? room.id : room.name, ns))
        canvas.draw_text(*lo, {kH, (kH - lo->measure().h) * 0.5f}, pal.text_primary);

    visual.image = std::shared_ptr<tk::Image>(surface->finish());
    visual.hotspot = {kH * 0.5f, kH * 0.5f};
    visual.opacity = 0.9f;
    return visual;
}

std::string SpaceAddRoomList::tooltip_text_(const tesseract::RoomInfo& room) const
{
    std::string text = room.name.empty() ? room.id : room.name;
    if (!room.topic.empty())
    {
        text += "\n\n";
        text += room.topic;
    }
    return text;
}

} // namespace tesseract::views
