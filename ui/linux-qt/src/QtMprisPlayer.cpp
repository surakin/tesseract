#include "QtMprisPlayer.h"

#include "app/AccountManager.h"
#include "app/MediaPlaybackHub.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <cctype>
#include <cstdint>
#include <string>

namespace
{

// MPRIS trackids must be valid D-Bus object paths ([A-Za-z0-9_/] only) —
// Matrix event ids ("$abc:server.org") are not, so sanitize.
QString sanitize_track_id(const std::string& event_id)
{
    QString out = QStringLiteral("/org/tesseract/qt/Track");
    for (char c : event_id)
    {
        out += std::isalnum(static_cast<unsigned char>(c)) ? QChar(QLatin1Char(c))
                                                             : QChar(QLatin1Char('_'));
    }
    return out;
}

QVariantMap build_metadata(const tesseract::NowPlaying& np)
{
    QVariantMap m;
    if (np.kind == tesseract::NowPlaying::Kind::None)
    {
        return m; // empty dict — nothing loaded
    }
    m[QStringLiteral("mpris:trackid")] =
        QVariant::fromValue(QDBusObjectPath(sanitize_track_id(np.event_id)));
    m[QStringLiteral("mpris:length")] =
        static_cast<qlonglong>(np.duration_ms) * 1000;
    m[QStringLiteral("xesam:title")] = QString::fromStdString(np.title);
    if (!np.artist.empty())
    {
        m[QStringLiteral("xesam:artist")] =
            QStringList{QString::fromStdString(np.artist)};
    }
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// MediaPlayer2Adaptor — org.mpris.MediaPlayer2 (root interface).
// ---------------------------------------------------------------------------

class MediaPlayer2Adaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)
public:
    explicit MediaPlayer2Adaptor(QObject* parent) : QDBusAbstractAdaptor(parent)
    {
    }

    bool canQuit() const
    {
        return false;
    }
    bool canRaise() const
    {
        return false;
    }
    bool hasTrackList() const
    {
        return false;
    }
    QString identity() const
    {
        return QStringLiteral("Tesseract");
    }
    QString desktopEntry() const
    {
        return QStringLiteral("tesseract-matrix");
    }
    QStringList supportedUriSchemes() const
    {
        return {};
    }
    QStringList supportedMimeTypes() const
    {
        return {};
    }

public slots:
    // CanRaise/CanQuit both report false, so well-behaved clients won't
    // offer these — no window to raise from a headless identity alone, and
    // wiring Quit to app-exit on a chat client (rather than a dedicated
    // media player) would be surprising for a stray MPRIS client to trigger.
    void Raise()
    {
    }
    void Quit()
    {
    }
};

// ---------------------------------------------------------------------------
// MediaPlayer2PlayerAdaptor — org.mpris.MediaPlayer2.Player.
// ---------------------------------------------------------------------------

class MediaPlayer2PlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ rate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)
public:
    MediaPlayer2PlayerAdaptor(QObject* parent, tesseract::AccountManager* am)
        : QDBusAbstractAdaptor(parent), account_manager_(am)
    {
    }

    QString playbackStatus() const
    {
        const auto& np = account_manager_->media_playback_hub().current();
        if (np.kind == tesseract::NowPlaying::Kind::None)
        {
            return QStringLiteral("Stopped");
        }
        return np.is_playing ? QStringLiteral("Playing") : QStringLiteral("Paused");
    }
    double rate() const
    {
        return 1.0;
    }
    QVariantMap metadata() const
    {
        return build_metadata(account_manager_->media_playback_hub().current());
    }
    double volume() const
    {
        return 1.0;
    }
    qlonglong position() const
    {
        return static_cast<qlonglong>(
                   account_manager_->media_playback_hub().current().position_ms) *
               1000;
    }
    double minimumRate() const
    {
        return 1.0;
    }
    double maximumRate() const
    {
        return 1.0;
    }
    // No playlist concept — rooms/messages aren't a linear playlist.
    bool canGoNext() const
    {
        return false;
    }
    bool canGoPrevious() const
    {
        return false;
    }
    bool canPlay() const
    {
        return loaded_();
    }
    bool canPause() const
    {
        return loaded_();
    }
    bool canSeek() const
    {
        return loaded_();
    }
    bool canControl() const
    {
        return true;
    }

