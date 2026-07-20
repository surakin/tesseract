#pragma once

// ImagePackEditorView — MSC2545 image pack editor for a specific room.
// Lists every pack in the room at once, each as its own named section with
// its own image grid, rather than editing one pack at a time. Stages every
// edit (pack create/remove, per-pack usage, image add/remove/rename) in
// memory; nothing is persisted until RoomSettingsView's own Accept commits
// it (see build_result()). Like RoomSettingsView's other tab sections
// (RoomGeneralSection etc.), this view has no footer of its own and no
// Client/Host dependency — the host pushes pack/image data in via setters
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

#include "views/ImagePackTileGridBase.h"

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/svg.h"
#include "tk/text_area.h"
#include "tk/text_field.h"
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

// ImagePackImageProvider / StagedPackImage now live in ImagePackTileGridBase.h
// (included above), shared with UserPackEditor.

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

// The full staged snapshot returned by build_result() — not a diff. Each
// entry in `packs` is written as a wholesale replacement of that pack's
// images (see ShellBase::apply_image_pack_changes_), since there's no
// diff to apply incrementally. `removed_state_keys` names existing packs
// the user deleted, by state_key (not the synthetic `pack_id`, which the
// backend has no use for) — a pack that never existed server-side just
// isn't in `packs` at all, so it needs no entry here.
struct ImagePackEditorResult
{
    std::string room_id;
    std::vector<StagedPack> packs;
    std::vector<std::string> removed_state_keys;
};

// The scrollable list of pack sections. Owns no data — reads directly from
// the owner's staged pack list (non-owning pointer) and reports clicks/
// drops back via callbacks. See the file-level comment for why this is a
// bespoke tk::ScrollableBase subclass rather than a tk::GridView one.
// Inherits the shared tile-grid mechanics (layout math, tile/hint paint,
// remove-chip hit-test) from ImagePackTileGridBase — also used by
// UserPackEditor (ui/shared/views/settings/) for the global Settings
// personal-pack tab — and keeps everything section/header-specific
// (multi-pack stacking, pack rename, usage toggle, active-pack selection)
// local to this class.
class ImagePackSectionList : public ImagePackTileGridBase
{
public:
    ImagePackSectionList();

    // Non-owning; must outlive this list (or be cleared before it's
    // freed). Call refresh() after any mutation of *packs.
    void set_packs(const std::vector<StagedPack>* packs);
    void set_active_pack_index(std::optional<std::size_t> idx);
    // Gates remove chips (hidden), the usage toggle, and name/shortcode
    // click-to-edit (all no-ops when false) — header-click-to-select-active
    // stays available regardless, since it doesn't mutate anything.
    void set_can_edit(bool can_edit) { can_edit_ = can_edit; }
    void set_editing(std::optional<std::pair<std::size_t, std::size_t>> pack_and_tile);
    // Which pack's name header (if any) is being renamed — hides that pack's
    // name text (the native overlay covers it) mirroring set_editing's tile
    // shortcode behavior.
    void set_editing_name(std::optional<std::size_t> pack_idx);
    void refresh();

    // Widget-local rect of pack `pack_idx`'s tile `tile_idx`'s shortcode
    // label, for the host's NativeTextField overlay. {} if out of range or
    // scrolled out of the viewport.
    tk::Rect label_rect_at(std::size_t pack_idx, std::size_t tile_idx) const;

    // Widget-local rect of pack `pack_idx`'s name header label, for the
    // host's NativeTextField overlay while renaming. {} if out of range or
    // scrolled out of the viewport. Mirrors label_rect_at's shape.
    tk::Rect name_rect_at(std::size_t pack_idx) const;

    // Which pack's section (header + grid) contains world-space point
    // `world`, if any — used for position-based drop routing. `world` is
    // assumed already known to be within this widget's own bounds().
    std::optional<std::size_t> pack_at(tk::Point world) const;

