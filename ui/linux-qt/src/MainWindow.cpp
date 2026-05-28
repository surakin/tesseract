#include "MainWindow.h"
#include "LoginView.h"
#include "views/BrandView.h"
#include "SettingsWidget.h"
#include "EmojiPicker.h"
#include "StickerPicker.h"
#include "JoinRoomDialog.h"
#include "LinuxScreenLockQt.h"
#include "app/SlashCommands.h"

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

#include <tesseract/mentions.h>
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
#include <QLocalServer>
#include <QLocalSocket>
#include <QWindow>
#ifdef HAVE_XDG_ACTIVATION
#include <wayland-client.h>
#include "xdg-activation-v1-protocol.h"
#include <QGuiApplication>
#include <qguiapplication_platform.h>
// QPlatformNativeInterface is needed to get wl_surface* via nativeResourceForWindow;
// Qt 6 exposes no public equivalent.
#include <QtGui/qpa/qplatformnativeinterface.h>
#endif
#include <QFile>
#include <QFileDialog>

#include <algorithm>
#include <fstream>
#include <unistd.h>
#include <unordered_set>

Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)
Q_DECLARE_METATYPE(tesseract::BackupProgress)

namespace qt6
{

// EventBridge is now a thin QObject wrapper around EventHandlerBase.
// All IEventHandler method bodies live in EventHandlerBase (shared/app/).
// No EventBridge:: method definitions needed here.

// ---------------------------------------------------------------------------
// MainWindow constructor
// ---------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    qRegisterMetaType<std::vector<tesseract::RoomInfo>>();
    qRegisterMetaType<tesseract::BackupProgress>();

    set_screen_lock_(std::make_unique<LinuxScreenLockQt>());

    setWindowTitle("Tesseract");
    resize(1100, 768);

    contentStack_ = new QStackedWidget(this);
    setCentralWidget(contentStack_);

    brandingSurface_ = new tk::qt6::Surface(tk::Theme::light(), contentStack_);
    brandingSurface_->set_root(std::make_unique<tesseract::views::BrandView>());
    contentStack_->addWidget(brandingSurface_);

    loginView_ = new LoginView(contentStack_);
    contentStack_->addWidget(loginView_);
    // Route the homeserver-discovery debounce through the shell's worker
    // drain so a blocked discover_homeserver call can't outlive ~LoginView
    // and corrupt the heap (mirrors the SettingsController wiring below).
    loginView_->set_run_async(
        [this](std::function<void()> fn) { run_async_(std::move(fn)); });
    connect(loginView_, &LoginView::loginSucceeded, this,
            &MainWindow::onLoginSucceeded);
    connect(loginView_, &LoginView::loginCancelled, this,
            &MainWindow::onLoginCancelled);

    // Single surface hosting the MainAppWidget tree (sidebar + chat + overlays).
    mainAppSurface_ = new tk::qt6::Surface(tk::Theme::light(), contentStack_);
    contentStack_->addWidget(mainAppSurface_);
    // Let the animation timer repaint only the rects where animated images are
    // drawn (see onMessageAnimTick_) instead of the whole surface.
    mainAppSurface_->set_anim_cache(&anim_cache_);

    // Feed pointer / wheel events into the PresenceTracker so we stay "Online"
    // while the user is engaging with the app. Focus + timer ticks are wired
    // separately below (changeEvent + QTimer).
    mainAppSurface_->host().set_on_user_activity(
        [this] { notify_user_activity_(); });

    // 30 s periodic tick — granular enough for a 5 min idle threshold without
    // burning CPU. The tracker is lazily created on first activity once sync
    // is up, so ticks before then are cheap no-ops.
    presence_tick_timer_ = new QTimer(this);
    connect(presence_tick_timer_, &QTimer::timeout, this,
            [this] { notify_presence_tick_(); });
    presence_tick_timer_->start(30000);

    tesseract::Settings::instance().load_from_disk(tesseract::config_dir());

