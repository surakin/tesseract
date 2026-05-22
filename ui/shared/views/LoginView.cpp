#include "LoginView.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <tesseract/client.h>

#include "views/text_util.h"

namespace tesseract::views
{

namespace
{

constexpr float kCardWidth    = 360.0f;
constexpr float kCardPadding  = 24.0f;
constexpr float kCardSpacing  = 12.0f;
constexpr float kHSFieldHeight = 36.0f;
constexpr float kButtonHeight  = 36.0f;

} // namespace

LoginView::LoginView()
{
    rebuild_tree();
}

void LoginView::rebuild_tree()
{
    auto card = std::make_unique<tk::VBox>();
    card->set_padding(tk::Edges::all(kCardPadding))
        .set_spacing(kCardSpacing)
        .set_cross(tk::Cross::Stretch)
        .set_main(tk::Main::Start);

    auto title =
        std::make_unique<tk::Label>("Sign in to Matrix", tk::FontRole::Title);
    title->set_halign(tk::TextHAlign::Center);

    auto caption = std::make_unique<tk::Label>(
        "We'll open your browser to complete sign-in.", tk::FontRole::Body);
    caption->set_halign(tk::TextHAlign::Center);
    caption->set_wrap(true);

    auto hs_input_label = std::make_unique<tk::Label>("Homeserver or Matrix ID",
                                                      tk::FontRole::Small);
    hs_input_label->set_halign(tk::TextHAlign::Leading);

    // Homeserver field — layout spacer only. The host overlays a native
    // edit control on top of homeserver_field_rect(); this widget holds
    // the space and drives the rect calculation but must not draw text.
    auto hs_field = std::make_unique<tk::Label>("", tk::FontRole::Body);
    hs_field->set_halign(tk::TextHAlign::Leading);
    hs_field->set_min_size({0.0f, kHSFieldHeight});

    auto discovery = std::make_unique<tk::Label>("", tk::FontRole::Small);
    discovery->set_halign(tk::TextHAlign::Leading);
    discovery->set_visible(false);

    auto sign_in = std::make_unique<tk::Button>(
        "Sign in", std::function<void()>{}, tk::Button::Variant::Primary);
    sign_in->set_min_size({0, kButtonHeight});
    sign_in->set_on_click([this] { sign_in_(); });

    auto cancel = std::make_unique<tk::Button>(
        "Cancel", std::function<void()>{}, tk::Button::Variant::Subtle);
    cancel->set_min_size({0, kButtonHeight});
    cancel->set_on_click([this] { cancel_(); });
    cancel->set_visible(false);

    auto status = std::make_unique<tk::Label>("", tk::FontRole::Small);
    status->set_halign(tk::TextHAlign::Center);
    status->set_wrap(true);
    status->set_visible(false);

    title_lbl_      = card->add_child(std::move(title));
    caption_lbl_    = card->add_child(std::move(caption));
    hs_input_label_ = card->add_child(std::move(hs_input_label));
    hs_field_lbl_   = card->add_child(std::move(hs_field));
    discovery_lbl_  = card->add_child(std::move(discovery));
    sign_in_btn_    = card->add_child(std::move(sign_in));

    auto register_link = std::make_unique<tk::Button>(
        "New here? Create an account", std::function<void()>{},
        tk::Button::Variant::Subtle);
    register_link->set_min_size({0, kButtonHeight});
    register_link->set_on_click([this] { start_oauth_(true); });
    register_link->set_visible(false);
    register_link_ = card->add_child(std::move(register_link));

    cancel_btn_     = card->add_child(std::move(cancel));
    status_lbl_     = card->add_child(std::move(status));

    card_ = add_child(std::move(card));
}

// ---------------------------------------------------------------------------
// Visual state
// ---------------------------------------------------------------------------

void LoginView::set_state(State s)
{
    state_ = s;
    if (!sign_in_btn_ || !cancel_btn_)
        return;
    sign_in_btn_->set_visible(s == State::Form);
    cancel_btn_->set_visible(mode_ == Mode::AddAccount);
    if (register_link_)
        register_link_->set_visible(s == State::Form && registration_supported_);
}

void LoginView::set_mode(Mode m)
{
    mode_ = m;
    if (cancel_btn_)
        cancel_btn_->set_visible(m == Mode::AddAccount);
    if (relayout_)
        relayout_();
}

bool LoginView::cancel_visible() const
{
    return cancel_btn_ && cancel_btn_->visible();
}

void LoginView::set_status(std::string message, std::optional<tk::Color> colour)
{
    if (!status_lbl_)
        return;
    if (message.empty())
    {
        status_lbl_->set_visible(false);
    }
    else
    {
        status_lbl_->set_text(std::move(message));
        status_lbl_->set_colour(colour);
        status_lbl_->set_visible(true);
    }
}

void LoginView::set_homeserver_label(std::string url)
{
    homeserver_label_ = std::move(url);
}

void LoginView::set_discovery_state(DiscoveryState s, std::string detail)
{
    discovery_state_ = s;
    ++registration_gen_;
    if (s != DiscoveryState::Resolved)
    {
        registration_supported_ = false;
        if (register_link_)
            register_link_->set_visible(false);
    }

    if (s == DiscoveryState::Resolved)
        resolved_base_url_ = detail;
    else if (s == DiscoveryState::Idle)
        resolved_base_url_.clear();

    // Probe before the discovery-label guard so the registration gate is
    // independent of that widget existing.
    if (s == DiscoveryState::Resolved)
        probe_registration_support_(resolved_base_url_);

    if (!discovery_lbl_)
        return;

    switch (s)
    {
    case DiscoveryState::Idle:
        discovery_lbl_->set_visible(false);
        break;
    case DiscoveryState::Discovering:
        resolved_base_url_.clear();
        discovery_lbl_->set_text("Checking\xe2\x80\xa6");
        discovery_lbl_->set_colour({});
        discovery_lbl_->set_visible(true);
        break;
    case DiscoveryState::Resolved:
        discovery_lbl_->set_text("\xe2\x9c\x93 " + detail);
        discovery_lbl_->set_colour(tk::Color::rgb(0x2e7d32));
        discovery_lbl_->set_visible(true);
        break;
    case DiscoveryState::Failed:
        discovery_lbl_->set_text(
            "\xe2\x9c\x97 " + (detail.empty() ? std::string("Could not reach this server")
                                              : detail));
        discovery_lbl_->set_colour(tk::Color::rgb(0xCC2200));
        discovery_lbl_->set_visible(true);
        break;
    }
}

// ---------------------------------------------------------------------------
// Controller wiring
// ---------------------------------------------------------------------------

void LoginView::init_with_field(std::unique_ptr<tk::NativeTextField> field)
{
    hs_field_ = std::move(field);
    hs_field_->set_placeholder("matrix.org or @user:matrix.org");
    hs_field_->set_text("matrix.org");
    hs_field_->set_on_submit([this] { sign_in_(); });
    hs_field_->set_on_changed(
        [this](const std::string& text) { hs_changed_(text); });
}

void LoginView::position_overlay()
{
    if (!hs_field_)
        return;
    auto r    = homeserver_field_rect_;
    r.x      += overlay_inset_;
    r.y      += overlay_inset_;
    r.w      -= overlay_inset_ * 2.0f;
    r.h      -= overlay_inset_ * 2.0f;
    hs_field_->set_rect(r);
}

void LoginView::set_status_message(const std::string& msg)
{
    set_status(msg);
    if (relayout_)
        relayout_();
}

void LoginView::shutdown()
{
    ++discovery_gen_;
    ++registration_gen_;
    cancelled_.store(true);
    if (client_)
        client_->cancel_oauth();
    join_worker_();
}

void LoginView::reset()
{
    ++discovery_gen_;
    cancelled_.store(true);
    if (client_)
        client_->cancel_oauth();
    join_worker_();
    cancelled_.store(false);

    set_status("");
    set_discovery_state(DiscoveryState::Idle);
    set_state(State::Form);
    if (hs_field_)
    {
        hs_field_->set_enabled(true);
        hs_field_->set_visible(true);
        hs_field_->set_focused(true);
    }
    if (relayout_)
        relayout_();
}

// ---------------------------------------------------------------------------
// Controller implementations
// ---------------------------------------------------------------------------

void LoginView::start_oauth_(bool register_account)
{
    if (!client_)
        return;
    std::string hs_raw = tesseract::text::trim(hs_field_->text());
    if (hs_raw.empty())
    {
        set_status("Please enter a homeserver.", tk::Color::rgb(0xB00020));
        relayout_();
        return;
    }

    // Use the pre-resolved URL when available; extract server name from a
    // raw MXID so begin_oauth() doesn't receive "@user:server".
    std::string hs;
    if (discovery_state_ == DiscoveryState::Resolved)
    {
        hs = resolved_base_url_;
    }
    else if (!hs_raw.empty() && hs_raw.front() == '@')
    {
        auto colon = hs_raw.find(':');
        hs = (colon != std::string::npos) ? hs_raw.substr(colon + 1) : hs_raw;
    }
    else
    {
        hs = hs_raw;
    }

    set_status("");
    hs_field_->set_enabled(false);
    set_state(State::Waiting);
    relayout_();

    join_worker_();
    cancelled_.store(false);
    if (on_begin_oauth_)
        on_begin_oauth_();

    auto* c = client_; // snapshot: set_client() may race with this worker
    worker_ = std::thread(
        [this, hs, c, register_account]
        {
            auto flow = c->begin_oauth(hs, register_account);
            if (cancelled_.load())
                return;
            bool        ok      = static_cast<bool>(flow);
            std::string payload = ok ? flow.auth_url : flow.message;
            post_to_ui_(
                [this, ok, payload = std::move(payload)]
                {
                    begin_completed_(ok, payload);
                });
        });
}

void LoginView::sign_in_()
{
    start_oauth_(false);
}

void LoginView::probe_registration_support_(const std::string& base_url)
{
    auto* snap = client_;
    if (!snap || base_url.empty())
        return;
    uint32_t gen = registration_gen_.load();
    auto body = [this, gen, snap, base_url]
    {
        if (gen != registration_gen_.load())
            return;
        bool supported = snap->homeserver_supports_registration(base_url);
        if (gen != registration_gen_.load())
            return;
        post_to_ui_(
            [this, gen, supported]
            {
                if (gen != registration_gen_.load())
                    return;
                registration_supported_ = supported;
                if (register_link_)
                    register_link_->set_visible(state_ == State::Form &&
                                                supported);
                if (relayout_)
                    relayout_();
            });
    };
    if (run_async_)
        run_async_(std::move(body));
    else
        std::thread(std::move(body)).detach();
}

void LoginView::hs_changed_(const std::string& text)
{
    uint32_t gen = ++discovery_gen_;
    if (text.empty())
    {
        set_discovery_state(DiscoveryState::Idle);
        relayout_();
        return;
    }
    set_discovery_state(DiscoveryState::Discovering);
    relayout_();

    auto* snap = client_;
    if (!snap)
        return;

    // Route through the host's `run_async` thunk so the worker is tracked
    // by ShellBase::workers_in_flight_ and drained on shutdown.  Without
    // this, a `discover_homeserver` blocked in tokio block_on outlives
    // ~LoginView; when rt.drop() later unblocks it, the post_to_ui_
    // callback fires into freed memory and corrupts the heap (the abort
    // typically surfaces later in an unrelated free, e.g. ~MessageListView).
    // Hosts that haven't wired set_run_async fall back to the legacy
    // detached thread for backward compat.
    auto body = [this, gen, snap, text]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (gen != discovery_gen_.load())
            return;
        auto result = snap->discover_homeserver(text);
        if (gen != discovery_gen_.load())
            return;
        post_to_ui_(
            [this, gen, result = std::move(result)]
            {
                if (gen != discovery_gen_.load())
                    return;
                if (result)
                    set_discovery_state(DiscoveryState::Resolved,
                                        result.base_url);
                else
                    set_discovery_state(DiscoveryState::Failed,
                                        result.error);
                relayout_();
            });
    };

