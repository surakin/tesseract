#include "LoginView.h"

#include "AlertDialog.h"
#include "tk/i18n.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <tesseract/client.h>

#include "views/text_util.h"

namespace tesseract::views
{

namespace
{

constexpr float kLoginCardWidth    = 360.0f;
constexpr float kLoginCardPadding  = 24.0f;
constexpr float kLoginCardSpacing  = 12.0f;
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
    card->set_padding(tk::Edges::all(kLoginCardPadding))
        .set_spacing(kLoginCardSpacing)
        .set_cross(tk::Cross::Stretch)
        .set_main(tk::Main::Start);

    auto title =
        std::make_unique<tk::Label>(tk::tr("Sign in to Matrix"), tk::FontRole::Title);
    title->set_halign(tk::TextHAlign::Center);

    auto caption = std::make_unique<tk::Label>(
        tk::tr("We'll open your browser to complete sign-in."), tk::FontRole::Body);
    caption->set_halign(tk::TextHAlign::Center);
    caption->set_wrap(true);

    auto hs_input_label = std::make_unique<tk::Label>(tk::tr("Homeserver or Matrix ID"),
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
        tk::tr("Sign in"), std::function<void()>{}, tk::Button::Variant::Primary);
    sign_in->set_min_size({0, kButtonHeight});
    sign_in->set_on_click([this] { sign_in_(); });

    auto cancel = std::make_unique<tk::Button>(
        tk::tr("Cancel"), std::function<void()>{}, tk::Button::Variant::Subtle);
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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    // Toggle button on the OAuthOnly form — only ever shown once discovery
    // positively confirms the server supports m.login.password.
    auto password_toggle = std::make_unique<tk::Button>(
        tk::tr("Sign in with password"), std::function<void()>{},
        tk::Button::Variant::Subtle);
    password_toggle->set_min_size({0, kButtonHeight});
    password_toggle->set_on_click([this] { switch_to_password_form_(); });
    password_toggle->set_visible(false);
    password_toggle_btn_ = card->add_child(std::move(password_toggle));

    // Password form fields — hidden until switch_to_password_form_() shows them.
    auto username_input_label = std::make_unique<tk::Label>(
        tk::tr("Matrix ID"), tk::FontRole::Small);
    username_input_label->set_halign(tk::TextHAlign::Leading);
    username_input_label->set_visible(false);

    auto username_field = std::make_unique<tk::Label>("", tk::FontRole::Body);
    username_field->set_halign(tk::TextHAlign::Leading);
    username_field->set_min_size({0.0f, kHSFieldHeight});
    username_field->set_visible(false);

    auto password_input_label = std::make_unique<tk::Label>(
        tk::tr("Password"), tk::FontRole::Small);
    password_input_label->set_halign(tk::TextHAlign::Leading);
    password_input_label->set_visible(false);

    auto password_field = std::make_unique<tk::Label>("", tk::FontRole::Body);
    password_field->set_halign(tk::TextHAlign::Leading);
    password_field->set_min_size({0.0f, kHSFieldHeight});
    password_field->set_visible(false);

    // Submit button for the Password form.
    auto password_sign_in = std::make_unique<tk::Button>(
        tk::tr("Sign in"), std::function<void()>{}, tk::Button::Variant::Primary);
    password_sign_in->set_min_size({0, kButtonHeight});
    password_sign_in->set_on_click([this] { submit_password_(); });
    password_sign_in->set_visible(false);

    // Back button — returns to the OAuthOnly form.
    auto back = std::make_unique<tk::Button>(
        tk::tr("Back"), std::function<void()>{}, tk::Button::Variant::Subtle);
    back->set_min_size({0, kButtonHeight});
    back->set_on_click([this] { switch_to_oauth_form_(); });
    back->set_visible(false);

    username_input_label_ = card->add_child(std::move(username_input_label));
    username_field_lbl_   = card->add_child(std::move(username_field));
    password_input_label_ = card->add_child(std::move(password_input_label));
    password_field_lbl_   = card->add_child(std::move(password_field));
    password_sign_in_btn_ = card->add_child(std::move(password_sign_in));
    back_btn_              = card->add_child(std::move(back));
#endif

    auto register_link = std::make_unique<tk::Button>(
        tk::tr("New here? Create an account"), std::function<void()>{},
        tk::Button::Variant::Subtle);
    register_link->set_min_size({0, kButtonHeight});
    register_link->set_on_click([this] { start_oauth_(true); });
    register_link->set_visible(false);
    register_link_ = card->add_child(std::move(register_link));

