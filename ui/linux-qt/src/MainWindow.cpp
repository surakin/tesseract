#include "MainWindow.h"
#include "AuxWindow.h"
#include "CallWindow.h"
#include "LinuxNotifier.h"
#include "LinuxUpConnectorQt.h"
#include "views/ComposePopups.h"
#include <QDesktopServices>
#include <QUrl>
#include "views/BrandView.h"
#include "views/media_drop.h"
#include "views/shortcut_registry.h"
#include "platform/linux/linux_autostart.h"
#include "LinuxPowerMonitorQt.h"
#include "LinuxScreenLockQt.h"
#include "app/DeferredTeardown.h"
#include "views/SlashCommands.h"
#include "app/status_links.h"
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
#include "app/ScreenshotFixture.h"
#endif

#include "tk/canvas_qpainter.h"
#include "tk/i18n.h"
#include "app/jxl_decode.h"
#include "tk/image_sniff.h"
#include "tk/status_icons.h"
#include "tk/svg.h"
#include "tk/theme.h"
#include "platform/video_decode.h"

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
#include <tesseract/visual.h>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QMetaType>
#include <QApplication>
#include <QProcess>
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
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
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
#include <QDir>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <unordered_set>

Q_DECLARE_METATYPE(std::vector<tesseract::RoomInfo>)
Q_DECLARE_METATYPE(tesseract::BackupProgress)

namespace qt6
{

namespace
{

// A registry chord as a Qt key sequence; empty for keys Qt can't bind here.
// ModPrimary is Ctrl: the Qt6 shell only ships on Linux.
QKeySequence to_qt_sequence_(const tk::KeyChord& chord)
{
    int key = 0;
    switch (chord.key)
    {
    case tk::Key::Character:
        // Qt's key code for a printable ASCII character is the character
        // itself, with letters in upper case (Qt::Key_K == 'K').
        if (chord.text.size() == 1)
            key = std::toupper(static_cast<unsigned char>(chord.text.front()));
        break;
    case tk::Key::Tab: key = Qt::Key_Tab; break;
    case tk::Key::Left: key = Qt::Key_Left; break;
    case tk::Key::Right: key = Qt::Key_Right; break;
    case tk::Key::F1: key = Qt::Key_F1; break;
    default: break;
    }
    if (key == 0)
        return {};
    int mods = 0;
    if (chord.mods & (tk::ModPrimary | tk::ModCtrl))
        mods |= Qt::CTRL;
    if (chord.mods & tk::ModShift)
        mods |= Qt::SHIFT;
    if (chord.mods & tk::ModAlt)
        mods |= Qt::ALT;
    if (chord.mods & tk::ModMeta)
        mods |= Qt::META;
    return QKeySequence(mods | key);
}

} // namespace

// EventBridge is now a thin QObject wrapper around EventHandlerBase.
// All IEventHandler method bodies live in EventHandlerBase (shared/app/).
// No EventBridge:: method definitions needed here.

// ---------------------------------------------------------------------------
// MainWindow constructor
// ---------------------------------------------------------------------------

MainWindow::MainWindow(tesseract::AccountManager& account_manager,
                       QWidget* parent, bool start_hidden
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
                       , bool screenshot_mode
#endif
                       )
    : QMainWindow(parent)
    , ShellBase(account_manager)
    , start_hidden_(start_hidden)
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    , screenshot_mode_(screenshot_mode)
#endif
{
    qRegisterMetaType<std::vector<tesseract::RoomInfo>>();
    qRegisterMetaType<tesseract::BackupProgress>();

    set_screen_lock_(std::make_unique<LinuxScreenLockQt>());
    set_power_monitor_(std::make_unique<LinuxPowerMonitorQt>());
    set_autostart_(std::make_unique<LinuxAutostart>("tesseract-matrix"));

    setWindowTitle("Tesseract");
    setMinimumSize(static_cast<int>(tesseract::visual::kMinWindowWidth),
                    static_cast<int>(tesseract::visual::kMinWindowHeight));
    resize(1100, 768);

    contentStack_ = new QStackedWidget(this);
    setCentralWidget(contentStack_);

    brandingSurface_ = new tk::qt6::Surface(tk::Theme::light(), contentStack_);
    // create_root_widget (not plain make_unique) so BrandView::host() is
    // valid from construction — Surface::set_root()/RootWidget::add_child()
    // adopt an already-built widget without ever fixing up its host_, so a
    // plain make_unique here leaves host_ permanently null and BrandView's
    // self-scheduling post_delayed() wireframe animation silently never
    // arms itself (confirmed: it depends entirely on incidental repaints
    // from elsewhere, freezing solid whenever nothing else invalidates it —
    // e.g. throughout the whole account-restore window).
    auto branding_owner =
        tk::create_root_widget<tesseract::views::BrandView>(&brandingSurface_->host());
    brandingView_ = branding_owner.get();
    brandingSurface_->set_root(std::move(branding_owner));
    contentStack_->addWidget(brandingSurface_);

    // The login page is created lazily by ShellBase::ensure_login_view_
    // (through create_login_surface_ui_) before each show.

    // Single surface hosting the MainAppWidget tree (sidebar + chat + overlays).
    mainAppSurface_ = new tk::qt6::Surface(tk::Theme::light(), contentStack_);
    contentStack_->addWidget(mainAppSurface_);

    wire_main_surface_(static_cast<float>(mainAppSurface_->devicePixelRatioF()));

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
        auto main_app_owner = create_main_app_();

        create_main_room_pane_();

        // ---- Provider wiring (avatar/image/sticker/preview/user-info) ----
        wire_main_app_widget_(main_app_);

        // ---- User info strip ----
        // The mouse right-click path is the surface-level region check in
        // customContextMenuRequested below; this is the keyboard route
        // (context-menu key / Enter with one account).
        main_app_->user_info()->on_secondary = [this](tk::Point world)
        {
            onUserStripContextMenu(mainAppSurface_->mapToGlobal(
                QPoint(static_cast<int>(world.x), static_cast<int>(world.y))));
        };
        main_app_->user_info()->on_primary = [this](tk::Point world)
        {
            open_account_picker_();
        };

        // Mention-pill avatar provider + on_fetch_room_members (which
        // populates the cache it reads) already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_), using
        // RoomPane's own cached_room_members_/cached_members_room_ instead
        // of this window's now-removed copy of those fields.
        wire_room_view_services_();

        // Drop-into-compose-bar wiring for RoomView::on_file_drop already
        // provided by main_room_pane_->attach() above (RoomPane::wire_room_view_),
        // equivalent to this block's old body — see RoomPane.cpp.

        setup_link_clicked_(main_app_->room_view());
        main_app_->room_view()->on_show_copy_menu = [this]()
        {
            auto* rv = main_app_->room_view();
            auto* menu = new QMenu(mainAppSurface_);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->setStyleSheet(tk::qt6::build_menu_qss(current_theme_));
            QAction* copyAct = menu->addAction(QString::fromStdString(rv->copy_menu_label()));
            QObject::connect(copyAct, &QAction::triggered, [rv]()
            {
                rv->copy_context_menu_target();
            });
            menu->popup(QCursor::pos());
        };
        {
            QPointer<tk::qt6::Surface> sfp = mainAppSurface_;
            main_app_->room_view()->on_link_hovered =
                [sfp](const std::string& url)
            {
                if (sfp)
                {
                    using HC = tesseract::views::MessageListView::HoverCursor;
                    const HC hc = tesseract::views::MessageListView::hover_cursor_for(url);
                    sfp->setCursor(hc == HC::Text      ? Qt::IBeamCursor
                                   : hc == HC::Pointer ? Qt::PointingHandCursor
                                                       : Qt::ArrowCursor);
                }
            };
            main_app_->space_root()->on_link_hovered =
                [sfp](const std::string& url)
            {
                if (sfp)
                {
                    sfp->setCursor(url.empty() ? Qt::ArrowCursor
                                               : Qt::PointingHandCursor);
                }
            };
            main_app_->on_sidebar_cursor =
                [sfp](tesseract::views::MainAppWidget::SidebarCursor c)
            {
                if (!sfp) return;
                using SC = tesseract::views::MainAppWidget::SidebarCursor;
                sfp->setCursor(c == SC::Resize   ? Qt::SplitHCursor
                               : c == SC::Toggle ? Qt::PointingHandCursor
                                                 : Qt::ArrowCursor);
            };
        }
        // on_video_clicked (both room_view()'s and room_media_view()'s
        // gallery-reuse alias) already provided by main_room_pane_->attach()
        // above (RoomPane::wire_room_view_, which aliases
        // room_media_view()->on_image_clicked/on_video_clicked to the same
        // handlers it installs on room_view()).

        // on_fetch_room_members / on_save_topic / on_leave_room /
        // on_room_settings_opened / on_room_settings_avatar_upload_requested
        // / room_settings_view()->on_accept already provided by
        // main_room_pane_->attach() above (RoomPane::wire_room_view_ +
        // Deps.on_left_room, constructed above). on_save_topic now reports
        // failures via an async status message (RoomPane::wire_room_view_'s
        // on_save_topic), matching the send_reply_/send_edit_ fix.
        // on_ignore_user already provided by main_room_pane_->attach() above
        // (RoomPane::wire_room_view_).

        mainAppSurface_->set_root(std::move(main_app_owner));
    }

