#pragma once

// UrlPreviewCardDisplay — the URL-preview (OpenGraph-style) card subsystem
// extracted from MessageListView. For Kind::Text / Notice / Emote rows whose
// body carries a first http(s) URL, the shell supplies preview metadata
// (title / description / image) through a PreviewProvider; this collaborator
// owns that provider, draws the card below the row's text, and records the
// card's clickable world-space rect for the pointer hit-test.
//
// MessageListView holds one of these by value. It forwards its public
// set_preview_provider() here, the Adapter calls paint_card() from the body
// paint and has_preview() from both measure and paint, on_pointer_down/up read
// geom_at()/geometry() for the click hit-test, and paint() calls
// clear_geometry() each frame (same lifecycle as the other geom maps).
//
// The pointer-press FSM (press_preview_ / press_preview_url_) stays on
// MessageListView — the established pattern — and the notify→invalidate→
// scroll-anchor→gate glue (notify_url_preview_ready) likewise stays in the
// view, since it needs messages_ / invalidate_data / scroll internals. The
// view still forwards the provider into this collaborator so paint, measure,
// the room-switch gate, and the pinning key all read one source of truth.

#include "tk/canvas.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace tk
{
struct PaintCtx;
class Image;
} // namespace tk

namespace tesseract::views
{

struct MessageRowData;
struct UrlPreviewData;

class UrlPreviewCardDisplay
{
public:
    using PreviewProvider =
        std::function<const UrlPreviewData*(const std::string& url)>;
    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc_or_url)>;

    // Card geometry, rebuilt every paint pass (world coords, keyed by
    // event_id) so the pointer handlers can locate the card without touching
    // the painter.
    struct PreviewCardHit
    {
        std::string url;
        tk::Rect    rect;
    };

    // --- wiring (forwarded from MessageListView's public API) ---
    void set_provider(PreviewProvider p) { provider_ = std::move(p); }
    void set_image_provider(ImageProvider p) { image_provider_ = std::move(p); }

    bool has_provider() const { return static_cast<bool>(provider_); }

    // Raw provider lookup. Returns nullptr when no provider is set or the URL
    // has no (yet-cached) preview. The provider returning nullptr is itself a
    // signal to the shell to kick off a fetch.
    const UrlPreviewData* lookup(const std::string& url) const
    {
        return provider_ ? provider_(url) : nullptr;
    }

    // The row-has-card predicate shared by measure and paint: true when the
    // row has a first URL and the provider has display-worthy preview content.
    bool has_preview(const MessageRowData& row) const;

    // --- card paint (Adapter delegates here) ---
    // Draws the card at (x, y) within `col_w`, using `p` (already resolved by
    // the caller via lookup/has_preview), and records the card's world-space
    // rect under row.event_id for the click hit-test.
    void paint_card(const MessageRowData& row, const UrlPreviewData& p,
                    tk::PaintCtx& ctx, float x, float y, float col_w);

    // --- geometry: written by paint, read by the pointer hit-test ---
    void clear_geometry() { card_geom_.clear(); }
    const std::unordered_map<std::string, PreviewCardHit>& geometry() const
    {
        return card_geom_;
    }
    const PreviewCardHit* geom_at(const std::string& event_id) const
    {
        auto it = card_geom_.find(event_id);
        return it == card_geom_.end() ? nullptr : &it->second;
    }

private:
    PreviewProvider provider_;
    ImageProvider   image_provider_;
    mutable std::unordered_map<std::string, PreviewCardHit> card_geom_;
};

} // namespace tesseract::views
