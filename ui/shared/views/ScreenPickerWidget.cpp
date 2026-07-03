#ifdef TESSERACT_CALLS_ENABLED
#include "ScreenPickerWidget.h"
#include "tk/i18n.h"

#include <algorithm>
#include <cmath>

namespace tesseract::views
{

namespace
{
constexpr float kPad    = 24.0f;
constexpr float kHdrH   = 48.0f;
constexpr float kTileW  = 220.0f;
constexpr float kTileH  = 140.0f;
constexpr float kGap    = 16.0f;
constexpr float kBtnW   = 100.0f;
constexpr float kBtnH   = 36.0f;

constexpr tk::Color kBackdrop  {  0,   0,   0, 180};
constexpr tk::Color kCardBg    { 30,  30,  30, 245};
constexpr tk::Color kCardBd    { 80,  80,  80, 180};
constexpr tk::Color kTileNorm  { 50,  50,  50, 255};
constexpr tk::Color kTileHov   { 60,  80, 120, 255};
constexpr tk::Color kTileBdN   { 70,  70,  70, 255};
constexpr tk::Color kTileBdH   { 90, 130, 200, 255};
constexpr tk::Color kTextTitle {220, 220, 220, 255};
constexpr tk::Color kTextLabel {200, 200, 200, 255};
} // namespace

ScreenPickerWidget::ScreenPickerWidget(std::vector<tk::ScreenSource> sources)
    : sources_(std::move(sources))
{
    auto btn = std::make_unique<tk::Button>(tk::tr("Cancel"),
                                            std::function<void()>{},
                                            tk::Button::Variant::Subtle);
    cancel_btn_ = add_child(std::move(btn));
    // Move the callback to a local before invoking — the callback will call
    // unmount_screen_picker() which destroys `this`, so on_cancelled must not
    // be accessed after the call.
    cancel_btn_->set_on_click([this] {
        auto cb = std::move(on_cancelled);
        if (cb) cb();
    });
    tile_rects_.resize(sources_.size());
    thumbs_.resize(sources_.size());
}

ScreenPickerWidget::~ScreenPickerWidget()
{
    // Liveness token for the background thumbnail worker (see alive_token()):
    // any thumbnail that arrives after this point must be dropped.
    *alive_ = false;
}

void ScreenPickerWidget::set_thumbnail(std::size_t index, std::vector<std::uint8_t> rgba,
                                       std::uint32_t w, std::uint32_t h)
{
    if (index >= thumbs_.size())
        return;
    TileThumb& t = thumbs_[index];
    t.rgba  = std::move(rgba);
    t.w     = w;
    t.h     = h;
    t.image.reset(); // rebuild lazily in paint()
    // Caller (ShellBase) triggers the repaint after this call, mirroring the
    // generic request_relayout_() hook used throughout that file.
}

tk::Size ScreenPickerWidget::measure(tk::LayoutCtx& ctx, tk::Size constraints)
{
    if (cancel_btn_)
        cancel_btn_->measure(ctx, {kBtnW, kBtnH});
    return constraints;
}

void ScreenPickerWidget::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    header_rect_ = tk::Rect{bounds.x + kPad, bounds.y + kPad,
                             bounds.w - kPad * 2.0f, kHdrH};

    const int icols = std::max(1, static_cast<int>(
        std::floor((bounds.w - kPad * 2.0f + kGap) / (kTileW + kGap))));
    const float fcols = static_cast<float>(icols);
    const float grid_w = fcols * (kTileW + kGap) - kGap;
    const float grid_x = bounds.x + (bounds.w - grid_w) / 2.0f;
    const float grid_y = header_rect_.y + kHdrH + kGap;

    grid_rect_ = tk::Rect{grid_x, grid_y, grid_w,
                           bounds.h - (grid_y - bounds.y) - kBtnH - kPad * 2.0f};

    const int nrows = sources_.empty() ? 0
        : (static_cast<int>(sources_.size()) + icols - 1) / icols;
    content_h_ = nrows > 0 ? static_cast<float>(nrows) * (kTileH + kGap) - kGap : 0.0f;
    clamp_scroll();

    for (std::size_t i = 0; i < sources_.size(); ++i)
    {
        const float col = static_cast<float>(static_cast<int>(i) % icols);
        const float row = static_cast<float>(static_cast<int>(i) / icols);
        tile_rects_[i] = tk::Rect{
            grid_x + col * (kTileW + kGap),
            grid_y + row * (kTileH + kGap),
            kTileW, kTileH};
    }

    if (cancel_btn_)
        cancel_btn_->arrange(ctx, tk::Rect{
            bounds.x + bounds.w - kPad - kBtnW,
            bounds.y + bounds.h - kPad - kBtnH,
            kBtnW, kBtnH});
}

