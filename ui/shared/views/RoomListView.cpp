#include "RoomListView.h"

#include "tk/theme.h"
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace tesseract::views
{

namespace
{

constexpr float kRowH = tesseract::visual::kRoomRowHeight;        // 48
constexpr float kAvatarSize = tesseract::visual::kRoomAvatarSize; // 36
constexpr float kPadX = 6.0f; // halved from kSpaceMD (12)
constexpr float kPadY = 4.0f; // halved from kSpaceSM (8)
constexpr float kAvatarGap = tesseract::visual::kSpaceMD;             // 12
constexpr float kBadgeMinW = tesseract::visual::kUnreadBadgeMinWidth; // 20
constexpr float kBadgeH = tesseract::visual::kUnreadBadgeHeight;      // 18
constexpr float kBadgePadX = 6.0f;
constexpr float kBadgeRadius = kBadgeH * 0.5f;

// Thumbnail chip painted on the right side of image/sticker rows.
constexpr float kThumb = kRowH - kPadY * 2.0f; // 40 px — full usable row height
constexpr float kThumbGap = 4.0f;

// Section header row dimensions.
constexpr float kHeaderH = 28.0f;
constexpr float kHeaderPadX = 10.0f;

// Search header dimensions. Matches LoginView's homeserver-field height.
constexpr float kSearchBarH = 36.0f;
constexpr float kSearchBarInsetX = 6.0f;
constexpr float kSearchBarInsetY = 4.0f;

std::string format_unread(std::uint64_t count)
{
    if (count > 99)
    {
        return "99+";
    }
    return std::to_string(count);
}

// Case-insensitive substring match (byte-level ASCII approximation).
bool name_matches(const std::string& name, const std::string& query)
{
    if (query.empty())
    {
        return true;
    }
    if (name.size() < query.size())
    {
        return false;
    }
    auto to_lower = [](char c)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + query.size() <= name.size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < query.size(); ++j)
        {
            if (to_lower(name[i + j]) != to_lower(query[j]))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

int classify_room_section(const tesseract::RoomInfo& r, bool group_inactive,
                          int threshold_days, std::uint64_t now_ms)
{
    // Favorites and Spaces are never grouped. Keep the original
    // favorite → DM → Room → Space precedence so the non-grouped result is
    // byte-for-byte unchanged; the inactivity branch only diverts DMs and
    // regular Rooms (the `!r.is_space` guard keeps spaces out of Inactive).
    if (r.is_favorite)
    {
        return RoomListView::kSecFavorites;
    }
    if (group_inactive && !r.is_space && r.last_activity_ts != 0)
    {
        std::uint64_t threshold_ms =
            static_cast<std::uint64_t>(threshold_days) * 86'400'000ULL;
        // last_activity_ts == 0 means the SDK hasn't returned a timestamp yet;
        // skip the inactive check rather than treating epoch as ancient history.
        // The `<= now_ms` guard avoids unsigned underflow treating a future-dated
        // timestamp (clock skew) as inactive.
        if (r.last_activity_ts <= now_ms &&
            now_ms - r.last_activity_ts > threshold_ms)
        {
            return RoomListView::kSecInactive;
        }
    }
    if (r.is_direct)
    {
        return RoomListView::kSecDMs;
    }
    if (!r.is_space)
    {
        return RoomListView::kSecRooms;
    }
    return RoomListView::kSecSpaces;
}

// ─────────────────────────────────────────────────────────────────────────

class RoomListView::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(RoomListView& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.items_.size();
    }

    float measure_row_height(std::size_t index, tk::LayoutCtx&,
                             float /*width*/) override
    {
        if (index >= owner_.items_.size())
        {
            return kRowH;
        }
        return owner_.items_[index].kind == Item::Kind::Header ? kHeaderH
                                                               : kRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool selected, bool hovered) override
    {
        // Flush all cached layouts when the factory changes (DPI migration).
        if (&ctx.factory != factory_seen_)
        {
            factory_seen_ = &ctx.factory;
            room_cache_.clear();
            for (auto& h : header_cache_)
                h = {};
        }

        if (index >= owner_.items_.size())
        {
            return;
        }
        const auto& item = owner_.items_[index];

        if (item.kind == Item::Kind::Header)
        {
            paint_header(item, ctx, bounds, hovered);
        }
        else if (item.kind == Item::Kind::Invite)
        {
            if (!owner_.invites_)
            {
                return;
            }
            const auto& inv = *owner_.invites_;
            if (item.room_idx < 0 ||
                item.room_idx >= static_cast<int>(inv.size()))
            {
                return;
            }
            paint_invite(inv[static_cast<std::size_t>(item.room_idx)], ctx,
                         bounds, selected, hovered);

            // 1px separator between consecutive invite rows.
            if (index > 0 &&
                owner_.items_[index - 1].kind == Item::Kind::Invite)
            {
                ctx.canvas.fill_rect({bounds.x, bounds.y, bounds.w, 1.0f},
                                     ctx.theme.palette.separator);
            }
        }
        else
        {
            const auto& rooms = owner_.section_rooms_[item.section];
            if (item.room_idx < 0 ||
                item.room_idx >= static_cast<int>(rooms.size()))
            {
                return;
            }
            paint_room(*rooms[item.room_idx], ctx, bounds, selected, hovered);

            // 1px separator at top of room row when the previous item is also
            // a room (not a section header or the very first row).  Drawn after
            // paint_room so the hover/selection fill does not paint over it.
            if (index > 0 && owner_.items_[index - 1].kind == Item::Kind::Room)
            {
                ctx.canvas.fill_rect({bounds.x, bounds.y, bounds.w, 1.0f},
                                     ctx.theme.palette.separator);
            }
        }
    }

private:
    // ── Per-room text-layout cache ────────────────────────────────────────
    struct RoomRowCache
    {
        // key — if any field differs the layouts are rebuilt
        std::string display_name;
        float       text_w     = -1.f;
        std::string preview;
        std::string badge_text;
        // cached layouts (nullptr when not applicable for this row)
        std::unique_ptr<tk::TextLayout> name_layout;
        std::unique_ptr<tk::TextLayout> preview_layout;
        std::unique_ptr<tk::TextLayout> badge_layout;
    };

    // ── Per-section header text-layout cache ─────────────────────────────
    struct HeaderRowCache
    {
        // key
        std::string   title;            // full rendered title string
        bool          collapsed       = false;
        std::uint64_t section_unread  = 0;
        bool          section_mention = false;
        bool          valid           = false; // false forces rebuild on first use
        // cached layouts
        std::unique_ptr<tk::TextLayout> title_layout;
        std::unique_ptr<tk::TextLayout> chevron_layout;
        std::unique_ptr<tk::TextLayout> badge_layout; // nullptr when unread == 0
    };

    void paint_header(const Item& item, tk::PaintCtx& ctx, tk::Rect bounds,
                      bool hovered)
    {
        ctx.canvas.fill_rect(bounds, hovered
                                         ? ctx.theme.palette.sidebar_selected
                                         : ctx.theme.palette.sidebar_hover);

        // Build the title string. For the Invitations section include the count.
        std::string title_str;
        if (item.section == RoomListView::kSecInvites)
        {
            const std::size_t n =
                owner_.invites_ ? owner_.invites_->size() : 0u;
            title_str = std::string("Invitations (") + std::to_string(n) + ")";
        }
        else
        {
            title_str = RoomListView::kSectionTitles[item.section];
        }
        const std::string& title  = title_str;
        bool               collapsed = owner_.collapsed_[item.section];

        // Sum notification counts and check for mentions (badge when collapsed).
        // Invitations have no unread badge.
        std::uint64_t section_unread  = 0;
        bool          section_mention = false;
        if (collapsed && item.section != RoomListView::kSecInvites)
        {
            for (const auto* r : owner_.section_rooms_[item.section])
            {
                section_unread  += r->notification_count;
                section_mention  = section_mention || (r->highlight_count > 0);
            }
        }

        // Rebuild layouts when any key field changes.
        auto& cache = header_cache_[item.section];
        if (!cache.valid || cache.title != title || cache.collapsed != collapsed ||
            cache.section_unread != section_unread ||
            cache.section_mention != section_mention)
        {
            cache.title           = title;
            cache.collapsed       = collapsed;
            cache.section_unread  = section_unread;
            cache.section_mention = section_mention;
            cache.valid           = true;

            tk::TextStyle ts{};
            ts.role            = tk::FontRole::Small;
            cache.title_layout = ctx.factory.build_text(title, ts);

            const char*   chevron = collapsed ? "\xE2\x96\xB8" : "\xE2\x96\xBE";
            tk::TextStyle cs{};
            cs.role              = tk::FontRole::Small;
            cache.chevron_layout = ctx.factory.build_text(chevron, cs);

            if (collapsed && section_unread > 0)
            {
                tk::TextStyle bs{};
                bs.role           = tk::FontRole::UnreadBadge;
                cache.badge_layout = ctx.factory.build_text(
                    format_unread(section_unread), bs);
            }
            else
            {
                cache.badge_layout = nullptr;
            }
        }

        // Draw from cache — same geometry as before.
        float chevron_x = bounds.x + bounds.w - kHeaderPadX;
        if (cache.title_layout)
        {
            float ty = bounds.y +
                       (bounds.h - cache.title_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*cache.title_layout,
                                 {bounds.x + kHeaderPadX, ty},
                                 ctx.theme.palette.text_muted);
        }
        if (cache.chevron_layout)
        {
            tk::Size csz = cache.chevron_layout->measure();
            float    cy  = bounds.y + (bounds.h - csz.h) * 0.5f;
            chevron_x   -= csz.w;
            ctx.canvas.draw_text(*cache.chevron_layout, {chevron_x, cy},
                                 ctx.theme.palette.text_muted);
        }
        if (collapsed && section_unread > 0 && cache.badge_layout)
        {
            tk::Size ts2    = cache.badge_layout->measure();
            float    pill_w = std::max(kBadgeMinW, ts2.w + kBadgePadX * 2);
            tk::Rect pill{chevron_x - kBadgePadX - pill_w,
                          bounds.y + (bounds.h - kBadgeH) * 0.5f,
                          pill_w, kBadgeH};
            const tk::Color sec_pill_bg   = section_mention
                                              ? ctx.theme.palette.accent
                                              : ctx.theme.palette.unread_bg;
            const tk::Color sec_pill_text = section_mention
                                              ? ctx.theme.palette.text_on_accent
                                              : ctx.theme.palette.unread_text;
            ctx.canvas.fill_rounded_rect(pill, kBadgeRadius, sec_pill_bg);
            ctx.canvas.draw_text(*cache.badge_layout,
                                 {pill.x + (pill.w - ts2.w) * 0.5f,
                                  pill.y + (pill.h - ts2.h) * 0.5f},
                                 sec_pill_text);
        }
    }

    void paint_room(const tesseract::RoomInfo& room, tk::PaintCtx& ctx,
                    tk::Rect bounds, bool selected, bool hovered)
    {
        // Row background — selection > hover > base sidebar fill.
        if (selected)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_selected);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);
        }

        // Avatar circle (left-aligned, vertically centred).
        float avatar_cx = bounds.x + kPadX + kAvatarSize * 0.5f;
        float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        const std::string& av_mxc = room.effective_avatar_url();
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
        }

        if (avatar)
        {
            ctx.canvas.draw_circle_image(*avatar, {avatar_cx, avatar_cy},
                                         kAvatarSize);
        }
        else
        {
            ctx.canvas.draw_initials_circle(
                room.name, {avatar_cx, avatar_cy}, kAvatarSize,
                ctx.theme.palette.avatar_initials_bg,
                ctx.theme.palette.avatar_initials_text);
        }

        // Presence dot — bottom-right of avatar, DM rooms only.
        if (!room.dm_counterpart_user_id.empty() && owner_.presence_provider_)
        {
            const auto ps = owner_.presence_provider_(room.dm_counterpart_user_id);
            tk::Color dot_color{};
            bool show_dot = true;
            if (ps == tesseract::PresenceState::Online)
            {
                dot_color = ctx.theme.palette.presence_online;
            }
            else if (ps == tesseract::PresenceState::Unavailable)
            {
                dot_color = ctx.theme.palette.presence_unavailable;
            }
            else
            {
                show_dot = false;
            }
            if (show_dot)
            {
                constexpr float kDotD = 8.0f;
                constexpr float kRing = 2.0f;
                const float outer_d   = kDotD + kRing * 2.0f;
                const float dot_cx    = avatar_cx + kAvatarSize * 0.5f;
                const float dot_cy    = avatar_cy + kAvatarSize * 0.5f;
                const tk::Color ring_col = selected ? ctx.theme.palette.sidebar_selected
                                         : hovered  ? ctx.theme.palette.sidebar_hover
                                                    : ctx.theme.palette.sidebar_bg;
                ctx.canvas.fill_rounded_rect(
                    {dot_cx - outer_d * 0.5f, dot_cy - outer_d * 0.5f,
                     outer_d, outer_d},
                    outer_d * 0.5f, ring_col);
                ctx.canvas.fill_rounded_rect(
                    {dot_cx - kDotD * 0.5f, dot_cy - kDotD * 0.5f, kDotD, kDotD},
                    kDotD * 0.5f, dot_color);
            }
        }

        // Text column geometry.
        float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
        float text_w = bounds.w - (text_x - bounds.x) - kPadX;

        // Reserve space for the badge (heuristic width keeps text_w stable
        // across frames so the name/preview layouts aren't rebuilt every time
        // the notification count increments by 1).
        float       badge_width = 0;
        std::string badge_text;
        if (room.notification_count > 0)
        {
            badge_text = format_unread(room.notification_count);
            badge_width = std::max(
                kBadgeMinW,
                kBadgePadX * 2 + 7.0f * static_cast<float>(badge_text.size()));
            text_w -= (badge_width + kPadX);
        }
        if (text_w < 0)
        {
            text_w = 0;
        }

        bool has_preview = !room.last_message_kind.empty();

        // Thumbnail lookup.
        const tk::Image* thumb     = nullptr;
        std::string      thumb_url;
        if (has_preview && owner_.sticker_provider_)
        {
            const std::string& kind = room.last_message_kind;
            thumb_url = kind == "sticker" ? room.last_message_sticker_url
                                          : (kind == "image"
                                                 ? room.last_message_thumbnail_url
                                                 : std::string{});
            if (!thumb_url.empty())
            {
                thumb = owner_.sticker_provider_(thumb_url);
            }
        }

        if (thumb)
        {
            text_w -= (kThumb + kThumbGap);
            if (text_w < 0)
            {
                text_w = 0;
            }
        }

        // Build preview string (cheap string ops, no TextLayout allocation).
        std::string preview;
        if (has_preview)
        {
            const std::string& kind   = room.last_message_kind;
            const std::string  sender = room.last_message_sender_name.empty()
                                            ? std::string("You")
                                            : room.last_message_sender_name;
            if (kind == "text")
            {
                preview = room.last_message_body;
                if (!room.is_direct)
                {
                    preview = sender + ": " + preview;
                }
            }
            else if (kind == "image")   { preview = sender + " sent an image"; }
            else if (kind == "video")   { preview = sender + " sent a video"; }
            else if (kind == "file")    { preview = sender + " sent a file"; }
            else if (kind == "audio")   { preview = sender + " sent a voice message"; }
            else if (kind == "sticker") { preview = sender + " sent a sticker"; }
        }

        // ── Text-layout cache lookup / rebuild ────────────────────────────
        // build_text is expensive (font shaping + measurement); we rebuild
        // only when the inputs that determine the layout actually change.
        const std::string display_name =
            room.name.empty() ? room.id : room.name;
        auto& cache = room_cache_[room.id];

        if (cache.display_name != display_name || cache.text_w != text_w ||
            cache.preview != preview || cache.badge_text != badge_text)
        {
            cache.display_name = display_name;
            cache.text_w       = text_w;
            cache.preview      = preview;
            cache.badge_text   = badge_text;

            tk::TextStyle name_style{};
            name_style.role      = tk::FontRole::Body;
            name_style.trim      = tk::TextTrim::Ellipsis;
            name_style.max_width = text_w;
            cache.name_layout    = ctx.factory.build_text(display_name, name_style);

            if (!preview.empty())
            {
                tk::TextStyle prev_style{};
                prev_style.role      = tk::FontRole::SidebarPreview;
                prev_style.trim      = tk::TextTrim::Ellipsis;
                prev_style.max_width = text_w;
                cache.preview_layout = ctx.factory.build_text(preview, prev_style);
            }
            else
            {
                cache.preview_layout = nullptr;
            }

            if (!badge_text.empty())
            {
                tk::TextStyle badge_style{};
                badge_style.role   = tk::FontRole::UnreadBadge;
                cache.badge_layout = ctx.factory.build_text(badge_text, badge_style);
            }
            else
            {
                cache.badge_layout = nullptr;
            }
        }

        // ── Draw from cache ───────────────────────────────────────────────
        if (cache.name_layout)
        {
            float name_y =
                has_preview
                    ? bounds.y +
                          (bounds.h * 0.5f - cache.name_layout->measure().h) * 0.5f
                    : bounds.y +
                          (bounds.h - cache.name_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*cache.name_layout, {text_x, name_y},
                                 ctx.theme.palette.text_primary);
        }

        if (has_preview)
        {
            if (cache.preview_layout)
            {
                float prev_y =
                    bounds.y + bounds.h * 0.5f +
                    (bounds.h * 0.5f - cache.preview_layout->measure().h) * 0.5f;
                ctx.canvas.draw_text(*cache.preview_layout, {text_x, prev_y},
                                     ctx.theme.palette.text_secondary);
            }

            if (thumb)
            {
                float thumb_right = bounds.x + bounds.w - kPadX -
                                    (badge_width > 0 ? badge_width + kPadX : 0.0f);
                float iw = thumb->width()  > 0 ? static_cast<float>(thumb->width())  : kThumb;
                float ih = thumb->height() > 0 ? static_cast<float>(thumb->height()) : kThumb;
                float s  = std::min(kThumb / iw, kThumb / ih);
                float fw = iw * s;
                float fh = ih * s;
                tk::Rect dst{thumb_right - kThumb + (kThumb - fw) * 0.5f,
                             bounds.y + (bounds.h - fh) * 0.5f,
                             fw, fh};
                ctx.canvas.draw_image(*thumb, dst);
                if (ctx.anim_damage)
                    ctx.anim_damage->note_image(thumb_url, dst);
            }
        }

        if (!badge_text.empty() && cache.badge_layout)
        {
            tk::Size badge_sz = cache.badge_layout->measure();
            float    pill_w   = std::max(kBadgeMinW, badge_sz.w + kBadgePadX * 2);
            tk::Rect pill{bounds.x + bounds.w - kPadX - pill_w,
                          bounds.y + (bounds.h - kBadgeH) * 0.5f,
                          pill_w, kBadgeH};
            const bool      is_mention = room.highlight_count > 0;
            const tk::Color pill_bg    = is_mention ? ctx.theme.palette.accent
                                                    : ctx.theme.palette.unread_bg;
            const tk::Color pill_text  = is_mention
                                             ? ctx.theme.palette.text_on_accent
                                             : ctx.theme.palette.unread_text;
            ctx.canvas.fill_rounded_rect(pill, kBadgeRadius, pill_bg);
            ctx.canvas.draw_text(*cache.badge_layout,
                                 {pill.x + (pill.w - badge_sz.w) * 0.5f,
                                  pill.y + (pill.h - badge_sz.h) * 0.5f},
                                 pill_text);
        }
    }

    void paint_invite(const tesseract::InviteInfo& inv, tk::PaintCtx& ctx,
                      tk::Rect bounds, bool selected, bool hovered)
    {
        // Row background — same as room rows.
        if (selected)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_selected);
        }
        else if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);
        }

        // Avatar circle (left-aligned, vertically centred).
        float avatar_cx = bounds.x + kPadX + kAvatarSize * 0.5f;
        float avatar_cy = bounds.y + bounds.h * 0.5f;

        // DM invite: show inviter avatar. Group invite: show room avatar.
        const std::string& av_mxc =
            inv.is_direct ? inv.inviter_avatar_url : inv.room_avatar_url;
        // Fallback initials text: inviter name for DM, room name for group.
        const std::string& initials_name =
            inv.is_direct ? (inv.inviter_display_name.empty()
                                 ? inv.inviter_user_id
                                 : inv.inviter_display_name)
                          : (inv.room_name.empty() ? inv.room_id : inv.room_name);

        const tk::Image* avatar = nullptr;
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
        }

        if (avatar)
        {
            ctx.canvas.draw_circle_image(*avatar, {avatar_cx, avatar_cy},
                                         kAvatarSize);
        }
        else
        {
            ctx.canvas.draw_initials_circle(
                initials_name, {avatar_cx, avatar_cy}, kAvatarSize,
                ctx.theme.palette.avatar_initials_bg,
                ctx.theme.palette.avatar_initials_text);
        }

        // Text column geometry (no badge reserved).
        float text_x = bounds.x + kPadX + kAvatarSize + kAvatarGap;
        float text_w = bounds.w - (text_x - bounds.x) - kPadX;
        if (text_w < 0)
        {
            text_w = 0;
        }

        // Primary / secondary text.
        // DM invite:    primary = inviter display name, secondary = @user_id
        // Group invite: primary = room name, secondary = "Invited by <name>"
        const std::string primary =
            inv.is_direct
                ? (inv.inviter_display_name.empty() ? inv.inviter_user_id
                                                    : inv.inviter_display_name)
                : (inv.room_name.empty() ? inv.room_id : inv.room_name);

        const std::string secondary =
            inv.is_direct
                ? inv.inviter_user_id
                : ("Invited by " + (inv.inviter_display_name.empty()
                                        ? inv.inviter_user_id
                                        : inv.inviter_display_name));

        // Cache keyed on room_id.
        auto& cache = room_cache_[inv.room_id];
        if (cache.display_name != primary || cache.text_w != text_w ||
            cache.preview != secondary)
        {
            cache.display_name = primary;
            cache.text_w       = text_w;
            cache.preview      = secondary;
            cache.badge_text   = {};

            tk::TextStyle name_style{};
            name_style.role      = tk::FontRole::Body;
            name_style.trim      = tk::TextTrim::Ellipsis;
            name_style.max_width = text_w;
            cache.name_layout    = ctx.factory.build_text(primary, name_style);

            tk::TextStyle prev_style{};
            prev_style.role      = tk::FontRole::SidebarPreview;
            prev_style.trim      = tk::TextTrim::Ellipsis;
            prev_style.max_width = text_w;
            cache.preview_layout = ctx.factory.build_text(secondary, prev_style);

            cache.badge_layout = nullptr;
        }

        if (cache.name_layout)
        {
            float name_y =
                bounds.y +
                (bounds.h * 0.5f - cache.name_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*cache.name_layout, {text_x, name_y},
                                 ctx.theme.palette.text_primary);
        }
        if (cache.preview_layout)
        {
            float prev_y =
                bounds.y + bounds.h * 0.5f +
                (bounds.h * 0.5f - cache.preview_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*cache.preview_layout, {text_x, prev_y},
                                 ctx.theme.palette.text_secondary);
        }
    }

    RoomListView& owner_;

    // When the factory pointer changes (DPI migration to a new screen) all
    // cached TextLayout objects are invalid and must be rebuilt.
    tk::CanvasFactory* factory_seen_ = nullptr;

    std::unordered_map<std::string, RoomRowCache>           room_cache_;
    HeaderRowCache header_cache_[RoomListView::kNumSections] = {};
};

