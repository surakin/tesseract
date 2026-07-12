#pragma once

// Full-screen "room media" gallery overlay: every image/video in the current
// room, grouped by month (oldest month at the top, newest at the bottom —
// mirrors the chat timeline), with older months paginating in as the user
// scrolls up. Mounted in MainAppWidget::overlay_stack_ alongside
// QuickSwitcher / MessageSearchView; unlike those it is full-bleed rather
// than a centred card, closer in shape to ThreadView (empty header strip +
// full-bleed list + floating close button).
//
// Data flow: this view does NOT own a separate SDK subscription. The shell
// (ShellBase) reuses the room's already-active Timeline subscription, seeds
// this view synchronously from the messages already known to the main
// timeline when it opens, and forwards every subsequent
// prepend/insert/reset batch for the open room here too (filtered to
// Image/Video). Backward pagination is driven by on_near_top → the shell's
// retry/accumulate pagination loop (a raw paginate_back_async batch is
// unfiltered, so most events in a batch are not media).
//
// The grid is a hybrid tk::ListAdapter: some rows are thin month-header
// rows, others are "media strip" rows the adapter paints as N thumbnail
// cells directly (mirrors MessageListView's day-separator rows interleaved
// with content rows). This — rather than tk::GridView — is what gives the
// gallery on_near_top / preserve_top_through / row_key scroll-anchoring for
// free.

#include "MessageListView.h" // MessageRowData, ImageHit, VideoHit

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tk
{
class ListView;
}

namespace tesseract::views
{

class RoomMediaView : public tk::Widget
{
public:
    RoomMediaView();
    ~RoomMediaView() override;

    // Resets all state and shows the overlay for `room_id`. The caller must
    // follow with set_media() to seed the already-known rows synchronously —
    // this view has no subscription of its own to populate them.
    void open(std::string room_id, std::string room_name);
    void close();
    bool is_open() const
    {
        return is_open_;
    }
    const std::string& room_id() const
    {
        return room_id_;
    }

    // Currently-known media count (post-filter). Used by the shell to decide
    // whether to proactively kick off backward pagination right after
    // open()/set_media() — tk::ListView's on_near_top/on_wheel both no-op
    // while the adapter reports count() == 0 (nothing to scroll), so a
    // gallery that opens empty can never self-trigger pagination via user
    // scroll alone; something has to kick the first round unconditionally.
    std::size_t item_count() const
    {
        return items_.size();
    }

    // True once the grid's actual content height reaches its own viewport
    // height — i.e. there is enough media to fill the visible area, mirroring
    // the same fullness check tk::ListView::arrange() uses internally for its
    // "fill on open" autofill.
    bool content_fills_viewport() const;

    // Roughly how many cells would fill the grid's current viewport (cols_ ×
    // however many rows fit list_'s arranged height), i.e. the real "enough
    // to cover a screenful" target — computed from geometry alone, so it's
    // available the instant a round's authoritative media count comes back,
    // without waiting for those rows to actually be rendered. Returns 0 if
    // this widget has never been arranged with a real (nonzero) viewport
    // height yet (e.g. the very first open() this session, before any
    // arrange() pass — see open_room_media_view_'s kickoff comment); callers
    // should fall back to a small fixed floor in that case.
    std::size_t estimated_capacity() const;

    // Replace the full row set (initial seed at open(), or a fresh
    // timeline_reset for the open room). `rows` is unfiltered — Text/File/etc
    // rows are dropped here. Expected oldest-first, matching
    // MessageListView::messages().
    void set_media(std::vector<MessageRowData> rows);
    // Prepend an older batch (a paginate_back_async result). `rows` is
    // unfiltered and oldest-first within the batch, matching
    // on_messages_prepended. No-op if it contains no media.
    void prepend_media(std::vector<MessageRowData> rows);
    // Append one newly-arrived live event (e.g. someone just posted an image
    // while the gallery is open). No-op if `row` is not Image/Video.
    void append_live_media(MessageRowData row);

    // Set by the shell once pagination for the open room is confirmed
    // exhausted. Drives the "No media in this room yet" vs "Loading media…"
    // empty-state text.
    void set_reached_start(bool reached_start);

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc_or_url)>;
    void set_image_provider(ImageProvider p);

    // Fires (at most once per approach to the top) when more history should
    // be requested. The shell's retry/accumulate loop may call
    // prepend_media() several times per firing before the next one arrives.
    std::function<void(std::string room_id)> on_load_older_media;
    std::function<void()> on_close;
    std::function<void(const MessageListView::ImageHit&)> on_image_clicked;
    std::function<void(const MessageListView::VideoHit&)> on_video_clicked;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_wheel(tk::Point local, float dx, float dy) override;

    static constexpr float kHeaderH     = 56.0f;
    static constexpr float kCloseSz     = 32.0f;
    static constexpr float kCloseInset  = 8.0f;
    static constexpr float kCellSize    = 120.0f;
    static constexpr float kCellSpacing = 2.0f;
    static constexpr float kPadX        = 16.0f;

private:
    class Adapter;
    friend class Adapter;
    // Defined at tesseract::views namespace scope in the .cpp (not nested —
    // see friend declaration below); a tk::ListView subclass that resolves a
    // pointer-up to a (row, column) cell instead of just a row index.
    friend class MediaGridList;

    struct MediaGridRow
    {
        enum class Kind
        {
            MonthHeader,
            MediaStrip,
        };
        Kind kind = Kind::MonthHeader;
        std::string month_key;   // "2026-03" — stable across rebuilds
        std::string month_label; // "March 2026", already tk::tr'd
        int strip_index = 0;     // this strip's position within its month
        std::vector<MessageRowData> items; // MediaStrip only
        // Cached header text layout — built once on first paint, not on
        // every paint (paint_row fires every frame while scrolling; text
        // shaping is not cheap enough to redo that often). Cleared
        // implicitly whenever rebuild_rows_() replaces the whole rows_
        // vector with freshly-default-constructed rows.
        std::unique_ptr<tk::TextLayout> label_layout;
    };

    // Re-bucket `items_` into `rows_` (month headers + fixed-width strips of
    // `cols_` cells) and push the new row count to the list. Called whenever
    // items_ changes or cols_ changes (a width-driven relayout).
    void rebuild_rows_();
    void paint_grid_row_(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                         bool hovered);
    void paint_cell_(tk::PaintCtx& ctx, tk::Rect cell,
                     const MessageRowData& item);
    void activate_item_(const MessageRowData& item);
    // Called by MediaGridList when a press/release lands on the same row;
    // resolves the column from `world` and activates that cell.
    void handle_cell_pointer_up_(std::size_t row_idx, tk::Point world);

    bool is_open_ = false;
    std::string room_id_;
    std::string room_name_;
    std::unique_ptr<tk::TextLayout> title_layout_;
    // The video-cell "▶" badge glyph is identical content on every cell —
    // built once and reused, rather than re-shaped per cell per paint.
    std::unique_ptr<tk::TextLayout> video_badge_glyph_;

    // Flat, oldest-first, filtered to Image/Video.
    std::vector<MessageRowData> items_;
    std::vector<MediaGridRow>   rows_;
    bool reached_start_ = false;
    int  cols_          = 1; // recomputed in arrange() from the available width

    ImageProvider image_provider_;

    std::unique_ptr<Adapter> adapter_;
    tk::ListView* list_      = nullptr; // actually a MediaGridList — see above
    tk::Button*   close_btn_ = nullptr;
};

} // namespace tesseract::views
