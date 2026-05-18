#include "LoginView.h"

#include <chrono>

#include <QDesktopServices>
#include <QResizeEvent>
#include <QUrl>

#include "tk/theme.h"
#include "views/text_util.h"

namespace qt6
{

LoginView::LoginView(QWidget* parent)
    : QWidget(parent), surface_(new tk::qt6::Surface(tk::Theme::light(), this))
{
    // Build the shared widget tree and mount it as Surface's root.
    auto shared_view = std::make_unique<tesseract::views::LoginView>();
    shared_ = shared_view.get();
    shared_->on_sign_in = [this]
    {
        on_sign_in();
    };
    shared_->on_cancel = [this]
    {
        on_cancel();
    };
    surface_->set_root(std::move(shared_view));

    // Native QLineEdit overlay for the homeserver input. The shared
    // view paints a styled box behind it; we position the real control
    // on top in layout_overlays().
    hs_field_ = surface_->host().make_text_field();
    hs_field_->set_placeholder("matrix.org or @user:matrix.org");
    hs_field_->set_text("matrix.org");
    hs_field_->set_on_submit(
        [this]
        {
            on_sign_in();
        });
    hs_field_->set_on_changed(
        [this](const std::string& text)
        {
            on_hs_text_changed(text);
        });
}

LoginView::~LoginView()
{
    ++discovery_gen_;
    cancelled_.store(true);
    if (client_)
    {
        client_->cancel_oauth();
    }
    join_worker();
}

void LoginView::set_client(tesseract::Client* client)
{
    client_ = client;
}

void LoginView::set_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
    }
}

void LoginView::set_mode(tesseract::views::LoginView::Mode m)
{
    if (shared_)
    {
        shared_->set_mode(m);
    }
}

void LoginView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (surface_)
    {
        surface_->setGeometry(0, 0, width(), height());
    }
    layout_overlays();
}

void LoginView::layout_overlays()
{
    if (!shared_ || !hs_field_)
    {
        return;
    }
    tk::Rect fr = shared_->homeserver_field_rect();
    hs_field_->set_rect(fr);
}

// ---------------------------------------------------------------------------

void LoginView::reset()
{
    ++discovery_gen_; // invalidate any in-flight discovery callback
    cancelled_.store(true);
    if (client_)
    {
        client_->cancel_oauth();
    }
    join_worker();
    cancelled_.store(false);

    shared_->set_status("");
    shared_->set_discovery_state(
        tesseract::views::LoginView::DiscoveryState::Idle);
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    hs_field_->set_visible(true);
    hs_field_->set_focused(true);
    surface_->relayout();
    layout_overlays();
}

void LoginView::on_hs_text_changed(const std::string& text)
{
    if (!shared_)
    {
        return;
    }
    uint32_t gen = ++discovery_gen_;
    if (text.empty())
    {
        shared_->set_discovery_state(
            tesseract::views::LoginView::DiscoveryState::Idle);
        surface_->relayout();
        layout_overlays(); // discovery label hidden → field box moves
        return;
    }
    shared_->set_discovery_state(
        tesseract::views::LoginView::DiscoveryState::Discovering);
    surface_->relayout();
    layout_overlays(); // discovery label shown → field box moves;
                       // keep the native QLineEdit aligned with it

    auto* snap = client_;
    if (!snap)
    {
        return;
    }

    std::thread(
        [this, gen, snap, text]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (gen != discovery_gen_.load())
            {
                return;
            }
            auto result = snap->discover_homeserver(text);
            surface_->host().post_to_ui(
                [this, gen, result = std::move(result)]
                {
                    if (gen != discovery_gen_.load() || !shared_)
                    {
                        return;
                    }
                    if (result)
                    {
                        shared_->set_discovery_state(
                            tesseract::views::LoginView::DiscoveryState::
                                Resolved,
                            result.base_url);
                    }
                    else
                    {
                        shared_->set_discovery_state(
                            tesseract::views::LoginView::DiscoveryState::Failed,
                            result.error);
                    }
                    surface_->relayout();
                    layout_overlays(); // Resolved/Failed label height differs from
                                       // "Checking…" → realign the native field
                });
        })
        .detach();
}

