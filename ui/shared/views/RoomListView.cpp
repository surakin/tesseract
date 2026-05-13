#include "RoomListView.h"

#include "tk/theme.h"
#include <tesseract/visual.h>

#include <algorithm>
#include <cctype>
#include <memory>
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

// Search header dimensions. Matches LoginView's homeserver-field height.
constexpr float kSearchBarH       = 36.0f;
constexpr float kSearchBarInsetX  = 6.0f;
constexpr float kSearchBarInsetY  = 4.0f;

std::string format_unread(std::uint64_t count) {
    if (count > 99) return "99+";
    return std::to_string(count);
}

// Case-insensitive substring match. Works on bytes — fine for ASCII names
// and a reasonable approximation for UTF-8 (lowercase ASCII still matches
// regardless of Unicode case folding on multibyte sequences).
bool name_matches(const std::string& name, const std::string& query) {
    if (query.empty()) return true;
    if (name.size() < query.size()) return false;
    auto to_lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + query.size() <= name.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < query.size(); ++j) {
            if (to_lower(name[i + j]) != to_lower(query[j])) {
                match = false; break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

class RoomListView::Adapter : public tk::ListAdapter {
public:
    explicit Adapter(RoomListView& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.filtered_rooms_.size(); }

    float measure_row_height(std::size_t /*index*/, tk::LayoutCtx&,
                              float /*width*/) override {
        return kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override {
        if (index >= owner_.filtered_rooms_.size()) return;
        const auto& room = owner_.filtered_rooms_[index];

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
            badge_width = std::max(kBadgeMinW,
                                    kBadgePadX * 2 + 7.0f
                                        * static_cast<float>(badge_text.size()));
            text_w -= (badge_width + kPadX);
        }
        if (text_w < 0) text_w = 0;

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
    auto list = std::make_unique<tk::ListView>();
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx) {
        if (idx < 0
            || static_cast<std::size_t>(idx) >= filtered_rooms_.size()) {
            return;
        }
        selected_room_id_cache_ = filtered_rooms_[idx].id;
        if (on_room_selected) on_room_selected(filtered_rooms_[idx].id);
    };
    list_ = add_child(std::move(list));
}

void RoomListView::set_rooms(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    rebuild_filtered();
    if (list_) list_->invalidate_data();
    // Reapply selection by ID — the inner list's selected_index may have
    // shifted with the new ordering / filter.
    set_selected_room(selected_room_id_cache_);
}

void RoomListView::set_avatar_provider(AvatarProvider p) {
    avatar_provider_ = std::move(p);
}

void RoomListView::set_selected_room(const std::string& room_id) {
    selected_room_id_cache_ = room_id;
    if (!list_) return;
    if (room_id.empty()) {
        list_->set_selected_index(-1);
        return;
    }
    auto it = std::find_if(filtered_rooms_.begin(), filtered_rooms_.end(),
        [&](const tesseract::RoomInfo& r) { return r.id == room_id; });
    if (it == filtered_rooms_.end()) {
        // Selection is hidden by the current filter — drop the visual
        // highlight but keep the cached id so it returns when the filter
        // clears or the room comes back into view.
        list_->set_selected_index(-1);
    } else {
        list_->set_selected_index(
            static_cast<int>(it - filtered_rooms_.begin()));
    }
}

std::string RoomListView::selected_room_id() const {
    if (!list_) return {};
    int idx = list_->selected_index();
    if (idx < 0
        || static_cast<std::size_t>(idx) >= filtered_rooms_.size()) {
        // Inner list has no live selection (often because the filter
        // hides it); fall back to the cached id.
        return selected_room_id_cache_;
    }
    return filtered_rooms_[idx].id;
}

int RoomListView::selected_index() const {
    return list_ ? list_->selected_index() : -1;
}

tk::Rect RoomListView::search_field_rect()    const { return search_field_rect_; }
bool     RoomListView::search_field_visible() const { return search_field_visible_; }

void RoomListView::set_search_text(std::string q) {
    if (q == search_text_) return;
    search_text_ = std::move(q);
    rebuild_filtered();
    if (list_) list_->invalidate_data();
    // Selection may need to swap visible/invisible based on the new filter.
    set_selected_room(selected_room_id_cache_);
}

void RoomListView::rebuild_filtered() {
    if (search_text_.empty()) {
        filtered_rooms_ = rooms_;
        return;
    }
    filtered_rooms_.clear();
    filtered_rooms_.reserve(rooms_.size());
    for (const auto& r : rooms_) {
        const std::string& haystack = r.name.empty() ? r.id : r.name;
        if (name_matches(haystack, search_text_)) {
            filtered_rooms_.push_back(r);
        }
    }
}

float RoomListView::search_header_h() const {
    return search_field_visible_ ? kSearchBarH : 0.0f;
}

tk::Size RoomListView::measure(tk::LayoutCtx&, tk::Size constraints) {
    return constraints;
}

void RoomListView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;
    if (!list_) return;

    // First pass: arrange the inner list at the full viewport to learn
    // its total content_height. The decision uses the would-be list
    // height (no header reserved) so that toggling visibility doesn't
    // oscillate — once overflow exists, showing the header keeps it
    // overflowing; once content fits at full height, the header stays
    // hidden whether or not we showed it last frame.
    list_->arrange(ctx, bounds);
    bool wants_search = list_->content_height() > bounds.h;
    search_field_visible_ = wants_search;

    if (wants_search) {
        search_field_rect_ = {
            bounds.x + kSearchBarInsetX,
            bounds.y + kSearchBarInsetY,
            std::max(0.0f, bounds.w - 2 * kSearchBarInsetX),
            std::max(0.0f, kSearchBarH - 2 * kSearchBarInsetY),
        };
        tk::Rect list_bounds{
            bounds.x,
            bounds.y + kSearchBarH,
            bounds.w,
            std::max(0.0f, bounds.h - kSearchBarH),
        };
        list_->arrange(ctx, list_bounds);
    } else {
        search_field_rect_ = {};
    }
}

void RoomListView::paint(tk::PaintCtx& ctx) {
    if (search_field_visible_) {
        // Header strip background + 1-px separator under it. The native
        // EDIT overlay paints its own chrome on top.
        tk::Rect header_rect{ bounds_.x, bounds_.y, bounds_.w, kSearchBarH };
        ctx.canvas.fill_rect(header_rect, ctx.theme.palette.sidebar_bg);
        tk::Rect sep{
            bounds_.x, bounds_.y + kSearchBarH - 1.0f, bounds_.w, 1.0f
        };
        ctx.canvas.fill_rect(sep, ctx.theme.palette.border);
    }
    if (list_ && list_->visible()) list_->paint(ctx);
}

bool RoomListView::on_pointer_down(tk::Point local) {
    if (!list_) return false;
    if (local.y < search_header_h()) return false; // host overlay owns this zone
    tk::Point list_local{ local.x, local.y - search_header_h() };
    return list_->on_pointer_down(list_local);
}

void RoomListView::on_pointer_up(tk::Point local, bool inside_self) {
    if (!list_) return;
    tk::Point list_local{ local.x, local.y - search_header_h() };
    bool inside_list = inside_self && local.y >= search_header_h();
    list_->on_pointer_up(list_local, inside_list);
}

} // namespace tesseract::views
