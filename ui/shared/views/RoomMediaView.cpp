#include "RoomMediaView.h"

#include "media_utils.h"

#include "tk/i18n.h"
#include "tk/list_view.h"
#include "tk/theme.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>

namespace tesseract::views
{

namespace
{

bool is_media_row(const MessageRowData& r)
{
    return r.kind == MessageRowData::Kind::Image ||
           r.kind == MessageRowData::Kind::Video;
}

struct MonthKey
{
    std::string key;   // "2026-03" — sortable/comparable, locale-independent
    std::string label; // "March 2026" — localized display text
};

MonthKey compute_month_key(std::uint64_t timestamp_ms)
{
    std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm tm_val{};
#if defined(_WIN32)
    localtime_s(&tm_val, &t);
#else
    localtime_r(&t, &tm_val);
#endif
    constexpr const char* kMonths[] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "%04d-%02d",
                 tm_val.tm_year + 1900, tm_val.tm_mon + 1);
    const std::string month_str = tk::tr(kMonths[tm_val.tm_mon]);
    char label_buf[32];
    std::snprintf(label_buf, sizeof(label_buf), "%s %d", month_str.c_str(),
                 tm_val.tm_year + 1900);
    return {std::string(key_buf), std::string(label_buf)};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Adapter — bridges tk::ListView to RoomMediaView's month-header /
//  media-strip row model.
// ─────────────────────────────────────────────────────────────────────────

class RoomMediaView::Adapter : public tk::ListAdapter
{
public:
    explicit Adapter(RoomMediaView& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.rows_.size();
    }

    float measure_row_height(std::size_t index, tk::LayoutCtx&, float) override
    {
        if (index >= owner_.rows_.size())
            return 0.0f;
        return owner_.rows_[index].kind == MediaGridRow::Kind::MonthHeader
                   ? kMonthHeaderH
                   : (RoomMediaView::kCellSize + RoomMediaView::kCellSpacing);
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool /*selected*/, bool hovered) override
    {
        owner_.paint_grid_row_(index, ctx, bounds, hovered);
    }

    bool is_selectable(std::size_t index) const override
    {
        return index < owner_.rows_.size() &&
               owner_.rows_[index].kind == MediaGridRow::Kind::MediaStrip;
    }

    std::string row_key(std::size_t index) const override
    {
        if (index >= owner_.rows_.size())
            return {};
        const auto& r = owner_.rows_[index];
        return r.kind == MediaGridRow::Kind::MonthHeader
                   ? ("month:" + r.month_key)
                   : ("strip:" + r.month_key + ":" +
                      std::to_string(r.strip_index));
    }

    static constexpr float kMonthHeaderH = 28.0f;

private:
    RoomMediaView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  MediaGridList — a tk::ListView that resolves a click to a (row, column)
//  cell instead of just a row index. tk::ListView::on_row_clicked only
//  reports which row was pressed+released; a MediaStrip row holds several
//  cells, so we need the pointer's x-position too. Overriding on_pointer_up
//  (rather than relying on the base's private pressed_index_) means we track
//  our own press row and accept "release lands on the same row" as the
//  click condition — slightly more permissive than the base class's exact
//  press==release-row check, but sufficient for a thumbnail grid.
// ─────────────────────────────────────────────────────────────────────────

class MediaGridList : public tk::ListView
{
public:
    explicit MediaGridList(RoomMediaView& owner) : owner_(owner)
    {
    }

    bool on_pointer_down(tk::Point local) override
    {
        press_row_ = index_at(local);
        return tk::ListView::on_pointer_down(local);
    }

