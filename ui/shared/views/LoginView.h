#pragma once

// Shared LoginView — visual skeleton and OAuth controller for all four
// platform hosts.  Visual layout/paint live here; platform-specific
// operations (posting to the UI thread, triggering relayout, notifying
// callers of success/cancel) are injected via std::function hooks set by
// the host before calling finish_init().

#include "AlertDialog.h"
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
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
public:
    explicit LoginView(tk::Host& host);

    // -----------------------------------------------------------------------
    // Visual state
    // -----------------------------------------------------------------------

    enum class State
    {
        Form,
        Waiting
    };

    /// `Initial` — first login on a fresh install; Cancel is hidden.
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

    /// Fired on the UI thread after cancel() finishes and state is reset.
    void set_on_cancel_done(std::function<void()> fn)
    {
        on_cancel_done_ = std::move(fn);
    }

    /// Sets placeholder/initial text on the (already-constructed, tree-owned)
    /// homeserver field and wires it to the OAuth controller. Must be called
    /// after all set_* above.
    void finish_init();

    /// Win32 insets the native EDIT 1 px inside the shared rect for a snug
    /// visual fit. Set before finish_init() if needed.
    void set_overlay_inset(float inset);

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
        return alive_;
    }

    // -----------------------------------------------------------------------
    // Widget tree
    // -----------------------------------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    void rebuild_tree();

    // Controller implementations
    void sign_in_();
    void start_oauth_(bool register_account);
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
    std::function<void()>                      relayout_;
    std::function<void(const std::string&)>    open_browser_;

    // Controller state
    tesseract::Client*                   client_       = nullptr;
    tk::Host&                            host_;
    std::thread                          worker_;
    std::atomic<bool>                    cancelled_{false};
    std::atomic<uint32_t>                discovery_gen_{0};
    std::shared_ptr<bool>                alive_{std::make_shared<bool>(true)};
    std::function<void()>                on_begin_oauth_;
    std::function<void()>                on_success_;
    std::function<void()>                on_cancel_done_;

    // Visual state
    State          state_          = State::Form;
    Mode           mode_           = Mode::Initial;
    DiscoveryState discovery_state_{DiscoveryState::Idle};
    std::string    resolved_base_url_;
    bool           status_is_error_ = false;

    // Alert overlay (owned via add_child, raw pointer borrowed back)
    AlertDialog* alert_          = nullptr;

    // Borrowed widget pointers (owned by card_)
    tk::VBox*      card_            = nullptr;
    tk::Label*     title_lbl_       = nullptr;
    tk::Label*     caption_lbl_     = nullptr;
    tk::Label*     hs_input_label_  = nullptr;
    tk::TextField* hs_field_        = nullptr;
    tk::Label*     discovery_lbl_   = nullptr;
    tk::Button*    sign_in_btn_     = nullptr;
    tk::Button*    cancel_btn_      = nullptr;
    tk::Button*    register_link_   = nullptr;
    tk::Label*     status_lbl_      = nullptr;

    bool                     registration_supported_ = false;
    std::atomic<uint32_t>    registration_gen_{0};

    // Permissive: defaults true (fail open) so the OAuth "Sign in" button
    // isn't hidden during Discovering/Failed/network-error states, nor
    // before the first discovery cycle completes. Only flips false once
    // discovery positively confirms (Resolved) that the homeserver's OAuth
    // metadata fetch failed. Unconditional (unlike password_available_)
    // because sign_in_btn_ exists in every build configuration.
    bool                     oauth_available_ = true;
    std::atomic<uint32_t>    oauth_gen_{0};

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

    tk::Label*     username_input_label_ = nullptr;
    tk::TextField* username_field_       = nullptr;
    tk::Label*     password_input_label_ = nullptr;
    tk::TextField* password_field_       = nullptr;
    tk::Button*    password_toggle_btn_  = nullptr; // on the OAuthOnly form
    tk::Button*    password_sign_in_btn_ = nullptr; // submit button on the Password form
    tk::Button*    back_btn_             = nullptr; // on the Password form
#endif
};

} // namespace tesseract::views