    cancel_btn_     = card->add_child(std::move(cancel));
    status_lbl_     = card->add_child(std::move(status));

    card_ = add_child(std::move(card));

    auto alert_dlg = std::make_unique<AlertDialog>();
    alert_dlg->on_layout_changed = [this] { if (relayout_) relayout_(); };
    alert_ = add_child(std::move(alert_dlg));
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
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    update_form_visibility_();
#endif
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

void LoginView::set_status(std::string message, bool is_error)
{
    status_is_error_ = is_error;
    if (!status_lbl_)
        return;
    if (message.empty())
    {
        status_lbl_->set_visible(false);
    }
    else
    {
        status_lbl_->set_text(std::move(message));
        status_lbl_->set_visible(true);
    }
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
        discovery_lbl_->set_text(tk::tr("Checking\xe2\x80\xa6"));
        discovery_lbl_->set_visible(true);
        break;
    case DiscoveryState::Resolved:
        discovery_lbl_->set_text("\xe2\x9c\x93 " + detail);
        discovery_lbl_->set_visible(true);
        break;
    case DiscoveryState::Failed:
        discovery_lbl_->set_text(
            "\xe2\x9c\x97 " + (detail.empty()
                                   ? tk::tr("Could not reach this server")
                                   : detail));
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
    hs_field_->set_placeholder(tk::tr("matrix.org or @user:matrix.org"));
    hs_field_->set_text("matrix.org");
    hs_field_->set_on_submit([this] { sign_in_(); });
    hs_field_->set_on_changed(
        [this](const std::string& text) { hs_changed_(text); });
    hs_changed_("matrix.org");
}

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
void LoginView::init_password_fields(std::unique_ptr<tk::NativeTextField> username_field,
                                     std::unique_ptr<tk::NativeTextField> password_field)
{
    username_field_ = std::move(username_field);
    username_field_->set_placeholder(tk::tr("@user:example.org"));
    username_field_->set_on_submit([this] { submit_password_(); });
    username_field_->set_visible(password_available_);

    password_field_ = std::move(password_field);
    password_field_->set_password(true);
    password_field_->set_on_submit([this] { submit_password_(); });
    password_field_->set_visible(password_available_);
}
#endif

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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (username_field_)
    {
        auto ur = username_field_rect_;
        ur.x += overlay_inset_;
        ur.y += overlay_inset_;
        ur.w -= overlay_inset_ * 2.0f;
        ur.h -= overlay_inset_ * 2.0f;
        username_field_->set_rect(ur);
    }
    if (password_field_)
    {
        auto pr = password_field_rect_;
        pr.x += overlay_inset_;
        pr.y += overlay_inset_;
        pr.w -= overlay_inset_ * 2.0f;
        pr.h -= overlay_inset_ * 2.0f;
        password_field_->set_rect(pr);
    }
#endif
}

void LoginView::set_status_message(const std::string& msg)
{
    set_status(msg);
    if (relayout_)
        relayout_();
}

void LoginView::set_client(tesseract::Client* c)
{
    // See the header for the full lifetime invariant. If an OAuth flow is in
    // flight (State::Waiting — worker_ blocked inside begin_oauth/await_oauth
    // on the current client_), tear it down before swapping the non-owning
    // alias, using the same sequence shutdown()/reset() use: mark cancelled so
    // the worker's UI-thread post-back drops, cancel_oauth() to unblock the
    // in-flight FFI call, then join_worker_() so the worker has fully returned
    // and stopped touching the old client_ before the alias changes.
    if (state_ == State::Waiting && client_ && client_ != c)
    {
        cancelled_.store(true);
        client_->cancel_oauth();
        join_worker_();
        cancelled_.store(false);
        set_state(State::Form);
    }
    client_ = c;
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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    form_kind_ = FormKind::OAuthOnly;
#endif
    set_status("");
    set_discovery_state(DiscoveryState::Idle);
    set_state(State::Form);
    if (hs_field_)
    {
        hs_field_->set_enabled(true);
        hs_field_->set_visible(true);
        hs_field_->set_focused(true);
        hs_changed_(hs_field_->text());
    }
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (username_field_)
        username_field_->set_enabled(true);
    if (password_field_)
        password_field_->set_enabled(true);
#endif
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
        set_status(tk::tr("Please enter a homeserver."), true);
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
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
                update_form_visibility_();
#else
                if (register_link_)
                    register_link_->set_visible(state_ == State::Form &&
                                                supported);
#endif
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
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
        update_password_availability_(false); // unknown while idle — stay hidden
#endif
        relayout_();
        return;
    }
    set_discovery_state(DiscoveryState::Discovering);
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    update_password_availability_(false); // unknown while still probing — stay hidden
#endif
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
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
                // Strict: the toggle button only ever appears once discovery
                // positively confirms (Resolved) the server supports
                // m.login.password. Any other outcome keeps it hidden.
                update_password_availability_(result && result.supports_password);
#endif
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
        set_status(tk::trf(tk::tr("Sign-in failed: {0}"), {url}), true);
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
    set_status(tk::trf(tk::tr("Sign-in failed: {0}"), {err}), true);
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
    set_status(tk::tr("Cancelling\xe2\x80\xa6"));
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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
void LoginView::update_password_availability_(bool supported)
{
    password_available_ = supported;
    update_form_visibility_();
}

void LoginView::switch_to_password_form_()
{
    form_kind_ = FormKind::Password;
    update_form_visibility_();
    if (relayout_)
        relayout_();
    if (username_field_)
        username_field_->set_focused(true);
}

void LoginView::switch_to_oauth_form_()
{
    form_kind_ = FormKind::OAuthOnly;
    // Clear the typed password as a minor security nicety; keep the typed
    // username since re-entering it is pure friction with no benefit.
    if (password_field_)
        password_field_->set_text("");
    update_form_visibility_();
    if (relayout_)
        relayout_();
}

void LoginView::update_form_visibility_()
{
    bool oauth_form_visible    = state_ == State::Form && form_kind_ == FormKind::OAuthOnly;
    bool password_form_visible = state_ == State::Form && form_kind_ == FormKind::Password;

    if (caption_lbl_)
        caption_lbl_->set_text(
            form_kind_ == FormKind::Password
                ? tk::tr("Sign in with your Matrix ID and password.")
                : tk::tr("We'll open your browser to complete sign-in."));

    if (hs_input_label_) hs_input_label_->set_visible(oauth_form_visible);
    if (hs_field_lbl_)   hs_field_lbl_->set_visible(oauth_form_visible);
    if (hs_field_)       hs_field_->set_visible(oauth_form_visible);
    if (discovery_lbl_)
        discovery_lbl_->set_visible(oauth_form_visible &&
                                    discovery_state_ != DiscoveryState::Idle);
    if (sign_in_btn_)
        sign_in_btn_->set_visible(oauth_form_visible);
    if (password_toggle_btn_)
        password_toggle_btn_->set_visible(oauth_form_visible && password_available_);
    if (register_link_)
        register_link_->set_visible(oauth_form_visible && registration_supported_);

    if (username_input_label_) username_input_label_->set_visible(password_form_visible);
    if (username_field_lbl_)   username_field_lbl_->set_visible(password_form_visible);
    if (username_field_)       username_field_->set_visible(password_form_visible);
    if (password_input_label_) password_input_label_->set_visible(password_form_visible);
    if (password_field_lbl_)   password_field_lbl_->set_visible(password_form_visible);
    if (password_field_)       password_field_->set_visible(password_form_visible);
    if (password_sign_in_btn_) password_sign_in_btn_->set_visible(password_form_visible);
    if (back_btn_)              back_btn_->set_visible(password_form_visible);

    // Cancel button: AddAccount mode shows it, except while a password login
    // is in flight — that call has no cancellation support (see
    // submit_password_), so leaving Cancel clickable would block the caller
    // inside join_worker_() for up to the HTTP client's timeout.
    if (cancel_btn_)
        cancel_btn_->set_visible(
            mode_ == Mode::AddAccount &&
            !(state_ == State::Waiting && form_kind_ == FormKind::Password));
}

void LoginView::submit_password_()
{
    if (!client_ || !username_field_ || !password_field_)
        return;

    std::string username = tesseract::text::trim(username_field_->text());
    std::string password  = password_field_->text(); // never trim a password
    if (username.empty() || password.empty())
    {
        set_status(tk::tr("Please enter your Matrix ID and password."), true);
        relayout_();
        return;
    }

    std::string hs_raw = tesseract::text::trim(hs_field_->text());
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
    if (hs.empty())
    {
        set_status(tk::tr("Please enter a homeserver."), true);
        relayout_();
        return;
    }

    set_status("");
    hs_field_->set_enabled(false);
    username_field_->set_enabled(false);
    password_field_->set_enabled(false);
    // set_state(Waiting) → update_form_visibility_() hides Cancel while
    // form_kind_ == Password (that call has no cancellation support).
    set_state(State::Waiting);
    relayout_();

    join_worker_();
    cancelled_.store(false);
    if (on_begin_oauth_)
        on_begin_oauth_(); // arms the temp dir; auth-agnostic despite the name

    auto* c = client_; // snapshot: set_client() may race with this worker
    worker_ = std::thread(
        [this, hs, c, username, password]
        {
            auto res = c->login_password(hs, username, password);
            if (cancelled_.load())
                return;
            bool        ok  = static_cast<bool>(res);
            std::string msg = res.message;
            post_to_ui_(
                [this, ok, msg = std::move(msg)]
                {
                    password_login_completed_(ok, msg);
                });
        });
}

void LoginView::password_login_completed_(bool ok, std::string err)
{
    join_worker_();
    if (ok)
    {
        if (on_success_)
            on_success_();
        return;
    }
    set_status(tk::trf(tk::tr("Sign-in failed: {0}"), {err}), true);
    set_state(State::Form);
    if (hs_field_)
        hs_field_->set_enabled(true);
    if (username_field_)
        username_field_->set_enabled(true);
    if (password_field_)
        password_field_->set_enabled(true);
    relayout_();
}
#endif

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

