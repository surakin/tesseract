#include "RoomWindow.h"
#include "MainWindow.h"

#include "views/ComposePopups.h"
#include "views/ImageViewerOverlay.h"
#include "views/PopoutRoomWidget.h"
#include "views/VideoViewerOverlay.h"

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
    img_viewer_            = room_widget->image_viewer();
    vid_viewer_            = room_widget->video_viewer();
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

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
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
            fetch_source_bytes_(src, std::move(on_ready));
        });

    // ── Image / video save dialogs ────────────────────────────────────────
    img_viewer_->on_save =
        [this](std::string source_url, std::string filename_hint)
    {
        std::string suggested = filename_hint.empty() ? "image" : filename_hint;
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save image"), QString::fromStdString(suggested),
            tr("Images (*.jpg *.jpeg *.png *.gif *.webp);;All files (*.*)"));
        if (!path.isEmpty())
            save_source_to_file_(std::move(source_url), path.toStdString());
    };
    vid_viewer_->on_save =
        [this](std::string source_json, std::string mime_type)
    {
        std::string suggested = "video";
        if (mime_type == "video/mp4")
            suggested = "video.mp4";
        else if (mime_type == "video/webm")
            suggested = "video.webm";
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save video"), QString::fromStdString(suggested),
            tr("Videos (*.mp4 *.webm *.mkv);;All files (*.*)"));
        if (!path.isEmpty())
            save_source_to_file_(std::move(source_json), path.toStdString());
    };
    room_view_->on_file_clicked =
        [this](const tesseract::views::MessageListView::FileHit& hit)
    {
        std::string suggested = hit.file_name.empty() ? "download" : hit.file_name;
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save file"), QString::fromStdString(suggested),
            tr("All files (*.*)"));
        if (path.isEmpty())
            return;
        std::string url = hit.source ? hit.source->fetch_token() : std::string{};
        save_source_to_file_(std::move(url), path.toStdString());
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
    room_view_->message_list()->on_show_copy_menu = [this]()
    {
        auto* ml = room_view_->message_list();
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QAction* copyAct = menu->addAction(tr("Copy"));
        QObject::connect(copyAct, &QAction::triggered, [ml]()
        {
            ml->copy_selection();
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
            shell_show_status_message_(std::move(reason));
        });

    // ── Compose text area (self-owned) + @mention autocomplete ────────────
    roomTextArea_ = room_view_->compose_bar()->text_area();
    roomTextArea_->set_on_changed(
        [this](const std::string& s)
        {
            if (room_view_)
                room_view_->set_current_text(s);
            // Drive all composer popups through the shared priority dispatch
            // (gif > slash > shortcode > mention).
            tesseract::views::dispatch_compose_text_changed(
                s, roomTextArea_->cursor_byte_pos(), gif_controller_.get(),
                slash_controller_.get(), shortcode_controller_.get(),
                mention_controller_.get());
        });
    roomTextArea_->set_on_submit(
        [this]
        {
            if (tesseract::views::dispatch_compose_submit(
                    gif_controller_.get(), slash_controller_.get(),
                    shortcode_controller_.get(), mention_controller_.get()))
                return;
            if (room_view_)
                room_view_->compose_bar()->trigger_send();
        });
    // Auto-grow (set_on_height_changed) is wired internally by ComposeBar's
    // own constructor now — see ComposeBar::ComposeBar()'s text_area_ setup.
    roomTextArea_->push_popup_nav(
        [this](tk::NavKey nk) -> bool
        {
            return tesseract::views::dispatch_compose_nav(
                nk, gif_controller_.get(), slash_controller_.get(),
                shortcode_controller_.get(), mention_controller_.get());
        });
    roomTextArea_->set_on_edit_last(
        [this] { return room_view_ && room_view_->edit_last_own(); });

    // Mention popup surface (eager, hidden until shown).
    mention_popup_frame_ = new QWidget(this);
    mention_popup_frame_->setFocusPolicy(Qt::NoFocus);
    mention_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        surface_->theme(), mention_popup_frame_, /*transparent=*/false);
    mention_popup_surface_->setFocusPolicy(Qt::NoFocus);
    {
        auto w = std::make_unique<tesseract::views::MentionPopup>();
        mention_popup_widget_ = w.get();
        mention_popup_surface_->set_root(std::move(w));
        auto* lay = new QVBoxLayout(mention_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(mention_popup_surface_.get());
    }
    mention_popup_frame_->hide();

    tesseract::views::MentionController::Hooks hooks;
    hooks.show = [this](tk::Rect cursor, int rows)
    { show_mention_popup_(cursor, rows); };
    hooks.hide = [this]
    {
        if (mention_popup_frame_)
            mention_popup_frame_->hide();
    };
    hooks.repaint = [this]
    {
        if (mention_popup_surface_)
            mention_popup_surface_->update();
    };
    hooks.room_id = [this] { return room_id_; };
    hooks.run_async = [this](std::function<void()> fn)
    { run_async_(std::move(fn)); };
    hooks.post_to_ui = [this](std::function<void()> fn)
    { post_to_ui_(std::move(fn)); };
    wire_mention_shell_hooks_(mention_popup_widget_, hooks);
    mention_controller_ = std::make_unique<tesseract::views::MentionController>(
        roomTextArea_, shell_client_(), mention_popup_widget_,
        std::move(hooks));

    // ── /command autocomplete popup ───────────────────────────────────────
    slash_popup_frame_ = new QWidget(this);
    slash_popup_frame_->setFocusPolicy(Qt::NoFocus);
    slash_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        surface_->theme(), slash_popup_frame_, /*transparent=*/false);
    slash_popup_surface_->setFocusPolicy(Qt::NoFocus);
    {
        auto w = std::make_unique<tesseract::views::SlashCommandPopup>();
        slash_popup_widget_ = w.get();
        slash_popup_surface_->set_root(std::move(w));
        auto* lay = new QVBoxLayout(slash_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(slash_popup_surface_.get());
    }
    slash_popup_frame_->hide();
    {
        tesseract::views::SlashCommandController::Hooks sh;
        sh.show = [this](tk::Rect cursor, int rows)
        { show_slash_popup_(cursor, rows); };
        sh.hide = [this]
        {
            if (slash_popup_frame_)
                slash_popup_frame_->hide();
        };
        sh.repaint = [this]
        {
            if (slash_popup_surface_)
                slash_popup_surface_->update();
        };
        sh.room_id = [this] { return room_id_; };
        sh.client = [this] { return shell_client_(); };
        sh.clear_composer = [this]
        {
            if (room_view_)
                room_view_->clear_compose_text();
        };
        sh.on_location = [this] { send_current_location_(); };
        slash_controller_ =
            std::make_unique<tesseract::views::SlashCommandController>(
                roomTextArea_, slash_popup_widget_, std::move(sh));
    }

    // ── :shortcode: emoji/emoticon autocomplete popup ─────────────────────
    shortcode_popup_frame_ = new QWidget(this);
    shortcode_popup_frame_->setFocusPolicy(Qt::NoFocus);
    shortcode_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        surface_->theme(), shortcode_popup_frame_, /*transparent=*/false);
    shortcode_popup_surface_->setFocusPolicy(Qt::NoFocus);
    {
        auto w = std::make_unique<tesseract::views::ShortcodePopup>();
        shortcode_popup_widget_ = w.get();
        // Custom-emoticon thumbnails: peek the shell media cache (populated by
        // the controller's fetch_image hook); Unicode emoji render as glyphs.
        shortcode_popup_widget_->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            { return shell_image_(url); });
        shortcode_popup_surface_->set_root(std::move(w));
        auto* lay = new QVBoxLayout(shortcode_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(shortcode_popup_surface_.get());
    }
    shortcode_popup_frame_->hide();
    {
        tesseract::views::ShortcodeController::Hooks sh;
        sh.show = [this](tk::Rect cursor, int rows)
        { show_shortcode_popup_(cursor, rows); };
        sh.hide = [this]
        {
            if (shortcode_popup_frame_)
                shortcode_popup_frame_->hide();
        };
        sh.repaint = [this]
        {
            if (shortcode_popup_surface_)
                shortcode_popup_surface_->update();
        };
        sh.emoticons = [this]() { return shell_emoticons_(); };
        sh.fetch_image = [this](const std::string& url)
        { shell_ensure_media_image_(url, 28, 28); };
        sh.resolve_image = [this](const std::string& url) -> const tk::Image*
        { return shell_image_(url); };
        shortcode_controller_ =
            std::make_unique<tesseract::views::ShortcodeController>(
                roomTextArea_, shortcode_popup_widget_, std::move(sh));
    }

    // ── /gif inline result strip ──────────────────────────────────────────
    gif_popup_frame_ = new QWidget(this);
    gif_popup_frame_->setFocusPolicy(Qt::NoFocus);
    gif_popup_frame_->hide();
    gif_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        surface_->theme(), gif_popup_frame_, /*transparent=*/false);
    gif_popup_surface_->setFocusPolicy(Qt::NoFocus);
    {
        auto w = std::make_unique<tesseract::views::GifPopup>();
        gif_popup_widget_ = w.get();
        // Strip cells render via the shell's shared two-stage provider. The
        // repaint refreshes THIS pop-out's surface and is self-guarded by the
        // window's liveness token (the shell's in-flight fetch may outlive us).
        gif_popup_widget_->set_image_provider(
            [this](const tesseract::GifResult& result) -> const tk::Image*
            {
                auto alive = alive_;
                return shell_gif_strip_image_(
                    result,
                    [this, alive]
                    {
                        if (*alive && gif_popup_surface_)
                            gif_popup_surface_->update();
                    });
            });
        gif_popup_surface_->set_root(std::move(w));
        auto* lay = new QVBoxLayout(gif_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(gif_popup_surface_.get());
    }
    {
        tesseract::views::GifController::Hooks gh;
        gh.show = [this] { show_gif_popup_(); };
        gh.hide = [this] { hide_gif_popup_(); };
        gh.repaint = [this]
        {
            if (gif_popup_surface_)
                gif_popup_surface_->update();
        };
        gh.room_id = [this] { return room_id_; };
        gh.client = [this] { return shell_client_(); };
        gh.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        gh.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        gh.post_delayed = [this](int ms, std::function<void()> fn)
        {
            if (surface_)
                surface_->host().post_delayed(ms, std::move(fn));
        };
        gh.api_key = []() -> std::string
        { return tesseract::Settings::instance().gif_api_key; };
        gh.client_key = []() -> std::string { return "tesseract"; };
        gh.clear_composer = [this]
        {
            if (roomTextArea_)
                roomTextArea_->set_text("");
            if (room_view_)
                room_view_->clear_compose_text();
        };
        gh.get_cached_gif_bytes =
            [this](const std::string& url) -> std::vector<std::uint8_t>
        { return shell_cached_gif_bytes_(url); };
        gif_controller_ = std::make_unique<tesseract::views::GifController>(
            roomTextArea_, gif_popup_widget_, std::move(gh));
    }

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
        if (surface_)
            surface_->setCursor(url.empty() ? Qt::ArrowCursor
                                            : Qt::PointingHandCursor);
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
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
        mention_popup_surface_->root()->apply_theme(t);
    }
}

void RoomWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_frame_ || !surface_)
    {
        return;
    }
    int h = int(rows * tesseract::views::MentionPopup::kRowHeight);
    int w = int(tesseract::views::MentionPopup::kWidth);
    QPoint pc = surface_->mapTo(
        this, QPoint(int(cursor_local.x), int(cursor_local.y)));
    QRect work = rect();
    int x = pc.x();
    int y_above = pc.y() - h - 4;
    int y_below = pc.y() + int(cursor_local.h) + 4;
    int y = (y_above >= work.top()) ? y_above : y_below;
    x = std::clamp(x, work.left(), work.right() - w);
    y = std::clamp(y, work.top(), work.bottom() - h);
    mention_popup_frame_->setGeometry(x, y, w, h);
    mention_popup_surface_->resize(w, h);
    mention_popup_frame_->show();
    mention_popup_frame_->raise();
    mention_popup_surface_->relayout();
}

void RoomWindow::show_anchored_popup_(QWidget* frame,
                                      tk::qt6::Surface* surface,
                                      tk::Rect cursor_local, int w, int h)
{
    if (!frame || !surface || !surface_)
    {
        return;
    }
    QPoint pc = surface_->mapTo(
        this, QPoint(int(cursor_local.x), int(cursor_local.y)));
    QRect work = rect();
    int x = pc.x();
    int y_above = pc.y() - h - 4;
    int y_below = pc.y() + int(cursor_local.h) + 4;
    int y = (y_above >= work.top()) ? y_above : y_below;
    x = std::clamp(x, work.left(), work.right() - w);
    y = std::clamp(y, work.top(), work.bottom() - h);
    frame->setGeometry(x, y, w, h);
    surface->resize(w, h);
    frame->show();
    frame->raise();
    surface->relayout();
}

void RoomWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    int h = int(rows * tesseract::views::SlashCommandPopup::kRowHeight);
    int w = int(tesseract::views::SlashCommandPopup::kWidth);
    show_anchored_popup_(slash_popup_frame_, slash_popup_surface_.get(),
                         cursor_local, w, h);
}

void RoomWindow::show_shortcode_popup_(tk::Rect cursor_local, int rows)
{
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);
    int w = int(tesseract::views::ShortcodePopup::kWidth);
    show_anchored_popup_(shortcode_popup_frame_, shortcode_popup_surface_.get(),
                         cursor_local, w, h);
}

void RoomWindow::show_gif_popup_()
{
    if (!gif_popup_frame_ || !gif_popup_widget_ || !roomTextArea_ ||
        !surface_ || !gif_popup_surface_ || !room_view_)
    {
        return;
    }
    // Full-width strip floating just above the compose bar (like the main
    // window's). content_size() drives the height + the empty/status check.
    const tk::Rect cb = room_view_->compose_bar_rect();
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    int w = std::max(1, int(cb.w));
    int h = std::max(1, int(sz.h));
    QPoint tl = surface_->mapTo(this, QPoint(int(cb.x), int(cb.y)));
    int x = tl.x();
    int y = tl.y() - h - 4; // bottom edge sits just above the compose bar top
    gif_popup_frame_->setGeometry(x, y, w, h);
    gif_popup_surface_->resize(w, h);
    gif_popup_frame_->show();
    gif_popup_frame_->raise();
    gif_popup_surface_->relayout();
}

