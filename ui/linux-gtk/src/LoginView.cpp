#include "LoginView.h"

#include "tk/theme.h"

namespace gtk4 {

LoginView::LoginView(tesseract::Client& client)
    : client_(client),
      surface_(std::make_unique<tk::gtk4::Surface>(tk::Theme::light()))
{
    auto shared_view = std::make_unique<tesseract::views::LoginView>();
    shared_ = shared_view.get();
    shared_->on_sign_in = [this] { on_sign_in(); };
    shared_->on_cancel  = [this] { on_cancel();  };
    surface_->set_root(std::move(shared_view));

    hs_field_ = surface_->host().make_text_field();
    hs_field_->set_placeholder("e.g. matrix.org");
    hs_field_->set_text("matrix.org");
    hs_field_->set_on_submit([this] { on_sign_in(); });

    // Keep the GtkEntry aligned with the shared LoginView's
    // homeserver_field_rect across every layout pass.
    surface_->set_on_layout([this] { position_overlay(); });
}

LoginView::~LoginView() {
    cancelled_.store(true);
    client_.cancel_oauth();
    join_worker();
}

GtkWidget* LoginView::widget() const {
    return surface_ ? surface_->widget() : nullptr;
}

void LoginView::position_overlay() {
    if (!shared_ || !hs_field_) return;
    hs_field_->set_rect(shared_->homeserver_field_rect());
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
}

void LoginView::set_status_message(const std::string& msg) {
    if (!shared_) return;
    if (msg.empty()) {
        shared_->set_status("");
    } else {
        // Plain informational message (e.g. "Saved session expired") —
        // not an error from this attempt. The default theme colour is
        // primary text, which reads neutral.
        shared_->set_status(msg);
    }
    surface_->relayout();
}

void LoginView::on_sign_in() {
    std::string hs = trim(hs_field_->text());
    if (hs.empty()) {
        shared_->set_status("Please enter a homeserver.",
                             tk::Color::rgb(0xB00020));
        surface_->relayout();
        return;
    }
    shared_->set_status("");
    hs_field_->set_enabled(false);
    shared_->set_state(tesseract::views::LoginView::State::Waiting);
    surface_->relayout();

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
        surface_->relayout();
        return;
    }

    if (!tesseract::Client::open_in_browser(err_or_url)) {
        // Browser launch failed; show the URL so the user can copy it.
        shared_->set_status("Open this URL in your browser:\n" + err_or_url);
        surface_->relayout();
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
        if (on_success_) on_success_();
        return;
    }
    shared_->set_status("Sign-in failed: " + err,
                         tk::Color::rgb(0xB00020));
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
}

void LoginView::on_cancel() {
    cancelled_.store(true);
    client_.cancel_oauth();
    shared_->set_status("Cancelling…");
    surface_->relayout();
    join_worker();
    shared_->set_status("");
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
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

} // namespace gtk4
