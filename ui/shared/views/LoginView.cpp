#include "LoginView.h"

#include <algorithm>

namespace tesseract::views {

namespace {

// Visual constants. Card width is fixed; the surrounding fill_main of
// the outer VBox centres the card vertically + horizontally inside the
// window. Numbers are deliberately concrete so the layout reads at a
// glance; they can be folded into tesseract::visual once the screen
// settles.
constexpr float kCardWidth        = 360.0f;
constexpr float kCardPadding      = 24.0f;
constexpr float kCardSpacing      = 12.0f;
constexpr float kHSFieldHeight    = 36.0f;
constexpr float kButtonHeight     = 36.0f;

} // namespace

LoginView::LoginView() {
    rebuild_tree();
}

void LoginView::rebuild_tree() {
    auto card = std::make_unique<tk::VBox>();
    card->set_padding(tk::Edges::all(kCardPadding))
        .set_spacing(kCardSpacing)
        .set_cross(tk::Cross::Stretch)
        .set_main(tk::Main::Start);

    auto title = std::make_unique<tk::Label>("Sign in to Matrix",
                                              tk::FontRole::Title);
    title->set_halign(tk::TextHAlign::Center);

    auto caption = std::make_unique<tk::Label>(
        "We'll open your browser to complete sign-in.",
        tk::FontRole::Body);
    caption->set_halign(tk::TextHAlign::Center);
    caption->set_wrap(true);

    // Homeserver field placeholder — drawn as a labelled box. The host
    // overlays a native edit control on top of homeserver_field_rect()
    // until tk::TextField + IME passthrough lands.
    auto hs_field = std::make_unique<tk::Label>(homeserver_label_,
                                                  tk::FontRole::Body);
    hs_field->set_halign(tk::TextHAlign::Leading);

    auto sign_in = std::make_unique<tk::Button>("Sign in",
                                                   std::function<void()>{},
                                                   tk::Button::Variant::Primary);
    sign_in->set_min_size({ 0, kButtonHeight });
    sign_in->set_on_click([this] { if (on_sign_in) on_sign_in(); });

    auto cancel = std::make_unique<tk::Button>("Cancel",
                                                  std::function<void()>{},
                                                  tk::Button::Variant::Subtle);
    cancel->set_min_size({ 0, kButtonHeight });
    cancel->set_on_click([this] { if (on_cancel) on_cancel(); });
    cancel->set_visible(false);

    auto status = std::make_unique<tk::Label>("", tk::FontRole::Small);
    status->set_halign(tk::TextHAlign::Center);
    status->set_wrap(true);
    status->set_visible(false);

    title_lbl_    = card->add_child(std::move(title));
    caption_lbl_  = card->add_child(std::move(caption));
    hs_field_lbl_ = card->add_child(std::move(hs_field));
    sign_in_btn_  = card->add_child(std::move(sign_in));
    cancel_btn_   = card->add_child(std::move(cancel));
    status_lbl_   = card->add_child(std::move(status));

    card_ = add_child(std::move(card));
}

void LoginView::set_state(State s) {
    state_ = s;
    if (!sign_in_btn_ || !cancel_btn_) return;
    sign_in_btn_->set_visible(s == State::Form);
    cancel_btn_ ->set_visible(s == State::Waiting);
}

void LoginView::set_status(std::string message,
                            std::optional<tk::Color> colour) {
    if (!status_lbl_) return;
    if (message.empty()) {
        status_lbl_->set_visible(false);
    } else {
        status_lbl_->set_text(std::move(message));
        status_lbl_->set_colour(colour);
        status_lbl_->set_visible(true);
    }
}

void LoginView::set_homeserver_label(std::string url) {
    homeserver_label_ = std::move(url);
    if (hs_field_lbl_) hs_field_lbl_->set_text(homeserver_label_);
}

tk::Size LoginView::measure(tk::LayoutCtx&, tk::Size constraints) {
    // Take everything we're given so we can centre the card inside.
    return constraints;
}

void LoginView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;
    if (!card_) return;

    // Measure the card at its target width to find its natural height.
    tk::Size card_size = card_->measure(ctx, { kCardWidth, bounds.h });
    float    card_w    = std::min(kCardWidth, bounds.w);
    float    card_h    = std::min(card_size.h, bounds.h);
    float    card_x    = bounds.x + (bounds.w - card_w) * 0.5f;
    float    card_y    = bounds.y + (bounds.h - card_h) * 0.5f;
    card_->arrange(ctx, { card_x, card_y, card_w, card_h });

    // Cache the homeserver-field placeholder rect in widget-local coords
    // so the host can overlay a native control on it without re-walking
    // the tree.
    if (hs_field_lbl_) {
        tk::Rect fr = hs_field_lbl_->bounds();
        // Expand to a touch-friendly height regardless of label measure.
        float h = std::max(fr.h, kHSFieldHeight);
        homeserver_field_rect_ = {
            fr.x - bounds.x,
            fr.y - bounds.y - (h - fr.h) * 0.5f,
            fr.w,
            h
        };
    }
}

void LoginView::paint(tk::PaintCtx& ctx) {
    // Background.
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    if (!card_) return;

    // Card surface — soft rounded rect behind the form, with a 1 px
    // separator-tinted border so it reads as a distinct affordance even
    // on chrome_bg backgrounds.
    tk::Rect cb = card_->bounds();
    cb = { cb.x - 0, cb.y - 0, cb.w, cb.h };   // copy; no inset yet
    ctx.canvas.fill_rounded_rect(cb, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(cb, 10.0f, ctx.theme.palette.border, 1.0f);

    // Host overlay rect for the homeserver field — drawn as a subtle
    // input affordance even when no native control is overlaid yet.
    if (hs_field_lbl_) {
        tk::Rect fr{
            homeserver_field_rect_.x + bounds_.x,
            homeserver_field_rect_.y + bounds_.y,
            homeserver_field_rect_.w,
            homeserver_field_rect_.h
        };
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border, 1.0f);
    }

    // Children draw on top.
    card_->paint(ctx);
}

} // namespace tesseract::views
