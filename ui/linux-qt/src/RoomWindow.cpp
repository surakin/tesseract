#include "RoomWindow.h"
#include "MainWindow.h"

#include "app/ComposerPopups.h"
#include "tk/i18n.h"
#include "views/MediaViewerOverlay.h"
#include "views/PopoutRoomWidget.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>
#include <tesseract/settings.h>

#include <QCloseEvent>
#include <QFileDialog>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace qt6
{

RoomWindow::RoomWindow(MainWindow* parent_shell, const std::string& room_id)
    : QWidget(nullptr, Qt::Window),
      tesseract::RoomWindowBase(parent_shell, room_id),
      parent_shell_(parent_shell)
{
    setAttribute(Qt::WA_DeleteOnClose,
                 false); // we manage lifetime via unique_ptr
    setMinimumSize(static_cast<int>(tesseract::visual::kMinWindowWidth),
                    static_cast<int>(tesseract::visual::kMinWindowHeight));
    // Apply saved geometry or fall back to the default size.
    {
        const auto geom = get_saved_popout_geometry_(800, 600);
        if (geom.valid)
            setGeometry(geom.x, geom.y, geom.w, geom.h);
        else
            resize(800, 600);
    }

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);

    auto room_widget = tk::create_root_widget<tesseract::views::PopoutRoomWidget>(
        &surface_->host());
    room_view_             = room_widget->room_view();
    media_viewer_          = room_widget->media_viewer();
    forward_picker_widget_ = room_widget->forward_picker();
    room_media_view_widget_ = room_widget->room_media_view();
    confirm_dialog_widget_ = room_widget->confirm_dialog();
    room_widget->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    surface_->set_root(std::move(room_widget));

    // ── Shared per-room wiring, via RoomPane ──────────────────────────────
    init_pane_(&surface_->host());
    pane_->attach({
        .room_view = room_view_,
        .media_viewer = media_viewer_,
        .forward_picker = forward_picker_widget_,
        .room_media_view = room_media_view_widget_,
        .focus_forward_picker_field = [this]
        {
            if (!forward_picker_widget_)
                return;
            if (auto* f = forward_picker_widget_->search_field())
            {
                f->set_text("");
                f->set_focused(true);
            }
        },
        .hide_forward_picker_field = [this]
        {
            if (forward_picker_widget_)
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
        },
    });

    // ── Video player for this window's media viewer ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        media_viewer_->set_video_player(std::move(player));
    }

    // Inline autoplay video/GIF in the timeline (separate from the lightbox
    // player above — MessageListView falls back to a static thumbnail unless
    // both of these are set).
    room_view_->set_video_player_factory(
        [this]() { return surface_->host().make_video_player(); });
    room_view_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready)
        {
            pane_->fetch_source_bytes_(src, std::move(on_ready));
        });

    // ── Media save dialog ─────────────────────────────────────────────────
    pane_->install_media_viewer_save_(
        [this](const tesseract::views::MediaSaveSpec& spec,
               std::function<void(std::string)> done)
        {
            QString path = QFileDialog::getSaveFileName(
                this, QString::fromStdString(spec.title),
                QString::fromStdString(spec.suggested_name), save_filter_for(spec));
            if (!path.isEmpty())
            {
                done(path.toStdString());
            }
        });

    // Full-screen toggle acts on this pop-out window, not the main one.
    const auto set_fs = [this](bool on)
    {
        if (on == isFullScreen())
            return;
        if (on)
        {
            rw_was_maximized_ = isMaximized();
            showFullScreen();
        }
        else if (rw_was_maximized_)
            showMaximized();
        else
            showNormal();
    };
    media_viewer_->on_request_fullscreen = set_fs;

    room_view_->on_file_clicked =
        [this](const tesseract::views::MessageListView::FileHit& hit)
    {
        std::string suggested = hit.file_name.empty() ? "download" : hit.file_name;
        QString path = QFileDialog::getSaveFileName(
            this, QString::fromStdString(tk::tr("Save file")), QString::fromStdString(suggested),
            QString::fromStdString(tk::tr("All files (*.*)")));
        if (path.isEmpty())
            return;
        std::string url = hit.source ? hit.source->fetch_token() : std::string{};
        pane_->save_source_to_file_(std::move(url), path.toStdString());
    };

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
    room_view_->on_set_clipboard = [this](std::string_view t)
    {
        if (surface_)
            surface_->host().set_clipboard_text(t);
    };
    room_view_->on_selection_started = [this]()
    {
        if (surface_)
            surface_->host().release_focus_to_canvas();
    };
    room_view_->on_show_copy_menu = [this]()
    {
        auto* rv = room_view_;
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(tk::qt6::build_menu_qss(surface_->theme()));
        QAction* copyAct = menu->addAction(QString::fromStdString(tk::tr("Copy")));
        QObject::connect(copyAct, &QAction::triggered, [rv]()
        {
            rv->copy_active_selection();
        });
        menu->popup(QCursor::pos());
    };

    // Drag-and-drop file ingest is now tree-dispatched automatically (see
    // Surface::dropEvent -> Host::dispatch_file_drop); RoomView::on_file_drop
    // routes into this window's compose bar via the provider fields wired in
    // wire_room_view_. Pop-outs never open their RoomSettingsView, so it's
    // always invisible and never claims a drop ahead of the compose bar.
    surface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            pane_->shell_show_status_message_(std::move(reason));
        });

    // ── Compose text area (self-owned) + composer popups ─────────────────
    roomTextArea_ = room_view_->compose_bar()->text_area();
    popups_ = std::make_unique<tesseract::ComposerPopups>(
        surface_->host(), roomTextArea_, pane_.get(), room_view_);

    surface_->set_on_layout(
        [this]
        {
            // Native child controls always paint over canvas-drawn overlays,
            // so hide them while the confirm dialog covers the window —
            // otherwise the compose box/search fields would poke through on
            // top of the modal backdrop. roomTextArea_ self-positions via
            // ComposeBar::arrange() otherwise (reached via the relayout this
            // set_on_layout callback runs after), so only a force-hide is
            // needed here — mirrors the search fields' own gating below.
            const bool confirm_open =
                confirm_dialog_widget_ && confirm_dialog_widget_->is_open();
            if (confirm_open && roomTextArea_)
            {
                roomTextArea_->set_visible(false);
            }
            if (confirm_open && room_view_)
            {
                // Search field self-positions via RoomSearchBar::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* bar = room_view_->room_search_bar())
                    if (auto* f = bar->search_field())
                        f->set_visible(false);
            }
            if (confirm_open && forward_picker_widget_)
            {
                // Search field self-positions via ForwardRoomPicker::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
            }
        });

    // Per-room "find in conversation" — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here (on_close is already
    // wired internally by RoomView's own constructor).
    if (room_view_)
    {
        if (auto* bar = room_view_->room_search_bar())
        {
            if (auto* rif = bar->search_field())
            {
                rif->push_popup_nav(
                    [this](tk::NavKey nk) -> bool
                    {
                        if (!room_view_ || !room_view_->room_search_open())
                            return false;
                        switch (nk)
                        {
                        case tk::NavKey::Up:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(-1);
                            return true;
                        case tk::NavKey::Down:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(+1);
                            return true;
                        case tk::NavKey::Escape:
                            room_view_->close_room_search();
                            return true;
                        default:
                            return false;
                        }
                    });
            }
        }
    }

    // Forward-message picker — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here.
    if (forward_picker_widget_)
    {
        if (auto* fpf = forward_picker_widget_->search_field())
        {
            fpf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    if (!forward_picker_widget_ || !forward_picker_widget_->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        forward_picker_widget_->move_selection(-1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        forward_picker_widget_->move_selection(+1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        forward_picker_widget_->close();
                        return true;
                    default:
                        return false;
                    }
                });
        }
    }

    room_view_->on_link_hovered = [this](const std::string& url)
    {
        if (!surface_) return;
        using HC = tesseract::views::MessageListView::HoverCursor;
        const HC hc = tesseract::views::MessageListView::hover_cursor_for(url);
        surface_->setCursor(hc == HC::Text      ? Qt::IBeamCursor
                            : hc == HC::Pointer ? Qt::PointingHandCursor
                                                : Qt::ArrowCursor);
    };

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
        surface_->root()->apply_theme(t);
    }
    if (popups_)
    {
        popups_->apply_theme(t);
    }
}

void RoomWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

void RoomWindow::on_gif_results(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results)
{
    if (popups_)
    {
        popups_->on_gif_results(request_id, std::move(results));
    }
}

void RoomWindow::on_gif_search_failed(std::uint64_t request_id,
                                      const std::string& message)
{
    if (popups_)
    {
        popups_->on_gif_search_failed(request_id, message);
    }
}

void RoomWindow::surface_repaint_()
{
    if (surface_)
    {
        surface_->update();
    }
}

void RoomWindow::repaint_anim_frame()
{
    RoomWindowBase::repaint_anim_frame();
    if (popups_)
    {
        popups_->repaint_anim_frame();
    }
}

void RoomWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (surface_)
    {
        surface_->relayout();
        surface_->update();

        // A popup's screen position is captured once when it opens and
        // never recomputed — leaving one open across a resize would strand
        // it away from whatever control anchored it. Close everything
        // instead (see MainWindow::resizeEvent for the same fix in the
        // main window).
        surface_->host().dismiss_active_popup();
        if (room_view_) room_view_->dismiss_popups();
        if (popups_)
        {
            popups_->hide_all();
        }
    }
    const QRect r = geometry();
    save_popout_geometry_(r.x(), r.y(), r.width(), r.height());
}

void RoomWindow::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    const QRect r = geometry();
    save_popout_geometry_(r.x(), r.y(), r.width(), r.height());
}

void RoomWindow::closeEvent(QCloseEvent* ev)
{
    schedule_self_close_();
    ev->accept();
}

void RoomWindow::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape)
    {
        if (room_view_ && room_view_->room_search_open())
        {
            room_view_->close_room_search();
            return;
        }
        if (close_media_viewer_if_open_())
            return;
    }
    QWidget::keyPressEvent(ev);
}

// ---------------------------------------------------------------------------

tesseract::RoomWindowBase*
MainWindow::create_secondary_room_window_(const std::string& room_id)
{
    return new RoomWindow(this, room_id);
}

} // namespace qt6
