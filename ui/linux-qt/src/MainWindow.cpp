#include "MainWindow.h"
#include "LoginView.h"
#include "SettingsWidget.h"
#include "EmojiPicker.h"
#include "StickerPicker.h"
#include "JoinRoomDialog.h"
#include "LinuxScreenLockQt.h"

#include "tk/canvas_qpainter.h"
#include "tk/theme.h"

#include <QThreadPool>
#include <QMenu>
#include <QAction>
#include <QBuffer>
#include <QImageReader>
#include <QPointer>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoFrame>

#include <tesseract/prefs.h>
#include <tesseract/session_store.h>
#include <tesseract/settings.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QMetaType>
#include <QApplication>
#include <QStyleHints>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QIcon>
#include <QScrollBar>
#include <QStringList>
#include <QToolButton>
#include <QToolTip>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QCalendarWidget>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QStandardItem>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QWindow>
#include <algorithm>
#include <thread>
#include <unordered_set>

Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)
Q_DECLARE_METATYPE(tesseract::BackupProgress)

namespace qt6 {

// EventBridge is now a thin QObject wrapper around EventHandlerBase.
// All IEventHandler method bodies live in EventHandlerBase (shared/app/).
// No EventBridge:: method definitions needed here.

// ---------------------------------------------------------------------------
// MainWindow constructor
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<std::vector<tesseract::RoomInfo>>();
    qRegisterMetaType<tesseract::BackupProgress>();

    set_screen_lock_(std::make_unique<LinuxScreenLockQt>());

    setWindowTitle("Tesseract");
    resize(1100, 768);

    contentStack_ = new QStackedWidget(this);
    setCentralWidget(contentStack_);

    loginView_ = new LoginView(contentStack_);
    contentStack_->addWidget(loginView_);
    connect(loginView_, &LoginView::loginSucceeded,
            this,       &MainWindow::onLoginSucceeded);
    connect(loginView_, &LoginView::loginCancelled,
            this,       &MainWindow::onLoginCancelled);

    // Single surface hosting the MainAppWidget tree (sidebar + chat + overlays).
    mainAppSurface_ = new tk::qt6::Surface(tk::Theme::light(), contentStack_);
    contentStack_->addWidget(mainAppSurface_);

    {
        auto main_app_owner = std::make_unique<tesseract::views::MainAppWidget>();
        mainApp_ = main_app_owner.get();

        mainApp_->on_space_back = [this] { onSpaceBack(); };

        // ---- Room list ----
        mainApp_->room_list_view()->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        mainApp_->room_list_view()->on_room_selected =
            [this](const std::string& room_id) {
                if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
                    tab_open_room(room_id);
                else
                    tab_select_room(room_id);
            };
        {
            auto* scrollDebounce = new QTimer(this);
            scrollDebounce->setSingleShot(true);
            mainApp_->room_list_view()->on_scroll = [this, scrollDebounce] {
                scrollDebounce->start(300);
            };
            connect(scrollDebounce, &QTimer::timeout, this, [this] {
                if (!mainApp_ || !client_)
                {
                    return;
                }
                auto ids = mainApp_->room_list_view()->visible_room_ids();
                client_->stop_background_backfill();
                client_->start_background_backfill(ids);
            });
        }
        mainApp_->room_list_view()->on_search_clear = [this] {
            if (roomSearchField_)
            {
                roomSearchField_->set_text("");
            }
            roomSearchPendingText_.clear();
            if (mainApp_)
            {
                mainApp_->room_list_view()->set_search_text("");
            }
            refreshRoomList();
        };
        mainApp_->room_list_view()->on_join_room_requested = [this] {
            if (joinRoomDialog_)
            {
                joinRoomDialog_->openDialog();
            }
        };

        // ---- Tab bar ----
        mainApp_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id) { tab_select_room(room_id); };
        mainApp_->tab_bar()->on_tab_closed =
            [this](const std::string& room_id) { tab_close(room_id); };

