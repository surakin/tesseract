#include "LoginView.h"

#include "tk/theme.h"

namespace win32 {

LoginView::LoginView(HINSTANCE hInst, HWND hParent)
    : surface_(std::make_unique<tk::win32::Surface>(hInst, hParent,
                                                      tk::Theme::light()))
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

    surface_->set_on_layout([this] { position_overlay(); });
}

LoginView::~LoginView() {
    cancelled_.store(true);
    if (client_) client_->cancel_oauth();
    join_worker();
}

HWND LoginView::hwnd() const {
    return surface_ ? surface_->hwnd() : nullptr;
}

void LoginView::layout(int w, int h) {
    if (!surface_) return;
    SetWindowPos(surface_->hwnd(), nullptr, 0, 0, w, h,
                  SWP_NOZORDER | SWP_NOACTIVATE);
    // WM_SIZE inside the Surface drives relayout; the EDIT is
    // repositioned via the on_layout callback set in the constructor.
}

void LoginView::position_overlay() {
    if (!shared_ || !hs_field_) return;
    hs_field_->set_rect(shared_->homeserver_field_rect());
}

// ---------------------------------------------------------------------------

void LoginView::set_mode(tesseract::views::LoginView::Mode m) {
    if (shared_) shared_->set_mode(m);
    if (surface_) surface_->relayout();
}

void LoginView::reset() {
    cancelled_.store(true);
    if (client_) client_->cancel_oauth();
    join_worker();
    cancelled_.store(false);

    shared_->set_status("");
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    hs_field_->set_visible(true);
    hs_field_->set_focused(true);
    surface_->relayout();
}

void LoginView::set_status_message(const std::wstring& msg) {
    if (!shared_) return;
    if (msg.empty()) {
        shared_->set_status("");
    } else {
        shared_->set_status(wstring_to_utf8(msg));
    }
    surface_->relayout();
}

void LoginView::on_sign_in() {
    if (!client_) return;
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
        auto flow = client_->begin_oauth(hs);
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

    tesseract::Client::open_in_browser(err_or_url);

    cancelled_.store(false);
    worker_ = std::thread([this] {
        auto res = client_->await_oauth();
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
    if (client_) client_->cancel_oauth();
    shared_->set_status("Cancelling\xe2\x80\xa6");
    surface_->relayout();
    join_worker();
    shared_->set_status("");
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
    if (on_cancel_fn_) on_cancel_fn_();
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

std::string LoginView::wstring_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                 static_cast<int>(s.size()),
                                 nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(),
                         static_cast<int>(s.size()),
                         out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace win32