    // The composer's popups (@mention, /command, :shortcode:, /gif) and the text
    // area's change/submit/navigation wiring are the shared ComposerPopups.
    create_compose_popups_(mainAppSurface_->host());

    // The topic field and the new-pack-name/shortcode/rename fields are all
    // self-owned by each RoomSettingsView instance (room_view_'s and the
    // space-root's, independently) — see RoomSettingsView::name_field()/
    // topic_field() and ImagePackEditorView::new_pack_name_field()/
    // shortcode_field()/pack_name_field()/paste_catcher() — so no shell-side
    // wiring is needed for any of them.

    // The room-list search field is wired in ShellBase::wire_main_app_widget_()
    // (shared across all four shells).

    // Global shortcuts (Ctrl+K, Ctrl+F, Ctrl+Tab, Alt+Left, Ctrl+/, ...),
    // bound from the shared registry. A QShortcut fires even while the
    // native compose / search QLineEdit/QTextEdit holds focus —
    // keyPressEvent on the window does not see keys a focused child widget
    // consumes. Each forwards the chord's KeyEvent into the widget tree, and
    // MainAppWidget decides what it does. Application scope fires from any
    // window; Window scope (room info, settings, shortcuts list) acts on this
    // window's room, so a pop-out room window must not trigger it. Ctrl+Tab's
    // commit-on-Ctrl-release is handled by host_qt.cpp's app-wide event
    // filter instead — QShortcut only fires on key-down/repeat.
    for (const auto& def : tesseract::views::shortcuts())
    {
        if (def.scope == tesseract::views::ShortcutScope::Contextual)
            continue;
        for (const auto& chord : def.chords)
        {
            const QKeySequence seq = to_qt_sequence_(chord);
            if (seq.isEmpty())
                continue;
            auto* sc = new QShortcut(seq, this);
            sc->setContext(def.scope == tesseract::views::ShortcutScope::Application
                               ? Qt::ApplicationShortcut
                               : Qt::WindowShortcut);
            connect(sc, &QShortcut::activated, this,
                    [this, event = tk::to_key_event(chord)]
                    {
                        if (main_app_)
                            main_app_->dispatch_key_down(event);
                    });
        }
    }


    // Right-click: user-strip actions (lower-left corner) or sticker save (chat).
    mainAppSurface_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        mainAppSurface_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& pos)
        {
            const bool in_user_strip =
                main_app_ && main_app_->user_info() &&
                [&]
            {
                const auto& r = main_app_->user_info()->bounds();
                return pos.x() >= r.x && pos.x() < r.right() &&
                       pos.y() >= r.y && pos.y() < r.bottom();
            }();
            if (in_user_strip)
            {
                onUserStripContextMenu(mainAppSurface_->mapToGlobal(pos));
                return;
            }
            if (!main_app_ || main_app_->room_view()->is_overlay_open())
            {
                return;
            }
            auto hit =
                main_app_->room_view()->message_list()->sticker_hit_at(tk::Point{
                    static_cast<float>(pos.x()), static_cast<float>(pos.y())});
            if (!hit)
            {
                return;
            }
            const auto mxc_url = hit->source ? hit->source->mxc_url() : std::string{};
            const auto body = hit->body;
            const auto info_json = hit->info_json;
            const bool already_saved =
                client_ && client_->user_pack_has_sticker(mxc_url, info_json);
            auto* menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->setStyleSheet(tk::qt6::build_menu_qss(current_theme_));
            QAction* add =
                menu->addAction(already_saved ? QString::fromStdString(tk::tr("Already in Saved Stickers"))
                                              : QString::fromStdString(tk::tr("Add to Saved Stickers")));
            add->setEnabled(!already_saved);
            if (!already_saved)
            {
                connect(add, &QAction::triggered, this,
                        [this, mxc_url, body, info_json]
                        {
                            if (!client_)
                                return;
                            // The save is a network and account-data write, so it
                            // runs on the mutable pool; the status line is set back
                            // on the UI thread.
                            // The session is held so a switch or sign-out mid-save
                            // cannot free the client under the worker.
                            auto sess = active_account_;
                            if (!sess || !sess->client)
                                return;
                            run_async_mut_(
                                [this, sess, mxc_url, body, info_json]()
                                {
                                    auto res = sess->client->save_sticker_to_user_pack(
                                        body, body, mxc_url, info_json);
                                    if (res.ok)
                                        return;
                                    show_status_message_(res.message, 6000);
                                });
                        });
            }
            menu->popup(mainAppSurface_->mapToGlobal(pos));
        });

    refresh_sync_status_(); // "Not logged in" until a session restores
    // Generic status-bar click (distinct from statusLinkLabel_'s
    // http(s)-only QLabel::linkActivated handling) — see eventFilter()'s
    // QEvent::MouseButtonRelease branch. Safe unconditionally:
    // trigger_persistent_status_click_() no-ops unless something
    // (currently: an in-progress history export) claimed the slot.
    statusBar()->installEventFilter(this);
    lowPowerLabel_ = new QLabel(this);
    lowPowerLabel_->setContentsMargins(0, 0, 4, 0);
    lowPowerLabel_->setToolTip(QString::fromStdString(
        tk::tr("Low power mode — background sync paused")));
    lowPowerLabel_->setVisible(false);
    statusBar()->addPermanentWidget(lowPowerLabel_);
    refreshLowPowerIcon_();

    inflightDot_ = new InflightDotWidget(this);
    inflightDot_->setContentsMargins(0, 0, 2, 0);
    statusBar()->addPermanentWidget(inflightDot_);
    init_pool_callbacks_();
    on_inflight_ui_();
    on_low_power_mode_ui_(low_power_active());

    read_portal_color_scheme_();
    read_portal_accent_color_();
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

    tk_inflight_timer_ = new QTimer(this);
    tk_inflight_timer_->setInterval(16);
    connect(tk_inflight_timer_, &QTimer::timeout, this,
            &MainWindow::onInflightTick_);

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (!screenshot_mode_)
#endif
        QMetaObject::invokeMethod(this, &MainWindow::doLogin,
                                  Qt::QueuedConnection);
    setupActivationListener_();

    account_manager_.register_window(this);
    broadcast_rebuild_tray_();
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
namespace
{

class QtScreenshotHost final : public tesseract::screenshot::ScreenshotHost
{
public:
    QtScreenshotHost(MainWindow& window, QString dir)
        : window_(window), dir_(std::move(dir))
    {
    }