        // ---- User info strip ----
        mainApp_->user_info()->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it != tk_avatars_.end() ? it->second.get() : nullptr;
            });
        mainApp_->user_info()->on_primary = [this](tk::Point world) {
            if (accounts_.size() < 2)
            {
                return;
            }
            openAccountPicker(mainAppSurface_->mapToGlobal(
                QPoint(static_cast<int>(world.x), static_cast<int>(world.y))));
        };

        // ---- Recovery banner ----
        mainApp_->recovery_banner()->on_verify =
            [this](const std::string& key) { (void)key; onRecoveryVerifyClicked(); };
        mainApp_->recovery_banner()->on_dismiss =
            [this] { onDismissRecoveryBanner(); };

        // ---- Verification banner ----
        mainApp_->verif_banner()->on_verify = [this] {
            if (client_)
            {
                client_->request_self_verification();
            }
        };
        mainApp_->verif_banner()->on_accept = [this] {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_match = [this] {
            if (client_ && !active_verification_flow_id_.empty())
            {
                mainApp_->verif_banner()->set_state(
                    tesseract::views::VerificationBanner::State::Confirming);
                mainAppSurface_->relayout();
                client_->confirm_sas(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_mismatch = [this] {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_cancel = [this] {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_dismiss = [this] {
            verification_banner_dismissed_ = true;
            mainApp_->show_verif_banner(false);
            mainAppSurface_->relayout();
        };
        mainApp_->verif_banner()->on_done = [this] {
            mainApp_->show_verif_banner(false);
            mainAppSurface_->relayout();
        };
        mainApp_->verif_banner()->on_use_recovery_key = [this] {
            mainApp_->show_verif_banner(false);
            mainAppSurface_->relayout();
            maybeShowRecoveryBanner();
        };

        // ---- Image viewer ----
        mainApp_->image_viewer()->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                if (auto it = viewerFullresCache_.find(url);
                    it != viewerFullresCache_.end())
                {
                    return it->second.get();
                }
                if (const auto* f = anim_cache_.current_frame(url))
                {
                    return f;
                }
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        mainApp_->image_viewer()->on_close = [this] {
            mainApp_->show_image_viewer(false);
            mainAppSurface_->relayout();
        };

        // ---- Video viewer ----
        mainApp_->video_viewer()->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        mainApp_->video_viewer()->set_video_player(
            mainAppSurface_->host().make_video_player());
        mainApp_->video_viewer()->set_repaint_requester([this] {
            if (mainAppSurface_)
            {
                mainAppSurface_->relayout();
            }
        });
        mainApp_->video_viewer()->on_close = [this] {
            mainApp_->show_video_viewer(false);
            mainAppSurface_->relayout();
        };

        // ---- Room view ----
        mainApp_->room_view()->set_avatar_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it == tk_avatars_.end() ? nullptr : it->second.get();
            });
        mainApp_->room_view()->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                if (const auto* f = anim_cache_.current_frame(mxc))
                {
                    return f;
                }
                auto it = tk_images_.find(mxc);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });
        mainApp_->room_view()->set_preview_provider(
            [this](const std::string& url) -> const tesseract::views::UrlPreviewData* {
                auto it = url_preview_data_.find(url);
                if (it == url_preview_data_.end())
                {
                    return nullptr;
                }
                if (!it->second.image_mxc.empty()
                    && !tk_images_.count(it->second.image_mxc)
                    && !anim_cache_.has(it->second.image_mxc))
                {
                    ensure_media_image_(it->second.image_mxc, 64, 64);
                }
                return &it->second;
            });
        if (auto player = mainAppSurface_->host().make_audio_player())
        {
            mainApp_->room_view()->set_audio_player(std::move(player));
        }
        mainApp_->room_view()->set_voice_bytes_provider(
            [this](const std::string& source_json) -> std::vector<std::uint8_t> {
                return client_->fetch_source_bytes(source_json);
            });
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            mainApp_->room_view()->set_repaint_requester([sfp]() {
                if (sfp)
                {
                    sfp->update();
                }
            });
            mainApp_->room_view()->set_post_delayed(
                [sfp](int ms, std::function<void()> fn) {
                    if (sfp)
                    {
                        sfp->host().post_delayed(ms, std::move(fn));
                    }
                });
        }
        mainApp_->room_view()->set_video_player_factory(
            [this]() { return mainAppSurface_->host().make_video_player(); });
        mainApp_->room_view()->set_video_fetch_provider(
            [this](const std::string& src,
                   std::function<void(std::vector<std::uint8_t>)> on_ready) {
                runOnPool_([this, src, on_ready = std::move(on_ready)]() mutable {
                    auto bytes = client_->fetch_source_bytes(src);
                    QMetaObject::invokeMethod(this,
                        [on_ready = std::move(on_ready), bytes = std::move(bytes)]() mutable {
                            on_ready(std::move(bytes));
                        }, Qt::QueuedConnection);
                });
            });

        mainApp_->room_view()->on_layout_changed = [this] {
            if (mainAppSurface_)
            {
                mainAppSurface_->relayout();
            }
        };
        mainApp_->room_view()->on_link_clicked = [](const std::string& url) {
            tesseract::Client::open_in_browser(url);
        };
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            mainApp_->room_view()->on_link_hovered = [sfp](const std::string& url) {
                if (sfp)
                {
                    sfp->setCursor(url.empty() ? Qt::ArrowCursor
                                               : Qt::PointingHandCursor);
                }
            };
        }
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            mainApp_->room_view()->on_show_tooltip = [sfp](std::string text, tk::Rect anchor) {
                if (!sfp)
                {
                    return;
                }
                QPoint local(static_cast<int>(anchor.x),
                             static_cast<int>(anchor.y + anchor.h));
                QToolTip::showText(sfp->mapToGlobal(local),
                                   QString::fromStdString(text), sfp);
            };
            mainApp_->room_view()->on_hide_tooltip = [] { QToolTip::hideText(); };
        }
        mainApp_->room_view()->on_receipt_needed = [this](const std::string& eid) {
            maybe_send_read_receipt_(current_room_id_, eid);
        };
        mainApp_->room_view()->message_list()->on_tile_needed =
            [this](int z, int x, int y) { ensure_tile_async(z, x, y); };
        mainApp_->room_view()->on_near_top = [this] {
            if (current_room_id_.empty())
            {
                return;
            }
            requestMoreHistory(current_room_id_);
        };
        mainApp_->room_view()->on_near_bottom = [this] {
            if (!current_room_id_.empty())
            {
                request_forward_history_(current_room_id_);
            }
        };
        mainApp_->room_view()->on_return_to_live = [this] {
            if (!current_room_id_.empty())
            {
                return_to_live_(current_room_id_);
            }
        };
        mainApp_->room_view()->on_scroll_to_original = [this](const std::string& event_id) {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string room = current_room_id_;
            begin_focused_subscription_(room, event_id);
            runOnPool_([this, room, event_id] {
                client_->subscribe_room_at(room, event_id);
            });
        };
        mainApp_->room_view()->on_jump_to_date_requested = [this] {
            openJumpToDateDialog();
        };
        mainApp_->room_view()->on_delete_requested = [this](const std::string& event_id) {
            if (current_room_id_.empty())
            {
                return;
            }
            client_->redact_event(current_room_id_, event_id);
        };
        mainApp_->room_view()->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key) {
                if (current_room_id_.empty())
                {
                    return;
                }
                client_->send_reaction(current_room_id_, event_id, key);
            };
        mainApp_->room_view()->on_add_reaction_requested =
            [this](const std::string& event_id, tk::Rect anchor) {
                if (!emojiPicker_ || current_room_id_.empty())
                {
                    return;
                }
                pendingReactionEventId_ = event_id;
                emojiPicker_->popupAtRect(mainAppSurface_, anchor);
            };
        mainApp_->room_view()->on_send = [this](const std::string& body) {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string trimmed = QString::fromStdString(body).trimmed().toStdString();
            if (trimmed.empty())
            {
                return;
            }
            auto res = client_->send_message(current_room_id_, trimmed);
            if (res)
            {
                if (roomTextArea_)
                {
                    roomTextArea_->set_text("");
                }
                mainApp_->room_view()->clear_compose_text();
            }
            else
            {
                statusBar()->showMessage(QString::fromStdString(res.message), 4000);
            }
        };
        mainApp_->room_view()->on_send_reply =
            [this](const std::string& reply_event_id, const std::string& body) {
                if (body.empty() || current_room_id_.empty())
                {
                    return;
                }
                auto res = client_->send_reply(current_room_id_, reply_event_id, body);
                if (!res)
                {
                    statusBar()->showMessage(
                        tr("Send reply failed: %1").arg(QString::fromStdString(res.message)), 4000);
                }
                if (roomTextArea_)
                {
                    roomTextArea_->set_text("");
                }
                mainApp_->room_view()->clear_compose_text();
            };
        mainApp_->room_view()->on_send_edit =
            [this](const std::string& event_id, const std::string& new_body) {
                if (new_body.empty() || current_room_id_.empty())
                {
                    return;
                }
                auto res = client_->send_edit(current_room_id_, event_id, new_body);
                if (!res)
                {
                    statusBar()->showMessage(
                        tr("Edit failed: %1").arg(QString::fromStdString(res.message)), 4000);
                }
                if (roomTextArea_)
                {
                    roomTextArea_->set_text("");
                }
                mainApp_->room_view()->clear_compose_text();
            };
        mainApp_->room_view()->on_send_image =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   int /*src_w*/, int /*src_h*/, std::string reply_id) {
                if (current_room_id_.empty())
                {
                    return;
                }
                const bool compress =
                    tesseract::Settings::instance().image_quality
                    == tesseract::Settings::ImageQuality::Compressed;
                auto enc = mainAppSurface_->host().encode_for_send(
                    bytes.data(), bytes.size(), compress);
                if (enc.bytes.empty())
                {
                    statusBar()->showMessage(tr("Image decode failed"), 4000);
                    return;
                }
                std::string out_name = filename;
                if (enc.mime == "image/jpeg")
                {
                    auto dot = out_name.find_last_of('.');
                    if (dot != std::string::npos)
                    {
                        out_name = out_name.substr(0, dot);
                    }
                    out_name += ".jpg";
                }
                auto res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
                                                out_name, caption,
                                                enc.width, enc.height, reply_id);
                if (!res)
                {
                    statusBar()->showMessage(
                        tr("Send image failed: %1").arg(
                            QString::fromStdString(res.message)), 4000);
                    return;
                }
                if (roomTextArea_)
                {
                    roomTextArea_->set_text("");
                }
                mainApp_->room_view()->clear_compose_text();
            };
        mainApp_->room_view()->on_send_file =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::string reply_id) {
                if (current_room_id_.empty())
                {
                    return;
                }
                auto res = client_->send_file(current_room_id_, bytes, mime,
                                              filename, caption, reply_id);
                if (!res)
                {
                    statusBar()->showMessage(
                        tr("Send file failed: %1").arg(
                            QString::fromStdString(res.message)), 4000);
                    return;
                }
                if (roomTextArea_)
                {
                    roomTextArea_->set_text("");
                }
                mainApp_->room_view()->clear_compose_text();
            };
        mainApp_->room_view()->on_edit_prefill = [this](const std::string& body) {
            if (roomTextArea_)
            {
                roomTextArea_->set_text(body);
                roomTextArea_->set_focused(true);
            }
        };
        mainApp_->room_view()->on_edit_cancelled = [this] {
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_reply_focus = [this] {
            if (roomTextArea_)
            {
                roomTextArea_->set_focused(true);
            }
        };
        mainApp_->room_view()->on_emoji = [this](tk::Rect btn) {
            if (!emojiPicker_)
            {
                return;
            }
            if (emojiPicker_->isVisible())
            {
                emojiPicker_->hide();
            }
            else
            {
                emojiPicker_->popupAtRect(mainAppSurface_, btn);
            }
        };
        mainApp_->room_view()->on_sticker = [this](tk::Rect btn) {
            if (!stickerPicker_)
            {
                return;
            }
            if (stickerPicker_->isVisible())
            {
                stickerPicker_->hide();
            }
            else
            {
                stickerPicker_->popupAtRect(mainAppSurface_, btn);
            }
        };
        mainApp_->room_view()->on_image_clicked =
            [this](const tesseract::views::MessageListView::ImageHit& hit) {
                mainApp_->image_viewer()->open(hit.media_url, hit.body,
                                               hit.natural_w, hit.natural_h);
                mainApp_->show_image_viewer(true);
                mainAppSurface_->relayout();
                mainAppSurface_->setFocus();
                const std::string url = hit.media_url;
                if (!url.empty()
                    && !viewerFullresCache_.count(url)
                    && !anim_cache_.has(url)
                    && viewerFullresInFlight_.insert(url).second)
                {
                    runOnPool_([this, url]() {
                        auto bytes = client_->fetch_source_bytes(url);
                        QMetaObject::invokeMethod(this,
                            [this, url, bytes = std::move(bytes)]() mutable {
                                viewerFullresInFlight_.erase(url);
                                if (bytes.empty() || viewerFullresCache_.count(url))
                                {
                                    return;
                                }
                                QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                                              static_cast<int>(bytes.size()));
                                QBuffer buf(&qb);
                                buf.open(QIODevice::ReadOnly);
                                QImageReader reader(&buf);
                                reader.setAutoTransform(true);
                                QImage img;
                                if (!reader.read(&img))
                                {
                                    return;
                                }
                                viewerFullresCache_.emplace(
                                    url, tk::qt6::make_image(std::move(img)));
                                if (mainAppSurface_)
                                {
                                    mainAppSurface_->update();
                                }
                            }, Qt::QueuedConnection);
                    });
                }
            };
        mainApp_->room_view()->on_video_clicked =
            [this](const tesseract::views::MessageListView::VideoHit& hit) {
                mainApp_->video_viewer()->open(hit.source_json, hit.thumbnail_url,
                                               hit.mime_type, hit.duration_ms,
                                               hit.natural_w, hit.natural_h,
                                               hit.autoplay, hit.loop,
                                               hit.no_audio, hit.hide_controls);
                mainApp_->show_video_viewer(true);
                mainAppSurface_->relayout();
                mainAppSurface_->setFocus();
                std::string src = hit.source_json;
                runOnPool_([this, src = std::move(src)]() mutable {
                    auto bytes = client_->fetch_source_bytes(src);
                    QMetaObject::invokeMethod(this,
                        [this, bytes = std::move(bytes)]() mutable {
                            if (mainApp_)
                            {
                                mainApp_->video_viewer()->load_bytes(
                                    bytes.data(), bytes.size());
                            }
                        }, Qt::QueuedConnection);
                });
            };

        mainAppSurface_->set_root(std::move(main_app_owner));
    }

    // ---- Native overlays ----
    roomTextArea_ = mainAppSurface_->host().make_text_area();
    roomTextArea_->set_font_role(tk::FontRole::Body);
    roomTextArea_->set_text_color(mainAppSurface_->theme().palette.text_primary);
    roomTextArea_->set_placeholder(tr("Message\xe2\x80\xa6").toStdString());
    roomTextArea_->set_on_changed([this](const std::string& s) {
        if (mainApp_)
        {
            mainApp_->room_view()->set_current_text(s);
        }

        int cursor = (int)s.size();

        // Auto-expand: ":smile:" + space → replace with glyph
        auto complete = shortcode_engine_.find_complete(s, cursor);
        if (complete)
        {
            auto hits = shortcode_engine_.lookup(complete->prefix, cached_emoticons_, 1);
            std::string r = (!hits.empty() && !hits.front().glyph.empty())
                ? hits.front().glyph
                : ":" + complete->prefix + ":";
            roomTextArea_->replace_range(complete->start, complete->end, r);
            hide_shortcode_popup_();
            return;
        }

        // Popup: ":gri" → show suggestions
        auto prefix_match = shortcode_engine_.find_prefix(s, cursor);
        if (prefix_match && prefix_match->prefix.size() >= 2)
        {
            shortcode_current_suggestions_ = shortcode_engine_.lookup(
                prefix_match->prefix, cached_emoticons_);
            if (!shortcode_current_suggestions_.empty())
            {
                shortcode_active_match_ = *prefix_match;
                for (const auto& sugg : shortcode_current_suggestions_)
                {
                    if (!sugg.emoticon.url.empty())
                    {
                        ensure_media_image_(sugg.emoticon.url, 28, 28);
                    }
                }
                bool was_visible = shortcode_popup_visible_();
                show_shortcode_popup_(shortcode_current_suggestions_,
                                      roomTextArea_->cursor_rect());
                if (!was_visible)
                {
                    roomTextArea_->set_on_popup_nav([this](tk::NativeTextArea::NavKey nk) -> bool {
                        if (!shortcode_popup_visible_())
                        {
                            return false;
                        }
                        int cur = shortcode_popup_widget_->selected_index();
                        int n   = shortcode_popup_widget_->visible_rows();
                        if (n <= 0)
                        {
                            return true;
                        }
                        int next = cur;
                        switch (nk)
                        {
                        case tk::NativeTextArea::NavKey::Up:
                            next = std::max(0, cur - 1);
                            break;
                        case tk::NativeTextArea::NavKey::Down:
                            next = std::min(n - 1, cur + 1);
                            break;
                        case tk::NativeTextArea::NavKey::Tab:
                        {
                            int sel = shortcode_popup_widget_->selected_index();
                            if (sel >= 0 && sel < (int)shortcode_current_suggestions_.size())
                            {
                                auto& s = shortcode_current_suggestions_[sel];
                                std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
                                roomTextArea_->replace_range(
                                    shortcode_active_match_.start, shortcode_active_match_.end, r);
                            }
                            hide_shortcode_popup_();
                            return true;
                        }
                        case tk::NativeTextArea::NavKey::ShiftTab:
                            return false;
                        case tk::NativeTextArea::NavKey::Escape:
                            hide_shortcode_popup_();
                            return true;
                        }
                        shortcode_popup_widget_->set_selected_index(next);
                        shortcode_popup_surface_->update();
                        return true;
                    });
                }
                return;
            }
        }
        hide_shortcode_popup_();
    });
    roomTextArea_->set_on_submit([this] {
        if (shortcode_popup_visible_())
        {
            int sel = shortcode_popup_widget_->selected_index();
            if (sel >= 0 && sel < (int)shortcode_current_suggestions_.size())
            {
                auto& s = shortcode_current_suggestions_[sel];
                std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
                roomTextArea_->replace_range(
                    shortcode_active_match_.start, shortcode_active_match_.end, r);
                hide_shortcode_popup_();
                return;
            }
            // Nothing selected — dismiss popup and send the message
            hide_shortcode_popup_();
        }
        onSendClicked();
    });
    roomTextArea_->set_on_edit_last([this] {
        return mainApp_ && mainApp_->room_view()
            && mainApp_->room_view()->edit_last_own();
    });
    roomTextArea_->set_on_height_changed([this](float h) {
        if (!mainApp_ || !mainAppSurface_)
        {
            return;
        }
        mainApp_->room_view()->set_text_area_natural_height(h);
        mainAppSurface_->relayout();
    });
    roomTextArea_->set_on_image_paste(
        [this](std::vector<std::uint8_t> bytes, std::string mime) {
            if (mainApp_)
            {
                mainApp_->room_view()->compose_bar()->set_pending_image(
                    std::move(bytes), std::move(mime));
            }
        });

    roomSearchField_ = mainAppSurface_->host().make_text_field();
    roomSearchField_->set_placeholder(tr("Search rooms\xe2\x80\xa6").toStdString());
    roomSearchField_->set_on_changed([this](const std::string& s) {
        roomSearchPendingText_ = s;
        if (mainApp_)
        {
            mainApp_->room_list_view()->set_search_text(s);
        }
        refreshRoomList();
    });

    recoveryKeyField_ = mainAppSurface_->host().make_text_field();
    recoveryKeyField_->set_password(true);

    mainAppSurface_->set_on_layout([this] {
        if (mainApp_ && roomTextArea_)
        {
            roomTextArea_->set_rect(mainApp_->compose_text_area_rect());
        }
        if (mainApp_ && roomSearchField_)
        {
            roomSearchField_->set_visible(mainApp_->room_search_field_visible());
            roomSearchField_->set_rect(mainApp_->room_search_field_rect());
        }
        if (mainApp_ && recoveryKeyField_)
        {
            recoveryKeyField_->set_visible(mainApp_->recovery_key_field_visible());
            recoveryKeyField_->set_rect(mainApp_->recovery_key_field_rect());
        }
    });

    mainAppSurface_->set_on_file_drop([this](std::vector<std::uint8_t> bytes,
                                              std::string mime,
                                              std::string filename) {
        if (!mainApp_)
        {
            return;
        }
        if (mime.starts_with("image/"))
        {
            mainApp_->room_view()->compose_bar()->set_pending_image(
                std::move(bytes), std::move(mime), std::move(filename));
        }
        else
        {
            mainApp_->room_view()->compose_bar()->set_pending_file(
                std::move(bytes), std::move(mime), std::move(filename));
        }
    });

    // Right-click: user-strip actions (lower-left corner) or sticker save (chat).
    mainAppSurface_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mainAppSurface_, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        using namespace tesseract::visual;
        const bool in_user_strip =
            pos.x() < kSidebarWidth
            && pos.y() >= mainAppSurface_->height() - kUserStripHeight;
        if (in_user_strip)
        {
            onUserStripContextMenu(mainAppSurface_->mapToGlobal(pos));
            return;
        }
        if (!mainApp_)
        {
            return;
        }
        auto hit = mainApp_->room_view()->message_list()->sticker_hit_at(
            tk::Point{ static_cast<float>(pos.x()),
                       static_cast<float>(pos.y()) });
        if (!hit)
        {
            return;
        }
        const bool already_saved = client_->user_pack_has_sticker(hit->mxc_url);
        const auto mxc_url   = hit->mxc_url;
        const auto body      = hit->body;
        const auto info_json = hit->info_json;
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QAction* add = menu->addAction(already_saved
            ? tr("Already in Saved Stickers")
            : tr("Add to Saved Stickers"));
        add->setEnabled(!already_saved);
        if (!already_saved)
        {
            connect(add, &QAction::triggered, this, [this, mxc_url, body, info_json]{
                auto res = client_->save_sticker_to_user_pack(body, body, mxc_url, info_json);
                if (!res.ok)
                    statusBar()->showMessage(
                        QString::fromStdString(res.message), 6000);
            });
        }
        menu->popup(mainAppSurface_->mapToGlobal(pos));
    });

    // Emoji picker: build the floating panel, wire selection → cursor
    // insert + account-data bump. Recents live in the SDK now (synced via
    // `io.element.recent_emoji`), so no local-disk load is needed.
    emojiPicker_ = new EmojiPicker(this);
    emojiPicker_->setClient(client_);
    emojiPicker_->setImageProvider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/false);
            return nullptr;
        });
    emojiPicker_->onSelected = [this](const QString& glyph) {
        // Reaction mode: a message's "+" chip set pendingReactionEventId_
        // before opening the picker. Route the glyph through
        // send_reaction (toggle semantics handled Rust-side) and skip
        // the compose-bar insert.
        if (!pendingReactionEventId_.empty())
        {
            std::string ev = std::move(pendingReactionEventId_);
            pendingReactionEventId_.clear();
            if (!current_room_id_.empty())
            {
                client_->send_reaction(current_room_id_, ev, glyph.toStdString());
            }
            client_->recent_emoji_bump(glyph.toStdString());
            emojiPicker_->hide();
            return;
        }
        if (!roomTextArea_)
        {
            return;
        }
        roomTextArea_->insert_at_cursor(glyph.toStdString());
        if (mainApp_)
        {
            mainApp_->room_view()->set_current_text(roomTextArea_->text());
        }
        roomTextArea_->set_focused(true);
        client_->recent_emoji_bump(glyph.toStdString());
    };

    emojiPicker_->onEmoticonSelected = [this](const tesseract::ImagePackImage& img) {
        if (!roomTextArea_)
        {
            return;
        }
        roomTextArea_->insert_at_cursor(":" + img.shortcode + ":");
        if (mainApp_)
        {
            mainApp_->room_view()->set_current_text(roomTextArea_->text());
        }
        roomTextArea_->set_focused(true);
    };

    // Sticker picker: floating panel anchored at the compose-bar sticker
    // button. On selection, send `m.sticker` to the current room (matrix-
    // sdk encrypts transparently in E2EE rooms).
    stickerPicker_ = new StickerPicker(this);
    stickerPicker_->setClient(client_);
    stickerPicker_->setImageProvider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/true);
            return nullptr;
        });
    stickerPicker_->onSelected =
        [this](const tesseract::ImagePackImage& img) {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string body = img.body.empty() ? img.shortcode : img.body;
            client_->send_sticker(current_room_id_, body, img.url, img.info_json);
            stickerPicker_->hide();
        };

    joinRoomDialog_ = new JoinRoomDialog(this);
    joinRoomDialog_->setClient(client_);
    joinRoomDialog_->setAvatarProvider(
        [this](const std::string& mxc_url) -> const tk::Image* {
            auto it = tk_avatars_.find(mxc_url);
            return (it != tk_avatars_.end()) ? it->second.get() : nullptr;
        });
    joinRoomDialog_->onJoined = [this](const std::string& room_id) {
        navigate_to_room(room_id);
    };

    statusBar()->showMessage(tr("Not logged in"));

    // Load saved theme preference and apply it.
    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());
    read_portal_color_scheme_();
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("SettingChanged"),
        this,
        SLOT(on_portal_setting_changed_(QString, QString, QDBusVariant)));
    apply_current_theme_();

    // Re-apply when the OS switches light/dark (only relevant in System mode).
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this] {
                if (tesseract::Settings::instance().theme_pref ==
                    tesseract::Settings::ThemePreference::System)
                {
                    apply_current_theme_();
                }
            });

    // Room selection is delivered through RoomListView's on_room_selected
    // callback wired in the surface-construction block above.

    // Bridges live in `accounts_[].bridge`; EventHandlerBase routes all
    // callbacks through post_to_ui_ → handle_*_ui_() on MainWindow.

    // Notifiers are created per-account in doLogin / onLoginSucceeded.

    // Animation frame-tick for inline media in the timeline (GIF /
    // animated WebP / APNG). 60 Hz; the timer self-stops in
    // `onMessageAnimTick_` when `anim_cache_` empties.
    tk_anim_timer_ = new QTimer(this);
    tk_anim_timer_->setInterval(16);
    connect(tk_anim_timer_, &QTimer::timeout,
            this, &MainWindow::onMessageAnimTick_);

    QMetaObject::invokeMethod(this, &MainWindow::doLogin, Qt::QueuedConnection);
}

