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

// Section header row dimensions.
constexpr float kHeaderH      = 28.0f;
constexpr float kHeaderPadX   = 10.0f;

// Search header dimensions. Matches LoginView's homeserver-field height.
constexpr float kSearchBarH       = 36.0f;
constexpr float kSearchBarInsetX  = 6.0f;
constexpr float kSearchBarInsetY  = 4.0f;

std::string format_unread(std::uint64_t count) {
    if (count > 99) return "99+";
    return std::to_string(count);
}

// Case-insensitive substring match (byte-level ASCII approximation).
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

// ─────────────────────────────────────────────────────────────────────────

class RoomListView::Adapter : public tk::ListAdapter {
public:
    explicit Adapter(RoomListView& owner) : owner_(owner) {}

    std::size_t count() const override {
        return owner_.items_.size();
    }

    float measure_row_height(std::size_t index, tk::LayoutCtx&,
                              float /*width*/) override {
        if (index >= owner_.items_.size()) return kRowH;
        return owner_.items_[index].kind == Item::Kind::Header
            ? kHeaderH : kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool selected, bool hovered) override {
        if (index >= owner_.items_.size()) return;
        const auto& item = owner_.items_[index];

        if (item.kind == Item::Kind::Header) {
            paint_header(item, ctx, bounds, hovered);
        } else {
            const auto& rooms = owner_.section_rooms_[item.section];
            if (item.room_idx < 0
                || item.room_idx >= static_cast<int>(rooms.size())) return;
            paint_room(*rooms[item.room_idx], ctx, bounds, selected, hovered);
        }
    }

private:
    void paint_header(const Item& item, tk::PaintCtx& ctx, tk::Rect bounds,
                       bool hovered) {
        if (hovered)
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);

        const char* title = RoomListView::kSectionTitles[item.section];
        bool collapsed = owner_.collapsed_[item.section]
                         && owner_.search_text_.empty();

        // Section name (left-aligned, vertically centred).
        tk::TextStyle ts{};
        ts.role = tk::FontRole::Small;
        auto layout = ctx.factory.build_text(title, ts);
        if (layout) {
            float ty = bounds.y + (bounds.h - layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*layout,
                                  { bounds.x + kHeaderPadX, ty },
                                  ctx.theme.palette.text_muted);
        }

        // Collapse chevron (right-aligned): ▾ expanded / ▸ collapsed.
        const char* chevron = collapsed ? "\xE2\x96\xB8" : "\xE2\x96\xBE";
        tk::TextStyle cs{};
        cs.role = tk::FontRole::Small;
        auto clayout = ctx.factory.build_text(chevron, cs);
        if (clayout) {
            float cw = clayout->measure().w;
            float cy = bounds.y + (bounds.h - clayout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*clayout,
                                  { bounds.x + bounds.w - kHeaderPadX - cw, cy },
                                  ctx.theme.palette.text_muted);
        }
    }

    void paint_room(const tesseract::RoomInfo& room, tk::PaintCtx& ctx,
                     tk::Rect bounds, bool selected, bool hovered) {
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
        if (owner_.avatar_provider_ && !room.avatar_url.empty())
            avatar = owner_.avatar_provider_(room.avatar_url);

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
                ? badge_layout->measure().w : 0.0f;
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

    RoomListView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

RoomListView::~RoomListView() = default;

RoomListView::RoomListView()
    : adapter_(std::make_unique<Adapter>(*this)) {
    auto list = std::make_unique<tk::ListView>();
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= items_.size()) return;
        const auto& item = items_[static_cast<std::size_t>(idx)];

        if (item.kind == Item::Kind::Header) {
            // Toggle collapse only when not searching (search always shows all).
            if (search_text_.empty()) {
                collapsed_[item.section] = !collapsed_[item.section];
                rebuild_items();
                list_->invalidate_data();
                // Re-apply selection — the selected room may have just been
                // hidden or revealed by the toggle.
                set_selected_room(selected_room_id_cache_);
            }
            return;
        }

        const auto& rooms = section_rooms_[item.section];
        if (item.room_idx < 0
            || item.room_idx >= static_cast<int>(rooms.size())) return;
        selected_room_id_cache_ = rooms[item.room_idx]->id;
        if (on_room_selected) on_room_selected(rooms[item.room_idx]->id);
    };
    list->on_scroll = [this] { if (on_scroll) on_scroll(); };
    list_ = add_child(std::move(list));
}

