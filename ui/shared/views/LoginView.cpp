#include "LoginView.h"

#include "AlertDialog.h"
#include "icons.h"
#include "tk/cache_key.h"
#include "tk/i18n.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

#include <tesseract/client.h>
#include <tesseract/paths.h>
#include <tesseract/provisioning.h>
#include <tesseract/version.h>

#include "views/text_util.h"

#if TESSERACT_HAS_BRAND_ICON
#include "brand_icon.h"
#endif

namespace tesseract::views
{

namespace
{

constexpr float kLoginCardWidth    = 360.0f;
constexpr float kLoginCardPadding  = 24.0f;
constexpr float kLoginCardSpacing  = 12.0f;
constexpr float kHSFieldHeight = 36.0f;
constexpr float kButtonHeight  = 36.0f;
constexpr float kBrandIconDiameter = 56.0f;

// Built-in default when no homeserver is provisioned. The homeserver field
// shows the default as a placeholder rather than real text (see
// finish_init()); an empty field is treated as if the user had typed it, both
// for discovery and for sign-in.
constexpr const char* kBuiltinDefaultHomeserver = "matrix.org";

} // namespace

LoginView::LoginView()
    : default_homeserver_(tesseract::provisioned_default_homeserver().value_or(
          kBuiltinDefaultHomeserver)),
      provisioned_(tesseract::login_provisioned())
{
    rebuild_tree();
}

void LoginView::rebuild_tree()
{
    auto card = tk::create_widget<tk::VBox>(this);
    card->set_padding(tk::Edges::all(kLoginCardPadding))
        .set_spacing(kLoginCardSpacing)
        .set_cross(tk::Cross::Stretch)
        .set_main(tk::Main::Start);

    auto brand_avatar = tk::create_widget<tk::Avatar>(this, tesseract::kAppName);
    brand_avatar->set_diameter(kBrandIconDiameter);

    auto title =
        tk::create_widget<tk::Label>(this, tk::tr("Sign in to Matrix"), tk::FontRole::Title);
    title->set_halign(tk::TextHAlign::Center);

    auto hs_input_label = tk::create_widget<tk::Label>(
        this,
        tk::tr("Your account or server - e.g. @alice:matrix.org or matrix.org"),
        tk::FontRole::Small);
    hs_input_label->set_halign(tk::TextHAlign::Leading);
    hs_input_label->set_wrap(true);

    auto hs_field_row = tk::create_widget<tk::HBox>(this);
    hs_field_row->set_spacing(8.0f).set_cross(tk::Cross::Center);

    auto hs_field = tk::create_widget<tk::TextField>(this, kHSFieldHeight);
    hs_field->set_layout_hints({.fill_main = true});

    // Inline ✓/✗/"Checking…" indicator next to the field — see
    // set_discovery_state(). No text for a valid result; the error text
    // (discovery_error_lbl_ below) is what explains a Failed one.
    auto discovery_icon = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    discovery_icon->set_halign(tk::TextHAlign::Leading);
    discovery_icon->set_visible(false);

    auto hs_help_btn = tk::create_widget<tk::Button>(
        this, tk::tr("?"), std::function<void()>{}, tk::Button::Variant::Subtle);

    auto discovery_error = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    discovery_error->set_halign(tk::TextHAlign::Leading);
    discovery_error->set_wrap(true);
    discovery_error->set_visible(false);

    auto sign_in = tk::create_widget<tk::Button>(
        this, tk::tr("Sign in"), std::function<void()>{}, tk::Button::Variant::Primary);
    sign_in->set_min_size({0, kButtonHeight});
    sign_in->set_on_click([this] { sign_in_(); });

    auto cancel = tk::create_widget<tk::Button>(
        this, tk::tr("Cancel"), std::function<void()>{}, tk::Button::Variant::Subtle);
    cancel->set_min_size({0, kButtonHeight});
    cancel->set_on_click([this] { cancel_(); });
    cancel->set_visible(false);

    auto status = tk::create_widget<tk::Label>(this, "", tk::FontRole::Small);
    status->set_halign(tk::TextHAlign::Center);
    status->set_wrap(true);
    status->set_visible(false);

    hs_field_            = hs_field_row->add_child(std::move(hs_field));
    discovery_icon_lbl_  = hs_field_row->add_child(std::move(discovery_icon));
    hs_help_btn_         = hs_field_row->add_child(std::move(hs_help_btn));
    hs_help_btn_->set_on_hover_enter([this]
    {
        if (host())
            host()->show_tooltip(
                hs_help_btn_,
                tk::tr("Your homeserver is where your account lives — similar to "
                       "choosing an email provider. Not sure? Use \"Find a server\" "
                       "to browse some. Any server works with anyone on Matrix."),
                hs_help_btn_->bounds());
    });
    hs_help_btn_->set_on_hover_leave([this]
    {
        if (host()) host()->hide_tooltip(hs_help_btn_);
    });

    brand_avatar_        = card->add_child(std::move(brand_avatar));
    title_lbl_           = card->add_child(std::move(title));
    hs_input_label_      = card->add_child(std::move(hs_input_label));
    card->add_child(std::move(hs_field_row));

#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    auto find_server = tk::create_widget<tk::Button>(
        this, tk::tr("Find a server"), std::function<void()>{}, tk::Button::Variant::Link);
    find_server->set_on_click([this] { open_server_browser(); });
    find_server->set_visible(!provisioned_);
    find_server_btn_ = card->add_child(std::move(find_server));
#endif

