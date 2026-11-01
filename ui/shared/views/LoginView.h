#pragma once

// Shared LoginView — visual skeleton and OAuth controller for all four
// platform hosts.  Visual layout/paint live here; platform-specific
// operations (posting to the UI thread, triggering relayout, notifying
// callers of success/cancel) are injected via std::function hooks set by
// the host before calling finish_init().

#include "AlertDialog.h"
#include "ServerBrowserOverlay.h" // empty unless TESSERACT_SERVER_BROWSER_ENABLED
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/media_disk_cache.h"
#include "tk/host.h"
#include "tk/image_view.h"
#include "tk/layout.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace tesseract
{
class Client; // forward declaration — host includes the full header
}

namespace tesseract::views
{

class LoginView : public tk::Widget
{
protected:
    LoginView();
    TK_WIDGET_FACTORY_FRIEND(LoginView)

public:
    ~LoginView() override
    {
        // A joinable worker_ would std::terminate the process.
        if (worker_.joinable())
        {
            shutdown();
        }
        invalidate_weak_self();
    }

    // -----------------------------------------------------------------------
    // Visual state
    // -----------------------------------------------------------------------

    enum class State
    {
        Form,
        Waiting
    };

    /// `Initial` — first login on a fresh install; Cancel is hidden on the
    /// form but shown while State::Waiting (so a user stuck on the browser
    /// step can abort and retry with a different homeserver).
    /// `AddAccount` — adding a second/Nth account; Cancel is always visible.
    enum class Mode
    {
        Initial,
        AddAccount
    };

    void set_state(State s);
    State state() const
    {
        return state_;
    }

    void set_mode(Mode m);
    Mode mode() const
    {
        return mode_;
    }

    bool cancel_visible() const;

    bool sign_in_visible() const
    {
        return sign_in_btn_ && sign_in_btn_->visible();
    }
    // Whether the sign-in button and "Create an account" link accept clicks
    // (they don't while the server's sign-in methods are being worked out).
    bool sign_in_enabled() const
    {
        return sign_in_btn_ && sign_in_btn_->enabled();
    }
    bool register_enabled() const
    {
        return register_link_ && register_link_->enabled();
    }

    /// The corner settings button (reaches Network settings before any
    /// account exists). Shown only on the first-login form.
    bool settings_visible() const
    {
        return settings_btn_ && settings_btn_->visible();
    }
    void simulate_settings_click()
    {
        if (on_open_settings_)
        {
            on_open_settings_();
        }
    }

    void set_status(std::string message, bool is_error = false);

    enum class DiscoveryState
    {
        Idle,
        Discovering,
        Resolved,
        Failed
    };

    void set_discovery_state(DiscoveryState s, std::string detail = "");
    DiscoveryState discovery_state() const
    {
        return discovery_state_;
    }
    const std::string& resolved_base_url() const
    {
        return resolved_base_url_;
    }

    /// What Enter in the homeserver field does right now: the form's main
    /// action for this server. `Wait` while the server is still being
    /// checked (the Enter is remembered and acted on once it's known).
    enum class SubmitAction
    {
        Wait,
        OAuth,
        Password,
        Nothing
    };
    SubmitAction homeserver_submit_action() const;
    bool homeserver_submit_pending() const
    {
        return submit_pending_;
    }

    // Test helpers: press Enter in the homeserver field; report the OAuth
    // metadata probe's answer (as probe_oauth_support_ would).
    void simulate_homeserver_enter()
    {
        submit_homeserver_();
    }
    void simulate_oauth_probe_result(bool supported)
    {
        oauth_probe_done_result_(supported);
    }
    // Test helper: the homeserver lookup's answer (as hs_changed_'s worker
    // reports it).
    void simulate_discovery_result(bool ok, const std::string& base_url_or_error,
                                   bool supports_password)
    {
        apply_discovery_result_(ok, base_url_or_error, supports_password);
    }

    // -----------------------------------------------------------------------
    // Controller wiring — call before finish_init()
    // -----------------------------------------------------------------------

