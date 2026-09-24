#include "SpaceChildRoomGrid.h"
#include "media_utils.h"

#include "tk/drag_gesture.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/list_view.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <unordered_set>

namespace tesseract::views
{

namespace
{
constexpr float kGridRowAvatarSize = tesseract::visual::kRoomAvatarSize;
} // namespace

// ── Inner grid view: layers drag-source initiation, hover tooltips, and
// Delete-key handling on top of tk::GridView's own selection/keyboard
// behavior. Must be a real subclass — see SpaceAddRoomList::InnerListView's
// identical rationale (pointer dispatch walks to the deepest hit widget,
// and a member field can't override GridView's own virtuals). ────────────
class SpaceChildRoomGrid::InnerGridView : public tk::GridView
{
protected:
    InnerGridView() = default;
    TK_WIDGET_FACTORY_FRIEND(InnerGridView)

public:
    SpaceChildRoomGrid* owner = nullptr;
    std::function<void(int)> on_delete_requested;

    bool on_pointer_down(tk::Point local) override
    {
        // See SpaceAddRoomList::InnerListView::on_pointer_down's identical
        // comment: scrollbar_dragging() must be checked after the base call
        // so a press on the scrollbar thumb never arms the drag tracker.
        const bool base = tk::GridView::on_pointer_down(local);
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
        tk::GridView::on_pointer_drag(local);
        if (!owner || !owner->can_manage_ || press_index_ < 0 || scrollbar_dragging())
            return;
        if (!drag_tracker_.crossed_threshold(local))
            return;
        const int idx = press_index_;
        press_index_ = -1;
        const auto* entry = owner->entry_at_(static_cast<std::size_t>(idx));
        if (!entry || !host())
            return;
        tk::DragVisual visual = owner->build_drag_visual_(*entry);
        host()->begin_drag(tk::DragPayload("space_room_id", entry->info.id),
                           std::move(visual), local);
    }

    bool on_pointer_move(tk::Point local) override
    {
        const bool base = tk::GridView::on_pointer_move(local);
        if (owner && host())
        {
            const int idx = index_at(local);
            if (idx != last_tooltip_index_)
            {
                last_tooltip_index_ = idx;
                const auto* entry =
                    idx >= 0 ? owner->entry_at_(static_cast<std::size_t>(idx))
                             : nullptr;
                if (entry)
                {
                    // rect_at() is already world-space (built straight off
                    // bounds_.x/y internally), despite its doc comment
                    // saying "widget-local" — adding bounds() again here
                    // double-offset the tooltip anchor.
                    host()->show_tooltip(this, owner->tooltip_text_(*entry),
                                         rect_at(idx));
                }
                else
                {
                    host()->hide_tooltip(this);
                }
            }
        }
        return base;
    }

    void on_pointer_leave() override
    {
        tk::GridView::on_pointer_leave();
        press_index_ = -1;
        drag_tracker_.cancel();
        if (last_tooltip_index_ >= 0 && host())
            host()->hide_tooltip(this);
        last_tooltip_index_ = -1;
    }