// ─────────────────────────────────────────────────────────────────────────

RoomListView::~RoomListView() = default;

RoomListView::RoomListView() : adapter_(std::make_unique<Adapter>(*this))
{
    collapsed_[kSecInactive] = true; // Inactive starts collapsed to declutter.

    auto list = std::make_unique<tk::ListView>();
    list->set_adapter(adapter_.get());
    list->on_row_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= items_.size())
        {
            return;
        }
        const auto& item = items_[static_cast<std::size_t>(idx)];

        if (item.kind == Item::Kind::Header)
        {
            collapsed_[item.section] = !collapsed_[item.section];
            if (on_section_toggled)
                on_section_toggled(item.section, collapsed_[item.section]);
            rebuild_items();
            list_->invalidate_data();
            // Re-apply selection — the selected room may have just been
            // hidden or revealed by the toggle.
            set_selected_room(selected_room_id_cache_);
            if (on_scroll)
                on_scroll();
            return;
        }

        if (item.kind == Item::Kind::Invite)
        {
            if (!invites_ || item.room_idx < 0 ||
                item.room_idx >= static_cast<int>(invites_->size()))
            {
                return;
            }
            const std::string& rid =
                (*invites_)[static_cast<std::size_t>(item.room_idx)].room_id;
            if (on_invite_selected)
            {
                on_invite_selected(rid);
            }
            return;
        }

        const auto& rooms = section_rooms_[item.section];
        if (item.room_idx < 0 ||
            item.room_idx >= static_cast<int>(rooms.size()))
        {
            return;
        }
        selected_room_id_cache_ = rooms[item.room_idx]->id;
        if (on_room_selected)
        {
            on_room_selected(rooms[item.room_idx]->id);
        }
    };
    list->on_scroll = [this]
    {
        if (on_scroll)
        {
            on_scroll();
        }
    };
    list_ = add_child(std::move(list));
}

