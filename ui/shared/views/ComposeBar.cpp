#include "ComposeBar.h"

#include "tk/theme.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <utility>

namespace tesseract::views {

namespace {

constexpr float kPadX        = 8.0f;
constexpr float kPadY        = 8.0f;
constexpr float kButtonSide  = 40.0f;
constexpr float kSendWidth   = 64.0f;
constexpr float kGap         = 6.0f;
constexpr float kRemoveBtnSide = 24.0f;
constexpr float kRemoveBtnInset = 4.0f;

// Compose-bar background is a tint of the surface — sits below the
// message list and above the bottom edge. Border is a 1px hairline on
// the top edge separating from the timeline.
inline tk::Color bar_bg(const tk::Theme& t)  { return t.palette.chrome_bg; }
inline tk::Color card_bg(const tk::Theme& t) { return t.palette.compose_card_bg; }

} // namespace

std::string ComposeBar::make_filename(const std::string& mime) {
    using namespace std::chrono;
    auto now  = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "clipboard-%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string ext = "bin";
    if      (mime == "image/png")  ext = "png";
    else if (mime == "image/jpeg") ext = "jpg";
    else if (mime == "image/webp") ext = "webp";
    else if (mime == "image/bmp")  ext = "bmp";
    else if (mime == "image/gif")  ext = "gif";
    return std::string(buf) + "." + ext;
}

ComposeBar::ComposeBar() {
    auto emoji = std::make_unique<tk::Button>(
        // U+1F600 GRINNING FACE
        std::string("\xF0\x9F\x98\x80"),
        std::function<void()>{},
        tk::Button::Variant::Subtle);
    emoji->set_on_click([this] { if (on_emoji) on_emoji(); });
    emoji->set_min_size({ kButtonSide, kButtonSide });
    emoji_btn_ = add_child(std::move(emoji));

    auto send = std::make_unique<tk::Button>(
        "Send",
        std::function<void()>{},
        tk::Button::Variant::Primary);
    send->set_on_click([this] {
        if (pending_.has_value()) {
            // Image send: hand off the raw bytes; integration re-encodes
            // per the user's image-quality setting.
            if (on_send_image) {
                on_send_image(std::move(pending_->bytes),
                              std::move(pending_->mime),
                              std::move(pending_->filename),
                              current_text_,
                              pending_->width,
                              pending_->height);
            }
            pending_.reset();
            recompute_height();
            if (remove_btn_) remove_btn_->set_visible(false);
            refresh_send_enabled();
            if (on_size_changed) on_size_changed();
        } else {
            if (on_send) on_send(current_text_);
        }
    });
    send->set_min_size({ kSendWidth, kButtonSide });
    send_btn_ = add_child(std::move(send));

    auto remove = std::make_unique<tk::Button>(
        // ✕ small (U+2715)
        std::string("\xE2\x9C\x95"),
        std::function<void()>{},
        tk::Button::Variant::Icon);
    remove->set_on_click([this] { clear_pending_image(); });
    remove->set_min_size({ kRemoveBtnSide, kRemoveBtnSide });
    remove->set_visible(false);
    remove_btn_ = add_child(std::move(remove));

    refresh_send_enabled();
}

void ComposeBar::set_text_area_natural_height(float h) {
    text_area_natural_ = h;
    recompute_height();
}

void ComposeBar::recompute_height() {
    float text_h = std::clamp(text_area_natural_ + kPadY * 2,
                               kMinHeight, kMaxHeight);
    float extra = pending_.has_value()
        ? (kPreviewBandH + kPreviewBandGap)
        : 0.0f;
    natural_height_ = text_h + extra;
}

void ComposeBar::set_current_text(std::string text) {
    current_text_ = std::move(text);
    refresh_send_enabled();
}

void ComposeBar::set_enabled(bool e) {
    if (enabled_ == e) return;
    enabled_ = e;
    if (emoji_btn_)  emoji_btn_->set_enabled(e);
    if (remove_btn_) remove_btn_->set_enabled(e);
    refresh_send_enabled();
}

void ComposeBar::set_pending_image(std::vector<std::uint8_t> bytes,
                                    std::string mime) {
    PendingImage pi;
    pi.bytes    = std::move(bytes);
    pi.mime     = std::move(mime);
    pi.filename = make_filename(pi.mime);
    pending_    = std::move(pi);
    // Dimensions + preview are filled in lazily on the next arrange()
    // pass (we don't have a CanvasFactory here).
    recompute_height();
    if (remove_btn_) remove_btn_->set_visible(true);
    refresh_send_enabled();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::clear_pending_image() {
    if (!pending_.has_value()) return;
    pending_.reset();
    recompute_height();
    if (remove_btn_) remove_btn_->set_visible(false);
    refresh_send_enabled();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::refresh_send_enabled() {
    if (!send_btn_) return;
    bool any_text = false;
    for (char c : current_text_) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            any_text = true; break;
        }
    }
    send_btn_->set_enabled(enabled_ && (any_text || pending_.has_value()));
}

tk::Size ComposeBar::measure(tk::LayoutCtx&, tk::Size constraints) {
    return { constraints.w, natural_height_ };
}

void ComposeBar::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    // ── Decode the pending image lazily (now that we have a factory) ──
    if (pending_.has_value() && !pending_->preview) {
        auto img = ctx.factory.decode_image(
            std::span<const std::uint8_t>(pending_->bytes.data(),
                                            pending_->bytes.size()));
        if (img) {
            pending_->width  = static_cast<std::uint32_t>(img->width());
            pending_->height = static_cast<std::uint32_t>(img->height());
            pending_->preview = std::move(img);
        }
    }

