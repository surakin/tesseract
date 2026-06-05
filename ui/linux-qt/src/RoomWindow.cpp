#include "RoomWindow.h"
#include "EmojiPicker.h"
#include "MainWindow.h"
#include "StickerPicker.h"

#include "views/ImageViewerOverlay.h"
#include "views/PopoutRoomWidget.h"
#include "views/VideoViewerOverlay.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>

#include <QCloseEvent>
#include <QFileDialog>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QToolTip>
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
    resize(800, 600);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);

    auto room_widget = std::make_unique<tesseract::views::PopoutRoomWidget>();
    room_view_  = room_widget->room_view();
    img_viewer_ = room_widget->image_viewer();
    vid_viewer_ = room_widget->video_viewer();
    surface_->set_root(std::move(room_widget));

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ─────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
    }

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

    // Drag-and-drop file ingest into this pop-out's compose bar (shared base
    // routes the payload + runs the shell's media probe against this window).
    surface_->set_on_file_drop(
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename)
        {
            handle_file_drop_(std::move(bytes), std::move(mime),
                              std::move(filename));
        });
    surface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            shell_->show_status_message_(std::move(reason));
        });

    // ── Compose text area overlay + @mention autocomplete ─────────────────
    roomTextArea_ = surface_->host().make_text_area();
    roomTextArea_->set_font_role(tk::FontRole::Body);
    roomTextArea_->set_text_color(surface_->theme().palette.text_primary);
    roomTextArea_->set_mention_colors(surface_->theme().palette.accent,
                                      surface_->theme().palette.text_on_accent);
    roomTextArea_->set_placeholder(tr("Message\xe2\x80\xa6").toStdString());
    roomTextArea_->set_on_changed(
        [this](const std::string& s)
        {
            if (room_view_)
                room_view_->set_current_text(s);
            if (mention_controller_)
                mention_controller_->on_text_changed(
                    s, roomTextArea_->cursor_byte_pos());
        });
    roomTextArea_->set_on_submit(
        [this]
        {
            if (mention_controller_ && mention_controller_->on_submit())
                return;
            if (room_view_)
                room_view_->compose_bar()->trigger_send();
        });
    roomTextArea_->set_on_height_changed(
        [this](float h)
        {
            if (room_view_)
                room_view_->set_text_area_natural_height(h);
            if (surface_)
                surface_->relayout();
        });
    roomTextArea_->set_on_popup_nav(
        [this](tk::NativeTextArea::NavKey nk) -> bool
        { return mention_controller_ && mention_controller_->on_nav(nk); });
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
        roomTextArea_.get(), shell_client_(), mention_popup_widget_,
        std::move(hooks));

    surface_->set_on_layout(
        [this]
        {
            if (room_view_ && roomTextArea_)
            {
                const tk::Rect ta = room_view_->compose_text_area_rect();
                roomTextArea_->set_visible(!ta.empty());
                if (!ta.empty())
                    roomTextArea_->set_rect(ta);
            }
        });

    // ── Platform popups the shared wire_room_view_ can't provide ──────────
    // Pop-out-local emoji / sticker pickers (parented to this window). The
    // emoji picker doubles as the reaction picker via pendingReactionEventId_.
    emojiPicker_ = new ::EmojiPicker(this);
    emojiPicker_->setClient(shell_client_());
    emojiPicker_->setImageProvider(picker_image_provider_(false));
    emojiPicker_->onSelected = [this](const QString& glyph)
    {
        if (!pendingReactionEventId_.empty())
        {
            std::string ev = std::move(pendingReactionEventId_);
            pendingReactionEventId_.clear();
            toggle_reaction_(ev, glyph.toStdString(), std::string{});
            emojiPicker_->hide();
            return;
        }
        if (!roomTextArea_)
            return;
        roomTextArea_->insert_at_cursor(glyph.toStdString());
        if (room_view_)
            room_view_->set_current_text(roomTextArea_->text());
        roomTextArea_->set_focused(true);
    };
    emojiPicker_->onEmoticonSelected =
        [this](const tesseract::ImagePackImage& img)
    {
        if (!pendingReactionEventId_.empty())
        {
            std::string ev = std::move(pendingReactionEventId_);
            pendingReactionEventId_.clear();
            if (!img.url.empty())
                toggle_reaction_(ev, std::string{}, img.url);
            emojiPicker_->hide();
            return;
        }
        if (!roomTextArea_)
            return;
        roomTextArea_->insert_at_cursor(":" + img.shortcode + ":");
        if (room_view_)
            room_view_->set_current_text(roomTextArea_->text());
        roomTextArea_->set_focused(true);
    };
    stickerPicker_ = new ::StickerPicker(this);
    stickerPicker_->setClient(shell_client_());
    stickerPicker_->setImageProvider(picker_image_provider_(true));
    stickerPicker_->onSelected = [this](const tesseract::ImagePackImage& img)
    {
        if (room_id_.empty())
            return;
        std::string body = img.body.empty() ? img.shortcode : img.body;
        if (auto* c = shell_client_())
            c->send_sticker(room_id_, body, img.url, img.info_json);
        stickerPicker_->hide();
    };
    room_view_->on_emoji = [this](tk::Rect btn)
    {
        if (!emojiPicker_)
            return;
        if (emojiPicker_->isVisible())
            emojiPicker_->hide();
        else
            emojiPicker_->popupAtRect(surface_, btn);
    };
    room_view_->on_sticker = [this](tk::Rect btn)
    {
        if (!stickerPicker_)
            return;
        if (stickerPicker_->isVisible())
            stickerPicker_->hide();
        else
            stickerPicker_->popupAtRect(surface_, btn);
    };
    room_view_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor)
    {
        if (!emojiPicker_ || room_id_.empty())
            return;
        pendingReactionEventId_ = event_id;
        emojiPicker_->popupAtRect(surface_, anchor);
    };
    room_view_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (!surface_)
            return;
        QPoint local(static_cast<int>(anchor.x),
                     static_cast<int>(anchor.y + anchor.h));
        QToolTip::showText(surface_->mapToGlobal(local),
                           QString::fromStdString(text), surface_);
    };
    room_view_->on_hide_tooltip = [] { QToolTip::hideText(); };
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
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
    }
    if (roomTextArea_)
    {
        roomTextArea_->set_mention_colors(t.palette.accent,
                                          t.palette.text_on_accent);
    }
    if (emojiPicker_)
    {
        emojiPicker_->set_theme(t);
    }
    if (stickerPicker_)
    {
        stickerPicker_->set_theme(t);
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
    if (emojiPicker_ && emojiPicker_->isVisible())
    {
        emojiPicker_->invalidateImages();
    }
    if (stickerPicker_ && stickerPicker_->isVisible())
    {
        stickerPicker_->invalidateImages();
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