    void apply_theme(tesseract::screenshot::ScreenshotTheme theme) override
    {
        window_.apply_screenshot_theme_(
            theme == tesseract::screenshot::ScreenshotTheme::Dark);
    }
    void refresh() override { window_.refresh_for_screenshot_(); }
    bool save_png(const std::string& filename) override
    {
        const QString path =
            QDir(dir_).filePath(QString::fromStdString(filename));
        if (window_.grab().save(path, "PNG"))
            return true;
        qCritical("Could not save screenshot: %s", qPrintable(path));
        return false;
    }
    void run_after(int ms, std::function<void()> fn) override
    {
        QTimer::singleShot(ms, &window_, std::move(fn));
    }
    void finish(bool ok) override { qApp->exit(ok ? 0 : 1); }

private:
    MainWindow& window_;
    QString dir_;
};

} // namespace

void MainWindow::apply_screenshot_theme_(bool dark)
{
    apply_theme_ui_(dark ? tk::Theme::dark() : tk::Theme::light());
}

void MainWindow::refresh_for_screenshot_()
{
    mainAppSurface_->relayout();
    mainAppSurface_->update();
    repaint();
}

void MainWindow::captureScreenshots(const std::string& output_dir)
{
    // Runs before app.exec(): qApp->exit() is a no-op without a running
    // event loop, so setup failures exit directly.
    if (!seed_screenshot_fixture_(mainAppSurface_->factory()))
    {
        qCritical("Could not load screenshot assets");
        std::exit(EXIT_FAILURE);
    }
    refresh_user_strip_();

    show_main_content_();
    resize(1100, 768);

    const QString dir = QString::fromStdString(output_dir);
    if (!QDir().mkpath(dir))
    {
        qCritical("Could not create screenshot directory: %s",
                  qPrintable(dir));
        std::exit(EXIT_FAILURE);
    }

    screenshot_host_ = std::make_unique<QtScreenshotHost>(*this, dir);
    start_screenshot_director_(*screenshot_host_, "qt6");
}
#endif

MainWindow::~MainWindow()
{
    release_popups_();
    release_settings_surface_();
    // unregister_window is called in closeEvent (before quit) so it is not
    // repeated here; broadcast_rebuild_tray_ is still needed to refresh any
    // remaining windows' tray menus after this window's C++ resources are freed.
    broadcast_rebuild_tray_();

    // Send this window's deferred "New messages" moves before the clients
    // stop (request_stop() would cancel them).
    flush_pending_fully_read_now_();

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
        // Hang up an active call while the SDK can still send the leave. Once
        // request_stop() and stop_sync() run, the call would only end when the
        // session is destroyed, and the others would still see us in it.
        end_call(true);
        // Signal every account first, in its own pass, before any stop_sync()
        // call below: request_stop() only needs a shared FFI lock, so it wins
        // the race against a concurrent send_message/subscribe_room on that
        // (or any other) account instead of queuing behind stop_sync()'s
        // exclusive lock — see Client::request_stop().
        for (auto& a : account_manager_.accounts())
        {
            if (a && a->client)
                a->client->request_stop();
        }
        for (auto& a : account_manager_.accounts())
        {
            if (a && a->client)
                a->client->stop_sync();
        }
    }
    if (pending_login_client_)
    {
        pending_login_client_->request_stop();
        pending_login_client_->stop_sync();
    }
    pool_.drain();
    mut_pool_.drain();
    media_prefetch_pool_.drain();

    client_ = nullptr;
    event_handler_ = nullptr;

    // Release the login page before the AccountSessions (and thus the Clients
    // it is bound to) and pending_login_client_ are destroyed.
    release_login_surface_();

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

void MainWindow::setupActivationListener_()
{
    // Protocol: see tk::ActivationRequest (ui/shared/platform/single_instance.h).
    // Shared with GTK4 — either backend's "losing" launch can forward here
    // regardless of which one won the lock.
    activationListener_ = std::make_unique<tk::ActivationListener>(
        [this](tk::ActivationRequest req)
        {
            activateWindowWithToken_(QString::fromStdString(req.token));
            if (!req.uri.empty())
            {
                openMatrixLink(req.uri);
            }
            dispatch_launch_action_(
                tesseract::launch_action_from_option_id(req.action),
                std::move(req.room_id));
        });
    if (activationListener_->fd() >= 0)
    {
        activationNotifier_ = new QSocketNotifier(
            activationListener_->fd(), QSocketNotifier::Read, this);
        connect(activationNotifier_, &QSocketNotifier::activated, this,
                [this]() { activationListener_->on_readable(); });
    }

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
            // A tray-icon click on Plasma/Wayland arrives with a granted,
            // compositor-issued activation token that Qt's StatusNotifierItem
            // adaptor has already placed in XDG_ACTIVATION_TOKEN (it handles
            // the ProvideXdgActivationToken D-Bus call Plasma makes just before
            // Activate). Prefer that real token over a self-issued one: we have
            // no fresh input serial of our own here (the click went to
            // plasmashell, not to us), so the self-issue path below is denied
            // by focus-stealing prevention. Consume it once.
            QByteArray env_tok = qgetenv("XDG_ACTIVATION_TOKEN");
            if (!env_tok.isEmpty())
            {
                qunsetenv("XDG_ACTIVATION_TOKEN");
                token = env_tok.toStdString();
            }
        }

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

void MainWindow::focus_forward_picker_field_()
{
    if (!main_app_ || !main_app_->forward_picker())
        return;
    if (auto* f = main_app_->forward_picker()->search_field())
    {
        f->set_text("");
        f->set_focused(true);
    }
}

void MainWindow::hide_forward_picker_field_()
{
    if (main_app_ && main_app_->forward_picker())
        if (auto* f = main_app_->forward_picker()->search_field())
            f->set_visible(false);
}

void MainWindow::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape && handle_escape_key_())
    {
        ev->accept();
        return;
    }
    // Ctrl+K is handled by an application-scoped QShortcut (see ctor) so it
    // works while a native text widget has focus; no keyPressEvent branch.
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

    // A popup's screen position is captured once when it opens and never
    // recomputed — leaving one open across a resize would strand it away
    // from whatever control anchored it. Close everything instead.
    if (mainAppSurface_) mainAppSurface_->host().dismiss_active_popup();
    if (room_view_) room_view_->dismiss_popups();
    hide_compose_popups_();
    close_account_picker_();
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
    else if (ev->type() == QEvent::WindowStateChange)
    {
        if (!isMinimized() && isVisible())
            start_anim_tick_();
        update_video_playback_suspension_();
    }
    else if (ev->type() == QEvent::ApplicationPaletteChange)
    {
        // Live-updates the QPalette::Highlight fallback path in
        // os_accent_color_() when the portal doesn't expose accent-color
        // (e.g. a style/platform-theme change with no portal). No-ops
        // unless the portal accent is actually unset, via
        // on_system_accent_changed_()'s equality check.
        on_system_accent_changed_();
    }
}

void MainWindow::showEvent(QShowEvent* ev)
{
    QMainWindow::showEvent(ev);
    start_anim_tick_();
    update_video_playback_suspension_();
}

void MainWindow::hideEvent(QHideEvent* ev)
{
    QMainWindow::hideEvent(ev);
    update_video_playback_suspension_();
}

// ---------------------------------------------------------------------------

void MainWindow::close_app_settings_ui_()
{
    if (!settings_surface_ || contentStack_->currentWidget() != settings_native_())
        return;
    stop_search_index_stats_poll_();
    leaveSettings_();
}

void MainWindow::rebind_login_client_ui_()
{
    if (!login_view_)
        return;
    login_view_->set_client(reset_pending_login_client_());
    // Swap only. Settings is open on top of the login view, and reset() would
    // refocus its homeserver field mid-typing; the homeserver check is re-run
    // by leaving Settings (take_login_recheck_()).
}

void MainWindow::leaveSettings_()
{
    // Opened from the login screen: go back to it (the view is still alive
    // in the stack) instead of the main content, which would tear it down.
    if (take_settings_from_login_() && login_view_)
    {
        contentStack_->setCurrentWidget(login_native_());
        // A proxy change made in Settings replaced the client: re-run the
        // homeserver check against it (a failed lookup made before the proxy
        // was fixed would otherwise stay on screen).
        if (take_login_recheck_())
            login_view_->reset();
    }
    else
        show_main_content_();
    set_app_settings_open_(false);
    teardown_settings_view_();
}