    tk::Size card_size = card_->measure(ctx, {kLoginCardWidth, bounds.h});
    float    card_w    = std::min(kLoginCardWidth, bounds.w);
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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (username_field_lbl_)
    {
        tk::Rect fr = username_field_lbl_->bounds();
        float    h  = std::max(fr.h, kHSFieldHeight);
        username_field_rect_ = {fr.x - bounds.x,
                                fr.y - bounds.y - (h - fr.h) * 0.5f, fr.w, h};
    }
    if (password_field_lbl_)
    {
        tk::Rect fr = password_field_lbl_->bounds();
        float    h  = std::max(fr.h, kHSFieldHeight);
        password_field_rect_ = {fr.x - bounds.x,
                                fr.y - bounds.y - (h - fr.h) * 0.5f, fr.w, h};
    }
#endif

    if (alert_) alert_->arrange(ctx, bounds);
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

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (username_field_lbl_ && username_field_lbl_->visible())
    {
        tk::Rect fr{username_field_rect_.x + bounds_.x,
                    username_field_rect_.y + bounds_.y,
                    username_field_rect_.w, username_field_rect_.h};
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border, 1.0f);
    }
    if (password_field_lbl_ && password_field_lbl_->visible())
    {
        tk::Rect fr{password_field_rect_.x + bounds_.x,
                    password_field_rect_.y + bounds_.y,
                    password_field_rect_.w, password_field_rect_.h};
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border, 1.0f);
    }