    if (run_async_)
    {
        run_async_(std::move(body));
    }
    else
    {
        std::thread(std::move(body)).detach();
    }
}

void LoginView::begin_completed_(bool ok, std::string url)
{
    join_worker_();
    if (!ok)
    {
        set_status("Sign-in failed: " + url, tk::Color::rgb(0xB00020));
        set_state(State::Form);
        if (hs_field_)
            hs_field_->set_enabled(true);
        relayout_();
        return;
    }

    if (open_browser_)
        open_browser_(url);
    else
        tesseract::Client::open_in_browser(url);

    cancelled_.store(false);
    auto* c = client_;
    worker_ = std::thread(
        [this, c]
        {
            auto        res = c->await_oauth();
            if (cancelled_.load())
                return;
            bool        ok  = static_cast<bool>(res);
            std::string msg = res.message;
            post_to_ui_(
                [this, ok, msg = std::move(msg)]
                {
                    await_completed_(ok, msg);
                });
        });
}

void LoginView::await_completed_(bool ok, std::string err)
{
    join_worker_();
    if (ok)
    {
        if (on_success_)
            on_success_();
        return;
    }
    set_status("Sign-in failed: " + err, tk::Color::rgb(0xB00020));
    set_state(State::Form);
    if (hs_field_)
        hs_field_->set_enabled(true);
    relayout_();
}