    {
        auto main_app_owner =
            std::make_unique<tesseract::views::MainAppWidget>();
        mainApp_ = main_app_owner.get();
        // Populate the shared ShellBase view pointers (before sync starts) so
        // hoisted handle_*_ui_ implementations can drive the view.
        main_app_ = mainApp_;
        room_view_ = mainApp_->room_view();

        mainApp_->on_space_back = [this]
        {
            onSpaceBack();
        };

        // ---- Provider wiring (avatar/image/sticker/preview/user-info) ----
        wire_main_app_widget_(mainApp_);

        // ---- Room list ----
        mainApp_->room_list_view()->on_room_selected =
            [this](const std::string& room_id)
        {
            if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
            {
                tab_open_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        {
            auto* scrollDebounce = new QTimer(this);
            scrollDebounce->setSingleShot(true);
            mainApp_->room_list_view()->on_scroll = [this, scrollDebounce]
            {
                scrollDebounce->start(300);
            };
            connect(scrollDebounce, &QTimer::timeout, this,
                    [this]
                    {
                        if (!mainApp_ || !client_)
                        {
                            return;
                        }
                        auto ids =
                            mainApp_->room_list_view()->visible_room_ids();
                        client_->stop_background_backfill();
                        client_->start_background_backfill(ids);
                    });
        }
        mainApp_->room_list_view()->on_search_clear = [this]
        {
            cancel_debounce_(DebounceSlot::RoomSearch);
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
        mainApp_->room_list_view()->on_join_room_requested = [this]
        {
            if (joinRoomDialog_)
            {
                joinRoomDialog_->openDialog();
            }
        };

        // ---- Tab bar ----
        mainApp_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id)
        {
            tab_select_room(room_id);
        };
        mainApp_->tab_bar()->on_tab_closed = [this](const std::string& room_id)
        {
            tab_close(room_id);
        };

        // ---- User info strip ----
        mainApp_->user_info()->on_primary = [this](tk::Point world)
        {
            if (accounts_.size() < 2)
            {
                return;
            }
            openAccountPicker(mainAppSurface_->mapToGlobal(
                QPoint(static_cast<int>(world.x), static_cast<int>(world.y))));
        };

        // ---- Recovery banner ----
        mainApp_->recovery_banner()->on_verify = [this](const std::string& key)
        {
            (void)key;
            onRecoveryVerifyClicked();
        };
        mainApp_->recovery_banner()->on_dismiss = [this]
        {
            onDismissRecoveryBanner();
        };

        // ---- Verification banner ----
        mainApp_->verif_banner()->on_verify = [this]
        {
            if (client_)
            {
                client_->request_self_verification();
            }
        };
        mainApp_->verif_banner()->on_accept = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->accept_verification(active_verification_flow_id_);
                client_->start_sas(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_match = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                mainApp_->verif_banner()->set_state(
                    tesseract::views::VerificationBanner::State::Confirming);
                mainAppSurface_->relayout();
                client_->confirm_sas(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_mismatch = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_cancel = [this]
        {
            if (client_ && !active_verification_flow_id_.empty())
            {
                client_->cancel_verification(active_verification_flow_id_);
            }
        };
        mainApp_->verif_banner()->on_dismiss = [this]
        {
            verification_banner_dismissed_ = true;
            mainApp_->show_verif_banner(false);
            mainAppSurface_->relayout();
        };
        mainApp_->verif_banner()->on_done = [this]
        {
            mainApp_->show_verif_banner(false);
            mainAppSurface_->relayout();
        };
        mainApp_->verif_banner()->on_use_recovery_key = [this]
        {
            mainApp_->show_verif_banner(false);
            recovery_key_chosen_ = true;
            if (!recovery_banner_dismissed_ && mainApp_)
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
            }
            mainAppSurface_->relayout();
        };

        // ---- Image + video viewers ----
        wire_main_app_viewers_(
            mainApp_, mainAppSurface_->host(),
            [this]
            {
                if (mainAppSurface_)
                {
                    mainAppSurface_->relayout();
                }
            },
            [this]
            {
                if (roomTextArea_)
                {
                    roomTextArea_->set_focused(true);
                }
            },
            [this]
            {
                if (roomTextArea_)
                {
                    roomTextArea_->set_focused(true);
                }
            });

        // Qt6-only residual: install a richer image_viewer provider that
        // also consults viewerFullresCache_ before falling back to the
        // shared anim_cache_/tk_images_ chain set by wire_main_app_viewers_.
        mainApp_->image_viewer()->set_image_provider(
            [this](const std::string& url) -> const tk::Image*
            {
                if (auto it = viewerFullresCache_.find(url);
                    it != viewerFullresCache_.end())
                {
                    return it->second.get();
                }
                if (const auto* f = anim_cache_.current_frame(url))
                {
                    start_anim_tick_();
                    return f;
                }
                if (auto it = tk_images_.find(url); it != tk_images_.end())
                {
                    return it->second.get();
                }
                // Avatars live in a separate cache — let the viewer find
                // them so clicking a profile avatar shows the cached image.
                auto av = tk_avatars_.find(url);
                return av == tk_avatars_.end() ? nullptr : av->second.get();
            });

        // ---- Room view ----
        mainApp_->room_view()->set_shortcode_provider(
            [this](const std::string& mxc) -> std::string
            {
                return shortcode_for_mxc_(mxc);
            });
        // Avatar inside received mention pills: resolve user id → member avatar
        // mxc → cached image (kicking a fetch on miss; the row repaints when
        // the bytes arrive).
        mainApp_->room_view()->message_list()->set_mention_avatar_provider(
            [this](const std::string& user_id) -> const tk::Image*
            {
                for (const auto& m : cached_room_members_)
                {
                    if (m.user_id != user_id)
                        continue;
                    if (m.avatar_url.empty())
                        return nullptr;
                    ensure_user_avatar_(m.avatar_url);
                    auto it = tk_avatars_.find(m.avatar_url);
                    return it == tk_avatars_.end() ? nullptr
                                                   : it->second.get();
                }
                return nullptr;
            });
        if (auto player = mainAppSurface_->host().make_audio_player())
        {
            mainApp_->room_view()->set_audio_player(std::move(player));
        }
        capture_ = mainAppSurface_->host().make_audio_capture();
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            wire_voice_capture_(
                mainApp_->room_view(),
                [sfp]() { if (sfp) sfp->update(); },
                [this]() { return current_room_id_; },
                [this]() { mainApp_->room_view()->set_current_text({}); });
        }
        mainApp_->room_view()->set_voice_bytes_provider(
            [this](const std::string& source_json) -> std::vector<std::uint8_t>
            {
                return client_->fetch_source_bytes(source_json);
            });
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            mainApp_->room_view()->set_repaint_requester(
                [sfp]()
                {
                    if (sfp)
                    {
                        sfp->update();
                    }
                });
            mainApp_->room_view()->set_post_delayed(
                [sfp](int ms, std::function<void()> fn)
                {
                    if (sfp)
                    {
                        sfp->host().post_delayed(ms, std::move(fn));
                    }
                });
        }
        mainApp_->room_view()->set_video_player_factory(
            [this]()
            {
                return mainAppSurface_->host().make_video_player();
            });
        mainApp_->room_view()->set_video_fetch_provider(
            [this](const std::string& src,
                   std::function<void(std::vector<std::uint8_t>)> on_ready)
            {
                runOnPool_(
                    [this, src, on_ready = std::move(on_ready)]() mutable
                    {
                        auto bytes = client_->fetch_source_bytes(src);
                        QMetaObject::invokeMethod(
                            this,
                            [on_ready = std::move(on_ready),
                             bytes = std::move(bytes)]() mutable
                            {
                                on_ready(std::move(bytes));
                            },
                            Qt::QueuedConnection);
                    });
            });

        mainApp_->room_view()->on_layout_changed = [this]
        {
            if (mainAppSurface_)
            {
                mainAppSurface_->relayout();
            }
        };
        mainApp_->room_view()->on_link_clicked = [](const std::string& url)
        {
            tesseract::Client::open_in_browser(url);
        };
        mainApp_->room_view()->on_set_clipboard = [this](std::string_view t)
        {
            if (mainAppSurface_)
                mainAppSurface_->host().set_clipboard_text(t);
        };
        mainApp_->room_view()->message_list()->on_show_copy_menu = [this]()
        {
            auto* ml = mainApp_->room_view()->message_list();
            auto* menu = new QMenu(mainAppSurface_);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            QAction* copyAct = menu->addAction(tr("Copy"));
            QObject::connect(copyAct, &QAction::triggered, [ml]()
            {
                ml->copy_selection();
            });
            menu->popup(QCursor::pos());
        };
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            mainApp_->room_view()->on_link_hovered =
                [sfp](const std::string& url)
            {
                if (sfp)
                {
                    sfp->setCursor(url.empty() ? Qt::ArrowCursor
                                               : Qt::PointingHandCursor);
                }
            };
        }
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            mainApp_->room_view()->on_show_tooltip =
                [sfp](std::string text, tk::Rect anchor)
            {
                if (!sfp)
                {
                    return;
                }
                QPoint local(static_cast<int>(anchor.x),
                             static_cast<int>(anchor.y + anchor.h));
                QToolTip::showText(sfp->mapToGlobal(local),
                                   QString::fromStdString(text), sfp);
            };
            mainApp_->room_view()->on_hide_tooltip = []
            {
                QToolTip::hideText();
            };
        }
        mainApp_->room_view()->on_receipt_needed =
            [this](const std::string& eid)
        {
            maybe_send_read_receipt_(current_room_id_, eid);
        };
        mainApp_->room_view()->message_list()->on_tile_needed =
            [this](int z, int x, int y)
        {
            ensure_tile_async(z, x, y);
        };
        mainApp_->room_view()->on_near_top = [this]
        {
            if (current_room_id_.empty())
            {
                return;
            }
            requestMoreHistory(current_room_id_);
        };
        mainApp_->room_view()->on_near_bottom = [this]
        {
            if (!current_room_id_.empty())
            {
                request_forward_history_(current_room_id_);
            }
        };
        mainApp_->room_view()->on_return_to_live = [this]
        {
            if (!current_room_id_.empty())
            {
                return_to_live_(current_room_id_);
            }
        };
        mainApp_->room_view()->on_scroll_to_original =
            [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            std::string room = current_room_id_;
            begin_focused_subscription_(room, event_id);
            runOnPool_(
                [this, room, event_id]
                {
                    client_->subscribe_room_at(room, event_id);
                });
        };
        mainApp_->room_view()->on_jump_to_date_requested = [this]
        {
            openJumpToDateDialog();
        };
        mainApp_->room_view()->on_threads_button_clicked = [this]
        {
            on_threads_button_clicked();
        };
        mainApp_->room_view()->on_pin_requested =
            [this](const std::string& ev) { on_pin_requested(ev); };
        mainApp_->room_view()->on_unpin_requested =
            [this](const std::string& ev) { on_unpin_requested(ev); };
        mainApp_->room_view()->on_thread_open_requested =
            [this](const std::string& root)
        {
            on_thread_open_requested(root);
        };
        mainApp_->room_view()->on_thread_close_requested = [this]
        {
            on_thread_close_requested();
        };
        mainApp_->room_view()->on_thread_send =
            [this](const std::string& body, const std::string& formatted)
        {
            on_thread_send_requested(body, formatted);
            if (roomTextArea_)
                roomTextArea_->set_text("");
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_thread_send_reply =
            [this](const std::string& reply_id,
                   const std::string& body,
                   const std::string& formatted)
        {
            on_thread_send_reply_requested(reply_id, body, formatted);
            if (roomTextArea_)
                roomTextArea_->set_text("");
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_delete_requested =
            [this](const std::string& event_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            client_->redact_event(current_room_id_, event_id);
        };
        mainApp_->room_view()->on_reaction_toggled =
            [this](const std::string& event_id, const std::string& key,
                   const std::string& source_mxc)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            if (!source_mxc.empty())
            {
                // For MSC4027 chips matrix-sdk aggregates by the mxc:// key
                // (so `key` IS the mxc URI). Look up the shortcode locally
                // so the outgoing event carries `:shortcode:` rather than
                // re-broadcasting the URI as its own shortcode.
                std::string sc = shortcode_for_mxc_(source_mxc);
                std::string shortcode =
                    sc.empty() ? std::string() : ":" + sc + ":";
                client_->send_reaction_custom(current_room_id_, event_id,
                                              source_mxc, shortcode);
                return;
            }
            client_->send_reaction(current_room_id_, event_id, key);
        };
        mainApp_->room_view()->on_add_reaction_requested =
            [this](const std::string& event_id, tk::Rect anchor)
        {
            if (!emojiPicker_ || current_room_id_.empty())
            {
                return;
            }
            pendingReactionEventId_ = event_id;
            emojiPicker_->popupAtRect(mainAppSurface_, anchor);
        };
        mainApp_->room_view()->on_send = [this](const std::string& body)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            // Build the message from the composer's mention draft so inline
            // pills become matrix.to links + m.mentions. Falls back to the
            // passed-in body when the native area has no draft.
            std::vector<tesseract::MentionSeg> draft =
                roomTextArea_ ? roomTextArea_->mention_draft()
                              : std::vector<tesseract::MentionSeg>{};
            bool has_mention = false;
            for (const auto& seg : draft)
            {
                if (seg.kind == tesseract::MentionSeg::Kind::Mention)
                {
                    has_mention = true;
                }
            }
            tesseract::MarkdownResult msg =
                draft.empty() ? tesseract::MarkdownResult{body, ""}
                              : tesseract::build_mention_message(draft);
            std::string trimmed =
                QString::fromStdString(msg.body).trimmed().toStdString();
            if (trimmed.empty() && !has_mention)
            {
                return;
            }
            auto res = tesseract::dispatch_compose_send(
                *client_, current_room_id_, msg.body, msg.formatted_body);
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
                statusBar()->showMessage(QString::fromStdString(res.message),
                                         4000);
            }
        };
        mainApp_->room_view()->on_send_reply =
            [this](const std::string& reply_event_id, const std::string& body)
        {
            if (body.empty() || current_room_id_.empty())
            {
                return;
            }
            auto res =
                client_->send_reply(current_room_id_, reply_event_id, body);
            if (!res)
            {
                statusBar()->showMessage(
                    tr("Send reply failed: %1")
                        .arg(QString::fromStdString(res.message)),
                    4000);
            }
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_send_edit =
            [this](const std::string& event_id, const std::string& new_body)
        {
            if (new_body.empty() || current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_edit(current_room_id_, event_id, new_body);
            if (!res)
            {
                statusBar()->showMessage(
                    tr("Edit failed: %1")
                        .arg(QString::fromStdString(res.message)),
                    4000);
            }
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_send_image =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int src_w,
                   int src_h, bool is_animated, std::string reply_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            tesseract::Result res;
            if (is_animated)
            {
                // Animated GIF/WebP: send the original bytes verbatim via
                // the MSC4230 raw path. Re-encoding would flatten the
                // animation to a single frame.
                res = client_->send_image(
                    current_room_id_, bytes, mime, filename, caption,
                    static_cast<std::uint32_t>(src_w < 0 ? 0 : src_w),
                    static_cast<std::uint32_t>(src_h < 0 ? 0 : src_h),
                    /*is_animated=*/true, reply_id);
            }
            else
            {
                const bool compress =
                    tesseract::Settings::instance().image_quality ==
                    tesseract::Settings::ImageQuality::Compressed;
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
                res = client_->send_image(current_room_id_, enc.bytes, enc.mime,
                                          out_name, caption, enc.width,
                                          enc.height, /*is_animated=*/false,
                                          reply_id);
            }
            if (!res)
            {
                statusBar()->showMessage(
                    tr("Send image failed: %1")
                        .arg(QString::fromStdString(res.message)),
                    4000);
                return;
            }
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_send_video =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption, int w, int h,
                   std::vector<std::uint8_t> thumb_bytes, int thumb_w,
                   int thumb_h, std::uint64_t duration_ms, std::string reply_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_video(
                current_room_id_, bytes, mime, filename, caption,
                static_cast<std::uint32_t>(w < 0 ? 0 : w),
                static_cast<std::uint32_t>(h < 0 ? 0 : h), thumb_bytes,
                static_cast<std::uint32_t>(thumb_w < 0 ? 0 : thumb_w),
                static_cast<std::uint32_t>(thumb_h < 0 ? 0 : thumb_h),
                duration_ms, reply_id);
            if (!res)
            {
                statusBar()->showMessage(
                    tr("Send video failed: %1")
                        .arg(QString::fromStdString(res.message)),
                    4000);
                return;
            }
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_send_audio =
            [this](std::vector<std::uint8_t> bytes, std::string mime,
                   std::string filename, std::string caption,
                   std::uint64_t duration_ms, std::string reply_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_audio(current_room_id_, bytes, mime,
                                           filename, caption, duration_ms,
                                           reply_id);
            if (!res)
            {
                statusBar()->showMessage(
                    tr("Send audio failed: %1")
                        .arg(QString::fromStdString(res.message)),
                    4000);
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
                   std::string reply_id)
        {
            if (current_room_id_.empty())
            {
                return;
            }
            auto res = client_->send_file(current_room_id_, bytes, mime,
                                          filename, caption, reply_id);
            if (!res)
            {
                statusBar()->showMessage(
                    tr("Send file failed: %1")
                        .arg(QString::fromStdString(res.message)),
                    4000);
                return;
            }
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_edit_prefill = [this](const std::string& body)
        {
            if (roomTextArea_)
            {
                roomTextArea_->set_text(body);
                roomTextArea_->set_focused(true);
            }
        };
        mainApp_->room_view()->on_edit_cancelled = [this]
        {
            if (roomTextArea_)
            {
                roomTextArea_->set_text("");
            }
            mainApp_->room_view()->clear_compose_text();
        };
        mainApp_->room_view()->on_reply_focus = [this]
        {
            if (roomTextArea_)
            {
                roomTextArea_->set_focused(true);
            }
        };
        mainApp_->room_view()->on_focus_input = [this]
        {
            if (roomTextArea_)
                roomTextArea_->set_focused(true);
        };
        mainApp_->room_view()->on_emoji = [this](tk::Rect btn)
        {
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
        mainApp_->room_view()->on_sticker = [this](tk::Rect btn)
        {
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
            [this](const tesseract::views::MessageListView::ImageHit& hit)
        {
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            mainApp_->image_viewer()->open(src_tok, thumb_tok,
                                           hit.body, hit.natural_w,
                                           hit.natural_h);
            mainApp_->show_image_viewer(true);
            mainAppSurface_->relayout();
            mainAppSurface_->setFocus();
            ensureViewerFullres_(src_tok);
        };

        // Avatar click → open the lightbox with the *original* avatar mxc,
        // not the 80×80 thumbnail stored in tk_avatars_. The shared
        // wire_main_app_widget_ already wires a basic on_avatar_clicked
        // that uses the thumbnail; override here so the Qt6 viewer gets
        // the full-resolution decode via ensureViewerFullres_.
        mainApp_->room_view()->on_avatar_clicked =
            [this](std::string url, std::string name)
        {
            if (url.empty())
                return;
            mainApp_->image_viewer()->open(url, url, name, 0, 0);
            mainApp_->show_image_viewer(true);
            mainAppSurface_->relayout();
            mainAppSurface_->setFocus();
            ensureViewerFullres_(url);
        };
        mainApp_->image_viewer()->on_save =
            [this](std::string source_url, std::string filename_hint)
        {
            std::string suggested = filename_hint.empty() ? "image" : filename_hint;
            QString path = QFileDialog::getSaveFileName(
                this, tr("Save image"),
                QString::fromStdString(suggested),
                tr("Images (*.jpg *.jpeg *.png *.gif *.webp);;All files (*.*)"));
            if (path.isEmpty())
                return;
            std::string dest = path.toStdString();
            runOnPool_(
                [this, source_url = std::move(source_url), dest]()
                {
                    auto bytes = client_->fetch_source_bytes(source_url);
                    QMetaObject::invokeMethod(
                        this,
                        [dest, bytes = std::move(bytes)]() mutable
                        {
                            if (bytes.empty())
                                return;
                            std::ofstream f(dest, std::ios::binary);
                            f.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()));
                        },
                        Qt::QueuedConnection);
                });
        };
        mainApp_->room_view()->on_video_clicked =
            [this](const tesseract::views::MessageListView::VideoHit& hit)
        {
            const std::string src_tok   = hit.source    ? hit.source->fetch_token()    : std::string{};
            const std::string thumb_tok = hit.thumbnail ? hit.thumbnail->fetch_token() : std::string{};
            mainApp_->video_viewer()->open(
                src_tok, thumb_tok, hit.mime_type,
                hit.duration_ms, hit.natural_w, hit.natural_h, hit.autoplay,
                hit.loop, hit.no_audio, hit.hide_controls);
            mainApp_->show_video_viewer(true);
            mainAppSurface_->relayout();
            mainAppSurface_->setFocus();
            std::string src = src_tok;
            runOnPool_(
                [this, src = std::move(src)]() mutable
                {
                    auto bytes = client_->fetch_source_bytes(src);
                    QMetaObject::invokeMethod(
                        this,
                        [this, bytes = std::move(bytes)]() mutable
                        {
                            if (mainApp_)
                            {
                                mainApp_->video_viewer()->load_bytes(
                                    bytes.data(), bytes.size());
                            }
                        },
                        Qt::QueuedConnection);
                });
        };
        mainApp_->room_view()->on_file_clicked =
            [this](const tesseract::views::MessageListView::FileHit& hit)
        {
            std::string suggested = hit.file_name.empty() ? "download" : hit.file_name;
            QString path = QFileDialog::getSaveFileName(
                this, tr("Save file"),
                QString::fromStdString(suggested),
                tr("All files (*.*)"));
            if (path.isEmpty())
                return;
            std::string url  = hit.source ? hit.source->fetch_token() : std::string{};
            std::string dest = path.toStdString();
            runOnPool_(
                [this, url, dest]()
                {
                    auto bytes = client_->fetch_source_bytes(url);
                    QMetaObject::invokeMethod(
                        this,
                        [dest, bytes = std::move(bytes)]() mutable
                        {
                            if (bytes.empty())
                                return;
                            std::ofstream f(dest, std::ios::binary);
                            f.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()));
                        },
                        Qt::QueuedConnection);
                });
        };
        mainApp_->room_view()->on_fetch_room_members =
            [this](const std::string& room_id)
        {
            if (!client_)
                return;
            auto* c = client_;
            runOnPool_(
                [this, c, room_id]()
                {
                    auto members = c->get_room_members(room_id);
                    QMetaObject::invokeMethod(
                        this,
                        [this, room_id, members = std::move(members)]() mutable
                        {
                            if (mainApp_)
                            {
                                for (const auto& m : members)
                                    ensure_user_avatar_(m.avatar_url);
                                // Cache for the mention popup + pill avatars.
                                cached_room_members_ = members;
                                cached_members_room_ = room_id;
                                mainApp_->room_view()->set_room_members(
                                    std::move(members));
                            }
                        },
                        Qt::QueuedConnection);
                });
        };
        mainApp_->room_view()->on_save_topic =
            [this](const std::string& room_id, const std::string& topic)
        {
            if (!client_)
                return;
            auto* c = client_;
            runOnPool_(
                [this, c, room_id, topic]()
                {
                    auto res = c->set_room_topic(room_id, topic);
                    if (!res.ok)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, msg = res.message]()
                            {
                                statusBar()->showMessage(
                                    tr("Failed to set topic: %1")
                                        .arg(QString::fromStdString(msg)),
                                    4000);
                            },
                            Qt::QueuedConnection);
                    }
                });
        };
        mainApp_->room_view()->on_leave_room =
            [this](const std::string& room_id)
        {
            if (!client_)
                return;
            auto* c = client_;
            runOnPool_(
                [this, c, room_id]()
                {
                    auto res = c->leave_room(room_id);
                    QMetaObject::invokeMethod(
                        this,
                        [this, room_id, ok = res.ok]()
                        {
                            if (!mainApp_ || !ok)
                                return;
                            if (current_room_id_ == room_id)
                            {
                                current_room_id_.clear();
                                mainApp_->room_view()->clear_room();
                                mainApp_->room_list_view()->set_selected_room(
                                    "");
                                if (mainAppSurface_)
                                    mainAppSurface_->relayout();
                            }
                        },
                        Qt::QueuedConnection);
                });
        };
        setup_dm_callbacks();
        mainApp_->room_view()->on_ignore_user =
            [this](const std::string& user_id)
        {
            if (!client_)
                return;
            auto* c = client_;
            runOnPool_(
                [c, user_id]()
                {
                    c->ignore_user(user_id);
                });
        };
        mainApp_->video_viewer()->on_save =
            [this](std::string source_json, std::string mime_type)
        {
            std::string ext = ".mp4";
            auto slash = mime_type.find('/');
            if (slash != std::string::npos)
                ext = "." + mime_type.substr(slash + 1);
            QString path = QFileDialog::getSaveFileName(
                this, tr("Save video"),
                QString::fromStdString("video" + ext),
                tr("Videos (*.mp4 *.webm *.mkv);;All files (*.*)"));
            if (path.isEmpty())
                return;
            std::string dest = path.toStdString();
            runOnPool_(
                [this, source_json = std::move(source_json), dest]()
                {
                    auto bytes = client_->fetch_source_bytes(source_json);
                    QMetaObject::invokeMethod(
                        this,
                        [dest, bytes = std::move(bytes)]() mutable
                        {
                            if (bytes.empty())
                                return;
                            std::ofstream f(dest, std::ios::binary);
                            f.write(reinterpret_cast<const char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size()));
                        },
                        Qt::QueuedConnection);
                });
        };

        mainAppSurface_->set_root(std::move(main_app_owner));
    }

    // ---- Native overlays ----
    roomTextArea_ = mainAppSurface_->host().make_text_area();
    roomTextArea_->set_font_role(tk::FontRole::Body);
    roomTextArea_->set_text_color(
        mainAppSurface_->theme().palette.text_primary);
    roomTextArea_->set_mention_colors(
        mainAppSurface_->theme().palette.accent,
        mainAppSurface_->theme().palette.text_on_accent);
    roomTextArea_->set_placeholder(tr("Message\xe2\x80\xa6").toStdString());
    roomTextArea_->set_on_changed(
        [this](const std::string& s)
        {
            if (mainApp_)
            {
                mainApp_->room_view()->set_current_text(s);
            }

            int cursor = roomTextArea_->cursor_byte_pos();

            // Slash-command popup: activate when the composer starts with '/'
            // and contains only name chars after it (no args yet).
            {
                auto m = slash_engine_.find_prefix(s, cursor);
                if (m.has_value())
                {
                    auto items = slash_engine_.lookup(m->prefix);
                    if (items.empty())
                    {
                        hide_slash_popup_();
                    }
                    else
                    {
                        // Only hide other popovers if they are actually visible —
                        // these hide functions reset set_on_popup_nav(nullptr), and
                        // calling them unconditionally on every text-change tick
                        // kills the slash popup's nav handler.
                        if (shortcode_popup_visible_()) hide_shortcode_popup_();
                        if (mention_popup_visible_())   hide_mention_popup_();
                        show_slash_popup_(items, roomTextArea_->cursor_rect());
                        // Reinstall the nav handler unconditionally on every tick:
                        // hide_*_popup_ calls above (when they did fire) wipe it,
                        // and even when they didn't, installing unconditionally
                        // matches what handle_mention_on_changed_ does and avoids
                        // the "first keystroke kills nav" bug.
                        roomTextArea_->set_on_popup_nav(
                            [this](tk::NativeTextArea::NavKey nk) -> bool
                            {
                                if (!slash_popup_visible_())
                                {
                                    return false;
                                }
                                int cur =
                                    slash_popup_widget_->selected_index();
                                int n = slash_popup_widget_->visible_rows();
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
                                    int sel =
                                        slash_popup_widget_->selected_index();
                                    if (sel >= 0 &&
                                        sel < slash_popup_widget_->visible_rows() &&
                                        slash_popup_widget_->on_accepted)
                                    {
                                        slash_popup_widget_->on_accepted(
                                            slash_popup_widget_->suggestion_at(
                                                sel));
                                    }
                                    else
                                    {
                                        hide_slash_popup_();
                                    }
                                    return true;
                                }
                                case tk::NativeTextArea::NavKey::ShiftTab:
                                    return false;
                                case tk::NativeTextArea::NavKey::Escape:
                                    hide_slash_popup_();
                                    return true;
                                }
                                slash_popup_widget_->set_selected_index(next);
                                slash_popup_surface_->update();
                                return true;
                            });
                    }
                    return;
                }
                if (slash_popup_visible_())
                {
                    hide_slash_popup_();
                }
            }

            // Auto-expand: ":smile:" + space → replace with glyph
            auto complete = shortcode_engine_.find_complete(s, cursor);
            if (complete)
            {
                auto hits = shortcode_engine_.lookup(complete->prefix,
                                                     cached_emoticons_, 1);
                std::string r = (!hits.empty() && !hits.front().glyph.empty())
                                    ? hits.front().glyph
                                    : ":" + complete->prefix + ":";
                roomTextArea_->replace_range(complete->start, complete->end, r);
                hide_shortcode_popup_();
                hide_mention_popup_();
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
                    hide_mention_popup_();
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
                        roomTextArea_->set_on_popup_nav(
                            [this](tk::NativeTextArea::NavKey nk) -> bool
                            {
                                if (!shortcode_popup_visible_())
                                {
                                    return false;
                                }
                                int cur =
                                    shortcode_popup_widget_->selected_index();
                                int n = shortcode_popup_widget_->visible_rows();
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
                                    int sel = shortcode_popup_widget_
                                                  ->selected_index();
                                    if (sel >= 0 &&
                                        sel <
                                            (int)shortcode_current_suggestions_
                                                .size())
                                    {
                                        auto& s =
                                            shortcode_current_suggestions_[sel];
                                        std::string r =
                                            s.glyph.empty()
                                                ? ":" + s.shortcode + ":"
                                                : s.glyph;
                                        roomTextArea_->replace_range(
                                            shortcode_active_match_.start,
                                            shortcode_active_match_.end, r);
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
                                shortcode_popup_widget_->set_selected_index(
                                    next);
                                shortcode_popup_surface_->update();
                                return true;
                            });
                    }
                    return;
                }
            }
            // @mention popup
            if (handle_mention_on_changed_(s, cursor))
            {
                return;
            }
            hide_shortcode_popup_();
            hide_mention_popup_();
        });
    roomTextArea_->set_on_submit(
        [this]
        {
            if (slash_popup_visible_())
            {
                int sel = slash_popup_widget_->selected_index();
                if (sel >= 0 && sel < slash_popup_widget_->visible_rows() &&
                    slash_popup_widget_->on_accepted)
                {
                    slash_popup_widget_->on_accepted(
                        slash_popup_widget_->suggestion_at(sel));
                    return;
                }
                hide_slash_popup_();
            }
            if (mention_popup_visible_())
            {
                int sel = mention_popup_widget_->selected_index();
                if (sel >= 0 &&
                    sel < (int)mention_current_candidates_.size())
                {
                    accept_mention_(mention_current_candidates_[sel]);
                    return;
                }
                hide_mention_popup_();
            }
            if (shortcode_popup_visible_())
            {
                int sel = shortcode_popup_widget_->selected_index();
                if (sel >= 0 &&
                    sel < (int)shortcode_current_suggestions_.size())
                {
                    auto& s = shortcode_current_suggestions_[sel];
                    std::string r =
                        s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
                    roomTextArea_->replace_range(shortcode_active_match_.start,
                                                 shortcode_active_match_.end,
                                                 r);
                    hide_shortcode_popup_();
                    return;
                }
                // Nothing selected — dismiss popup and send the message
                hide_shortcode_popup_();
            }
            onSendClicked();
        });
    roomTextArea_->set_on_edit_last(
        [this]
        {
            return mainApp_ && mainApp_->room_view() &&
                   mainApp_->room_view()->edit_last_own();
        });
    roomTextArea_->set_on_height_changed(
        [this](float h)
        {
            if (!mainApp_ || !mainAppSurface_)
            {
                return;
            }
            mainApp_->room_view()->set_text_area_natural_height(h);
            mainAppSurface_->relayout();
        });
    roomTextArea_->set_on_image_paste(
        [this](std::vector<std::uint8_t> bytes, std::string mime)
        {
            if (mainApp_)
            {
                mainApp_->room_view()->compose_bar()->set_pending_image(
                    std::move(bytes), std::move(mime));
            }
        });

    topicTextArea_ = mainAppSurface_->host().make_text_area();
    topicTextArea_->set_font_role(tk::FontRole::Body);
    topicTextArea_->set_text_color(
        mainAppSurface_->theme().palette.text_primary);
    topicTextArea_->set_on_changed(
        [this](const std::string& t)
        {
            if (mainApp_)
                mainApp_->room_view()->set_topic_edit_text(t);
        });
    topicTextArea_->set_visible(false);

    roomSearchField_ = mainAppSurface_->host().make_text_field();
    roomSearchField_->set_text_color(
        mainAppSurface_->theme().palette.text_primary);
    roomSearchField_->set_placeholder(
        tr("Search rooms\xe2\x80\xa6").toStdString());
    roomSearchField_->set_on_changed(
        [this](const std::string& s)
        {
            roomSearchPendingText_ = s;
            debounce_(DebounceSlot::RoomSearch,
                      tesseract::views::RoomListView::kSearchDebounceMs,
                      [this]
                      {
                          if (mainApp_)
                          {
                              mainApp_->room_list_view()->set_search_text(
                                  roomSearchPendingText_);
                          }
                          refreshRoomList();
                      });
        });

    recoveryKeyField_ = mainAppSurface_->host().make_text_field();
    recoveryKeyField_->set_password(true);

    mainAppSurface_->set_on_layout(
        [this]
        {
            if (mainApp_ && roomTextArea_)
            {
                const tk::Rect ta = mainApp_->compose_text_area_rect();
                roomTextArea_->set_visible(!ta.empty());
                if (!ta.empty())
                    roomTextArea_->set_rect(ta);
            }
            if (mainApp_ && roomSearchField_)
            {
                roomSearchField_->set_visible(
                    mainApp_->room_search_field_visible());
                roomSearchField_->set_rect(mainApp_->room_search_field_rect());
            }
            if (mainApp_ && recoveryKeyField_)
            {
                recoveryKeyField_->set_visible(
                    mainApp_->recovery_key_field_visible());
                recoveryKeyField_->set_rect(
                    mainApp_->recovery_key_field_rect());
            }
            if (mainApp_ && topicTextArea_)
            {
                const tk::Rect tr =
                    mainApp_->room_view()->topic_edit_rect();
                const bool edit_visible =
                    mainApp_->room_view()->topic_edit_visible();
                const bool now_visible = edit_visible && !tr.empty();
                // Detect transition from hidden to visible to prefill text.
                const bool was_visible = topicTextAreaVisible_;
                topicTextAreaVisible_ = now_visible;
                topicTextArea_->set_visible(now_visible);
                if (now_visible)
                {
                    topicTextArea_->set_rect(tr);
                    if (!was_visible)
                        topicTextArea_->set_text(
                            mainApp_->room_view()->topic_edit_initial_text());
                }
            }
        });

    mainAppSurface_->set_on_file_drop(
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename)
        {
            if (!mainApp_)
                return;
            if (bytes.empty())
                return;
            auto* cb = mainApp_->room_view()->compose_bar();

            if (mime == "image/gif" || mime == "image/webp")
            {
                // Show first frame immediately; detect animation on a bg thread.
                cb->set_pending_image(bytes, mime, filename, false);
                auto gen = cb->pending_gen();
                run_async_([this, gen, bytes = std::move(bytes),
                             mime = std::move(mime)]() mutable
                {
                    QByteArray ba(
                        reinterpret_cast<const char*>(bytes.data()),
                        static_cast<qsizetype>(bytes.size()));
                    QBuffer buf;
                    buf.setData(ba);
                    buf.open(QIODevice::ReadOnly);
                    QImageReader reader(&buf);
                    tesseract::views::MediaInfo info;
                    info.is_animated = reader.imageCount() > 1;
                    info.pending_gen = gen;
                    QMetaObject::invokeMethod(
                        this,
                        [this, info]() mutable
                        {
                            if (mainApp_)
                                mainApp_->room_view()->compose_bar()
                                    ->update_pending_attachment(info);
                        },
                        Qt::QueuedConnection);
                });
            }
            else if (mime.starts_with("image/"))
            {
                cb->set_pending_image(std::move(bytes), std::move(mime),
                                      std::move(filename), false);
            }
            else if (mime.starts_with("video/"))
            {
                cb->set_pending_video(bytes, mime, filename);
                auto gen = cb->pending_gen();
                extract_drop_video_(gen, std::move(bytes));
            }
            else if (mime.starts_with("audio/"))
            {
                cb->set_pending_audio(bytes, mime, filename);
                auto gen = cb->pending_gen();
                extract_drop_audio_(gen, std::move(bytes));
            }
            else
            {
                cb->set_pending_file(std::move(bytes), std::move(mime),
                                     std::move(filename));
            }
        });

    // Right-click: user-strip actions (lower-left corner) or sticker save (chat).
    mainAppSurface_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        mainAppSurface_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& pos)
        {
            using namespace tesseract::visual;
            const bool in_user_strip =
                pos.x() < kSidebarWidth &&
                pos.y() >= mainAppSurface_->height() - kUserStripHeight;
            if (in_user_strip)
            {
                onUserStripContextMenu(mainAppSurface_->mapToGlobal(pos));
                return;
            }
            if (!mainApp_)
            {
                return;
            }
            auto hit =
                mainApp_->room_view()->message_list()->sticker_hit_at(tk::Point{
                    static_cast<float>(pos.x()), static_cast<float>(pos.y())});
            if (!hit)
            {
                return;
            }
            const auto mxc_url = hit->source ? hit->source->mxc_url() : std::string{};
            const auto body = hit->body;
            const auto info_json = hit->info_json;
            const bool already_saved =
                client_->user_pack_has_sticker(mxc_url, info_json);
            auto* menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            QAction* add =
                menu->addAction(already_saved ? tr("Already in Saved Stickers")
                                              : tr("Add to Saved Stickers"));
            add->setEnabled(!already_saved);
            if (!already_saved)
            {
                connect(add, &QAction::triggered, this,
                        [this, mxc_url, body, info_json]
                        {
                            auto res = client_->save_sticker_to_user_pack(
                                body, body, mxc_url, info_json);
                            if (!res.ok)
                            {
                                statusBar()->showMessage(
                                    QString::fromStdString(res.message), 6000);
                            }
                        });
            }
            menu->popup(mainAppSurface_->mapToGlobal(pos));
        });

    // Emoji picker: build the floating panel, wire selection → cursor
    // insert + account-data bump. Recents live in the SDK now (synced via
    // `io.element.recent_emoji`), so no local-disk load is needed.
    emojiPicker_ = new EmojiPicker(this);
    emojiPicker_->setClient(client_);
    emojiPicker_->setImageProvider(make_picker_image_provider_(false));
    emojiPicker_->onSelected = [this](const QString& glyph)
    {
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
                client_->send_reaction(current_room_id_, ev,
                                       glyph.toStdString());
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

    emojiPicker_->onEmoticonSelected =
        [this](const tesseract::ImagePackImage& img)
    {
        // Reaction mode (mirrors the Unicode onSelected branch above): the
        // "+" chip set pendingReactionEventId_ before opening the picker.
        // Send an MSC4027 custom-image reaction and skip the compose insert.
        if (!pendingReactionEventId_.empty())
        {
            std::string ev = std::move(pendingReactionEventId_);
            pendingReactionEventId_.clear();
            if (!current_room_id_.empty() && !img.url.empty())
            {
                client_->send_reaction_custom(current_room_id_, ev, img.url,
                                              ":" + img.shortcode + ":");
            }
            emojiPicker_->hide();
            return;
        }
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
    stickerPicker_->setImageProvider(make_picker_image_provider_(true));
    stickerPicker_->onSelected = [this](const tesseract::ImagePackImage& img)
    {
        if (current_room_id_.empty())
        {
            return;
        }
        std::string body = img.body.empty() ? img.shortcode : img.body;
        if (thread_panel_ == ThreadPanel::Open && !current_thread_root_.empty())
            client_->send_thread_sticker(current_room_id_,
                                         current_thread_root_, body,
                                         img.url, img.info_json);
        else
            client_->send_sticker(current_room_id_, body, img.url, img.info_json);
        stickerPicker_->hide();
    };

    joinRoomDialog_ = new JoinRoomDialog(this);
    joinRoomDialog_->setClient(client_);
    joinRoomDialog_->setAvatarProvider(make_avatar_image_provider_());
    joinRoomDialog_->onJoined = [this](const std::string& room_id)
    {
        navigate_to_room(room_id);
    };

    statusBar()->showMessage(tr("Not logged in"));

    read_portal_color_scheme_();
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("SettingChanged"), this,
        SLOT(on_portal_setting_changed_(QString, QString, QDBusVariant)));
    apply_current_theme_();

    // Re-apply when the OS switches light/dark (only relevant in System mode).
    // colorSchemeChanged was added in Qt 6.5; on older Qt the XDG portal
    // signal already handles this via on_portal_setting_changed_.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this,
            [this]
            {
                if (tesseract::Settings::instance().theme_pref ==
                    tesseract::Settings::ThemePreference::System)
                {
                    apply_current_theme_();
                }
            });