    // Which pack (if any) currently shows the drag-hover highlight — driven
    // by ImagePackEditorView::on_drag_hover/on_drag_leave. Painted inside
    // paint()'s own clip/scroll handling so it's naturally culled when the
    // section scrolls out of view, mirroring pack_at's targeting.
    void set_drag_hover_pack(std::optional<std::size_t> pack_idx)
    {
        drag_hover_pack_ = pack_idx;
    }
    // Test accessor.
    std::optional<std::size_t> drag_hover_pack() const { return drag_hover_pack_; }

    std::function<void(std::size_t pack_idx)> on_pack_header_clicked;
    std::function<void(std::size_t pack_idx)> on_pack_name_clicked;
    std::function<void(std::size_t pack_idx, tesseract::PackUsage)> on_pack_usage_changed;
    std::function<void(std::size_t pack_idx)> on_pack_remove_requested;
    std::function<void(std::size_t pack_idx, std::size_t tile_idx)> on_tile_remove_requested;
    std::function<void(std::size_t pack_idx, std::size_t tile_idx)> on_tile_shortcode_clicked;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_drag(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

protected:
    float content_height() const override;

private:
    struct SectionLayout
    {
        float top = 0.0f;    // un-scrolled y offset of this section's top
        float height = 0.0f; // header + grid, including trailing gap
        tk::Rect header_rect;
        tk::Rect name_rect;
        tk::Rect usage_rect[3]; // Any / Emoticon / Sticker, left to right
        tk::Rect remove_chip_rect;
        std::vector<tk::Rect> tiles; // 1:1 with that pack's images, plus a
                                     // trailing hint tile — see
                                     // ImagePackTileGridBase::layout_tile_row_
    };

    // Pure layout computation in LOCAL, un-scrolled coordinates (first
    // section starts at y=0). Called from paint()/hit-testing/content_height();
    // cheap enough to recompute each time given how few packs/images a room
    // realistically has.
    std::vector<SectionLayout> compute_layout_(float width) const;

    void paint_header_(tk::PaintCtx&, std::size_t pack_idx, const SectionLayout&,
                       tk::Point origin, bool active, bool hovered_remove) const;

    const std::vector<StagedPack>* packs_ = nullptr;
    std::optional<std::size_t> active_pack_index_;
    bool can_edit_ = false;
    std::optional<std::pair<std::size_t, std::size_t>> editing_;
    std::optional<std::size_t> editing_name_;

    // Hover state, tracked for repaint-only hover affordances (tile remove
    // chips are hover-only; header remove chips are always visible).
    std::optional<std::pair<std::size_t, std::size_t>> hovered_tile_;
    std::optional<std::size_t> hovered_header_remove_;

    // Drag-hover highlight target — see set_drag_hover_pack.
    std::optional<std::size_t> drag_hover_pack_;

    // Lucide "close" (circle-x) icon for the header's remove chip — mutable
    // because paint_header_ is const and IconCache::draw() lazily
    // rasterizes/caches on first use. Kept separate from the inherited
    // remove_icon_ (used for tile remove chips) because the two are drawn at
    // different sizes — see ImagePackTileGridBase's own comment on why a
    // single IconCache isn't shared across distinct draw sizes.
    mutable tk::IconCache header_remove_icon_;

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
protected:
    // host() is nullable: when null, the 3 native-overlay fields below are
    // simply not constructed — lets tests that don't care about them
    // default-construct without a Host.
    ImagePackEditorView();
    TK_WIDGET_FACTORY_FRIEND(ImagePackEditorView)

public:
    ~ImagePackEditorView() override;

    // Resets local state for `room_id` (clears every staged pack, editing
    // state, the new-pack name draft). Does NOT fetch anything — the host
    // pushes data in via the setters below once its own Client calls
    // resolve.
    void open(std::string room_id);
    void close();
    bool is_open() const { return open_; }
    const std::string& room_id() const { return room_id_; }

    // Gates create_btn_ and the native-overlay rects below while
    // RoomSettingsView's shared Accept commit is in flight — mirrors
    // RoomGeneralSection::set_committing's shallow scope (not a full
    // interaction lock over every list click).
    void set_committing(bool committing);

    // Single all-or-nothing gate for the whole tab (Matrix has no finer
    // granularity than "can this user send the room's image-pack state
    // event at all" — mirrors RoomPermissionsSection::set_field_permissions'
    // shape). false disables pack creation, removal, renaming, usage
    // changes, shortcode editing, and paste/drop; header-click-to-select-
    // active still works, since it doesn't mutate anything. Defaults to
    // false until the host calls this (safe-by-default, same as every
    // other tab's permission gate before ShellBase seeds the real value).
    void set_field_permissions(bool can_edit);

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

    // Rect + text plumbing for the fixed "new pack name" row (create-time
    // only). The field itself is self-owned — see new_pack_name_field() —
    // these remain as pure geometry/text queries.
    tk::Rect new_pack_name_field_rect() const;
    void     set_new_pack_name_text(std::string text);
    // Bumped every time Create succeeds (the field's own text is cleared
    // directly in create_pack_() — this counter is otherwise just a test
    // observable now).
    std::uint64_t new_pack_name_reset_generation() const { return new_pack_name_reset_gen_; }
    // Borrowed — owned via add_child(). Null when constructed without a
    // Host.
    tk::TextField* new_pack_name_field() const { return new_pack_name_field_; }

    tk::Rect    shortcode_edit_rect() const; // valid only while a tile is being edited
    std::string shortcode_edit_initial_text() const; // seed text once, on begin_editing_shortcode_
    tk::TextField* shortcode_field() const { return shortcode_field_; }
    void        set_editing_shortcode_text(std::string text);
    void        commit_editing_shortcode(); // called on submit/Enter
    // Discards any edits made since this tile's shortcode field was
    // opened, restoring the value it had at that point (the suggested
    // shortcode for a fresh drop, or the existing one when renaming) —
    // called when focus leaves the field by any means other than Enter
    // (click elsewhere, Escape, switching tabs, etc).
    void        cancel_editing_shortcode();

    // Clicking an existing pack's name header turns it into an editable
    // field (mirrors the shortcode field above, scoped to a pack index
    // instead of a (pack, tile) pair).
    tk::Rect    pack_name_edit_rect() const; // valid only while a pack name is being edited
    std::string pack_name_edit_initial_text() const; // seed text once, on begin_editing_pack_name_
    tk::TextField* pack_name_field() const { return pack_name_field_; }
    void        set_editing_pack_name_text(std::string text);
    void        commit_editing_pack_name(); // called on submit/blur

    // Shadows tk::Widget::set_visible (not virtual — same idiom as
    // tk::TextField's own shadow) so hiding this view also hides all 3
    // fields' native controls. tk::Widget::set_visible does not cascade to
    // children by design.
    void set_visible(bool v);

    void on_theme_changed(const tk::Theme& t) override;

    // Clipboard paste has no position — targets the active pack. No-op if
    // there is no active pack (e.g. the room has no packs yet). Paste
    // never carries a filename, so the new tile's shortcode starts empty
    // (same as before this existed).
    void add_pending_image_to_active(std::vector<std::uint8_t> bytes, std::string mime);
    // Drag-drop has a position (world/surface space, same origin as
    // bounds()/dispatch_pointer_down elsewhere in this codebase). Targets
    // whichever pack's section contains `world`; falls back to the active
    // pack if the point isn't over any pack's section; no-op if neither
    // resolves. `filename` (the dropped file's basename, empty if
    // unavailable) seeds the new tile's shortcode via
    // suggest_pack_shortcode_from_filename, de-duped against that pack's
    // other staged images.
    void add_pending_image_at(tk::Point world, std::vector<std::uint8_t> bytes,
                              std::string mime, std::string filename = {});

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

    // Self-owned, invisible 1x1 clipboard-paste catcher — positioned over
    // list_rect() and focused whenever this view becomes visible, so a
    // plain Cmd+V/Ctrl+V with no field explicitly focused still lands here
    // and routes to add_pending_image_to_active(). Borrowed — owned via
    // add_child(). Null when constructed without a Host.
    tk::TextArea* paste_catcher() const { return paste_catcher_; }

    // True once the user has made any edit since the last open()/
    // set_available_packs() baseline (pack create/remove/rename/usage
    // change, image add/remove/shortcode edit) — a coarse dirty bit, not a
    // real diff (build_result() is a full snapshot, not a diff — see its
    // own doc comment). Lets RoomSettingsView's shared Accept skip
    // building/delivering a result at all when this tab was never touched,
    // mirroring how every other tab only reports fields that changed.
    bool has_changes() const { return dirty_; }

    // The current staged snapshot — not a diff, see ImagePackEditorResult's
    // own doc comment. Called by RoomSettingsView's shared Accept click
    // handler, guarded by has_changes().
    ImagePackEditorResult build_result() const;

    // Fired whenever this view's own layout-affecting state changes (open/
    // close, pack add/remove, begin/end editing a shortcode) so the host
    // can reposition/hide its native overlays — mirrors RoomSettingsView's
    // on_layout_changed idiom.
    std::function<void()> on_layout_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

    // Tree-dispatched drop (see tk::Widget::on_file_drop). `local` is
    // relative to this widget's own origin; converted back to world space
    // and forwarded to add_pending_image_at, which already does the
    // per-pack hit-test/fallback. Returns false (leaving `payload`
    // untouched) when the tab isn't open or there's no pack to target.
    bool on_file_drop(tk::Point local, tk::FileDropPayload& payload) override;

    // Drag-hover feedback — same targeting as on_file_drop (pack under the
    // point, falling back to the active pack), forwarded to
    // list_->set_drag_hover_pack so ImagePackSectionList::paint draws the
    // highlight within its own scroll/clip handling.
    bool on_drag_hover(tk::Point local) override;
    void on_drag_leave() override;

    // Test accessors.
    tk::Button* create_button() const { return create_btn_; }
    ImagePackSectionList* list() const { return list_; }
    const std::vector<StagedPack>& packs() const { return packs_; }
    std::optional<std::size_t> active_pack_index() const { return active_pack_index_; }

private:
    void select_active_pack_(std::size_t idx);
    void remove_pack_(std::size_t idx);
    void create_pack_();
    void begin_editing_shortcode_(std::size_t pack_idx, std::size_t tile_idx);
    void begin_editing_pack_name_(std::size_t pack_idx);
    void remove_tile_(std::size_t pack_idx, std::size_t tile_idx);
    void layout_changed_();
    // Common tail of add_pending_image_to_active/add_pending_image_at once
    // the target pack index is known. `filename` empty means no suggested
    // shortcode (paste).
    void add_pending_image_to_pack_(std::size_t pack_idx,
                                    std::vector<std::uint8_t> bytes,
                                    std::string mime, std::string filename);

    bool open_       = false;
    bool committing_ = false;
    bool dirty_      = false;
    bool can_edit_   = false;

    std::string room_id_;
    std::vector<StagedPack> packs_;
    std::vector<std::string> removed_state_keys_;
    std::optional<std::size_t> active_pack_index_;
    std::uint64_t next_local_id_ = 0;

    std::string new_pack_name_draft_;
    std::uint64_t new_pack_name_reset_gen_ = 0;

    std::optional<std::pair<std::size_t, std::size_t>> editing_; // (pack_idx, tile_idx)
    std::string editing_shortcode_original_; // snapshot for cancel_editing_shortcode
    std::optional<std::size_t> editing_pack_name_;

    ImagePackSectionList* list_       = nullptr;
    tk::Button*           create_btn_ = nullptr;

    // Borrowed — owned via add_child(). Null when constructed without a Host.
    tk::TextField* new_pack_name_field_ = nullptr;
    tk::TextField* shortcode_field_     = nullptr;
    tk::TextField* pack_name_field_     = nullptr;
    tk::TextArea*  paste_catcher_       = nullptr;

    tk::Rect new_pack_name_field_rect_{};

    std::unique_ptr<tk::TextLayout> new_pack_label_layout_;

    static constexpr float kPadX     = 24.0f;
    static constexpr float kPadY     = 16.0f;
    static constexpr float kRowH     = 32.0f;
    static constexpr float kLabelGap = 4.0f;
    static constexpr float kLabelH   = 16.0f;
    static constexpr float kBtnGap   = 8.0f;
};

} // namespace tesseract::views
