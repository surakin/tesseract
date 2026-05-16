#pragma once
#include <QString>
#include <QWidget>

#include <tesseract/client.h>

#include <atomic>
#include <memory>
#include <thread>

#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/LoginView.h"

namespace qt6 {

/// Sign-in view shown inside the main window when the user is not logged
/// in. The OAuth state machine + worker threads live in this shell; the
/// actual visuals are rendered by the shared `tesseract::views::LoginView`
/// mounted inside a `tk::qt6::Surface` child. The homeserver text input
/// is a native QLineEdit overlaid on the surface via NativeTextField —
/// IME / selection stay native until tk::TextField lands.
class LoginView final : public QWidget {
    Q_OBJECT
public:
    explicit LoginView(QWidget* parent = nullptr);
    ~LoginView() override;

    /// Rebind the target `tesseract::Client` that this view drives through
    /// OAuth. Called by `MainWindow` before showing the view: a fresh
    /// `Client` (with its own data dir) is created for each login attempt
    /// — initial or "Add Account" — so each account ends up with its own
    /// matrix-sdk store. Must be set before the user clicks Sign In.
    void set_client(tesseract::Client* client);

    /// Toggle between initial-login and add-account presentation. Forwards
    /// to the shared widget's `Mode` and (re)wires the visible buttons:
    /// `Initial` hides Cancel; `AddAccount` shows it in both Form and
    /// Waiting states so the user can back out at any time.
    void set_mode(tesseract::views::LoginView::Mode m);

    /// Called on the UI thread just before the OAuth worker thread starts.
    /// Used by MainWindow to lazily create the pending account directory and
    /// call set_data_dir() only when the user actually initiates login.
    void set_on_begin_oauth(std::function<void()> cb) { on_begin_oauth_ = std::move(cb); }

    /// Return the view to its initial "form" state. Call before showing
    /// the view again after a successful sign-in or logout.
    void reset();

signals:
    void loginSucceeded();
    void loginCancelled();   // emitted when the user clicks Cancel in AddAccount mode

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void layout_overlays();
    void on_sign_in();
    void on_cancel();
    void on_begin_completed(bool ok, std::string err_or_url);
    void on_await_completed(bool ok, std::string err);
    void on_hs_text_changed(const std::string& text);
    void join_worker();

    static std::string trim(std::string s);

    tesseract::Client* client_ = nullptr;   // non-owning; rebound per login attempt by `set_client`
    std::function<void()>                    on_begin_oauth_;

    tk::qt6::Surface*                       surface_  = nullptr;
    tesseract::views::LoginView*            shared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>    hs_field_;

    std::thread                 worker_;
    std::atomic<bool>           cancelled_{ false };
    std::atomic<uint32_t>       discovery_gen_{ 0 };
};

} // namespace qt6
