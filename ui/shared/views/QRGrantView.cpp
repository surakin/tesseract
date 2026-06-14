#include "QRGrantView.h"

#include "tk/i18n.h"

#include <tesseract/client.h>

namespace tesseract::views
{

namespace
{

constexpr float kCardWidth   = 360.0f;
constexpr float kCardPadding = 24.0f;
constexpr float kCardSpacing = 12.0f;
constexpr float kQrSize      = 240.0f;
constexpr float kButtonH     = 36.0f;
constexpr float kFieldH      = 36.0f;

} // namespace

QRGrantView::QRGrantView()
{
    rebuild_tree_();
}

void QRGrantView::rebuild_tree_()
{
    auto card = std::make_unique<tk::VBox>();
    card->set_padding(tk::Edges::all(kCardPadding))
        .set_spacing(kCardSpacing)
        .set_cross(tk::Cross::Stretch)
        .set_main(tk::Main::Start);

    auto status = std::make_unique<tk::Label>("", tk::FontRole::Body);
    status->set_halign(tk::TextHAlign::Center);
    status->set_wrap(true);

    auto qr = std::make_unique<tk::ImageView>(nullptr, tk::ImageView::ContentMode::Contain);
    qr->set_size({kQrSize, kQrSize});
    qr->set_visible(false);

    // Layout spacer for the NativeTextField overlay — drives check_code_rect_.
    auto check_input = std::make_unique<tk::Label>("", tk::FontRole::Body);
    check_input->set_min_size({0.0f, kFieldH});
    check_input->set_visible(false);

    auto confirm = std::make_unique<tk::Button>(
        tk::tr("Confirm"), std::function<void()>{}, tk::Button::Variant::Primary);
    confirm->set_min_size({0.0f, kButtonH});
    confirm->set_on_click([this] {
        try {
            int v = std::stoi(check_code_text_);
            if (client_ && v >= 0 && v <= 255)
                client_->submit_qr_check_code(static_cast<uint8_t>(v));
        } catch (...) {}
        // Prevent double-submit while the worker is still in await_qr_auth().
        confirm_btn_->set_enabled(false);
    });
    confirm->set_visible(false);

    auto cancel = std::make_unique<tk::Button>(
        tk::tr("Cancel"), std::function<void()>{}, tk::Button::Variant::Subtle);
    cancel->set_min_size({0.0f, kButtonH});
    cancel->set_on_click([this] {
        cancelled_.store(true);
        if (client_) client_->cancel_qr_grant();
        if (on_cancel_) on_cancel_();
    });
    cancel->set_visible(false);

    auto close = std::make_unique<tk::Button>(
        tk::tr("Close"), std::function<void()>{}, tk::Button::Variant::Primary);
    close->set_min_size({0.0f, kButtonH});
    close->set_on_click([this] { if (on_done_) on_done_(); });
    close->set_visible(false);

    auto retry = std::make_unique<tk::Button>(
        tk::tr("Try again"), std::function<void()>{}, tk::Button::Variant::Primary);
    retry->set_min_size({0.0f, kButtonH});
    retry->set_on_click([this] { start(); });
    retry->set_visible(false);

    status_lbl_      = card->add_child(std::move(status));
    qr_view_         = card->add_child(std::move(qr));
    check_input_lbl_ = card->add_child(std::move(check_input));
    confirm_btn_     = card->add_child(std::move(confirm));
    cancel_btn_      = card->add_child(std::move(cancel));
    close_btn_       = card->add_child(std::move(close));
    retry_btn_       = card->add_child(std::move(retry));

    card_ = add_child(std::move(card));
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void QRGrantView::set_state_(State s)
{
    state_ = s;
    if (!card_) return;

    // Hide everything, then show per state.
    qr_view_->set_visible(false);
    check_input_lbl_->set_visible(false);
    confirm_btn_->set_visible(false);
    cancel_btn_->set_visible(false);
    close_btn_->set_visible(false);
    retry_btn_->set_visible(false);
    status_lbl_->set_colour({});

    switch (s)
    {
    case State::Loading:
        status_lbl_->set_text(tk::tr("Setting up secure channel\xe2\x80\xa6"));
        break;

    case State::ShowQR:
        status_lbl_->set_text(tk::tr("Scan this QR code with your Matrix app"));
        qr_view_->set_visible(true);
        cancel_btn_->set_label(tk::tr("Cancel"));
        cancel_btn_->set_visible(true);
        break;

    case State::CheckCode:
        status_lbl_->set_text(
            tk::tr("Enter the numeric code shown on the new device:"));
        check_input_lbl_->set_visible(true);
        confirm_btn_->set_enabled(true);
        confirm_btn_->set_visible(true);
        cancel_btn_->set_label(tk::tr("Cancel"));
        cancel_btn_->set_visible(true);
        break;

    case State::WaitingForAuth:
        status_lbl_->set_text(
            tk::tr("Approve the sign-in in your browser\xe2\x80\xa6"));
        cancel_btn_->set_label(tk::tr("Cancel"));
        cancel_btn_->set_visible(true);
        break;

    case State::Done:
        status_lbl_->set_text(tk::tr("Device added successfully."));
        close_btn_->set_visible(true);
        break;

    case State::Error:
        status_lbl_->set_text(error_msg_);
        status_lbl_->set_colour(tk::Color::rgb(0xB00020));
        retry_btn_->set_visible(true);
        cancel_btn_->set_label(tk::tr("Close"));
        cancel_btn_->set_visible(true);
        break;
    }
}

// ---------------------------------------------------------------------------
// Client wiring
// ---------------------------------------------------------------------------

void QRGrantView::set_client(tesseract::Client* c)
{
    client_ = c;
}

// ---------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------

void QRGrantView::start()
{
    if (!client_) return;
    join_worker_();
    alive_ = std::make_shared<bool>(true);
    cancelled_.store(false);
    qr_pixels_.clear();
    qr_image_.reset();
    set_state_(State::Loading);
    if (relayout_) relayout_();

    auto* c = client_;
    auto run = [this, c] {
        // Step 1: generate QR bitmap
        auto bitmap = c->begin_qr_grant();
        if (cancelled_.load()) return;
        if (!bitmap) {
            post_to_ui_(
                [alive = std::weak_ptr<bool>(alive_), this, msg = bitmap.message] {
                    if (auto p = alive.lock()) {
                        error_msg_ = msg;
                        set_state_(State::Error);
                        if (relayout_) relayout_();
                    }
                });
            return;
        }

        // Step 2: show QR
        post_to_ui_(
            [alive = std::weak_ptr<bool>(alive_), this,
             pixels = bitmap.pixels, side = bitmap.side] {
                if (auto p = alive.lock()) {
                    qr_pixels_    = pixels;
                    qr_side_      = side;
                    qr_image_.reset();   // re-create in next paint
                    last_factory_ = nullptr;
                    set_state_(State::ShowQR);
                    if (relayout_) relayout_();
                }
            });

        // Step 3: wait for phone scan
        auto scanned = c->await_qr_scanned();
        if (cancelled_.load()) return;
        if (!scanned) {
            post_to_ui_(
                [alive = std::weak_ptr<bool>(alive_), this, msg = scanned.message] {
                    if (auto p = alive.lock()) {
                        error_msg_ = msg;
                        set_state_(State::Error);
                        if (relayout_) relayout_();
                    }
                });
            return;
        }
        post_to_ui_(
            [alive = std::weak_ptr<bool>(alive_), this] {
                if (auto p = alive.lock()) {
                    set_state_(State::CheckCode);
                    if (relayout_) relayout_();
                }
            });

        // Step 4: blocks until check code is submitted AND Rust reaches WaitingForAuth
        auto auth = c->await_qr_auth();
        if (cancelled_.load()) return;
        if (!auth) {
            post_to_ui_(
                [alive = std::weak_ptr<bool>(alive_), this, msg = auth.message] {
                    if (auto p = alive.lock()) {
                        error_msg_ = msg;
                        set_state_(State::Error);
                        if (relayout_) relayout_();
                    }
                });
            return;
        }
        post_to_ui_(
            [alive = std::weak_ptr<bool>(alive_), this, uri = auth.verification_uri] {
                if (auto p = alive.lock()) {
                    if (open_browser_) open_browser_(uri);
                    set_state_(State::WaitingForAuth);
                    if (relayout_) relayout_();
                }
            });

        // Step 5: wait for completion
        auto complete = c->await_qr_complete();
        if (cancelled_.load()) return;
        if (!complete) {
            post_to_ui_(
                [alive = std::weak_ptr<bool>(alive_), this, msg = complete.message] {
                    if (auto p = alive.lock()) {
                        error_msg_ = msg;
                        set_state_(State::Error);
                        if (relayout_) relayout_();
                    }
                });
            return;
        }
        post_to_ui_(
            [alive = std::weak_ptr<bool>(alive_), this] {
                if (auto p = alive.lock()) {
                    set_state_(State::Done);
                    if (relayout_) relayout_();
                }
            });
    };

    if (run_async_)
        run_async_(std::move(run));
    else
        worker_ = std::thread(std::move(run));
}

void QRGrantView::shutdown()
{
    cancelled_.store(true);
    if (client_) client_->cancel_qr_grant();
    join_worker_();
}

void QRGrantView::join_worker_()
{
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------------------
// NativeTextField overlay
// ---------------------------------------------------------------------------

tk::Rect QRGrantView::check_code_field_rect() const
{
    return check_code_rect_;
}

bool QRGrantView::check_code_field_visible() const
{
    return state_ == State::CheckCode;
}

// ---------------------------------------------------------------------------
// Widget tree
// ---------------------------------------------------------------------------

tk::Size QRGrantView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void QRGrantView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!card_) return;

    tk::Size card_size = card_->measure(ctx, {kCardWidth, bounds.h});
    float    card_w    = std::min(kCardWidth, bounds.w);
    float    card_h    = std::min(card_size.h, bounds.h);
    float    card_x    = bounds.x + (bounds.w - card_w) * 0.5f;
    float    card_y    = bounds.y + (bounds.h - card_h) * 0.5f;
    card_->arrange(ctx, {card_x, card_y, card_w, card_h});

    if (check_input_lbl_) {
        auto b        = check_input_lbl_->bounds();
        check_code_rect_ = {b.x - bounds_.x, b.y - bounds_.y, b.w, b.h};
    }
}

void QRGrantView::paint(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);
    if (!card_) return;

    // Lazy QR image creation — factory is not available until paint.
    if (!qr_pixels_.empty() &&
        (qr_image_ == nullptr || last_factory_ != &ctx.factory)) {
        last_factory_ = &ctx.factory;
        qr_image_ = ctx.factory.create_image_rgba(
            qr_pixels_.data(),
            static_cast<int>(qr_side_),
            static_cast<int>(qr_side_));
        if (qr_view_) qr_view_->set_image(qr_image_.get());
    }

    tk::Rect cb = card_->bounds();
    ctx.canvas.fill_rounded_rect(cb, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(cb, 10.0f, ctx.theme.palette.border, 1.0f);

    card_->paint(ctx);
}

} // namespace tesseract::views
