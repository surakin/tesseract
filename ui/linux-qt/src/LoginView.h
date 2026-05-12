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
    explicit LoginView(tesseract::Client& client, QWidget* parent = nullptr);
    ~LoginView() override;

    /// Return the view to its initial "form" state. Call before showing
    /// the view again after a successful sign-in or logout.
    void reset();

signals:
    void loginSucceeded();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void layout_overlays();
    void on_sign_in();
    void on_cancel();
    void on_begin_completed(bool ok, std::string err_or_url);
    void on_await_completed(bool ok, std::string err);
    void join_worker();

    static std::string trim(std::string s);

    tesseract::Client& client_;

    tk::qt6::Surface*                       surface_  = nullptr;
    tesseract::views::LoginView*            shared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>    hs_field_;

    std::thread       worker_;
    std::atomic<bool> cancelled_{ false };
};

} // namespace qt6
