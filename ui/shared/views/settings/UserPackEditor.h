#pragma once

// UserPackEditor — editor for the user's single MSC2545-adjacent personal
// image pack (`im.ponies.user_emotes` / `m.image_pack`; a de-facto
// Element/Cinny extension, not part of the merged MSC2545 text, but one this
// app keeps supporting — see ImagePacksSection.h). Reuses the tile-grid
// layout/paint/staging pattern from ImagePackEditorView/ImagePackSectionList
// (StagedPackImage, tile hit-testing, shortcode click-to-edit) — the shared
// tile-grid mechanics (layout math, tile/hint paint, remove-chip hit-test)
// live in ImagePackTileGridBase, this class's base — but drops all of that
// view's multi-pack machinery (header click-to-select-active, create-pack,
// remove-pack, per-pack usage toggle) since there is always exactly one
// personal pack that can neither be created nor removed.
//
// Like ImagePackEditorView, this widget stages every edit in memory
// (add/remove/rename) and reports nothing until the host reads
// build_result() (gated by has_changes()) — ImagePacksSection's "Save"
// button drives that, mirroring RoomSettingsView's Accept button for the
// per-room tab.

#include "views/ImagePackTileGridBase.h"

#include "tk/canvas.h"
#include "tk/widget.h"

#include <tesseract/image_pack.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tesseract::views
{

class UserPackEditor : public ImagePackTileGridBase
{
public:
    UserPackEditor();
    ~UserPackEditor() override;

    // Baseline load — resets dirty_ to false. Called by the host once its
    // Client::list_pack_images("user", ...) call resolves.
    void set_images(std::vector<tesseract::ImagePackImage> images);

    // Pushed by the host once its off-thread decode of a pending image
    // resolves. No-op if local_id no longer matches any staged image.
    void set_tile_preview(std::uint64_t local_id, std::shared_ptr<tk::Image> image);

    // Clipboard paste has no position — appends to the end of the grid.
    // Paste never carries a filename, so the new tile's shortcode starts
    // empty (same as before this existed).
    void add_pasted_image(std::vector<std::uint8_t> bytes, std::string mime);
    // Drag-drop has a position but there is only one grid to land on, so
    // this is equivalent to add_pasted_image — kept as a separate name to
    // mirror ImagePackEditorView's add_pending_image_at call sites in the
    // host and make the "drop vs paste" distinction explicit at call sites.
    // `filename` (empty if unavailable) seeds the new tile's shortcode via
    // suggest_pack_shortcode_from_filename, de-duped against the other
    // staged images.
    void add_dropped_image(tk::Point world, std::vector<std::uint8_t> bytes,
                           std::string mime, std::string filename = {});

    std::function<void(std::uint64_t local_id,
                       const std::vector<std::uint8_t>& bytes,
                       const std::string& mime)>
        on_pending_image_added;

    // NativeTextField overlay rect + text plumbing for the shortcode being
    // edited, mirrors ImagePackEditorView's shortcode_edit_rect/
    // set_editing_shortcode_text/commit_editing_shortcode.
    tk::Rect    shortcode_edit_rect() const;
    std::string shortcode_edit_initial_text() const; // seed text once, per shortcode_edit_reset_generation()
    // Bumped every time a *different* tile starts being edited — see
    // ImagePackEditorView::shortcode_edit_reset_generation()'s doc comment
    // for why a visibility rising edge alone isn't enough.
    std::uint64_t shortcode_edit_reset_generation() const { return shortcode_edit_reset_gen_; }
    void        set_editing_shortcode_text(std::string text);
    void        commit_editing_shortcode(); // called by the host on submit/Enter
    // Discards any edits made since this tile's shortcode field was
    // opened — called by the host when focus leaves the field by any
    // means other than Enter.
    void        cancel_editing_shortcode();

    // True once the user has made any edit since the last set_images()
    // baseline (add/remove/rename) — drives ImagePacksSection's Save button
    // enabled state.
    bool has_changes() const { return dirty_; }

    struct Result
    {
        std::vector<StagedPackImage> images;
        std::vector<std::string> removed_shortcodes;
    };
    // The current staged snapshot (not a diff) plus the shortcodes removed
    // since the last set_images() baseline. Called by
    // SettingsController::save_user_pack_changes when Save is clicked.
    Result build_result() const;

    // Shallow disable while Save is in flight — mirrors
    // ImagePackEditorView::set_committing.
    void set_committing(bool committing);

    // Fired whenever open/close-affecting state changes (begin/end editing a
    // shortcode) so the host can reposition its native overlay — mirrors
    // ImagePackEditorView::on_layout_changed.
    std::function<void()> on_layout_changed;

    // Drop-target scope for the host's drag-drop hit-test.
    tk::Rect list_rect() const { return bounds(); }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_drag(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;

    // Tree-dispatched drop (see tk::Widget::on_file_drop) — forwards to the
    // existing add_dropped_image, which already ignores position (single
    // grid). Always accepts.
    bool on_file_drop(tk::Point local, tk::FileDropPayload& payload) override;

    // Drag-hover feedback — single grid, no position targeting needed
    // (mirrors on_file_drop). Always claims; paint() highlights bounds().
    bool on_drag_hover(tk::Point local) override;
    void on_drag_leave() override;

    // Test accessors.
    const std::vector<StagedPackImage>& images() const { return images_; }
    bool drag_hover() const { return drag_hover_; }

protected:
    float content_height() const override;

private:
    void add_pending_image_(std::vector<std::uint8_t> bytes, std::string mime,
                            std::string filename);
    void begin_editing_shortcode_(std::size_t tile_idx);
    void remove_tile_(std::size_t tile_idx);
    void layout_changed_();

    std::vector<StagedPackImage> images_;
    std::vector<std::string> removed_shortcodes_;
    bool committing_ = false;
    bool dirty_      = false;
    bool drag_hover_ = false; // true while claiming on_drag_hover
    std::uint64_t next_local_id_ = 0;

    std::optional<std::size_t> editing_; // tile index whose shortcode is edited
    std::uint64_t shortcode_edit_reset_gen_ = 0;
    std::string editing_shortcode_original_; // snapshot for cancel_editing_shortcode

    std::optional<std::size_t> hovered_tile_;

    // Fixed viewport height — SettingsPage is a plain (non-scrolling) VBox,
    // so this widget carries its own internal scroll region of a fixed
    // height rather than filling all available constraint height (which
    // FlexBox has no bound for), mirroring how a scrollable child sizes
    // itself inside a stack of otherwise-natural-height rows.
    static constexpr float kViewportH = 320.0f;
};

} // namespace tesseract::views