void RoomWindow::hide_gif_popup_()
{
    if (gif_popup_frame_)
    {
        gif_popup_frame_->hide();
    }
}

void RoomWindow::on_gif_results(std::uint64_t request_id,
                                std::vector<tesseract::GifResult> results)
{
    if (gif_controller_)
    {
        gif_controller_->on_results(request_id, std::move(results));
    }
}

void RoomWindow::on_gif_search_failed(std::uint64_t request_id,
                                      const std::string& message)
{
    if (gif_controller_)
    {
        gif_controller_->on_search_failed(request_id, message);
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
    surface_repaint_();
    if (room_view_)
    {
        if (room_view_->emoji_picker_visible() && room_view_->emoji_picker())
            room_view_->emoji_picker()->invalidate_image_cache();
        if (room_view_->sticker_picker_visible() && room_view_->sticker_picker())
            room_view_->sticker_picker()->invalidate_image_cache();
    }
    // Advance the /gif strip's animated cells (frames come from the shared
    // anim cache; the shell's tick fires this for every window).
    if (gif_popup_frame_ && gif_popup_frame_->isVisible() && gif_popup_surface_)
    {
        gif_popup_surface_->update();
    }
}

void RoomWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (surface_)
    {
        surface_->relayout();
        surface_->update();
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
        if (vid_viewer_ && vid_viewer_->is_open())
        {
            vid_viewer_->close();
            vid_viewer_->set_visible(false);
            if (surface_)
                surface_->relayout();
            return;
        }
        if (img_viewer_ && img_viewer_->is_open())
        {
            img_viewer_->close();
            img_viewer_->set_visible(false);
            if (surface_)
                surface_->relayout();
            return;
        }
    }
    if (ev->key() == Qt::Key_C && (ev->modifiers() & Qt::ControlModifier))
    {
        if (room_view_ && room_view_->message_list()->has_selection())
        {
            room_view_->message_list()->copy_selection();
            ev->accept();
            return;
        }
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