    void on_pointer_up(tk::Point local, bool inside_self) override
    {
        const int row_idx = inside_self ? index_at(local) : -1;
        if (row_idx >= 0 && row_idx == press_row_)
        {
            const tk::Point world{local.x + bounds().x, local.y + bounds().y};
            owner_.handle_cell_pointer_up_(static_cast<std::size_t>(row_idx),
                                           world);
        }
        press_row_ = -1;
        tk::ListView::on_pointer_up(local, inside_self);
    }

private:
    RoomMediaView& owner_;
    int press_row_ = -1;
};

// ─────────────────────────────────────────────────────────────────────────
//  RoomMediaView
// ─────────────────────────────────────────────────────────────────────────

RoomMediaView::RoomMediaView() : adapter_(std::make_unique<Adapter>(*this))
{
    set_visible(false);

    auto list = std::make_unique<MediaGridList>(*this);
    list->set_adapter(adapter_.get());
    list->set_anchor_content_bottom(true);
    // Without this, ListView::arrange()'s "fill on open" autofill re-fires
    // on_near_top unconditionally on every relayout whenever the grid's
    // content is shorter than the viewport (see list_view.cpp) — which a
    // media-sparse room's gallery, with only a handful of items, never
    // outgrows. Combined with the shell's retry/accumulate pagination loop
    // (ShellBase::on_media_view_load_older_), that fired an unbounded chain
    // of paginate_back_async rounds as fast as each one completed. Mirrors
    // MessageListView's identical fix for the main timeline: only autofill
    // while genuinely empty (the open()/set_media() bootstrap case); further
    // rounds are the user's own scroll via the latched on_near_top path.
    list->set_autofill_only_when_empty(true);
    list->on_near_top = [this]
    {
        if (on_load_older_media)
            on_load_older_media(room_id_);
    };
    list_ = add_child(std::move(list));

    auto close_button = tk::create_widget<tk::Button>(
        this, "\xC3\x97", std::function<void()>{}, tk::Button::Variant::Icon);
    close_btn_ = add_child(std::move(close_button));
    close_btn_->set_on_click([this] { close(); });
}

RoomMediaView::~RoomMediaView() = default;

void RoomMediaView::open(std::string room_id, std::string room_name)
{
    room_id_   = std::move(room_id);
    room_name_ = std::move(room_name);
    title_layout_.reset();
    items_.clear();
    rows_.clear();
    reached_start_ = false;
    is_open_        = true;
    set_visible(true);
    if (list_)
    {
        list_->invalidate_data();
        list_->scroll_to_bottom();
    }
}

void RoomMediaView::close()
{
    if (!is_open_)
        return;
    is_open_ = false;
    set_visible(false);
    // Defense in depth: the arrange-time autofill can still call on_near_top
    // (hence on_load_older_media(room_id_)) after close, since this widget
    // keeps getting arranged while hidden. Clearing room_id_ makes any such
    // stale call harmless even if a future caller reaches on_load_older_media
    // without going through ShellBase's own media_view_room_id_ check.
    room_id_.clear();
    if (on_close)
        on_close();
}

void RoomMediaView::set_media(std::vector<MessageRowData> rows)
{
    items_.clear();
    items_.reserve(rows.size());
    for (auto& r : rows)
    {
        if (is_media_row(r))
            items_.push_back(std::move(r));
    }
    if (list_)
    {
        list_->preserve_top_through([this] { rebuild_rows_(); });
        list_->reset_near_top_latch();
    }
}

void RoomMediaView::prepend_media(std::vector<MessageRowData> rows)
{
    if (rows.empty())
        return;
    std::vector<MessageRowData> filtered;
    filtered.reserve(rows.size());
    for (auto& r : rows)
    {
        if (is_media_row(r))
            filtered.push_back(std::move(r));
    }
    if (filtered.empty())
        return;
    items_.insert(items_.begin(),
                  std::make_move_iterator(filtered.begin()),
                  std::make_move_iterator(filtered.end()));
    if (list_)
    {
        list_->preserve_top_through([this] { rebuild_rows_(); });
        list_->reset_near_top_latch();
    }
}

void RoomMediaView::append_live_media(MessageRowData row)
{
    if (!is_media_row(row))
        return;
    items_.push_back(std::move(row));
    if (list_)
    {
        list_->preserve_top_through([this] { rebuild_rows_(); });
    }
}

bool RoomMediaView::content_fills_viewport() const
{
    return list_ && list_->content_height() >= list_->bounds().h;
}

std::size_t RoomMediaView::estimated_capacity() const
{
    if (!list_ || list_->bounds().h <= 0.0f)
        return 0;
    const int rows = std::max(
        1, static_cast<int>(std::ceil(list_->bounds().h /
                                       (kCellSize + kCellSpacing))));
    return static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols_);
}

void RoomMediaView::set_reached_start(bool reached_start)
{
    reached_start_ = reached_start;
    // Called once per completed pagination round for this gallery,
    // including "dry" rounds that found no media (prepend_media() is only
    // called when a batch actually contains media, so a dry round would
    // otherwise never re-arm the latch). Without this, on_near_top would
    // stay latched after the first firing and never fire again for a
    // follow-up scroll gesture once a round comes back empty.
    if (list_)
        list_->reset_near_top_latch();
}

void RoomMediaView::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void RoomMediaView::rebuild_rows_()
{
    // Sort oldest-first before bucketing so months come out older-to-newer
    // (and items within a month oldest-to-newest) regardless of the order
    // set_media()/prepend_media()/append_live_media() happened to add them
    // in — the bucketing below only detects a month *change* by scanning
    // forward, so it silently produces wrong (or duplicated, if the same
    // month appears twice non-contiguously) grouping unless items_ is
    // already fully sorted. std::stable_sort keeps same-timestamp items
    // (e.g. two images in one batch) in their arrival order.
    std::stable_sort(items_.begin(), items_.end(),
                     [](const MessageRowData& a, const MessageRowData& b)
                     { return a.timestamp_ms < b.timestamp_ms; });

    rows_.clear();
    const int cols = std::max(1, cols_);
    std::string cur_month_key;
    int strip_idx_in_month = 0;
    std::vector<MessageRowData>* cur_strip = nullptr;

    for (const auto& item : items_)
    {
        const MonthKey mk = compute_month_key(item.timestamp_ms);
        if (mk.key != cur_month_key)
        {
            cur_month_key       = mk.key;
            strip_idx_in_month  = 0;
            MediaGridRow hdr;
            hdr.kind        = MediaGridRow::Kind::MonthHeader;
            hdr.month_key   = mk.key;
            hdr.month_label = mk.label;
            rows_.push_back(std::move(hdr));
            cur_strip = nullptr;
        }
        if (!cur_strip ||
            cur_strip->size() >= static_cast<std::size_t>(cols))
        {
            MediaGridRow strip;
            strip.kind        = MediaGridRow::Kind::MediaStrip;
            strip.month_key   = cur_month_key;
            strip.strip_index = strip_idx_in_month++;
            rows_.push_back(std::move(strip));
            cur_strip = &rows_.back().items;
        }
        cur_strip->push_back(item);
    }

    if (list_)
        list_->invalidate_data();
}

void RoomMediaView::handle_cell_pointer_up_(std::size_t row_idx,
                                            tk::Point world)
{
    if (row_idx >= rows_.size() || !list_)
        return;
    const auto& row = rows_[row_idx];
    if (row.kind != MediaGridRow::Kind::MediaStrip || row.items.empty())
        return;
    const tk::Rect rrect = list_->row_world_rect(static_cast<int>(row_idx));
    if (rrect.w <= 0.0f)
        return;
    int col = static_cast<int>(
        (world.x - rrect.x - kPadX) / (kCellSize + kCellSpacing));
    col = std::clamp(col, 0, static_cast<int>(row.items.size()) - 1);
    activate_item_(row.items[static_cast<std::size_t>(col)]);
}

void RoomMediaView::activate_item_(const MessageRowData& item)
{
    if (item.kind == MessageRowData::Kind::Video)
    {
        if (!on_video_clicked)
            return;
        MessageListView::VideoHit hit;
        hit.event_id      = item.event_id;
        hit.source        = item.source;
        hit.thumbnail     = item.thumbnail;
        hit.mime_type     = item.video_mime;
        hit.natural_w     = item.media_w;
        hit.natural_h     = item.media_h;
        hit.duration_ms   = item.duration_ms;
        hit.autoplay      = item.video_autoplay;
        hit.loop          = item.video_loop;
        hit.no_audio      = item.video_no_audio;
        hit.hide_controls = item.video_hide_controls;
        hit.gif           = item.video_gif;
        on_video_clicked(hit);
    }
    else if (item.kind == MessageRowData::Kind::Image)
    {
        if (!on_image_clicked)
            return;
        MessageListView::ImageHit hit;
        hit.event_id  = item.event_id;
        hit.source    = item.source;
        hit.thumbnail = item.thumbnail;
        hit.body      = item.body;
        hit.natural_w = item.media_w;
        hit.natural_h = item.media_h;
        on_image_clicked(hit);
    }
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size RoomMediaView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void RoomMediaView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    if (close_btn_)
    {
        const float cx = bounds.x + bounds.w - kCloseSz - kCloseInset;
        const float cy = bounds.y + (kHeaderH - kCloseSz) * 0.5f;
        close_btn_->arrange(lc, {cx, cy, kCloseSz, kCloseSz});
    }

    const int new_cols = std::max(
        1, static_cast<int>((bounds.w - kPadX * 2.0f) /
                            (kCellSize + kCellSpacing)));
    if (new_cols != cols_)
    {
        cols_ = new_cols;
        rebuild_rows_();
    }

    if (list_)
    {
        const float list_top = bounds.y + kHeaderH;
        const float list_h   = std::max(0.0f, bounds.bottom() - list_top);
        list_->arrange(lc, {bounds.x, list_top, bounds.w, list_h});
    }
}

bool RoomMediaView::on_wheel(tk::Point /*local*/, float /*dx*/, float /*dy*/)
{
    // Unconditionally consume wheel events over the gallery while it's
    // open — list_ already handles its own scrolling via the normal
    // child-dispatch path (checked before this fallback fires); without
    // this, a wheel event over a short/empty/header-only viewport (where
    // list_->on_wheel has nothing to scroll and declines) falls through to
    // whatever is underneath, scrolling the room timeline behind the
    // gallery. Mirrors MessageSearchView::on_wheel.
    return is_open_;
}

// ── paint ─────────────────────────────────────────────────────────────────

void RoomMediaView::paint(tk::PaintCtx& ctx)
{
    if (!is_open_)
        return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(bounds_, pal.bg);

    // Header strip.
    const tk::Rect header_rect{bounds_.x, bounds_.y, bounds_.w, kHeaderH};
    cv.fill_rect(header_rect, pal.chrome_bg);
    cv.fill_rect({bounds_.x, bounds_.y + kHeaderH - 1.0f, bounds_.w, 1.0f},
                 pal.separator);

    if (!title_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = std::max(
            0.0f, bounds_.w - kPadX * 2.0f - kCloseSz - kCloseInset);
        title_layout_ = ctx.factory.build_text(
            tk::trf(tk::tr("Media \xE2\x80\x94 {0}"), {room_name_}), st);
    }
    if (title_layout_)
    {
        const tk::Size sz = title_layout_->measure();
        cv.draw_text(*title_layout_,
                     {bounds_.x + kPadX, bounds_.y + (kHeaderH - sz.h) * 0.5f},
                     pal.text_primary);
    }

    // Body: either the list, or an empty/loading placeholder.
    if (rows_.empty())
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        const std::string msg = reached_start_
                                     ? tk::tr("No media in this room yet")
                                     : tk::tr("Loading media\xE2\x80\xA6");
        auto lo = ctx.factory.build_text(msg, st);
        if (lo)
        {
            const tk::Size sz = lo->measure();
            const tk::Rect body{bounds_.x, bounds_.y + kHeaderH, bounds_.w,
                                std::max(0.0f, bounds_.h - kHeaderH)};
            cv.draw_text(*lo,
                         {body.x + (body.w - sz.w) * 0.5f,
                          body.y + (body.h - sz.h) * 0.5f},
                         pal.text_muted);
        }
    }
    else if (list_ && list_->visible())
    {
        list_->paint(ctx);
    }