void RoomListView::set_rooms(std::vector<tesseract::RoomInfo> rooms)
{
    // heights_dirty_ is true until the first paint pass, so visible_range()
    // returns {0,-1} and visible_room_ids() returns {} on the very first call.
    // Track the empty→non-empty transition separately so the initial load
    // always fires on_scroll() and triggers the first backfill pass.
    bool first_load = rooms_.empty() && !rooms.empty();
    auto prev_visible = visible_room_ids();
    rooms_ = std::move(rooms);
    rebuild_items();
    // Sample BEFORE invalidate_data(): that call sets heights_dirty_, which
    // makes visible_range() return {0,-1} and visible_room_ids() return {}.
    // Capturing here means we read the rebuilt items_ with the pre-rebuild
    // row offsets (still valid since heights haven't been remeasured yet),
    // giving a correct comparison even when rows are uniform height.
    bool layout_changed = on_scroll &&
        (first_load || visible_room_ids() != prev_visible);
    if (list_)
    {
        list_->invalidate_data();
    }
    set_selected_room(selected_room_id_cache_);
    if (layout_changed)
        on_scroll();
}

void RoomListView::set_invites(const std::vector<tesseract::InviteInfo>* invites)
{
    invites_ = invites;
    rebuild_items();
    if (list_)
    {
        list_->invalidate_data();
    }
    set_selected_room(selected_room_id_cache_);
}

