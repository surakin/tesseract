#include "MainWindow.h"
#include "LinuxNotifier.h"
#include "LinuxUpConnectorQt.h"
#include "views/ComposePopups.h"
#include "LoginView.h"
#include "views/BrandView.h"
#include "views/media_drop.h"
#include "SettingsWidget.h"
#include "EmojiPicker.h"
#include "StickerPicker.h"
#include "JoinRoomDialog.h"
#include "LinuxScreenLockQt.h"
#include "app/SlashCommands.h"
#include "app/status_links.h"

#include "tk/canvas_qpainter.h"
#include "tk/theme.h"
#include "tk/video_decode.h"

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
#include <QClipboard>
#include <QStyleHints>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QShortcut>
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
#include <QDate>
#include <QDateTime>
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
#include <cstdlib>
#include <filesystem>
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

MainWindow::MainWindow(tesseract::AccountManager& account_manager, QWidget* parent)
    : QMainWindow(parent)
    , ShellBase(account_manager)
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
    mainAppSurface_->set_anim_cache(&account_manager_.anim_cache());

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

    // Apply saved window geometry, or use the platform default.
    {
        const auto geom = clamp_to_screens_(
            tesseract::Settings::instance().main_window_geometry,
            1100, 768, get_screen_work_areas_());
        if (geom.valid)
            setGeometry(geom.x, geom.y, geom.w, geom.h);
        else
            resize(1100, 768);
    }

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
            // A space is not a room: clicking one drills into it (show its
            // children + the back affordance) rather than opening it as the
            // active room/tab. Without this guard tab_select_room would set
            // the space as current_room_id_ and surface its title in the
            // room header.
            for (const auto& r : rooms_)
            {
                if (r.id == room_id && r.is_space)
                {
                    space_stack_.push_back(room_id);
                    refreshRoomList();
                    return;
                }
            }
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
                        if (!mainApp_ || !active_account_)
                        {
                            return;
                        }
                        auto ids =
                            mainApp_->room_list_view()->visible_room_ids();
                        auto sess = active_account_;
                        run_async_mut_([sess, ids = std::move(ids)]() mutable
                        {
                            if (sess && sess->client)
                            {
                                sess->client->stop_background_backfill();
                                sess->client->start_background_backfill(ids);
                            }
                        });
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
        mainApp_->room_list_view()->on_unjoined_room_selected =
            [this](const tesseract::RoomSummary& s)
        {
            if (!s.avatar_url.empty())
                ensure_media_thumbnail_(s.avatar_url, 64, 64, false);
            if (mainApp_)
            {
                mainApp_->show_room_preview(s, make_avatar_image_provider_());
                request_relayout_();
            }
        };
        if (auto* rp = mainApp_->room_preview())
        {
            rp->on_avatar_needed = [this](const std::string& mxc)
            {
                ensure_media_thumbnail_(mxc, 64, 64, false);
            };
            rp->on_join = [this, rp](const std::string& room_id)
            {
                rp->set_state(tesseract::views::RoomPreviewView::State::Joining);
                join_room_command_(room_id);
            };
            rp->on_dismiss = [this]
            {
                if (mainApp_)
                    mainApp_->hide_room_preview();
            };
        }

        // ---- Tab bar ----
        mainApp_->tab_bar()->on_tab_selected =
            [this](const std::string& room_id)
        {
            // Ctrl+click pops the room out into its own window (and closes the
            // tab); a plain click just switches to it.
            if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier)
            {
                tab_popout_room(room_id);
            }
            else
            {
                tab_select_room(room_id);
            }
        };
        mainApp_->tab_bar()->on_tab_closed = [this](const std::string& room_id)
        {
            tab_close(room_id);
        };

        // ---- User info strip ----
        mainApp_->user_info()->on_primary = [this](tk::Point world)
        {
            if (account_manager_.accounts().size() < 2)
            {
                return;
            }
            openAccountPicker(mainAppSurface_->mapToGlobal(
                QPoint(static_cast<int>(world.x), static_cast<int>(world.y))));
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
            // The recovery-key entry path now lives in the encryption-setup
            // overlay (Recover mode); the old inline RecoveryBanner was removed.
            show_encryption_setup_overlay_(
                tesseract::views::EncryptionSetupOverlay::Mode::Recover);
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

        // The image_viewer provider (full-res cache → anim → image → thumbnail)
        // is installed by the shared wire_main_app_viewers_ above.

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
                    return account_manager_.thumbnail_cache().peek(m.avatar_url);
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
                // Non-blocking: warmed bytes or empty + async fetch (repaint on
                // arrival) so playback never freezes the UI thread.
                return voice_bytes_or_fetch_(source_json,
                                             [this] { request_relayout_(); });
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
                run_async_(
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
        setup_link_clicked_(mainApp_->room_view());
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
            run_async_mut_(
                [this, room, event_id]
                {
                    client_->subscribe_room_at(room, event_id);
                });
        };
        mainApp_->room_view()->on_date_jump = [this](std::uint64_t ts_ms)
        {
            handle_date_jump_(ts_ms);
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
            auto outcome = dispatch_room_send_(current_room_id_, msg.body,
                                               msg.formatted_body);
            if (outcome.handled_as_command || outcome.send_result)
            {
                if (roomTextArea_)
                {
                    roomTextArea_->set_text("");
                }
                mainApp_->room_view()->clear_compose_text();
            }
            else
            {
                statusBar()->showMessage(
                    QString::fromStdString(outcome.send_result.message), 4000);
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
            ensure_viewer_fullres_(src_tok);
        };

        // Avatar click → open the lightbox with the *original* avatar mxc,
        // not the 80×80 thumbnail stored in tk_avatars_. The shared
        // wire_main_app_widget_ already wires a basic on_avatar_clicked
        // that uses the thumbnail; override here so focus is grabbed and the
        // viewer gets the full-resolution decode via ensure_viewer_fullres_.
        mainApp_->room_view()->on_avatar_clicked =
            [this](std::string url, std::string name)
        {
            if (url.empty())
                return;
            mainApp_->image_viewer()->open(url, url, name, 0, 0);
            mainApp_->show_image_viewer(true);
            mainAppSurface_->relayout();
            mainAppSurface_->setFocus();
            ensure_viewer_fullres_(url);
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
            run_async_(
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
                hit.duration_ms, hit.natural_w, hit.natural_h,
                hit.loop, hit.no_audio, hit.hide_controls);
            mainApp_->show_video_viewer(true);
            mainAppSurface_->relayout();
            mainAppSurface_->setFocus();
            std::string src = src_tok;
            run_async_(
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
            run_async_(
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
            run_async_(
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
            run_async_mut_(
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
            run_async_mut_(
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
            run_async_mut_(
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
            run_async_(
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
    // All four composer popups (gif > slash > shortcode > mention) are driven
    // through the shared ComposePopups dispatch; the controllers are created
    // just below (slash/shortcode/mention) and in the GIF block further down.
    roomTextArea_->set_on_changed(
        [this](const std::string& s)
        {
            if (mainApp_)
            {
                mainApp_->room_view()->set_current_text(s);
            }
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
            {
                return;
            }
            onSendClicked();
        });
    roomTextArea_->set_on_popup_nav(
        [this](tk::NativeTextArea::NavKey nk) -> bool
        {
            return tesseract::views::dispatch_compose_nav(
                nk, gif_controller_.get(), slash_controller_.get(),
                shortcode_controller_.get(), mention_controller_.get());
        });

    // ── /command autocomplete popup ───────────────────────────────────────
    slash_popup_frame_ = new QWidget(this);
    slash_popup_frame_->setFocusPolicy(Qt::NoFocus);
    slash_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
        slash_popup_frame_, /*transparent=*/false);
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
        sh.hide = [this] { hide_slash_popup_(); };
        sh.repaint = [this]
        {
            if (slash_popup_surface_)
                slash_popup_surface_->update();
        };
        sh.room_id = [this] { return current_room_id_; };
        sh.client = [this]() -> tesseract::Client* { return client_; };
        sh.clear_composer = [this]
        {
            if (mainApp_)
                mainApp_->room_view()->clear_compose_text();
        };
        slash_controller_ =
            std::make_unique<tesseract::views::SlashCommandController>(
                roomTextArea_.get(), slash_popup_widget_, std::move(sh));
    }

    // ── :shortcode: emoji/emoticon autocomplete popup ─────────────────────
    shortcode_popup_frame_ = new QWidget(this);
    shortcode_popup_frame_->setFocusPolicy(Qt::NoFocus);
    shortcode_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
        shortcode_popup_frame_, /*transparent=*/false);
    shortcode_popup_surface_->setFocusPolicy(Qt::NoFocus);
    {
        auto w = std::make_unique<tesseract::views::ShortcodePopup>();
        shortcode_popup_widget_ = w.get();
        shortcode_popup_widget_->set_image_provider(
            make_static_image_provider_());
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
        sh.hide = [this] { hide_shortcode_popup_(); };
        sh.repaint = [this]
        {
            if (shortcode_popup_surface_)
                shortcode_popup_surface_->update();
        };
        sh.emoticons = [this]() -> const std::vector<tesseract::ImagePackImage>&
        { return cached_emoticons_; };
        sh.fetch_image = [this](const std::string& url)
        { ensure_media_image_(url, 28, 28); };
        shortcode_controller_ =
            std::make_unique<tesseract::views::ShortcodeController>(
                roomTextArea_.get(), shortcode_popup_widget_, std::move(sh));
    }

    // ── @mention autocomplete popup ───────────────────────────────────────
    mention_popup_frame_ = new QWidget(this);
    mention_popup_frame_->setFocusPolicy(Qt::NoFocus);
    mention_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
        mention_popup_frame_, /*transparent=*/false);
    mention_popup_surface_->setFocusPolicy(Qt::NoFocus);
    {
        auto w = std::make_unique<tesseract::views::MentionPopup>();
        mention_popup_widget_ = w.get();
        mention_popup_widget_->set_image_provider(
            make_avatar_image_provider_());
        mention_popup_surface_->set_root(std::move(w));
        auto* lay = new QVBoxLayout(mention_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(mention_popup_surface_.get());
    }
    mention_popup_frame_->hide();
    {
        tesseract::views::MentionController::Hooks mh;
        mh.show = [this](tk::Rect cursor, int rows)
        { show_mention_popup_(cursor, rows); };
        mh.hide = [this] { hide_mention_popup_(); };
        mh.repaint = [this]
        {
            if (mention_popup_surface_)
                mention_popup_surface_->update();
        };
        mh.room_id = [this] { return current_room_id_; };
        mh.client = [this]() -> tesseract::Client* { return client_; };
        mh.fetch_avatar = [this](const std::string& mxc)
        { ensure_user_avatar_(mxc); };
        mh.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        mh.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        mention_controller_ =
            std::make_unique<tesseract::views::MentionController>(
                roomTextArea_.get(), client_, mention_popup_widget_,
                std::move(mh));
    }

    // ── GIF picker (/gif <query>) ────────────────────────────────────────────
    // Eagerly create the strip + controller so the on_changed/on_submit lambdas
    // above (which capture `this`) can drive it once the user types `/gif `.
    gif_popup_frame_ = new QWidget(this);
    gif_popup_frame_->setFocusPolicy(Qt::NoFocus);
    gif_popup_frame_->hide();
    gif_popup_surface_ = std::make_unique<tk::qt6::Surface>(
        mainAppSurface_ ? mainAppSurface_->theme() : tk::Theme::light(),
        gif_popup_frame_, /*transparent=*/false);
    gif_popup_surface_->setFocusPolicy(Qt::NoFocus);
    gif_popup_surface_->set_anim_cache(&account_manager_.anim_cache());
    {
        auto w = std::make_unique<tesseract::views::GifPopup>();
        gif_popup_widget_ = w.get();
        gif_popup_surface_->set_root(std::move(w));
        auto* lay = new QVBoxLayout(gif_popup_frame_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(gif_popup_surface_.get());
    }
    // Strip preview provider: two-stage loading.
    // Stage 1 — preview_url (static JPEG, fast): shown immediately as fallback.
    // Stage 2 — image_url (MP4/WebP/GIF): replaces static once decoded.
    // Two-stage GIF strip cell provider, parameterised on a `repaint` callback
    // so the identical body serves the main window's strip and every pop-out's
    // (each passes a repaint targeting its own popup surface, self-guarded by
    // that window's liveness token). Stored as a member; pop-outs reach it via
    // the gif_strip_image_() override.
    gif_strip_provider_ =
        [this](const tesseract::GifResult& result,
               const std::function<void()>& repaint) -> const tk::Image*
        {
            // The strip animates strip_url (WebP/GIF, native decode), keyed in
            // anim_cache. Serving a cached frame means animated content is on
            // screen, so make sure the tick timer is running: re-shown searches
            // take this path without re-fetching, skipping the fetch-path start.
            if (const tk::Image* f = account_manager_.anim_cache().current_frame(result.strip_url))
            {
                start_anim_tick_();
                return f;
            }
            // NOTE: the static-preview fallback is returned at the *end* of this
            // lambda, AFTER the animated re-fetch is kicked below. Returning it
            // here would short-circuit re-animation on a re-shown search whose
            // anim_cache_ entry was evicted (budget/TTL) while its static
            // thumbnail lingers in gif_previews_.
            // Kick off static preview fetch only when not already cached.
            if (!gif_previews_.count(result.preview_url) &&
                gif_preview_inflight_.insert(result.preview_url).second)
            {
                auto alive = gif_alive_;
                auto url = result.preview_url;
                run_async_(
                    [this, url, alive, repaint]
                    {
                        // Disk-cache the preview too, symmetrically with the
                        // animated source below. Otherwise a GIF whose MP4 is
                        // already on disk loads its video faster than its
                        // preview downloads from the network, so the cell stays
                        // blank until the video appears instead of showing the
                        // thumbnail first.
                        const std::string disk_key = gif_src_disk_key_(url);
                        std::vector<std::uint8_t> bytes =
                            account_manager_.media_disk_cache().load(disk_key);
                        if (bytes.empty() && client_)
                        {
                            bytes = client_->fetch_url_bytes(url);
                            if (!bytes.empty())
                                account_manager_.media_disk_cache().store(disk_key, bytes);
                        }
                        post_to_ui_(
                            [this, url, b = std::move(bytes), alive,
                             repaint]() mutable
                            {
                                if (!*alive)
                                    return;
                                gif_preview_inflight_.erase(url);
                                if (b.empty())
                                    return;
                                using CW = tesseract::views::GifPopup;
                                DecodedImage d = decode_image_(
                                    b, int(CW::kCellW) * 2, int(CW::kCellH) * 2);
                                if (d.still)
                                    gif_previews_[url] = std::move(d.still);
                                repaint();
                            });
                    });
            }
            // Kick off the strip-display fetch (strip_url: WebP/GIF) — decode
            // entirely on the worker thread. The MP4 send form is fetched
            // separately at send time, not here.
            if (gif_anim_inflight_.insert(result.strip_url).second)
            {
                auto alive = gif_alive_;
                auto anim_url = result.strip_url;
                auto anim_mime = result.strip_mime;
                run_async_(
                    [this, anim_url, anim_mime, alive, repaint]
                    {
                        // Source bytes: disk cache first, else download and
                        // persist so the send path (accept) reuses them without
                        // a second round-trip.
                        const std::string disk_key = gif_src_disk_key_(anim_url);
                        std::vector<std::uint8_t> bytes =
                            account_manager_.media_disk_cache().load(disk_key);
                        if (bytes.empty() && client_)
                        {
                            bytes = client_->fetch_url_bytes(anim_url);
                            if (!bytes.empty())
                                account_manager_.media_disk_cache().store(disk_key, bytes);
                        }
                        using CW = tesseract::views::GifPopup;
                        if (!bytes.empty() && anim_mime == "video/mp4")
                        {
                            // Decode all frames off the UI thread.
                            tk::DecodedVideoFrames dvf = tk::decode_video_frames(
                                bytes.data(), bytes.size(),
                                int(CW::kCellW) * 2, int(CW::kCellH) * 2);
                            // Wrap frames in shared_ptr so the lambda is
                            // copy-constructible (required by std::function).
                            auto imgs = std::make_shared<
                                std::vector<std::unique_ptr<tk::Image>>>();
                            std::vector<int> delays;
                            for (auto& f : dvf.frames)
                            {
                                // GStreamer BGRA is byte-order B,G,R,A, which
                                // matches QImage::Format_ARGB32 on little-endian
                                // (0xAARRGGBB). Format_RGBA8888 would swap R/B.
                                QImage qi(f.bgra.data(), f.w, f.h,
                                          QImage::Format_ARGB32);
                                imgs->push_back(
                                    tk::qt6::make_image(qi.copy()));
                                delays.push_back(f.delay_ms);
                            }
                            post_to_ui_(
                                [this, anim_url, imgs,
                                 delays = std::move(delays), alive,
                                 repaint]() mutable
                                {
                                    if (!*alive)
                                        return;
                                    gif_anim_inflight_.erase(anim_url);
                                    if (!imgs->empty())
                                    {
                                        account_manager_.anim_cache().store(
                                            anim_url, std::move(*imgs),
                                            std::move(delays),
                                            QDateTime::currentMSecsSinceEpoch());
                                        if (tk_anim_timer_ &&
                                            !tk_anim_timer_->isActive())
                                            tk_anim_timer_->start();
                                    }
                                    repaint();
                                });
                        }
                        else
                        {
                            // WebP/GIF (or empty bytes): decode off UI thread.
                            auto d = std::make_shared<DecodedImage>(
                                bytes.empty()
                                    ? DecodedImage{}
                                    : decode_image_(bytes,
                                                    int(CW::kCellW) * 2,
                                                    int(CW::kCellH) * 2));
                            post_to_ui_(
                                [this, anim_url, d, alive, repaint]() mutable
                                {
                                    if (!*alive)
                                        return;
                                    gif_anim_inflight_.erase(anim_url);
                                    if (!d->frames.empty())
                                    {
                                        account_manager_.anim_cache().store(
                                            anim_url, std::move(d->frames),
                                            std::move(d->delays_ms),
                                            QDateTime::currentMSecsSinceEpoch());
                                        if (tk_anim_timer_ &&
                                            !tk_anim_timer_->isActive())
                                            tk_anim_timer_->start();
                                    }
                                    else if (d->still)
                                    {
                                        gif_previews_[anim_url] =
                                            std::move(d->still);
                                    }
                                    repaint();
                                });
                        }
                    });
            }
            // Static JPEG preview shown while the animation decodes (or as the
            // permanent fallback for a non-animated result).
            if (auto it = gif_previews_.find(result.preview_url);
                it != gif_previews_.end())
                return it->second.get();
            return nullptr;
        };
    // The main window's own strip repaints its own surface.
    gif_popup_widget_->set_image_provider(
        [this](const tesseract::GifResult& result) -> const tk::Image*
        {
            return gif_strip_provider_(result,
                                       [this]
                                       {
                                           if (gif_popup_surface_)
                                               gif_popup_surface_->update();
                                       });
        });
    {
        tesseract::views::GifController::Hooks gh;
        gh.show = [this] { show_gif_popup_(); };
        gh.hide = [this] { hide_gif_popup_(); };
        gh.repaint = [this]
        {
            if (gif_popup_surface_)
                gif_popup_surface_->update();
        };
        gh.room_id = [this] { return current_room_id_; };
        gh.client = [this]() -> tesseract::Client* { return client_; };
        gh.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        gh.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        gh.post_delayed = [this](int ms, std::function<void()> fn)
        {
            if (mainAppSurface_)
                mainAppSurface_->host().post_delayed(ms, std::move(fn));
        };
        gh.api_key = []() -> std::string
        { return tesseract::Settings::instance().gif_api_key; };
        gh.client_key = []() -> std::string { return "tesseract"; };
        gh.clear_composer = [this]
        {
            if (roomTextArea_)
                roomTextArea_->set_text("");
            if (mainApp_ && mainApp_->room_view())
                mainApp_->room_view()->set_current_text("");
        };
        gh.get_cached_gif_bytes =
            [this](const std::string& url) -> std::vector<std::uint8_t>
        {
            // The strip persisted the source bytes to the disk cache on fetch;
            // reuse them so a selected GIF sends without a second download.
            return account_manager_.media_disk_cache().load(gif_src_disk_key_(url));
        };
        gif_controller_ = std::make_unique<tesseract::views::GifController>(
            roomTextArea_.get(), gif_popup_widget_, std::move(gh));
    }

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

    quickSwitchField_ = mainAppSurface_->host().make_text_field();
    quickSwitchField_->set_text_color(
        mainAppSurface_->theme().palette.text_primary);
    quickSwitchField_->set_placeholder(
        tr("Jump to a room, or @user to start a chat\xe2\x80\xa6").toStdString());
    quickSwitchField_->set_visible(false);
    quickSwitchField_->set_on_changed(
        [this](const std::string& q)
        {
            if (mainApp_ && mainApp_->quick_switcher())
            {
                mainApp_->quick_switcher()->set_query(q);
                mainAppSurface_->relayout();
            }
        });
    quickSwitchField_->set_on_submit(
        [this]
        {
            if (mainApp_ && mainApp_->quick_switcher())
                mainApp_->quick_switcher()->activate_selected();
        });
    quickSwitchField_->set_on_popup_nav(
        [this](tk::NavKey nk) -> bool
        {
            auto* qs = mainApp_ ? mainApp_->quick_switcher() : nullptr;
            if (!qs || !qs->is_open())
                return false;
            switch (nk)
            {
            case tk::NavKey::Up:
                qs->move_selection(-1);
                mainAppSurface_->relayout();
                return true;
            case tk::NavKey::Down:
                qs->move_selection(+1);
                mainAppSurface_->relayout();
                return true;
            case tk::NavKey::Escape:
                closeQuickSwitch_();
                return true;
            default:
                return false;
            }
        });
    if (mainApp_ && mainApp_->quick_switcher())
        mainApp_->quick_switcher()->on_close = [this] { closeQuickSwitch_(); };

    // Message search (Ctrl+Shift+F) native field — mirrors the quick switcher.
    messageSearchField_ = mainAppSurface_->host().make_text_field();
    messageSearchField_->set_text_color(
        mainAppSurface_->theme().palette.text_primary);
    messageSearchField_->set_placeholder(
        tr("Search your messages\xe2\x80\xa6").toStdString());
    messageSearchField_->set_visible(false);
    messageSearchField_->set_on_changed(
        [this](const std::string& q)
        {
            if (mainApp_ && mainApp_->message_search())
            {
                mainApp_->message_search()->set_query(q);
                mainAppSurface_->relayout();
            }
        });
    messageSearchField_->set_on_submit(
        [this]
        {
            if (mainApp_ && mainApp_->message_search())
                mainApp_->message_search()->activate_selected();
        });
    messageSearchField_->set_on_popup_nav(
        [this](tk::NavKey nk) -> bool
        {
            auto* ms = mainApp_ ? mainApp_->message_search() : nullptr;
            if (!ms || !ms->is_open())
                return false;
            switch (nk)
            {
            case tk::NavKey::Up:
                ms->move_selection(-1);
                mainAppSurface_->relayout();
                return true;
            case tk::NavKey::Down:
                ms->move_selection(+1);
                mainAppSurface_->relayout();
                return true;
            case tk::NavKey::Escape:
                closeMessageSearch_();
                return true;
            default:
                return false;
            }
        });
    if (mainApp_ && mainApp_->message_search())
        mainApp_->message_search()->on_close = [this] { closeMessageSearch_(); };

    // Per-room "find in conversation" (Ctrl+F) native field — docked under the
    // RoomHeader inside the shared search bar strip. Mirrors messageSearchField_
    // except: no popup-nav (UP/DOWN are button clicks in the strip), submit is
    // a no-op, and Escape closes the bar.
    findInRoomField_ = mainAppSurface_->host().make_text_field();
    findInRoomField_->set_text_color(
        mainAppSurface_->theme().palette.text_primary);
    findInRoomField_->set_placeholder(
        tr("Find in conversation\xe2\x80\xa6").toStdString());
    findInRoomField_->set_visible(false);
    findInRoomField_->set_on_changed(
        [this](const std::string& q)
        {
            if (mainApp_ && mainApp_->room_view() &&
                mainApp_->room_view()->room_search_bar())
            {
                mainApp_->room_view()->room_search_bar()->set_query(q);
                mainAppSurface_->relayout();
            }
        });
    findInRoomField_->set_on_popup_nav(
        [this](tk::NavKey nk) -> bool
        {
            auto* rv = mainApp_ ? mainApp_->room_view() : nullptr;
            if (!rv || !rv->room_search_open())
                return false;
            switch (nk)
            {
            case tk::NavKey::Up:
                if (rv->on_room_search_navigate) rv->on_room_search_navigate(-1);
                mainAppSurface_->relayout();
                return true;
            case tk::NavKey::Down:
                if (rv->on_room_search_navigate) rv->on_room_search_navigate(+1);
                mainAppSurface_->relayout();
                return true;
            case tk::NavKey::Escape:
                closeFindInRoom_();
                return true;
            default:
                return false;
            }
        });
    if (mainApp_ && mainApp_->room_view())
        mainApp_->room_view()->room_search_bar()->on_close =
            [this] { closeFindInRoom_(); };

    // Ctrl+K accelerator. An application-scoped QShortcut fires even when the
    // native compose / search QLineEdit/QTextEdit holds focus — keyPressEvent
    // on the window does not see keys consumed by a focused child widget.
    {
        auto* sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this, [this] { openQuickSwitch_(); });
    }
    // Ctrl+Shift+F: open global message search (application-scoped so it fires
    // while the compose box holds focus).
    {
        auto* sc = new QShortcut(
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this,
                [this] { openMessageSearch_(); });
    }
    // Ctrl+F: open per-room "find in conversation" (application-scoped so it
    // fires while the compose box holds focus; no-op when no room is open).
    {
        auto* sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this,
                [this] { openFindInRoom_(); });
    }
    // Alt+Left / Alt+Right: navigate room history back / forward.
    // ApplicationShortcut so these fire while the compose box holds focus.
    {
        auto* sc = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this,
                [this] { navigate_history_back(); });
    }
    {
        auto* sc = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
        sc->setContext(Qt::ApplicationShortcut);
        connect(sc, &QShortcut::activated, this,
                [this] { navigate_history_forward(); });
    }

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
            if (mainApp_ && quickSwitchField_)
            {
                const bool vis = mainApp_->quick_switch_field_visible();
                quickSwitchField_->set_visible(vis);
                if (vis)
                    quickSwitchField_->set_rect(
                        mainApp_->quick_switch_field_rect());
            }
            if (mainApp_ && messageSearchField_)
            {
                const bool vis = mainApp_->message_search_field_visible();
                messageSearchField_->set_visible(vis);
                if (vis)
                    messageSearchField_->set_rect(
                        mainApp_->message_search_field_rect());
            }
            if (mainApp_ && findInRoomField_)
            {
                const bool vis = mainApp_->in_room_search_field_visible();
                findInRoomField_->set_visible(vis);
                if (vis)
                    findInRoomField_->set_rect(
                        mainApp_->in_room_search_field_rect());
            }
            if (mainApp_ && encPassphraseField_)
            {
                encPassphraseField_->set_visible(
                    mainApp_->encryption_setup_passphrase_field_visible());
                encPassphraseField_->set_rect(
                    mainApp_->encryption_setup_passphrase_field_rect());
            }
            if (mainApp_ && encKeyField_)
            {
                encKeyField_->set_visible(
                    mainApp_->encryption_setup_key_field_visible());
                encKeyField_->set_rect(
                    mainApp_->encryption_setup_key_field_rect());
            }
            if (mainApp_ && qrCheckCodeField_)
            {
                const bool vis = mainApp_->qr_grant_check_code_field_visible();
                qrCheckCodeField_->set_visible(vis);
                if (vis)
                    qrCheckCodeField_->set_rect(
                        mainApp_->qr_grant_check_code_field_rect());
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
            auto outcome = tesseract::views::dispatch_file_drop(
                *mainApp_->room_view()->compose_bar(), std::move(bytes),
                std::move(mime), std::move(filename),
                client_->media_upload_limit(),
                [this](std::uint32_t gen, std::vector<std::uint8_t> b,
                       std::string m)
                { extract_drop_media_(gen, std::move(b), std::move(m)); });
            if (outcome == tesseract::views::FileDropOutcome::TooLarge)
                show_status_message_(tr("File exceeds the upload limit").toStdString());
        });
    mainAppSurface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            show_status_message_(std::move(reason));
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
    joinRoomDialog_->onOpenUrl = [this](const std::string& url)
    {
        if (tesseract::Client::parse_matrix_link(url).kind !=
            tesseract::Client::MatrixLink::Kind::Unknown)
            open_matrix_link(url);
        else
            tesseract::Client::open_in_browser(url);
    };

    statusBar()->showMessage(tr("Not logged in"));
    inflightDot_ = new InflightDotWidget(this);
    inflightDot_->setContentsMargins(0, 0, 2, 0);
    statusBar()->addPermanentWidget(inflightDot_);
    init_pool_callbacks_();
    on_inflight_ui_();

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

    account_manager_.register_window(this);
    broadcast_rebuild_tray_();
}

MainWindow::~MainWindow()
{
    // unregister_window is called in closeEvent (before quit) so it is not
    // repeated here; broadcast_rebuild_tray_ is still needed to refresh any
    // remaining windows' tray menus after this window's C++ resources are freed.
    broadcast_rebuild_tray_();

    // Signal Rust's cancellation channel first so any worker thread
    // currently blocked inside a `block_on(tokio::select! { stop_rx })`
    // FFI call returns immediately.  drain() can then join all threads
    // without blocking.  The invariant "no worker is calling client_->*
    // when the client is destroyed" is still satisfied because drain()
    // runs before the client destructor.
    // Multi-window: only the primary (non-pinned) window tears down the SHARED
    // accounts' background sync. The primary is a stack local destroyed only at
    // app exit, so its destruction == shutdown. A secondary (pinned) window
    // closing must leave every account syncing for the surviving windows; it
    // still drains its own per-window pools below.
    if (!is_pinned_window_)
    {
        for (auto& a : account_manager_.accounts())
        {
            if (a && a->client)
                a->client->stop_sync();
        }
    }
    if (pending_login_client_)
        pending_login_client_->stop_sync();
    pool_.drain();
    mut_pool_.drain();

    client_ = nullptr;
    event_handler_ = nullptr;

    // LoginView is a child widget, normally destroyed during ~QMainWindow
    // — but that runs *after* the AccountSessions (and thus their
    // Clients) are destroyed, and ~LoginView calls cancel_oauth on its
    // bound client. Tear it down here while everything is still alive.
    delete loginView_;
    loginView_ = nullptr;

    pending_login_client_.reset();
    // AccountManager owns the sessions — they are cleaned up when
    // account_manager_ is destroyed by the caller.
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
        // Protocol: newline-delimited.
        //   Line 1: XDG_ACTIVATION_TOKEN (may be empty)
        //   Line 2: matrix: URI to navigate to (optional)
        const QByteArray all = sock->readAll();
        const int nl = all.indexOf('\n');
        const QString token =
            (nl >= 0 ? all.left(nl) : all).trimmed();
        activateWindowWithToken_(token);
        if (nl >= 0)
        {
            const QString uri = QString::fromUtf8(all.mid(nl + 1)).trimmed();
            if (!uri.isEmpty())
                openMatrixLink(uri.toStdString());
        }
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

// ---------------------------------------------------------------------------

// eventFilter is defined further down (multi-account section) so it can
// dispatch user-strip left-clicks into the AccountPicker popover.

void MainWindow::openQuickSwitch_()
{
    if (!mainApp_ || !mainApp_->quick_switcher())
        return;
    // The Ctrl+K shortcut is application-scoped, so this can fire while a
    // pop-out room window holds focus. Bring the main window forward (the
    // switcher lives here) so it is visible and its search field can focus.
    if (!isActiveWindow())
        activateWindowWithToken_(QString{});
    mainApp_->show_quick_switch(true);
    if (mainAppSurface_)
        mainAppSurface_->relayout();
    if (quickSwitchField_)
    {
        quickSwitchField_->set_text("");
        quickSwitchField_->set_focused(true);
    }
}

void MainWindow::closeQuickSwitch_()
{
    if (mainApp_)
        mainApp_->show_quick_switch(false);
    if (quickSwitchField_)
        quickSwitchField_->set_visible(false);
    if (mainAppSurface_)
        mainAppSurface_->relayout();
}

void MainWindow::openMessageSearch_()
{
    if (!mainApp_ || !mainApp_->message_search())
        return;
    // Application-scoped shortcut: may fire while a pop-out holds focus. Bring
    // the main window forward (search lives here) so its field can focus.
    if (!isActiveWindow())
        activateWindowWithToken_(QString{});
    mainApp_->show_message_search(true);
    if (mainAppSurface_)
        mainAppSurface_->relayout();
    if (messageSearchField_)
    {
        messageSearchField_->set_text("");
        messageSearchField_->set_focused(true);
    }
}

void MainWindow::closeMessageSearch_()
{
    if (mainApp_)
        mainApp_->show_message_search(false);
    if (messageSearchField_)
        messageSearchField_->set_visible(false);
    if (mainAppSurface_)
        mainAppSurface_->relayout();
}

void MainWindow::openFindInRoom_()
{
    if (!mainApp_ || !mainApp_->room_view())
        return;
    if (current_room_id_.empty())
        return;
    mainApp_->room_view()->open_room_search();
    if (mainAppSurface_)
        mainAppSurface_->relayout();
    if (findInRoomField_)
    {
        findInRoomField_->set_text("");
        findInRoomField_->set_focused(true);
    }
}

void MainWindow::closeFindInRoom_()
{
    if (mainApp_ && mainApp_->room_view())
        mainApp_->room_view()->close_room_search();
    if (findInRoomField_)
    {
        findInRoomField_->set_visible(false);
        findInRoomField_->set_text("");
    }
    if (mainAppSurface_)
        mainAppSurface_->relayout();
}

void MainWindow::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape)
    {
        // Quick switcher is the topmost modal — close it first.
        if (mainApp_ && mainApp_->quick_switcher() &&
            mainApp_->quick_switcher()->is_open())
        {
            closeQuickSwitch_();
            ev->accept();
            return;
        }
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
        if (mainApp_ && mainApp_->room_view() &&
            mainApp_->room_view()->room_search_open())
        {
            closeFindInRoom_();
            ev->accept();
            return;
        }
    }
    // Ctrl+K is handled by an application-scoped QShortcut (see ctor) so it
    // works while a native text widget has focus; no keyPressEvent branch.
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
    auto& g = tesseract::Settings::instance().main_window_geometry;
    const QRect r = geometry();
    g.x = r.x(); g.y = r.y(); g.w = r.width(); g.h = r.height();
    g.valid = (g.w > 0 && g.h > 0);
    save_settings_debounced_();
}

void MainWindow::moveEvent(QMoveEvent* ev)
{
    QMainWindow::moveEvent(ev);
    auto& g = tesseract::Settings::instance().main_window_geometry;
    const QRect r = geometry();
    g.x = r.x(); g.y = r.y(); g.w = r.width(); g.h = r.height();
    g.valid = (g.w > 0 && g.h > 0);
    save_settings_debounced_();
}

std::vector<tk::Rect> MainWindow::get_screen_work_areas_() const
{
    std::vector<tk::Rect> result;
    for (QScreen* s : QGuiApplication::screens())
    {
        const QRect r = s->availableGeometry();
        result.push_back({static_cast<float>(r.x()),
                          static_cast<float>(r.y()),
                          static_cast<float>(r.width()),
                          static_cast<float>(r.height())});
    }
    return result;
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
    // Secondary (spawned) window: the shared AccountManager is already populated
    // and syncing, and set_initial_account() pinned the account to display. Bind
    // the UI to it without touching disk, restoring, or re-adding accounts.
    if (is_secondary_window_startup_())
    {
        finishLoginUi_(active_account_->user_id);
        return;
    }

    statusBar()->showMessage(tr("Restoring sessions\xe2\x80\xa6"));

    // Migrate + restore every stored account (shared loop in ShellBase). The
    // native per-account notifier / UnifiedPush construction runs through
    // install_account_notifier_ / install_account_up_connector_ below.
    auto restore = restore_all_accounts_();

    if (!restore.any_accounts)
    {
        // Fresh install or every stored account failed to restore → login view.
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        loginView_->set_client(pending_login_client_.get());
        loginView_->set_on_begin_oauth([this] { arm_pending_login_(); });
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Not logged in"));
        if (restore.any_restore_failed)
            loginView_->show_restore_error(restore.restore_error,
                                           [this] { doLogin(); });
        return;
    }

    finishLoginUi_(restore.active_uid);
}

std::unique_ptr<tesseract::IEventHandler>
MainWindow::make_account_bridge_(const std::string& uid)
{
    auto bridge = std::make_unique<EventBridge>(this);
    bridge->set_user_id(uid);
    return bridge;
}

void MainWindow::install_account_notifier_(tesseract::AccountSession& session)
{
    // Per-account notifier: click switches to this account then navigates.
    const std::string uid = session.user_id;
    session.notifier = std::make_unique<LinuxNotifierQt>(
        [this, uid](std::string room_id, std::string token)
        {
            switchActiveAccount(uid);
            pending_wayland_token_ = QString::fromStdString(token);
            navigate_to_room(std::move(room_id));
        });
}

void MainWindow::install_account_up_connector_(tesseract::AccountSession& session)
{
    // Per-account UnifiedPush connector (registers with distributor on start).
    auto up = std::make_unique<LinuxUpConnectorQt>();
    up->set_run_async(
        [this](std::function<void()> fn) { run_async_(std::move(fn)); });
    up->set_post_to_ui(
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); });
    up->start(session.client.get(), session.user_id);
    session.up_connector = std::move(up);
}