#endif

    if (status_lbl_)
        status_lbl_->set_colour(
            status_is_error_ ? std::optional<tk::Color>(ctx.theme.palette.destructive)
                              : std::nullopt);
    if (discovery_lbl_)
    {
        switch (discovery_state_)
        {
        case DiscoveryState::Resolved:
            discovery_lbl_->set_colour(ctx.theme.palette.success);
            break;
        case DiscoveryState::Failed:
            discovery_lbl_->set_colour(ctx.theme.palette.destructive);
            break;
        default:
            discovery_lbl_->set_colour(std::nullopt);
            break;
        }
    }

    card_->paint(ctx);
    if (alert_) alert_->paint(ctx);
}

// ---------------------------------------------------------------------------
// Error overlay
// ---------------------------------------------------------------------------

void LoginView::show_restore_error(std::string body, std::function<void()> retry_cb)
{
    if (!alert_) return;
    AlertDialog::Options opts;
    opts.title           = tk::tr("Connection Error");
    opts.body            = std::move(body);
    opts.primary_label   = tk::tr("Retry");
    opts.secondary_label = tk::tr("Sign In");
    alert_->open(std::move(opts),
                 std::move(retry_cb),
                 [this] { if (alert_) alert_->close(); });
}

} // namespace tesseract::views