void RoomListView::refresh()
{
    rebuild_items();
    if (list_)
    {
        list_->invalidate_data();
    }
    set_selected_room(selected_room_id_cache_);
}

void RoomListView::set_section_collapsed(int section, bool collapsed)
{
    if (section < 0 || section >= kNumSections)
        return;
    if (collapsed_[section] == collapsed)
        return;
    collapsed_[section] = collapsed;
    rebuild_items();
    if (list_)
        list_->invalidate_data();
    if (on_scroll)
        on_scroll();
}

void RoomListView::set_avatar_provider(AvatarProvider p)
{
    avatar_provider_ = std::move(p);
}

void RoomListView::set_sticker_provider(StickerProvider p)
{
    sticker_provider_ = std::move(p);
}

void RoomListView::set_presence_provider(PresenceProvider p)
{
    presence_provider_ = std::move(p);
}

void RoomListView::set_selected_room(const std::string& room_id)
{
    selected_room_id_cache_ = room_id;
    if (!list_)
    {
        return;
    }
    if (room_id.empty())
    {
        list_->set_selected_index(-1);
        return;
    }
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        const auto& item = items_[static_cast<std::size_t>(i)];
        if (item.kind != Item::Kind::Room)
        {
            continue;
        }
        const auto& rooms = section_rooms_[item.section];
        if (item.room_idx >= 0 &&
            item.room_idx < static_cast<int>(rooms.size()) &&
            rooms[item.room_idx]->id == room_id)
        {
            list_->set_selected_index(i);
            return;
        }
    }
    list_->set_selected_index(-1);
}

