#include "LoginView.h"

#include "tk/theme.h"

namespace gtk4
{

LoginView::LoginView()
    : surface_(std::make_unique<tk::gtk4::Surface>(tk::Theme::light()))
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
    shared_->set_open_browser(
        [surf = surface_.get(), sh = shared_](const std::string& url)
        {
            if (!tesseract::Client::open_in_browser(url))
            {
                sh->set_status("Open this URL in your browser:\n" + url);
                surf->relayout();
            }
        });

    surface_->set_on_layout([this] { shared_->position_overlay(); });
    surface_->set_root(std::move(view));
    shared_->init_with_field(surface_->host().make_text_field());
}

LoginView::~LoginView()
{
    if (shared_)
        shared_->shutdown();
}

GtkWidget* LoginView::widget() const
{
    return surface_ ? surface_->widget() : nullptr;
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

void LoginView::reset()
{
    if (shared_)
        shared_->reset();
}

void LoginView::set_status_message(const std::string& msg)
{
    if (shared_)
        shared_->set_status_message(msg);
}

void LoginView::show_restore_error(const std::string& body,
                                   std::function<void()> retry_cb)
{
    if (shared_)
        shared_->show_restore_error(body, std::move(retry_cb));
}

} // namespace gtk4
