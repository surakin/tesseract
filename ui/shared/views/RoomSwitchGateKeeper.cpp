#include "views/RoomSwitchGateKeeper.h"

#include "tk/canvas.h"
#include "views/MessageListView.h"

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
    using K = MessageRowData::Kind;
    // When the event carries intrinsic media dimensions, the measure path
    // reserves the media box from media_w/media_h (see measure_row_height for
    // Image/Sticker/Video), so the row's height is already final — the pixels
    // decode in place without reflow. Don't hold the whole list invisible
    // waiting for that decode; only wait when the height is genuinely unknown.
    if ((m.kind == K::Image || m.kind == K::Sticker || m.kind == K::Video) &&
        m.media_w > 0 && m.media_h > 0)
    {
        return true;
    }
    switch (m.kind)
    {
    case K::Image:
    case K::Sticker:
    {
        const auto* look = m.thumbnail ? m.thumbnail.get() : m.source.get();
        const std::string wait_key = look ? look->fetch_token() : std::string{};
        if (wait_key.empty() || !image_provider_)
        {
            return true;
        }
        if (const tk::Image* im = image_provider_(wait_key))
        {
            return im->width() > 0 && im->height() > 0;
        }
        return false;
    }
    case K::Video:
        // Only a server-provided thumbnail is worth waiting for. When the
        // server omits one the row falls back to a client-generated frame
        // (no generator on every platform) — don't stall the whole list
        // on it; the metadata/placeholder height is already stable.
        if (!m.thumbnail || !image_provider_)
        {
            return true;
        }
        if (const tk::Image* im = image_provider_(m.thumbnail->fetch_token()))
        {
            return im->width() > 0 && im->height() > 0;
        }
        return false;
    case K::Text:
    case K::Notice:
    case K::Unhandled:
    case K::Emote:
        // A pending preview returns nullptr; a failed one is released via
        // on_url_preview_failed_ → notify_url_preview_ready (height stays
        // 0, so no jump) so we don't wait the full timeout on dead links.
        if (m.first_url.empty() || !preview_provider_)
        {
            return true;
        }
        return preview_provider_(m.first_url) != nullptr;
    default:
        return true; // file / voice / redacted / separators: height final
    }
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
            if (dep_satisfied(m))
            {
                return;
            }
            using K = MessageRowData::Kind;
            if (m.kind == K::Image || m.kind == K::Sticker)
            {
                const auto* look =
                    m.thumbnail ? m.thumbnail.get() : m.source.get();
                if (look)
                    g.pending.insert(look->fetch_token());
            }
            else if (m.kind == K::Video)
            {
                if (m.thumbnail)
                    g.pending.insert(m.thumbnail->fetch_token());
            }
            else if (!m.first_url.empty())
            {
                g.pending.insert(m.first_url);
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