void MainWindow::show_main_content_()
{
    contentStack_->setCurrentWidget(mainAppSurface_);
    teardown_login_view_();
    mark_main_content_ready_();
}

void MainWindow::bring_main_window_forward_for_overlay_()
{
    if (!isActiveWindow())
    {
        activateWindowWithToken_(QString{});
    }
}

void MainWindow::raise_main_window_ui_()
{
    // Focus/activation itself is handled by the caller's path (startup
    // activateOnStartup() or the forwarded activation token) — this only
    // makes sure a tray-hidden or minimized window is actually on screen.
    if (!isVisible())
    {
        show();
    }
    if (isMinimized())
    {
        showNormal();
    }
    raise();
}

void MainWindow::doLogin()
{
    // Secondary (spawned) window: the shared AccountManager is already populated
    // and syncing, and set_initial_account() pinned the account to display. Bind
    // the UI to it without touching disk, restoring, or re-adding accounts.
    if (is_secondary_window_startup_())
    {
        finish_login_ui_(active_account_->user_id);
        return;
    }

    // Pre-flight OS-level connectivity check — see tk::Host::
    // is_network_available()'s doc comment. Computed here, on the UI
    // thread, and threaded through so the worker-thread restore loop below
    // never touches Host.
    const bool network_available = brandingSurface_->host().is_network_available();

    // Migrate + restore every stored account (shared loop in ShellBase), off
    // the UI thread so the window stays responsive. The native per-account
    // notifier / UnifiedPush construction runs through
    // install_account_notifier_ / install_account_up_connector_ below.
    restore_all_accounts_async_(
        [this](RestoreResult restore)
        {
            if (!restore.any_accounts)
            {
                // Fresh install or every stored account failed to restore →
                // login view. Nothing to silently restore, so an autostart
                // launch can't stay hidden — the user needs to log in.
                if (start_hidden_)
                {
                    start_hidden_ = false;
                    show();
                }
                add_account_return_uid_.clear();
                show_login_after_restore_(restore, [this] { doLogin(); });
                return;
            }

            finish_login_ui_(restore.active_uid);
        },
        network_available);
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
        [this, uid](std::string room_id, std::string event_id,
                    std::string token)
        {
            on_notification_clicked_(uid, std::move(room_id),
                                     std::move(event_id), std::move(token));
        },
        [this, uid](std::string room_id, std::string event_id,
                    std::string reply_text)
        {
            // Deliberately does not switch account or navigate — matches
            // KDE's own reply UX of not raising the app on submit.
            send_notification_reply_(uid, std::move(room_id),
                                     std::move(event_id),
                                     std::move(reply_text));
        });
}

void MainWindow::activate_for_notification_(const std::string& token)
{
    pending_wayland_token_ = QString::fromStdString(token);
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

std::unique_ptr<tk::AudioPlayback> MainWindow::make_call_audio_output_()
{
    return mainAppSurface_->host().make_audio_playback();
}

tesseract::CallWindowBase* MainWindow::create_call_window_()
{
    return new qt6::CallWindow(this);
}

tesseract::AuxWindowBase* MainWindow::create_aux_window_(std::string title,
                                                         std::unique_ptr<tk::Widget> root,
                                                         int width, int height)
{
    return new qt6::AuxWindow(title, std::move(root), width, height, current_theme_);
}

std::vector<std::uint8_t> MainWindow::encode_selfie_jpeg_(
    const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h)
{
    QImage img(bgra, static_cast<int>(w), static_cast<int>(h),
               QImage::Format_ARGB32);
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 90);
    const QByteArray& ba = buf.data();
    return std::vector<std::uint8_t>(ba.begin(), ba.end());
}

bool MainWindow::main_window_foreground_for_tray_() const
{
    return isVisible() && isActiveWindow();
}

void MainWindow::hide_main_window_for_tray_()
{
    hide();
}

void MainWindow::show_main_window_for_tray_(const std::string& token)
{
    activateWindowWithToken_(QString::fromStdString(token));
}

void MainWindow::start_search_provider_if_needed_()
{
    if (krunner_)
    {
        return;
    }
    // Exactly one window owns the single app-wide KRunner D-Bus object
    // (multi-window), mirroring the tray-icon ownership guard above.
    if (!account_manager_.claim_search_provider_owner(this))
    {
        return;
    }
    krunner_ = std::make_unique<QtKRunnerPlugin>(account_manager_);
    if (!krunner_->is_available())
    {
        krunner_.reset();
        account_manager_.release_search_provider_owner(this);
    }
}

void MainWindow::start_mpris_if_needed_()
{
    if (mpris_)
    {
        return;
    }
    // Exactly one window owns the single app-wide MPRIS D-Bus object
    // (multi-window), mirroring the tray/KRunner ownership guards above.
    if (!account_manager_.claim_mpris_owner(this))
    {
        return;
    }
    mpris_ = std::make_unique<QtMprisPlayer>(account_manager_);
    if (!mpris_->is_available())
    {
        mpris_.reset();
        account_manager_.release_mpris_owner(this);
    }
}

bool MainWindow::spawn_relaunch_(const std::vector<std::string>& args)
{
    // Inside an AppImage the running binary lives on a mount that goes away
    // with this process; relaunch the .AppImage itself.
    const QByteArray appimage = qgetenv("APPIMAGE");
    const QString program = appimage.isEmpty()
                                ? QCoreApplication::applicationFilePath()
                                : QString::fromLocal8Bit(appimage);
    QStringList qargs;
    for (const auto& a : args)
        qargs << QString::fromStdString(a);
    return QProcess::startDetached(program, qargs);
}

void MainWindow::quit_app_()
{
    do_quit_();
}

void MainWindow::do_quit_()
{
    // Explicit quit (tray menu or userinfo "Quit"): bypass the hide-to-tray
    // logic in closeEvent so the app actually exits instead of just hiding.
    explicitly_quitting_ = true;
    qApp->quit();
}