public slots:
    void Play()
    {
        account_manager_->media_playback_hub().play();
    }
    void Pause()
    {
        account_manager_->media_playback_hub().pause();
    }
    void PlayPause()
    {
        account_manager_->media_playback_hub().play_pause();
    }
    void Stop()
    {
        account_manager_->media_playback_hub().stop();
    }
    void Seek(qlonglong offsetUs)
    {
        account_manager_->media_playback_hub().seek(
            static_cast<std::int64_t>(offsetUs / 1000));
    }
    void SetPosition(const QDBusObjectPath&, qlonglong)
    {
        // v1: only relative Seek is wired through MediaPlaybackHub; an
        // absolute SetPosition would need the current position round-tripped
        // from TimelineMediaController, which isn't exposed here. Treat as a
        // no-op rather than silently doing the wrong seek — mirrors
        // GtkMprisPlayer.cpp's identical v1 scope decision.
    }

signals:
    void Seeked(qlonglong position);

private:
    bool loaded_() const
    {
        return account_manager_->media_playback_hub().current().kind !=
               tesseract::NowPlaying::Kind::None;
    }

    tesseract::AccountManager* account_manager_;
};

// ---------------------------------------------------------------------------
// QtMprisPlayer
// ---------------------------------------------------------------------------

struct QtMprisPlayer::Impl
{
    QObject* host = nullptr;
    tesseract::AccountManager* account_manager = nullptr;

    void emit_properties_changed()
    {
        const auto& np = account_manager->media_playback_hub().current();
        const bool loaded = np.kind != tesseract::NowPlaying::Kind::None;
        QVariantMap changed;
        changed[QStringLiteral("PlaybackStatus")] =
            np.kind == tesseract::NowPlaying::Kind::None
                ? QStringLiteral("Stopped")
                : (np.is_playing ? QStringLiteral("Playing")
                                 : QStringLiteral("Paused"));
        changed[QStringLiteral("Metadata")] = build_metadata(np);
        // CanPlay/CanPause/CanSeek flip false->true (and back) alongside
        // PlaybackStatus/Metadata — clients like Plasma's media widget cache
        // the initial GetAll snapshot and only re-read on PropertiesChanged,
        // so omitting these left the controls permanently disabled after the
        // very first (nothing-loaded) snapshot.
        changed[QStringLiteral("CanPlay")] = loaded;
        changed[QStringLiteral("CanPause")] = loaded;
        changed[QStringLiteral("CanSeek")] = loaded;

        QDBusMessage msg = QDBusMessage::createSignal(
            QStringLiteral("/org/mpris/MediaPlayer2"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        msg << QStringLiteral("org.mpris.MediaPlayer2.Player") << changed
            << QStringList{};
        QDBusConnection::sessionBus().send(msg);
    }

    ~Impl()
    {
        if (host)
        {
            QDBusConnection::sessionBus().unregisterObject(
                QStringLiteral("/org/mpris/MediaPlayer2"));
            delete host;
            QDBusConnection::sessionBus().interface()->unregisterService(
                QStringLiteral("org.mpris.MediaPlayer2.Tesseract"));
        }
    }
};

QtMprisPlayer::QtMprisPlayer(tesseract::AccountManager& account_manager)
    : impl_(std::make_unique<Impl>())
{
    impl_->account_manager = &account_manager;

    auto reg = QDBusConnection::sessionBus().interface()->registerService(
        QStringLiteral("org.mpris.MediaPlayer2.Tesseract"),
        QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::DontAllowReplacement);
    if (!reg.isValid() ||
        reg.value() != QDBusConnectionInterface::ServiceRegistered)
    {
        return;
    }

    impl_->host = new QObject();
    new MediaPlayer2Adaptor(impl_->host);
    new MediaPlayer2PlayerAdaptor(impl_->host, &account_manager);
    if (!QDBusConnection::sessionBus().registerObject(
            QStringLiteral("/org/mpris/MediaPlayer2"), impl_->host,
            QDBusConnection::ExportAdaptors))
    {
        delete impl_->host;
        impl_->host = nullptr;
        QDBusConnection::sessionBus().interface()->unregisterService(
            QStringLiteral("org.mpris.MediaPlayer2.Tesseract"));
        return;
    }

    account_manager.media_playback_hub().on_changed = [impl = impl_.get()]
    { impl->emit_properties_changed(); };

    available_ = true;
}

QtMprisPlayer::~QtMprisPlayer()
{
    if (impl_ && impl_->account_manager)
    {
        // Drop the Hub's callback into this (about-to-be-destroyed) Impl —
        // otherwise a later report()/report_stopped() from a surviving
        // window would call into freed memory.
        impl_->account_manager->media_playback_hub().on_changed = nullptr;
    }
}

#include "QtMprisPlayer.moc"