void RoomListView::set_rooms(std::vector<tesseract::RoomInfo> rooms) {
    rooms_ = std::move(rooms);
    rebuild_items();
    if (list_) list_->invalidate_data();
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
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto& item = items_[static_cast<std::size_t>(i)];
        if (item.kind != Item::Kind::Room) continue;
        const auto& rooms = section_rooms_[item.section];
        if (item.room_idx >= 0
            && item.room_idx < static_cast<int>(rooms.size())
            && rooms[item.room_idx]->id == room_id) {
            list_->set_selected_index(i);
            return;
        }
    }
    list_->set_selected_index(-1);
}

std::string RoomListView::selected_room_id() const {
    if (!list_) return {};
    int flat = list_->selected_index();
    if (flat >= 0 && flat < static_cast<int>(items_.size())) {
        const auto& item = items_[static_cast<std::size_t>(flat)];
        if (item.kind == Item::Kind::Room) {
            const auto& rooms = section_rooms_[item.section];
            if (item.room_idx >= 0
                && item.room_idx < static_cast<int>(rooms.size()))
                return rooms[item.room_idx]->id;
        }
    }
    return selected_room_id_cache_;
}

int RoomListView::selected_index() const {
    if (!list_) return -1;
    int flat = list_->selected_index();
    if (flat < 0 || flat >= static_cast<int>(items_.size())) return -1;
    if (items_[static_cast<std::size_t>(flat)].kind == Item::Kind::Header)
        return -1;
    // Count visible room items before this flat index (headers excluded).
    int room_idx = 0;
    for (int i = 0; i < flat; ++i) {
        if (items_[static_cast<std::size_t>(i)].kind == Item::Kind::Room)
            ++room_idx;
    }
    return room_idx;
}

tk::Rect RoomListView::search_field_rect()    const { return search_field_rect_; }
bool     RoomListView::search_field_visible() const { return search_field_visible_; }

void RoomListView::set_search_text(std::string q) {
    if (q == search_text_) return;
    search_text_ = std::move(q);
    rebuild_items();
    if (list_) list_->invalidate_data();
    set_selected_room(selected_room_id_cache_);
}

std::vector<std::string> RoomListView::visible_room_ids() const {
    if (!list_) return {};
    auto [first, last] = list_->visible_range();
    if (last < first) return {};
    std::vector<std::string> ids;
    for (int i = first; i <= last; ++i) {
        const auto& item = items_[static_cast<std::size_t>(i)];
        if (item.kind == Item::Kind::Room) {
            const auto& rooms = section_rooms_[item.section];
            if (item.room_idx >= 0 &&
                item.room_idx < static_cast<int>(rooms.size()))
                ids.push_back(rooms[item.room_idx]->id);
        }
    }
    return ids;
}

void RoomListView::rebuild_items() {
    // 1. Clear buckets.
    for (auto& sr : section_rooms_) sr.clear();

    // 2. Classify each room into a section, applying the search filter.
    for (const auto& r : rooms_) {
        const std::string& hay = r.name.empty() ? r.id : r.name;
        if (!search_text_.empty() && !name_matches(hay, search_text_)) continue;

        int sec;
        if (r.is_favorite)        sec = kSecFavorites;
        else if (r.is_direct)     sec = kSecDMs;
        else if (!r.is_space)     sec = kSecRooms;
        else                      sec = kSecSpaces;
        section_rooms_[sec].push_back(&r);
    }

    // 3. Build flat item list.
    items_.clear();
    bool searching = !search_text_.empty();
    for (int s = 0; s < kNumSections; ++s) {
        if (section_rooms_[s].empty()) continue;
        items_.push_back({ Item::Kind::Header, s, 0 });
        // Respect collapsed state only when not actively searching.
        if (!searching && collapsed_[s]) continue;
        for (int r = 0; r < static_cast<int>(section_rooms_[s].size()); ++r)
            items_.push_back({ Item::Kind::Room, s, r });
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

    list_->arrange(ctx, bounds);
    bool wants_search = list_->content_height() > bounds.h
                        || !search_text_.empty();
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
    if (local.y < search_header_h()) return false;
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
