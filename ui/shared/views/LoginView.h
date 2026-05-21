#pragma once

// Shared LoginView — visual skeleton and OAuth controller for all four
// platform hosts.  Visual layout/paint live here; platform-specific
// operations (posting to the UI thread, triggering relayout, notifying
// callers of success/cancel) are injected via std::function hooks set by
// the host before calling init_with_field().

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/layout.h"
#include "tk/widget.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
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
    LoginView();

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

    void set_status(std::string message, std::optional<tk::Color> colour = {});

    void set_homeserver_label(std::string url);
    const std::string& homeserver_label() const
    {
        return homeserver_label_;
    }

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

    /// Rect in widget-local coordinates for the host's native text overlay.
    tk::Rect homeserver_field_rect() const
    {
        return homeserver_field_rect_;
    }

    // -----------------------------------------------------------------------
    // Controller wiring — call before init_with_field()
    // -----------------------------------------------------------------------

    void set_client(tesseract::Client* c)
    {
        client_ = c;
    }

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

    /// Takes ownership of the native homeserver field; sets placeholder/text,
    /// wires button → OAuth callbacks.  Must be called after all set_* above.
    void init_with_field(std::unique_ptr<tk::NativeTextField> field);

    /// Win32 insets the native EDIT 1 px inside the shared rect for a
    /// snug visual fit.  Set before init_with_field() if needed.
    void set_overlay_inset(float inset)
    {
        overlay_inset_ = inset;
    }

    /// Reposition the native field over homeserver_field_rect().
    /// Called by the host's set_on_layout callback on every layout pass.
    void position_overlay();

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
    void hs_changed_(const std::string& text);
    void begin_completed_(bool ok, std::string url);
    void await_completed_(bool ok, std::string err);
    void cancel_();
    void join_worker_();

    // Injected platform hooks
    std::function<void(std::function<void()>)> post_to_ui_;
    std::function<void(std::function<void()>)> run_async_;
    std::function<void()>                      relayout_;
    std::function<void(const std::string&)>    open_browser_;

    // Controller state
    tesseract::Client*                   client_       = nullptr;
    std::unique_ptr<tk::NativeTextField> hs_field_;
    float                                overlay_inset_{0.0f};
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
    std::string    homeserver_label_{"matrix.org"};
    DiscoveryState discovery_state_{DiscoveryState::Idle};
    std::string    resolved_base_url_;

    // Borrowed widget pointers (owned by card_)
    tk::VBox*   card_            = nullptr;
    tk::Label*  title_lbl_       = nullptr;
    tk::Label*  caption_lbl_     = nullptr;
    tk::Label*  hs_input_label_  = nullptr;
    tk::Label*  hs_field_lbl_    = nullptr;
    tk::Label*  discovery_lbl_   = nullptr;
    tk::Button* sign_in_btn_     = nullptr;
    tk::Button* cancel_btn_      = nullptr;
    tk::Label*  status_lbl_      = nullptr;

    tk::Rect homeserver_field_rect_{};
};

} // namespace tesseract::views
