#include "views/RoomSwitchGateKeeper.h"

#include "tk/canvas.h"
#include "views/MessageListView.h"
#include "views/html_spans.h"

#include <memory>
#include <utility>

namespace tesseract::views
{

std::uint64_t RoomSwitchGateKeeper::begin_room_switch()
{
    // Supersede any prior gate (rapid re-switch / same-room reset). The
    // bumped epoch also neutralises an outstanding timeout closure.
    ++epoch_;
    gate_.reset();

    Gate g;
    g.epoch = epoch_;
    gate_   = std::move(g);

    // Dependencies are collected on the first paint (the visible band needs a
    // measure pass). Arm the timeout fallback now so a slow / offline network
    // can never hold the list invisible forever.
    if (post_delayed_)
    {
        std::weak_ptr<bool> walive = alive_;
        std::uint64_t       ep     = epoch_;
        post_delayed_(
            kTimeoutMs,
            [this, walive, ep]()
            {
                auto live = walive.lock();
                if (!live || !*live)
                {
                    return;
                }
                if (!gate_ || gate_->epoch != ep)
                {
                    return;
                }
                // Mark evaluated so a first paint that hasn't run yet (window
                // occluded / paint delayed past the deadline) won't re-derive
                // and re-arm `pending` in evaluate() — the one-shot timeout
                // would otherwise be lost and the list stay hidden.
                gate_->evaluated = true;
                gate_->pending.clear(); // force reveal next paint
                if (request_repaint_)
                {
                    request_repaint_();
                }
            });
    }
    return epoch_;
}

void RoomSwitchGateKeeper::reset_within_switch()
{
    if (!gate_)
    {
        return; // ordinary backfill on an already-revealed room: stay inactive
    }
    const bool        focused = gate_->focused;
    const std::string fid     = gate_->focus_event_id;
    begin_room_switch(); // bumps epoch, fresh Gate, re-arms the 400ms timeout
    if (focused)
    {
        gate_->focused        = true;
        gate_->focus_event_id = fid;
    }
}

void RoomSwitchGateKeeper::set_focus_event(const std::string& focus_event_id)
{
    if (!gate_ || gate_->evaluated)
    {
        return;
    }
    gate_->focused        = true;
    gate_->focus_event_id = focus_event_id;
}

bool RoomSwitchGateKeeper::dep_satisfied(const MessageRowData& m) const
{
    return pending_keys_for(m).empty();
}

std::vector<std::string>
RoomSwitchGateKeeper::pending_keys_for(const MessageRowData& m) const
{
    using K = MessageRowData::Kind;
    std::vector<std::string> pending;

    // ---- media / URL-preview ----
    // When the event carries intrinsic media dimensions, the measure path
    // reserves the media box from media_w/media_h (see measure_row_height for
    // Image/Sticker/Video), so the row's height is already final — the pixels
    // decode in place without reflow. Don't hold the whole list invisible
    // waiting for that decode; only wait when the height is genuinely unknown.
    const bool media_dims_known =
        (m.kind == K::Image || m.kind == K::Sticker || m.kind == K::Video) &&
        m.media_w > 0 && m.media_h > 0;
    if (!media_dims_known)
    {
        switch (m.kind)
        {
        case K::Image:
        case K::Sticker:
        {
            const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
            const std::string wait_key =
                look ? look->fetch_token() : std::string{};
            if (!wait_key.empty() && image_provider_)
            {
                const tk::Image* im = image_provider_(wait_key);
                if (!im || im->width() <= 0 || im->height() <= 0)
                    pending.push_back(wait_key);
            }
            break;
        }
        case K::Video:
            // Only a server-provided thumbnail is worth waiting for. When the
            // server omits one the row falls back to a client-generated frame
            // (no generator on every platform) — don't stall the whole list
            // on it; the metadata/placeholder height is already stable.
            if (m.thumbnail && image_provider_)
            {
                const tk::Image* im = image_provider_(m.thumbnail->fetch_token());
                if (!im || im->width() <= 0 || im->height() <= 0)
                    pending.push_back(m.thumbnail->fetch_token());
            }
            break;
        case K::Text:
        case K::Notice:
        case K::Unhandled:
        case K::Emote:
            // A pending preview returns nullptr; a failed one is released via
            // on_url_preview_failed_ → notify_url_preview_ready (height stays
            // 0, so no jump) so we don't wait the full timeout on dead links.
            if (!m.first_url.empty() && preview_provider_ &&
                !preview_provider_(m.first_url))
            {
                pending.push_back(m.first_url);
            }
            break;
        default:
            break; // file / voice / redacted / separators: height final
        }
    }

    // ---- avatar ----
    // Only kinds whose paint path actually draws a sender/membership-target
    // avatar are checked. DaySeparator/ReadMarker/TimelineStart/PinnedEvent/
    // CallNotification never draw one. Membership rows show
    // membership_target_avatar_url, never sender_avatar_url.
    std::string avatar_key;
    if (m.kind == K::Membership)
    {
        avatar_key = m.membership_target_avatar_url;
    }
    else if (m.kind != K::DaySeparator && m.kind != K::ReadMarker &&
             m.kind != K::TimelineStart && m.kind != K::PinnedEvent &&
             m.kind != K::CallNotification)
    {
        avatar_key = m.sender_avatar_url;
    }
    if (!avatar_key.empty() && avatar_provider_ && !avatar_provider_(avatar_key))
    {
        pending.push_back(avatar_key);
    }

    // ---- quoted/reply ----
    if (!m.in_reply_to_id.empty() && m.in_reply_to_sender_name.empty())
    {
        // Keyed by the REPLY ROW's own event_id (matching messages_[i]'s
        // identity in evaluate()'s scan), not the quoted event's id — this is
        // a model-field transition, not an mxc/URL fetch.
        pending.push_back(m.event_id);
    }
    if (m.has_reply_image())
    {
        const auto* look = m.in_reply_to_image_source.get();
        const std::string key = look ? look->fetch_token() : std::string{};
        if (!key.empty() && image_provider_ && !image_provider_(key))
        {
            pending.push_back(key);
        }
    }

    // ---- MSC2545 custom-emoji glyphs ----
    // Cheap substring pre-filter avoids parsing every row's HTML — most rows
    // have no inline emoji. Mirrors the same substring-is-sufficient
    // reasoning already used by notify_image_ready's emoticon_match.
    if ((m.kind == K::Text || m.kind == K::Notice || m.kind == K::Emote ||
         m.kind == K::Unhandled) &&
        !m.formatted_body.empty() &&
        m.formatted_body.find("data-mx-emoticon") != std::string::npos)
    {
        for (const auto& sp : html_to_spans(m.formatted_body))
        {
            if (sp.is_image && !sp.image_mxc.empty() && image_provider_ &&
                !image_provider_(sp.image_mxc))
            {
                pending.push_back(sp.image_mxc);
            }
        }
    }

    return pending;
}

void RoomSwitchGateKeeper::evaluate(
    const std::function<void(
        const std::function<void(const MessageRowData&)>&)>& scan)
{
    if (!gate_ || gate_->evaluated)
    {
        return;
    }
    auto& g = *gate_;
    g.pending.clear();

    scan(
        [&](const MessageRowData& m)
        {
            for (auto& key : pending_keys_for(m))
            {
                g.pending.insert(std::move(key));
            }
        });

    g.evaluated = true;
}

void RoomSwitchGateKeeper::notify_loaded(const std::string& key)
{
    if (!gate_)
    {
        return;
    }
    auto& g = *gate_;
    g.pending.erase(key);
    if (g.evaluated && g.pending.empty() && request_repaint_)
    {
        request_repaint_(); // next paint reveals via try_reveal()
    }
}

bool RoomSwitchGateKeeper::try_reveal()
{
    if (!gate_)
    {
        return false;
    }
    const bool        focused = gate_->focused;
    const std::string fid     = gate_->focus_event_id;
    gate_.reset();
    // Heights are already final for this frame (ensure_measured ran before we
    // got here and every gated dependency is resolved). Just re-pin the scroll
    // so the very first visible frame is correct: the bottom case is already
    // handled by stick_to_bottom_ inside ensure_measured; focused mode must
    // recompute against the now-final offsets.
    if (focused && !fid.empty())
    {
        if (scroll_to_event_)
            scroll_to_event_(fid);
    }
    else
    {
        if (scroll_to_bottom_)
            scroll_to_bottom_();
    }
    return true;
}

} // namespace tesseract::views