std::string RoomListView::selected_room_id() const
{
    if (!list_)
    {
        return {};
    }
    int flat = list_->selected_index();
    if (flat >= 0 && flat < static_cast<int>(items_.size()))
    {
        const auto& item = items_[static_cast<std::size_t>(flat)];
        if (item.kind == Item::Kind::Room)
        {
            const auto& rooms = section_rooms_[item.section];
            if (item.room_idx >= 0 &&
                item.room_idx < static_cast<int>(rooms.size()))
            {
                return rooms[item.room_idx]->id;
            }
        }
    }
    return selected_room_id_cache_;
}

int RoomListView::selected_index() const
{
    if (!list_)
    {
        return -1;
    }
    int flat = list_->selected_index();
    if (flat < 0 || flat >= static_cast<int>(items_.size()))
    {
        return -1;
    }
    if (items_[static_cast<std::size_t>(flat)].kind == Item::Kind::Header ||
        items_[static_cast<std::size_t>(flat)].kind == Item::Kind::Invite)
    {
        return -1;
    }
    // Count visible room items before this flat index (headers excluded).
    int room_idx = 0;
    for (int i = 0; i < flat; ++i)
    {
        if (items_[static_cast<std::size_t>(i)].kind == Item::Kind::Room)
        {
            ++room_idx;
        }
    }
    return room_idx;
}