MainWindow::~MainWindow()
{
    // Drain ALL background workers before tearing the clients down.
    //
    // Two independent worker systems must both be drained:
    //
    //  1. mediaPool_ (QThreadPool) — used by runOnPool_() for pagination.
    //     Guarded by shuttingDown_; drained by mediaPool_.waitForDone(-1).
    //
    //  2. ShellBase detached std::threads — used by run_async_() for avatar /
    //     media fetches (ensure_room_avatar_, ensure_user_avatar_,
    //     ensure_media_image_).  Guarded by shutting_down_ (ShellBase);
    //     tracked by workers_in_flight_ / workers_cv_.
    //
    // Both flags must be flipped first so no new work is enqueued after the
    // clear/drain calls.  stop_sync() is called before the waits so the
    // Rust-side cancellation channel fires and any worker blocked inside a
    // tokio block_on() returns promptly instead of waiting on a network hop.
    // ~ClientFfi calls stop_sync() a second time as a no-op safety net
    // (handler.take() returns None on repeated calls).
    shuttingDown_.store(true, std::memory_order_release);
    shutting_down_.store(true, std::memory_order_release);
    mediaPool_.clear();
    for (auto& a : accounts_)
    {
        if (a && a->client)
        {
            a->client->stop_sync();
        }
    }
    if (pending_login_client_)
    {
        pending_login_client_->stop_sync();
    }
    // Drain ShellBase detached threads (avatar / media fetches).
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                              [this]{ return workers_in_flight_ == 0; });
    }
    // Drain Qt pool workers (pagination).  Unbounded: shutting_down_ is true
    // so no new runnables can be queued, and clear() removed pending ones.
    mediaPool_.waitForDone(-1);

    client_        = nullptr;
    event_handler_ = nullptr;

    // LoginView is a child widget, normally destroyed during ~QMainWindow
    // — but that runs *after* the AccountSessions (and thus their
    // Clients) are destroyed, and ~LoginView calls cancel_oauth on its
    // bound client. Tear it down here while everything is still alive.
    delete loginView_;
    loginView_ = nullptr;
}

void MainWindow::runOnPool_(std::function<void()> fn)
{
    if (shuttingDown_.load(std::memory_order_acquire))
    {
        return;
    }
    auto* runner = QRunnable::create([this, fn = std::move(fn)]() mutable {
        if (shuttingDown_.load(std::memory_order_acquire))
        {
            return;
        }
        fn();
    });
    mediaPool_.start(runner);
}

// ---------------------------------------------------------------------------

// eventFilter is defined further down (multi-account section) so it can
// dispatch user-strip left-clicks into the AccountPicker popover.

void MainWindow::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape)
    {
        if (mainApp_ && mainApp_->video_viewer()->is_open())
        {
            mainApp_->video_viewer()->close();
            mainApp_->show_video_viewer(false);
            mainAppSurface_->relayout();
            ev->accept();
            return;
        }
        if (mainApp_ && mainApp_->image_viewer()->is_open())
        {
            mainApp_->image_viewer()->close();
            mainApp_->show_image_viewer(false);
            mainAppSurface_->relayout();
            ev->accept();
            return;
        }
    }
    QMainWindow::keyPressEvent(ev);
}

void MainWindow::resizeEvent(QResizeEvent* ev)
{
    QMainWindow::resizeEvent(ev);
}

// ---------------------------------------------------------------------------

void MainWindow::doLogin()
{
    // One-shot migration from the legacy single-account layout. Runs once
    // per install before any Client is constructed; idempotent on every
    // subsequent launch.
    tesseract::SessionStore::migrate_legacy_layout();

    // Restore every account on disk, in index order, so notifications
    // fire for any of them while the user works in the foreground one.
    auto idx = tesseract::SessionStore::load_index();

    if (idx.user_ids.empty())
    {
        // Fresh install: no accounts → initial LoginView, no Cancel.
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        loginView_->set_client(pending_login_client_.get());
        loginView_->set_on_begin_oauth([this] {
            if (!pending_login_temp_dir_.empty())
            {
                return;
            }
            pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
            std::error_code ec;
            std::filesystem::create_directories(pending_login_temp_dir_, ec);
            pending_login_client_->set_data_dir(
                (pending_login_temp_dir_ / "matrix-store").string());
        });
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Not logged in"));
        return;
    }

    statusBar()->showMessage(tr("Restoring sessions\xe2\x80\xa6"));

    int target_active = -1;
    for (std::size_t i = 0; i < idx.user_ids.size(); ++i)
    {
        const auto& uid = idx.user_ids[i];
        auto json = tesseract::SessionStore::load_account(uid);
        if (!json)
        {
            continue;
        }

        auto session = std::make_unique<tesseract::AccountSession>();
        session->user_id = uid;
        session->client  = std::make_unique<tesseract::Client>();
        session->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());

        auto res = session->client->restore_session(*json);
        if (!res)
        {
            // This account's session is bad — wipe it from disk so the
            // next launch doesn't keep retrying.
            tesseract::SessionStore::clear_account(uid);
            statusBar()->showMessage(
                tr("Account %1 expired: %2")
                    .arg(QString::fromStdString(uid),
                         QString::fromStdString(res.message)),
                6000);
            continue;
        }
        session->display_name = session->client->get_display_name();
        session->avatar_url   = session->client->get_avatar_url();
        session->last_room    = tesseract::Prefs::parse(
            session->client->load_prefs_json()).last_room;

        auto bridge = std::make_unique<EventBridge>(this, this);
        bridge->set_user_id(uid);
        session->client->start_sync(bridge.get());
        session->sync_started = true;
        session->bridge = std::move(bridge);

        // Per-account notifier: click switches to this account then navigates.
        session->notifier = std::make_unique<LinuxNotifierQt>(
            [this, uid](std::string room_id, std::string token) {
                for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
                {
                    if (accounts_[i]->user_id == uid)
                    {
                        switchActiveAccount(i);
                        break;
                    }
                }
                // Pass xdg_activation_v1 token (non-empty on modern Wayland)
                // so the compositor grants window focus on navigate_to_room.
                if (!token.empty() && windowHandle())
                {
                    windowHandle()->setProperty(
                        "_q_waylandActivationToken",
                        QString::fromStdString(token));
                }
                navigate_to_room(std::move(room_id));
            });

        // Per-account UnifiedPush connector (registers with distributor on start).
        auto up = std::make_unique<LinuxUpConnectorQt>();
        up->start(session->client.get(), uid);
        session->up_connector = std::move(up);

        if (uid == idx.active_user_id)
        {
            target_active = static_cast<int>(accounts_.size());
        }
        accounts_.push_back(std::move(session));
    }

    if (accounts_.empty())
    {
        // Every stored account failed to restore — drop to initial login.
        tesseract::SessionStore::save_index({});
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        loginView_->set_client(pending_login_client_.get());
        loginView_->set_on_begin_oauth([this] {
            if (!pending_login_temp_dir_.empty())
            {
                return;
            }
            pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
            std::error_code ec;
            std::filesystem::create_directories(pending_login_temp_dir_, ec);
            pending_login_client_->set_data_dir(
                (pending_login_temp_dir_ / "matrix-store").string());
        });
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Not logged in"));
        return;
    }

    if (target_active < 0)
    {
        target_active = 0;
    }
    switchActiveAccount(target_active);
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainAppSurface_);
    maybeShowRecoveryBanner();

    if (!tray_)
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this]{ show(); raise(); activateWindow(); },
            [this]{
                if (isVisible())
                {
                    hide();
                }
                else
                {
                    show();
                    raise();
                    activateWindow();
                }
            },
            []{ qApp->quit(); },
            this);
        if (tray_->is_available())
        {
            qApp->setQuitOnLastWindowClosed(false);
        }
    }
}