void ScreenPickerWidget::paint(tk::PaintCtx& ctx)
{
    const tk::Rect& b = bounds_;

    ctx.canvas.fill_rect(b, kBackdrop);

    const tk::Rect card{b.x + kPad, b.y + kPad,
                        b.w - kPad * 2.0f, b.h - kPad * 2.0f};
    ctx.canvas.fill_rect(card, kCardBg);
    ctx.canvas.stroke_rect(card, kCardBd, 1.0f);

    {
        tk::TextStyle ts{};
        ts.role = tk::FontRole::SenderName;
        auto layout = ctx.factory.build_text(tk::tr("Select what to share"), ts);
        if (layout)
        {
            const tk::Size sz = layout->measure();
            const float ty = header_rect_.y + (header_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*layout, {header_rect_.x, ty}, kTextTitle);
        }
    }

    // Factory changed (e.g. surface recreated) — drop cached images so they
    // get rebuilt against the new factory below, mirroring QRGrantView.
    if (image_factory_ != &ctx.factory)
    {
        image_factory_ = &ctx.factory;
        for (auto& t : thumbs_)
            t.image.reset();
    }

    ctx.canvas.push_clip_rect(grid_rect_);

    for (std::size_t i = 0; i < sources_.size(); ++i)
    {
        // Apply scroll offset to the stored (unscrolled) tile position.
        tk::Rect r = tile_rects_[i];
        r.y -= scroll_y_;

        // Skip tiles entirely outside the viewport.
        if (r.y + r.h <= grid_rect_.y || r.y >= grid_rect_.y + grid_rect_.h)
            continue;

        const bool hov = (static_cast<int>(i) == hovered_idx_);

        ctx.canvas.fill_rect(r, hov ? kTileHov : kTileNorm);

        TileThumb& thumb = thumbs_[i];
        if (thumb.w > 0 && thumb.h > 0)
        {
            if (!thumb.image && !thumb.rgba.empty())
            {
                thumb.image = ctx.factory.create_image_rgba(
                    thumb.rgba.data(), static_cast<int>(thumb.w), static_cast<int>(thumb.h));
            }
            if (thumb.image)
            {
                // Centre-fit preserving aspect ratio, same math as CameraWidget's preview.
                const float img_w = static_cast<float>(thumb.w);
                const float img_h = static_cast<float>(thumb.h);
                const float scale = std::min(r.w / img_w, r.h / img_h);
                const float dw = img_w * scale;
                const float dh = img_h * scale;
                const tk::Rect fit{r.x + (r.w - dw) * 0.5f, r.y + (r.h - dh) * 0.5f, dw, dh};
                ctx.canvas.draw_image(*thumb.image, fit);
            }
        }

        ctx.canvas.stroke_rect(r, hov ? kTileBdH : kTileBdN, 1.5f);

        const std::string& label = sources_[i].is_window
            ? sources_[i].display_name
            : std::string(tk::tr("Screen"));
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Body;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = r.w - 16.0f;
        auto lbl = ctx.factory.build_text(label, ts);
        if (lbl)
        {
            const tk::Size sz = lbl->measure();
            ctx.canvas.draw_text(*lbl, {r.x + 8.0f, r.y + r.h - sz.h - 8.0f}, kTextLabel);
        }
    }

    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);

    if (cancel_btn_ && cancel_btn_->visible())
        cancel_btn_->paint(ctx);
}

bool ScreenPickerWidget::on_pointer_down(tk::Point local)
{
    // Scrollbar thumb wins over any tile beneath it.
    if (scrollbar_on_pointer_down(local))
        return true;

    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    pressed_tile_idx_ = -1;
    for (std::size_t i = 0; i < tile_rects_.size(); ++i)
    {
        const tk::Rect& r  = tile_rects_[i];
        const float     vy = r.y - scroll_y_; // visual (scrolled) top
        // Ignore tiles scrolled outside the grid viewport.
        if (vy + r.h <= grid_rect_.y || vy >= grid_rect_.y + grid_rect_.h)
            continue;
        if (world.x >= r.x && world.x < r.x + r.w &&
            world.y >= vy    && world.y < vy + r.h)
        {
            pressed_tile_idx_ = static_cast<int>(i);
            break;
        }
    }
    // Always claim capture (return true), including clicks outside any tile,
    // so this modal eats every click and nothing falls through to whatever is
    // behind it. The actual selection fires on pointer-up (below) since the
    // host clears pressed_widget_ before that call, making it safe for the
    // callback to destroy this widget (same idiom as Button::on_pointer_up).
    return true;
}

bool ScreenPickerWidget::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    const float prev = scroll_y_;
    scroll_y_ += dy;
    clamp_scroll();
    // Return true regardless of whether scroll changed: this modal always
    // consumes wheel events so they don't fall through to the room view below.
    (void)prev;
    return true;
}

void ScreenPickerWidget::on_pointer_drag(tk::Point local)
{
    scrollbar_on_pointer_drag(local);
    // Host always calls request_repaint() after on_pointer_drag.
}

void ScreenPickerWidget::on_pointer_up(tk::Point /*local*/, bool inside_self)
{
    if (scrollbar_on_pointer_up())
        return;
    const int idx = pressed_tile_idx_;
    pressed_tile_idx_ = -1;
    if (idx < 0 || !inside_self)
        return;
    // Move to a local before calling — unmount_screen_picker() will destroy
    // `this` (and on_source_selected with it) during the call.
    auto cb = std::move(on_source_selected);
    if (cb) cb(sources_[static_cast<std::size_t>(idx)].id);
}

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