    bool on_key_down(const tk::KeyEvent& e) override
    {
        if (tk::GridView::on_key_down(e))
            return true; // preserves arrow/Enter/Space navigation
        if (owner && owner->can_manage_ && e.key == tk::Key::Delete &&
            selected_index() >= 0)
        {
            if (on_delete_requested)
                on_delete_requested(selected_index());
            return true;
        }
        return false;
    }

protected:
    // Match the container's own fill (paint()'s fill_rounded_rect below) —
    // GridView::paint() otherwise fills with the base class default
    // (theme.palette.bg), which visibly mismatched the rest of this section.
    tk::Color background_color(const tk::Theme& theme) const override
    {
        return theme.palette.compose_card_bg;
    }

private:
    int press_index_ = -1;
    int last_tooltip_index_ = -1;
    tk::DragGestureTracker drag_tracker_;
};

// ── Cell painter: compact vertical layout (avatar centered on top, name
// below, ellipsized) — reads as "the same visual language" as a
// RoomListView row (same avatar rendering, same font role) without forcing
// a horizontal row shape into a grid cell. ────────────────────────────────
class SpaceChildRoomGrid::Adapter : public tk::GridAdapter
{
public:
    explicit Adapter(SpaceChildRoomGrid& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.children_.size(); }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override
    {
        const auto* entry = owner_.entry_at_(index);
        if (!entry)
            return;
        const auto& pal = ctx.theme.palette;

        if (selected)
            ctx.canvas.fill_rounded_rect(bounds, tesseract::visual::kRadiusSM,
                                         pal.sidebar_selected);
        else if (hovered)
            ctx.canvas.fill_rounded_rect(bounds, tesseract::visual::kRadiusSM,
                                         pal.sidebar_hover);

        const tesseract::RoomInfo& info = entry->info;
        const bool loading = !entry->joined && info.name.empty();

        if (loading && owner_.on_unjoined_summary_needed &&
            !requested_.count(info.id))
        {
            requested_.insert(info.id);
            owner_.on_unjoined_summary_needed(info.id);
        }

        // Horizontal RoomListView-style row (avatar + single-line name) —
        // same visual language as SpaceAddRoomList's rows and RoomListView's
        // own rows, just laid out as a wrapping grid cell instead of a
        // vertical list.
        const float avatar_cx =
            bounds.x + tesseract::visual::kSpaceSM + kGridRowAvatarSize * 0.5f;
        const float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        const std::string& av_mxc = info.effective_avatar_url();
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
            if (!avatar && owner_.on_room_avatar_needed)
                owner_.on_room_avatar_needed(info);
        }
        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kGridRowAvatarSize,
                    info.name.empty() ? info.id : info.name,
                    pal.avatar_initials_bg, pal.avatar_initials_text);

        const float text_x = bounds.x + tesseract::visual::kSpaceSM +
                             kGridRowAvatarSize + tesseract::visual::kSpaceMD;
        const float text_w = std::max(
            0.0f, bounds.x + bounds.w - text_x - tesseract::visual::kSpaceSM);
        const std::string label =
            loading ? tk::tr("Loading\xe2\x80\xa6")
                    : (info.name.empty() ? info.id : info.name);
        tk::TextStyle ns{};
        ns.role = tk::FontRole::SidebarName;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = text_w;
        if (auto lo = ctx.factory.build_text(label, ns))
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(
                *lo, {text_x, bounds.y + (bounds.h - sz.h) * 0.5f},
                (loading || !owner_.can_manage_) ? pal.text_muted
                                                  : pal.text_primary);
        }
    }

    // Reset once per set_children() call so a still-loading child is
    // re-requested if it comes back around (e.g. after a failed fetch).
    void reset_requested() { requested_.clear(); }

private:
    SpaceChildRoomGrid& owner_;
    std::unordered_set<std::string> requested_;
};

// ─────────────────────────────────────────────────────────────────────────

SpaceChildRoomGrid::SpaceChildRoomGrid() : adapter_(std::make_unique<Adapter>(*this))
{
    auto grid = tk::create_widget<InnerGridView>(this);
    grid->owner = this;
    grid->set_adapter(adapter_.get());
    grid->set_cell_size(kCellW, kCellH);
    grid->set_spacing(8.0f, 8.0f);
    grid->set_padding(tk::Edges::all(8.0f));
    grid->on_delete_requested = [this](int idx)
    {
        if (!can_manage_ || idx < 0)
            return;
        const auto* entry = entry_at_(static_cast<std::size_t>(idx));
        if (entry && on_remove_requested)
            on_remove_requested(entry->info.id);
    };
    grid_ = add_child(std::move(grid));
}

SpaceChildRoomGrid::~SpaceChildRoomGrid() = default;

void SpaceChildRoomGrid::set_children(std::vector<ChildRoomEntry> children)
{
    children_ = std::move(children);
    adapter_->reset_requested();
    if (grid_)
        grid_->invalidate_data();
}

void SpaceChildRoomGrid::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void SpaceChildRoomGrid::set_can_manage(bool v)
{
    can_manage_ = v;
}