    discovery_error_lbl_ = card->add_child(std::move(discovery_error));
    sign_in_btn_         = card->add_child(std::move(sign_in));

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    // Toggle button on the OAuthOnly form — only ever shown once discovery
    // positively confirms the server supports m.login.password.
    auto password_toggle = tk::create_widget<tk::Button>(
        this, tk::tr("Sign in with password"), std::function<void()>{},
        tk::Button::Variant::Subtle);
    password_toggle->set_min_size({0, kButtonHeight});
    password_toggle->set_on_click([this] { switch_to_password_form_(); });
    password_toggle->set_visible(false);
    password_toggle_btn_ = card->add_child(std::move(password_toggle));

    // Password form fields — hidden until switch_to_password_form_() shows them.
    auto username_input_label = tk::create_widget<tk::Label>(
        this, tk::tr("Matrix ID"), tk::FontRole::Small);
    username_input_label->set_halign(tk::TextHAlign::Leading);
    username_input_label->set_visible(false);

    auto username_field = tk::create_widget<tk::TextField>(this, kHSFieldHeight);
    username_field->set_visible(false);

    auto password_input_label = tk::create_widget<tk::Label>(
        this, tk::tr("Password"), tk::FontRole::Small);
    password_input_label->set_halign(tk::TextHAlign::Leading);
    password_input_label->set_visible(false);

    auto password_field = tk::create_widget<tk::TextField>(this, kHSFieldHeight);
    password_field->set_visible(false);

    // Submit button for the Password form.
    auto password_sign_in = tk::create_widget<tk::Button>(
        this, tk::tr("Sign in"), std::function<void()>{}, tk::Button::Variant::Primary);
    password_sign_in->set_min_size({0, kButtonHeight});
    password_sign_in->set_on_click([this] { submit_password_(); });
    password_sign_in->set_visible(false);

    // Back button — returns to the OAuthOnly form.
    auto back = tk::create_widget<tk::Button>(
        this, tk::tr("Back"), std::function<void()>{}, tk::Button::Variant::Subtle);
    back->set_min_size({0, kButtonHeight});
    back->set_on_click([this] { switch_to_oauth_form_(); });
    back->set_visible(false);

    username_input_label_ = card->add_child(std::move(username_input_label));
    username_field_       = card->add_child(std::move(username_field));
    password_input_label_ = card->add_child(std::move(password_input_label));
    password_field_       = card->add_child(std::move(password_field));
    password_sign_in_btn_ = card->add_child(std::move(password_sign_in));
    back_btn_              = card->add_child(std::move(back));
#endif

