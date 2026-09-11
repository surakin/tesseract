#pragma once

// UrlPreviewCardDisplay — the URL-preview (OpenGraph-style) card subsystem
// extracted from MessageListView. For Kind::Text / Notice / Emote rows it
// draws one or more preview cards below the row's text and records each
// card's clickable world-space rect for the pointer hit-test.
//
// Two data sources feed the cards, resolved by cards_for(), each with its own
// layout (paint_one_bundled_ / paint_one_legacy_):
//   * MSC4095 bundled previews carried inline on the event
//     (MessageRowData::bundled_previews) — the sender generated these; render
//     them directly, image-on-top sized like an inline timeline image
//     (min(col_w, kMaxInlineImageWidth) x kMaxInlineImageHeight,
//     aspect-fitted) with text below. A bundled entry that carries only a
//     matched_url (no content) is resolved through the PreviewProvider like
//     the legacy path but still uses the bundled layout.
//   * the legacy homeserver `/preview_url` fetch — used only when the event
//     carried no bundled-preview field at all. The shell supplies the metadata
//     through the PreviewProvider, keyed by MessageRowData::first_url.
//     Rendered as a small fixed-height row with an aspect-fitted thumbnail.
//
// MessageListView holds one of these by value. It forwards its public
// set_preview_provider() here; the Adapter calls stack_height()/has_preview()
// from measure and paint_cards() from the body paint; on_pointer_down/up read
// geometry() for the click hit-test; and paint() calls clear_geometry() each
// frame (same lifecycle as the other geom maps).
//
// The pointer-press FSM (press_preview_ / press_preview_url_) stays on
// MessageListView — the established pattern — and the notify→invalidate→
// scroll-anchor→gate glue (notify_url_preview_ready) likewise stays in the
// view, since it needs messages_ / invalidate_data / scroll internals.

#include "tk/canvas.h"

#include <functional>
#include <string>
#include <vector>

namespace tk
{
struct PaintCtx;
class Image;
class CanvasFactory;
struct Size;
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

    // One painted card's clickable region (world coords, rebuilt every paint
    // pass) plus the URL it opens.
    struct PreviewCardHit
    {
        std::string url;
        tk::Rect    rect;
    };

    // --- wiring (forwarded from MessageListView's public API) ---
    void set_provider(PreviewProvider p) { provider_ = std::move(p); }
    void set_image_provider(ImageProvider p) { image_provider_ = std::move(p); }

    bool has_provider() const { return static_cast<bool>(provider_); }

    // Raw provider lookup by URL. Returns nullptr when no provider is set or the
    // URL has no (yet-cached) preview. Used by the room-switch gate keeper.
    const UrlPreviewData* lookup(const std::string& url) const
    {
        return provider_ ? provider_(url) : nullptr;
    }

    // The preview cards to render for a row, in paint order. Empty when the row
    // has no displayable preview. Pointers into MessageRowData::bundled_previews
    // and/or the PreviewProvider cache — valid for the current frame only.
    std::vector<const UrlPreviewData*> cards_for(const MessageRowData& row) const;

    // Shared measure/paint predicate.
    bool has_preview(const MessageRowData& row) const
    {
        return !cards_for(row).empty();
    }

    // Total vertical space the card stack occupies (excludes the gap above it,
    // which the caller adds). 0 when there is no card. `factory` is needed to
    // measure a bundled card's text block (its height is content-dependent);
    // `col_w` is needed because a bundled card's image is capped at
    // min(col_w, kMaxInlineImageWidth), which can also affect its fitted
    // height for a wide/panoramic image. Neither affects the legacy layout,
    // whose per-card height is always the constant kUrlPreviewCardH.
    float stack_height(const MessageRowData& row, tk::CanvasFactory& factory,
                       float col_w) const;

    // --- card paint (Adapter delegates here) ---
    // Draws every card_for(row) card starting at (x, y) within `col_w`,
    // advancing downward, and records each card's world-space rect for the
    // click hit-test.
    void paint_cards(const MessageRowData& row, tk::PaintCtx& ctx, float x,
                     float y, float col_w);

    // --- geometry: written by paint, read by the pointer hit-test ---
    void clear_geometry() { card_geom_.clear(); }
    const std::vector<PreviewCardHit>& geometry() const { return card_geom_; }

private:
    // Legacy homeserver-fetched card: fixed kUrlPreviewCardH-tall row, small
    // aspect-fitted thumbnail on the left, text on the right. Returns the
    // (constant) height painted.
    float paint_one_legacy_(const std::string& target_url,
                            const std::string& fallback_url,
                            const UrlPreviewData& p, tk::PaintCtx& ctx, float x,
                            float y, float col_w);

    // Sender-bundled (MSC4095) card: image on top sized like an inline
    // timeline image (min(col_w, kMaxInlineImageWidth) x kMaxInlineImageHeight,
    // aspect-fitted), text below. Returns the actual height painted, which
    // varies per card.
    float paint_one_bundled_(const std::string& target_url,
                             const std::string& fallback_url,
                             const UrlPreviewData& p, tk::PaintCtx& ctx, float x,
                             float y, float col_w);

    // {max_w, total_height} for a bundled card, shared between stack_height()
    // (measure) and paint_one_bundled_() (paint) so the space reserved always
    // matches what's drawn. `max_w` is min(col_w, kMaxInlineImageWidth).
    tk::Size bundled_card_size_(const UrlPreviewData& p,
                                const std::string& third_line,
                                tk::CanvasFactory& factory, float max_w) const;

    PreviewProvider provider_;
    ImageProvider   image_provider_;
    mutable std::vector<PreviewCardHit> card_geom_;
};

} // namespace tesseract::views