void LoginView::cancel_()
{
    cancelled_.store(true);
    if (client_)
        client_->cancel_oauth();
    set_status("Cancelling\xe2\x80\xa6");
    relayout_();
    join_worker_();
    set_status("");
    set_state(State::Form);
    if (hs_field_)
        hs_field_->set_enabled(true);
    relayout_();
    if (on_cancel_done_)
        on_cancel_done_();
}

void LoginView::join_worker_()
{
    if (worker_.joinable())
        worker_.join();
}

// ---------------------------------------------------------------------------
// Widget tree
// ---------------------------------------------------------------------------

tk::Size LoginView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void LoginView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (!card_)
        return;

    tk::Size card_size = card_->measure(ctx, {kCardWidth, bounds.h});
    float    card_w    = std::min(kCardWidth, bounds.w);
    float    card_h    = std::min(card_size.h, bounds.h);
    float    card_x    = bounds.x + (bounds.w - card_w) * 0.5f;
    float    card_y    = bounds.y + (bounds.h - card_h) * 0.5f;
    card_->arrange(ctx, {card_x, card_y, card_w, card_h});

    if (hs_field_lbl_)
    {
        tk::Rect fr = hs_field_lbl_->bounds();
        float    h  = std::max(fr.h, kHSFieldHeight);
        homeserver_field_rect_ = {fr.x - bounds.x,
                                  fr.y - bounds.y - (h - fr.h) * 0.5f, fr.w, h};
    }
}

void LoginView::paint(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    if (!card_)
        return;

    tk::Rect cb = card_->bounds();
    ctx.canvas.fill_rounded_rect(cb, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(cb, 10.0f, ctx.theme.palette.border, 1.0f);

    if (hs_field_lbl_)
    {
        tk::Rect fr{homeserver_field_rect_.x + bounds_.x,
                    homeserver_field_rect_.y + bounds_.y,
                    homeserver_field_rect_.w, homeserver_field_rect_.h};
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border, 1.0f);
    }

    card_->paint(ctx);
}

} // namespace tesseract::views
