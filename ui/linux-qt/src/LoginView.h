#pragma once
#include <QString>
#include <QWidget>

#include <tesseract/client.h>

#include <functional>

#include "tk/host_qt.h"
#include "views/LoginView.h"

namespace qt6
{

/// Sign-in view shown inside the main window when the user is not logged in.
/// Visuals are rendered by the shared `tesseract::views::LoginView` mounted
/// inside a `tk::qt6::Surface`. Controller logic (OAuth state machine, worker
/// threads, homeserver discovery) lives in the shared view; this shell only
/// wires the platform-specific hooks and forwards public API calls.
class LoginView final : public QWidget
{
    Q_OBJECT
public:
    explicit LoginView(QWidget* parent = nullptr);
    ~LoginView() override;

    void set_client(tesseract::Client* c);
    void set_mode(tesseract::views::LoginView::Mode m);
    void set_theme(const tk::Theme& t);
    void set_on_begin_oauth(std::function<void()> cb);
    void set_run_async(std::function<void(std::function<void()>)> fn);
    void reset();

signals:
    void loginSucceeded();
    void loginCancelled();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void layout_overlays();

    tk::qt6::Surface*             surface_ = nullptr;
    tesseract::views::LoginView*  shared_  = nullptr; // borrowed from surface
};

} // namespace qt6
