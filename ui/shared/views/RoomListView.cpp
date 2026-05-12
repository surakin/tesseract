#include "RoomListView.h"

#include "tk/theme.h"
#include <tesseract/visual.h>

#include <algorithm>
#include <string>

namespace tesseract::views {

namespace {

constexpr float kRowH         = tesseract::visual::kRoomRowHeight;     // 62
constexpr float kAvatarSize   = tesseract::visual::kRoomAvatarSize;    // 36
constexpr float kPadX         = tesseract::visual::kSpaceMD;           // 12
constexpr float kPadY         = tesseract::visual::kSpaceSM;           // 8
constexpr float kAvatarGap    = tesseract::visual::kSpaceMD;           // 12
constexpr float kBadgeMinW    = tesseract::visual::kUnreadBadgeMinWidth;  // 20
constexpr float kBadgeH       = tesseract::visual::kUnreadBadgeHeight;    // 18
constexpr float kBadgePadX    = 6.0f;
constexpr float kBadgeRadius  = kBadgeH * 0.5f;

std::string format_unread(std::uint64_t count) {
    if (count > 99) return "99+";
    return std::to_string(count);
}

} // namespace

class RoomListView::Adapter : public tk::ListAdapter {
public:
    explicit Adapter(RoomListView& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.rooms_.size(); }

    float measure_row_height(std::size_t /*index*/, tk::LayoutCtx&,
                              float /*width*/) override {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override {
        if (index >= owner_.rooms_.size()) return;
        const auto& room = owner_.rooms_[index];

        // Row background — selection > hover > base sidebar fill.
        if (selected) {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_selected);
        } else if (hovered) {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);
        }

        // Avatar circle (left-aligned, vertically centred).
        float avatar_cx = bounds.x + kPadX + kAvatarSize * 0.5f;
        float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        if (owner_.avatar_provider_ && !room.avatar_url.empty()) {
            avatar = owner_.avatar_provider_(room.avatar_url);
        }
        if (avatar) {
            ctx.canvas.draw_circle_image(*avatar,
                                          { avatar_cx, avatar_cy },
                                          kAvatarSize);
        } else {
            ctx.canvas.draw_initials_circle(
                room.name,
                { avatar_cx, avatar_cy },
                kAvatarSize,
                ctx.theme.palette.avatar_initials_bg,
                ctx.theme.palette.avatar_initials_text);
        }

        // Text column geometry.
        float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
        float text_w = bounds.w - (text_x - bounds.x) - kPadX;

        // Reserve space on the right for the unread badge (if any).
        float badge_width = 0;
        std::string badge_text;
        if (room.unread_count > 0) {
            badge_text = format_unread(room.unread_count);
            // Approximate width before measuring exact glyph advance —
            // good enough for layout reserve since the badge auto-sizes
            // to its measured text in the paint pass below.
            badge_width = std::max(kBadgeMinW,
                                    kBadgePadX * 2 + 7.0f
                                        * static_cast<float>(badge_text.size()));
            text_w -= (badge_width + kPadX);
        }
        if (text_w < 0) text_w = 0;

        // Name. When the row has a preview underneath, the name pins to
        // the top and the preview pins to the bottom. When there's no
        // preview (the common case until matrix-sdk surfaces last-msg
        // bodies), centre the name vertically so it lines up with the
        // avatar disc rather than floating above it.
        bool has_preview = !room.last_message_body.empty();
        tk::TextStyle name_style{};
        name_style.role      = tk::FontRole::SidebarName;
        name_style.trim      = tk::TextTrim::Ellipsis;
        name_style.max_width = text_w;
        auto name_layout = ctx.factory.build_text(
            room.name.empty() ? room.id : room.name, name_style);
        if (name_layout) {
            float name_y = has_preview
                ? bounds.y + kPadY
                : bounds.y + (bounds.h - name_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*name_layout,
                                  { text_x, name_y },
                                  ctx.theme.palette.text_primary);
        }

        // Preview (bottom row).
        if (!room.last_message_body.empty()) {
            tk::TextStyle prev_style{};
            prev_style.role      = tk::FontRole::SidebarPreview;
            prev_style.trim      = tk::TextTrim::Ellipsis;
            prev_style.max_width = text_w;
            auto prev_layout = ctx.factory.build_text(
                room.last_message_body, prev_style);
            if (prev_layout) {
                float prev_y = bounds.y + bounds.h - kPadY
                                - prev_layout->measure().h;
                ctx.canvas.draw_text(*prev_layout,
                                      { text_x, prev_y },
                                      ctx.theme.palette.text_secondary);
            }
        }

        // Unread badge — accent pill, right-aligned, vertically centred.
        if (!badge_text.empty()) {
            tk::TextStyle badge_style{};
            badge_style.role   = tk::FontRole::UnreadBadge;
            badge_style.halign = tk::TextHAlign::Center;
            auto badge_layout = ctx.factory.build_text(badge_text, badge_style);
            float text_w_measured = badge_layout
                ? badge_layout->measure().w
                : 0.0f;
            float pill_w = std::max(kBadgeMinW,
                                     text_w_measured + kBadgePadX * 2);
            tk::Rect pill{
                bounds.x + bounds.w - kPadX - pill_w,
                bounds.y + (bounds.h - kBadgeH) * 0.5f,
                pill_w,
                kBadgeH
            };
            ctx.canvas.fill_rounded_rect(pill, kBadgeRadius,
                                           ctx.theme.palette.unread_bg);
            if (badge_layout) {
                tk::Size ts = badge_layout->measure();
                ctx.canvas.draw_text(
                    *badge_layout,
                    { pill.x + (pill.w - ts.w) * 0.5f,
                      pill.y + (pill.h - ts.h) * 0.5f },
                    ctx.theme.palette.unread_text);
            }
        }
    }

private:
    RoomListView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

RoomListView::~RoomListView() = default;

RoomListView::RoomListView()
    : adapter_(std::make_unique<Adapter>(*this)) {
    set_adapter(adapter_.get());
    on_row_clicked = [this](int idx) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= rooms_.size()) return;
        if (on_room_selected) on_room_selected(rooms_[idx].id);
    };
}

void RoomListView::set_rooms(std::vector<tesseract::RoomInfo> rooms) {
    // Preserve selection by room ID across the swap.
    std::string keep = selected_room_id();
    rooms_ = std::move(rooms);
    invalidate_data();
    set_selected_room(keep);
}

void RoomListView::set_avatar_provider(AvatarProvider p) {
    avatar_provider_ = std::move(p);
}

void RoomListView::set_selected_room(const std::string& room_id) {
    if (room_id.empty()) { set_selected_index(-1); return; }
    auto it = std::find_if(rooms_.begin(), rooms_.end(),
        [&](const tesseract::RoomInfo& r) { return r.id == room_id; });
    if (it == rooms_.end()) set_selected_index(-1);
    else set_selected_index(static_cast<int>(it - rooms_.begin()));
}

std::string RoomListView::selected_room_id() const {
    int idx = selected_index();
    if (idx < 0 || static_cast<std::size_t>(idx) >= rooms_.size()) return {};
    return rooms_[idx].id;
}

} // namespace tesseract::views