const SpaceChildRoomGrid::ChildRoomEntry*
SpaceChildRoomGrid::entry_at_(std::size_t index) const
{
    if (index >= children_.size())
        return nullptr;
    return &children_[index];
}

tk::Size SpaceChildRoomGrid::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void SpaceChildRoomGrid::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (grid_)
        grid_->arrange(ctx, bounds);
}

void SpaceChildRoomGrid::paint(tk::PaintCtx& ctx)
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

    if (children_.empty())
    {
        tk::TextStyle es{};
        es.role = tk::FontRole::Body;
        es.halign = tk::TextHAlign::Center;
        es.max_width = std::max(0.0f, bounds_.w - 16.0f);
        if (auto lo = ctx.factory.build_text(
                tk::tr("This space has no rooms yet."), es))
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(
                *lo, {bounds_.x + (bounds_.w - sz.w) * 0.5f, bounds_.y + 16.0f},
                pal.text_muted);
        }
        return;
    }

    if (grid_)
        grid_->paint(ctx);

    if (!can_manage_)
    {
        tk::TextStyle ns{};
        ns.role = tk::FontRole::Small;
        ns.wrap = true;
        ns.max_width = std::max(0.0f, bounds_.w - 16.0f);
        if (auto lo = ctx.factory.build_text(
                tk::tr("You don't have permission to remove rooms from this space."),
                ns))
        {
            ctx.canvas.draw_text(
                *lo,
                {bounds_.x + 8.0f,
                 bounds_.y + bounds_.h - lo->measure().h - 4.0f},
                pal.destructive);
        }
    }
}

bool SpaceChildRoomGrid::on_drag_enter(tk::Point, const tk::DragPayload& payload)
{
    if (!can_manage_ || payload.kind() != "space_room_id")
        return false;
    drag_hover_ = true;
    return true;
}

void SpaceChildRoomGrid::on_drag_over(tk::Point, const tk::DragPayload&)
{
}

void SpaceChildRoomGrid::on_drag_leave_target()
{
    drag_hover_ = false;
}

bool SpaceChildRoomGrid::on_drop(tk::Point, tk::DragPayload payload)
{
    drag_hover_ = false;
    const auto* id = payload.get_if<std::string>("space_room_id");
    if (!id)
        return false;
    if (on_room_dropped_for_add)
        on_room_dropped_for_add(*id);
    return true;
}

tk::DragVisual SpaceChildRoomGrid::build_drag_visual_(const ChildRoomEntry& entry)
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

    const tesseract::RoomInfo& info = entry.info;
    const tk::Image* avatar = (avatar_provider_ && !info.effective_avatar_url().empty())
                                  ? avatar_provider_(info.effective_avatar_url())
                                  : nullptr;
    draw_avatar(canvas, avatar, {kH * 0.5f, kH * 0.5f}, kInnerAvatar,
                info.name.empty() ? info.id : info.name,
                pal.avatar_initials_bg, pal.avatar_initials_text);

    tk::TextStyle ns{};
    ns.role = tk::FontRole::SidebarName;
    ns.trim = tk::TextTrim::Ellipsis;
    ns.max_width = kW - kH - 8.0f;
    if (auto lo = factory_->build_text(info.name.empty() ? info.id : info.name, ns))
        canvas.draw_text(*lo, {kH, (kH - lo->measure().h) * 0.5f}, pal.text_primary);

    visual.image = std::shared_ptr<tk::Image>(surface->finish());
    visual.hotspot = {kH * 0.5f, kH * 0.5f};
    visual.opacity = 0.9f;
    return visual;
}

std::string SpaceChildRoomGrid::tooltip_text_(const ChildRoomEntry& entry) const
{
    const tesseract::RoomInfo& info = entry.info;
    if (!entry.joined && info.name.empty())
        return tk::tr("Loading\xe2\x80\xa6");
    std::string text = info.name.empty() ? info.id : info.name;
    if (!info.topic.empty())
    {
        text += "\n\n";
        text += info.topic;
    }
    return text;
}

} // namespace tesseract::views