    auto register_link = tk::create_widget<tk::Button>(
        this, tk::tr("New here? Create an account"), std::function<void()>{},
        tk::Button::Variant::Subtle);
    register_link->set_min_size({0, kButtonHeight});
    register_link->set_on_click([this] { on_register_clicked_(); });
    register_link->set_visible(false);
    register_link_ = card->add_child(std::move(register_link));

    cancel_btn_     = card->add_child(std::move(cancel));
    status_lbl_     = card->add_child(std::move(status));

    card_ = add_child(std::move(card));

    auto settings = tk::create_widget<tk::Button>(
        this, "", std::function<void()>{}, tk::Button::Variant::Icon);
    settings->set_icon(kWrenchSvg, 18.0f);
    settings->set_accessible_name(tk::tr("Settings"));
    settings->set_on_click(
        [this]
        {
            if (on_open_settings_)
                on_open_settings_();
        });
    settings_btn_ = add_child(std::move(settings));
    update_settings_visibility_();

#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    auto browser = tk::create_widget<ServerBrowserOverlay>(this);
    browser->on_layout_changed = [this]
    {
        // The homeserver field is a native control and would draw above the
        // overlay, so hide it while the browser is open (see
        // KeyboardShortcutsOverlay).
        if (hs_field_ && server_browser_)
        {
            if (server_browser_->is_open())
            {
                if (hs_field_->visible())
                {
                    hs_field_->set_visible(false);
                    hs_field_hidden_by_browser_ = true;
                }
            }
            else if (hs_field_hidden_by_browser_)
            {
                hs_field_hidden_by_browser_ = false;
                hs_field_->set_visible(true);
            }
        }
        if (relayout_)
            relayout_();
    };
    browser->on_pick = [this](const std::string& domain)
    {
        if (!hs_field_)
            return;
        hs_field_->set_text(domain);
        hs_changed_(domain); // native controls don't all report programmatic edits
        hs_field_->set_focused(true);
    };
    browser->on_open_url = [this](const std::string& url)
    {
        if (open_browser_)
            open_browser_(url);
        else
            tesseract::Client::open_in_browser(url);
    };
    browser->on_retry = [this] { load_server_list_(); };
    server_browser_ = add_child(std::move(browser));
#endif

    auto alert_dlg = std::make_unique<AlertDialog>();
    alert_dlg->on_layout_changed = [this] { if (relayout_) relayout_(); };
    alert_ = add_child(std::move(alert_dlg));
}

// ---------------------------------------------------------------------------
// Visual state
// ---------------------------------------------------------------------------

void LoginView::update_settings_visibility_()
{
    if (settings_btn_)
        settings_btn_->set_visible(mode_ == Mode::Initial &&
                                   state_ == State::Form);
}

void LoginView::set_state(State s)
{
    state_ = s;
    update_settings_visibility_();
    if (!sign_in_btn_ || !cancel_btn_)
        return;
    sign_in_btn_->set_visible(s == State::Form && oauth_available_);
    cancel_btn_->set_visible(mode_ == Mode::AddAccount || s == State::Waiting);
    if (register_link_)
        register_link_->set_visible(s == State::Form && registration_supported_);
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    if (find_server_btn_)
        find_server_btn_->set_visible(!provisioned_ && s == State::Form);
#endif
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    update_form_visibility_();
#endif
}