    // ── Preview band on top (only when an image is attached) ──────────
    float text_top = bounds.y;
    if (pending_.has_value()) {
        preview_band_rect_ = {
            bounds.x + kPadX,
            bounds.y + kPadY,
            std::max(0.0f, bounds.w - kPadX * 2),
            kPreviewBandH
        };
        // Thumbnail letterboxed inside the band, preserving aspect.
        float img_w = static_cast<float>(pending_->width);
        float img_h = static_cast<float>(pending_->height);
        if (img_w <= 0 || img_h <= 0) {
            preview_image_rect_ = preview_band_rect_;
        } else {
            float s = std::min(preview_band_rect_.w / img_w,
                                preview_band_rect_.h / img_h);
            float dw = img_w * s;
            float dh = img_h * s;
            preview_image_rect_ = {
                preview_band_rect_.x,                  // left-aligned in the band
                preview_band_rect_.y + (preview_band_rect_.h - dh) * 0.5f,
                dw, dh
            };
        }
        // Remove button overlaid top-right of the thumbnail.
        remove_btn_rect_ = {
            preview_image_rect_.x + preview_image_rect_.w
                - kRemoveBtnSide - kRemoveBtnInset,
            preview_image_rect_.y + kRemoveBtnInset,
            kRemoveBtnSide, kRemoveBtnSide
        };
        text_top = preview_band_rect_.y + preview_band_rect_.h
                    + kPreviewBandGap;
    } else {
        preview_band_rect_ = {};
        preview_image_rect_ = {};
        remove_btn_rect_ = {};
    }

    float text_strip_h = bounds.y + bounds.h - text_top;

    emoji_rect_ = {
        bounds.x + kPadX,
        text_top + (text_strip_h - kButtonSide) * 0.5f,
        kButtonSide,
        kButtonSide
    };

    send_rect_ = {
        bounds.x + bounds.w - kPadX - kSendWidth,
        text_top + (text_strip_h - kButtonSide) * 0.5f,
        kSendWidth,
        kButtonSide
    };

    float left  = emoji_rect_.x + emoji_rect_.w + kGap;
    float right = send_rect_.x - kGap;
    text_area_rect_ = {
        left,
        text_top + kPadY,
        std::max(0.0f, right - left),
        std::max(0.0f, text_strip_h - kPadY * 2)
    };

    if (emoji_btn_) emoji_btn_->arrange(ctx, emoji_rect_);
    if (send_btn_)  send_btn_->arrange(ctx, send_rect_);
    if (remove_btn_ && pending_.has_value())
        remove_btn_->arrange(ctx, remove_btn_rect_);
}

void ComposeBar::paint(tk::PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, bar_bg(ctx.theme));
    // 1 px top hairline so the bar reads as a separate strip from the
    // message list above it.
    tk::Rect hairline{
        bounds_.x, bounds_.y, bounds_.w, 1.0f
    };
    ctx.canvas.fill_rect(hairline, ctx.theme.palette.border);

    if (pending_.has_value()) {
        // Subtle card behind the preview band.
        ctx.canvas.fill_rounded_rect(preview_band_rect_, 8.0f,
                                       card_bg(ctx.theme));
        if (pending_->preview) {
            ctx.canvas.draw_image(*pending_->preview, preview_image_rect_);
        }
    }

    // Outline the text-area rect so its placement is visible even before
    // the host overlay has rendered (avoids a "missing input" feeling on
    // the first frame).
    if (!text_area_rect_.empty()) {
        ctx.canvas.fill_rounded_rect(text_area_rect_, 6.0f,
                                       card_bg(ctx.theme));
        ctx.canvas.stroke_rounded_rect(text_area_rect_, 6.0f,
                                         ctx.theme.palette.border, 1.0f);
    }

    if (emoji_btn_) emoji_btn_->paint(ctx);
    if (send_btn_)  send_btn_->paint(ctx);
    if (remove_btn_ && pending_.has_value()) remove_btn_->paint(ctx);
}

} // namespace tesseract::views
