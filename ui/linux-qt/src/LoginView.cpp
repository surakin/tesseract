#include "LoginView.h"

#include <QDesktopServices>
#include <QResizeEvent>
#include <QUrl>

#include "tk/theme.h"

namespace qt6 {

LoginView::LoginView(tesseract::Client& client, QWidget* parent)
    : QWidget(parent),
      client_(client),
      surface_(new tk::qt6::Surface(tk::Theme::light(), this))
{
    // Build the shared widget tree and mount it as Surface's root.
    auto shared_view = std::make_unique<tesseract::views::LoginView>();
    shared_ = shared_view.get();
    shared_->on_sign_in = [this] { on_sign_in(); };
    shared_->on_cancel  = [this] { on_cancel();  };
    surface_->set_root(std::move(shared_view));

    // Native QLineEdit overlay for the homeserver input. The shared
    // view paints a styled box behind it; we position the real control
    // on top in layout_overlays().
    hs_field_ = surface_->host().make_text_field();
    hs_field_->set_placeholder("e.g. matrix.org");
    hs_field_->set_text("matrix.org");
    hs_field_->set_on_submit([this] { on_sign_in(); });
}

LoginView::~LoginView() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
}

void LoginView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (surface_) surface_->setGeometry(0, 0, width(), height());
    layout_overlays();
}

void LoginView::layout_overlays() {
    if (!shared_ || !hs_field_) return;
    tk::Rect fr = shared_->homeserver_field_rect();
    hs_field_->set_rect(fr);
}

// ---------------------------------------------------------------------------

void LoginView::reset() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
    cancelled_.store(false);

    shared_->set_status("");
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    hs_field_->set_visible(true);
    hs_field_->set_focused(true);
    surface_->relayout();
    layout_overlays();
}

void LoginView::on_sign_in() {
    std::string hs = trim(hs_field_->text());
    if (hs.empty()) {
        shared_->set_status("Please enter a homeserver.",
                             tk::Color::rgb(0xB00020));
        surface_->update();
        return;
    }
    shared_->set_status("");
    hs_field_->set_enabled(false);
    shared_->set_state(tesseract::views::LoginView::State::Waiting);
    surface_->relayout();
    layout_overlays();

    join_worker();
    cancelled_.store(false);
    worker_ = std::thread([this, hs] {
        auto flow = client_.begin_oauth(hs);
        if (cancelled_.load()) return;
        bool        ok      = static_cast<bool>(flow);
        std::string payload = ok ? flow.auth_url : flow.message;
        surface_->host().post_to_ui(
            [this, ok, payload = std::move(payload)] {
                on_begin_completed(ok, payload);
            });
    });
}

void LoginView::on_begin_completed(bool ok, std::string err_or_url) {
    join_worker();
    if (!ok) {
        shared_->set_status("Sign-in failed: " + err_or_url,
                             tk::Color::rgb(0xB00020));
        shared_->set_state(tesseract::views::LoginView::State::Form);
        hs_field_->set_enabled(true);
        layout_overlays();
        surface_->update();
        return;
    }

    if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(err_or_url)))) {
        tesseract::Client::open_in_browser(err_or_url);
    }

    cancelled_.store(false);
    worker_ = std::thread([this] {
        auto res = client_.await_oauth();
        if (cancelled_.load()) return;
        bool        ok  = static_cast<bool>(res);
        std::string msg = res.message;
        surface_->host().post_to_ui(
            [this, ok, msg = std::move(msg)] {
                on_await_completed(ok, msg);
            });
    });
}

void LoginView::on_await_completed(bool ok, std::string err) {
    join_worker();
    if (ok) {
        emit loginSucceeded();
        return;
    }
    shared_->set_status("Sign-in failed: " + err,
                         tk::Color::rgb(0xB00020));
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
    layout_overlays();
}

void LoginView::on_cancel() {
    cancelled_.store(true);
    client_.cancel_oauth();
    shared_->set_status("Cancelling…");
    surface_->update();
    join_worker();
    shared_->set_status("");
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
    layout_overlays();
}

void LoginView::join_worker() {
    if (worker_.joinable()) worker_.join();
}

std::string LoginView::trim(std::string s) {
    auto a = s.find_first_not_of(" \t\n\r");
    auto b = s.find_last_not_of (" \t\n\r");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

} // namespace qt6
