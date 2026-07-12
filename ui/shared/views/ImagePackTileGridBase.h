#pragma once

// ImagePackTileGridBase — shared tile-grid mechanics for MSC2545 image-pack
// editors. Factored out of UserPackEditor (ui/shared/views/settings/) and
// ImagePackSectionList (ImagePackEditorView.cpp, used by RoomSettingsView's
// per-room tab): both widgets render a grid of sticker/emoji tiles (plus a
// trailing "drop or paste" hint tile) with identical layout math, tile paint
// routine, and remove-chip hit-testing. What differs between them — whether
// there's one grid or several stacked per-pack sections with headers, and how
// StagedPackImage entries are owned/indexed — stays in each subclass; this
// base only owns the parts that were byte-for-byte duplicated.

#include "tk/canvas.h"
#include "tk/scrollable_base.h"
#include "tk/svg.h"

#include <tesseract/image_pack.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

// Host-supplied thumbnail lookup, keyed by mxc:// url. Same shape as
// AvatarEditControl::ImageProvider / TabbedGridPicker's provider. Shared here
// (rather than in ImagePackEditorView.h, its original home) so both
// ImagePackEditorView.h and UserPackEditor.h can depend on this header
// without a cycle.
using ImagePackImageProvider =
    std::function<const tk::Image*(const std::string& mxc)>;

// One staged sticker/emoji tile — used both by ImagePackEditorView's
// per-room, multi-pack editor (each pack owns a vector<StagedPackImage>) and
// by UserPackEditor's flat single-pack grid.
struct StagedPackImage
{
    // Stable id assigned by add_pending_image_at()/add_pending_image_to_active()
    // (ImagePackEditorView) or add_pasted_image()/add_dropped_image()
    // (UserPackEditor) for a newly-added (not yet uploaded) image, used to
    // correlate the host's async local-decode result (set_tile_preview) back
    // to this entry even if the image list has since been reordered or had
    // entries removed. 0 for images loaded from an existing pack — those are
    // identified by existing_url instead.
    std::uint64_t local_id = 0;

    std::string shortcode; // editable, click-to-edit
    std::string existing_url; // mxc:// — empty if newly added, not yet uploaded
    std::vector<std::uint8_t> pending_bytes; // raw bytes if newly added
    std::string pending_mime;
    std::string body;
    std::string info_json;
    tesseract::PackUsage usage = tesseract::PackUsage::Any; // per-image override
    bool favorite = false;
    // Decoded off-thread by the host for pending_bytes tiles (existing_url
    // tiles are painted via the host's ImagePackImageProvider instead).
    std::shared_ptr<tk::Image> local_preview;
};

// Suggest a shortcode from a dropped file's name — strips the extension
// and sanitizes to lowercase alphanumerics with spaces/dashes/underscores
// folded to a single underscore (mirrors the Rust-side
// `suggest_shortcode`'s character-filtering, kept independent here since
// this runs synchronously in the widget, before any Client round trip).
// Falls back to "sticker" when nothing usable remains (e.g. an
// extension-only or symbol-only name). Does not de-duplicate against
// sibling tiles — callers do that themselves against whatever siblings
// they're staging into (ImagePackEditorView: one pack's images;
// UserPackEditor: the whole flat list).
std::string suggest_pack_shortcode_from_filename(const std::string& filename);

// De-dupes `base` (e.g. from suggest_pack_shortcode_from_filename) against
// every already-staged sibling's shortcode in `siblings`, appending a
// numeric suffix on collision (mirrors the Rust-side `suggest_shortcode`'s
// loop). Returns `base` unchanged when it doesn't collide.
std::string dedupe_pack_shortcode(const std::vector<StagedPackImage>& siblings,
                                  const std::string& base);

class ImagePackTileGridBase : public tk::ScrollableBase
{
public:
    void set_image_provider(ImagePackImageProvider p)
    {
        image_provider_ = std::move(p);
    }

protected:
    // Row-major tile placement for `tile_count` cells (staged images plus one
    // trailing hint tile) in LOCAL, un-scrolled coordinates, starting at
    // y = `y_start` (0 for a flat single-grid widget; the offset right after
    // a pack's header for a stacked multi-section widget). Pure math — no
    // side effects, cheap enough to recompute on every paint/hit-test given
    // how few images a pack realistically has.
    std::vector<tk::Rect> layout_tile_row_(float width, std::size_t tile_count,
                                           float y_start) const;

    // Shared tile paint: rounded background, thumbnail (img.local_preview or
    // image_provider_(img.existing_url)), shortcode label (unless
    // `is_editing`, in which case the host's NativeTextField overlay covers
    // it), remove chip (only when `hovered_remove`).
    void paint_tile_shared_(tk::PaintCtx&, const StagedPackImage& img,
                            const tk::Rect& cell_local, tk::Point origin,
                            bool hovered_remove, bool is_editing) const;

    // Shared "Drop image" placeholder tile, painted for the trailing
    // slot after every real image tile.
    void paint_hint_tile_shared_(tk::PaintCtx&, const tk::Rect& cell_local,
                                 tk::Point origin) const;

    // Circular remove-chip hit-test at `image_rect`'s top-right corner.
    // `local`/`y_content` are the same (widget-local x, content-space y)
    // coordinates each subclass's on_pointer_down already computes.
    bool hit_remove_chip_(tk::Point local, float y_content,
                         const tk::Rect& image_rect) const;

    ImagePackImageProvider image_provider_;

    // Lucide "close" (circle-x) icon for the tile-level remove chip —
    // mutable because paint_tile_shared_ is const and IconCache::draw()
    // lazily rasterizes/caches on first use.
    mutable tk::IconCache remove_icon_;

    static constexpr float kImageH      = 76.0f; // thumbnail square side
    static constexpr float kLabelH      = 20.0f; // shortcode strip below it
    static constexpr float kTileSize    = 96.0f; // full cell (image+label)
    static constexpr float kTileSpacing = 8.0f;
    static constexpr float kTilePad     = 8.0f;
    static constexpr float kRemoveChipR = 9.0f; // mirrors AvatarEditControl
};

} // namespace tesseract::views