void LoginView::on_sign_in()
{
    if (!client_)
    {
        return; // set_client() must be called before showing
    }
    std::string hs_raw = trim(hs_field_->text());
    if (hs_raw.empty())
    {
        shared_->set_status("Please enter a homeserver.",
                            tk::Color::rgb(0xB00020));
        surface_->update();
        return;
    }
    // Use the pre-resolved URL when available; extract server name from MXID
    // otherwise so begin_oauth doesn't receive a raw @user:server string.
    std::string hs;
    using DS = tesseract::views::LoginView::DiscoveryState;
    if (shared_->discovery_state() == DS::Resolved)
    {
        hs = shared_->resolved_base_url();
    }
    else if (!hs_raw.empty() && hs_raw.front() == '@')
    {
        auto colon = hs_raw.find(':');
        hs = (colon != std::string::npos) ? hs_raw.substr(colon + 1) : hs_raw;
    }
    else
    {
        hs = hs_raw;
    }
    shared_->set_status("");
    hs_field_->set_enabled(false);
    shared_->set_state(tesseract::views::LoginView::State::Waiting);
    surface_->relayout();
    layout_overlays();

    join_worker();
    cancelled_.store(false);
    if (on_begin_oauth_)
    {
        on_begin_oauth_();
    }
    // Snapshot client_ on the GUI thread: set_client() (via MainWindow's
    // beginAddAccount) can rebind it concurrently with this worker, which
    // would be a data race on the raw pointer.
    auto* c = client_;
    worker_ = std::thread(
        [this, hs, c]
        {
            auto flow = c->begin_oauth(hs);
            if (cancelled_.load())
            {
                return;
            }
            bool ok = static_cast<bool>(flow);
            std::string payload = ok ? flow.auth_url : flow.message;
            surface_->host().post_to_ui(
                [this, ok, payload = std::move(payload)]
                {
                    on_begin_completed(ok, payload);
                });
        });
}

void LoginView::on_begin_completed(bool ok, std::string err_or_url)
{
    join_worker();
    if (!ok)
    {
        shared_->set_status("Sign-in failed: " + err_or_url,
                            tk::Color::rgb(0xB00020));
        shared_->set_state(tesseract::views::LoginView::State::Form);
        hs_field_->set_enabled(true);
        layout_overlays();
        surface_->update();
        return;
    }

    if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(err_or_url))))
    {
        tesseract::Client::open_in_browser(err_or_url);
    }

    cancelled_.store(false);
    auto* c = client_;
    worker_ = std::thread(
        [this, c]
        {
            auto res = c->await_oauth();
            if (cancelled_.load())
            {
                return;
            }
            bool ok = static_cast<bool>(res);
            std::string msg = res.message;
            surface_->host().post_to_ui(
                [this, ok, msg = std::move(msg)]
                {
                    on_await_completed(ok, msg);
                });
        });
}

void LoginView::on_await_completed(bool ok, std::string err)
{
    join_worker();
    if (ok)
    {
        emit loginSucceeded();
        return;
    }
    shared_->set_status("Sign-in failed: " + err, tk::Color::rgb(0xB00020));
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
    layout_overlays();
}

void LoginView::on_cancel()
{
    cancelled_.store(true);
    if (client_)
    {
        client_->cancel_oauth();
    }
    shared_->set_status("Cancelling…");
    surface_->update();
    join_worker();
    shared_->set_status("");
    shared_->set_state(tesseract::views::LoginView::State::Form);
    hs_field_->set_enabled(true);
    surface_->relayout();
    layout_overlays();
    // Tell the host so it can swap back to the previous account's UI (in
    // AddAccount mode) or do nothing (in Initial mode the host has no
    // back-state and just leaves the LoginView visible).
    emit loginCancelled();
}

void LoginView::join_worker()
{
    if (worker_.joinable())
    {
        worker_.join();
    }
}

std::string LoginView::trim(std::string s)
{
    return tesseract::text::trim(s);
}

} // namespace qt6