void MainWindow::onLoginSucceeded()
{
    if (!pending_login_client_)
    {
        return;   // defensive — should not happen
    }

    // The OAuth round-trip completed on `pending_login_client_`, which the
    // LoginView drove against a temporary data dir
    // (`pending_login_temp_dir_/matrix-store`). We don't yet know the
    // user_id; ask the client now.
    std::string user_id = pending_login_client_->get_user_id();
    if (user_id.empty())
    {
        statusBar()->showMessage(tr("Sign-in failed: no user id"), 6000);
        return;
    }

    // If an account with the same user_id is already signed in (the user
    // added an account they're already logged into), refuse rather than
    // colliding on disk.
    for (auto& a : accounts_)
    {
        if (a->user_id == user_id)
        {
            statusBar()->showMessage(
                tr("Already signed in as %1")
                    .arg(QString::fromStdString(user_id)),
                4000);
            pending_login_client_.reset();
            loginView_->set_client(nullptr);
            std::error_code ec;
            std::filesystem::remove_all(pending_login_temp_dir_, ec);
            // Restore previous active account's UI.
            if (add_account_return_idx_ >= 0
                && add_account_return_idx_ < static_cast<int>(accounts_.size()))
            {
                switchActiveAccount(add_account_return_idx_);
                contentStack_->setCurrentWidget(mainAppSurface_);
            }
            pending_login_is_add_account_ = false;
            add_account_return_idx_ = -1;
            return;
        }
    }

    // Snapshot the session blob before we drop the in-flight Client —
    // re-opening it below needs to restore from this JSON.
    auto session_json = pending_login_client_->export_session();
    if (session_json.empty())
    {
        statusBar()->showMessage(tr("Sign-in failed: empty session"), 6000);
        return;
    }

    // Drop the in-flight Client so its SQLite handles are released
    // before we rename the directory underneath it.
    pending_login_client_.reset();
    loginView_->set_client(nullptr);

    // Move the temp account directory into its final per-user-id home.
    std::filesystem::path final_dir = tesseract::SessionStore::account_dir(user_id);
    {
        std::error_code ec;
        std::filesystem::create_directories(final_dir.parent_path(), ec);
        std::filesystem::rename(pending_login_temp_dir_, final_dir, ec);
        if (ec)
        {
            // Rename failed — try recursive copy + remove, falling back
            // to leaving the data in the temp dir if even that fails.
            std::error_code ec2;
            std::filesystem::copy(pending_login_temp_dir_, final_dir,
                std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::overwrite_existing, ec2);
            if (ec2)
            {
                statusBar()->showMessage(
                    tr("Sign-in failed: couldn't persist matrix store: %1")
                        .arg(QString::fromStdString(ec2.message())),
                    6000);
                return;
            }
            std::filesystem::remove_all(pending_login_temp_dir_, ec2);
        }
    }
    pending_login_temp_dir_.clear();

    // Persist the session blob into the final per-account dir.
    if (!tesseract::SessionStore::save_account(user_id, session_json))
    {
        statusBar()->showMessage(tr("Sign-in failed: couldn't persist session"), 6000);
        return;
    }

    // Open a fresh Client against the final store path and restore from
    // the just-exported session JSON (the matrix-sdk reuses the moved
    // SQLite store transparently — no resync).
    auto session = std::make_unique<tesseract::AccountSession>();
    session->user_id = user_id;
    session->client  = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(user_id).string());
    auto res = session->client->restore_session(session_json);
    if (!res)
    {
        statusBar()->showMessage(
            tr("Sign-in failed at restore: %1")
                .arg(QString::fromStdString(res.message)),
            6000);
        tesseract::SessionStore::clear_account(user_id);
        return;
    }
    session->display_name = session->client->get_display_name();
    session->avatar_url   = session->client->get_avatar_url();
    session->last_room    = tesseract::Prefs::parse(
        session->client->load_prefs_json()).last_room;

    auto bridge = std::make_unique<EventBridge>(this, this);
    bridge->set_user_id(user_id);
    session->client->start_sync(bridge.get());
    session->sync_started = true;
    session->bridge = std::move(bridge);

    // Per-account notifier: click switches to this account then navigates.
    session->notifier = std::make_unique<LinuxNotifierQt>(
        [this, uid = user_id](std::string room_id, std::string token) {
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
            {
                if (accounts_[i]->user_id == uid)
                {
                    switchActiveAccount(i);
                    break;
                }
            }
            if (!token.empty() && windowHandle())
            {
                windowHandle()->setProperty(
                    "_q_waylandActivationToken",
                    QString::fromStdString(token));
            }
            navigate_to_room(std::move(room_id));
        });

    // Per-account UnifiedPush connector.
    {
        auto up = std::make_unique<LinuxUpConnectorQt>();
        up->start(session->client.get(), user_id);
        session->up_connector = std::move(up);
    }

    int new_idx = static_cast<int>(accounts_.size());
    accounts_.push_back(std::move(session));

    // Update the on-disk index. Active = the account we just added.
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = user_id;
    for (auto& a : accounts_)
    {
        idx.user_ids.push_back(a->user_id);
    }
    tesseract::SessionStore::save_index(idx);

    switchActiveAccount(new_idx);
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainAppSurface_);
    maybeShowRecoveryBanner();

    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;

    if (!tray_)
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this]{ show(); raise(); activateWindow(); },
            [this]{
                if (isVisible())
                {
                    hide();
                }
                else
                {
                    show();
                    raise();
                    activateWindow();
                }
            },
            []{ qApp->quit(); },
            this);
        if (tray_->is_available())
        {
            qApp->setQuitOnLastWindowClosed(false);
        }
    }
}

void MainWindow::onLoginCancelled()
{
    // The user clicked Cancel during AddAccount. Return to the previous
    // foreground account; the in-flight client is discarded.
    pending_login_client_.reset();
    loginView_->set_client(nullptr);
    if (!pending_login_is_add_account_)
    {
        return;   // no back-state in Initial mode
    }

    int back = add_account_return_idx_;
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
    if (back >= 0 && back < static_cast<int>(accounts_.size()))
    {
        switchActiveAccount(back);
        contentStack_->setCurrentWidget(mainAppSurface_);
    }
}

void MainWindow::closeEvent(QCloseEvent* ev)
{
    if (tray_ && tray_->is_available())
    {
        ev->ignore();
        hide();
        return;
    }
    QMainWindow::closeEvent(ev);
}

void MainWindow::navigate_to_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    if (mainApp_)
    {
        mainApp_->room_list_view()->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
    show();
    raise();
    activateWindow();
}


void MainWindow::onSendClicked()
{
    if (mainApp_)
    {
        mainApp_->room_view()->compose_bar()->trigger_send();
    }
}

void MainWindow::onRoomSelected(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }

    // Drill into a space if the clicked row is one.
    for (const auto& r : rooms_)
    {
        if (r.id == room_id && r.is_space)
        {
            space_stack_.push_back(room_id);
            refreshRoomList();
            return;
        }
    }

    hide_shortcode_popup_();
    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id
            && room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }

    current_room_id_ = room_id;
    clear_focused_state_(room_id);
    if (!markReadTimer_)
    {
        markReadTimer_ = new QTimer(this);
        markReadTimer_->setSingleShot(true);
        connect(markReadTimer_, &QTimer::timeout, this, [this] {
            mark_room_read_(current_room_id_);
        });
    }
    markReadTimer_->start(tesseract::Settings::instance().mark_as_read_delay_ms);
    update_typing_bar_({}, false);
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = current_room_id_;
        client_->save_prefs_json(tesseract::Prefs::serialize(prefs));
    }
    if (mainApp_)
    {
        mainApp_->room_view()->compose_bar()->clear_reply();
        mainApp_->room_view()->compose_bar()->clear_editing();
        mainApp_->room_view()->clear_compose_text();
    }
    if (roomTextArea_)
    {
        roomTextArea_->set_text("");
    }
    if (roomTextArea_)
    {
        roomTextArea_->set_focused(true);
    }

    for (const auto& r : rooms_)
    {
        if (r.id == current_room_id_)
        {
            if (mainApp_)
            {
                mainApp_->room_view()->set_room(r);
            }
            break;
        }
    }

    // subscribe_room + paginate_back both block inside the Rust runtime;
    // run them on a worker thread so the UI stays responsive during the
    // first-load network round-trip.
    auto visible_ids = mainApp_ ? mainApp_->room_list_view()->visible_room_ids()
                                : std::vector<std::string>{};
    std::string sub_room = current_room_id_;
    // Snapshot client_ on the GUI thread: an account switch while this
    // worker runs would otherwise race the raw pointer and could subscribe /
    // paginate against the wrong account's client.
    auto* c = client_;
    if (!c)
    {
        return;
    }
    runOnPool_([this, c, sub_room, visible_ids = std::move(visible_ids)]{
        auto res = c->subscribe_room(sub_room);
        bool ok  = res.ok;
        std::string msg = res.message;
        bool reached = false;
        if (ok)
        {
            auto pr = c->paginate_back_with_status(sub_room, kPaginationBatch);
            reached = pr.ok && pr.reached_start;
            c->start_background_backfill(visible_ids);
        }
        QMetaObject::invokeMethod(
            this,
            [this, sub_room, ok, msg = std::move(msg), reached]() mutable {
                if (!ok)
                {
                    statusBar()->showMessage(
                        tr("Subscribe failed: %1").arg(
                            QString::fromStdString(msg)), 4000);
                    return;
                }
                if (current_room_id_ == sub_room)
                {
                    auto& state = pagination_[sub_room];
                    state.in_flight     = false;
                    state.reached_start = reached;
                }
            },
            Qt::QueuedConnection);
    });
}


void MainWindow::requestMoreHistory(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    auto& state = pagination_[room_id];
    if (state.in_flight || state.reached_start)
    {
        return;
    }
    state.in_flight = true;

    // Run the blocking SDK call off the UI thread; bounce the result back
    // via a queued connection. `client_` is thread-safe (Rust runtime
    // serialises concurrent calls).
    auto* c = client_;   // snapshot: avoid account-switch race on client_
    if (!c)
    {
        state.in_flight = false;
        return;
    }
    runOnPool_([this, c, room_id]{
        auto res = c->paginate_back_with_status(room_id, kPaginationBatch);
        bool reached = res.ok && res.reached_start;
        QMetaObject::invokeMethod(
            this,
            "onPaginateFinished",
            Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(room_id)),
            Q_ARG(bool, reached));
    });
}