#endif

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
    connect(tk_anim_timer_, &QTimer::timeout, this,
            &MainWindow::onMessageAnimTick_);

    QMetaObject::invokeMethod(this, &MainWindow::doLogin, Qt::QueuedConnection);
    setupLocalServer_();
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
    //     ensure_media_image_), SettingsController, LoginView's
    //     homeserver-discovery debounce, and LinuxUpConnectorQt's distributor
    //     scan. Guarded by shutting_down_ (ShellBase); tracked by
    //     workers_in_flight_ / workers_cv_. (Historically the LoginView and
    //     UpConnector workers ran on their own untracked threads; this is
    //     the defect class that produced both the ~StickerPicker abort
    //     ace5261 fixed in SettingsController and the ~MessageListView
    //     unlink_chunk abort that motivated extending the drain here.)
    //
    // JoinRoomDialog's QThreadPool::globalInstance() tasks are deliberately
    // not drained here — the dialog is modal and tears its workers down on
    // close before this destructor runs.
    //
    // Both flags must be flipped first so no new work is enqueued after the
    // clear/drain calls.  stop_sync() is called early to cancel the Rust sync
    // loop; it does NOT cancel individual fetch_*() HTTP requests (those block
    // on tokio block_on(), which only unblocks when rt.drop() kills the I/O
    // driver inside ~Client()).  ~ClientFfi calls stop_sync() a second time as
    // a no-op safety net (handler.take() returns None on repeated calls).
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
    // Drain ShellBase detached std::threads (avatar / media fetches).
    // First pass: give threads already past the shutting_down_ check a chance
    // to finish on their own (stop_sync() above cancels the sync loop, but
    // does NOT cancel individual fetch_*() HTTP requests).
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                             [this]
                             {
                                 return workers_in_flight_ == 0;
                             });
    }
    // Drain Qt pool workers (pagination).  Unbounded: shutting_down_ is true
    // so no new runnables can be queued, and clear() removed pending ones.
    mediaPool_.waitForDone(-1);

    client_ = nullptr;
    event_handler_ = nullptr;

    // LoginView is a child widget, normally destroyed during ~QMainWindow
    // — but that runs *after* the AccountSessions (and thus their
    // Clients) are destroyed, and ~LoginView calls cancel_oauth on its
    // bound client. Tear it down here while everything is still alive.
    delete loginView_;
    loginView_ = nullptr;

    // Second pass: explicitly destroy all accounts and the pending-login
    // client HERE, while workers_mu_ / workers_cv_ are still alive (destructor
    // body, before ShellBase's member-destructor pass).
    //
    // Why this matters: in ShellBase, workers_mu_ is declared after accounts_,
    // so C++'s reverse-order member destruction destroys workers_mu_ BEFORE
    // accounts_.  If the 5-second wait above timed out (a thread was blocked
    // in a slow HTTP fetch that stop_sync() didn't cancel), the thread is still
    // alive when member destruction runs.  Each ~Client() calls rt.drop() which
    // shuts the tokio I/O driver; the blocked block_on() then unblocks and the
    // thread tries to acquire workers_mu_ — but the mutex is already destroyed
    // → heap corruption.
    //
    // By clearing accounts_ here, rt.drop() fires while workers_mu_ is still
    // alive.  The second wait below gives those threads time to lock workers_mu_,
    // decrement workers_in_flight_, and exit before the member-destructor pass
    // runs.  After the second wait, workers_mu_ can be safely destroyed.
    pending_login_client_.reset();
    accounts_.clear();
    {
        std::unique_lock<std::mutex> lk(workers_mu_);
        workers_cv_.wait_for(lk, std::chrono::seconds(5),
                             [this]
                             {
                                 return workers_in_flight_ == 0;
                             });
    }
}