    // Lifetime invariant: the Client is owned by the shell, not by LoginView
    // (client_ is a non-owning alias). The OAuth worker thread snapshots
    // `auto* c = client_` and calls blocking FFI (begin_oauth / await_oauth)
    // on it; that raw pointer stays valid only while the Client object lives.
    // OAuth is modal — the shell keeps the Client alive for the whole flow and
    // does not destroy it mid-flow. If a flow is in flight when the alias is
    // swapped or cleared, tear it down first (mark cancelled, cancel_oauth to
    // unblock the in-flight FFI call, join the worker so it has fully returned
    // and stopped touching the old client_) before changing client_. Defined
    // out-of-line in LoginView.cpp because it touches the worker machinery.
    void set_client(tesseract::Client* c);

    /// post_to_ui: schedule fn on the UI thread.  Implementations should
    /// wrap the callback with the liveness guard from alive_token().
    void set_post_to_ui(std::function<void(std::function<void()>)> fn)
    {
        post_to_ui_ = std::move(fn);
    }

    /// run_async: spawn a background worker that participates in the host's
    /// shutdown drain.  Used by the homeserver-discovery debounce so a
    /// blocking `discover_homeserver` call can't outlive the LoginView and
    /// then post back into a freed widget.  Hosts should pass a thunk that
    /// forwards to `ShellBase::run_async_`.  When unset, discovery falls
    /// back to a detached std::thread (legacy behaviour).
    void set_run_async(std::function<void(std::function<void()>)> fn)
    {
        run_async_ = std::move(fn);
    }

    /// relayout: call surface->relayout().
    void set_relayout(std::function<void()> fn)
    {
        relayout_ = std::move(fn);
    }

    /// open_browser: open a URL in the system browser.
    /// Defaults to Client::open_in_browser when not set.
    void set_open_browser(std::function<void(const std::string&)> fn)
    {
        open_browser_ = std::move(fn);
    }

    void set_on_begin_oauth(std::function<void()> fn)
    {
        on_begin_oauth_ = std::move(fn);
    }

    /// Fired on the UI thread when OAuth completes successfully.
    void set_on_success(std::function<void()> fn)
    {
        on_success_ = std::move(fn);
    }

    /// Fired on the UI thread when the corner settings button is clicked.
    void set_on_open_settings(std::function<void()> fn)
    {
        on_open_settings_ = std::move(fn);
    }

    /// Fired on the UI thread after cancel() finishes and state is reset.
    void set_on_cancel_done(std::function<void()> fn)
    {
        on_cancel_done_ = std::move(fn);
    }

#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    /// Opens the "Find a server" browser (loading the community list first if
    /// it hasn't arrived yet).
    void open_server_browser();
    ServerBrowserOverlay* server_browser() const
    {
        return server_browser_;
    }
    /// Whether the "Find a server" affordance is offered at all: not when the
    /// deployment provisions a default homeserver.
    bool server_browser_available() const
    {
        return !provisioned_;
    }
    // Test helper: the community list's answer (as load_server_list_'s worker
    // posts it).
    void simulate_server_list_result(const tesseract::Client::ServerList& list)
    {
        apply_server_list_(list);
    }
#endif
    tk::TextField* homeserver_field() const
    {
        return hs_field_;
    }
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    tk::Button* find_server_button() const
    {
        return find_server_btn_;
    }
#endif
    tk::Button* register_button() const
    {
        return register_link_;
    }
    // Test helper: click "Create an account".
    void simulate_register_click()
    {
        on_register_clicked_();
    }

    /// Sets placeholder/initial text on the (already-constructed, tree-owned)
    /// homeserver field and wires it to the OAuth controller. Must be called
    /// after all set_* above.
    void finish_init();

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    /// Wires the username/password fields (already tree-owned) and the
    /// password-sign-in button. Call after finish_init().
    void finish_password_init();
#endif

    // -----------------------------------------------------------------------
    // Error overlay
    // -----------------------------------------------------------------------

    /// Show a "Connection Error" alert with Retry and Sign In buttons on top
    /// of the login form.  Called by the shell when session restore fails due
    /// to a network error.  `retry_cb` is invoked if the user clicks Retry.
    void show_restore_error(std::string body, std::function<void()> retry_cb);

    /// Like show_restore_error(), but for the cold-start pre-flight case: no
    /// restore was even attempted because tk::Host::is_network_available()
    /// reported no OS-level connectivity. Fixed, friendly copy — no raw
    /// error text, since there isn't a backend error to show. `retry_cb`
    /// re-runs the whole restore flow, including a fresh connectivity probe.
    void show_offline_error(std::function<void()> retry_cb);