void MainWindow::openJumpToDateDialog()
{
    if (current_room_id_.empty())
    {
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Jump to Date"));
    auto* cal = new QCalendarWidget(&dlg);
    cal->setMinimumDate(QDate(1970, 1, 1));
    cal->setMaximumDate(QDate::currentDate());
    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* lo = new QVBoxLayout(&dlg);
    lo->addWidget(cal);
    lo->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted)
    {
        return;
    }


    const QDate date = cal->selectedDate();
    const uint64_t ts_ms = static_cast<uint64_t>(
        QDateTime(date, QTime(0, 0, 0), QTimeZone::utc()).toMSecsSinceEpoch());

    const std::string room_id = current_room_id_;
    runOnPool_([this, room_id, ts_ms] {
        auto res = client_->timestamp_to_event(room_id, ts_ms, "f");
        if (!res.ok)
        {
            QMetaObject::invokeMethod(this, [this, msg = res.message] {
                statusBar()->showMessage(
                    tr("Jump to date failed: %1")
                        .arg(QString::fromStdString(msg)), 4000);
            }, Qt::QueuedConnection);
            return;
        }
        const std::string event_id = res.message;
        QMetaObject::invokeMethod(this, [this, room_id, event_id] {
            begin_focused_subscription_(room_id, event_id);
            runOnPool_([this, room_id, event_id] {
                client_->subscribe_room_at(room_id, event_id);
            });
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onPaginateFinished(QString roomId, bool reached_start)
{
    const std::string rid = roomId.toStdString();
    bool is_current = (rid == current_room_id_);
    push_paginate_result_(rid, reached_start);
    if (is_current && mainApp_)
    {
        mainApp_->room_view()->message_list()->reset_near_top_latch();
    }
}


void MainWindow::clearMessages()
{
    if (mainApp_)
    {
        mainApp_->room_view()->clear_room();
        mainApp_->room_view()->set_messages({});
    }
}

// ---------------------------------------------------------------------------
// ShellBase virtual hook implementations
// ---------------------------------------------------------------------------

void MainWindow::post_to_ui_(std::function<void()> fn)
{
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

void MainWindow::on_rooms_updated_()
{
    refreshRoomList();
    if (!current_room_id_.empty())
    {
        for (const auto& r : rooms_)
        {
            if (r.id == current_room_id_)
            {
                if (mainApp_)
                {
                    mainApp_->room_view()->set_room(r);
                }
                break;
            }
        }
    }
    else if (!pending_restore_room_.empty())
    {
        for (const auto& r : rooms_)
        {
            if (r.id == pending_restore_room_ && !r.is_space)
            {
                std::string target = std::move(pending_restore_room_);
                pending_restore_room_.clear();
                onRoomSelected(target);
                break;
            }
        }
    }

    for (const auto& [room_id, w] : secondary_windows_)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == room_id)
            {
                w->on_room_info_updated(r);
                break;
            }
        }
    }
}

void MainWindow::on_media_bytes_ready_(const std::string& cache_key,
                                        MediaKind kind,
                                        std::vector<uint8_t> bytes)
{
    // Called on the UI thread (already posted via post_to_ui_ in ShellBase).
    if (bytes.empty())
    {
        mediaImageSizes_.erase(cache_key);
        return;
    }

    if (kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar)
    {
        if (tk_avatars_.count(cache_key))
        {
            return;
        }
        QImage img;
        if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                              static_cast<int>(bytes.size())))
        {
            return;
        }
        const int size = (kind == MediaKind::RoomAvatar)
                          ? kRoomAvatarSize : kMsgAvatarSize;
        QImage scaled = img.scaled(size, size,
                                    Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        tk_avatars_.emplace(cache_key, tk::qt6::make_image(std::move(scaled)));
        if (mainAppSurface_)
        {
            mainAppSurface_->update();
        }
        return;
    }

    if (kind == MediaKind::Tile)
    {
        if (tk_images_.count(cache_key)) return;
        QImage img;
        if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                              static_cast<int>(bytes.size())))
        {
            return;
        }
        tk_images_.emplace(cache_key, tk::qt6::make_image(std::move(img)));
        if (mainApp_)
            mainApp_->room_view()->message_list()->invalidate_data();
        if (mainAppSurface_)
        {
            mainAppSurface_->relayout();
            mainAppSurface_->update();
        }
        return;
    }

    // MediaImage — decode (same path as pickers) then store. Decode stays
    // on the UI thread here (unchanged behaviour); pickers decode on a
    // worker via ensure_picker_image_.
    if (tk_images_.count(cache_key) || anim_cache_.has(cache_key)) {
        mediaImageSizes_.erase(cache_key);
        return;
    }
    int max_w = kMaxImageWidth, max_h = kMaxImageHeight;
    if (auto sit = mediaImageSizes_.find(cache_key);
        sit != mediaImageSizes_.end()) {
        max_w = sit->second.first;
        max_h = sit->second.second;
        mediaImageSizes_.erase(sit);
    }
    DecodedImage d = decode_image_(bytes, max_w, max_h);
    if (!d.frames.empty()) {
        anim_cache_.store(cache_key, std::move(d.frames),
                          std::move(d.delays_ms),
                          QDateTime::currentMSecsSinceEpoch());
        if (tk_anim_timer_ && !tk_anim_timer_->isActive())
            tk_anim_timer_->start();
    } else if (d.still) {
        tk_images_.emplace(cache_key, std::move(d.still));
    } else {
        return;
    }
    if (mainApp_) mainApp_->room_view()->notify_image_ready(cache_key);
    if (mainAppSurface_) { mainAppSurface_->relayout(); mainAppSurface_->update(); }
    if (shortcode_popup_visible_() && shortcode_popup_surface_)
        shortcode_popup_surface_->update();
    return;
}

MainWindow::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes,
                          int max_w, int max_h) {
    DecodedImage d;
    if (bytes.empty()) return d;
    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    if (reader.supportsAnimation() && reader.imageCount() > 1) {
        QImage frame;
        while (reader.read(&frame)) {
            int delay = reader.nextImageDelay();
            if (delay <= 0) delay = 100;
            if (delay < 20) delay = 20;
            QImage scaled = frame.scaled(max_w, max_h,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
            d.frames.push_back(tk::qt6::make_image(std::move(scaled)));
            d.delays_ms.push_back(delay);
        }
        if (!d.frames.empty()) return d;
        d.delays_ms.clear();
        buf.seek(0);
    }
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(qb.constData()),
                          qb.size()))
        return d;
    QImage scaled = img.scaled(max_w, max_h,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    d.still = tk::qt6::make_image(std::move(scaled));
    return d;
}

std::int64_t MainWindow::monotonic_ms_() {
    return QDateTime::currentMSecsSinceEpoch();
}

void MainWindow::start_anim_tick_() {
    if (tk_anim_timer_ && !tk_anim_timer_->isActive())
        tk_anim_timer_->start();
}

void MainWindow::repaint_pickers_() {
    if (emojiPicker_)   emojiPicker_->invalidateImages();
    if (stickerPicker_) stickerPicker_->invalidateImages();
    if (mainAppSurface_) { mainAppSurface_->relayout(); mainAppSurface_->update(); }
    if (shortcode_popup_visible_() && shortcode_popup_surface_)
        shortcode_popup_surface_->update();
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                            const std::string& video_url)
{
    const std::string src = video_url;
    runOnPool_([this, eid = event_id, src]() {
        auto bytes = client_->fetch_source_bytes(src);
        if (bytes.empty())
        {
            return;
        }
        // Decode the first frame on the UI thread — Qt multimedia
        // objects (QMediaPlayer, QVideoSink) must live there.
        QMetaObject::invokeMethod(this,
            [this, eid, bytes = std::move(bytes)]() mutable {
                const std::string key = "thumb::" + eid;
                if (tk_images_.count(key))
                {
                    return;
                }
                auto* player = new QMediaPlayer(this);
                auto* sink   = new QVideoSink(player);
                player->setVideoSink(sink);
                auto* buf = new QBuffer(player);
                QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<qsizetype>(bytes.size()));
                buf->setData(ba);
                buf->open(QIODevice::ReadOnly);
                player->setSourceDevice(buf);
                QObject::connect(
                    sink, &QVideoSink::videoFrameChanged,
                    sink, [this, key, player](const QVideoFrame& frame) {
                        if (!frame.isValid())
                        {
                            return;
                        }
                        player->stop();
                        player->deleteLater();
                        if (tk_images_.count(key))
                        {
                            return;
                        }
                        QImage img = frame.toImage();
                        if (img.isNull())
                        {
                            return;
                        }
                        QByteArray enc;
                        QBuffer encbuf(&enc);
                        encbuf.open(QIODevice::WriteOnly);
                        img.save(&encbuf, "JPEG", 85);
                        if (!enc.isEmpty())
                        {
                            std::vector<uint8_t> v(
                                reinterpret_cast<const uint8_t*>(enc.constData()),
                                reinterpret_cast<const uint8_t*>(enc.constData()) + enc.size());
                            on_media_bytes_ready_(key, MediaKind::MediaImage,
                                                  std::move(v));
                        }
                    });
                player->play();
            }, Qt::QueuedConnection);
    });
}

void MainWindow::onMessageAnimTick_()
{
    if (anim_cache_.empty())
    {
        if (tk_anim_timer_)
        {
            tk_anim_timer_->stop();
        }
        return;
    }
    if (anim_cache_.advance(QDateTime::currentMSecsSinceEpoch()) && mainAppSurface_)
    {
        mainAppSurface_->update();
    }
}

void MainWindow::on_url_preview_ready_(const std::string& url,
                                        const tesseract::Client::UrlPreview& preview)
{
    tesseract::views::UrlPreviewData d;
    d.title       = preview.title;
    d.description = preview.description;
    d.image_mxc   = preview.image_mxc;
    d.image_w     = preview.image_w;
    d.image_h     = preview.image_h;
    url_preview_data_.emplace(url, std::move(d));

    if (!preview.image_mxc.empty())
    {
        ensure_media_image_(preview.image_mxc, 64, 64);
    }

    // Invalidate cached row heights so the preview card is included in the
    // next measure pass, then relayout to apply the new heights.
    if (mainApp_)
    {
        mainApp_->room_view()->notify_url_preview_ready(url);
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
        mainAppSurface_->update();
    }

    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
            w->request_relayout();
        }
    }
}

void MainWindow::on_url_preview_failed_(const std::string& url)
{
    // No card to show (height unchanged) — just release the room-switch
    // gate so it doesn't wait the full timeout on a dead link.
    if (mainApp_)
        mainApp_->room_view()->notify_url_preview_ready(url);
    for (const auto& [rid, w] : secondary_windows_)
        if (w->room_view())
            w->room_view()->notify_url_preview_ready(url);
}

void MainWindow::cache_rgba_image_(const std::string& key, int w, int h,
                                    std::vector<uint8_t> rgba)
{
    if (tk_images_.count(key))
    {
        return;
    }
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), rgba.data(), rgba.size());
    tk_images_.emplace(key, tk::qt6::make_image(std::move(img)));
    if (mainAppSurface_)
    {
        mainAppSurface_->update();
    }
}

void MainWindow::showRooms(const std::vector<tesseract::RoomInfo>& rooms)
{
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms)
    {
        if (!r.is_space)
        {
            sorted.push_back(r);
        }
    }
    for (const auto& r : rooms)
    {
        if (r.is_space)
        {
            sorted.push_back(r);
        }
    }

    // Eagerly fetch avatars for the new room set so the first paint has
    // them ready. Bytes-already-cached is a no-op via tk_avatars_.count.
    for (const auto& r : sorted)
    {
        ensure_room_avatar_(r);
    }

    if (!mainApp_)
    {
        return;
    }
    mainApp_->room_list_view()->set_rooms(std::move(sorted));
    if (!current_room_id_.empty())
    {
        mainApp_->room_list_view()->set_selected_room(current_room_id_);
    }
    mainAppSurface_->relayout();
}

void MainWindow::refreshRoomList()
{
    // Both branches below dereference client_ (space_children). After logout
    // client_ is null; show an empty list rather than crash.
    if (!client_)
    {
        showRooms({});
        if (mainApp_)
        {
            mainApp_->set_space_nav(false);
        }
        return;
    }
    if (space_stack_.empty())
    {
        if (!roomSearchPendingText_.empty())
        {
            showRooms(rooms_);
            if (mainApp_)
            {
                mainApp_->set_space_nav(false);
            }
            return;
        }
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
        {
            if (!r.is_space)
            {
                continue;
            }
            for (const auto& id : client_->space_children(r.id))
            {
                in_space.insert(id);
            }
        }
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (!r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        for (const auto& r : rooms_)
        {
            if (r.is_space && (!in_space.count(r.id) || r.is_favorite))
            {
                filtered.push_back(r);
            }
        }
        showRooms(filtered);
        if (mainApp_)
        {
            mainApp_->set_space_nav(false);
        }
    }
    else
    {
        const std::string& space_id = space_stack_.back();
        auto child_ids = client_->space_children(space_id);
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) != child_ids.end())
            {
                filtered.push_back(r);
            }
        }
        showRooms(filtered);
        if (mainApp_)
        {
            std::string space_name;
            for (const auto& r : rooms_)
            {
                if (r.id == space_id)
                {
                    space_name = r.name;
                    break;
                }
            }
            mainApp_->set_space_nav(true, space_name);
        }
    }
}

void MainWindow::onSpaceBack()
{
    if (!space_stack_.empty())
    {
        space_stack_.pop_back();
    }
    refreshRoomList();
}

// ---------------------------------------------------------------------------

void MainWindow::ensureRowMedia(const tesseract::Event& ev)
{
    // Store decode size hints before delegating to the ShellBase helper.
    if (ev.type == tesseract::EventType::Image)
    {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        if (!img.image_url.empty())
        {
            mediaImageSizes_[img.image_url] = { kMaxImageWidth, kMaxImageHeight };
        }
    }
    else if (ev.type == tesseract::EventType::Sticker)
    {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        if (!s.image_url.empty())
        {
            mediaImageSizes_[s.image_url] = { kMaxStickerSize, kMaxStickerSize };
        }
    }
    else if (ev.type == tesseract::EventType::Video)
    {
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        if (!vid.thumbnail_url.empty())
        {
            mediaImageSizes_[vid.thumbnail_url] = { kMaxImageWidth, kMaxImageHeight };
        }
    }
    for (const auto& r : ev.reactions)
    {
        if (!r.source_json.empty())
        {
            mediaImageSizes_[r.source_json] = { 20, 20 };
        }
    }
    ensure_row_media_(ev);
}


// ---------------------------------------------------------------------------
// Recovery banner + dialog (Step 6)
// ---------------------------------------------------------------------------

void MainWindow::maybeShowRecoveryBanner()
{
    if (recovery_banner_dismissed_)
    {
        return;
    }
    if (!client_->needs_recovery())
    {
        return;
    }
    if (!mainApp_)
    {
        return;
    }
    // Verification takes priority — don't show recovery banner while the
    // verification banner is active. The "Use recovery key" link hands off.
    if (verification_banner_dismissed_ == false && mainApp_->verif_banner()->visible())
    {
        return;
    }
    if (!mainApp_->recovery_banner()->visible())
    {
        mainApp_->recovery_banner()->set_state(
            tesseract::views::RecoveryBanner::State::Form);
        mainApp_->recovery_banner()->set_current_key("");
        if (recoveryKeyField_)
        {
            recoveryKeyField_->set_text("");
            recoveryKeyField_->set_enabled(true);
        }
        mainApp_->show_recovery_banner(true);
        mainAppSurface_->relayout();
    }
}

