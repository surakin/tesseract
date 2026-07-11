#pragma once

// ImagePackEditorView — MSC2545 image pack editor for a specific room.
// Lists every pack in the room at once, each as its own named section with
// its own image grid, rather than editing one pack at a time. Stages every
// edit (pack create/remove, per-pack usage, image add/remove/rename) in
// memory; nothing is persisted until Accept fires on_accept with the full
// staged snapshot. Like RoomSettingsView, this view has no Client/Host
// dependency of its own — the host pushes pack/image data in via setters
// (set_available_packs/set_pack_images) and receives raw bytes to decode/
// upload via callbacks (on_pending_image_added), mirroring how
// RoomSettingsView's tabs are fed by ShellBase rather than fetching
// themselves.
//
// One pack is the "active" pack (selected by clicking its header) — the
// target for clipboard-paste (which has no position) and the fallback
// target for a drag-drop that doesn't land on any specific pack's grid.
// A drop that does land on a pack's grid targets that pack directly
// (see add_pending_image_at), independent of which pack is active.
//
// ImagePackSectionList owns the whole scrollable list — one continuous
// document of stacked [header, tile grid] sections, not nested per-pack
// scroll regions. It subclasses tk::ScrollableBase directly (not
// tk::GridView: GridView's own cell-wrap layout is private to itself, so a
// further subclass can't ask "how tall would this pack's grid be
// unclipped" to stack several of them in one scroll) and implements its
// own layout math, reusing only the scrollbar/wheel/clamp machinery
// ScrollableBase provides generically.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/scrollable_base.h"
#include "tk/svg.h"
#include "tk/widget.h"

