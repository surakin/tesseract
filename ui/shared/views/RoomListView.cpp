#include "RoomListView.h"

#include "html_spans.h"
#include "roomlist_unread.h"

#include "icons.h"
#include "media_utils.h"
#include "text_util.h"
#include "tk/i18n.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include <tesseract/settings.h>
#include <tesseract/visual.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tesseract::views
{

namespace
{

constexpr float kRoomListRowH = tesseract::visual::kRoomRowHeight;        // 48
constexpr float kRoomListAvatarSize = tesseract::visual::kRoomAvatarSize; // 36
constexpr float kRoomListPadX = 6.0f; // halved from kSpaceMD (12)
constexpr float kRoomListPadY = 4.0f; // halved from kSpaceSM (8)
constexpr float kRoomListAvatarGap = tesseract::visual::kSpaceMD;             // 12
constexpr float kBadgeMinW = tesseract::visual::kUnreadBadgeMinWidth; // 20
constexpr float kBadgeH = tesseract::visual::kUnreadBadgeHeight;      // 18
constexpr float kBadgePadX = 6.0f;
constexpr float kBadgeRadius = kBadgeH * 0.5f;
// Quiet-unread dot (rooms with unread messages but no notification).
constexpr float kDotSize = 8.0f;

// Thumbnail chip painted on the right side of image/sticker rows.
constexpr float kThumb = kRoomListRowH - kRoomListPadY * 2.0f; // 40 px — full usable row height
constexpr float kThumbGap = 4.0f;

// Section header row dimensions.
constexpr float kRoomListHeaderH = 28.0f;
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

using tesseract::text::name_matches;

} // namespace

// ─────────────────────────────────────────────────────────────────────────

std::vector<tesseract::RoomInfo> filter_root_rooms(
    const std::vector<tesseract::RoomInfo>& rooms,
    const std::unordered_map<std::string, std::vector<std::string>>& sc_cache,
    bool group_unread_rooms)
{
    // Build the set of rooms that are children of any space.
    std::unordered_set<std::string> in_space;
    for (const auto& r : rooms)
    {
        if (!r.is_space)
            continue;
        auto it = sc_cache.find(r.id);
        if (it != sc_cache.end())
            for (const auto& id : it->second)
                in_space.insert(id);
    }

    std::vector<tesseract::RoomInfo> filtered;
    filtered.reserve(rooms.size());

    // Non-space rooms first.
    for (const auto& r : rooms)
    {
        if (r.is_space)
            continue;
        if (in_space.count(r.id) && !r.is_favorite)
        {
            // Space-child and not a favorite: include only if group_unread is on
            // and the room has a visible unread indicator.
            if (!group_unread_rooms ||
                unread_style_for(r.notification_count, r.highlight_count,
                                 r.unread_count, r.muted) == UnreadStyle::None)
                continue;
        }
        filtered.push_back(r);
    }

    // Spaces: only top-level spaces (not children of another space) or favorites.
    for (const auto& r : rooms)
    {
        if (r.is_space && (!in_space.count(r.id) || r.is_favorite))
            filtered.push_back(r);
    }

    return filtered;
}

int classify_room_section(const tesseract::RoomInfo& r,
                          bool group_unread, bool group_inactive,
                          int threshold_days, std::uint64_t now_ms)
{
    // Favorites and Spaces are never moved to Unread or Inactive.
    if (r.is_favorite)
        return RoomListView::kSecFavorites;

    // Unread check runs before Inactive so an inactive+unread room surfaces
    // in the Unread section where the user can find it. Applies to DMs and
    // regular rooms only (not spaces, consistent with Inactive's scope).
    if (group_unread && !r.is_space)
    {
        using tesseract::views::UnreadStyle;
        using tesseract::views::unread_style_for;
        if (unread_style_for(r.notification_count, r.highlight_count,
                             r.unread_count, r.muted) != UnreadStyle::None)
            return RoomListView::kSecUnread;
    }

    if (group_inactive && !r.is_space && r.last_activity_ts != 0)
    {
        std::uint64_t threshold_ms =
            static_cast<std::uint64_t>(threshold_days) * 86'400'000ULL;
        if (r.last_activity_ts <= now_ms &&
            now_ms - r.last_activity_ts > threshold_ms)
            return RoomListView::kSecInactive;
    }

    if (r.is_direct)
        return RoomListView::kSecDMs;
    if (!r.is_space)
        return RoomListView::kSecRooms;
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
            return kRoomListRowH;
        }
        return owner_.items_[index].kind == Item::Kind::Header ? kRoomListHeaderH
                                                               : kRoomListRowH;
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
            preview_metrics_layout_.reset();
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
        else if (item.kind == Item::Kind::SpaceUnjoined)
        {
            const auto& unjoined = owner_.space_unjoined_rooms_;
            if (item.room_idx < 0 ||
                item.room_idx >= static_cast<int>(unjoined.size()))
            {
                return;
            }
            paint_unjoined_room(unjoined[static_cast<std::size_t>(item.room_idx)],
                                ctx, bounds, selected, hovered);
            if (index > 0 &&
                owner_.items_[index - 1].kind == Item::Kind::SpaceUnjoined)
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
        bool        unread     = false; // bold title when there are unreads
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
        std::uint64_t section_unread       = 0;
        bool          section_mention      = false;
        bool          section_quiet_unread = false;
        bool          valid           = false; // false forces rebuild on first use
        // cached layouts
        std::unique_ptr<tk::TextLayout> title_layout;
        std::unique_ptr<tk::TextLayout> chevron_layout;
        std::unique_ptr<tk::TextLayout> badge_layout; // nullptr when unread == 0
    };

    void paint_header(const Item& item, tk::PaintCtx& ctx, tk::Rect bounds,
                      bool hovered)
    {
        ctx.canvas.fill_rect(bounds,
                             hovered
                                 ? ctx.theme.palette.section_header_hover
                                 : ctx.theme.palette.section_header_bg);

        // Build the title string. Invitations and Inactive sections include counts.
        std::string title_str;
        if (item.section == RoomListView::kSecInvites)
        {
            const std::size_t n =
                owner_.invites_ ? owner_.invites_->size() : 0u;
            title_str = tk::trf(tk::tr("Invitations ({0})"), {std::to_string(n)});
        }
        else if (item.section == RoomListView::kSecInactive)
        {
            const std::size_t n = owner_.section_rooms_[kSecInactive].size();
            title_str = tk::trf(tk::tr("Inactive ({0})"), {std::to_string(n)});
        }
        else
        {
            title_str = tk::tr(RoomListView::kSectionTitles[item.section]);
        }
        const std::string& title  = title_str;
        bool               collapsed = owner_.collapsed_[item.section];

        // Sum notification counts and check for mentions (badge when collapsed).
        // Invitations have no unread badge.
        std::uint64_t section_unread       = 0;
        bool          section_mention      = false;
        bool          section_quiet_unread = false;
        if (collapsed && item.section != RoomListView::kSecInvites)
        {
            for (const auto* r : owner_.section_rooms_[item.section])
            {
                section_unread  += r->notification_count;
                section_mention  = section_mention || (r->highlight_count > 0);
                section_quiet_unread =
                    section_quiet_unread ||
                    unread_style_for(r->notification_count, r->highlight_count,
                                     r->unread_count, r->muted) ==
                        UnreadStyle::Dot;
            }
        }

        // Rebuild layouts when any key field changes.
        auto& cache = header_cache_[item.section];
        if (!cache.valid || cache.title != title || cache.collapsed != collapsed ||
            cache.section_unread != section_unread ||
            cache.section_mention != section_mention ||
            cache.section_quiet_unread != section_quiet_unread)
        {
            cache.title                = title;
            cache.collapsed            = collapsed;
            cache.section_unread       = section_unread;
            cache.section_mention      = section_mention;
            cache.section_quiet_unread = section_quiet_unread;
            cache.valid                = true;

            tk::TextStyle ts{};
            ts.role            = tk::FontRole::Caption;
            cache.title_layout = ctx.factory.build_text(title, ts);

            const char*   chevron = collapsed ? "\xE2\x96\xB8" : "\xE2\x96\xBE";
            tk::TextStyle cs{};
            cs.role              = tk::FontRole::UiSemibold;
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
                                 ctx.theme.palette.text_primary);
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
        else if (collapsed && section_quiet_unread)
        {
            // No notifying rooms, but some collapsed room has quiet unread:
            // mirror the per-row dot on the section header.
            tk::Rect dot{chevron_x - kBadgePadX - kDotSize,
                         bounds.y + (bounds.h - kDotSize) * 0.5f,
                         kDotSize, kDotSize};
            ctx.canvas.fill_rounded_rect(dot, kDotSize * 0.5f,
                                         ctx.theme.palette.unread_bg);
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
        float avatar_cx = bounds.x + kRoomListPadX + kRoomListAvatarSize * 0.5f;
        float avatar_cy = bounds.y + bounds.h * 0.5f;

        const tk::Image* avatar = nullptr;
        const std::string& av_mxc = room.effective_avatar_url();
        if (owner_.avatar_provider_ && !av_mxc.empty())
        {
            avatar = owner_.avatar_provider_(av_mxc);
            // Lazy fetch: this row is being painted, so it's visible — request
            // the avatar on a cache miss. Collapsed / off-screen rooms never
            // reach paint, so they never trigger a fetch.
            if (!avatar && owner_.on_room_avatar_needed)
                owner_.on_room_avatar_needed(room);
        }

        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kRoomListAvatarSize,
                    room.name, ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

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
                const float dot_cx    = avatar_cx + kRoomListAvatarSize * 0.5f;
                const float dot_cy    = avatar_cy + kRoomListAvatarSize * 0.5f;
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
        float text_x = bounds.x + kRoomListPadX + kRoomListAvatarSize + kRoomListAvatarGap;
        float text_w = bounds.w - (text_x - bounds.x) - kRoomListPadX;

        // Reserve space for the badge (heuristic width keeps text_w stable
        // across frames so the name/preview layouts aren't rebuilt every time
        // the notification count increments by 1).
        const UnreadStyle unread_style = unread_style_for(
            room.notification_count, room.highlight_count, room.unread_count,
            room.muted);
        const bool show_count = unread_style == UnreadStyle::Count ||
                                unread_style == UnreadStyle::Mention;
        const bool show_dot = unread_style == UnreadStyle::Dot;

        float       badge_width = 0;
        std::string badge_text;
        if (show_count)
        {
            badge_text = format_unread(room.notification_count);
            badge_width = std::max(
                kBadgeMinW,
                kBadgePadX * 2 + 7.0f * static_cast<float>(badge_text.size()));
            text_w -= (badge_width + kRoomListPadX);
        }
        else if (show_dot)
        {
            badge_width = kDotSize; // reserve the right-aligned dot slot
            text_w -= (badge_width + kRoomListPadX);
        }
        // Reserve space for the active-call indicator (phone icon, right-aligned
        // to the right of the badge/dot slot).
        constexpr float kCallIconPx  = 14.0f;
        constexpr float kCallIconGap = 4.0f;
        const float call_icon_w = room.has_active_call
                                      ? kCallIconPx + kCallIconGap
                                      : 0.f;
        text_w -= call_icon_w;
        if (text_w < 0)
        {
            text_w = 0;
        }

        bool has_preview = !room.last_message_kind.empty();

        // Thumbnail lookup.
        const tk::Image* thumb     = nullptr;
        std::string      thumb_url;
        const bool is_own_sender = room.last_message_sender_name.empty();
        const bool media_ok      = !owner_.media_allowed_provider_
                                   || owner_.media_allowed_provider_(room.id, is_own_sender);
        if (!room.is_space && has_preview && owner_.sticker_provider_ && media_ok)
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
        if (room.is_space)
        {
            preview     = room.topic;
            has_preview = !preview.empty();
        }
        else if (has_preview)
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
            else if (kind == "gif")     { preview = sender + " sent a GIF"; }
            else if (kind == "file")    { preview = sender + " sent a file"; }
            else if (kind == "audio")   { preview = sender + " sent a voice message"; }
            else if (kind == "sticker") { preview = sender + " sent a sticker"; }
        }

        // ── Text-layout cache lookup / rebuild ────────────────────────────
        // build_text is expensive (font shaping + measurement); we rebuild
        // only when the inputs that determine the layout actually change.
        const std::string display_name =
            room.name.empty() ? room.id : room.name;
        const bool unread = unread_style != UnreadStyle::None;
        auto& cache = room_cache_[room.id];

        if (cache.display_name != display_name || cache.text_w != text_w ||
            cache.preview != preview || cache.badge_text != badge_text ||
            cache.unread != unread)
        {
            cache.display_name = display_name;
            cache.text_w       = text_w;
            cache.preview      = preview;
            cache.badge_text   = badge_text;
            cache.unread       = unread;

            // Unread rooms get a semibold title (SidebarName is the Body
            // point-size at DemiBold weight); read rooms stay regular Body.
            tk::TextStyle name_style{};
            name_style.role      = unread ? tk::FontRole::SidebarName
                                          : tk::FontRole::Body;
            name_style.trim      = tk::TextTrim::Ellipsis;
            name_style.max_width = text_w;
            cache.name_layout    = ctx.factory.build_text(display_name, name_style);

            if (!preview.empty())
            {
                tk::TextStyle prev_style{};
                prev_style.role      = tk::FontRole::SidebarPreview;
                prev_style.trim      = tk::TextTrim::Ellipsis;
                prev_style.max_width = text_w;

                // build_text folds hard line breaks for wrap=false styles
                // internally (see tk::fold_hard_breaks_utf8) so a multi-line
                // message body still renders on one line; build_rich_text
                // does not do this for us, so fold before segmenting.
                tk::TextSpan whole;
                whole.text = tk::fold_hard_breaks_utf8(preview);
                cache.preview_layout = ctx.factory.build_rich_text(
                    tesseract::views::segment_emoji_runs(whole), prev_style);
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
                if (!preview_metrics_layout_)
                {
                    tk::TextStyle metrics_style{};
                    metrics_style.role = tk::FontRole::SidebarPreview;
                    // Must match prev_style's trim (Ellipsis) below: on
                    // macOS, CTLayout::ascent() returns a genuinely
                    // different value (true typographic ascent vs. full box
                    // height) depending on whether the layout is single-line
                    // elided, so the reference and real layout need to take
                    // the same code path for their ascent()s to be
                    // comparable.
                    metrics_style.trim = tk::TextTrim::Ellipsis;
                    preview_metrics_layout_ =
                        ctx.factory.build_text(" ", metrics_style);
                }

                tk::Rect preview_area{text_x, bounds.y + bounds.h * 0.5f,
                                      text_w, bounds.h * 0.5f};
                // An inline-emoji run inflates measure()/ascent() (it's
                // taller than plain SidebarPreview text), so centering by the
                // *actual* layout's height would move its baseline — and with
                // it the surrounding regular text — down whenever an emoji is
                // present. Anchor the baseline instead at the fixed spot a
                // plain (no-emoji) preview line would occupy — from
                // preview_metrics_layout_, which never varies with content —
                // then offset the real layout so *its* baseline lands there
                // too. The emoji's extra height then only grows upward from
                // an unmoved baseline, clipped by preview_area.
                const float baseline_y =
                    preview_area.y +
                    (preview_area.h - preview_metrics_layout_->measure().h) *
                        0.5f +
                    preview_metrics_layout_->ascent();
                const float prev_y = baseline_y - cache.preview_layout->ascent();
                ctx.canvas.push_clip_rect(preview_area);
                ctx.canvas.draw_text(*cache.preview_layout, {text_x, prev_y},
                                     ctx.theme.palette.text_secondary);
                ctx.canvas.pop_clip();
            }

            if (thumb)
            {
                float thumb_right = bounds.x + bounds.w - kRoomListPadX - call_icon_w -
                                    (badge_width > 0 ? badge_width + kRoomListPadX : 0.0f);
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
            tk::Rect pill{bounds.x + bounds.w - kRoomListPadX - call_icon_w - pill_w,
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
        else if (show_dot)
        {
            // Quiet unread (messages but no notification): a small neutral dot
            // where the count pill would sit.
            tk::Rect dot{bounds.x + bounds.w - kRoomListPadX - call_icon_w - kDotSize,
                         bounds.y + (bounds.h - kDotSize) * 0.5f,
                         kDotSize, kDotSize};
            ctx.canvas.fill_rounded_rect(dot, kDotSize * 0.5f,
                                         ctx.theme.palette.unread_bg);
        }

        if (room.has_active_call)
        {
            tk::Rect icon_box{bounds.x + bounds.w - kRoomListPadX - kCallIconPx,
                              bounds.y + (bounds.h - kCallIconPx) * 0.5f,
                              kCallIconPx, kCallIconPx};
            owner_.call_icon_.draw(ctx.canvas, ctx.factory, kPhoneSvg,
                                   icon_box, kCallIconPx,
                                   ctx.theme.palette.accent);
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
        float avatar_cx = bounds.x + kRoomListPadX + kRoomListAvatarSize * 0.5f;
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

        // MSC4278: suppress invite avatars when disabled → initials fallback.
        const tk::Image* avatar = nullptr;
        if (owner_.avatar_provider_ && !av_mxc.empty() &&
            tesseract::Settings::instance().invite_avatars)
        {
            avatar = owner_.avatar_provider_(av_mxc);
        }

        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kRoomListAvatarSize,
                    initials_name, ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

        // Text column geometry (no badge reserved).
        float text_x = bounds.x + kRoomListPadX + kRoomListAvatarSize + kRoomListAvatarGap;
        float text_w = bounds.w - (text_x - bounds.x) - kRoomListPadX;
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

    void paint_unjoined_room(const tesseract::RoomSummary& s,
                             tk::PaintCtx& ctx, tk::Rect bounds,
                             bool selected, bool hovered)
    {
        const auto& pal = ctx.theme.palette;

        if (selected)
            ctx.canvas.fill_rect(bounds, pal.sidebar_selected);
        else if (hovered)
            ctx.canvas.fill_rect(bounds, pal.sidebar_hover);

        float avatar_cx = bounds.x + kRoomListPadX + kRoomListAvatarSize * 0.5f;
        float avatar_cy = bounds.y + bounds.h * 0.5f;

        const bool loading = s.name.empty();

        // Per-row lazy fetch: fire once per room_id; host deduplicates in-flight requests.
        if (loading && owner_.on_unjoined_room_summary_needed)
            owner_.on_unjoined_room_summary_needed(s.room_id);

        const tk::Image* avatar = nullptr;
        if (!loading && owner_.avatar_provider_ && !s.avatar_url.empty())
        {
            avatar = owner_.avatar_provider_(s.avatar_url);
            if (!avatar && owner_.on_unjoined_room_avatar_needed)
                owner_.on_unjoined_room_avatar_needed(s);
        }
        // Use room_id for the initials disc even while loading so each row has a
        // distinct placeholder colour.
        const std::string& initials_src = loading ? s.room_id : s.name;
        draw_avatar(ctx.canvas, avatar, {avatar_cx, avatar_cy}, kRoomListAvatarSize,
                    initials_src, pal.avatar_initials_bg, pal.avatar_initials_text);

        float text_x = bounds.x + kRoomListPadX + kRoomListAvatarSize + kRoomListAvatarGap;
        float text_w = bounds.w - (text_x - bounds.x) - kRoomListPadX;
        if (text_w < 0) text_w = 0;

        // Name + members/join-rule on two lines, muted to signal unjoined.
        constexpr std::uint8_t kAlpha = 153; // ~60%

        const std::string display = loading ? tk::tr("Loading\xe2\x80\xa6") : s.name;
        std::string meta;
        if (loading)
        {
            meta = tk::tr("Fetching room details\xe2\x80\xa6");
        }
        else
        {
            meta = std::to_string(s.num_joined_members) + " " + tk::tr("members");
            if (s.join_rule == "knock")            meta += " \xc2\xb7 " + tk::tr("Knock");
            else if (s.join_rule == "invite")      meta += " \xc2\xb7 " + tk::tr("Invite-only");
            else if (s.join_rule == "restricted")  meta += " \xc2\xb7 " + tk::tr("Restricted");
        }

        auto& cache = room_cache_[s.room_id];
        if (cache.display_name != display || cache.text_w != text_w ||
            cache.preview != meta)
        {
            cache.display_name = display;
            cache.text_w       = text_w;
            cache.preview      = meta;
            cache.badge_layout = nullptr;

            tk::TextStyle name_style{};
            name_style.role      = tk::FontRole::Body;
            name_style.trim      = tk::TextTrim::Ellipsis;
            name_style.max_width = text_w;
            cache.name_layout    = ctx.factory.build_text(display, name_style);

            tk::TextStyle prev_style{};
            prev_style.role      = tk::FontRole::SidebarPreview;
            prev_style.trim      = tk::TextTrim::Ellipsis;
            prev_style.max_width = text_w;
            cache.preview_layout = ctx.factory.build_text(meta, prev_style);
        }

        if (cache.name_layout)
        {
            float name_y = bounds.y +
                           (bounds.h * 0.5f - cache.name_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*cache.name_layout, {text_x, name_y},
                                 pal.text_primary.with_alpha(kAlpha));
        }
        if (cache.preview_layout)
        {
            float prev_y = bounds.y + bounds.h * 0.5f +
                           (bounds.h * 0.5f - cache.preview_layout->measure().h) * 0.5f;
            ctx.canvas.draw_text(*cache.preview_layout, {text_x, prev_y},
                                 pal.text_secondary.with_alpha(kAlpha));
        }
    }

    RoomListView& owner_;

    // When the factory pointer changes (DPI migration to a new screen) all
    // cached TextLayout objects are invalid and must be rebuilt.
    tk::CanvasFactory* factory_seen_ = nullptr;

    // A stable, content-independent reference layout (plain FontRole::
    // SidebarPreview, no emoji) whose ascent/height anchor the preview
    // baseline in paint_row — see the comment at its use site.
    std::unique_ptr<tk::TextLayout> preview_metrics_layout_;

    std::unordered_map<std::string, RoomRowCache>           room_cache_;
    HeaderRowCache header_cache_[RoomListView::kNumSections] = {};
};

// ─────────────────────────────────────────────────────────────────────────

RoomListView::~RoomListView() = default;

RoomListView::RoomListView(tk::Host* host)
    : adapter_(std::make_unique<Adapter>(*this))
{
    collapsed_[kSecInactive] = true; // Inactive starts collapsed to declutter.

    auto list = std::make_unique<tk::ListView>();
    list->set_adapter(adapter_.get());
    // Mouse-driven only: row selection/collapse-toggle below is wired via
    // on_row_clicked (fired from on_pointer_up, independent of focusable()),
    // not Tab/arrow-key navigation. Leaving this list Tab-focusable would
    // make Host::dispatch_pointer_down's click-focus logic move keyboard
    // focus onto the room list itself on an ordinary click (even a
    // re-click of the already-active room, or a section-header click),
    // stealing focus from the compose box for no reason.
    // Stays focusable() (Tab/arrow-key row navigation is a real,
    // intentional keyboard feature) but opts out of the click-focus grab:
    // row selection/collapse-toggle below is wired via on_row_clicked
    // (fired from on_pointer_up, independent of focus_on_click()), so an
    // ordinary mouse click already performs a complete action on its own
    // and shouldn't also steal keyboard focus from the compose box (e.g.
    // re-clicking the already-active room, or clicking a section header).
    list->set_focus_on_click(false);
    list->on_row_clicked = [this](int idx)
    {
        if (idx < 0 || static_cast<std::size_t>(idx) >= items_.size())
        {
            return;
        }
        const auto& item = items_[static_cast<std::size_t>(idx)];

        if (item.kind == Item::Kind::Header)
        {
            toggle_section_collapsed_(item.section);
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

        if (item.kind == Item::Kind::SpaceUnjoined)
        {
            if (item.room_idx < 0 ||
                item.room_idx >= static_cast<int>(space_unjoined_rooms_.size()))
            {
                return;
            }
            if (on_unjoined_room_selected)
            {
                on_unjoined_room_selected(
                    space_unjoined_rooms_[static_cast<std::size_t>(item.room_idx)]);
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

    if (host)
    {
        auto search = std::make_unique<tk::TextField>(
            *host, kSearchBarH - 2.0f * kSearchBarInsetY);
        search->set_placeholder(tk::tr("Search rooms\xe2\x80\xa6"));
        search_field_ = add_child(std::move(search));
    }
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
    // After the section buckets are rebuilt: bring the most-recent unread room
    // into view if new activity arrived (and the user enabled the behavior).
    autoscroll_to_unread_();
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

void RoomListView::set_space_unjoined_rooms(
    std::vector<tesseract::RoomSummary> rooms)
{
    space_unjoined_rooms_ = std::move(rooms);
    rebuild_items();
    if (list_)
    {
        list_->invalidate_data();
    }
}

void RoomListView::clear_space_unjoined_rooms()
{
    space_unjoined_rooms_.clear();
    rebuild_items();
    if (list_)
    {
        list_->invalidate_data();
    }
}

void RoomListView::update_unjoined_room_summary(const tesseract::RoomSummary& s)
{
    for (auto& entry : space_unjoined_rooms_)
    {
        if (entry.room_id == s.room_id)
        {
            entry = s;
            break;
        }
    }
    if (list_)
        list_->invalidate_data();
}

int RoomListView::unjoined_room_count() const
{
    return static_cast<int>(space_unjoined_rooms_.size());
}

bool RoomListView::unjoined_rows_visible() const
{
    if (space_unjoined_rooms_.empty())
        return false;
    return !collapsed_[kSecSpaceUnjoined];
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

float RoomListView::scroll_fraction() const
{
    return list_ ? list_->scroll_fraction() : 0.f;
}

void RoomListView::scroll_to_offset(float t)
{
    if (list_)
        list_->scroll_to_offset(t);
}

std::array<bool, RoomListView::kNumSections> RoomListView::collapsed_state() const
{
    std::array<bool, kNumSections> s;
    std::copy(collapsed_, collapsed_ + kNumSections, s.begin());
    return s;
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

void RoomListView::set_media_allowed_provider(MediaAllowedProvider p)
{
    media_allowed_provider_ = std::move(p);
}

int RoomListView::item_index_for_room_(const std::string& id) const
{
    if (id.empty())
    {
        return -1;
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
            rooms[item.room_idx]->id == id)
        {
            return i;
        }
    }
    return -1;
}

void RoomListView::set_selected_room(const std::string& room_id)
{
    selected_room_id_cache_ = room_id;
    if (!list_)
    {
        return;
    }
    list_->set_selected_index(item_index_for_room_(room_id));
}

const tesseract::RoomInfo* RoomListView::most_recent_unread_active_() const
{
    const tesseract::RoomInfo* best = nullptr;
    // Favorites/DMs/Rooms first, Spaces last: on equal timestamps a concrete
    // unread room is preferred over the space that aggregates it.
    for (int sec : {kSecUnread, kSecFavorites, kSecDMs, kSecRooms, kSecSpaces})
    {
        for (const auto* r : section_rooms_[sec])
        {
            if (r->notification_count == 0 || r->is_low_priority)
            {
                continue;
            }
            if (!best || r->last_activity_ts > best->last_activity_ts)
            {
                best = r;
            }
        }
    }
    return best;
}

void RoomListView::autoscroll_to_unread_()
{
    if (!list_ || !tesseract::Settings::instance().autoscroll_unread_rooms)
    {
        return;
    }
    const tesseract::RoomInfo* target = most_recent_unread_active_();
    if (!target || target->last_activity_ts <= last_unread_scroll_ts_)
    {
        return;
    }
    last_unread_scroll_ts_ = target->last_activity_ts;
    int idx = item_index_for_room_(target->id);
    if (idx >= 0)
    {
        // Deferred: set_rooms just marked heights dirty, so row offsets aren't
        // valid until the next arrange()/paint pass.
        list_->scroll_to_index_deferred(idx, /*align_top=*/false);
    }
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

void RoomListView::on_theme_changed(const tk::Theme& t)
{
    if (search_field_)
        search_field_->set_text_color(t.palette.text_primary);
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
    const bool group_unread   = settings.group_unread_rooms;
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

        int sec = classify_room_section(r, group_unread, group_inactive, threshold_days, now_ms);
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

    // Emit kSecUnread between Invitations and Favorites.
    if (!section_rooms_[kSecUnread].empty())
    {
        items_.push_back({Item::Kind::Header, kSecUnread, 0});
        if (collapsed_[kSecUnread] && search_text_.empty())
        {
            for (int i = 0; i < static_cast<int>(section_rooms_[kSecUnread].size()); ++i)
            {
                const auto* r = section_rooms_[kSecUnread][i];
                if (r->notification_count > 0 || r->highlight_count > 0 ||
                    r->id == selected_room_id_cache_)
                    items_.push_back({Item::Kind::Room, kSecUnread, i});
            }
        }
        else
        {
            for (int i = 0; i < static_cast<int>(section_rooms_[kSecUnread].size()); ++i)
                items_.push_back({Item::Kind::Room, kSecUnread, i});
        }
    }

    // Room sections — kSecInvites and kSecSpaceUnjoined slots are skipped
    // (they use separate data sources; kSecSpaceUnjoined is appended below).
    for (int s = kSecFavorites; s < kNumSections; ++s)
    {
        if (s == kSecSpaceUnjoined || s == kSecUnread)
            continue; // handled separately above/below
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

    // "Available to Join" — driven by space_unjoined_rooms_, not section_rooms_.
    // Only shown when the caller has populated it (i.e. drilled into a space).
    if (!space_unjoined_rooms_.empty())
    {
        items_.push_back({Item::Kind::Header, kSecSpaceUnjoined, 0});
        if (!collapsed_[kSecSpaceUnjoined])
        {
            for (int i = 0;
                 i < static_cast<int>(space_unjoined_rooms_.size()); ++i)
            {
                items_.push_back(
                    {Item::Kind::SpaceUnjoined, kSecSpaceUnjoined, i});
            }
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

        if (search_field_)
        {
            search_field_->set_visible(true);
            search_field_->arrange(ctx, search_field_rect_);
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
        if (search_field_)
            search_field_->set_visible(false);
        search_field_rect_ = {};
        search_clear_rect_ = {};
        join_room_rect_ = {};
    }
}

void RoomListView::toggle_section_collapsed_(int section)
{
    if (section < 0 || section >= kNumSections || !list_)
    {
        return;
    }
    collapsed_[section] = !collapsed_[section];
    if (on_section_toggled)
        on_section_toggled(section, collapsed_[section]);
    rebuild_items();
    list_->invalidate_data();
    // Re-apply selection — the selected room may have just been hidden or
    // revealed by the toggle.
    set_selected_room(selected_room_id_cache_);
    if (on_scroll)
        on_scroll();
}

RoomListView::StickyHeader RoomListView::sticky_header_() const
{
    StickyHeader s;
    if (!list_ || items_.empty())
    {
        return s;
    }
    auto [first, last] = list_->visible_range();
    (void)last;
    if (first < 0 || static_cast<std::size_t>(first) >= items_.size())
    {
        return s;
    }

    // Find the header item for the section whose rows sit at the viewport top.
    const int section = items_[static_cast<std::size_t>(first)].section;
    int       hdr     = -1;
    if (items_[static_cast<std::size_t>(first)].kind == Item::Kind::Header)
    {
        hdr = first;
    }
    else
    {
        for (int i = first; i >= 0; --i)
        {
            const auto& it = items_[static_cast<std::size_t>(i)];
            if (it.kind == Item::Kind::Header && it.section == section)
            {
                hdr = i;
                break;
            }
        }
    }
    if (hdr < 0)
    {
        return s;
    }

    const float list_top = bounds_.y + search_header_h();

    // If the real header is at or below the viewport top it renders normally —
    // no overlay needed (and drawing one would double up).
    if (list_->row_world_rect(hdr).y >= list_top)
    {
        return s;
    }

    float world_y = list_top;
    // The next section's header pushes the pinned header up as it approaches.
    for (int i = hdr + 1; i < static_cast<int>(items_.size()); ++i)
    {
        if (items_[static_cast<std::size_t>(i)].kind == Item::Kind::Header)
        {
            const float next_y = list_->row_world_rect(i).y;
            if (next_y < list_top + kRoomListHeaderH)
            {
                world_y = next_y - kRoomListHeaderH;
            }
            break;
        }
    }

    s.show        = true;
    s.header_item = hdr;
    s.section     = section;
    s.world_y     = world_y;
    return s;
}

bool RoomListView::sticky_band_contains_(const StickyHeader& s,
                                         tk::Point world) const
{
    if (!s.show)
    {
        return false;
    }
    const float list_top = bounds_.y + search_header_h();
    return world.x >= bounds_.x && world.x < bounds_.x + bounds_.w &&
           world.y >= list_top && world.y < s.world_y + kRoomListHeaderH;
}

tk::Widget* RoomListView::dispatch_pointer_down(tk::Point world)
{
    // The pinned header must win over the rows the inner ListView would
    // otherwise claim first (children are tried before this widget).
    if (visible() && contains_world(world))
    {
        StickyHeader s = sticky_header_();
        if (sticky_band_contains_(s, world))
        {
            press_sticky_section_ = s.section;
            return this;
        }
    }
    return tk::Widget::dispatch_pointer_down(world);
}

tk::Widget* RoomListView::dispatch_pointer_move(tk::Point world, bool* dirty)
{
    if (visible() && contains_world(world))
    {
        StickyHeader s = sticky_header_();
        if (sticky_band_contains_(s, world))
        {
            if (!sticky_hovered_)
            {
                sticky_hovered_ = true;
                if (dirty)
                    *dirty = true;
            }
            // Clear any row hover beneath the pinned header so two rows don't
            // appear highlighted at once.
            if (list_)
                list_->on_pointer_leave();
            return this;
        }
    }
    if (sticky_hovered_)
    {
        sticky_hovered_ = false;
        if (dirty)
            *dirty = true;
    }
    return tk::Widget::dispatch_pointer_move(world, dirty);
}

void RoomListView::on_pointer_leave()
{
    sticky_hovered_ = false;
    if (list_)
        list_->on_pointer_leave();
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
            // Lucide join (+) icon, tinted accent (or text_primary when pressed).
            join_icon_.draw(ctx.canvas, ctx.factory, kJoinSvg, join_room_rect_,
                            18.0f,
                            press_join_room_ ? ctx.theme.palette.text_primary
                                             : ctx.theme.palette.accent);
        }
    }
    if (list_ && list_->visible())
    {
        list_->paint(ctx);

        // Sticky section header: pin the current section's header to the top
        // of the list while its rows occupy the viewport, painted on top of the
        // freshly-drawn rows. Reuses the adapter's header renderer so it is
        // pixel-identical to a real header.
        StickyHeader s = sticky_header_();
        if (s.show && adapter_)
        {
            const float list_top = bounds_.y + search_header_h();
            tk::Rect    area{bounds_.x, list_top, bounds_.w,
                          std::max(0.0f, bounds_.h - search_header_h())};
            ctx.canvas.push_clip_rect(area);
            tk::Rect sb{bounds_.x, s.world_y, bounds_.w, kRoomListHeaderH};
            adapter_->paint_row(static_cast<std::size_t>(s.header_item), ctx, sb,
                                false, sticky_hovered_);
            ctx.canvas.pop_clip();
        }
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
    // Sticky-header click → toggle the pinned section (press claimed in
    // dispatch_pointer_down). Only fires when released back over the band.
    if (press_sticky_section_ >= 0)
    {
        int sec               = press_sticky_section_;
        press_sticky_section_ = -1;
        if (inside_self)
        {
            tk::Point    world{local.x + bounds_.x, local.y + bounds_.y};
            StickyHeader s = sticky_header_();
            if (s.section == sec && sticky_band_contains_(s, world))
            {
                toggle_section_collapsed_(sec);
            }
        }
        return;
    }
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