tk::Rect RoomListView::search_field_rect() const
{
    return search_field_rect_;
}
bool RoomListView::search_field_visible() const
{
    return search_field_visible_;
}

void RoomListView::set_search_text(std::string q)
{
    if (q == search_text_)
    {
        return;
    }
    search_text_ = std::move(q);
    rebuild_items();
    if (list_)
    {
        list_->invalidate_data();
    }
    set_selected_room(selected_room_id_cache_);
}

std::vector<std::string> RoomListView::visible_room_ids() const
{
    if (!list_)
    {
        return {};
    }
    auto [first, last] = list_->visible_range();
    if (last < first)
    {
        return {};
    }
    std::vector<std::string> ids;
    for (int i = first;
         i <= last && static_cast<std::size_t>(i) < items_.size(); ++i)
    {
        const auto& item = items_[static_cast<std::size_t>(i)];
        if (item.kind == Item::Kind::Room)
        {
            const auto& rooms = section_rooms_[item.section];
            if (item.room_idx >= 0 &&
                item.room_idx < static_cast<int>(rooms.size()))
            {
                ids.push_back(rooms[item.room_idx]->id);
            }
        }
    }
    return ids;
}

void RoomListView::rebuild_items()
{
    // 1. Clear buckets.
    for (auto& sr : section_rooms_)
    {
        sr.clear();
    }

    // 2. Classify each room into a section, applying the search filter.
    const auto& settings = tesseract::Settings::instance();
    const bool group_inactive = settings.group_inactive_rooms;
    const int threshold_days = settings.inactive_room_threshold_days;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    for (const auto& r : rooms_)
    {
        const std::string& hay = r.name.empty() ? r.id : r.name;
        if (!search_text_.empty() && !name_matches(hay, search_text_))
        {
            continue;
        }

        int sec = classify_room_section(r, group_inactive, threshold_days, now_ms);
        section_rooms_[sec].push_back(&r);
    }

    // 3. Build flat item list.
    items_.clear();

    // Invitations section — always comes first, never collapsed, no search
    // filter (invites are few and always shown in full).
    if (invites_ && !invites_->empty())
    {
        items_.push_back({Item::Kind::Header, kSecInvites, 0});
        for (int i = 0; i < static_cast<int>(invites_->size()); ++i)
        {
            items_.push_back({Item::Kind::Invite, kSecInvites, i});
        }
    }

    // Room sections — kSecInvites slot is skipped (its section_rooms_ is
    // always empty, but skip explicitly to be safe).
    for (int s = kSecFavorites; s < kNumSections; ++s)
    {
        if (section_rooms_[s].empty())
        {
            continue;
        }
        items_.push_back({Item::Kind::Header, s, 0});
        if (collapsed_[s] && search_text_.empty())
        {
            for (int r = 0; r < static_cast<int>(section_rooms_[s].size()); ++r)
            {
                if (section_rooms_[s][r]->notification_count > 0 ||
                    section_rooms_[s][r]->highlight_count > 0 ||
                    section_rooms_[s][r]->id == selected_room_id_cache_)
                {
                    items_.push_back({Item::Kind::Room, s, r});
                }
            }
            continue;
        }
        for (int r = 0; r < static_cast<int>(section_rooms_[s].size()); ++r)
        {
            items_.push_back({Item::Kind::Room, s, r});
        }
    }
}