void MainWindow::closeEvent(QCloseEvent* ev)
{
    // Only the window that owns the app-wide tray icon consults CloseAction —
    // spawned per-account windows have no tray_ and always close for real
    // below. HideToTray/Minimize are intercepted here (ignoring the event);
    // Quit falls through so the window closes. Hiding is only reachable when
    // the tray icon actually exists, so CloseAction::HideToTray can never
    // strand the app unreachable.
    if (!explicitly_quitting_ && tray_ && tray_->is_available())
    {
        switch (tesseract::Settings::instance().close_action)
        {
        case tesseract::Settings::CloseAction::HideToTray:
            ev->ignore();
            hide();
            return;
        case tesseract::Settings::CloseAction::Minimize:
            ev->ignore();
            showMinimized();
            return;
        case tesseract::Settings::CloseAction::Quit:
            break;
        }
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
    if (main_app_)
    {
        main_app_->room_list_view()->set_selected_room(room_id);
    }
    tab_navigate_room(room_id);
    QString token = pending_wayland_token_;
    pending_wayland_token_.clear();
    activateWindowWithToken_(token);
}

void MainWindow::apply_window_title_ui_(const std::string& title)
{
    setWindowTitle(QString::fromStdString(title));
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

void MainWindow::repaint_pickers_native_()
{
    if (compose_popups_)
        compose_popups_->on_image_loaded();
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

void MainWindow::on_space_children_cache_ready_ui_()
{
    refresh_room_list_();
    refresh_pickers_packs_();
}

void MainWindow::on_space_unjoined_summaries_ready_ui_(const std::string&)
{
    refresh_room_list_();
}

void MainWindow::on_tray_unread_changed_(bool has_unread, bool has_highlight)
{
    if (tray_)
    {
        tray_->set_unread(has_unread, has_highlight);
    }
}

std::unique_ptr<tk::Image>
MainWindow::decode_avatar_(const std::vector<std::uint8_t>& bytes)
{
    // QImage/QImageReader are safe on a worker, and make_image wraps a QImage
    // (not a QPixmap). First frame only, scaled to the avatar cache size.
    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    const QSize native = reader.size();
    if (native.isValid() &&
        (native.width() > kAvatarCacheSize || native.height() > kAvatarCacheSize))
    {
        reader.setScaledSize(
            native.scaled(kAvatarCacheSize, kAvatarCacheSize, Qt::KeepAspectRatio));
    }
    QImage img = reader.read();
    if (!img.isNull() &&
        (img.width() > kAvatarCacheSize || img.height() > kAvatarCacheSize))
    {
        img = img.scaled(kAvatarCacheSize, kAvatarCacheSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    }
    if (img.isNull())
    {
        return nullptr;
    }
    return tk::qt6::make_image(std::move(img));
}

void MainWindow::ingest_tile_media_(const tk::CacheKey& cache_key,
                                    std::vector<std::uint8_t> bytes)
{
    // Map tiles are small; decode off the UI thread anyway.
    run_async_(
        [this, cache_key, bytes = std::move(bytes)]() mutable
        {
            QImage img;
            if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                  static_cast<int>(bytes.size())))
            {
                return;
            }
            auto decoded = std::make_shared<QImage>(std::move(img));
            post_to_ui_alive_(
                [this, cache_key, decoded]() mutable
                {
                    if (account_manager_.image_cache().contains(cache_key))
                    {
                        return;
                    }
                    account_manager_.image_cache().store(
                        cache_key, tk::qt6::make_image(std::move(*decoded)));
                    if (mainAppSurface_)
                    {
                        mainAppSurface_->update();
                    }
                    notify_secondary_media_ready_(cache_key.id, MediaKind::Tile);
                });
        });
}

void MainWindow::prompt_passphrase_(std::string title,
                                    std::function<void(std::string)> cb)
{
    bool ok = false;
    const QString pass = QInputDialog::getText(
        this, QString::fromStdString(title),
        QString::fromStdString(tk::tr("Passphrase")), QLineEdit::Password, "",
        &ok);
    if (ok && !pass.isEmpty())
        cb(pass.toStdString());
}

void MainWindow::pick_open_file_(std::string title,
                                 std::function<void(std::string)> cb)
{
    const QString path =
        QFileDialog::getOpenFileName(this, QString::fromStdString(title));
    if (!path.isEmpty())
        cb(path.toStdString());
}

void MainWindow::pick_folder_(std::string title,
                              std::function<void(std::string)> cb)
{
    const QString dir =
        QFileDialog::getExistingDirectory(this, QString::fromStdString(title));
    if (!dir.isEmpty())
        cb(dir.toStdString());
}

void MainWindow::show_message_dialog_(std::string title, std::string text,
                                      bool is_error)
{
    const QString t = QString::fromStdString(title);
    const QString m = QString::fromStdString(text);
    if (is_error)
        QMessageBox::warning(this, t, m);
    else
        QMessageBox::information(this, t, m);
}

void MainWindow::pick_save_file_(std::string title, std::string suggested_name,
                                 std::function<void(std::string)> cb)
{
    const QString path = QFileDialog::getSaveFileName(
        this, QString::fromStdString(title), QString::fromStdString(suggested_name));
    if (!path.isEmpty())
        cb(path.toStdString());
}

void MainWindow::pick_media_save_path_(const tesseract::views::MediaSaveSpec& spec,
                                       std::function<void(std::string)> cb)
{
    const QString path = QFileDialog::getSaveFileName(
        this, QString::fromStdString(spec.title),
        QString::fromStdString(spec.suggested_name), save_filter_for(spec));
    if (!path.isEmpty())
        cb(path.toStdString());
}

void MainWindow::pick_image_file_(
    std::function<void(std::vector<uint8_t>, std::string)> cb)
{
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromStdString(tk::tr("Select image")), {},
        QString::fromStdString(tk::tr("Images (*.png *.jpg *.jpeg *.gif *.webp)")));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    // The file is chosen by the user but read here on the UI thread, so refuse
    // anything too large to be a still worth sending before reading it.
    constexpr qint64 kMaxPickedImageBytes = 64LL * 1024 * 1024;
    if (f.size() > kMaxPickedImageBytes)
    {
        show_status_message_(tk::tr("That image is too large to send."));
        return;
    }
    QByteArray data = f.readAll();
    std::vector<uint8_t> bytes(data.begin(), data.end());
    // The type comes from the content, not the extension: a mislabelled file
    // would otherwise be uploaded with the wrong MIME type.
    const std::string_view sniffed = tk::sniff_image_mime(bytes);
    if (sniffed.empty())
    {
        show_status_message_(tk::tr("That file isn't a PNG, JPEG, GIF or WebP image."));
        return;
    }
    const std::string mime(sniffed);
    post_to_ui_([cb = std::move(cb), bytes = std::move(bytes), mime]() mutable
    {
        cb(std::move(bytes), mime);
    });
}

MainWindow::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                          int max_h)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_image(bytes, max_w, max_h, tk::qt6::make_image_rgba);
    // Untrusted bytes: refuse over-budget images before any codec allocates (tk/image_sniff.h).
    if (!tk::decode_size_allowed(bytes, tk::bytes_may_be_animated(bytes)))
        return DecodedImage{};
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
        // Hard cap on decoded frame count — shared with every other
        // platform (tk::kAnimDecodeMaxFrames). A pathological/malicious
        // animated image (huge frame count) would otherwise allocate one
        // full-size QImage per frame with no ceiling.
        QImage frame;
        int frame_count = 0;
        while (frame_count < tk::kAnimDecodeMaxFrames && reader.read(&frame))
        {
            int delay = tk::normalize_frame_delay_ms(reader.nextImageDelay());
            // Safety clamp — no-op when setScaledSize already handled it.
            if (frame.width() > max_w || frame.height() > max_h)
            {
                frame = frame.scaled(max_w, max_h, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            d.frames.push_back(tk::qt6::make_image(std::move(frame)));
            d.delays_ms.push_back(delay);
            ++frame_count;
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

bool MainWindow::decode_image_streamed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_streamed(bytes, max_w, max_h, tk::qt6::make_image_rgba,
                                               on_first_frame, on_frame);
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    if (bytes.empty())
    {
        return false;
    }
    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    const QSize native_size = reader.size();
    if (native_size.isValid() &&
        (native_size.width() > max_w || native_size.height() > max_h))
    {
        reader.setScaledSize(
            native_size.scaled(max_w, max_h, Qt::KeepAspectRatio));
    }

    if (!reader.supportsAnimation() || reader.imageCount() <= 1)
    {
        return false;
    }

    // Hard cap on decoded frame count — shared with every other platform
    // and decode_image_'s whole-batch loop above (tk::kAnimDecodeMaxFrames).
    bool got_first = false;
    int index = 0;
    QImage frame;
    while (index < tk::kAnimDecodeMaxFrames && reader.read(&frame))
    {
        int delay = tk::normalize_frame_delay_ms(reader.nextImageDelay());
        // Safety clamp — no-op when setScaledSize already handled it.
        if (frame.width() > max_w || frame.height() > max_h)
        {
            frame = frame.scaled(max_w, max_h, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
        auto img = tk::qt6::make_image(std::move(frame));
        if (index == 0)
        {
            got_first = true;
            on_first_frame(std::move(img), delay);
        }
        else
        {
            on_frame(index, std::move(img), delay);
        }
        ++index;
        frame = QImage();
    }
    return got_first;
}

namespace
{

// tk::AnimDecodeSession for Qt6: QImageReader is a strictly sequential
// decoder (no seek — Qt::jumpToImage() is unsupported for GIF and most
// animated formats), so unlike a genuinely random-access backend, this
// session must keep its reader alive across decode_next_batch() calls to
// avoid re-decoding from frame 0 every time. restart() (looping back after
// the window has advanced past frame 0) is the one place a full re-decode
// from the retained bytes is unavoidable — inherent to how these formats
// work, not a Qt limitation (GTK4's gdk-pixbuf reader and, for GIF
// specifically, even Windows' WIC decoder have the exact same constraint).
class Qt6AnimSession final : public tk::AnimDecodeSession
{
public:
    Qt6AnimSession(QByteArray bytes, int max_w, int max_h, int total_frames)
        : bytes_(std::move(bytes)), max_w_(max_w), max_h_(max_h),
          total_frames_(total_frames)
    {
        open_();
    }

    int decode_next_batch(
        int n,
        const std::function<void(int, std::unique_ptr<tk::Image>, int)>&
            on_frame) override
    {
        int produced = 0;
        for (int attempted = 0; attempted < n && cursor_ < total_frames_;
             ++attempted)
        {
            QImage frame;
            if (!reader_->read(&frame))
            {
                // A sequential reader can't skip a bad frame and keep going
                // — one failed read leaves its internal position unusable,
                // so treat this as having reached the end rather than
                // retrying (matches decode_image_'s existing whole-batch
                // loop, which also stops at the first read failure).
                cursor_ = total_frames_;
                break;
            }
            int delay = tk::normalize_frame_delay_ms(reader_->nextImageDelay());
            if (frame.width() > max_w_ || frame.height() > max_h_)
            {
                frame = frame.scaled(max_w_, max_h_, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            const int idx = cursor_++;
            on_frame(idx, tk::qt6::make_image(std::move(frame)), delay);
            ++produced;
        }
        return produced;
    }

    bool exhausted() const override { return cursor_ >= total_frames_; }

    void restart() override
    {
        open_();
        cursor_ = 0;
    }

    std::size_t memory_bytes() const override
    {
        return static_cast<std::size_t>(bytes_.size());
    }

private:
    void open_()
    {
        buf_ = std::make_unique<QBuffer>(&bytes_);
        buf_->open(QIODevice::ReadOnly);
        reader_ = std::make_unique<QImageReader>(buf_.get());
        reader_->setAutoTransform(true);
        const QSize native = reader_->size();
        if (native.isValid() &&
            (native.width() > max_w_ || native.height() > max_h_))
        {
            reader_->setScaledSize(
                native.scaled(max_w_, max_h_, Qt::KeepAspectRatio));
        }
    }

    QByteArray bytes_;
    std::unique_ptr<QBuffer> buf_;
    std::unique_ptr<QImageReader> reader_;
    int max_w_, max_h_;
    int total_frames_;
    int cursor_ = 0;
};

} // namespace

bool MainWindow::decode_image_streamed_windowed_(
    const std::vector<uint8_t>& bytes, int max_w, int max_h,
    const std::function<void(std::unique_ptr<tk::Image>, int,
                             std::shared_ptr<tk::AnimDecodeSession>,
                             std::size_t)>& on_first_frame,
    const std::function<void(int, std::unique_ptr<tk::Image>, int)>& on_frame)
{
    if (tesseract::is_jxl_data(bytes))
        return tesseract::jxl::decode_streamed_windowed(bytes, max_w, max_h, tk::qt6::make_image_rgba,
                                                        on_first_frame, on_frame);
    if (!tk::decode_size_allowed(bytes, /*animated=*/true))
        return false;
    if (bytes.empty())
    {
        return false;
    }
    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    const QSize native_size = reader.size();
    if (native_size.isValid() &&
        (native_size.width() > max_w || native_size.height() > max_h))
    {
        reader.setScaledSize(
            native_size.scaled(max_w, max_h, Qt::KeepAspectRatio));
    }

    if (!reader.supportsAnimation() || reader.imageCount() <= 1)
    {
        return false;
    }

    int frame_count = reader.imageCount();
    if (frame_count <= 0)
    {
        // Some plugins report an unknown count (-1) up front — clamp to the
        // cap and let the session's own read-failure handling in
        // decode_next_batch discover the true end.
        frame_count = tk::kAnimDecodeMaxFrames;
    }
    frame_count = std::min(frame_count, tk::kAnimDecodeMaxFrames);

    // Animations at or under this many frames decode whole (unwindowed) —
    // matches AnimImageCache's own tuning for "not worth the complexity".
    if (frame_count <= tk::kAnimDecodeWindowThresholdFrames)
    {
        bool got_first = false;
        int index = 0;
        QImage frame;
        while (index < tk::kAnimDecodeMaxFrames && reader.read(&frame))
        {
            int delay = tk::normalize_frame_delay_ms(reader.nextImageDelay());
            if (frame.width() > max_w || frame.height() > max_h)
            {
                frame = frame.scaled(max_w, max_h, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            auto img = tk::qt6::make_image(std::move(frame));
            if (index == 0)
            {
                got_first = true;
                on_first_frame(std::move(img), delay, nullptr, 0);
            }
            else
            {
                on_frame(index, std::move(img), delay);
            }
            ++index;
        }
        return got_first;
    }

    // Windowed path: qb is copied into the session (its own retained
    // source-byte copy), so the caller's buf/reader/qb above can be
    // discarded once we're done sniffing the header here.
    auto session =
        std::make_shared<Qt6AnimSession>(qb, max_w, max_h, frame_count);
    bool got_first = false;
    session->decode_next_batch(
        tk::kAnimDecodeInitialBatchFrames,
        [&](int idx, std::unique_ptr<tk::Image> img, int delay_ms)
        {
            if (!got_first)
            {
                got_first = true;
                on_first_frame(std::move(img), delay_ms, session,
                               static_cast<std::size_t>(frame_count));
            }
            else
            {
                on_frame(idx, std::move(img), delay_ms);
            }
        });
    return got_first;
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

// A backend can parse a file and then never report a frame, a loaded status or an
// error (audio-only data in a video container, a stalled pipeline). Without a limit
// the player and its copy of the file would live until the window closed.
constexpr int kMediaProbeTimeoutMs = 10000;

void MainWindow::probe_drop_video_(std::uint32_t pending_gen,
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
    auto* timeout = new QTimer(player);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, player,
        [this, player, state, target, target_alive]()
        {
            if (state->done) return;
            state->done = true;
            player->stop();
            player->deleteLater();
            post_pending_attachment_(state->info, target, target_alive);
        });
    timeout->start(kMediaProbeTimeoutMs);
    player->play();
}

void MainWindow::probe_drop_audio_(std::uint32_t pending_gen,
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
    auto* timeout = new QTimer(player);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, player,
        [this, player, pending_gen, done, target, target_alive]()
        {
            if (*done) return;
            *done = true;
            player->stop();
            player->deleteLater();
            tesseract::views::MediaInfo info;
            info.pending_gen = pending_gen;
            post_pending_attachment_(info, target, target_alive);
        });
    timeout->start(kMediaProbeTimeoutMs);
    player->play();
}

void MainWindow::decode_video_first_frame_(
    std::vector<std::uint8_t> bytes,
    std::function<void(std::vector<std::uint8_t>)> done)
{
    // Runs the QMediaPlayer/QVideoSink decode + JPEG-encode against `bytes`
    // and invokes `done(jpeg)` exactly once (empty jpeg on any failure,
    // including a decode error — e.g. a truncated prefix that lacks its
    // container's sample table).
    // Qt multimedia objects (QMediaPlayer, QVideoSink) must live on
    // the UI thread — the caller is already on the UI thread.
    auto* player = new QMediaPlayer(this);
    auto* sink = new QVideoSink(player);
    player->setVideoSink(sink);
    auto* buf = new QBuffer(player);
    QByteArray ba(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<qsizetype>(bytes.size()));
    buf->setData(ba);
    buf->open(QIODevice::ReadOnly);
    player->setSourceDevice(buf);
    // Both signal handlers can fire (e.g. an error after a partial
    // frame) — `fired` ensures `done` runs exactly once.
    auto fired = std::make_shared<bool>(false);
    QObject::connect(
        sink, &QVideoSink::videoFrameChanged, sink,
        [done, player, fired](const QVideoFrame& frame) mutable
        {
            if (*fired || !frame.isValid())
                return;
            *fired = true;
            player->stop();
            player->deleteLater();
            QImage img = frame.toImage();
            if (img.isNull())
            {
                done({});
                return;
            }
            QByteArray enc;
            QBuffer encbuf(&enc);
            encbuf.open(QIODevice::WriteOnly);
            img.save(&encbuf, "JPEG", 85);
            if (enc.isEmpty())
            {
                done({});
                return;
            }
            std::vector<std::uint8_t> v(
                reinterpret_cast<const std::uint8_t*>(enc.constData()),
                reinterpret_cast<const std::uint8_t*>(enc.constData()) +
                    enc.size());
            done(std::move(v));
        });
    QObject::connect(
        player, &QMediaPlayer::errorOccurred, player,
        [done, player, fired](QMediaPlayer::Error error,
                              const QString&) mutable
        {
            if (*fired || error == QMediaPlayer::NoError)
                return;
            *fired = true;
            player->stop();
            player->deleteLater();
            done({});
        });
    auto* timeout = new QTimer(player);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, player,
        [done, player, fired]() mutable
        {
            if (*fired)
                return;
            *fired = true;
            player->stop();
            player->deleteLater();
            done({});
        });
    timeout->start(kMediaProbeTimeoutMs);
    player->play();
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

void MainWindow::onInflightTick_()
{
    inflight_tick_();
}

void MainWindow::start_inflight_tick_()
{
    if (tk_inflight_timer_ && !tk_inflight_timer_->isActive())
        tk_inflight_timer_->start();
}

void MainWindow::stop_inflight_tick_()
{
    if (tk_inflight_timer_)
        tk_inflight_timer_->stop();
}

void MainWindow::refreshLowPowerIcon_()
{
    if (!lowPowerLabel_)
        return;
    const qreal dpr = devicePixelRatioF();
    const int phys = std::max(1, static_cast<int>(std::lround(14.0 * dpr)));
    auto rgba = tk::rasterize_svg_rgba(tk::low_power_icon_svg(), phys,
                                       current_theme_.palette.text_secondary);
    if (static_cast<int>(rgba.size()) != phys * phys * 4)
    {
        lowPowerLabel_->clear();
        return;
    }
    QImage img(rgba.data(), phys, phys, phys * 4, QImage::Format_RGBA8888);
    QPixmap pm = QPixmap::fromImage(img.copy()); // copy: detach from rgba buffer
    pm.setDevicePixelRatio(dpr);
    lowPowerLabel_->setPixmap(pm);
}

void MainWindow::on_low_power_mode_ui_(bool active)
{
    if (lowPowerLabel_)
        lowPowerLabel_->setVisible(active);
}

void MainWindow::repaint_inflight_spinner_()
{
    if (inflightDot_)
    {
        inflightDot_->update_phase(inflight_spin_phase_());
        inflightDot_->update();
    }
}

void MainWindow::update_main_anim_regions_()
{
    if (mainAppSurface_)
    {
        mainAppSurface_->update_anim_regions();
    }
}

void MainWindow::cache_rgba_image_(const tk::CacheKey& key, int w, int h,
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

// ---------------------------------------------------------------------------


void MainWindow::show_sync_status_text_(const std::string& text)
{
    // Plain text replaces whatever the link label was showing.
    if (statusLinkLabel_)
    {
        statusLinkLabel_->hide();
    }
    statusBar()->showMessage(QString::fromStdString(text));
}

void MainWindow::on_sync_status_override_kept_()
{
    if (statusLinkLabel_ && !statusLinkLabel_->text().isEmpty())
    {
        statusBar()->clearMessage();
        statusLinkLabel_->show();
    }
}

// ---------------------------------------------------------------------------
// User identity strip + logout
// ---------------------------------------------------------------------------

void MainWindow::onUserStripContextMenu(const QPoint& global_pos)
{
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setStyleSheet(tk::qt6::build_menu_qss(current_theme_));
    const auto items = build_user_menu_items_(
        [this] { openSettings(); },
        [this] { begin_add_account(); },
        [this] { start_qr_grant_overlay(); },
        [this] { logout_active_account_ui_(); },
        [this] { do_quit_(); },
        verify_session_menu_callback_());
    for (const auto& item : items)
    {
        if (item.label.empty())
        {
            menu->addSeparator();
            continue;
        }
        auto* act = menu->addAction(QString::fromStdString(item.label));
        QObject::connect(act, &QAction::triggered, this,
                         [cb = item.callback] { cb(); });
    }
    menu->popup(global_pos);
}

std::unique_ptr<tk::SurfaceBase> MainWindow::create_login_surface_ui_()
{
    auto* surface = new tk::qt6::Surface(current_theme_, contentStack_);
    contentStack_->addWidget(surface);
    return std::unique_ptr<tk::SurfaceBase>(surface);
}

void MainWindow::show_login_page_ui_()
{
    contentStack_->setCurrentWidget(login_native_());
}

bool MainWindow::open_login_url_ui_(const std::string& url)
{
    if (!tesseract::Client::is_launchable_url(url))
        return true; // nothing to open, nothing to report
    if (QDesktopServices::openUrl(QUrl(QString::fromStdString(url))))
        return true;
    return tesseract::Client::open_in_browser(url);
}

std::unique_ptr<tk::SurfaceBase> MainWindow::create_settings_surface_ui_()
{
    auto* surface = new tk::qt6::Surface(current_theme_, contentStack_);
    contentStack_->addWidget(surface);
    return std::unique_ptr<tk::SurfaceBase>(surface);
}

void MainWindow::grab_main_surface_focus_()
{
    if (mainAppSurface_)
        mainAppSurface_->setFocus();
}

tk::SurfaceBase* MainWindow::main_surface_()
{
    return mainAppSurface_;
}

void MainWindow::openSettings()
{
    if (!prepare_settings_view_())
        return;

    contentStack_->setCurrentWidget(settings_native_());
    set_app_settings_open_(true);
    start_search_index_stats_poll_();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // Slash/shortcode/mention popups' outside-click dismissal is now handled
    // internally by their PopupSurfaceHandle (see
    // QtPopupSurfaceHandle::on_dismiss_requested, wired to each controller's
    // hide() at construction) — no block needed here anymore.
    if (obj == statusBar() && event->type() == QEvent::MouseButtonRelease)
        trigger_persistent_status_click_();
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
    // (refresh_user_strip_).
    handle_sync_error_impl_(std::move(context), std::move(user_id),
                            std::move(description), soft_logout);
}

// ── Tab management (ShellBase virtual hooks) ──────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::on_room_list_state_ui_()
{
    refresh_sync_status_();
    on_inflight_ui_();
}

bool MainWindow::tray_available_() const
{
    return tray_ && tray_->is_available();
}

void MainWindow::on_inflight_ui_()
{
    const auto c  = inflight_dot_color_();
    inflightDot_->update_state(inflight_total_(), c);
    const QString tip = QString::fromStdString(inflight_tooltip_text_());
    inflightDot_->setToolTip(tip);
    // Force-refresh the tooltip window if it is currently shown over this widget.
    if (inflightDot_->underMouse())
        QToolTip::showText(QCursor::pos(), tip, inflightDot_);
}

void MainWindow::on_show_status_message_ui_(const std::string& msg)
{
    const auto segs = parse_status_message_(msg); // opt-in gate (server text → plain)
    if (!tesseract::status_has_links(segs))
    {
        if (statusLinkLabel_)
        {
            // clear() as well as hide() so a stale link text can't be mistaken
            // for an active hyperlinked override in refresh_sync_status_().
            statusLinkLabel_->clear();
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
    refresh_sync_status_();
}

void MainWindow::on_startup_restore_progress_ui_(const std::string& status)
{
    if (brandingView_)
    {
        brandingView_->set_status(status);
    }
}

// ---------------------------------------------------------------------------
// Multi-account orchestration
// ---------------------------------------------------------------------------

void MainWindow::start_platform_services_ui_()
{
    if (!tray_ && account_manager_.claim_tray_owner(this))
    {
        tray_ = std::make_unique<LinuxQtTrayIcon>(
            [this] { tray_activate_(); }, [this] { tray_toggle_(); },
            [this] { do_quit_(); }, this);
        if (tray_->is_available())
        {
            qApp->setQuitOnLastWindowClosed(false);
            tray_->set_unread(last_tray_unread_, last_tray_highlight_);
        }
    }
    broadcast_rebuild_tray_();
    start_search_provider_if_needed_();
    start_mpris_if_needed_();
}

void MainWindow::doLogout()
{
    // Legacy shim that the tray / external code may still call. Routes to
    // the active-account logout path.
    logout_active_account_ui_();
}

// ── Cross-signing / SAS verification hooks ────────────────────────────────────

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

std::optional<tk::Color> MainWindow::os_accent_color_() const
{
    if (portal_accent_valid_)
    {
        return portal_accent_;
    }
    // No portal accent-color support (older DE, or portal missing). Fall
    // back to the application palette's highlight colour — reflects the
    // KDE/Breeze accent under Plasma even without portal support, or the
    // current style's generic highlight otherwise.
    const QColor hl = qApp->palette().color(QPalette::Active, QPalette::Highlight);
    return tk::Color::rgba(static_cast<std::uint8_t>(hl.red()),
                            static_cast<std::uint8_t>(hl.green()),
                            static_cast<std::uint8_t>(hl.blue()), 255);
}

namespace
{

// org.freedesktop.appearance's "accent-color" portal key is a (ddd) sRGB
// struct, components in [0,1]. Reading the three doubles out of the
// QDBusArgument extracted from the reply's QVariant *must* go through a
// dedicated operator>> like this one rather than inline
// beginStructure()/operator>>/endStructure() calls in the same scope where
// the argument was obtained — empirically, the inline form corrupts the
// underlying D-Bus message iterator on this Qt6/libdbus combination and
// aborts the process with "type struct 114 not a basic type" (a hard
// libdbus type-mismatch check, not a recoverable Qt error). Routing the
// extraction through its own function, the way Qt's own custom-type
// marshalling examples do, avoids it.
struct PortalAccentTriple
{
    double r = -1.0, g = -1.0, b = -1.0;
};

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalAccentTriple& t)
{
    arg.beginStructure();
    arg >> t.r >> t.g >> t.b;
    arg.endStructure();
    return arg;
}

std::optional<tk::Color> parse_portal_accent_(const QVariant& v)
{
    if (!v.canConvert<QDBusArgument>())
    {
        return std::nullopt;
    }
    QDBusArgument arg = v.value<QDBusArgument>();
    if (arg.currentSignature() != QLatin1String("(ddd)"))
    {
        return std::nullopt;
    }
    PortalAccentTriple t;
    arg >> t;
    if (t.r < 0.0 || t.r > 1.0 || t.g < 0.0 || t.g > 1.0 || t.b < 0.0 || t.b > 1.0)
    {
        return std::nullopt;
    }
    return tk::Color::rgba(static_cast<std::uint8_t>(qRound(t.r * 255.0)),
                            static_cast<std::uint8_t>(qRound(t.g * 255.0)),
                            static_cast<std::uint8_t>(qRound(t.b * 255.0)), 255);
}
} // namespace

// Reads one org.freedesktop.appearance key without blocking: a portal that is
// still starting can hold the call for seconds, and the change signal only
// fires on a later change, so a bounded sync read could never be corrected.
// The watcher is a child of this window, so a reply after it is gone is dropped.
// ReadOne (portal interface v2+), not the deprecated Read: Read double-wraps its
// return value in an extra D-Bus variant layer, which makes QVariant::toInt()
// silently yield 0 instead of the real setting.
void MainWindow::read_portal_setting_async_(
    const QString& key, std::function<void(const QVariant&)> apply)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("ReadOne"));
    msg << QStringLiteral("org.freedesktop.appearance") << key;
    auto* watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [watcher, apply = std::move(apply)]
            {
                watcher->deleteLater();
                QDBusPendingReply<QDBusVariant> reply = *watcher;
                if (!reply.isError())
                {
                    apply(reply.value().variant());
                }
            });
}

void MainWindow::read_portal_accent_color_()
{
    read_portal_setting_async_(QStringLiteral("accent-color"),
                               [this](const QVariant& v)
                               { apply_portal_accent_(v); });
}

void MainWindow::read_portal_color_scheme_()
{
    read_portal_setting_async_(QStringLiteral("color-scheme"),
                               [this](const QVariant& v)
                               { apply_portal_color_scheme_(v.toInt()); });
}

// Applies the new OS theme when it differs from the one on screen and the user
// follows the OS.
void MainWindow::apply_portal_color_scheme_(int scheme)
{
    portal_color_scheme_ = scheme;
    if (tesseract::Settings::instance().theme_pref ==
            tesseract::Settings::ThemePreference::System &&
        os_color_scheme_() != current_theme_.mode)
    {
        apply_current_theme_();
    }
}

void MainWindow::apply_portal_accent_(const QVariant& value)
{
    if (auto c = parse_portal_accent_(value))
    {
        portal_accent_ = *c;
        portal_accent_valid_ = true;
    }
    else
    {
        portal_accent_valid_ = false;
    }
    on_system_accent_changed_();
}

void MainWindow::on_portal_setting_changed_(const QString& ns,
                                            const QString& key,
                                            const QDBusVariant& value)
{
    if (ns != QLatin1String("org.freedesktop.appearance"))
    {
        return;
    }
    if (key == QLatin1String("color-scheme"))
    {
        apply_portal_color_scheme_(value.variant().toInt());
    }
    else if (key == QLatin1String("accent-color"))
    {
        apply_portal_accent_(value.variant());
    }
}

// ---------------------------------------------------------------------------
// Shortcode popup
// ---------------------------------------------------------------------------

// ── Slash-command popup ─────────────────────────────────────────────────────

std::unique_ptr<tk::Image> MainWindow::make_image_from_bgra_(
    int w, int h, const std::uint8_t* bgra)
{
    QImage qi(bgra, w, h, QImage::Format_ARGB32);
    return tk::qt6::make_image(qi.copy());
}

// ── Shortcode popup ─────────────────────────────────────────────────────────

// ── @mention popup ─────────────────────────────────────────────────────────

// ---------------------------------------------------------------------------

void MainWindow::apply_native_theme_(const tk::Theme& t)
{
    tk::qt6::set_link_color(t.palette.link);
}

void MainWindow::after_theme_applied_(const tk::Theme& t)
{
    {
        const auto& p = t.palette;
        QPalette pal = statusBar()->palette();
        pal.setColor(QPalette::Window, QColor(p.chrome_bg.r, p.chrome_bg.g,
                                              p.chrome_bg.b, p.chrome_bg.a));
        pal.setColor(QPalette::WindowText,
                     QColor(p.text_secondary.r, p.text_secondary.g,
                            p.text_secondary.b, p.text_secondary.a));
        // Rich-text status links (statusLinkLabel_) take their colour from
        // QPalette::Link, which otherwise stays Qt's dark blue.
        const QColor link(p.accent.r, p.accent.g, p.accent.b, p.accent.a);
        pal.setColor(QPalette::Link, link);
        pal.setColor(QPalette::LinkVisited, link);
        statusBar()->setPalette(pal);
        statusBar()->setAutoFillBackground(true);
    }
    refreshLowPowerIcon_(); // re-tint the glyph for the new theme
    if (mainAppSurface_)
    {
        mainAppSurface_->relayout();
    }
}

void MainWindow::for_each_shell_surface_(
    const std::function<void(tk::SurfaceBase&)>& fn)
{
    if (brandingSurface_)
        fn(*brandingSurface_);
    if (mainAppSurface_)
        fn(*mainAppSurface_);
}

void MainWindow::raise_and_activate_()
{
    raise();
    activateWindow();
}

void MainWindow::set_window_fullscreen_(bool on)
{
    if (on == isFullScreen())
        return;
    // Drop the status bar too — a lone strip along the bottom looks wrong
    // behind full-screen media.
    statusBar()->setVisible(!on);
    if (on)
    {
        fs_was_maximized_ = isMaximized();
        showFullScreen();
    }
    else if (fs_was_maximized_)
    {
        showMaximized();
    }
    else
    {
        showNormal();
    }
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