    // -----------------------------------------------------------------------
    // Runtime control
    // -----------------------------------------------------------------------

    /// Cancel in-flight OAuth, join worker.  Call from the host's
    /// destructor/dealloc BEFORE the surface (which owns *this) tears down.
    void shutdown();

    /// Reset visual + controller state.  Cancels any in-flight flow.
    void reset();

    /// set_status(msg) + relayout.
    void set_status_message(const std::string& msg);

    /// Weak pointer to the liveness sentinel.  The host's post_to_ui hook
    /// should capture this so deferred callbacks expire safely after
    /// destruction.
    std::weak_ptr<bool> alive_token() const
    {
        return weak_flag();
    }

    /// Wraps fn so it only runs while this LoginView is still alive. Exposed
    /// publicly (guarded() is otherwise protected) so each platform host's
    /// post_to_ui hook can guard a continuation without duplicating a
    /// liveness token of its own.
    using tk::Widget::guarded;

    // -----------------------------------------------------------------------
    // Widget tree
    // -----------------------------------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;

private:
    void rebuild_tree();

    // Controller implementations
    void sign_in_();
    // Enter in the homeserver field (see homeserver_submit_action()).
    void submit_homeserver_();
    // Acts on a remembered Enter once the server check has settled.
    void run_pending_submit_();
    // probe_oauth_support_'s outcome (or its absence): records it, updates
    // the form and runs a remembered Enter.
    void oauth_probe_done_result_(bool supported);
    // The sign-in, "Create an account" and password-form controls are inert
    // while the server's sign-in methods are still being worked out (the same
    // Wait state that makes Enter defer): until then the default offered
    // method may be the wrong one.
    void update_method_controls_enabled_();
    // The homeserver lookup finished: record what the server offers and run
    // a remembered Enter if nothing else is outstanding.
    void apply_discovery_result_(bool ok, const std::string& base_url_or_error,
                                 bool supports_password);
    void start_oauth_(bool register_account);
    // "Create an account": with an empty field (and no provisioned server)
    // there is no server to register on yet, so offer the browser instead.
    void on_register_clicked_();
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    void load_server_list_();
    void apply_server_list_(const tesseract::Client::ServerList& list);
    // Fetches the favicons the browser doesn't have yet (disk cache first,
    // then each server's site), a few at a time on the worker pool. Only runs
    // while the browser is open, so nothing contacts the listed sites before
    // the user asks to see them.
    void request_server_icons_();
#endif
    void probe_registration_support_(const std::string& base_url);
    void probe_oauth_support_(const std::string& base_url);
    void update_oauth_availability_(bool available);
    void hs_changed_(const std::string& text);
    void begin_completed_(bool ok, std::string url);
    void await_completed_(bool ok, std::string err);
    void cancel_();
    void join_worker_();

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    void submit_password_();
    void password_login_completed_(bool ok, std::string err);
    void update_password_availability_(bool supported);
    void switch_to_password_form_();
    void switch_to_oauth_form_();
    void update_form_visibility_();
#endif

    // Injected platform hooks
    std::function<void(std::function<void()>)> post_to_ui_;
    std::function<void(std::function<void()>)> run_async_;
    std::function<void()> relayout_;
    std::function<void(const std::string&)> open_browser_;

    // Controller state
    tesseract::Client* client_ = nullptr;
    std::thread worker_;
    std::atomic<bool> cancelled_{false};
    std::atomic<uint32_t> discovery_gen_{0};
    // Server an empty homeserver field stands for: the provisioned one
    // (tesseract/provisioning.h) or "matrix.org".
    std::string default_homeserver_;
    std::function<void()> on_begin_oauth_;
    std::function<void()> on_success_;
    std::function<void()> on_cancel_done_;
    std::function<void()> on_open_settings_;

    // Visual state
    State state_ = State::Form;
    Mode mode_ = Mode::Initial;
    DiscoveryState discovery_state_{DiscoveryState::Idle};
    std::string resolved_base_url_;
    bool status_is_error_ = false;