void MainWindow::finishLoginUi_(const std::string& uid)
{
    switchActiveAccount(uid);
    ensure_settings_controller_();
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainAppSurface_);

    // Exactly one window owns the single app-wide tray icon (multi-window).
    if (!tray_ && account_manager_.claim_tray_owner(this))
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this]
            {
                // If the unread room is popped out, raise that window instead.
                if (focus_tray_unread_popout_())
                    return;
                activateWindowWithToken_(QString{});
                navigate_tray_unread_();
            },
            [this]
            {
                // If the unread room is popped out, raise that window instead.
                if (focus_tray_unread_popout_())
                    return;
                if (isVisible() && !last_tray_unread_)
                    hide();
                else
                {
                    activateWindowWithToken_(QString{});
                    navigate_tray_unread_();
                }
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

    // The LoginView holds a raw alias to pending_login_client_; clear it before
    // finalize_login_ resets the client underneath us.
    loginView_->set_client(nullptr);

    // Agnostic add-account core: dedupe check, export/rename-to-final/save,
    // reopen + restore + prefs, bridge/notifier/UP via the shared install hooks,
    // add_account, index update. See ShellBase::finalize_login_.
    const auto fin = finalize_login_();

    if (fin.rejected_duplicate)
    {
        statusBar()->showMessage(tr("Already signed in as %1")
                                     .arg(QString::fromStdString(fin.user_id)),
                                 4000);
        // Restore previous active account's UI.
        const int back = add_account_return_idx_;
        if (back >= 0 && back < static_cast<int>(account_manager_.accounts().size()))
        {
            switchActiveAccount(account_manager_.accounts()[back]->user_id);
            contentStack_->setCurrentWidget(mainAppSurface_);
        }
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        return;
    }

    if (!fin.ok)
    {
        statusBar()->showMessage(
            tr("Sign-in failed: %1").arg(QString::fromStdString(fin.error)),
            6000);
        return;
    }

    switchActiveAccount(fin.user_id);
    ensure_settings_controller_();
    statusBar()->showMessage(tr("Connected"));
    contentStack_->setCurrentWidget(mainAppSurface_);

    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;

    // Exactly one window owns the single app-wide tray icon (multi-window).
    if (!tray_ && account_manager_.claim_tray_owner(this))
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this]
            {
                // If the unread room is popped out, raise that window instead.
                if (focus_tray_unread_popout_())
                    return;
                activateWindowWithToken_(QString{});
                navigate_tray_unread_();
            },
            [this]
            {
                // If the unread room is popped out, raise that window instead.
                if (focus_tray_unread_popout_())
                    return;
                if (isVisible() && !last_tray_unread_)
                    hide();
                else
                {
                    activateWindowWithToken_(QString{});
                    navigate_tray_unread_();
                }
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
    loginView_->set_client(nullptr);
    pending_login_client_.reset();
    if (!pending_login_is_add_account_)
    {
        return; // no back-state in Initial mode
    }

    int back = add_account_return_idx_;
    pending_login_is_add_account_ = false;
    add_account_return_idx_ = -1;
    if (back >= 0 && back < static_cast<int>(account_manager_.accounts().size()))
    {
        switchActiveAccount(account_manager_.accounts()[back]->user_id);
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
    // Hand this window's account bridge back to the primary, release its dedicated
    // mapping and tray ownership (multi-window), then unregister.
    on_window_closing_();
    account_manager_.unregister_window(this);
    if (account_manager_.window_count() == 0)
        QApplication::quit();
    ev->accept();
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
    if (const auto* r = room_by_id_(room_id); r && r->is_space)
    {
        space_stack_.push_back(room_id);
        refreshRoomList();
        return;
    }

    // Route through the controllers so their visible_ state stays in sync with
    // the hidden frames.
    if (slash_controller_)
        slash_controller_->hide();
    if (shortcode_controller_)
        shortcode_controller_->hide();
    if (mention_controller_)
        mention_controller_->hide();
    handle_compose_room_leaving_(current_room_id_);
    // (No unsubscribe-on-leave here: the warm-subscription LRU in
    // ShellBase::prune_warm_subscriptions_ now owns timeline lifecycle, keeping
    // recently-left rooms warm for instant reuse and evicting the rest.)
    current_room_id_ = room_id;
    // Member prefetch (for mention pills/clicks) now lives in the shared
    // RoomView::set_room(), so every shell gets it without wiring it here.
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
    persist_room_layout_pref_();
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

    if (const auto* r = room_by_id_(current_room_id_))
    {
        if (mainApp_)
        {
            mainApp_->room_view()->set_room(*r);
        }
    }

    // Subscribe (mut pool) + initial history (shared pool). The split keeps the
    // network paginate off the single mut thread so the next switch's reset is
    // never blocked. See ShellBase::start_room_subscription_.
    auto visible_ids = mainApp_ ? mainApp_->room_list_view()->visible_room_ids()
                                : std::vector<std::string>{};
    start_room_subscription_(current_room_id_, std::move(visible_ids));
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
    if (room_view_)
        room_view_->set_paginating(true);

    // Run the blocking SDK call off the UI thread; bounce the result back
    // via a queued connection. `client_` is thread-safe (Rust runtime
    // serialises concurrent calls).
    auto* c = client_; // snapshot: avoid account-switch race on client_
    if (!c)
    {
        state.in_flight = false;
        if (room_view_)
            room_view_->set_paginating(false);
        return;
    }
    run_async_(
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

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    refreshRoomList();
}

void MainWindow::on_join_room_outcome_ui_(bool ok, const std::string&)
{
    if (!ok && mainApp_ && mainApp_->room_preview())
        mainApp_->room_preview()->set_state(
            tesseract::views::RoomPreviewView::State::Idle);
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
        if (account_manager_.thumbnail_cache().contains(cache_key))
        {
            return;
        }
        QByteArray qb_av(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<int>(bytes.size()));
        QBuffer buf_av(&qb_av);
        buf_av.open(QIODevice::ReadOnly);
        QImageReader reader_av(&buf_av);
        reader_av.setAutoTransform(true);
        const QSize native_av = reader_av.size();
        if (native_av.isValid() &&
            (native_av.width() > kAvatarCacheSize ||
             native_av.height() > kAvatarCacheSize))
        {
            reader_av.setScaledSize(
                native_av.scaled(kAvatarCacheSize, kAvatarCacheSize,
                                 Qt::KeepAspectRatio));
        }
        QImage img = reader_av.read();
        if (img.isNull())
        {
            media_decode_failed_.insert(cache_key);
            return;
        }
        if (img.width() > kAvatarCacheSize || img.height() > kAvatarCacheSize)
        {
            img = img.scaled(kAvatarCacheSize, kAvatarCacheSize,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        account_manager_.thumbnail_cache().store(cache_key, tk::qt6::make_image(std::move(img)));
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
        notify_secondary_media_ready_(cache_key, kind);
        return;
    }

    if (kind == MediaKind::Tile)
    {
        if (account_manager_.image_cache().contains(cache_key))
        {
            return;
        }
        QImage img;
        if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                              static_cast<int>(bytes.size())))
        {
            return;
        }
        account_manager_.image_cache().store(cache_key, tk::qt6::make_image(std::move(img)));
        if (mainApp_)
        {
            mainApp_->room_view()->message_list()->invalidate_data();
        }
        if (mainAppSurface_)
        {
            mainAppSurface_->relayout();
            mainAppSurface_->update();
        }
        notify_secondary_media_ready_(cache_key, kind);
        return;
    }

    // MediaImage / MediaThumbnail — decode off the UI thread (QImageReader is
    // thread-safe), then store + repaint on the UI thread. Mirrors GTK4 and
    // ensure_picker_image_; pickers already use this pattern so all paths are
    // now consistent.
    const bool is_thumb = (kind == MediaKind::MediaThumbnail);
    if ((is_thumb ? account_manager_.thumbnail_cache() : account_manager_.image_cache()).contains(cache_key) ||
        account_manager_.anim_cache().has(cache_key))
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
    run_async_(
        [this, cache_key, kind, is_thumb, max_w, max_h,
         bytes = std::move(bytes)]() mutable
        {
            auto d = std::make_shared<DecodedImage>(
                decode_image_(bytes, max_w, max_h));
            post_to_ui_(
                [this, cache_key, kind, is_thumb, d]() mutable
                {
                    auto& still_cache =
                        is_thumb ? account_manager_.thumbnail_cache()
                                 : account_manager_.image_cache();
                    if (still_cache.contains(cache_key) ||
                        account_manager_.anim_cache().has(cache_key))
                        return;
                    if (!d->frames.empty())
                    {
                        account_manager_.anim_cache().store(
                                          cache_key, std::move(d->frames),
                                          std::move(d->delays_ms),
                                          QDateTime::currentMSecsSinceEpoch());
                        if (tk_anim_timer_ && !tk_anim_timer_->isActive())
                            tk_anim_timer_->start();
                    }
                    else if (d->still)
                    {
                        still_cache.store(cache_key, std::move(d->still));
                    }
                    else
                    {
                        media_decode_failed_.insert(cache_key);
                        return;
                    }
                    if (mainApp_)
                        mainApp_->room_view()->notify_image_ready(cache_key);
                    if (mainAppSurface_)
                    {
                        mainAppSurface_->relayout();
                        mainAppSurface_->update();
                    }
                    if (shortcode_popup_visible_() && shortcode_popup_surface_)
                        shortcode_popup_surface_->update();
                    notify_secondary_media_ready_(cache_key, kind);
                });
        });
}

void MainWindow::bind_settings_controller_()
{
    // settings_controller_ is freshly constructed by
    // ShellBase::ensure_settings_controller_(); bind it to the native widget.
    // The SettingsWidget installs its own key/file dialog hooks internally,
    // so there is no separate wire_key_dialog_callbacks_ step on Qt.
    if (settingsWidget_)
        settingsWidget_->set_controller(settings_controller_.get(),
                                        my_display_name_);
}

void MainWindow::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select image"), {},
        tr("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QByteArray data = f.readAll();
    const std::string mime =
        path.endsWith(".png",  Qt::CaseInsensitive) ? "image/png"  :
        path.endsWith(".gif",  Qt::CaseInsensitive) ? "image/gif"  :
        path.endsWith(".webp", Qt::CaseInsensitive) ? "image/webp" :
        "image/jpeg";
    std::vector<uint8_t> bytes(data.begin(), data.end());
    post_to_ui_([cb = std::move(cb), bytes = std::move(bytes), mime]() mutable
    {
        cb(std::move(bytes), mime);
    });
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

    // Pre-scale large images to avoid Qt's 256 MB allocation limit. A raw
    // Open Graph image can easily be 4K+ RGBA (≥33 MB) and some exceed 256 MB.
    // setScaledSize() makes the codec decode directly to the target size without
    // ever allocating the oversized intermediate buffer.
    const QSize native_size = reader.size();
    if (native_size.isValid() &&
        (native_size.width() > max_w || native_size.height() > max_h))
    {
        reader.setScaledSize(
            native_size.scaled(max_w, max_h, Qt::KeepAspectRatio));
    }

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
            // Safety clamp — no-op when setScaledSize already handled it.
            if (frame.width() > max_w || frame.height() > max_h)
            {
                frame = frame.scaled(max_w, max_h, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            d.frames.push_back(tk::qt6::make_image(std::move(frame)));
            d.delays_ms.push_back(delay);
        }
        if (!d.frames.empty())
        {
            return d;
        }
        d.delays_ms.clear();
    }
    // Still-image path: use a fresh reader so the animation-check state does
    // not interfere. setScaledSize is applied here too to guard against the
    // 256 MB limit on the still decode.
    {
        QBuffer still_buf(&qb);
        still_buf.open(QIODevice::ReadOnly);
        QImageReader still_reader(&still_buf);
        still_reader.setAutoTransform(true);
        const QSize native = still_reader.size();
        if (native.isValid() &&
            (native.width() > max_w || native.height() > max_h))
        {
            still_reader.setScaledSize(
                native.scaled(max_w, max_h, Qt::KeepAspectRatio));
        }
        QImage img = still_reader.read();
        if (img.isNull())
            return d;
        // Safety clamp if the format plugin ignored setScaledSize.
        if (img.width() > max_w || img.height() > max_h)
        {
            img = img.scaled(max_w, max_h, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        }
        d.still = tk::qt6::make_image(std::move(img));
    }
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

void MainWindow::extract_drop_media_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes,
                                     std::string mime,
                                     tesseract::views::ComposeBar* target,
                                     std::shared_ptr<bool> target_alive)
{
    if (mime == "image/gif" || mime == "image/webp")
    {
        // Detect animation on a bg thread, then post back to the UI thread.
        run_async_([this, pending_gen, target, target_alive,
                    bytes = std::move(bytes)]() mutable
        {
            QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<qsizetype>(bytes.size()));
            QBuffer buf;
            buf.setData(ba);
            buf.open(QIODevice::ReadOnly);
            QImageReader reader(&buf);
            tesseract::views::MediaInfo info;
            info.is_animated = reader.imageCount() > 1;
            info.pending_gen = pending_gen;
            QMetaObject::invokeMethod(
                this,
                [this, info, target, target_alive]() mutable
                { post_pending_attachment_(info, target, target_alive); },
                Qt::QueuedConnection);
        });
    }
    else if (mime.starts_with("video/"))
    {
        extract_drop_video_(pending_gen, std::move(bytes), target,
                            std::move(target_alive));
    }
    else if (mime.starts_with("audio/"))
    {
        extract_drop_audio_(pending_gen, std::move(bytes), target,
                            std::move(target_alive));
    }
}

void MainWindow::post_pending_attachment_(
    const tesseract::views::MediaInfo& info,
    tesseract::views::ComposeBar* target, std::shared_ptr<bool> alive)
{
    if (target)
    {
        // Pop-out window: post to its compose bar only while it lives.
        if (alive && *alive)
            target->update_pending_attachment(info);
    }
    else if (mainApp_)
    {
        mainApp_->room_view()->compose_bar()->update_pending_attachment(info);
    }
}

void MainWindow::extract_drop_video_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes,
                                     tesseract::views::ComposeBar* target,
                                     std::shared_ptr<bool> target_alive)
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
        [this, player, state, target, target_alive](const QVideoFrame& frame)
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
            post_pending_attachment_(state->info, target, target_alive);
        });
    QObject::connect(player,
        qOverload<QMediaPlayer::Error, const QString&>(&QMediaPlayer::errorOccurred),
        player,
        [this, player, state, target, target_alive](QMediaPlayer::Error,
                                                     const QString&)
        {
            if (state->done) return;
            state->done = true;
            player->deleteLater();
            post_pending_attachment_(state->info, target, target_alive);
        });
    player->play();
}