void MainWindow::onRecoveryVerifyClicked()
{
    std::string key;
    if (recoveryKeyField_)
    {
        key = recoveryKeyField_->text();
    }
    // Trim whitespace.
    auto a = key.find_first_not_of(" \t\r\n");
    auto b = key.find_last_not_of (" \t\r\n");
    if (a == std::string::npos)
    {
        if (mainApp_)
        {
            mainApp_->recovery_banner()->set_state(
                tesseract::views::RecoveryBanner::State::Failed);
            mainApp_->recovery_banner()->set_failure_message(
                "Please enter a recovery key or passphrase.");
            mainAppSurface_->relayout();
        }
        return;
    }
    key = key.substr(a, b - a + 1);

    if (mainApp_)
    {
        mainApp_->recovery_banner()->set_state(
            tesseract::views::RecoveryBanner::State::Verifying);
        mainAppSurface_->relayout();
    }
    if (recoveryKeyField_)
    {
        recoveryKeyField_->set_enabled(false);
    }

    runOnPool_([this, k = key]() {
        auto res = client_->recover(k);
        emit recoverFinished(res.ok, QString::fromStdString(res.message));
    });
}

void MainWindow::onRecoverFinished(bool ok, QString error)
{
    if (!mainApp_)
    {
        return;
    }
    if (ok)
    {
        mainApp_->recovery_banner()->set_state(
            tesseract::views::RecoveryBanner::State::Importing);
        mainAppSurface_->relayout();
        return;
    }
    mainApp_->recovery_banner()->set_state(
        tesseract::views::RecoveryBanner::State::Failed);
    mainApp_->recovery_banner()->set_failure_message(error.toStdString());
    if (recoveryKeyField_)
    {
        recoveryKeyField_->set_enabled(true);
        recoveryKeyField_->set_focused(true);
    }
    mainAppSurface_->relayout();
}

void MainWindow::onDismissRecoveryBanner()
{
    recovery_banner_dismissed_ = true;
    if (mainApp_)
    {
        mainApp_->show_recovery_banner(false);
        mainAppSurface_->relayout();
    }
}