#include <tesseract/image_pack.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tesseract::views
{

// Host-supplied thumbnail lookup, keyed by mxc:// url. Same shape as
// AvatarEditControl::ImageProvider / TabbedGridPicker's provider.
using ImagePackImageProvider =
    std::function<const tk::Image*(const std::string& mxc)>;

struct StagedPackImage
{
    // Stable id assigned by add_pending_image_at()/add_pending_image_to_active()
    // for a newly-added (not yet uploaded) image, used to correlate the
    // host's async local-decode result (set_tile_preview) back to this
    // entry even if some pack's image list has since been reordered or had
    // entries removed. Globally unique across every pack (one counter on
    // ImagePackEditorView), so no pack id is needed to disambiguate. 0 for
    // images loaded from an existing pack — those are identified by
    // existing_url instead.
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

// One list entry — a pack currently shown in the editor, staged in memory.
struct StagedPack
{
    bool is_new = false;
    std::string pack_id;   // empty if is_new
    std::string state_key; // empty if is_new
    std::string display_name;
    tesseract::PackUsage usage = tesseract::PackUsage::Any;
    std::vector<StagedPackImage> images;
};

// The full staged snapshot handed to on_accept — not a diff. Whoever
// implements the (currently nonexistent) backend create/update/delete call
// can diff `packs` against Client::list_image_packs()/list_pack_images()
// for the room; `removed_pack_ids` names existing packs the user deleted
// (a pack that never existed server-side just isn't in `packs` at all, so
// it needs no entry here).
struct ImagePackEditorResult
{
    std::string room_id;
    std::vector<StagedPack> packs;
    std::vector<std::string> removed_pack_ids;
};

// The scrollable list of pack sections. Owns no data — reads directly from
// the owner's staged pack list (non-owning pointer) and reports clicks/
// drops back via callbacks. See the file-level comment for why this is a
// bespoke tk::ScrollableBase subclass rather than a tk::GridView one.
class ImagePackSectionList : public tk::ScrollableBase
{
public:
    ImagePackSectionList();

    // Non-owning; must outlive this list (or be cleared before it's
    // freed). Call refresh() after any mutation of *packs.
    void set_packs(const std::vector<StagedPack>* packs);
    void set_image_provider(ImagePackImageProvider provider);
    void set_active_pack_index(std::optional<std::size_t> idx);
    void set_editing(std::optional<std::pair<std::size_t, std::size_t>> pack_and_tile);
    void refresh();

    // Widget-local rect of pack `pack_idx`'s tile `tile_idx`'s shortcode
    // label, for the host's NativeTextField overlay. {} if out of range or
    // scrolled out of the viewport.
    tk::Rect label_rect_at(std::size_t pack_idx, std::size_t tile_idx) const;

    // Which pack's section (header + grid) contains world-space point
    // `world`, if any — used for position-based drop routing. `world` is
    // assumed already known to be within this widget's own bounds().
    std::optional<std::size_t> pack_at(tk::Point world) const;

    std::function<void(std::size_t pack_idx)> on_pack_header_clicked;
    std::function<void(std::size_t pack_idx, tesseract::PackUsage)> on_pack_usage_changed;
    std::function<void(std::size_t pack_idx)> on_pack_remove_requested;
    std::function<void(std::size_t pack_idx, std::size_t tile_idx)> on_tile_remove_requested;
    std::function<void(std::size_t pack_idx, std::size_t tile_idx)> on_tile_shortcode_clicked;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_wheel(tk::Point local, float dx, float dy) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_drag(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

protected:
    float content_height() const override;

private:
    struct TileLayout
    {
        tk::Rect image_rect;
    };
    struct SectionLayout
    {
        float top = 0.0f;    // un-scrolled y offset of this section's top
        float height = 0.0f; // header + grid, including trailing gap
        tk::Rect header_rect;
        tk::Rect name_rect;
        tk::Rect usage_rect[3]; // Any / Emoticon / Sticker, left to right
        tk::Rect remove_chip_rect;
        std::vector<TileLayout> tiles; // 1:1 with that pack's images, plus a
                                        // trailing hint tile
    };

    // Pure layout computation in LOCAL, un-scrolled coordinates (first
    // section starts at y=0). Called from paint()/hit-testing/content_height();
    // cheap enough to recompute each time given how few packs/images a room
    // realistically has.
    std::vector<SectionLayout> compute_layout_(float width) const;

    void paint_header_(tk::PaintCtx&, std::size_t pack_idx, const SectionLayout&,
                       tk::Point origin, bool active, bool hovered_remove) const;
    void paint_tile_(tk::PaintCtx&, std::size_t pack_idx, std::size_t tile_idx,
                     const tk::Rect& image_rect, tk::Point origin,
                     bool hovered_remove) const;
    void paint_hint_tile_(tk::PaintCtx&, const tk::Rect& image_rect,
                          tk::Point origin) const;

    const std::vector<StagedPack>* packs_ = nullptr;
    ImagePackImageProvider image_provider_;
    std::optional<std::size_t> active_pack_index_;
    std::optional<std::pair<std::size_t, std::size_t>> editing_;

    // Hover state, tracked for repaint-only hover affordances (tile remove
    // chips are hover-only; header remove chips are always visible).
    std::optional<std::pair<std::size_t, std::size_t>> hovered_tile_;
    std::optional<std::size_t> hovered_header_remove_;

    // Lucide "close" (circle-x) icon for remove chips — mutable because
    // paint_header_/paint_tile_ are const and IconCache::draw() lazily
    // rasterizes/caches on first use. Separate instances per draw size
    // (header vs. tile chips) so alternating sizes across one paint pass
    // doesn't thrash a single cache slot back and forth.
    mutable tk::IconCache header_remove_icon_;
    mutable tk::IconCache tile_remove_icon_;

    static constexpr float kImageH      = 76.0f; // thumbnail square side
    static constexpr float kLabelH      = 20.0f; // shortcode strip below it
    static constexpr float kTileSize    = 96.0f; // full cell (image+label)
    static constexpr float kTileSpacing = 8.0f;
    static constexpr float kTilePad     = 8.0f;
    static constexpr float kRemoveChipR = 9.0f; // mirrors AvatarEditControl

    static constexpr float kHeaderH        = 40.0f;
    static constexpr float kSectionGap     = 16.0f;
    static constexpr float kHeaderPadX     = 12.0f;
    static constexpr float kUsageSegW      = 52.0f;
    static constexpr float kUsageSegH      = 24.0f;
    static constexpr float kUsageSegGap    = 4.0f;
    static constexpr float kHeaderRemoveR  = 10.0f;
    static constexpr float kActiveBarW     = 4.0f;
};

class ImagePackEditorView : public tk::Widget
{
public:
    ImagePackEditorView();
    ~ImagePackEditorView() override;

    // Resets local state for `room_id` (clears every staged pack, editing
    // state, the new-pack name draft). Does NOT fetch anything — the host
    // pushes data in via the setters below once its own Client calls
    // resolve.
    void open(std::string room_id);
    void close();
    bool is_open() const { return open_; }
    const std::string& room_id() const { return room_id_; }

    // Pushed by the host right after open() (and again if the room's pack
    // list changes while open). Populates the section list with every
    // pack, auto-selects the first as active, and fires
    // on_pack_images_needed(pack_id) for EVERY pack (not just one) so the
    // host can fetch and push each pack's images.
    void set_available_packs(std::vector<tesseract::ImagePack> packs);
    std::function<void(std::string pack_id)> on_pack_images_needed;

    // Pushed by the host once its list_pack_images(pack_id, ...) call for
    // one pack resolves. Ignored if no currently-listed pack has this id
    // (it was removed in the meantime).
    void set_pack_images(std::string pack_id,
                         std::vector<tesseract::ImagePackImage> images);

    void set_image_provider(ImagePackImageProvider p);

    // NativeTextField overlay rect + text plumbing for the fixed
    // "new pack name" row (create-time only — existing packs' names are
    // read-only header labels, not renameable from this view).
    tk::Rect new_pack_name_field_rect() const;
    void     set_new_pack_name_text(std::string text);
    // Bumped every time Create succeeds. The field stays visible
    // continuously (unlike a field that shows/hides), so there's no
    // visibility-transition edge for the host to hook a "clear the native
    // control's displayed text" reset off of — it diffs this counter each
    // layout pass instead and calls set_text("") when it changes.
    std::uint64_t new_pack_name_reset_generation() const { return new_pack_name_reset_gen_; }

    tk::Rect shortcode_edit_rect() const; // valid only while a tile is being edited
    void     set_editing_shortcode_text(std::string text);
    void     commit_editing_shortcode(); // called by the host on submit/blur

    // Clipboard paste has no position — targets the active pack. No-op if
    // there is no active pack (e.g. the room has no packs yet).
    void add_pending_image_to_active(std::vector<std::uint8_t> bytes, std::string mime);
    // Drag-drop has a position (world/surface space, same origin as
    // bounds()/dispatch_pointer_down elsewhere in this codebase). Targets
    // whichever pack's section contains `world`; falls back to the active
    // pack if the point isn't over any pack's section; no-op if neither
    // resolves.
    void add_pending_image_at(tk::Point world, std::vector<std::uint8_t> bytes,
                              std::string mime);

    std::function<void(std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)>
        on_pending_image_added;
    // Pushed by the host once its off-thread decode of a pending image
    // resolves. No-op if local_id no longer matches any staged image (it
    // was removed in the meantime).
    void set_tile_preview(std::uint64_t local_id, std::shared_ptr<tk::Image> image);

    // Scope for the host's drop-target hit-test: non-empty exactly when
    // open() has been called and this view is visible, regardless of
    // whether any specific pack is under the point (see
    // add_pending_image_at for the actual per-pack routing).
    tk::Rect list_rect() const;

    std::function<void()> on_cancel;
    std::function<void(ImagePackEditorResult result)> on_accept;
    // Fired whenever this view's own layout-affecting state changes (open/
    // close, pack add/remove, begin/end editing a shortcode) so the host
    // can reposition/hide its native overlays — mirrors RoomSettingsView's
    // on_layout_changed idiom.
    std::function<void()> on_layout_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    // Test accessors.
    tk::Button* create_button() const { return create_btn_; }
    tk::Button* accept_button() const { return accept_btn_; }
    tk::Button* cancel_button() const { return cancel_btn_; }
    ImagePackSectionList* list() const { return list_; }
    const std::vector<StagedPack>& packs() const { return packs_; }
    std::optional<std::size_t> active_pack_index() const { return active_pack_index_; }

private:
    void select_active_pack_(std::size_t idx);
    void remove_pack_(std::size_t idx);
    void create_pack_();
    void begin_editing_shortcode_(std::size_t pack_idx, std::size_t tile_idx);
    void remove_tile_(std::size_t pack_idx, std::size_t tile_idx);
    void refresh_accept_enabled_();
    void layout_changed_();
    // Common tail of add_pending_image_to_active/add_pending_image_at once
    // the target pack index is known.
    void add_pending_image_to_pack_(std::size_t pack_idx,
                                    std::vector<std::uint8_t> bytes,
                                    std::string mime);

    bool open_       = false;
    bool committing_ = false;

    std::string room_id_;
    std::vector<StagedPack> packs_;
    std::vector<std::string> removed_pack_ids_;
    std::optional<std::size_t> active_pack_index_;
    std::uint64_t next_local_id_ = 0;

    std::string new_pack_name_draft_;
    std::uint64_t new_pack_name_reset_gen_ = 0;

    std::optional<std::pair<std::size_t, std::size_t>> editing_; // (pack_idx, tile_idx)

    ImagePackSectionList* list_       = nullptr;
    tk::Button*           create_btn_ = nullptr;
    tk::Button*           accept_btn_ = nullptr;
    tk::Button*           cancel_btn_ = nullptr;

    tk::Rect new_pack_name_field_rect_{};

    std::unique_ptr<tk::TextLayout> new_pack_label_layout_;

    static constexpr float kPadX     = 24.0f;
    static constexpr float kPadY     = 16.0f;
    static constexpr float kRowH     = 32.0f;
    static constexpr float kLabelGap = 4.0f;
    static constexpr float kLabelH   = 16.0f;
    static constexpr float kFooterH  = 64.0f;
    static constexpr float kBtnH     = 36.0f;
    static constexpr float kBtnGap   = 8.0f;
};

} // namespace tesseract::views
