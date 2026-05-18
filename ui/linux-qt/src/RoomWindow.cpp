#include "RoomWindow.h"
#include "MainWindow.h"

#include "views/RoomView.h"
#include "views/text_util.h"
#include <tesseract/client.h>

#include <QCloseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace qt6
{

RoomWindow::RoomWindow(MainWindow* parent_shell, const std::string& room_id)
    : QWidget(nullptr, Qt::Window),
      tesseract::RoomWindowBase(parent_shell, room_id),
      parent_shell_(parent_shell)
{
    setAttribute(Qt::WA_DeleteOnClose,
                 false); // we manage lifetime via unique_ptr
    resize(800, 600);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);

    auto room_root = std::make_unique<tesseract::views::RoomView>();
    room_view_ = room_root.get();
    surface_->set_root(std::move(room_root));

    // ── Shared RoomView wiring (providers + compose callbacks) ───────────
    wire_room_view_(room_view_);

    // ── Surface-bound providers (need this shell's own surface_) ─────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }
    room_view_->set_post_delayed(
        [this](int ms, std::function<void()> fn)
        {
            if (surface_)
            {
                surface_->host().post_delayed(ms, std::move(fn));
            }
        });
    room_view_->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };

    // NativeTextArea is not used here: Qt6's host creates it lazily if needed.
    // The compose bar's built-in QTextEdit handles input on this platform.

    show();
    finish_init_();
}

RoomWindow::~RoomWindow() = default;

// ---------------------------------------------------------------------------

void RoomWindow::bring_to_front()
{
    raise();
    activateWindow();
}

void RoomWindow::close_window()
{
    close();
}

void RoomWindow::request_relayout()
{
    if (surface_)
    {
        surface_->relayout();
        surface_->update();
    }
}

void RoomWindow::update_window_title_(const std::string& name)
{
    setWindowTitle(QString::fromStdString(name));
}

void RoomWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
    }
}

void RoomWindow::surface_repaint_()
{
    if (surface_)
    {
        surface_->update();
    }
}

const tesseract::views::UrlPreviewData*
RoomWindow::preview_lookup_(const std::string& url)
{
    auto it = parent_shell_->url_preview_data_.find(url);
    return it == parent_shell_->url_preview_data_.end() ? nullptr : &it->second;
}

void RoomWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (surface_)
    {
        surface_->relayout();
        surface_->update();
    }
}

void RoomWindow::closeEvent(QCloseEvent* ev)
{
    schedule_self_close_();
    ev->accept();
}

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace qt6
