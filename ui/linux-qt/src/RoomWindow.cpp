#include "RoomWindow.h"
#include "EmojiPicker.h"
#include "MainWindow.h"
#include "StickerPicker.h"

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

    auto room_widget = std::make_unique<tesseract::views::PopoutRoomWidget>();
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
            shell_show_status_message_(std::move(reason));
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
        roomTextArea_.get(), shell_client_(), mention_popup_widget_,
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
        slash_controller_ =
            std::make_unique<tesseract::views::SlashCommandController>(
                roomTextArea_.get(), slash_popup_widget_, std::move(sh));
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
        sh.emoticons = [this]() -> const std::vector<tesseract::ImagePackImage>&
        { return shell_emoticons_(); };
        sh.fetch_image = [this](const std::string& url)
        { shell_ensure_media_image_(url, 28, 28); };
        sh.resolve_image = [this](const std::string& url) -> const tk::Image*
        { return shell_image_(url); };
        shortcode_controller_ =
            std::make_unique<tesseract::views::ShortcodeController>(
                roomTextArea_.get(), shortcode_popup_widget_, std::move(sh));
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
            roomTextArea_.get(), gif_popup_widget_, std::move(gh));
    }

    surface_->set_on_layout(
        [this]
        {
            // Native child controls always paint over canvas-drawn overlays,
            // so hide them while the confirm dialog covers the window —
            // otherwise the compose box/search fields would poke through on
            // top of the modal backdrop.
            const bool confirm_open =
                confirm_dialog_widget_ && confirm_dialog_widget_->is_open();
            if (room_view_ && roomTextArea_)
            {
                const tk::Rect ta = room_view_->compose_text_area_rect();
                roomTextArea_->set_visible(!confirm_open && !ta.empty());
                if (!confirm_open && !ta.empty())
                    roomTextArea_->set_rect(ta);
            }
            if (room_view_ && roomSearchField_)
            {
                const bool vis =
                    !confirm_open && room_view_->room_search_field_visible();
                roomSearchField_->set_visible(vis);
                if (vis)
                {
                    tk::Rect r = room_view_->room_search_field_rect();
                    r.x += 2; r.y += 2; r.w -= 4; r.h -= 4;
                    roomSearchField_->set_rect(r);
                }
            }
            if (forward_picker_widget_ && forward_picker_field_)
            {
                const bool vis = !confirm_open &&
                                forward_picker_widget_->search_field_visible();
                forward_picker_field_->set_visible(vis);
                if (vis)
                {
                    forward_picker_field_->set_rect(
                        forward_picker_widget_->search_field_rect());
                }
            }
        });

    // ── In-room search native text field ─────────────────────────────────
    roomSearchField_ = surface_->host().make_text_field();
    roomSearchField_->set_placeholder(tr("Find in conversation\xe2\x80\xa6").toStdString());
    roomSearchField_->set_visible(false);
    roomSearchField_->set_on_changed(
        [this](const std::string& q)
        {
            if (room_view_)
                if (auto* bar = room_view_->room_search_bar())
                {
                    bar->set_query(q);
                    if (surface_) surface_->relayout();
                }
        });
    roomSearchField_->set_on_popup_nav(
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

    // ── Forward-message picker native search field ─────────────────────────
    forward_picker_field_ = surface_->host().make_text_field();
    forward_picker_field_->set_placeholder(tr("Search rooms\xe2\x80\xa6").toStdString());
    forward_picker_field_->set_visible(false);
    forward_picker_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (forward_picker_widget_)
            {
                forward_picker_widget_->set_query(q);
                if (surface_) surface_->relayout();
            }
        });
    forward_picker_field_->set_on_submit(
        [this]
        {
            if (forward_picker_widget_)
            {
                forward_picker_widget_->confirm();
            }
        });
    forward_picker_field_->set_on_popup_nav(
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
        const tk::Image* image = picker_image_provider_(false)(img.url, img.url);
        int pos = roomTextArea_->cursor_byte_pos();
        roomTextArea_->insert_emoticon(pos, pos, img.shortcode, img.url, image);
        if (room_view_)
            room_view_->set_current_text(roomTextArea_->text());
        roomTextArea_->set_focused(true);
    };
    emojiPicker_->onDismiss = [this]()
    {
        // Fires on every close (selection or outside click). Clear any
        // pending reaction target and release the hover lock taken in
        // on_add_reaction_requested so the row's action buttons hide again.
        pendingReactionEventId_.clear();
        if (room_view_ && room_view_->message_list())
            room_view_->message_list()->set_hover_locked(false);
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
        // Keep the message row's action buttons visible while the reaction
        // picker is open (released in onDismiss).
        if (room_view_ && room_view_->message_list())
            room_view_->message_list()->set_hover_locked(true);
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
    if (emojiPicker_ && emojiPicker_->isVisible())
    {
        emojiPicker_->invalidateImages();
    }
    if (stickerPicker_ && stickerPicker_->isVisible())
    {
        stickerPicker_->invalidateImages();
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