float RoomListView::search_header_h() const
{
    return search_field_visible_ ? kSearchBarH : 0.0f;
}

tk::Size RoomListView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void RoomListView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!list_)
    {
        return;
    }

    bool wants_search = true;
    search_field_visible_ = wants_search;

    if (wants_search)
    {
        const float btn_side = kSearchBarH - 2.0f * kSearchBarInsetY;

        // "+" join-room button always anchored to the far right.
        join_room_rect_ = {
            bounds.x + bounds.w - kSearchBarInsetX - btn_side,
            bounds.y + kSearchBarInsetY,
            btn_side,
            btn_side,
        };

        search_field_rect_ = {
            bounds.x + kSearchBarInsetX,
            bounds.y + kSearchBarInsetY,
            std::max(0.0f, join_room_rect_.x - kSearchBarInsetX -
                               (bounds.x + kSearchBarInsetX)),
            std::max(0.0f, kSearchBarH - 2 * kSearchBarInsetY),
        };

        // Clear (×) button: shown to the left of "+" when the search query is
        // non-empty.  Shrink the text field rect further to leave room for it.
        if (!search_text_.empty())
        {
            const float btn_x = join_room_rect_.x - kSearchBarInsetX - btn_side;
            search_clear_rect_ = {
                btn_x,
                bounds.y + kSearchBarInsetY,
                btn_side,
                btn_side,
            };
            search_field_rect_.w =
                std::max(0.0f, btn_x - kSearchBarInsetX - search_field_rect_.x);
        }
        else
        {
            search_clear_rect_ = {};
        }

        tk::Rect list_bounds{
            bounds.x,
            bounds.y + kSearchBarH,
            bounds.w,
            std::max(0.0f, bounds.h - kSearchBarH),
        };
        list_->arrange(ctx, list_bounds);
    }
    else
    {
        search_field_rect_ = {};
        search_clear_rect_ = {};
        join_room_rect_ = {};
    }
}