void MainWindow::extract_drop_audio_(std::uint32_t pending_gen,
                                     std::vector<std::uint8_t> bytes,
                                     tesseract::views::ComposeBar* target,
                                     std::shared_ptr<bool> target_alive)
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
        [this, player, pending_gen, done, target,
         target_alive](QMediaPlayer::MediaStatus status)
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
            post_pending_attachment_(info, target, target_alive);
        });
    QObject::connect(player,
        qOverload<QMediaPlayer::Error, const QString&>(&QMediaPlayer::errorOccurred),
        player,
        [this, player, pending_gen, done, target,
         target_alive](QMediaPlayer::Error, const QString&)
        {
            if (*done) return;
            *done = true;
            player->deleteLater();
            tesseract::views::MediaInfo info;
            info.pending_gen = pending_gen;
            post_pending_attachment_(info, target, target_alive);
        });
    player->play();
}

void MainWindow::generate_video_thumbnail_(const std::string& event_id,
                                           const std::string& video_url)
{
    const std::string src = video_url;
    run_async_(
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
                    if (account_manager_.image_cache().contains(key))
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
                            if (account_manager_.image_cache().contains(key))
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
    if (inflightDot_ && inflight_needs_anim_())
    {
        inflightDot_->update_phase(inflight_spin_phase_());
        inflightDot_->update();
    }
    if (mainAppSurface_)
    {
        mainAppSurface_->update_anim_regions();
    }
    if (gif_popup_surface_ && gif_popup_frame_ && gif_popup_frame_->isVisible())
    {
        gif_popup_surface_->update_anim_regions();
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
    if (account_manager_.image_cache().contains(key))
    {
        return;
    }
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), rgba.data(), rgba.size());
    account_manager_.image_cache().store(key, tk::qt6::make_image(std::move(img)));
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

    if (!mainApp_)
    {
        return;
    }
    // Avatars are fetched lazily as rows are painted (RoomListView's
    // on_room_avatar_needed), so collapsed / off-screen rooms aren't requested.
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
            mainApp_->room_list_view()->clear_space_unjoined_rooms();
            cancel_unjoined_summaries_();
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
            const auto& unjoined =
                get_cached_unjoined_summaries_(space_stack_.back());
            mainApp_->room_list_view()->set_space_unjoined_rooms(
                std::vector<tesseract::RoomSummary>(unjoined));
        }
        showRooms(filtered);
    }
}