void LoginView::set_mode(Mode m)
{
    mode_ = m;
    update_settings_visibility_();
    if (cancel_btn_)
        cancel_btn_->set_visible(m == Mode::AddAccount || state_ == State::Waiting);
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
    ++oauth_gen_;
    oauth_probe_done_ = false; // a new answer is coming (or none, if not Resolved)
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

    // Probe before the discovery-label guard so the gates are independent of
    // that widget existing.
    if (s == DiscoveryState::Resolved)
    {
        probe_registration_support_(resolved_base_url_);
        // Deliberately a second, independent metadata fetch rather than
        // consolidating with the registration probe above — see oauth.rs's
        // supports_oauth doc comment.
        probe_oauth_support_(resolved_base_url_);
    }

    if (!discovery_icon_lbl_ || !discovery_error_lbl_)
        return;

    switch (s)
    {
    case DiscoveryState::Idle:
        discovery_icon_lbl_->set_visible(false);
        discovery_error_lbl_->set_visible(false);
        break;
    case DiscoveryState::Discovering:
        resolved_base_url_.clear();
        discovery_icon_lbl_->set_text(tk::tr("Checking\xe2\x80\xa6"));
        discovery_icon_lbl_->set_visible(true);
        discovery_error_lbl_->set_visible(false);
        break;
    case DiscoveryState::Resolved:
        // `detail` is the resolved base URL — an implementation detail the
        // user doesn't need to see; resolved_base_url_ (set above) still
        // carries it for begin_oauth(). Just the checkmark, no text.
        discovery_icon_lbl_->set_text("\xe2\x9c\x93");
        discovery_icon_lbl_->set_visible(true);
        discovery_error_lbl_->set_visible(false);
        break;
    case DiscoveryState::Failed:
        discovery_icon_lbl_->set_text("\xe2\x9c\x97");
        discovery_icon_lbl_->set_visible(true);
        discovery_error_lbl_->set_text(
            detail.empty() ? tk::tr("Could not reach this server") : detail);
        discovery_error_lbl_->set_visible(true);
        break;
    }
}

// ---------------------------------------------------------------------------
// Controller wiring
// ---------------------------------------------------------------------------

void LoginView::finish_init()
{
    hs_field_->set_placeholder(default_homeserver_ == kBuiltinDefaultHomeserver
                                   ? tk::tr("matrix.org")
                                   : default_homeserver_);
    hs_field_->set_on_submit([this] { submit_homeserver_(); });
    hs_field_->set_on_changed(
        [this](const std::string& text) { hs_changed_(text); });
    hs_changed_("");
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    if (!provisioned_)
        load_server_list_();
#endif
}

void LoginView::on_register_clicked_()
{
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    if (!provisioned_ && hs_field_ && tesseract::text::trim(hs_field_->text()).empty())
    {
        open_server_browser();
        return;
    }
#endif
    start_oauth_(true);
}

#ifdef TESSERACT_SERVER_BROWSER_ENABLED
void LoginView::open_server_browser()
{
    if (!server_browser_ || provisioned_)
        return;
    if (server_browser_->state() != ServerBrowserOverlay::State::Ready)
        load_server_list_();
    server_browser_->open();
    request_server_icons_();
}

void LoginView::load_server_list_()
{
    auto* snap = client_;
    if (!snap || !server_browser_)
        return;
    if (server_list_loading_)
        return;
    server_list_loading_ = true;
    server_browser_->set_state(ServerBrowserOverlay::State::Loading);
    const uint32_t gen = ++server_list_gen_;
    auto body = [this, gen, snap]
    {
        auto list = snap->load_server_list();
        if (gen != server_list_gen_.load() || !post_to_ui_)
            return;
        post_to_ui_(
            [this, gen, list = std::move(list)]
            {
                if (gen != server_list_gen_.load())
                    return;
                server_list_loading_ = false;
                apply_server_list_(list);
            });
    };
    if (run_async_)
        run_async_(std::move(body));
    else
        std::thread(std::move(body)).detach();
}

void LoginView::apply_server_list_(const tesseract::Client::ServerList& list)
{
    if (!server_browser_)
        return;
    if (!list.ok)
    {
        server_browser_->set_state(ServerBrowserOverlay::State::Failed);
        return;
    }
    server_browser_->set_source(list.source_name, list.source_url);
    auto entries = list.entries;
    // matrix.org isn't on the community list; offer it as one choice among many.
    ServerBrowserOverlay::Entry mx;
    mx.name = "matrix.org";
    mx.client_domain = "matrix.org";
    mx.homepage = "https://matrix.org";
    mx.description = tk::tr("Run by the Matrix.org Foundation. The largest public server.");
    mx.software = "Synapse";
    mx.longstanding = true;
    mx.is_public = true;
    entries.push_back(std::move(mx));
    std::mt19937 rng{std::random_device{}()};
    server_icon_targets_.clear();
    for (const auto& e : entries)
        server_icon_targets_.emplace_back(e.client_domain, e.homepage);
    server_browser_->set_servers(
        ServerBrowserOverlay::arrange_for_display(std::move(entries), rng));
    if (server_browser_->is_open())
        request_server_icons_();
}

