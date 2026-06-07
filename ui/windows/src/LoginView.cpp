#include "LoginView.h"

#include "tk/theme.h"

namespace win32
{

LoginView::LoginView(HINSTANCE hInst, HWND hParent)
    : surface_(std::make_unique<tk::win32::Surface>(hInst, hParent,
                                                    tk::Theme::light()))
{
    auto view = std::make_unique<tesseract::views::LoginView>();
    shared_   = view.get();

    std::weak_ptr<bool> w = shared_->alive_token();
    shared_->set_post_to_ui(
        [this, w](std::function<void()> fn)
        {
            surface_->host().post_to_ui(
                [w, fn = std::move(fn)]
                {
                    if (!w.expired())
                        fn();
                });
        });
    shared_->set_relayout([this] { surface_->relayout(); });

    // Win32 insets the native EDIT 1 px inside the shared rect for a snug fit.
    shared_->set_overlay_inset(1.0f);

    surface_->set_on_layout([this] { shared_->position_overlay(); });
    surface_->set_root(std::move(view));
    shared_->init_with_field(surface_->host().make_text_field());
}

LoginView::~LoginView()
{
    if (shared_)
        shared_->shutdown();
}

HWND LoginView::hwnd() const
{
    return surface_ ? surface_->hwnd() : nullptr;
}

void LoginView::layout(int w, int h)
{
    if (!surface_)
        return;
    SetWindowPos(surface_->hwnd(), nullptr, 0, 0, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    // WM_SIZE inside the Surface drives relayout; the EDIT is
    // repositioned via the on_layout callback set in the constructor.
}

void LoginView::set_client(tesseract::Client* c)
{
    if (shared_)
        shared_->set_client(c);
}

void LoginView::set_theme(const tk::Theme& t)
{
    if (surface_)
        surface_->set_theme(t);
}

void LoginView::set_mode(tesseract::views::LoginView::Mode m)
{
    if (shared_)
        shared_->set_mode(m);
}

void LoginView::reset()
{
    if (shared_)
        shared_->reset();
}

void LoginView::set_status_message(const std::wstring& msg)
{
    if (!shared_)
        return;
    shared_->set_status_message(msg.empty() ? std::string{} : wstring_to_utf8(msg));
}

void LoginView::show_restore_error(const std::string& body,
                                   std::function<void()> retry_cb)
{
    if (shared_)
        shared_->show_restore_error(body, std::move(retry_cb));
}

void LoginView::set_on_begin_oauth(std::function<void()> cb)
{
    if (shared_)
        shared_->set_on_begin_oauth(std::move(cb));
}

void LoginView::set_on_success(std::function<void()> cb)
{
    if (shared_)
        shared_->set_on_success(std::move(cb));
}

void LoginView::set_on_cancel(std::function<void()> cb)
{
    if (shared_)
        shared_->set_on_cancel_done(std::move(cb));
}

void LoginView::set_run_async(std::function<void(std::function<void()>)> fn)
{
    if (shared_)
        shared_->set_run_async(std::move(fn));
}

std::string LoginView::wstring_to_utf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace win32