#ifdef HAVE_XDG_ACTIVATION
namespace
{
// Lazily-bound xdg_activation_v1 global. Initialised once from the Wayland
// registry in setupLocalServer_() via a one-shot roundtrip. All access is on
// the main thread so no locking is needed.
xdg_activation_v1* s_xdgActivation = nullptr;

void registry_global_cb(void*, wl_registry* reg, uint32_t name,
                         const char* iface, uint32_t)
{
    if (!s_xdgActivation &&
        strcmp(iface, xdg_activation_v1_interface.name) == 0)
    {
        s_xdgActivation = static_cast<xdg_activation_v1*>(
            wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1));
    }
}
void registry_global_remove_cb(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {registry_global_cb,
                                                 registry_global_remove_cb};

// Token-request callback: stores the compositor-issued token string.
struct TokenResult { std::string value; };

void s_token_done(void* data, xdg_activation_token_v1* tok, const char* token)
{
    static_cast<TokenResult*>(data)->value = token ? token : "";
    xdg_activation_token_v1_destroy(tok);
}

const xdg_activation_token_v1_listener kTokenDoneListener = {s_token_done};
} // namespace
#endif

void MainWindow::setupLocalServer_()
{
    const QString name = QStringLiteral("tesseract-activate-")
                         + QString::number(getuid());
    QLocalServer::removeServer(name);
    localServer_ = new QLocalServer(this);
    localServer_->listen(name);
    connect(localServer_, &QLocalServer::newConnection, this,
            &MainWindow::onActivateRequested);

#ifdef HAVE_XDG_ACTIVATION
    // Bind xdg_activation_v1 from the Wayland registry so we can pass
    // compositor-issued tokens directly without going through Qt private API.
    auto* waylandApp =
        qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (waylandApp)
    {
        auto* wl_disp = waylandApp->display();
        if (wl_disp && !s_xdgActivation)
        {
            wl_registry* reg = wl_display_get_registry(wl_disp);
            wl_registry_add_listener(reg, &kRegistryListener, nullptr);
            wl_display_roundtrip(wl_disp);
            wl_registry_destroy(reg);
        }
    }
#endif
}

void MainWindow::activateWindowWithToken_(const QString& external_token)
{
    show();
    raise();
#ifdef HAVE_XDG_ACTIVATION
    auto* waylandApp =
        qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto* wl_disp = waylandApp ? waylandApp->display() : nullptr;
    // wl_surface* has no public Qt 6 accessor; nativeResourceForWindow is the
    // only non-private path available.
    auto* ni = QGuiApplication::platformNativeInterface();
    auto* surf = (ni && windowHandle()) ? static_cast<wl_surface*>(
        ni->nativeResourceForWindow("surface", windowHandle())) : nullptr;

    if (wl_disp && surf && s_xdgActivation)
    {
        std::string token = external_token.toStdString();

        if (token.empty())
        {
            // No externally-provided token (e.g. tray icon click).
            // Request a self-issued token synchronously, supplying the last
            // known input serial and seat as proof of user intent. Without a
            // valid serial (e.g. cold start with no prior input) compositors
            // enforcing focus-stealing prevention deny the request: KWin hands
            // back a "not-granted-*" token and only flags the window as
            // demanding attention rather than raising it. Foregrounding on
            // launch therefore relies on a granted XDG_ACTIVATION_TOKEN from
            // the launcher; a token-less launch (terminal/IDE) cannot steal
            // focus by design.
            TokenResult result;
            xdg_activation_token_v1* tok =
                xdg_activation_v1_get_activation_token(s_xdgActivation);
            xdg_activation_token_v1_set_app_id(tok, "tesseract");
            xdg_activation_token_v1_set_surface(tok, surf);
            if (waylandApp->lastInputSerial() && waylandApp->lastInputSeat())
            {
                xdg_activation_token_v1_set_serial(
                    tok, waylandApp->lastInputSerial(),
                    waylandApp->lastInputSeat());
            }
            xdg_activation_token_v1_add_listener(
                tok, &kTokenDoneListener, &result);
            xdg_activation_token_v1_commit(tok);
            wl_display_roundtrip(wl_disp);
            token = result.value;
        }

        if (!token.empty())
        {
            xdg_activation_v1_activate(
                s_xdgActivation, token.c_str(), surf);
            wl_display_flush(wl_disp);
            return;
        }
    }
#endif
    activateWindow();
}

void MainWindow::onActivateRequested()
{
    QLocalSocket* sock = localServer_->nextPendingConnection();
    if (!sock)
        return;
    auto doActivate = [this, sock]()
    {
        const QString token = QString::fromUtf8(sock->readAll()).trimmed();
        activateWindowWithToken_(token);
        sock->deleteLater();
    };
    // If data is already buffered (fast sender), read immediately.
    // Otherwise wait for readyRead; a safety timer cleans up if no data comes.
    if (sock->bytesAvailable() > 0)
    {
        doActivate();
    }
    else
    {
        connect(sock, &QLocalSocket::readyRead, this, doActivate);
        QTimer::singleShot(500, sock, [sock]() { sock->deleteLater(); });
    }
}

void MainWindow::activateOnStartup()
{
    const char* env_tok = std::getenv("XDG_ACTIVATION_TOKEN");
    const QString token = (env_tok && *env_tok) ? QString::fromUtf8(env_tok) : QString{};
    // Defer by one event-loop tick so the Wayland surface is fully mapped
    // before we attempt activation.
    QTimer::singleShot(0, this, [this, token]() { activateWindowWithToken_(token); });
}

void MainWindow::runOnPool_(std::function<void()> fn)
{
    if (shuttingDown_.load(std::memory_order_acquire))
    {
        return;
    }
    auto* runner = QRunnable::create(
        [this, fn = std::move(fn)]() mutable
        {
            if (shuttingDown_.load(std::memory_order_acquire))
            {
                return;
            }
            fn();
        });
    mediaPool_.start(runner);
}