void LoginView::request_server_icons_()
{
    auto* snap = client_;
    if (!snap || !server_browser_ || icons_fetching_)
        return;
    struct Work
    {
        std::vector<std::pair<std::string, std::string>> todo;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> active{0};
    };
    auto work = std::make_shared<Work>();
    for (const auto& t : server_icon_targets_)
        if (!t.second.empty() && !server_browser_->has_icon(t.first))
            work->todo.push_back(t);
    if (work->todo.empty())
        return;
    if (!icon_cache_)
        icon_cache_ = std::make_shared<tk::MediaDiskCache>(tesseract::cache_dir() / "media");
    icons_fetching_ = true;
    const uint32_t gen = icon_gen_.load();
    const std::size_t workers = std::min<std::size_t>(4, work->todo.size());
    work->active = workers;
    for (std::size_t w = 0; w < workers; ++w)
    {
        auto body = [this, gen, snap, work, cache = icon_cache_]
        {
            for (;;)
            {
                const std::size_t i = work->next.fetch_add(1);
                if (i >= work->todo.size() || gen != icon_gen_.load())
                    break;
                const auto& [domain, homepage] = work->todo[i];
                const tk::CacheKey key = tk::CacheKey::server_icon(homepage);
                auto bytes = cache->load(key);
                if (bytes.empty())
                {
                    bytes = snap->fetch_server_icon(homepage);
                    if (!bytes.empty())
                        cache->store(key, bytes);
                }
                if (bytes.empty() || gen != icon_gen_.load() || !post_to_ui_)
                    continue;
                post_to_ui_(
                    [this, gen, domain = domain, bytes = std::move(bytes)]() mutable
                    {
                        if (gen == icon_gen_.load() && server_browser_)
                            server_browser_->set_icon(domain, std::move(bytes));
                    });
            }
            if (work->active.fetch_sub(1) == 1 && gen == icon_gen_.load() && post_to_ui_)
                post_to_ui_(
                    [this, gen]
                    {
                        if (gen == icon_gen_.load())
                            icons_fetching_ = false;
                    });
        };
        if (run_async_)
            run_async_(std::move(body));
        else
            std::thread(std::move(body)).detach();
    }
}
#endif // TESSERACT_SERVER_BROWSER_ENABLED

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
void LoginView::finish_password_init()
{
    username_field_->set_placeholder(tk::tr("@user:example.org"));
    username_field_->set_on_submit([this] { submit_password_(); });
    username_field_->set_visible(password_available_);
    if (auto user_id = tesseract::provisioned_user_id())
        username_field_->set_text(*user_id);

    password_field_->set_password(true);
    password_field_->set_on_submit([this] { submit_password_(); });
    password_field_->set_visible(password_available_);
}
#endif

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
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    ++server_list_gen_;
    ++icon_gen_;