void RoomListView::paint(tk::PaintCtx& ctx)
{
    if (search_field_visible_)
    {
        tk::Rect header_rect{bounds_.x, bounds_.y, bounds_.w, kSearchBarH};
        ctx.canvas.fill_rect(header_rect, ctx.theme.palette.sidebar_bg);
        tk::Rect sep{bounds_.x, bounds_.y + kSearchBarH - 1.0f, bounds_.w,
                     1.0f};
        ctx.canvas.fill_rect(sep, ctx.theme.palette.border);

        // Search field card — same style as the compose input.
        if (!search_field_rect_.empty())
        {
            ctx.canvas.fill_rounded_rect(search_field_rect_, 6.0f,
                                         ctx.theme.palette.compose_card_bg);
            ctx.canvas.stroke_rounded_rect(search_field_rect_, 6.0f,
                                           ctx.theme.palette.border, 1.0f);
        }

        // Clear (×) button — shown only when the search query is non-empty.
        if (!search_clear_rect_.empty())
        {
            tk::TextStyle xs{};
            xs.role = tk::FontRole::Body;
            // U+00D7 MULTIPLICATION SIGN (×)
            auto x_lo = ctx.factory.build_text(std::string("\xC3\x97"), xs);
            if (x_lo)
            {
                tk::Size sz = x_lo->measure();
                tk::Color col = press_search_clear_
                                    ? ctx.theme.palette.text_primary
                                    : ctx.theme.palette.text_muted;
                ctx.canvas.draw_text(*x_lo,
                                     {search_clear_rect_.x +
                                          (search_clear_rect_.w - sz.w) * 0.5f,
                                      search_clear_rect_.y +
                                          (search_clear_rect_.h - sz.h) * 0.5f},
                                     col);
            }
        }

        // Join room "+" button — always in the far right of the header.
        if (!join_room_rect_.empty())
        {
            if (press_join_room_)
            {
                ctx.canvas.fill_rounded_rect(join_room_rect_, 4.0f,
                                             ctx.theme.palette.sidebar_hover);
            }
            tk::TextStyle xs{};
            xs.role = tk::FontRole::UiSemibold;
            auto plus_lo = ctx.factory.build_text(std::string("+"), xs);
            if (plus_lo)
            {
                tk::Size sz = plus_lo->measure();
                tk::Color col = press_join_room_
                                    ? ctx.theme.palette.text_primary
                                    : ctx.theme.palette.accent;
                ctx.canvas.draw_text(
                    *plus_lo,
                    {join_room_rect_.x + (join_room_rect_.w - sz.w) * 0.5f,
                     join_room_rect_.y + (join_room_rect_.h - sz.h) * 0.5f},
                    col);
            }
        }
    }
    if (list_ && list_->visible())
    {
        list_->paint(ctx);
    }
}

bool RoomListView::on_pointer_down(tk::Point local)
{
    if (!list_)
    {
        return false;
    }
    press_search_clear_ = false;
    press_join_room_ = false;

    // Join room "+" button — highest priority, always in header.
    if (!join_room_rect_.empty() && local.x >= join_room_rect_.x &&
        local.x < join_room_rect_.x + join_room_rect_.w &&
        local.y >= join_room_rect_.y &&
        local.y < join_room_rect_.y + join_room_rect_.h)
    {
        press_join_room_ = true;
        return true;
    }

    // Clear (×) button.
    if (!search_clear_rect_.empty() && local.x >= search_clear_rect_.x &&
        local.x < search_clear_rect_.x + search_clear_rect_.w &&
        local.y >= search_clear_rect_.y &&
        local.y < search_clear_rect_.y + search_clear_rect_.h)
    {
        press_search_clear_ = true;
        return true;
    }

    if (local.y < search_header_h())
    {
        return false;
    }
    tk::Point list_local{local.x, local.y - search_header_h()};
    return list_->on_pointer_down(list_local);
}

void RoomListView::on_pointer_up(tk::Point local, bool inside_self)
{
    if (press_join_room_)
    {
        press_join_room_ = false;
        if (inside_self && !join_room_rect_.empty() &&
            local.x >= join_room_rect_.x &&
            local.x < join_room_rect_.x + join_room_rect_.w &&
            local.y >= join_room_rect_.y &&
            local.y < join_room_rect_.y + join_room_rect_.h)
        {
            if (on_join_room_requested)
            {
                on_join_room_requested();
            }
        }
        return;
    }
    if (press_search_clear_)
    {
        press_search_clear_ = false;
        if (inside_self && !search_clear_rect_.empty() &&
            local.x >= search_clear_rect_.x &&
            local.x < search_clear_rect_.x + search_clear_rect_.w &&
            local.y >= search_clear_rect_.y &&
            local.y < search_clear_rect_.y + search_clear_rect_.h)
        {
            if (on_search_clear)
            {
                on_search_clear();
            }
        }
        return;
    }
    if (!list_)
    {
        return;
    }
    tk::Point list_local{local.x, local.y - search_header_h()};
    bool inside_list = inside_self && local.y >= search_header_h();
    list_->on_pointer_up(list_local, inside_list);
}

} // namespace tesseract::views
