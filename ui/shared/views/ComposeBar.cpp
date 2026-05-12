#include "ComposeBar.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views {

namespace {

constexpr float kPadX        = 8.0f;
constexpr float kPadY        = 8.0f;
constexpr float kButtonSide  = 40.0f;
constexpr float kSendWidth   = 64.0f;
constexpr float kGap         = 6.0f;

// Compose-bar background is a tint of the surface — sits below the
// message list and above the bottom edge. Border is a 1px hairline on
// the top edge separating from the timeline.
inline tk::Color bar_bg(const tk::Theme& t)  { return t.palette.chrome_bg; }
inline tk::Color card_bg(const tk::Theme& t) { return t.palette.compose_card_bg; }

} // namespace

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
        if (on_send) on_send(current_text_);
    });
    send->set_min_size({ kSendWidth, kButtonSide });
    send_btn_ = add_child(std::move(send));

    refresh_send_enabled();
}

void ComposeBar::set_text_area_natural_height(float h) {
    // Text-area natural height + vertical padding for the chrome.
    float total = std::clamp(h + kPadY * 2, kMinHeight, kMaxHeight);
    if (total == natural_height_) return;
    natural_height_ = total;
    // Parent layouts re-measure when the host runs its next relayout —
    // integration code calls surface.relayout() in response to the
    // NativeTextArea's on_height_changed callback.
}

void ComposeBar::set_current_text(std::string text) {
    current_text_ = std::move(text);
    refresh_send_enabled();
}

void ComposeBar::set_enabled(bool e) {
    if (enabled_ == e) return;
    enabled_ = e;
    if (emoji_btn_) emoji_btn_->set_enabled(e);
    refresh_send_enabled();
}

void ComposeBar::refresh_send_enabled() {
    if (!send_btn_) return;
    bool any_text = false;
    for (char c : current_text_) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            any_text = true; break;
        }
    }
    send_btn_->set_enabled(enabled_ && any_text);
}

tk::Size ComposeBar::measure(tk::LayoutCtx&, tk::Size constraints) {
    return { constraints.w, natural_height_ };
}

void ComposeBar::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    emoji_rect_ = {
        bounds.x + kPadX,
        bounds.y + (bounds.h - kButtonSide) * 0.5f,
        kButtonSide,
        kButtonSide
    };

    send_rect_ = {
        bounds.x + bounds.w - kPadX - kSendWidth,
        bounds.y + (bounds.h - kButtonSide) * 0.5f,
        kSendWidth,
        kButtonSide
    };

    float left  = emoji_rect_.x + emoji_rect_.w + kGap;
    float right = send_rect_.x - kGap;
    text_area_rect_ = {
        left,
        bounds.y + kPadY,
        std::max(0.0f, right - left),
        std::max(0.0f, bounds.h - kPadY * 2)
    };

    if (emoji_btn_) emoji_btn_->arrange(ctx, emoji_rect_);
    if (send_btn_)  send_btn_->arrange(ctx, send_rect_);
}

void ComposeBar::paint(tk::PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, bar_bg(ctx.theme));
    // 1 px top hairline so the bar reads as a separate strip from the
    // message list above it.
    tk::Rect hairline{
        bounds_.x, bounds_.y, bounds_.w, 1.0f
    };
    ctx.canvas.fill_rect(hairline, ctx.theme.palette.border);

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
}

} // namespace tesseract::views
