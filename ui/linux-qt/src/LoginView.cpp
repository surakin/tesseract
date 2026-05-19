#include "LoginView.h"

#include <QDesktopServices>
#include <QResizeEvent>
#include <QUrl>

#include "tk/theme.h"

namespace qt6
{

LoginView::LoginView(QWidget* parent)
    : QWidget(parent), surface_(new tk::qt6::Surface(tk::Theme::light(), this))
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
    shared_->set_on_success([this] { emit loginSucceeded(); });
    shared_->set_on_cancel_done([this] { emit loginCancelled(); });
    shared_->set_open_browser(
        [](const std::string& url)
        {
            if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(url))))
                tesseract::Client::open_in_browser(url);
        });

    surface_->set_on_layout([this] { layout_overlays(); });
    surface_->set_root(std::move(view));
    shared_->init_with_field(surface_->host().make_text_field());
}

LoginView::~LoginView()
{
    if (shared_)
        shared_->shutdown();
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

void LoginView::reset()
{
    if (shared_)
        shared_->reset();
}

void LoginView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (surface_)
        surface_->setGeometry(0, 0, width(), height());
}

void LoginView::layout_overlays()
{
    if (shared_)
        shared_->position_overlay();
}

} // namespace qt6