void MainWindow::ensureViewerFullres_(const std::string& url)
{
    if (url.empty() || viewerFullresCache_.count(url) || anim_cache_.has(url))
    {
        return;
    }
    if (!viewerFullresInFlight_.insert(url).second)
    {
        return;
    }
    runOnPool_(
        [this, url]()
        {
            auto bytes = client_->fetch_source_bytes(url);
            QMetaObject::invokeMethod(
                this,
                [this, url, bytes = std::move(bytes)]() mutable
                {
                    viewerFullresInFlight_.erase(url);
                    if (bytes.empty() || viewerFullresCache_.count(url))
                    {
                        return;
                    }
                    QByteArray qb(
                        reinterpret_cast<const char*>(bytes.data()),
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
                },
                Qt::QueuedConnection);
        });
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
    if (ev->key() == Qt::Key_C && (ev->modifiers() & Qt::ControlModifier))
    {
        if (mainApp_ && mainApp_->room_view()->message_list()->has_selection())
        {
            mainApp_->room_view()->message_list()->copy_selection();
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

void MainWindow::changeEvent(QEvent* ev)
{
    QMainWindow::changeEvent(ev);
    if (ev->type() == QEvent::ActivationChange)
    {
        // Tell the PresenceTracker the window's foreground / background
        // status — gaining focus forces Online; losing focus arms the idle
        // decay (the periodic tick handles the actual transition).
        notify_window_active_(isActiveWindow());
    }
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
        loginView_->set_on_begin_oauth(
            [this]
            {
                if (!pending_login_temp_dir_.empty())
                {
                    return;
                }
                pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                    "pending-" +
                    std::to_string(QDateTime::currentMSecsSinceEpoch()));
                std::error_code ec;
                std::filesystem::create_directories(pending_login_temp_dir_,
                                                    ec);
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
        session->client = std::make_unique<tesseract::Client>();
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
        session->avatar_url = session->client->get_avatar_url();
        {
            auto prefs = tesseract::Prefs::parse(session->client->load_prefs_json());
            session->last_room  = prefs.last_room;
            session->open_rooms = prefs.open_rooms;
        }

        auto bridge = std::make_unique<EventBridge>(this, this);
        bridge->set_user_id(uid);
        session->client->start_sync(bridge.get());
        session->sync_started = true;
        session->bridge = std::move(bridge);

        // Per-account notifier: click switches to this account then navigates.
        session->notifier = std::make_unique<LinuxNotifierQt>(
            [this, uid](std::string room_id, std::string token)
            {
                for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
                {
                    if (accounts_[i]->user_id == uid)
                    {
                        switchActiveAccount(i);
                        break;
                    }
                }
                pending_wayland_token_ = QString::fromStdString(token);
                navigate_to_room(std::move(room_id));
            });

        // Per-account UnifiedPush connector (registers with distributor on start).
        auto up = std::make_unique<LinuxUpConnectorQt>();
        up->set_run_async(
            [this](std::function<void()> fn) { run_async_(std::move(fn)); });
        up->set_post_to_ui(
            [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); });
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
        loginView_->set_on_begin_oauth(
            [this]
            {
                if (!pending_login_temp_dir_.empty())
                {
                    return;
                }
                pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                    "pending-" +
                    std::to_string(QDateTime::currentMSecsSinceEpoch()));
                std::error_code ec;
                std::filesystem::create_directories(pending_login_temp_dir_,
                                                    ec);
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
    auto* active_up_connector =
        (active_account_index_ >= 0 &&
         active_account_index_ < static_cast<int>(accounts_.size()))
            ? accounts_[active_account_index_]->up_connector.get()
            : nullptr;
    settings_controller_ = std::make_unique<tesseract::SettingsController>(
        client_,
        [this](auto fn) { post_to_ui_(std::move(fn)); },
        [this](auto fn) { run_async_(std::move(fn)); },
        [this](auto cb)
        {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Select avatar image"), {},
                tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
            if (path.isEmpty()) return;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return;
            QByteArray data = f.readAll();
            const std::string mime =
                path.endsWith(".png",  Qt::CaseInsensitive) ? "image/png"  :
                path.endsWith(".gif",  Qt::CaseInsensitive) ? "image/gif"  :
                path.endsWith(".webp", Qt::CaseInsensitive) ? "image/webp" :
                "image/jpeg";
            std::vector<uint8_t> bytes(data.begin(), data.end());
            post_to_ui_([cb = std::move(cb),
                         bytes = std::move(bytes),
                         mime]() mutable
            {
                cb(std::move(bytes), mime);
            });
        });
    settings_controller_->set_up_connector(active_up_connector);
    if (settingsWidget_)
        settingsWidget_->set_controller(settings_controller_.get(),
                                        my_display_name_);
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainAppSurface_);
    maybeShowRecoveryBanner();

    if (!tray_)
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this] { activateWindowWithToken_(QString{}); },
            [this]
            {
                if (isVisible())
                    hide();
                else
                    activateWindowWithToken_(QString{});
            },
            [] { qApp->quit(); },
            this);
        if (tray_->is_available())
        {
            qApp->setQuitOnLastWindowClosed(false);
            // Seed the new tray with the current aggregate so an already-
            // unread state shows immediately instead of waiting for the next
            // sync tick to flip on_tray_unread_changed_.
            tray_->set_unread(last_tray_unread_, last_tray_highlight_);
        }
    }
}

void MainWindow::onLoginSucceeded()
{
    if (!pending_login_client_)
    {
        return; // defensive — should not happen
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
            statusBar()->showMessage(tr("Already signed in as %1")
                                         .arg(QString::fromStdString(user_id)),
                                     4000);
            pending_login_client_.reset();
            loginView_->set_client(nullptr);
            std::error_code ec;
            std::filesystem::remove_all(pending_login_temp_dir_, ec);
            // Restore previous active account's UI.
            if (add_account_return_idx_ >= 0 &&
                add_account_return_idx_ < static_cast<int>(accounts_.size()))
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
    std::filesystem::path final_dir =
        tesseract::SessionStore::account_dir(user_id);
    {
        std::error_code ec;
        std::filesystem::create_directories(final_dir.parent_path(), ec);
        std::filesystem::rename(pending_login_temp_dir_, final_dir, ec);
        if (ec)
        {
            // Rename failed — try recursive copy + remove, falling back
            // to leaving the data in the temp dir if even that fails.
            std::error_code ec2;
            std::filesystem::copy(
                pending_login_temp_dir_, final_dir,
                std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing,
                ec2);
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
        statusBar()->showMessage(tr("Sign-in failed: couldn't persist session"),
                                 6000);
        return;
    }

    // Open a fresh Client against the final store path and restore from
    // the just-exported session JSON (the matrix-sdk reuses the moved
    // SQLite store transparently — no resync).
    auto session = std::make_unique<tesseract::AccountSession>();
    session->user_id = user_id;
    session->client = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(user_id).string());
    auto res = session->client->restore_session(session_json);
    if (!res)
    {
        statusBar()->showMessage(tr("Sign-in failed at restore: %1")
                                     .arg(QString::fromStdString(res.message)),
                                 6000);
        tesseract::SessionStore::clear_account(user_id);
        return;
    }
    session->display_name = session->client->get_display_name();
    session->avatar_url = session->client->get_avatar_url();
    {
        auto prefs = tesseract::Prefs::parse(session->client->load_prefs_json());
        session->last_room  = prefs.last_room;
        session->open_rooms = prefs.open_rooms;
    }

    auto bridge = std::make_unique<EventBridge>(this, this);
    bridge->set_user_id(user_id);
    session->client->start_sync(bridge.get());
    session->sync_started = true;
    session->bridge = std::move(bridge);

    // Per-account notifier: click switches to this account then navigates.
    session->notifier = std::make_unique<LinuxNotifierQt>(
        [this, uid = user_id](std::string room_id, std::string token)
        {
            for (int i = 0; i < static_cast<int>(accounts_.size()); ++i)
            {
                if (accounts_[i]->user_id == uid)
                {
                    switchActiveAccount(i);
                    break;
                }
            }
            pending_wayland_token_ = QString::fromStdString(token);
            navigate_to_room(std::move(room_id));
        });

    // Per-account UnifiedPush connector.
    {
        auto up = std::make_unique<LinuxUpConnectorQt>();
        up->set_run_async(
            [this](std::function<void()> fn) { run_async_(std::move(fn)); });
        up->set_post_to_ui(
            [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); });
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
    auto* new_account_up_connector =
        (active_account_index_ >= 0 &&
         active_account_index_ < static_cast<int>(accounts_.size()))
            ? accounts_[active_account_index_]->up_connector.get()
            : nullptr;
    settings_controller_ = std::make_unique<tesseract::SettingsController>(
        client_,
        [this](auto fn) { post_to_ui_(std::move(fn)); },
        [this](auto fn) { run_async_(std::move(fn)); },
        [this](auto cb)
        {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Select avatar image"), {},
                tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
            if (path.isEmpty()) return;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return;
            QByteArray data = f.readAll();
            const std::string mime =
                path.endsWith(".png",  Qt::CaseInsensitive) ? "image/png"  :
                path.endsWith(".gif",  Qt::CaseInsensitive) ? "image/gif"  :
                path.endsWith(".webp", Qt::CaseInsensitive) ? "image/webp" :
                "image/jpeg";
            std::vector<uint8_t> bytes(data.begin(), data.end());
            post_to_ui_([cb = std::move(cb),
                         bytes = std::move(bytes),
                         mime]() mutable
            {
                cb(std::move(bytes), mime);
            });
        });
    settings_controller_->set_up_connector(new_account_up_connector);
    if (settingsWidget_)
        settingsWidget_->set_controller(settings_controller_.get(),
                                        my_display_name_);
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainAppSurface_);
    maybeShowRecoveryBanner();

    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;

    if (!tray_)
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this] { activateWindowWithToken_(QString{}); },
            [this]
            {
                if (isVisible())
                    hide();
                else
                    activateWindowWithToken_(QString{});
            },
            [] { qApp->quit(); },
            this);
        if (tray_->is_available())
        {
            qApp->setQuitOnLastWindowClosed(false);
            tray_->set_unread(last_tray_unread_, last_tray_highlight_);
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
        return; // no back-state in Initial mode
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
    QString token = pending_wayland_token_;
    pending_wayland_token_.clear();
    activateWindowWithToken_(token);
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

    hide_slash_popup_();
    hide_shortcode_popup_();
    hide_mention_popup_();
    handle_compose_room_leaving_(current_room_id_);
    if (!current_room_id_.empty() && current_room_id_ != room_id &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }

    current_room_id_ = room_id;
    // Prefetch members so mention pills (avatar) and mention clicks (name +
    // avatar) resolve without needing the room-info panel opened first.
    if (mainApp_ && mainApp_->room_view()->on_fetch_room_members)
    {
        mainApp_->room_view()->on_fetch_room_members(room_id);
    }
    clear_focused_state_(room_id);
    if (!markReadTimer_)
    {
        markReadTimer_ = new QTimer(this);
        markReadTimer_->setSingleShot(true);
        connect(markReadTimer_, &QTimer::timeout, this,
                [this]
                {
                    mark_room_read_(current_room_id_);
                });
    }
    markReadTimer_->start(
        tesseract::Settings::instance().mark_as_read_delay_ms);
    update_typing_bar_({}, false);
    reply_details_requested_.clear();
    {
        auto prefs = tesseract::Prefs::parse(client_->load_prefs_json());
        prefs.last_room = current_room_id_;
        prefs.open_rooms.clear();
        for (const auto& t : tabs_)
            prefs.open_rooms.push_back(t.room_id);
        if (prefs.open_rooms.empty())
            prefs.open_rooms.push_back(current_room_id_);
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
    {
        auto& state = pagination_[sub_room];
        if (state.in_flight)
            return;
        state.in_flight = true;
    }
    runOnPool_(
        [this, c, sub_room, visible_ids = std::move(visible_ids)]
        {
            auto res = c->subscribe_room(sub_room);
            bool ok = res.ok;
            std::string msg = res.message;
            bool reached = false;
            if (ok)
            {
                auto pr =
                    c->paginate_back_with_status(sub_room, kPaginationBatch);
                reached = pr.ok && pr.reached_start;
                c->start_background_backfill(visible_ids);
            }
            QMetaObject::invokeMethod(
                this,
                [this, sub_room, ok, msg = std::move(msg), reached]() mutable
                {
                    if (!ok)
                    {
                        statusBar()->showMessage(
                            tr("Subscribe failed: %1")
                                .arg(QString::fromStdString(msg)),
                            4000);
                        return;
                    }
                    if (current_room_id_ == sub_room)
                    {
                        auto& state = pagination_[sub_room];
                        state.in_flight = false;
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
    auto* c = client_; // snapshot: avoid account-switch race on client_
    if (!c)
    {
        state.in_flight = false;
        return;
    }
    runOnPool_(
        [this, c, room_id]
        {
            auto res = c->paginate_back_with_status(room_id, kPaginationBatch);
            bool reached = res.ok && res.reached_start;
            QMetaObject::invokeMethod(
                this, "onPaginateFinished", Qt::QueuedConnection,
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
    runOnPool_(
        [this, room_id, ts_ms]
        {
            auto res = client_->timestamp_to_event(room_id, ts_ms, "f");
            if (!res.ok)
            {
                QMetaObject::invokeMethod(
                    this,
                    [this, msg = res.message]
                    {
                        statusBar()->showMessage(
                            tr("Jump to date failed: %1")
                                .arg(QString::fromStdString(msg)),
                            4000);
                    },
                    Qt::QueuedConnection);
                return;
            }
            const std::string event_id = res.message;
            QMetaObject::invokeMethod(
                this,
                [this, room_id, event_id]
                {
                    begin_focused_subscription_(room_id, event_id);
                    runOnPool_(
                        [this, room_id, event_id]
                        {
                            client_->subscribe_room_at(room_id, event_id);
                        });
                },
                Qt::QueuedConnection);
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

void MainWindow::post_to_ui_after_(int ms, std::function<void()> fn)
{
    QTimer::singleShot(ms, this, std::move(fn));
}

void MainWindow::request_relayout_()
{
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
        mainAppSurface_->update();
    }
}

void MainWindow::request_repaint_()
{
    if (mainAppSurface_)
    {
        mainAppSurface_->update();
    }
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
    else if (!pending_restore_rooms_.empty())
    {
        if (try_restore_tab_session_(pending_restore_rooms_,
                                     pending_restore_rooms_[0]))
            pending_restore_rooms_.clear();
    }

    update_secondary_room_infos_();
}

void MainWindow::on_invites_updated_()
{
    if (mainApp_)
    {
        mainApp_->room_list_view()->set_invites(&invites_);
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
    }
}

void MainWindow::on_space_children_cache_ready_ui_()
{
    refreshRoomList();
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
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
        QImage scaled = img.scaled(kAvatarCacheSize, kAvatarCacheSize, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
        tk_avatars_.emplace(cache_key, tk::qt6::make_image(std::move(scaled)));
        if (mainAppSurface_)
        {
            mainAppSurface_->update();
        }
        if (mention_popup_visible_() && mention_popup_surface_)
        {
            mention_popup_surface_->update();
        }
        if (accountPickerPopover_ && accountPickerPopover_->isVisible() &&
            accountPickerSurface_)
        {
            accountPickerSurface_->update();
        }
        return;
    }

    if (kind == MediaKind::Tile)
    {
        if (tk_images_.count(cache_key))
        {
            return;
        }
        QImage img;
        if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                              static_cast<int>(bytes.size())))
        {
            return;
        }
        tk_images_.emplace(cache_key, tk::qt6::make_image(std::move(img)));
        if (mainApp_)
        {
            mainApp_->room_view()->message_list()->invalidate_data();
        }
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
    if (tk_images_.count(cache_key) || anim_cache_.has(cache_key))
    {
        mediaImageSizes_.erase(cache_key);
        return;
    }
    int max_w = kMaxImageWidth, max_h = kMaxImageHeight;
    if (auto sit = mediaImageSizes_.find(cache_key);
        sit != mediaImageSizes_.end())
    {
        max_w = sit->second.first;
        max_h = sit->second.second;
        mediaImageSizes_.erase(sit);
    }
    DecodedImage d = decode_image_(bytes, max_w, max_h);
    if (!d.frames.empty())
    {
        anim_cache_.store(cache_key, std::move(d.frames),
                          std::move(d.delays_ms),
                          QDateTime::currentMSecsSinceEpoch());
        if (tk_anim_timer_ && !tk_anim_timer_->isActive())
        {
            tk_anim_timer_->start();
        }
    }
    else if (d.still)
    {
        tk_images_.emplace(cache_key, std::move(d.still));
    }
    else
    {
        return;
    }
    if (mainApp_)
    {
        mainApp_->room_view()->notify_image_ready(cache_key);
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
        mainAppSurface_->update();
    }
    if (shortcode_popup_visible_() && shortcode_popup_surface_)
    {
        shortcode_popup_surface_->update();
    }
    return;
}

MainWindow::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                          int max_h)
{
    DecodedImage d;
    if (bytes.empty())
    {
        return d;
    }
    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    if (reader.supportsAnimation() && reader.imageCount() > 1)
    {
        QImage frame;
        while (reader.read(&frame))
        {
            int delay = reader.nextImageDelay();
            if (delay <= 0)
            {
                delay = 100;
            }
            if (delay < 20)
            {
                delay = 20;
            }
            QImage scaled = frame.scaled(max_w, max_h, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
            d.frames.push_back(tk::qt6::make_image(std::move(scaled)));
            d.delays_ms.push_back(delay);
        }
        if (!d.frames.empty())
        {
            return d;
        }
        d.delays_ms.clear();
        buf.seek(0);
    }
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(qb.constData()),
                          qb.size()))
    {
        return d;
    }
    QImage scaled =
        img.scaled(max_w, max_h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    d.still = tk::qt6::make_image(std::move(scaled));
    return d;
}

std::int64_t MainWindow::monotonic_ms_()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void MainWindow::start_anim_tick_()
{
    if (tk_anim_timer_ && !tk_anim_timer_->isActive())
    {
        tk_anim_timer_->start();
    }
}

void MainWindow::repaint_pickers_()
{
    if (emojiPicker_)
    {
        emojiPicker_->invalidateImages();
    }
    if (stickerPicker_)
    {
        stickerPicker_->invalidateImages();
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
        mainAppSurface_->update();
    }
    if (shortcode_popup_visible_() && shortcode_popup_surface_)
    {
        shortcode_popup_surface_->update();
    }
}

void MainWindow::extract_drop_video_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes)
{
    auto* player = new QMediaPlayer(this);
    auto* sink = new QVideoSink(player);
    player->setVideoSink(sink);
    auto* buf = new QBuffer(player);
    QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<qsizetype>(bytes.size()));
    buf->setData(ba);
    buf->open(QIODevice::ReadOnly);
    player->setSourceDevice(buf);

    struct State
    {
        bool done = false;
        tesseract::views::MediaInfo info;
    };
    auto state = std::make_shared<State>();
    state->info.pending_gen = pending_gen;

    QObject::connect(sink, &QVideoSink::videoFrameChanged, sink,
        [this, player, state](const QVideoFrame& frame)
        {
            if (state->done || !frame.isValid())
                return;
            state->done = true;
            state->info.duration_ms = static_cast<std::uint64_t>(
                std::max(qint64(0), player->duration()));
            player->stop();
            player->deleteLater();
            QImage img = frame.toImage();
            if (!img.isNull())
            {
                state->info.video_w = static_cast<std::uint32_t>(img.width());
                state->info.video_h = static_cast<std::uint32_t>(img.height());
                state->info.thumb_w = state->info.video_w;
                state->info.thumb_h = state->info.video_h;
                QByteArray enc;
                QBuffer encbuf(&enc);
                encbuf.open(QIODevice::WriteOnly);
                img.save(&encbuf, "JPEG", 85);
                state->info.thumb_bytes.assign(
                    reinterpret_cast<const std::uint8_t*>(enc.constData()),
                    reinterpret_cast<const std::uint8_t*>(enc.constData()) +
                        enc.size());
            }
            if (mainApp_)
                mainApp_->room_view()->compose_bar()
                    ->update_pending_attachment(state->info);
        });
    QObject::connect(player,
        qOverload<QMediaPlayer::Error, const QString&>(&QMediaPlayer::errorOccurred),
        player,
        [this, player, state](QMediaPlayer::Error, const QString&)
        {
            if (state->done) return;
            state->done = true;
            player->deleteLater();
            if (mainApp_)
                mainApp_->room_view()->compose_bar()
                    ->update_pending_attachment(state->info);
        });
    player->play();
}

void MainWindow::extract_drop_audio_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes)
{
    auto* player = new QMediaPlayer(this);
    auto* buf = new QBuffer(player);
    QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<qsizetype>(bytes.size()));
    buf->setData(ba);
    buf->open(QIODevice::ReadOnly);
    player->setSourceDevice(buf);

    auto done = std::make_shared<bool>(false);

    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, player,
        [this, player, pending_gen, done](QMediaPlayer::MediaStatus status)
        {
            if (*done) return;
            if (status != QMediaPlayer::LoadedMedia &&
                status != QMediaPlayer::BufferedMedia &&
                status != QMediaPlayer::EndOfMedia)
                return;
            *done = true;
            tesseract::views::MediaInfo info;
            info.pending_gen = pending_gen;
            info.duration_ms = static_cast<std::uint64_t>(
                std::max(qint64(0), player->duration()));
            player->stop();
            player->deleteLater();
            if (mainApp_)
                mainApp_->room_view()->compose_bar()
                    ->update_pending_attachment(info);
        });
    QObject::connect(player,
        qOverload<QMediaPlayer::Error, const QString&>(&QMediaPlayer::errorOccurred),
        player,
        [this, player, pending_gen, done](QMediaPlayer::Error, const QString&)
        {
            if (*done) return;
            *done = true;
            player->deleteLater();
            tesseract::views::MediaInfo info;
            info.pending_gen = pending_gen;
            if (mainApp_)
                mainApp_->room_view()->compose_bar()
                    ->update_pending_attachment(info);
        });
    player->play();
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                           const std::string& video_url)
{
    const std::string src = video_url;
    runOnPool_(
        [this, eid = event_id, src]()
        {
            auto bytes = client_->fetch_source_bytes(src);
            if (bytes.empty())
            {
                return;
            }
            // Decode the first frame on the UI thread — Qt multimedia
            // objects (QMediaPlayer, QVideoSink) must live there.
            QMetaObject::invokeMethod(
                this,
                [this, eid, bytes = std::move(bytes)]() mutable
                {
                    const std::string key = "thumb::" + eid;
                    if (tk_images_.count(key))
                    {
                        return;
                    }
                    auto* player = new QMediaPlayer(this);
                    auto* sink = new QVideoSink(player);
                    player->setVideoSink(sink);
                    auto* buf = new QBuffer(player);
                    QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<qsizetype>(bytes.size()));
                    buf->setData(ba);
                    buf->open(QIODevice::ReadOnly);
                    player->setSourceDevice(buf);
                    QObject::connect(
                        sink, &QVideoSink::videoFrameChanged, sink,
                        [this, key, player](const QVideoFrame& frame)
                        {
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
                                    reinterpret_cast<const uint8_t*>(
                                        enc.constData()),
                                    reinterpret_cast<const uint8_t*>(
                                        enc.constData()) +
                                        enc.size());
                                on_media_bytes_ready_(
                                    key, MediaKind::MediaImage, std::move(v));
                            }
                        });
                    player->play();
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::onMessageAnimTick_()
{
    tick_anim_();
}

void MainWindow::stop_anim_tick_()
{
    if (tk_anim_timer_)
    {
        tk_anim_timer_->stop();
    }
}

void MainWindow::repaint_anim_frame_()
{
    if (mainAppSurface_)
    {
        mainAppSurface_->update_anim_regions();
    }
    if (emojiPicker_ && emojiPicker_->isVisible())
    {
        emojiPicker_->invalidateImages();
    }
    if (stickerPicker_ && stickerPicker_->isVisible())
    {
        stickerPicker_->invalidateImages();
    }
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
    if (space_stack_.empty())
    {
        if (!roomSearchPendingText_.empty())
        {
            if (mainApp_)
            {
                mainApp_->set_space_nav(false);
            }
            showRooms(rooms_);
            return;
        }
        std::unordered_set<std::string> in_space;
        for (const auto& r : rooms_)
        {
            if (!r.is_space)
            {
                continue;
            }
            auto sc_it = space_children_cache_.find(r.id);
            if (sc_it != space_children_cache_.end())
            {
                for (const auto& id : sc_it->second)
                {
                    in_space.insert(id);
                }
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
        apply_space_child_counts_(filtered);
        if (mainApp_)
        {
            mainApp_->set_space_nav(false);
        }
        showRooms(filtered);
    }
    else
    {
        const std::string& space_id = space_stack_.back();
        static const std::vector<std::string> kNoChildren;
        const auto sc_it = space_children_cache_.find(space_id);
        const auto& child_ids =
            sc_it != space_children_cache_.end() ? sc_it->second : kNoChildren;
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : rooms_)
        {
            if (std::find(child_ids.begin(), child_ids.end(), r.id) !=
                child_ids.end())
            {
                filtered.push_back(r);
            }
        }
        if (mainApp_)
        {
            std::string space_name;
            std::string space_avatar;
            for (const auto& r : rooms_)
            {
                if (r.id == space_id)
                {
                    space_name = r.name;
                    space_avatar = r.avatar_url;
                    ensure_room_avatar_(r);
                    break;
                }
            }
            mainApp_->set_space_nav(true, space_name, space_avatar);
        }
        showRooms(filtered);
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

void MainWindow::prep_row_media_(const tesseract::Event& ev)
{
    // Store decode size hints before delegating to the ShellBase helper.
    if (ev.type == tesseract::EventType::Image)
    {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        if (img.source)
            mediaImageSizes_[img.source->fetch_token()] = {kMaxImageWidth, kMaxImageHeight};
    }
    else if (ev.type == tesseract::EventType::Sticker)
    {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        if (s.source)
            mediaImageSizes_[s.source->fetch_token()] = {kMaxStickerSize, kMaxStickerSize};
    }
    else if (ev.type == tesseract::EventType::Video)
    {
        const auto& vid = static_cast<const tesseract::VideoEvent&>(ev);
        if (vid.thumbnail)
            mediaImageSizes_[vid.thumbnail->fetch_token()] = {kMaxImageWidth, kMaxImageHeight};
    }
    for (const auto& r : ev.reactions)
    {
        if (r.source)
            mediaImageSizes_[r.source->fetch_token()] = {20, 20};
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
    if (verification_banner_dismissed_ == false &&
        mainApp_->verif_banner()->visible())
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
    auto b = key.find_last_not_of(" \t\r\n");
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

    runOnPool_(
        [this, k = key]()
        {
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
    recovery_key_chosen_ = false;
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
    using BS = tesseract::BackupState;

    const bool room_busy = (last_room_list_state_ == RLS::Init ||
                            last_room_list_state_ == RLS::SettingUp);
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
            connect(syncStatusDebounce_, &QTimer::timeout, this,
                    [this]
                    {
                        using RLS2 = tesseract::RoomListState;
                        if (last_room_list_state_ == RLS2::Init ||
                            last_room_list_state_ == RLS2::SettingUp)
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
    QAction* addAct = menu->addAction(tr("Add Account…"));
    QAction* settingsAct = menu->addAction(tr("Settings…"));
    QString logout_label =
        tr("Log Out %1")
            .arg(my_display_name_.empty()
                     ? QString::fromStdString(my_user_id_)
                     : QString::fromStdString(my_display_name_));
    QAction* logoutAct = menu->addAction(logout_label);
    menu->addSeparator();
    QAction* quitAct = menu->addAction(tr("Quit"));
    QObject::connect(menu, &QMenu::triggered, this,
                     [this, addAct, settingsAct, logoutAct, quitAct](QAction* a)
                     {
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

        connect(settingsWidget_, &SettingsWidget::settingsClosed, this,
                [this]
                {
                    contentStack_->setCurrentWidget(mainAppSurface_);
                });
        connect(settingsWidget_, &SettingsWidget::logoutRequested, this,
                [this]
                {
                    contentStack_->setCurrentWidget(mainAppSurface_);
                    logoutActiveAccount();
                });
        connect(settingsWidget_, &SettingsWidget::themeChanged, this,
                [this](tesseract::Settings::ThemePreference pref)
                {
                    set_theme_preference_(pref);
                });
        connect(settingsWidget_, &SettingsWidget::notificationsChanged, this,
                [this](bool enabled)
                {
                    if (settings_controller_)
                        settings_controller_->set_notifications_enabled(enabled);
                });
        connect(settingsWidget_, &SettingsWidget::presenceChanged, this,
                [this](bool enabled)
                {
                    handle_send_presence_toggle_(enabled);
                });
        connect(settingsWidget_, &SettingsWidget::roomListGroupingChanged, this,
                [this]
                {
                    if (mainApp_ && mainApp_->room_list_view())
                    {
                        mainApp_->room_list_view()->refresh();
                    }
                });

        connect(settingsWidget_, &SettingsWidget::clearCachesRequested, this,
                [this]
                {
                    clear_all_caches_([this](uint64_t local, uint64_t sdk)
                    {
                        if (settingsWidget_)
                            settingsWidget_->set_cache_sizes(local, sdk);
                    });
                });

        connect(settingsWidget_, &SettingsWidget::localAvatarChanged, this,
                [this](const QString& new_mxc)
                {
                    my_avatar_url_ = new_mxc.toStdString();
                    if (active_account_index_ >= 0 &&
                        active_account_index_ <
                            static_cast<int>(accounts_.size()))
                    {
                        accounts_[active_account_index_]->avatar_url =
                            my_avatar_url_;
                    }
                    populateUserStrip();
                });

        // server_info_ may have already arrived before this lazy widget was
        // created — apply it now so capability gating is correct on first open.
        settingsWidget_->set_server_info(server_info_);
    }

    settingsWidget_->populate(
        my_display_name_, my_user_id_, my_avatar_url_,
        [this](const std::string& mxc) -> const tk::Image*
        {
            auto it = tk_avatars_.find(mxc);
            return it != tk_avatars_.end() ? it->second.get() : nullptr;
        },
        tesseract::Settings::instance().theme_pref,
        tesseract::Settings::instance().notifications_enabled);

    if (settings_controller_)
        settingsWidget_->set_controller(settings_controller_.get(),
                                        my_display_name_);

    // Refresh storage sizes each time settings opens.
    compute_cache_sizes_([this](uint64_t local, uint64_t sdk)
    {
        if (settingsWidget_)
            settingsWidget_->set_cache_sizes(local, sdk);
    });

    contentStack_->setCurrentWidget(settingsWidget_);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (slash_popup_visible_() && event->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(event);
        QPoint global = me->globalPosition().toPoint();
        if (!slash_popup_frame_->rect().contains(
                slash_popup_frame_->mapFromGlobal(global)))
        {
            hide_slash_popup_();
        }
    }
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
    if (mention_popup_visible_() && event->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(event);
        QPoint global = me->globalPosition().toPoint();
        if (!mention_popup_frame_->rect().contains(
                mention_popup_frame_->mapFromGlobal(global)))
        {
            hide_mention_popup_();
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// EventHandlerBase UI-thread hook implementations (Qt6)
// ---------------------------------------------------------------------------

void MainWindow::handle_sync_error_ui_(std::string context, std::string user_id,
                                       std::string description,
                                       bool soft_logout)
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
            QTimer::singleShot(
                5000, this,
                [this, uid = affected->user_id]()
                {
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
            if (auto saved =
                    tesseract::SessionStore::load_account(affected->user_id))
            {
                statusBar()->showMessage(
                    tr("Reconnecting session\xe2\x80\xa6"));
                if (affected->client->restore_session(*saved))
                {
                    affected->display_name =
                        affected->client->get_display_name();
                    affected->avatar_url = affected->client->get_avatar_url();
                    if (affected ==
                        accounts_[std::max(0, active_account_index_)].get())
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
            tr("Sync error: %1").arg(QString::fromStdString(description)),
            8000);
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

    if (mainApp_ && mainApp_->recovery_banner()->visible() &&
        mainApp_->recovery_banner()->state() ==
            tesseract::views::RecoveryBanner::State::Importing &&
        progress.state == tesseract::BackupState::Downloading &&
        progress.imported_keys > 0)
    {
        mainApp_->recovery_banner()->set_import_progress(
            progress.imported_keys);
        mainAppSurface_->relayout();
    }
    if (progress.state == tesseract::BackupState::Enabled &&
        !client_->needs_recovery())
    {
        if (mainApp_)
        {
            mainApp_->show_recovery_banner(false);
            mainAppSurface_->relayout();
        }
        recovery_key_chosen_ = false;
    }

    last_backup_state_ = progress.state;
    last_imported_keys_ = progress.imported_keys;
    refreshSyncStatus();
}

void MainWindow::refresh_pickers_packs_()
{
    if (stickerPicker_)
    {
        stickerPicker_->refreshPacks();
    }
    if (emojiPicker_)
    {
        emojiPicker_->refreshEmoticonPacks();
    }
}

void MainWindow::handle_notification_ui_(
    std::string user_id, std::string room_id, std::string room_name,
    std::string sender, std::string body, bool is_mention,
    std::vector<uint8_t> avatar_bytes, std::vector<uint8_t> image_bytes)
{
    if (!tesseract::Settings::instance().notifications_enabled)
    {
        return;
    }
    apply_notification_redaction_(sender, room_name, body, avatar_bytes,
                                  image_bytes);

    bool win_visible = isVisible() && !isMinimized();
    bool win_focused = isActiveWindow();

    for (auto& sess : accounts_)
    {
        if (sess->user_id != user_id)
        {
            continue;
        }
        // Suppress only when the user is actively focused on this exact room.
        if (win_focused && active_account_index_ >= 0 &&
            accounts_[active_account_index_]->user_id == user_id &&
            current_room_id_ == room_id)
        {
            return;
        }
        // Flash the taskbar when the window is visible but not the active app.
        if (win_visible && !win_focused)
        {
            QApplication::alert(this, 0);
        }
        // Send a system notification regardless of window state unless already
        // watching the exact room above.
        if (sess->notifier)
        {
            tesseract::Notification n;
            n.room_id = room_id;
            n.room_name = room_name;
            n.sender = sender;
            n.body = body;
            n.is_mention = is_mention;
            n.avatar_bytes = std::move(avatar_bytes);
            n.image_bytes = std::move(image_bytes);
            sess->notifier->notify(n);
        }
        return;
    }
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────

void MainWindow::on_tab_state_changed_ui_()
{
    if (!mainApp_)
    {
        return;
    }

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
            std::string name;
            for (const auto& r : rooms_)
            {
                if (r.id != t.room_id)
                {
                    continue;
                }
                name = r.name;
                const std::string& av_mxc = r.effective_avatar_url();
                if (!av_mxc.empty())
                {
                    auto it = tk_avatars_.find(av_mxc);
                    if (it != tk_avatars_.end())
                    {
                        avatar = it->second.get();
                    }
                }
                break;
            }
            tb->add_tab(t.room_id, name, avatar);
        }

        if (active_tab_idx_ < tabs_.size())
        {
            tb->set_active(tabs_[active_tab_idx_].room_id);
        }
    }

    // Navigate to the active tab's room.
    if (active_tab_idx_ < tabs_.size())
    {
        const auto& active = tabs_[active_tab_idx_];
        onRoomSelected(active.room_id);

        // Restore compose draft (onRoomSelected clears it via set_text("")).
        if (!active.compose_draft.empty())
        {
            if (roomTextArea_)
            {
                roomTextArea_->set_text(active.compose_draft);
            }
            if (mainApp_)
            {
                mainApp_->room_view()->set_current_text(active.compose_draft);
            }
        }
    }

    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
    }
}

float MainWindow::get_message_scroll_fraction_()
{
    if (!mainApp_ || !mainApp_->room_view()->message_list())
    {
        return 0.f;
    }
    return mainApp_->room_view()->message_list()->scroll_fraction();
}

void MainWindow::set_message_scroll_fraction_(float t)
{
    if (!mainApp_ || !mainApp_->room_view()->message_list())
    {
        return;
    }
    mainApp_->room_view()->message_list()->scroll_to_offset(t);
}

std::string MainWindow::get_compose_draft_()
{
    if (!mainApp_ || !mainApp_->room_view()->compose_bar())
    {
        return {};
    }
    return mainApp_->room_view()->compose_bar()->current_text();
}

void MainWindow::set_compose_draft_(const std::string& draft)
{
    if (roomTextArea_)
    {
        roomTextArea_->set_text(draft);
    }
    if (mainApp_)
    {
        mainApp_->room_view()->set_current_text(draft);
    }
}


// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::on_room_list_state_ui_()
{
    refreshSyncStatus();
}

void MainWindow::on_server_info_ready_ui_()
{
    if (settingsWidget_)
        settingsWidget_->set_server_info(server_info_);
    if (mainApp_ && mainApp_->room_view())
        mainApp_->room_view()->header()->set_jump_to_date_enabled(
            server_info_.supports_msc3030);
    if (mainAppSurface_)
        mainAppSurface_->relayout();
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
    if (client_ && !current_room_id_.empty() &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }
    current_room_id_.clear();
    tabs_.clear();
    active_tab_idx_ = 0;
    // Per-account, room-id-keyed state must not bleed into the next account
    // (a room id present in both accounts would otherwise inherit stale
    // pagination / space-drill / reply-fetch state).
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();
    clearMessages();

    const int old_idx = active_account_index_;
    reset_server_info_();
    active_account_index_ = new_idx;
    auto& s = *accounts_[new_idx];
    client_ = s.client.get();
    event_handler_ =
        s.bridge.get(); // keep ShellBase's non-owning alias in sync

    my_user_id_ = s.user_id;
    my_display_name_ = s.display_name;
    my_avatar_url_ = s.avatar_url;
    pending_restore_rooms_ = s.open_rooms.empty()
        ? (s.last_room.empty() ? std::vector<std::string>{}
                               : std::vector<std::string>{s.last_room})
        : s.open_rooms;
    // Rotate last_room to [0] so it opens as the active tab.
    if (!s.last_room.empty() && !pending_restore_rooms_.empty() &&
        pending_restore_rooms_[0] != s.last_room)
    {
        auto it = std::find(pending_restore_rooms_.begin(),
                            pending_restore_rooms_.end(), s.last_room);
        if (it != pending_restore_rooms_.end())
            std::rotate(pending_restore_rooms_.begin(), it, it + 1);
    }

    if (settings_controller_)
    {
        settings_controller_->set_client(client_);
        settings_controller_->set_up_connector(s.up_connector.get());
    }

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
        // Rooms are already in cache — try to restore the tab session
        // immediately. on_rooms_updated_ handles the async case (no cache).
        if (!pending_restore_rooms_.empty())
        {
            if (try_restore_tab_session_(pending_restore_rooms_,
                                         pending_restore_rooms_[0]))
                pending_restore_rooms_.clear();
        }
    }
    else
    {
        rooms_.clear();
        refreshRoomList();
    }

    // Restore the invite snapshot for the incoming account (parallel to rooms_).
    auto inv_it = per_account_invites_.find(s.user_id);
    invites_ = (inv_it != per_account_invites_.end())
                   ? inv_it->second
                   : std::vector<tesseract::InviteInfo>{};
    on_invites_updated_();

    // Dismiss any stale InviteCard from the previous account.
    current_invite_room_id_.clear();
    current_invite_inviter_id_.clear();
    if (main_app_)
        main_app_->show_room();

    // Persist the active selection.
    tesseract::SessionStore::AccountIndex idx;
    idx.active_user_id = s.user_id;
    for (auto& a : accounts_)
    {
        idx.user_ids.push_back(a->user_id);
    }
    tesseract::SessionStore::save_index(idx);

    // Save banner state for the outgoing account, then load for the incoming.
    if (old_idx >= 0 && old_idx < static_cast<int>(accounts_.size()))
    {
        accounts_[old_idx]->recovery_banner_dismissed     = recovery_banner_dismissed_;
        accounts_[old_idx]->recovery_key_chosen           = recovery_key_chosen_;
        accounts_[old_idx]->verification_banner_dismissed = verification_banner_dismissed_;
    }
    if (mainApp_)
    {
        mainApp_->show_recovery_banner(false);
        mainApp_->show_verif_banner(false);
        mainAppSurface_->relayout();
    }
    recovery_banner_dismissed_     = s.recovery_banner_dismissed;
    recovery_key_chosen_           = s.recovery_key_chosen;
    verification_banner_dismissed_ = s.verification_banner_dismissed;

    rebuildAccountPicker();
    handle_verification_state_ui_(!s.unverified);
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
    loginView_->set_on_begin_oauth(
        [this]
        {
            if (!pending_login_temp_dir_.empty())
            {
                return;
            }
            pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                "pending-" +
                std::to_string(QDateTime::currentMSecsSinceEpoch()));
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
    notify_presence_logout_();
    a.client->logout();
    a.client->stop_sync();
    tesseract::SessionStore::clear_account(uid);
    per_account_rooms_.erase(uid);
    per_account_invites_.erase(uid);
    // Recompute the tray aggregate so the dot clears (or rolls over to the
    // surviving accounts) immediately; without this the indicator can stick
    // when the only account with unreads was the one we just signed out.
    notify_tray_unread_();

    // Remove this account from the live vector.
    accounts_.erase(accounts_.begin() + active_account_index_);
    active_account_index_ = -1;
    client_ = nullptr;
    event_handler_ = nullptr;

    // Reset visible state regardless of where we go next.
    current_room_id_.clear();
    space_stack_.clear();
    my_user_id_.clear();
    my_display_name_.clear();
    my_avatar_url_.clear();
    rooms_.clear();
    invites_.clear();
    current_invite_room_id_.clear();
    current_invite_inviter_id_.clear();
    reset_server_info_();
    refreshRoomList();
    clearMessages();
    if (mainApp_)
    {
        mainApp_->clear_content();
        mainApp_->show_recovery_banner(false);
        mainAppSurface_->relayout();
    }
    recovery_banner_dismissed_ = false;
    recovery_key_chosen_ = false;
    verification_banner_dismissed_ = false;

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
        loginView_->set_on_begin_oauth(
            [this]
            {
                if (!pending_login_temp_dir_.empty())
                {
                    return;
                }
                pending_login_temp_dir_ = tesseract::SessionStore::account_dir(
                    "pending-" +
                    std::to_string(QDateTime::currentMSecsSinceEpoch()));
                std::error_code ec;
                std::filesystem::create_directories(pending_login_temp_dir_,
                                                    ec);
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
    statusBar()->showMessage(
        tr("Signed out of %1").arg(QString::fromStdString(uid)), 3000);
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
            a.user_id,
            a.display_name,
            a.avatar_url,
            static_cast<int>(i) == active_account_index_,
        });
        if (!a.avatar_url.empty())
        {
            ensure_user_avatar_(a.avatar_url);
        }
    }
    accountPicker_->set_entries(std::move(entries));
}

static QString accountPopoverQss(const tk::Theme& t)
{
    const auto& p = t.palette;
    auto hex = [](const tk::Color& c)
    {
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
        accountPickerPopover_->setWindowFlags(Qt::Popup |
                                              Qt::FramelessWindowHint);
        accountPickerPopover_->setFrameShape(QFrame::Box);
        accountPickerPopover_->setStyleSheet(accountPopoverQss(current_theme_));
        auto* lay = new QVBoxLayout(accountPickerPopover_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        accountPickerSurface_ =
            new tk::qt6::Surface(tk::Theme::light(), accountPickerPopover_);
        auto picker_owner = std::make_unique<tesseract::views::AccountPicker>();
        accountPicker_ = picker_owner.get();
        accountPicker_->set_image_provider(make_avatar_image_provider_());
        accountPicker_->on_select = [this](const std::string& uid)
        {
            onAccountSelected(uid);
        };
        accountPickerSurface_->set_root(std::move(picker_owner));
        lay->addWidget(accountPickerSurface_);
    }
    rebuildAccountPicker();

    constexpr int kPickerWidth = 260;
    constexpr int kRowHeight = 56;
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
        mainApp_->verif_banner()->set_state(
            tesseract::views::VerificationBanner::State::Prompt);
        // Verification takes priority — hide recovery banner if it appeared
        // before the verification state callback arrived (race on first sync).
        // But if recovery is actively in progress (Verifying/Importing), let
        // it finish rather than interrupting with the verification banner.
        if (mainApp_->recovery_banner()->visible())
        {
            auto rs = mainApp_->recovery_banner()->state();
            if (rs == tesseract::views::RecoveryBanner::State::Form ||
                rs == tesseract::views::RecoveryBanner::State::Failed)
            {
                if (recovery_key_chosen_)
                {
                    return;
                }
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

void MainWindow::handle_verification_request_ui_(std::string flow_id,
                                                 std::string /*user_id*/,
                                                 std::string /*device_id*/,
                                                 bool incoming)
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
    mainApp_->verif_banner()->set_state(
        tesseract::views::VerificationBanner::State::Done);
    mainAppSurface_->relayout();
    QTimer::singleShot(1500, this,
                       [this]
                       {
                           if (mainApp_ && mainApp_->verif_banner()->on_done)
                           {
                               mainApp_->verif_banner()->on_done();
                           }
                       });
}

void MainWindow::handle_verification_cancelled_ui_(std::string /*flow_id*/,
                                                   std::string reason)
{
    if (!mainApp_)
    {
        return;
    }
    mainApp_->verif_banner()->set_state(
        tesseract::views::VerificationBanner::State::Cancelled);
    mainApp_->verif_banner()->set_cancel_reason(std::move(reason));
    mainApp_->show_verif_banner(true);
    mainAppSurface_->relayout();
}

tk::ThemeMode MainWindow::os_color_scheme_() const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto qt_scheme = QGuiApplication::styleHints()->colorScheme();
    if (qt_scheme != Qt::ColorScheme::Unknown)
    {
        return qt_scheme == Qt::ColorScheme::Dark ? tk::ThemeMode::Dark
                                                  : tk::ThemeMode::Light;
    }
#endif
    // Qt could not determine the OS color scheme (common on GNOME without
    // QGnomePlatform / Qt < 6.5). Fall back to the XDG portal value.
    return portal_color_scheme_ == 1 ? tk::ThemeMode::Dark
                                     : tk::ThemeMode::Light;
}

void MainWindow::read_portal_color_scheme_()
{
    QDBusInterface iface(QStringLiteral("org.freedesktop.portal.Desktop"),
                         QStringLiteral("/org/freedesktop/portal/desktop"),
                         QStringLiteral("org.freedesktop.portal.Settings"),
                         QDBusConnection::sessionBus());
    if (!iface.isValid())
    {
        return;
    }

    QDBusReply<QDBusVariant> reply = iface.call(
        QStringLiteral("Read"), QStringLiteral("org.freedesktop.appearance"),
        QStringLiteral("color-scheme"));
    if (reply.isValid())
    {
        portal_color_scheme_ = reply.value().variant().toInt();
    }
}

void MainWindow::on_portal_setting_changed_(const QString& ns,
                                            const QString& key,
                                            const QDBusVariant& value)
{
    if (ns != QLatin1String("org.freedesktop.appearance") ||
        key != QLatin1String("color-scheme"))
    {
        return;
    }
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

// ── Slash-command popup ─────────────────────────────────────────────────────

void MainWindow::show_slash_popup_(
    const std::vector<tesseract::views::SlashCommandSuggestion>& items,
    tk::Rect cursor_local)
{
    if (!slash_popup_frame_)
    {
        // Regular child widget — no separate window, so focus never leaves
        // the compose text area regardless of window manager behaviour.
        slash_popup_frame_ = new QWidget(this);
        slash_popup_frame_->setFocusPolicy(Qt::NoFocus);

        slash_popup_surface_ = std::make_unique<tk::qt6::Surface>(
            mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
            slash_popup_frame_,
            /*transparent=*/false);
        slash_popup_surface_->setFocusPolicy(Qt::NoFocus);

        auto widget = std::make_unique<tesseract::views::SlashCommandPopup>();
        slash_popup_widget_ = widget.get();
        slash_popup_surface_->set_root(std::move(widget));

        slash_popup_widget_->on_accepted =
            [this](tesseract::views::SlashCommandSuggestion s)
        {
            hide_slash_popup_();
            if (!roomTextArea_) return;
            if (!client_ || current_room_id_.empty())
            {
                // Account torn down while the popup was open — nothing to send.
                return;
            }
            if (s.args_hint.empty())
            {
                // No args — send immediately.
                std::string body = "/" + s.name;
                (void)tesseract::dispatch_compose_send(
                    *client_, current_room_id_, body, std::string{});
                roomTextArea_->set_text("");
                mainApp_->room_view()->clear_compose_text();
            }
            else
            {
                // Needs args — autocomplete to `/name ` and leave the
                // composer open for the user to type arguments. Use
                // replace_range (not set_text) so the caret lands after the
                // trailing space and the shared composer state stays in sync.
                std::string body = "/" + s.name + " ";
                roomTextArea_->replace_range(
                    0, static_cast<int>(roomTextArea_->text().size()), body);
            }
        };
        slash_popup_widget_->on_dismissed = [this]
        {
            hide_slash_popup_();
        };

        auto* lay = new QVBoxLayout(slash_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(slash_popup_surface_.get());
    }

    slash_popup_widget_->set_suggestions(items);
    slash_popup_widget_->set_selected_index(0);

    int rows = std::min((int)items.size(),
                        (int)tesseract::views::SlashCommandPopup::kMaxRows);
    int h = int(rows * tesseract::views::SlashCommandPopup::kRowHeight);
    int w = int(tesseract::views::SlashCommandPopup::kWidth);

    // Map cursor rect from surface-local into main-window coordinates.
    QPoint parent_cursor = mainAppSurface_->mapTo(
        this, QPoint(int(cursor_local.x), int(cursor_local.y)));

    QRect work = rect(); // main window bounds — popup is clipped to this
    int x = parent_cursor.x();
    int y_above = parent_cursor.y() - h - 4;
    int y_below = parent_cursor.y() + int(cursor_local.h) + 4;
    int y = (y_above >= work.top()) ? y_above : y_below;
    x = std::clamp(x, work.left(), work.right() - w);
    y = std::clamp(y, work.top(), work.bottom() - h);

    const bool was_hidden = !slash_popup_frame_->isVisible();
    slash_popup_frame_->setGeometry(x, y, w, h);
    slash_popup_surface_->resize(w, h);
    slash_popup_frame_->show();
    slash_popup_frame_->raise();
    slash_popup_surface_->relayout();
    if (was_hidden)
    {
        qApp->installEventFilter(this);
    }
}

void MainWindow::hide_slash_popup_()
{
    qApp->removeEventFilter(this);
    if (slash_popup_frame_)
    {
        slash_popup_frame_->hide();
    }
    if (roomTextArea_)
    {
        roomTextArea_->set_on_popup_nav(nullptr);
    }
}

// ── Shortcode popup ─────────────────────────────────────────────────────────

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

        shortcode_popup_widget_->on_accepted =
            [this](tesseract::views::ShortcodeSuggestion s)
        {
            std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
            roomTextArea_->replace_range(shortcode_active_match_.start,
                                         shortcode_active_match_.end,
                                         std::move(r));
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->on_dismissed = [this]
        {
            hide_shortcode_popup_();
        };
        shortcode_popup_widget_->set_image_provider(
            make_static_image_provider_());

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
    QPoint parent_cursor = mainAppSurface_->mapTo(
        this, QPoint(int(cursor_local.x), int(cursor_local.y)));

    QRect work = rect(); // main window bounds — popup is clipped to this
    int x = parent_cursor.x();
    int y_above = parent_cursor.y() - h - 4;
    int y_below = parent_cursor.y() + int(cursor_local.h) + 4;
    int y = (y_above >= work.top()) ? y_above : y_below;
    x = std::clamp(x, work.left(), work.right() - w);
    y = std::clamp(y, work.top(), work.bottom() - h);

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

// ── @mention popup ─────────────────────────────────────────────────────────

bool MainWindow::handle_mention_on_changed_(const std::string& s, int cursor)
{
    auto m = mention_engine_.find_prefix(s, cursor);
    if (!m)
    {
        return false;
    }
    // Member list must be fetched off the UI thread (get_room_members blocks).
    // When the cache is stale, kick off an async fetch and re-run once it lands
    // — the popup appears on the next tick rather than stalling input.
    if (cached_members_room_ != current_room_id_)
    {
        if (members_fetching_room_ != current_room_id_ && client_)
        {
            members_fetching_room_ = current_room_id_;
            auto* c = client_;
            std::string rid = current_room_id_;
            run_async_(
                [this, c, rid]
                {
                    auto members = c->get_room_members(rid);
                    post_to_ui_(
                        [this, rid, members = std::move(members)]() mutable
                        {
                            cached_room_members_ = std::move(members);
                            cached_members_room_ = rid;
                            members_fetching_room_.clear();
                            if (roomTextArea_)
                            {
                                handle_mention_on_changed_(
                                    roomTextArea_->text(),
                                    roomTextArea_->cursor_byte_pos());
                            }
                        });
                });
        }
        return false;
    }
    mention_current_candidates_ =
        mention_engine_.lookup(m->prefix, cached_room_members_, 8, true);
    if (mention_current_candidates_.empty())
    {
        return false;
    }
    mention_active_match_ = *m;
    hide_shortcode_popup_(); // clears popup_nav_ — must reinstall below
    show_mention_popup_(mention_current_candidates_,
                        roomTextArea_->cursor_rect());
    // Reinstall every time: hide_shortcode_popup_() above (and a prior
    // keystroke) clear popup_nav_, so a conditional install would leave nav
    // dead after the first character following the popup appearing.
    {
        roomTextArea_->set_on_popup_nav(
            [this](tk::NativeTextArea::NavKey nk) -> bool
            {
                if (!mention_popup_visible_())
                {
                    return false;
                }
                int cur = mention_popup_widget_->selected_index();
                int n = mention_popup_widget_->visible_rows();
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
                    int sel = mention_popup_widget_->selected_index();
                    if (sel >= 0 &&
                        sel < (int)mention_current_candidates_.size())
                    {
                        accept_mention_(mention_current_candidates_[sel]);
                    }
                    else
                    {
                        hide_mention_popup_();
                    }
                    return true;
                }
                case tk::NativeTextArea::NavKey::ShiftTab:
                    return false;
                case tk::NativeTextArea::NavKey::Escape:
                    hide_mention_popup_();
                    return true;
                }
                mention_popup_widget_->set_selected_index(next);
                mention_popup_surface_->update();
                return true;
            });
    }
    return true;
}

void MainWindow::accept_mention_(const tesseract::views::MentionCandidate& c)
{
    if (roomTextArea_)
    {
        roomTextArea_->insert_mention(mention_active_match_.start,
                                      mention_active_match_.end, c.user_id,
                                      c.display_name, c.is_room);
    }
    hide_mention_popup_();
}

void MainWindow::show_mention_popup_(
    const std::vector<tesseract::views::MentionCandidate>& candidates,
    tk::Rect cursor_local)
{
    if (!mention_popup_frame_)
    {
        mention_popup_frame_ = new QWidget(this);
        mention_popup_frame_->setFocusPolicy(Qt::NoFocus);

        mention_popup_surface_ = std::make_unique<tk::qt6::Surface>(
            mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
            mention_popup_frame_,
            /*transparent=*/false);
        mention_popup_surface_->setFocusPolicy(Qt::NoFocus);

        auto widget = std::make_unique<tesseract::views::MentionPopup>();
        mention_popup_widget_ = widget.get();
        mention_popup_surface_->set_root(std::move(widget));

        mention_popup_widget_->on_accepted =
            [this](tesseract::views::MentionCandidate c)
        {
            accept_mention_(c);
        };
        mention_popup_widget_->on_dismissed = [this] { hide_mention_popup_(); };
        mention_popup_widget_->set_image_provider(
            make_avatar_image_provider_());

        auto* lay = new QVBoxLayout(mention_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(mention_popup_surface_.get());
    }

    // Kick off avatar fetches; they appear on a later repaint once decoded.
    for (const auto& cand : candidates)
    {
        if (!cand.is_room && !cand.avatar_url.empty())
        {
            ensure_user_avatar_(cand.avatar_url);
        }
    }

    mention_popup_widget_->set_candidates(candidates);

    int rows = std::min((int)candidates.size(),
                        (int)tesseract::views::MentionPopup::kMaxRows);
    int h = int(rows * tesseract::views::MentionPopup::kRowHeight);
    int w = int(tesseract::views::MentionPopup::kWidth);

    QPoint parent_cursor = mainAppSurface_->mapTo(
        this, QPoint(int(cursor_local.x), int(cursor_local.y)));

    QRect work = rect();
    int x = parent_cursor.x();
    int y_above = parent_cursor.y() - h - 4;
    int y_below = parent_cursor.y() + int(cursor_local.h) + 4;
    int y = (y_above >= work.top()) ? y_above : y_below;
    x = std::clamp(x, work.left(), work.right() - w);
    y = std::clamp(y, work.top(), work.bottom() - h);

    const bool was_hidden = !mention_popup_frame_->isVisible();
    mention_popup_frame_->setGeometry(x, y, w, h);
    mention_popup_surface_->resize(w, h);
    mention_popup_frame_->show();
    mention_popup_frame_->raise();
    mention_popup_surface_->relayout();
    if (was_hidden)
    {
        qApp->installEventFilter(this);
    }
}

void MainWindow::hide_mention_popup_()
{
    if (!mention_popup_visible_())
    {
        return;
    }
    qApp->removeEventFilter(this);
    if (mention_popup_frame_)
    {
        mention_popup_frame_->hide();
    }
    if (roomTextArea_)
    {
        roomTextArea_->set_on_popup_nav(nullptr);
    }
}

// ---------------------------------------------------------------------------

void MainWindow::apply_theme_ui_(const tk::Theme& t)
{
    if (brandingSurface_)
    {
        brandingSurface_->set_theme(t);
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->set_theme(t);
    }
    if (accountPickerSurface_)
    {
        accountPickerSurface_->set_theme(t);
    }
    if (slash_popup_surface_)
    {
        slash_popup_surface_->set_theme(t);
    }
    if (shortcode_popup_surface_)
    {
        shortcode_popup_surface_->set_theme(t);
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
        roomTextArea_->set_text_color(t.palette.text_primary);
    if (roomSearchField_)
        roomSearchField_->set_text_color(t.palette.text_primary);
    if (recoveryKeyField_)
        recoveryKeyField_->set_text_color(t.palette.text_primary);
    {
        const auto& p = t.palette;
        QPalette pal = statusBar()->palette();
        pal.setColor(QPalette::Window, QColor(p.chrome_bg.r, p.chrome_bg.g,
                                              p.chrome_bg.b, p.chrome_bg.a));
        pal.setColor(QPalette::WindowText,
                     QColor(p.text_secondary.r, p.text_secondary.g,
                            p.text_secondary.b, p.text_secondary.a));
        statusBar()->setPalette(pal);
        statusBar()->setAutoFillBackground(true);
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
    }
}

} // namespace qt6