void MainWindow::refreshSyncStatus()
{
    // Compose progress text for the status bar. Priority order:
    //   1. Init / SettingUp        → "Syncing rooms…" (debounced 300 ms)
    //   2. Recovering              → "Reconnecting…"
    //   3. backup Downloading      → "Downloading encryption keys (N)…"
    //   4. else                    → clear (restores prior "Connected" copy)
    //
    // Error / Terminated leave whatever the sync_error path already wrote.
    using RLS = tesseract::RoomListState;
    using BS  = tesseract::BackupState;

    const bool room_busy = (last_room_list_state_ == RLS::Init
                         || last_room_list_state_ == RLS::SettingUp);
    const bool reconnecting = (last_room_list_state_ == RLS::Recovering);
    const bool keys_busy = (last_backup_state_ == BS::Downloading);

    if (room_busy)
    {
        // Debounce: only show "Syncing rooms…" after 300 ms of being
        // non-Running, so already-cached sessions that flash through
        // Init→Running in a single tick don't churn the bar.
        if (!syncStatusDebounce_)
        {
            syncStatusDebounce_ = new QTimer(this);
            syncStatusDebounce_->setSingleShot(true);
            connect(syncStatusDebounce_, &QTimer::timeout, this, [this] {
                using RLS2 = tesseract::RoomListState;
                if (last_room_list_state_ == RLS2::Init
                 || last_room_list_state_ == RLS2::SettingUp)
                {
                    sync_progress_shown_ = true;
                    statusBar()->showMessage(tr("Syncing rooms…"));
                }
            });
        }
        if (!syncStatusDebounce_->isActive() && !sync_progress_shown_)
        {
            syncStatusDebounce_->start(300);
        }
        else if (sync_progress_shown_)
        {
            statusBar()->showMessage(tr("Syncing rooms…"));
        }
        return;
    }

    if (syncStatusDebounce_)
    {
        syncStatusDebounce_->stop();
    }

    if (reconnecting)
    {
        sync_progress_shown_ = true;
        statusBar()->showMessage(tr("Reconnecting…"));
        return;
    }
    if (keys_busy)
    {
        sync_progress_shown_ = true;
        statusBar()->showMessage(
            tr("Downloading encryption keys (%1)…")
                .arg(static_cast<qulonglong>(last_imported_keys_)));
        return;
    }
    if (sync_progress_shown_)
    {
        // We covered up a prior login message; settle to the steady-state copy.
        sync_progress_shown_ = false;
        statusBar()->showMessage(tr("Connected"));
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::populateUserStrip()
{
    if (!mainApp_)
    {
        return;
    }
    mainApp_->user_info()->set_display_name(my_display_name_);
    mainApp_->user_info()->set_user_id(my_user_id_);
    mainApp_->user_info()->set_avatar_url(my_avatar_url_);
    ensure_user_avatar_(my_avatar_url_);
    if (mainAppSurface_)
    {
        mainAppSurface_->update();
    }
}

void MainWindow::onUserStripContextMenu(const QPoint& global_pos)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction* addAct      = menu->addAction(tr("Add Account…"));
    QAction* settingsAct = menu->addAction(tr("Settings…"));
    QString logout_label = tr("Log Out %1").arg(
        my_display_name_.empty()
            ? QString::fromStdString(my_user_id_)
            : QString::fromStdString(my_display_name_));
    QAction* logoutAct = menu->addAction(logout_label);
    menu->addSeparator();
    QAction* quitAct = menu->addAction(tr("Quit"));
    QObject::connect(menu, &QMenu::triggered, this, [this, addAct, settingsAct, logoutAct, quitAct](QAction* a) {
        if (a == addAct)
        {
            beginAddAccount();
        }
        else if (a == settingsAct)
        {
            openSettings();
        }
        else if (a == logoutAct)
        {
            logoutActiveAccount();
        }
        else if (a == quitAct)
        {
            qApp->quit();
        }
    });
    menu->popup(global_pos);
}

void MainWindow::openSettings()
{
    if (!settingsWidget_)
    {
        settingsWidget_ = new SettingsWidget(this);
        settingsWidget_->set_theme(current_theme_);
        contentStack_->addWidget(settingsWidget_);

        connect(settingsWidget_, &SettingsWidget::settingsClosed, this, [this]
        {
            contentStack_->setCurrentWidget(mainAppSurface_);
        });
        connect(settingsWidget_, &SettingsWidget::themeChanged, this,
            [this](tesseract::Settings::ThemePreference pref)
        {
            set_theme_preference_(pref);
        });
        connect(settingsWidget_, &SettingsWidget::notificationsChanged, this,
            [this](bool enabled)
        {
            tesseract::Settings::instance().notifications_enabled = enabled;
            tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
        });
    }

    settingsWidget_->populate(
        my_display_name_,
        my_user_id_,
        my_avatar_url_,
        [this](const std::string& mxc) -> const tk::Image*
        {
            auto it = tk_avatars_.find(mxc);
            return it != tk_avatars_.end() ? it->second.get() : nullptr;
        },
        tesseract::Settings::instance().theme_pref,
        tesseract::Settings::instance().notifications_enabled);

    contentStack_->setCurrentWidget(settingsWidget_);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (shortcode_popup_visible_() && event->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(event);
        QPoint global = me->globalPosition().toPoint();
        // mapFromGlobal() yields a point in the frame's own coordinate space
        // (origin at its top-left), so it must be tested against rect()
        // (0,0,w,h) — not geometry(), which is parent-relative. Using
        // geometry() here made every press (including ones inside the popup)
        // look "outside", dismissing the popup before the click could reach
        // the suggestion row.
        if (!shortcode_popup_frame_->rect().contains(
                shortcode_popup_frame_->mapFromGlobal(global)))
        {
            hide_shortcode_popup_();
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// EventHandlerBase UI-thread hook implementations (Qt6)
// ---------------------------------------------------------------------------

void MainWindow::handle_timeline_reset_ui_(
    std::string room_id,
    std::vector<std::unique_ptr<tesseract::Event>> snapshot)
{
    if (room_id == current_room_id_)
    {
        std::vector<tesseract::views::MessageRowData> rows;
        rows.reserve(snapshot.size());
        for (auto& ev : snapshot)
        {
            if (!ev)
            {
                continue;
            }
            ensureRowMedia(*ev);
            if (!ev->in_reply_to_id.empty())
            {
                ensure_reply_details_(ev->event_id);
            }
            rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
        }
        // A genuine switch, OR a re-population of an emptied view (e.g.
        // logout → login → same room): both warrant the display gate.
        const auto* ml =
            mainApp_ ? mainApp_->room_view()->message_list() : nullptr;
        const bool room_switch =
            view_displayed_room_id_ != room_id ||
            (ml && ml->messages().empty());
        view_displayed_room_id_ = room_id;
        if (mainApp_)
        {
            mainApp_->room_view()->set_messages(std::move(rows), room_switch);
        }
        if (mainAppSurface_)
        {
            mainAppSurface_->relayout();
        }
        if (mainApp_ && mainApp_->room_view()->message_list())
        {
            const auto& pstate = pagination_[room_id];
            if (room_switch && pstate.is_focused)
            {
                mainApp_->room_view()->message_list()->begin_focused_gate(
                    pstate.focus_event_id);
            }
            mainApp_->room_view()->message_list()->set_historical_mode(pstate.is_focused);
            if (pstate.is_focused)
            {
                mainApp_->room_view()->message_list()->scroll_to_event_id(pstate.focus_event_id);
            }

            // Restore saved scroll offset when returning to a tab that had
            // been scrolled up from the bottom.
            if (room_switch && !pstate.is_focused
                && active_tab_idx_ < tabs_.size()
                && tabs_[active_tab_idx_].room_id == room_id
                && tabs_[active_tab_idx_].scroll_offset > 0.f)
            {
                mainApp_->room_view()->message_list()->scroll_to_offset(
                    tabs_[active_tab_idx_].scroll_offset);
            }
        }
    }

    dispatch_to_secondary_windows_(room_id, [&](tesseract::RoomWindowBase* w) {
        std::vector<tesseract::views::MessageRowData> rows;
        rows.reserve(snapshot.size());
        for (auto& ev : snapshot)
        {
            if (!ev)
            {
                continue;
            }
            ensureRowMedia(*ev);
            if (!ev->in_reply_to_id.empty())
            {
                ensure_reply_details_(ev->event_id);
            }
            rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
        }
        w->on_timeline_reset(std::move(rows));
    });
}

void MainWindow::handle_message_inserted_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }

    if (room_id == current_room_id_)
    {
        ensureRowMedia(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        if (mainApp_)
        {
            mainApp_->room_view()->insert_message(index,
                tesseract::views::make_row_data(*ev, my_user_id_));
        }
        if (mainAppSurface_)
        {
            mainAppSurface_->relayout();
        }
    }

    dispatch_to_secondary_windows_(room_id, [&](tesseract::RoomWindowBase* w) {
        ensureRowMedia(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        w->on_message_inserted(index,
            tesseract::views::make_row_data(*ev, my_user_id_));
    });
}

void MainWindow::handle_message_updated_ui_(
    std::string room_id, std::size_t index,
    std::unique_ptr<tesseract::Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }

    if (room_id == current_room_id_)
    {
        ensureRowMedia(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        if (mainApp_)
        {
            mainApp_->room_view()->update_message(index,
                tesseract::views::make_row_data(*ev, my_user_id_));
        }
        if (mainAppSurface_)
        {
            mainAppSurface_->relayout();
        }
    }

    dispatch_to_secondary_windows_(room_id, [&](tesseract::RoomWindowBase* w) {
        ensureRowMedia(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        w->on_message_updated(index,
            tesseract::views::make_row_data(*ev, my_user_id_));
    });
}

void MainWindow::handle_message_removed_ui_(
    std::string room_id, std::size_t index)
{
    if (room_id == current_room_id_)
    {
        if (mainApp_)
        {
            mainApp_->room_view()->remove_message(index);
        }
        if (mainAppSurface_)
        {
            mainAppSurface_->relayout();
        }
    }

    dispatch_to_secondary_windows_(room_id, [&](tesseract::RoomWindowBase* w) {
        w->on_message_removed(index);
    });
}

void MainWindow::handle_sync_error_ui_(
    std::string context, std::string user_id,
    std::string description, bool soft_logout)
{
    tesseract::AccountSession* affected = nullptr;
    for (auto& a : accounts_)
    {
        if (a->user_id == user_id)
        {
            affected = a.get();
            break;
        }
    }

    if (context == "sync_reconnect")
    {
        statusBar()->showMessage(tr("Sync error: reconnecting\xe2\x80\xa6"));
        if (affected && affected->client)
        {
            affected->client->stop_sync();
            affected->sync_started = false;
            QTimer::singleShot(5000, this, [this, uid = affected->user_id]() {
                for (auto& a : accounts_)
                {
                    if (a->user_id == uid && !a->sync_started && a->client)
                    {
                        a->sync_started = true;
                        a->client->start_sync(a->bridge.get());
                    }
                }
            });
        }
    }
    else if (context == "sync_auth_error")
    {
        if (soft_logout && affected)
        {
            if (auto saved = tesseract::SessionStore::load_account(affected->user_id))
            {
                statusBar()->showMessage(tr("Reconnecting session\xe2\x80\xa6"));
                if (affected->client->restore_session(*saved))
                {
                    affected->display_name = affected->client->get_display_name();
                    affected->avatar_url   = affected->client->get_avatar_url();
                    if (affected == accounts_[std::max(0, active_account_index_)].get())
                    {
                        switchActiveAccount(active_account_index_);
                    }
                    affected->client->start_sync(affected->bridge.get());
                    statusBar()->showMessage(tr("Reconnected"));
                    maybeShowRecoveryBanner();
                    return;
                }
            }
        }
        if (affected)
        {
            tesseract::SessionStore::clear_account(affected->user_id);
            affected->client->stop_sync();
        }
        statusBar()->showMessage(tr("Session expired; please log in again."));
        doLogin();
    }
    else
    {
        statusBar()->showMessage(
            tr("Sync error: %1").arg(QString::fromStdString(description)), 8000);
    }
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    // Only the active account's backup state drives the recovery banner and
    // status bar — but we can't filter by user_id here since BackupProgress
    // doesn't carry one. We rely on EventHandlerBase's post_to_ui_ being
    // per-instance: the active client_ pointer check gates the banner update.
    if (!client_)
    {
        return;
    }

    maybeShowRecoveryBanner();

    if (mainApp_ && mainApp_->recovery_banner()->visible()
        && mainApp_->recovery_banner()->state() ==
            tesseract::views::RecoveryBanner::State::Importing
        && progress.state == tesseract::BackupState::Downloading
        && progress.imported_keys > 0)
    {
        mainApp_->recovery_banner()->set_import_progress(progress.imported_keys);
        mainAppSurface_->relayout();
    }
    if (progress.state == tesseract::BackupState::Enabled
        && !client_->needs_recovery())
    {
        if (mainApp_)
        {
            mainApp_->show_recovery_banner(false);
            mainAppSurface_->relayout();
        }
    }

    last_backup_state_  = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refreshSyncStatus();
}

void MainWindow::handle_image_packs_updated_ui_()
{
    if (stickerPicker_)
    {
        stickerPicker_->refreshPacks();
    }
    if (emojiPicker_)
    {
        emojiPicker_->refreshEmoticonPacks();
    }

    cached_emoticons_.clear();
    if (client_)
    {
        for (auto& pack : client_->list_image_packs())
        {
            for (auto& img : client_->list_pack_images(
                    pack.id, tesseract::PackUsageFilter::Emoticon))
            {
                cached_emoticons_.push_back(std::move(img));
            }
        }
    }
}

void MainWindow::handle_account_prefs_updated_ui_(
    std::string user_id, std::string json)
{
    // Only the active account's prefs set the pending restore room.
    if (active_account_index_ < 0
            || accounts_[active_account_index_]->user_id != user_id)
    {
        return;
    }
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.last_room.empty()
            && pending_restore_room_.empty()
            && current_room_id_.empty())
    {
        pending_restore_room_ = prefs.last_room;
    }
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id,
    std::string room_name, std::string sender,
    std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes,
    std::vector<uint8_t> image_bytes)
{
    if (!tesseract::Settings::instance().notifications_enabled)
        return;
    if (!notification_image_allowed_())
        image_bytes.clear();

    bool win_visible = isVisible() && !isMinimized();
    bool win_focused = isActiveWindow();

    for (auto& sess : accounts_)
    {
        if (sess->user_id != user_id)
        {
            continue;
        }
        // Suppress only when the user is actively focused on this exact room.
        if (win_focused
                && active_account_index_ >= 0
                && accounts_[active_account_index_]->user_id == user_id
                && current_room_id_ == room_id)
        {
            return;
        }
        // Flash the taskbar when the window is visible but not the active app.
        if (win_visible && !win_focused)
            QApplication::alert(this, 0);
        // Send a system notification regardless of window state unless already
        // watching the exact room above.
        if (sess->notifier)
        {
            tesseract::Notification n;
            n.room_id      = room_id;
            n.room_name    = room_name;
            n.sender       = sender;
            n.body         = body;
            n.is_mention   = is_mention;
            n.avatar_bytes = std::move(avatar_bytes);
            n.image_bytes  = std::move(image_bytes);
            sess->notifier->notify(n);
        }
        return;
    }
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::on_tab_state_changed_ui_()
{
    if (!mainApp_) return;

    auto* tb = mainApp_->tab_bar();
    const bool show_bar = tabs_.size() > 1;
    mainApp_->set_tab_bar_visible(show_bar);

    if (tb)
    {
        // Rebuild in tabs_ order so visual order is always stable.
        tb->clear();
        for (const auto& t : tabs_)
        {
            const tk::Image* avatar = nullptr;
            std::string      name;
            for (const auto& r : rooms_)
            {
                if (r.id != t.room_id) continue;
                name = r.name;
                if (!r.avatar_url.empty())
                {
                    auto it = tk_avatars_.find(r.avatar_url);
                    if (it != tk_avatars_.end()) avatar = it->second.get();
                }
                break;
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
            tb->set_active(tabs_[active_tab_idx_].room_id);
    }

    // Navigate to the active tab's room.
    if (active_tab_idx_ < tabs_.size())
    {
        const auto& active = tabs_[active_tab_idx_];
        onRoomSelected(active.room_id);

        // Restore compose draft (onRoomSelected clears it via set_text("")).
        if (!active.compose_draft.empty())
        {
            if (roomTextArea_) roomTextArea_->set_text(active.compose_draft);
            if (mainApp_)
                mainApp_->room_view()->set_current_text(active.compose_draft);
        }
    }

    if (mainAppSurface_) mainAppSurface_->relayout();
}

float MainWindow::get_message_scroll_fraction_()
{
    if (!mainApp_ || !mainApp_->room_view()->message_list()) return 0.f;
    return mainApp_->room_view()->message_list()->scroll_fraction();
}

void MainWindow::set_message_scroll_fraction_(float t)
{
    if (!mainApp_ || !mainApp_->room_view()->message_list()) return;
    mainApp_->room_view()->message_list()->scroll_to_offset(t);
}

std::string MainWindow::get_compose_draft_()
{
    if (!mainApp_ || !mainApp_->room_view()->compose_bar()) return {};
    return mainApp_->room_view()->compose_bar()->current_text();
}

void MainWindow::set_compose_draft_(const std::string& draft)
{
    if (roomTextArea_) roomTextArea_->set_text(draft);
    if (mainApp_)
        mainApp_->room_view()->set_current_text(draft);
}

// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::on_room_list_state_ui_()
{
    refreshSyncStatus();
}

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (mainApp_)
    {
        mainApp_->room_view()->set_typing_text(text);
    }
}

// ---------------------------------------------------------------------------
// Multi-account orchestration
// ---------------------------------------------------------------------------

void MainWindow::switchActiveAccount(int new_idx)
{
    if (new_idx < 0 || new_idx >= static_cast<int>(accounts_.size()))
    {
        return;
    }
    if (new_idx == active_account_index_ && client_)
    {
        return;
    }

    // Unsubscribe the previous account's open room so its timeline stops
    // streaming updates to the message list when we swap surfaces.
    if (client_ && !current_room_id_.empty()
            && room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }
    current_room_id_.clear();
    // Per-account, room-id-keyed state must not bleed into the next account
    // (a room id present in both accounts would otherwise inherit stale
    // pagination / space-drill / reply-fetch state).
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();
    clearMessages();

    active_account_index_ = new_idx;
    auto& s = *accounts_[new_idx];
    client_        = s.client.get();
    event_handler_ = s.bridge.get();   // keep ShellBase's non-owning alias in sync

    my_user_id_      = s.user_id;
    my_display_name_ = s.display_name;
    my_avatar_url_   = s.avatar_url;
    pending_restore_room_ = s.last_room;

    populateUserStrip();
    if (emojiPicker_)
    {
        emojiPicker_->setClient(client_);
    }
    if (stickerPicker_)
    {
        stickerPicker_->setClient(client_);
    }
    if (joinRoomDialog_)
    {
        joinRoomDialog_->setClient(client_);
    }

    // Use this account's last-known rooms snapshot if we have one
    // cached; otherwise wait for the next on_rooms_updated callback to
    // populate the list.
    auto it = per_account_rooms_.find(s.user_id);
    if (it != per_account_rooms_.end())
    {
        rooms_ = it->second;
        refreshRoomList();
        // Rooms are already in cache — try to restore the last open room
        // immediately. on_rooms_updated_ handles the async case (no cache).
        if (!pending_restore_room_.empty())
        {
            for (const auto& r : rooms_)
            {
                if (r.id == pending_restore_room_ && !r.is_space)
                {
                    std::string target = std::move(pending_restore_room_);
                    pending_restore_room_.clear();
                    onRoomSelected(target);
                    break;
                }
            }
        }
    }
    else
    {
        rooms_.clear();
        refreshRoomList();
    }

    // Persist the active selection.
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = s.user_id;
    for (auto& a : accounts_)
    {
        idx.user_ids.push_back(a->user_id);
    }
    tesseract::SessionStore::save_index(idx);

    rebuildAccountPicker();
    maybeShowRecoveryBanner();
}

void MainWindow::beginAddAccount()
{
    add_account_return_idx_ = active_account_index_;
    pending_login_is_add_account_ = true;

    // Create a fresh client for the OAuth round-trip. The user_id won't
    // be known until await_oauth completes, so we point the SDK at a
    // per-attempt "pending-<ts>" directory; onLoginSucceeded renames
    // it to accounts/<sanitized-uid>/ once the round-trip completes.
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    loginView_->set_client(pending_login_client_.get());
    loginView_->set_on_begin_oauth([this] {
        if (!pending_login_temp_dir_.empty())
        {
            return;
        }
        pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
            "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
        std::error_code ec;
        std::filesystem::create_directories(pending_login_temp_dir_, ec);
        pending_login_client_->set_data_dir(
            (pending_login_temp_dir_ / "matrix-store").string());
    });
    loginView_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
    loginView_->reset();
    contentStack_->setCurrentWidget(loginView_);
    statusBar()->showMessage(tr("Add Account"));
}

void MainWindow::logoutActiveAccount()
{
    if (active_account_index_ < 0)
    {
        return;
    }
    auto& a = *accounts_[active_account_index_];
    const std::string uid = a.user_id;

    if (a.up_connector)
    {
        a.up_connector->logout();
    }
    a.client->logout();
    a.client->stop_sync();
    tesseract::SessionStore::clear_account(uid);
    per_account_rooms_.erase(uid);

    // Remove this account from the live vector.
    accounts_.erase(accounts_.begin() + active_account_index_);
    active_account_index_ = -1;
    client_        = nullptr;
    event_handler_ = nullptr;

    // Reset visible state regardless of where we go next.
    current_room_id_.clear();
    space_stack_.clear();
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    rooms_.clear();
    refreshRoomList();
    clearMessages();
    if (mainApp_)
    {
        mainApp_->show_recovery_banner(false);
        mainAppSurface_->relayout();
    }
    recovery_banner_dismissed_ = false;

    if (accounts_.empty())
    {
        // No accounts left → back to initial login.
        tesseract::SessionStore::save_index({});
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        loginView_->set_client(pending_login_client_.get());
        loginView_->set_on_begin_oauth([this] {
            if (!pending_login_temp_dir_.empty())
            {
                return;
            }
            pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                "pending-" + std::to_string(QDateTime::currentMSecsSinceEpoch()));
            std::error_code ec;
            std::filesystem::create_directories(pending_login_temp_dir_, ec);
            pending_login_client_->set_data_dir(
                (pending_login_temp_dir_ / "matrix-store").string());
        });
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Signed out"), 3000);
        rebuildAccountPicker();
        return;
    }

    // Otherwise switch to the next account (or the first one if we
    // removed the last in the vector). Update the on-disk active pointer.
    switchActiveAccount(0);
    statusBar()->showMessage(tr("Signed out of %1")
                                 .arg(QString::fromStdString(uid)),
                             3000);
}

void MainWindow::rebuildAccountPicker()
{
    if (!accountPicker_)
    {
        return;
    }
    std::vector<tesseract::views::AccountEntry> entries;
    entries.reserve(accounts_.size());
    for (std::size_t i = 0; i < accounts_.size(); ++i)
    {
        const auto& a = *accounts_[i];
        entries.push_back({
            a.user_id, a.display_name, a.avatar_url,
            static_cast<int>(i) == active_account_index_,
        });
    }
    accountPicker_->set_entries(std::move(entries));
}

static QString accountPopoverQss(const tk::Theme& t)
{
    const auto& p = t.palette;
    auto hex = [](const tk::Color& c) {
        return QString::asprintf("#%02X%02X%02X", c.r, c.g, c.b);
    };
    return QStringLiteral(
               "QFrame { background-color: %1; border:1px solid %2; }")
        .arg(hex(p.chrome_bg), hex(p.popup_border));
}

void MainWindow::openAccountPicker(const QPoint& global_anchor)
{
    if (!accountPickerPopover_)
    {
        accountPickerPopover_ = new QFrame(this);
        accountPickerPopover_->setWindowFlags(Qt::Popup
                                              | Qt::FramelessWindowHint);
        accountPickerPopover_->setFrameShape(QFrame::Box);
        accountPickerPopover_->setStyleSheet(accountPopoverQss(current_theme_));
        auto* lay = new QVBoxLayout(accountPickerPopover_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        accountPickerSurface_ = new tk::qt6::Surface(tk::Theme::light(),
                                                     accountPickerPopover_);
        auto picker_owner = std::make_unique<tesseract::views::AccountPicker>();
        accountPicker_ = picker_owner.get();
        accountPicker_->set_image_provider(
            [this](const std::string& mxc) -> const tk::Image* {
                auto it = tk_avatars_.find(mxc);
                return it != tk_avatars_.end() ? it->second.get() : nullptr;
            });
        accountPicker_->on_select = [this](const std::string& uid) {
            onAccountSelected(uid);
        };
        accountPickerSurface_->set_root(std::move(picker_owner));
        lay->addWidget(accountPickerSurface_);
    }
    rebuildAccountPicker();

    constexpr int kPickerWidth = 260;
    constexpr int kRowHeight   = 56;
    const int rows = static_cast<int>(accounts_.size());
    const int height = std::max(kRowHeight, rows * kRowHeight) + 2;
    accountPickerPopover_->resize(kPickerWidth, height);

    // Anchor above the strip (popover hangs down from the user-strip top
    // edge on most desktops; if it would clip the screen bottom Qt::Popup
    // reflows automatically).
    QPoint anchor = global_anchor;
    anchor.setY(anchor.y() - height);
    accountPickerPopover_->move(anchor);
    accountPickerPopover_->show();
}

void MainWindow::onAccountSelected(const std::string& user_id)
{
    if (accountPickerPopover_)
    {
        accountPickerPopover_->hide();
    }
    for (std::size_t i = 0; i < accounts_.size(); ++i)
    {
        if (accounts_[i]->user_id == user_id)
        {
            switchActiveAccount(static_cast<int>(i));
            contentStack_->setCurrentWidget(mainAppSurface_);
            return;
        }
    }
}

void MainWindow::doLogout()
{
    // Legacy shim that the tray / external code may still call. Routes to
    // the active-account logout path.
    logoutActiveAccount();
}

// ── Cross-signing / SAS verification hooks ────────────────────────────────────

void MainWindow::handle_verification_state_ui_(bool is_verified)
{
    if (!mainApp_)
    {
        return;
    }
    if (is_verified)
    {
        mainApp_->show_verif_banner(false);
        mainAppSurface_->relayout();
        return;
    }
    if (verification_banner_dismissed_)
    {
        return;
    }
    if (!mainApp_->verif_banner()->visible())
    {
        active_verification_flow_id_.clear();
        mainApp_->verif_banner()->set_state(tesseract::views::VerificationBanner::State::Prompt);
        // Verification takes priority — hide recovery banner if it appeared
        // before the verification state callback arrived (race on first sync).
        // But if recovery is actively in progress (Verifying/Importing), let
        // it finish rather than interrupting with the verification banner.
        if (mainApp_->recovery_banner()->visible())
        {
            auto rs = mainApp_->recovery_banner()->state();
            if (rs == tesseract::views::RecoveryBanner::State::Form
             || rs == tesseract::views::RecoveryBanner::State::Failed)
            {
                mainApp_->show_recovery_banner(false);
            }
            else
            {
                return;
            }
        }
        mainApp_->show_verif_banner(true);
        mainAppSurface_->relayout();
    }
}

void MainWindow::handle_verification_request_ui_(
    std::string flow_id, std::string /*user_id*/,
    std::string /*device_id*/, bool incoming)
{
    if (!mainApp_)
    {
        return;
    }
    active_verification_flow_id_ = flow_id;
    if (incoming)
    {
        mainApp_->verif_banner()->set_state(
            tesseract::views::VerificationBanner::State::IncomingRequest);
    }
    else
    {
        mainApp_->verif_banner()->set_state(
            tesseract::views::VerificationBanner::State::Waiting);
        if (client_)
        {
            client_->start_sas(flow_id);
        }
    }
    mainApp_->show_verif_banner(true);
    mainAppSurface_->relayout();
}

void MainWindow::handle_sas_ready_ui_(
    std::string /*flow_id*/, std::vector<tesseract::VerificationEmoji> emojis)
{
    if (!mainApp_)
    {
        return;
    }
    mainApp_->verif_banner()->set_emojis(emojis);
    mainApp_->show_verif_banner(true);
    mainAppSurface_->relayout();
}

void MainWindow::handle_verification_done_ui_(std::string /*flow_id*/)
{
    if (!mainApp_)
    {
        return;
    }
    mainApp_->verif_banner()->set_state(tesseract::views::VerificationBanner::State::Done);
    mainAppSurface_->relayout();
    QTimer::singleShot(1500, this, [this] {
        if (mainApp_ && mainApp_->verif_banner()->on_done)
        {
            mainApp_->verif_banner()->on_done();
        }
    });
}

void MainWindow::handle_verification_cancelled_ui_(
    std::string /*flow_id*/, std::string reason)
{
    if (!mainApp_)
    {
        return;
    }
    mainApp_->verif_banner()->set_state(tesseract::views::VerificationBanner::State::Cancelled);
    mainApp_->verif_banner()->set_cancel_reason(std::move(reason));
    mainApp_->show_verif_banner(true);
    mainAppSurface_->relayout();
}

tk::ThemeMode MainWindow::os_color_scheme_() const
{
    const auto qt_scheme = QGuiApplication::styleHints()->colorScheme();
    if (qt_scheme != Qt::ColorScheme::Unknown)
        return qt_scheme == Qt::ColorScheme::Dark ? tk::ThemeMode::Dark
                                                  : tk::ThemeMode::Light;
    // Qt could not determine the OS color scheme (common on GNOME without
    // QGnomePlatform / Qt < 6.6). Fall back to the XDG portal value.
    return portal_color_scheme_ == 1 ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
}

void MainWindow::read_portal_color_scheme_()
{
    QDBusInterface iface(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QDBusConnection::sessionBus());
    if (!iface.isValid()) return;

    QDBusReply<QDBusVariant> reply = iface.call(
        QStringLiteral("Read"),
        QStringLiteral("org.freedesktop.appearance"),
        QStringLiteral("color-scheme"));
    if (reply.isValid())
        portal_color_scheme_ = reply.value().variant().toInt();
}

void MainWindow::on_portal_setting_changed_(const QString& ns,
                                             const QString& key,
                                             const QDBusVariant& value)
{
    if (ns != QLatin1String("org.freedesktop.appearance") ||
        key != QLatin1String("color-scheme"))
        return;
    portal_color_scheme_ = value.variant().toInt();
    if (tesseract::Settings::instance().theme_pref ==
        tesseract::Settings::ThemePreference::System)
    {
        apply_current_theme_();
    }
}

// ---------------------------------------------------------------------------
// Shortcode popup
// ---------------------------------------------------------------------------

void MainWindow::show_shortcode_popup_(
    const std::vector<tesseract::views::ShortcodeSuggestion>& suggestions,
    tk::Rect cursor_local)
{
    if (!shortcode_popup_frame_)
    {
        // Regular child widget — no separate window, so focus never leaves
        // the compose text area regardless of window manager behaviour.
        shortcode_popup_frame_ = new QWidget(this);
        shortcode_popup_frame_->setFocusPolicy(Qt::NoFocus);

        shortcode_popup_surface_ = std::make_unique<tk::qt6::Surface>(
            mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
            shortcode_popup_frame_,
            /*transparent=*/false);
        shortcode_popup_surface_->setFocusPolicy(Qt::NoFocus);

        auto widget = std::make_unique<tesseract::views::ShortcodePopup>();
        shortcode_popup_widget_ = widget.get();
        shortcode_popup_surface_->set_root(std::move(widget));

        shortcode_popup_widget_->on_accepted = [this](tesseract::views::ShortcodeSuggestion s) {
            std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
            roomTextArea_->replace_range(
                shortcode_active_match_.start,
                shortcode_active_match_.end,
                std::move(r));
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->on_dismissed = [this] { hide_shortcode_popup_(); };
        shortcode_popup_widget_->set_image_provider(
            [this](const std::string& url) -> const tk::Image* {
                auto it = tk_images_.find(url);
                return it == tk_images_.end() ? nullptr : it->second.get();
            });

        auto* lay = new QVBoxLayout(shortcode_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(shortcode_popup_surface_.get());
    }

    shortcode_popup_widget_->set_suggestions(suggestions);

    int rows = std::min((int)suggestions.size(),
                        (int)tesseract::views::ShortcodePopup::kMaxRows);
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);
    int w = int(tesseract::views::ShortcodePopup::kWidth);

    // Map cursor rect from surface-local into main-window coordinates.
    // The popup is a child of the main window, so we use mapTo(this, ...).
    QPoint parent_cursor = mainAppSurface_->mapTo(this,
        QPoint(int(cursor_local.x), int(cursor_local.y)));

    QRect work  = rect();  // main window bounds — popup is clipped to this
    int x       = parent_cursor.x();
    int y_above = parent_cursor.y() - h - 4;
    int y_below = parent_cursor.y() + int(cursor_local.h) + 4;
    int y       = (y_above >= work.top()) ? y_above : y_below;
    x = std::clamp(x, work.left(), work.right()  - w);
    y = std::clamp(y, work.top(),  work.bottom() - h);

    const bool was_hidden = !shortcode_popup_frame_->isVisible();
    shortcode_popup_frame_->setGeometry(x, y, w, h);
    shortcode_popup_surface_->resize(w, h);
    shortcode_popup_frame_->show();
    shortcode_popup_frame_->raise();
    shortcode_popup_surface_->relayout();
    if (was_hidden)
    {
        qApp->installEventFilter(this);
    }
}

void MainWindow::hide_shortcode_popup_()
{
    qApp->removeEventFilter(this);
    if (shortcode_popup_frame_)
    {
        shortcode_popup_frame_->hide();
    }
    if (roomTextArea_)
    {
        roomTextArea_->set_on_popup_nav(nullptr);
    }
}

// ---------------------------------------------------------------------------

void MainWindow::apply_theme_ui_(const tk::Theme& t)
{
    if (mainAppSurface_)
    {
        mainAppSurface_->set_theme(t);
    }
    if (accountPickerSurface_)
    {
        accountPickerSurface_->set_theme(t);
    }
    if (shortcode_popup_surface_)
    {
        shortcode_popup_surface_->set_theme(t);
    }
    if (settingsWidget_)
    {
        settingsWidget_->set_theme(t);
    }
    if (loginView_)
    {
        loginView_->set_theme(t);
    }
    if (emojiPicker_)
    {
        emojiPicker_->set_theme(t);
    }
    if (stickerPicker_)
    {
        stickerPicker_->set_theme(t);
    }
    if (joinRoomDialog_)
    {
        joinRoomDialog_->set_theme(t);
    }
    if (accountPickerPopover_)
    {
        accountPickerPopover_->setStyleSheet(accountPopoverQss(t));
    }
    apply_theme_to_secondary_windows_(t);
    if (roomTextArea_)
    {
        roomTextArea_->set_text_color(t.palette.text_primary);
    }
    {
        const auto& p = t.palette;
        QPalette pal = statusBar()->palette();
        pal.setColor(QPalette::Window,
                     QColor(p.chrome_bg.r, p.chrome_bg.g, p.chrome_bg.b, p.chrome_bg.a));
        pal.setColor(QPalette::WindowText,
                     QColor(p.text_secondary.r, p.text_secondary.g, p.text_secondary.b, p.text_secondary.a));
        statusBar()->setPalette(pal);
        statusBar()->setAutoFillBackground(true);
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
    }
}

} // namespace qt6