    // Alert overlay (owned via add_child, raw pointer borrowed back)
    AlertDialog* alert_ = nullptr;
#ifdef TESSERACT_SERVER_BROWSER_ENABLED
    // "Find a server" browser, added before alert_ so alerts paint above it.
    ServerBrowserOverlay* server_browser_ = nullptr;
    tk::Button* find_server_btn_ = nullptr;
    bool server_list_loading_ = false;
    // hs_field_ was visible when the browser opened and is restored on close.
    bool hs_field_hidden_by_browser_ = false;
    std::atomic<uint32_t> server_list_gen_{0};
    // (client_domain, homepage) of the listed servers, for request_server_icons_.
    std::vector<std::pair<std::string, std::string>> server_icon_targets_;
    std::shared_ptr<tk::MediaDiskCache> icon_cache_; // the shared media cache dir
    bool icons_fetching_ = false;
    std::atomic<uint32_t> icon_gen_{0};
#endif
    // A deployment-provisioned default homeserver replaces the picker.
    bool provisioned_ = false;

    // Borrowed widget pointers (owned by card_)
    tk::VBox* card_ = nullptr;
    tk::Avatar* brand_avatar_ = nullptr;
    tk::Label* title_lbl_ = nullptr;
    tk::Label* hs_input_label_ = nullptr;
    tk::TextField* hs_field_ = nullptr;
    // Small ✓/✗/"Checking…" indicator next to hs_field_ — see set_discovery_state().
    tk::Label* discovery_icon_lbl_ = nullptr;
    tk::Button* hs_help_btn_ = nullptr;
    // Error detail, shown below the field only on DiscoveryState::Failed —
    // a valid/pending result shows no text at all, just the icon above.
    tk::Label* discovery_error_lbl_ = nullptr;
    tk::Button* sign_in_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;
    tk::Button* settings_btn_ = nullptr;
    void update_settings_visibility_();
    tk::Button* register_link_ = nullptr;
    tk::Label* status_lbl_ = nullptr;

    // Decoded once, lazily, in arrange() — see BrandView's identical pattern.
    std::unique_ptr<tk::Image> brand_icon_;

    bool registration_supported_ = false;
    std::atomic<uint32_t> registration_gen_{0};

    // Permissive: defaults true (fail open) so the OAuth "Sign in" button
    // isn't hidden during Discovering/Failed/network-error states, nor
    // before the first discovery cycle completes. Only flips false once
    // discovery positively confirms (Resolved) that the homeserver's OAuth
    // metadata fetch failed. Unconditional (unlike password_available_)
    // because sign_in_btn_ exists in every build configuration.
    bool oauth_available_ = true;
    std::atomic<uint32_t> oauth_gen_{0};
    // Whether this discovery cycle's OAuth probe has answered: until then,
    // oauth_available_ is only the permissive default.
    bool oauth_probe_done_ = false;
    // Enter was pressed in the homeserver field while the server was still
    // being checked.
    bool submit_pending_ = false;
    // The status line shows submit_homeserver_'s "Checking…" message.
    bool submit_status_shown_ = false;
    // This cycle's homeserver lookup has answered (with its sign-in methods).
    bool discovery_settled_ = false;

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    /// `OAuthOnly` — the main form (homeserver field, OAuth button, and the
    /// password-login toggle button when the server supports it). `Password`
    /// — a distinct screen with only the username/password fields, entered
    /// by clicking the toggle button and left via the Back button.
    enum class FormKind
    {
        OAuthOnly,
        Password
    };

    FormKind form_kind_ = FormKind::OAuthOnly;

    // Strict gating: starts false, and only flips true once discovery
    // positively confirms (Resolved) that the homeserver advertises
    // m.login.password. Drives whether password_toggle_btn_ is offered at
    // all — no permissive/inconclusive fallback (see update_form_visibility_).
    bool password_available_ = false;

    tk::Label* username_input_label_ = nullptr;
    tk::TextField* username_field_ = nullptr;
    tk::Label* password_input_label_ = nullptr;
    tk::TextField* password_field_ = nullptr;
    tk::Button* password_toggle_btn_ = nullptr;  // on the OAuthOnly form
    tk::Button* password_sign_in_btn_ = nullptr; // submit button on the Password form
    tk::Button* back_btn_ = nullptr;             // on the Password form
#endif
};

} // namespace tesseract::views