    // Close button — painted last so it always reads above the grid.
    if (close_btn_ && close_btn_->visible())
    {
        close_btn_->paint(ctx);
        const tk::Rect cb = close_btn_->bounds();
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        auto glyph = ctx.factory.build_text("\xC3\x97", st); // U+00D7 ×
        if (glyph)
        {
            const tk::Size sz = glyph->measure();
            cv.draw_text(*glyph,
                         {cb.x + (cb.w - sz.w) * 0.5f,
                          cb.y + (cb.h - sz.h) * 0.5f},
                         pal.text_secondary);
        }
    }
}

void RoomMediaView::paint_grid_row_(std::size_t index, tk::PaintCtx& ctx,
                                    tk::Rect bounds, bool hovered)
{
    if (index >= rows_.size())
        return;
    auto& row       = rows_[index];
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    if (row.kind == MediaGridRow::Kind::MonthHeader)
    {
        cv.fill_rect(bounds, pal.bg);
        if (!row.label_layout)
        {
            tk::TextStyle st{};
            st.role   = tk::FontRole::Small;
            st.halign = tk::TextHAlign::Leading;
            row.label_layout = ctx.factory.build_text(row.month_label, st);
        }
        if (row.label_layout)
        {
            cv.draw_text(*row.label_layout,
                         {bounds.x + kPadX,
                          bounds.y + (bounds.h - row.label_layout->measure().h) * 0.5f},
                         pal.text_muted);
        }
        return;
    }

    // MediaStrip — a row-level hover tint (not per-cell; see MediaGridList's
    // doc comment on the simplified click/press model).
    cv.fill_rect(bounds, hovered ? pal.subtle_hover : pal.bg);
    for (std::size_t i = 0; i < row.items.size(); ++i)
    {
        const float cx = bounds.x + kPadX +
                         static_cast<float>(i) * (kCellSize + kCellSpacing);
        const tk::Rect cell{cx, bounds.y, kCellSize, kCellSize};
        paint_cell_(ctx, cell, row.items[i]);
    }
}

void RoomMediaView::paint_cell_(tk::PaintCtx& ctx, tk::Rect cell,
                                const MessageRowData& item)
{
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.push_clip_rect(cell);
    cv.fill_rect(cell, pal.subtle_hover);

    const std::string key =
        item.thumbnail ? item.thumbnail->fetch_token()
                       : (item.source ? item.source->fetch_token()
                                     : std::string{});
    const tk::Image* img =
        (image_provider_ && !key.empty()) ? image_provider_(key) : nullptr;
    if (img && img->width() > 0 && img->height() > 0)
    {
        const tk::Size fitted =
            fit_media(static_cast<float>(img->width()),
                     static_cast<float>(img->height()), cell.w, cell.h);
        const tk::Rect dst{cell.x + (cell.w - fitted.w) * 0.5f,
                           cell.y + (cell.h - fitted.h) * 0.5f, fitted.w,
                           fitted.h};
        cv.draw_image(*img, dst);
    }

    if (item.kind == MessageRowData::Kind::Video)
    {
        if (!video_badge_glyph_)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::Title;
            video_badge_glyph_ = ctx.factory.build_text("\xE2\x96\xB6", st); // ▶
        }
        if (video_badge_glyph_)
        {
            const tk::Size sz = video_badge_glyph_->measure();
            const tk::Rect badge{cell.x + (cell.w - sz.w) * 0.5f - 8.0f,
                                 cell.y + (cell.h - sz.h) * 0.5f - 4.0f,
                                 sz.w + 16.0f, sz.h + 8.0f};
            cv.fill_rounded_rect(badge, 6.0f, tk::Color::rgba(0, 0, 0, 140));
            cv.draw_text(*video_badge_glyph_,
                         {cell.x + (cell.w - sz.w) * 0.5f,
                          cell.y + (cell.h - sz.h) * 0.5f},
                         tk::Color::rgba(255, 255, 255, 255));
        }
    }

    cv.pop_clip();
}

} // namespace tesseract::views