void MainWindow::onSpaceBack()
{
    if (!space_stack_.empty())
        space_stack_.pop_back();
    if (mainApp_)
        mainApp_->hide_room_preview();
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

void MainWindow::refreshSyncStatus()
{
    // A hyperlinked status message (statusLinkLabel_) is superseded by any
    // sync-status update; hide it so it can't reappear when a temporary
    // showMessage() clears.
    if (statusLinkLabel_)
    {
        statusLinkLabel_->hide();
    }
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
    // Steady state: settle to "Connected" unless a persistent status override
    // (e.g., "Fetching older messages…" from in-room search) is still active.
    if (has_status_override_())
        return;
    sync_progress_shown_ = false;
    statusBar()->showMessage(tr("Connected"));
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
    QAction* addAct = menu->addAction(tr("Add Account\xe2\x80\xa6"));
    QAction* settingsAct = menu->addAction(tr("Settings\xe2\x80\xa6"));
    QAction* qrAct = server_info_.supports_qr_grant
                         ? menu->addAction(tr("Add device via QR\xe2\x80\xa6"))
                         : nullptr;
    QString logout_label =
        tr("Log Out %1")
            .arg(my_display_name_.empty()
                     ? QString::fromStdString(my_user_id_)
                     : QString::fromStdString(my_display_name_));
    QAction* logoutAct = menu->addAction(logout_label);
    menu->addSeparator();
    QAction* quitAct = menu->addAction(tr("Quit"));
    QObject::connect(menu, &QMenu::triggered, this,
                     [this, addAct, settingsAct, qrAct, logoutAct, quitAct](QAction* a)
                     {
                         if (a == addAct)
                         {
                             beginAddAccount();
                         }
                         else if (a == settingsAct)
                         {
                             openSettings();
                         }
                         else if (a == qrAct)
                         {
                             start_qr_grant_overlay();
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
        stats_settings_view_ = settingsWidget_->settings_view();

        connect(settingsWidget_, &SettingsWidget::settingsClosed, this,
                [this]
                {
                    stop_search_index_stats_poll_();
                    contentStack_->setCurrentWidget(mainAppSurface_);
                });
        connect(settingsWidget_, &SettingsWidget::logoutRequested, this,
                [this]
                {
                    contentStack_->setCurrentWidget(mainAppSurface_);
                    logoutActiveAccount();
                });
        connect(settingsWidget_, &SettingsWidget::resetIdentityRequested, this,
                [this]
                {
                    // The reset overlay lives on the main window — leave
                    // settings first, then start the reset flow.
                    contentStack_->setCurrentWidget(mainAppSurface_);
                    begin_crypto_identity_reset_();
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
        connect(settingsWidget_, &SettingsWidget::indexMessagesChanged, this,
                [this](bool enabled)
                {
                    handle_index_messages_toggle_(enabled);
                });
        connect(settingsWidget_, &SettingsWidget::mediaPreviewsChanged, this,
                [this](tesseract::Settings::MediaPreviews mode)
                {
                    apply_media_preview_config_(
                        mode, tesseract::Settings::instance().invite_avatars);
                });
        connect(settingsWidget_, &SettingsWidget::inviteAvatarsChanged, this,
                [this](bool enabled)
                {
                    apply_media_preview_config_(
                        tesseract::Settings::instance().media_previews, enabled);
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
                    clear_all_caches_(
                        [this](uint64_t local, uint64_t sdk, uint64_t memory,
                               uint64_t mh, uint64_t mm,
                               uint64_t dh, uint64_t dm)
                    {
                        if (settingsWidget_)
                            settingsWidget_->set_cache_sizes(local, sdk,
                                                             memory, mh, mm,
                                                             dh, dm);
                    });
                });

        connect(settingsWidget_, &SettingsWidget::localAvatarChanged, this,
                [this](const QString& new_mxc)
                {
                    my_avatar_url_ = new_mxc.toStdString();
                    if (active_account_)
                    {
                        active_account_->avatar_url = my_avatar_url_;
                    }
                    populateUserStrip();
                });

        connect(settingsWidget_, &SettingsWidget::profileFieldChanged, this,
                [this](const QString& key, const QString& value_json)
                {
                    handle_profile_field_change_(key.toStdString(),
                                                 value_json.toStdString());
                });

        // server_info_ may have already arrived before this lazy widget was
        // created — apply it now so capability gating is correct on first open.
        settingsWidget_->set_server_info(server_info_);
    }

    settingsWidget_->populate(
        my_display_name_, my_user_id_, my_avatar_url_,
        [this](const std::string& mxc) -> const tk::Image*
        { return account_manager_.thumbnail_cache().peek(mxc); },
        tesseract::Settings::instance().theme_pref,
        tesseract::Settings::instance().notifications_enabled);

    if (settings_controller_)
        settingsWidget_->set_controller(settings_controller_.get(),
                                        my_display_name_);

    // set_controller creates the NativeTextField overlays; push extended profile
    // after so set_text calls land on live fields.
    if (!own_extended_profile_.pronouns.empty() ||
        !own_extended_profile_.tz.empty() ||
        !own_extended_profile_.biography.empty())
        settingsWidget_->set_extended_profile(own_extended_profile_);

    // Refresh storage sizes each time settings opens.
    compute_cache_sizes_([this](uint64_t local, uint64_t sdk, uint64_t memory,
                                uint64_t mh, uint64_t mm,
                                uint64_t dh, uint64_t dm)
    {
        if (settingsWidget_)
            settingsWidget_->set_cache_sizes(local, sdk, memory, mh, mm, dh,
                                             dm);
    });

    contentStack_->setCurrentWidget(settingsWidget_);
    start_search_index_stats_poll_();
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
            if (slash_controller_)
                slash_controller_->hide();
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
            if (shortcode_controller_)
                shortcode_controller_->hide();
        }
    }
    if (mention_popup_visible_() && event->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(event);
        QPoint global = me->globalPosition().toPoint();
        if (!mention_popup_frame_->rect().contains(
                mention_popup_frame_->mapFromGlobal(global)))
        {
            if (mention_controller_)
                mention_controller_->hide();
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
    // Agnostic state machine lives in ShellBase; this shell only supplies the
    // native restart timer (post_to_ui_after_), status bar, user strip
    // (refresh_user_strip_) and relogin (request_relogin_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

void MainWindow::refresh_user_strip_()
{
    populateUserStrip();
}

void MainWindow::request_relogin_(const std::string& /*user_id*/)
{
    doLogin();
}

void MainWindow::handle_backup_progress_ui_(tesseract::BackupProgress progress)
{
    // Only the active account's backup state drives the status bar — but we
    // can't filter by user_id here since BackupProgress doesn't carry one. We
    // rely on EventHandlerBase's post_to_ui_ being per-instance: the active
    // client_ pointer check gates the update. Key-download progress is surfaced
    // by refreshSyncStatus() ("Downloading encryption keys (N)…").
    if (!client_)
    {
        return;
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

    auto sess = account_manager_.find(user_id);
    if (sess)
    {
        // Suppress only when the user is actively focused on this exact room.
        if (win_focused && active_account_ &&
            active_account_->user_id == user_id &&
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
            if (const auto* r = room_by_id_(t.room_id))
            {
                name = r->name;
                const std::string& av_mxc = r->effective_avatar_url();
                if (!av_mxc.empty())
                {
                    avatar = account_manager_.thumbnail_cache().peek(av_mxc);
                }
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
    on_inflight_ui_();
}

void MainWindow::on_inflight_ui_()
{
    const auto c  = inflight_dot_color_();
    const auto n  = inflight_total_();
    const auto fp = pool_pending_count_();
    const auto sp = mut_pool_pending_count_();
    const auto mp = pending_media_count_();
    inflightDot_->update_state(n, c);
    const QString first = (n == 1) ? tr("1 request in flight")
                                   : tr("%1 requests in flight").arg(n);
    QString tip =
        first +
        tr("\nmedia: %1 loading · fetch: %2 queued · send: %3 queued")
            .arg(mp).arg(fp).arg(sp);
#ifndef NDEBUG
    if (!last_inflight_urls_.empty()) {
        tip += QString("\n── requests ──\n");
        tip += QString::fromStdString(last_inflight_urls_);
    }
#endif
    inflightDot_->setToolTip(tip);
    // Force-refresh the tooltip window if it is currently shown over this widget.
    if (inflightDot_->underMouse())
        QToolTip::showText(QCursor::pos(), tip, inflightDot_);
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

void MainWindow::on_own_extended_profile_ready_ui_()
{
    if (settingsWidget_)
        settingsWidget_->set_extended_profile(own_extended_profile_);
}

void MainWindow::on_profile_field_result_ui_(const std::string& key,
                                              bool ok,
                                              const std::string& error)
{
    if (!settingsWidget_) return;
    settingsWidget_->set_profile_field_busy(key, false);
    if (!ok)
        settingsWidget_->set_profile_field_error(key, error);
}

void MainWindow::update_typing_bar_(const std::string& text, bool /*visible*/)
{
    if (mainApp_)
    {
        mainApp_->room_view()->set_typing_text(text);
    }
}

void MainWindow::on_show_status_message_ui_(const std::string& msg)
{
    const auto segs = parse_status_message_(msg); // opt-in gate (server text → plain)
    if (!tesseract::status_has_links(segs))
    {
        if (statusLinkLabel_)
        {
            statusLinkLabel_->hide();
        }
        statusBar()->showMessage(QString::fromStdString(msg));
        return;
    }

    // Hyperlinked message → rich-text label (showMessage cannot render
    // links). Created once; clearMessage() is required because a temporary
    // message would hide normal status-bar widgets.
    if (!statusLinkLabel_)
    {
        statusLinkLabel_ = new QLabel(this);
        statusLinkLabel_->setTextFormat(Qt::RichText);
        statusLinkLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
        statusLinkLabel_->setOpenExternalLinks(false);
        connect(statusLinkLabel_, &QLabel::linkActivated, this,
                [](const QString& url)
                { tesseract::Client::open_in_browser(url.toStdString()); });
        statusBar()->addWidget(statusLinkLabel_, 1);
    }
    QString html;
    for (const auto& seg : segs)
    {
        const QString text = QString::fromStdString(seg.text).toHtmlEscaped();
        if (seg.url.empty())
        {
            html += text;
        }
        else
        {
            html += QStringLiteral("<a href=\"%1\">%2</a>")
                        .arg(QString::fromStdString(seg.url).toHtmlEscaped(),
                             text);
        }
    }
    statusBar()->clearMessage();
    statusLinkLabel_->setText(html);
    statusLinkLabel_->show();
}

void MainWindow::on_restore_status_ui_()
{
    refreshSyncStatus();
}

// ---------------------------------------------------------------------------
// Multi-account orchestration
// ---------------------------------------------------------------------------

void MainWindow::switchActiveAccount(const std::string& user_id)
{
    // All platform-agnostic bookkeeping (unsubscribe previous room, clear
    // per-account state, swap active_account_ / aliases / identity, compute
    // pending restores, swap rooms_/invites_ snapshots, persist the index)
    // lives in ShellBase. Returns false (no-op) when the account isn't found
    // or is already active with a bound client.
    if (!switch_active_account_impl_(user_id))
    {
        return;
    }
    refresh_account_ui_after_switch_();
}

void MainWindow::refresh_account_ui_after_switch_()
{
    clearMessages();

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

    refreshRoomList();
    // Rooms already in cache — try to restore the tab session immediately.
    // on_rooms_updated_ handles the async case (no cache, rooms_ empty).
    if (!rooms_.empty() && !pending_restore_rooms_.empty())
    {
        if (try_restore_tab_session_(pending_restore_rooms_,
                                     pending_restore_rooms_[0]))
            pending_restore_rooms_.clear();
    }

    if (main_app_)
        main_app_->show_room();

    if (mainApp_)
    {
        mainApp_->show_verif_banner(false);
        mainAppSurface_->relayout();
    }

    rebuildAccountPicker();
    handle_verification_state_ui_(active_account_ && !active_account_->unverified);
}

void MainWindow::beginAddAccount()
{
    // Record the current active account's position in the accounts list so
    // Cancel can return to it. -1 means no active account.
    const auto& accs = account_manager_.accounts();
    add_account_return_idx_ = -1;
    if (active_account_)
    {
        for (int i = 0; i < static_cast<int>(accs.size()); ++i)
        {
            if (accs[i]->user_id == active_account_->user_id)
            {
                add_account_return_idx_ = i;
                break;
            }
        }
    }
    pending_login_is_add_account_ = true;

    // Create a fresh client for the OAuth round-trip. The user_id won't
    // be known until await_oauth completes, so we point the SDK at a
    // per-attempt "pending-<ts>" directory; onLoginSucceeded renames
    // it to accounts/<sanitized-uid>/ once the round-trip completes.
    pending_login_temp_dir_.clear();
    pending_login_client_ = std::make_unique<tesseract::Client>();
    loginView_->set_client(pending_login_client_.get());
    loginView_->set_on_begin_oauth([this] { arm_pending_login_(); });
    loginView_->set_mode(tesseract::views::LoginView::Mode::AddAccount);
    loginView_->reset();
    contentStack_->setCurrentWidget(loginView_);
    statusBar()->showMessage(tr("Add Account"));
}

void MainWindow::logoutActiveAccount()
{
    // Platform-agnostic teardown (unsubscribe the room, up_connector/presence
    // logout, client_->logout() + failure surface, stop_sync, clear account
    // state, tray refresh, index update, and — when other accounts remain — the
    // switch to a survivor) lives in ShellBase.
    const auto result = logout_active_account_impl_();
    if (!result.logged_out)
    {
        return;
    }

    // Native widget cleanup of the now-empty surface (the remaining-account
    // branch already repainted via refresh_account_ui_after_switch_).
    if (!result.has_remaining)
    {
        refreshRoomList();
        clearMessages();
        if (mainApp_)
        {
            mainApp_->clear_content();
            mainAppSurface_->relayout();
        }
    }
    verification_banner_dismissed_ = false;

    if (!result.has_remaining)
    {
        // No accounts left → back to initial login.
        loginView_->set_mode(tesseract::views::LoginView::Mode::Initial);
        pending_login_is_add_account_ = false;
        add_account_return_idx_ = -1;
        pending_login_temp_dir_.clear();
        pending_login_client_ = std::make_unique<tesseract::Client>();
        loginView_->set_client(pending_login_client_.get());
        loginView_->set_on_begin_oauth([this] { arm_pending_login_(); });
        loginView_->reset();
        contentStack_->setCurrentWidget(loginView_);
        statusBar()->showMessage(tr("Signed out"), 3000);
        rebuildAccountPicker();
        return;
    }

    statusBar()->showMessage(
        tr("Signed out of %1").arg(QString::fromStdString(result.logged_out_uid)),
        3000);
}

void MainWindow::rebuildAccountPicker()
{
    if (!accountPicker_)
    {
        return;
    }
    std::vector<tesseract::views::AccountEntry> entries;
    const auto& accs = account_manager_.accounts();
    entries.reserve(accs.size());
    for (const auto& sp : accs)
    {
        const auto& a = *sp;
        const bool is_active = active_account_ && a.user_id == active_account_->user_id;
        entries.push_back({
            a.user_id,
            a.display_name,
            a.avatar_url,
            is_active,
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
        .arg(hex(p.sidebar_bg), hex(p.popup_border));
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
            new tk::qt6::Surface(current_theme_, accountPickerPopover_);
        auto picker_owner = std::make_unique<tesseract::views::AccountPicker>();
        accountPicker_ = picker_owner.get();
        accountPicker_->set_image_provider(make_avatar_image_provider_());
        accountPicker_->on_select = [this](const std::string& uid)
        {
            if (accountPickerPopover_)
            {
                accountPickerPopover_->hide();
            }
            on_account_picker_select_(uid);
        };
        accountPickerSurface_->set_root(std::move(picker_owner));
        lay->addWidget(accountPickerSurface_);
    }
    rebuildAccountPicker();

    constexpr int kPickerWidth = 260;
    constexpr int kRowHeight = 56;
    const int rows = static_cast<int>(account_manager_.accounts().size());
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
    if (account_manager_.find(user_id))
    {
        switchActiveAccount(user_id);
        contentStack_->setCurrentWidget(mainAppSurface_);
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
    // Only prompt when there is actually an identity to verify against. On a
    // fresh/only device our own login-time bootstrap holds the cross-signing
    // keys, so "verify this device" is a dead end — check_encryption_setup_
    // drives the Fresh setup overlay instead.
    if (!foreign_cross_signing_identity_())
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
        mainApp_->show_verif_banner(true);
        mainAppSurface_->relayout();
    }
}

void MainWindow::show_encryption_setup_overlay_(
    tesseract::views::EncryptionSetupOverlay::Mode mode)
{
    if (!mainApp_) return;
    auto* ov = mainApp_->encryption_setup();
    if (!ov) return;

    // Reconfigure the overlay (clears prior callbacks) before re-creating the
    // native fields, then wire the shared callbacks via ShellBase.
    ov->reset(mode);

    encPassphraseField_ = mainAppSurface_->host().make_text_field();
    encPassphraseField_->set_password(true);
    encKeyField_ = mainAppSurface_->host().make_text_field();
    encKeyField_->set_password(false);

    wire_encryption_setup_callbacks_(*ov, mainAppSurface_->host(),
                                     encPassphraseField_.get(),
                                     encKeyField_.get());

    mainApp_->show_encryption_setup(true);
    mainAppSurface_->relayout();
}

void MainWindow::show_qr_grant_overlay_()
{
    if (!mainApp_) return;
    auto* view = mainApp_->qr_grant_view();
    if (!view) return;
    qrCheckCodeField_ = mainAppSurface_->host().make_text_field();
    qrCheckCodeField_->set_on_changed([view](const std::string& t) {
        view->set_check_code_text(t);
    });
    qrCheckCodeField_->set_visible(false);
}

void MainWindow::hide_qr_grant_overlay_()
{
    qrCheckCodeField_.reset();
}

void MainWindow::open_join_room_dialog_ui_(const std::string& prefill)
{
    if (joinRoomDialog_)
        joinRoomDialog_->openDialogWithPrefill(prefill);
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

void MainWindow::show_slash_popup_(tk::Rect cursor_local, int rows)
{
    // The widget + controller are created eagerly in the constructor; the
    // controller has already set the suggestions. This positions the popup at
    // the caret, clamped to the window, and installs the outside-click filter.
    if (!slash_popup_frame_ || !slash_popup_surface_ || !mainAppSurface_)
    {
        return;
    }
    int h = int(rows * tesseract::views::SlashCommandPopup::kRowHeight);
    int w = int(tesseract::views::SlashCommandPopup::kWidth);
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
}

void MainWindow::show_gif_popup_()
{
    if (!gif_popup_frame_ || !gif_popup_widget_ || !roomTextArea_ ||
        !mainAppSurface_ || !gif_popup_surface_)
    {
        return;
    }
    // Full-width strip spanning the compose bar, floating just above it (like
    // the attachment preview band). content_size() drives only the height and
    // the empty/status check; the width comes from the compose bar.
    const tk::Rect cb = room_view_ ? room_view_->compose_bar_rect() : tk::Rect{};
    const tk::Size sz = gif_popup_widget_->content_size(cb.w);
    if (cb.w <= 0.0f || sz.h <= 0.0f)
    {
        hide_gif_popup_();
        return;
    }
    int w = std::max(1, int(cb.w));
    int h = std::max(1, int(sz.h));

    QPoint tl = mainAppSurface_->mapTo(this, QPoint(int(cb.x), int(cb.y)));
    int x = tl.x();
    int y = tl.y() - h - 4; // bottom edge sits just above the compose bar top

    gif_popup_frame_->setGeometry(x, y, w, h);
    gif_popup_surface_->resize(w, h);
    gif_popup_frame_->show();
    gif_popup_frame_->raise();
    gif_popup_surface_->relayout();
}

void MainWindow::hide_gif_popup_()
{
    if (gif_popup_frame_)
    {
        gif_popup_frame_->hide();
    }
}

const tk::Image*
MainWindow::gif_strip_image_(const tesseract::GifResult& result,
                             const std::function<void()>& repaint)
{
    // Shared with every pop-out's GIF strip (RoomWindowBase::shell_gif_strip_image_
    // → here). The pop-out passes a repaint that refreshes its own popup surface.
    return gif_strip_provider_ ? gif_strip_provider_(result, repaint) : nullptr;
}

void MainWindow::handle_gif_results_ui_(std::uint64_t request_id,
                                        std::vector<tesseract::GifResult> results)
{
    if (gif_controller_)
    {
        gif_controller_->on_results(request_id, std::move(results));
    }
}

void MainWindow::handle_gif_search_failed_ui_(std::uint64_t request_id,
                                              std::string message)
{
    if (gif_controller_)
    {
        gif_controller_->on_search_failed(request_id, std::move(message));
    }
}

// ── Shortcode popup ─────────────────────────────────────────────────────────

void MainWindow::show_shortcode_popup_(tk::Rect cursor_local, int rows)
{
    if (!shortcode_popup_frame_ || !shortcode_popup_surface_ ||
        !mainAppSurface_)
    {
        return;
    }
    int h = int(rows * tesseract::views::ShortcodePopup::kRowHeight);
    int w = int(tesseract::views::ShortcodePopup::kWidth);
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
}

// ── @mention popup ─────────────────────────────────────────────────────────

void MainWindow::show_mention_popup_(tk::Rect cursor_local, int rows)
{
    if (!mention_popup_frame_ || !mention_popup_surface_ || !mainAppSurface_)
    {
        return;
    }
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

void MainWindow::raise_and_activate_()
{
    raise();
    activateWindow();
}

void MainWindow::rebuild_tray_()
{
    if (!tray_ || !tray_->is_available())
        return;

    auto items = build_tray_items_();
    tray_->rebuild_menu(std::move(items));
}

bool MainWindow::is_ctrl_held_() const
{
    return QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
}

void MainWindow::switch_active_account_(const std::string& user_id)
{
    switchActiveAccount(user_id);
}

void MainWindow::spawn_main_window_(
    std::shared_ptr<tesseract::AccountSession> account)
{
    auto* win = new qt6::MainWindow(account_manager_);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->set_initial_account(account);
    // Shared hand-off: re-point the account's bridge at the new window, seed its
    // caches for instant paint, mark it pinned, and register it as dedicated.
    // Runs before the new window's deferred (queued) doLogin().
    hand_account_to_spawned_window_(win, account);
    win->show();
    win->raise();
    win->activateWindow();
}

} // namespace qt6