#endif
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
        hs_raw = default_homeserver_; // empty field shows the placeholder default

    // OAuth login hint (prefills the username on the server): a Matrix ID typed
    // into the homeserver field wins over the provisioned one.
    std::string user_id_hint;
    if (!register_account)
    {
        if (hs_raw.size() > 1 && hs_raw.front() == '@' && hs_raw.find(':') != std::string::npos)
            user_id_hint = hs_raw;
        else if (auto user_id = tesseract::provisioned_user_id())
            user_id_hint = *user_id;
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
        [this, hs, c, register_account, user_id_hint]
        {
            auto flow = c->begin_oauth(hs, register_account, user_id_hint);
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

void LoginView::update_method_controls_enabled_()
{
    const bool ready = homeserver_submit_action() != SubmitAction::Wait;
    if (sign_in_btn_)
        sign_in_btn_->set_enabled(ready);
    if (register_link_)
        register_link_->set_enabled(ready);
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (password_toggle_btn_)
        password_toggle_btn_->set_enabled(ready);
#endif
}

LoginView::SubmitAction LoginView::homeserver_submit_action() const
{
    // Still checking what the server offers (its sign-in methods and OAuth
    // support): the OAuth default isn't an answer yet, and a password-only
    // server would get an OAuth attempt.
    if (!discovery_settled_ ||
        (discovery_state_ == DiscoveryState::Resolved && !oauth_probe_done_))
        return SubmitAction::Wait;
    // Otherwise the same as the button the form shows.
    if (oauth_available_)
        return SubmitAction::OAuth;
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (password_available_)
        return SubmitAction::Password;
#endif
    return SubmitAction::Nothing;
}

void LoginView::submit_homeserver_()
{
    submit_pending_ = false;
    if (submit_status_shown_)
    {
        set_status("");
        submit_status_shown_ = false;
    }
    if (state_ != State::Form)
        return;
    switch (homeserver_submit_action())
    {
        case SubmitAction::Wait:
            // Say the Enter was taken; it goes ahead once the check is done.
            submit_pending_      = true;
            submit_status_shown_ = true;
            set_status(tk::tr("Checking how you can sign in to this server\xe2\x80\xa6"));
            if (relayout_) relayout_();
            break;
        case SubmitAction::OAuth:
            start_oauth_(false);
            break;
        case SubmitAction::Password:
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
            switch_to_password_form_();
#endif
            break;
        case SubmitAction::Nothing:
            break;
    }
}

void LoginView::run_pending_submit_()
{
    if (submit_pending_ && homeserver_submit_action() != SubmitAction::Wait)
        submit_homeserver_();
}

void LoginView::apply_discovery_result_(bool ok, const std::string& base_url_or_error,
                                        bool supports_password)
{
    if (ok)
        set_discovery_state(DiscoveryState::Resolved, base_url_or_error);
    else
        set_discovery_state(DiscoveryState::Failed, base_url_or_error);
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    // Strict: the toggle button only ever appears once discovery positively
    // confirms (Resolved) the server supports m.login.password. Any other
    // outcome keeps it hidden.
    update_password_availability_(ok && supports_password);
#else
    (void)supports_password;
#endif
    discovery_settled_ = true;
    update_method_controls_enabled_();
    relayout_();
    // A Failed check settles it; a Resolved one still waits for the OAuth
    // probe (which runs it when it answers).
    run_pending_submit_();
}

void LoginView::oauth_probe_done_result_(bool supported)
{
    oauth_probe_done_ = true;
    update_oauth_availability_(supported);
    update_method_controls_enabled_();
    if (relayout_)
        relayout_();
    run_pending_submit_();
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

void LoginView::probe_oauth_support_(const std::string& base_url)
{
    auto* snap = client_;
    if (!snap || base_url.empty())
    {
        // Nothing to ask: keep the permissive default, but count the check
        // as done so a remembered Enter isn't stuck waiting.
        oauth_probe_done_result_(oauth_available_);
        return;
    }
    uint32_t gen = oauth_gen_.load();
    auto body = [this, gen, snap, base_url]
    {
        if (gen != oauth_gen_.load())
            return;
        bool supported = snap->homeserver_supports_oauth(base_url);
        if (gen != oauth_gen_.load())
            return;
        post_to_ui_(
            [this, gen, supported]
            {
                if (gen != oauth_gen_.load())
                    return;
                oauth_probe_done_result_(supported);
            });
    };
    if (run_async_)
        run_async_(std::move(body));
    else
        std::thread(std::move(body)).detach();
}

void LoginView::hs_changed_(const std::string& text)
{
    // An empty field shows the "matrix.org" placeholder (see finish_init()),
    // so treat it as if the user had typed that — discovery still runs and
    // confirms it, rather than leaving the field looking unvalidated.
    const std::string effective = text.empty() ? default_homeserver_ : text;

    uint32_t gen = ++discovery_gen_;
    // A different server: an Enter pressed for the old one no longer applies.
    submit_pending_    = false;
    oauth_probe_done_  = false;
    discovery_settled_ = false;
    if (submit_status_shown_)
    {
        set_status("");
        submit_status_shown_ = false;
    }
    set_discovery_state(DiscoveryState::Discovering);
    update_oauth_availability_(true); // fresh cycle: reset to permissive default
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    update_password_availability_(false); // unknown while still probing — stay hidden
#endif
    update_method_controls_enabled_();
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
    auto body = [this, gen, snap, effective]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (gen != discovery_gen_.load())
            return;
        auto result = snap->discover_homeserver(effective);
        if (gen != discovery_gen_.load())
            return;
        post_to_ui_(
            [this, gen, result = std::move(result)]
            {
                if (gen != discovery_gen_.load())
                    return;
                apply_discovery_result_(static_cast<bool>(result),
                                        result ? result.base_url : result.error,
                                        result && result.supports_password);
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

void LoginView::update_oauth_availability_(bool available)
{
    oauth_available_ = available;
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    update_form_visibility_();
#else
    if (sign_in_btn_)
        sign_in_btn_->set_visible(state_ == State::Form && oauth_available_);
#endif
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

    if (hs_input_label_) hs_input_label_->set_visible(oauth_form_visible);
    if (hs_field_)       hs_field_->set_visible(oauth_form_visible);
    if (hs_help_btn_)    hs_help_btn_->set_visible(oauth_form_visible);
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    if (find_server_btn_)
        find_server_btn_->set_visible(!provisioned_ && oauth_form_visible);
#endif
    if (discovery_icon_lbl_)
        discovery_icon_lbl_->set_visible(oauth_form_visible &&
                                    discovery_state_ != DiscoveryState::Idle);
    if (discovery_error_lbl_)
        discovery_error_lbl_->set_visible(oauth_form_visible &&
                                    discovery_state_ == DiscoveryState::Failed);
    if (sign_in_btn_)
        sign_in_btn_->set_visible(oauth_form_visible && oauth_available_);
    if (password_toggle_btn_)
        password_toggle_btn_->set_visible(oauth_form_visible && password_available_);
    if (register_link_)
        register_link_->set_visible(oauth_form_visible && registration_supported_);

    if (username_input_label_) username_input_label_->set_visible(password_form_visible);
    if (username_field_)       username_field_->set_visible(password_form_visible);
    if (password_input_label_) password_input_label_->set_visible(password_form_visible);
    if (password_field_)       password_field_->set_visible(password_form_visible);
    if (password_sign_in_btn_) password_sign_in_btn_->set_visible(password_form_visible);
    if (back_btn_)              back_btn_->set_visible(password_form_visible);

    // Cancel button: AddAccount mode shows it, except while a password login
    // is in flight — that call has no cancellation support (see
    // submit_password_), so leaving Cancel clickable would block the caller
    // inside join_worker_() for up to the HTTP client's timeout.
    if (cancel_btn_)
        cancel_btn_->set_visible(
            (mode_ == Mode::AddAccount || state_ == State::Waiting) &&
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
        hs = default_homeserver_; // empty field shows the placeholder default

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

#if TESSERACT_HAS_BRAND_ICON
    if (!brand_icon_)
    {
        brand_icon_ = ctx.factory.decode_image(
            std::span<const std::uint8_t>(kBrandIconPng, sizeof(kBrandIconPng)));
        if (brand_icon_ && brand_avatar_)
            brand_avatar_->set_image(brand_icon_.get());
    }
#endif

    tk::Size card_size = card_->measure(ctx, {kLoginCardWidth, bounds.h});
    float    card_w    = std::min(kLoginCardWidth, bounds.w);
    float    card_h    = std::min(card_size.h, bounds.h);

    // Center using the height the card would have if every row that can
    // toggle on its own (an async probe resolving, a sign-in error) were
    // also visible right now — not the actual, possibly-smaller current
    // height. That keeps the card's top edge fixed as those rows come and
    // go; only the bottom edge moves. Rows behind a mode/state that's fixed
    // for this view's whole lifetime (Cancel's AddAccount-only visibility)
    // are excluded — reserving space for those would just waste it forever
    // rather than absorb a real, in-session layout change. Likewise the
    // legacy password-only screen is a deliberate navigation via its own
    // toggle button, not an incidental reveal, so its fields stay out too.
    float max_card_h = card_h;
    tk::Size inner_constraints{kLoginCardWidth - 2.0f * kLoginCardPadding, bounds.h};
    auto reserve_if_hidden = [&](tk::Widget* w)
    {
        if (w && !w->visible())
            max_card_h += w->measure(ctx, inner_constraints).h + kLoginCardSpacing;
    };
    reserve_if_hidden(discovery_error_lbl_);
    reserve_if_hidden(sign_in_btn_);
    reserve_if_hidden(register_link_);
    reserve_if_hidden(status_lbl_);
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    reserve_if_hidden(password_toggle_btn_);
#endif
    max_card_h = std::min(max_card_h, bounds.h);

    float card_x = bounds.x + (bounds.w - card_w) * 0.5f;
    float card_y = bounds.y + (bounds.h - max_card_h) * 0.5f;
    card_->arrange(ctx, {card_x, card_y, card_w, card_h});

    if (settings_btn_)
    {
        constexpr float kSettingsBtnSize = 36.0f;
        constexpr float kSettingsBtnInset = 12.0f;
        settings_btn_->measure(ctx, {kSettingsBtnSize, kSettingsBtnSize});
        settings_btn_->arrange(
            ctx, {bounds.x + bounds.w - kSettingsBtnSize - kSettingsBtnInset,
                  bounds.y + kSettingsBtnInset, kSettingsBtnSize,
                  kSettingsBtnSize});
    }

#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    if (server_browser_) server_browser_->arrange(ctx, bounds);
#endif
    if (alert_) alert_->arrange(ctx, bounds);
}

void LoginView::paint_before_children(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    if (!card_)
        return;

    tk::Rect cb = card_->bounds();
    ctx.canvas.fill_rounded_rect(cb, 10.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(cb, 10.0f, ctx.theme.palette.border_strong, 1.0f);

    if (hs_field_ && hs_field_->visible())
    {
        tk::Rect fr = hs_field_->bounds();
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border_strong, 1.0f);
    }

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    if (username_field_ && username_field_->visible())
    {
        tk::Rect fr = username_field_->bounds();
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border_strong, 1.0f);
    }
    if (password_field_ && password_field_->visible())
    {
        tk::Rect fr = password_field_->bounds();
        ctx.canvas.fill_rounded_rect(fr, 6.0f, ctx.theme.palette.bg);
        ctx.canvas.stroke_rounded_rect(fr, 6.0f, ctx.theme.palette.border_strong, 1.0f);
    }
#endif

    if (status_lbl_)
        status_lbl_->set_colour(
            status_is_error_ ? std::optional<tk::Color>(ctx.theme.palette.destructive)
                              : std::nullopt);
    if (discovery_icon_lbl_)
    {
        switch (discovery_state_)
        {
        case DiscoveryState::Resolved:
            discovery_icon_lbl_->set_colour(ctx.theme.palette.success);
            break;
        case DiscoveryState::Failed:
            discovery_icon_lbl_->set_colour(ctx.theme.palette.destructive);
            break;
        default:
            discovery_icon_lbl_->set_colour(std::nullopt);
            break;
        }
    }
    if (discovery_error_lbl_)
        discovery_error_lbl_->set_colour(ctx.theme.palette.destructive);

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

void LoginView::show_offline_error(std::function<void()> retry_cb)
{
    if (!alert_) return;
    AlertDialog::Options opts;
    opts.title           = tk::tr("No Internet Connection");
    opts.body            = tk::tr("Tesseract couldn't detect an internet connection. "
                                  "Check your Wi-Fi or network settings, then try again.");
    opts.primary_label   = tk::tr("Retry");
    opts.secondary_label = tk::tr("Sign In");
    alert_->open(std::move(opts),
                 std::move(retry_cb),
                 [this] { if (alert_) alert_->close(); });
}

} // namespace tesseract::views
