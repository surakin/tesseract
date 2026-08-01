#include "QtKRunnerPlugin.h"

#include "app/AccountManager.h"
#include "app/SearchBackend.h"

#include <tesseract/visual.h>

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <string>

// ---------------------------------------------------------------------------
// org.kde.krunner1's RemoteMatch/RemoteAction structured D-Bus types
// (signatures "(sssida{sv})" / "(sss)") and their QDBusArgument marshaling —
// standard KRunner-plugin boilerplate, new to this repo.
// ---------------------------------------------------------------------------

struct RemoteMatch
{
    QString id;
    QString text;
    QString iconName;
    int type = 0;
    double relevance = 0.0;
    QVariantMap properties;
};
using RemoteMatches = QList<RemoteMatch>;

struct RemoteAction
{
    QString id;
    QString text;
    QString iconName;
};
using RemoteActions = QList<RemoteAction>;

// Match's "icon-data" property value (iiibiiay): width, height, rowstride,
// has_alpha, bits_per_sample, channels, raw pixel data — the same shape
// org.freedesktop.Notifications' "image-data" hint uses (see
// LinuxNotifier.cpp), per org.kde.krunner1's own introspection XML. KRunner
// prefers this over IconName when present, falling back to IconName only if
// decoding fails, so IconName is left set as a safety net regardless.
struct IconData
{
    int        width = 0;
    int        height = 0;
    int        rowstride = 0;
    bool       hasAlpha = false;
    int        bitsPerSample = 0;
    int        channels = 0;
    QByteArray data;
};

Q_DECLARE_METATYPE(RemoteMatch)
Q_DECLARE_METATYPE(RemoteMatches)
Q_DECLARE_METATYPE(RemoteAction)
Q_DECLARE_METATYPE(RemoteActions)
Q_DECLARE_METATYPE(IconData)

// These must be at namespace scope (not inside an anonymous namespace) —
// qDBusRegisterMetaType<T>()'s internal marshaling templates (QtDBus)
// instantiate `arg << t`/`arg >> t` and rely on ADL finding these operators
// via RemoteMatch/RemoteAction's associated (global) namespace; nested in an
// anonymous namespace, they weren't found by that lookup and every call
// below failed to compile ("no match for operator<<").
QDBusArgument& operator<<(QDBusArgument& arg, const RemoteMatch& m)
{
    arg.beginStructure();
    arg << m.id << m.text << m.iconName << m.type << m.relevance
        << m.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, RemoteMatch& m)
{
    arg.beginStructure();
    arg >> m.id >> m.text >> m.iconName >> m.type >> m.relevance
        >> m.properties;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const RemoteAction& a)
{
    arg.beginStructure();
    arg << a.id << a.text << a.iconName;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, RemoteAction& a)
{
    arg.beginStructure();
    arg >> a.id >> a.text >> a.iconName;
    arg.endStructure();
    return arg;
}

QDBusArgument& operator<<(QDBusArgument& arg, const IconData& d)
{
    arg.beginStructure();
    arg << d.width << d.height << d.rowstride << d.hasAlpha << d.bitsPerSample
        << d.channels << d.data;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, IconData& d)
{
    arg.beginStructure();
    arg >> d.width >> d.height >> d.rowstride >> d.hasAlpha >>
        d.bitsPerSample >> d.channels >> d.data;
    arg.endStructure();
    return arg;
}

namespace
{
// KRunner's Plasma::QueryMatch::Type banding — a title that starts with the
// (whole, untokenised) query is treated as an exact match, everything else
// (topic/alias/mxid substring hits) as merely possible; KRunner's own scorer
// handles finer-grained ranking against other runners' results.
constexpr int kExactMatch = 100;
constexpr int kPossibleMatch = 30;

// Icon-data is decoded (unlike GTK's raw passthrough to GNOME Shell), so
// this path already pays a decode cost — shrink to keep the D-Bus payload
// modest across up to ~10 results per query rather than sending the full
// kAvatarCacheSize (80px) cache entry.
constexpr int kIconDataSize = 32;

// Cache lookup only — Match() must answer synchronously, so a miss (avatar
// never rendered yet elsewhere in the app) just leaves properties without
// "icon-data", falling back to the themed iconName already set below.
QVariant avatar_icon_data(tesseract::AccountManager& account_manager,
                          const std::string& avatar_url)
{
    if (avatar_url.empty())
        return {};
    const std::string tkey = tesseract::visual::thumb_key(
        avatar_url, tesseract::visual::kAvatarCacheSize,
        tesseract::visual::kAvatarCacheSize);
    const auto bytes = account_manager.media_disk_cache().load(tkey);
    if (bytes.empty())
        return {};

    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                          static_cast<int>(bytes.size())))
        return {};
    img = img.convertToFormat(QImage::Format_RGBA8888)
              .scaled(kIconDataSize, kIconDataSize, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
    if (img.isNull())
        return {};

    IconData d;
    d.width = img.width();
    d.height = img.height();
    d.rowstride = static_cast<int>(img.bytesPerLine());
    d.hasAlpha = true;
    d.bitsPerSample = 8;
    d.channels = 4;
    d.data = QByteArray(reinterpret_cast<const char*>(img.constBits()),
                        static_cast<int>(img.sizeInBytes()));
    return QVariant::fromValue(d);
}
} // namespace

// ---------------------------------------------------------------------------
// KRunner1Adaptor — exports org.kde.krunner1 on D-Bus, mirroring the
// QDBusAbstractAdaptor idiom established by LinuxUpConnectorQt's
// UpConnector1Adaptor.
// ---------------------------------------------------------------------------

class KRunner1Adaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.krunner1")
public:
    KRunner1Adaptor(QObject* parent, tesseract::AccountManager* account_manager)
        : QDBusAbstractAdaptor(parent), account_manager_(account_manager)
    {
    }

public slots:
    RemoteMatches Match(const QString& searchTerm)
    {
        RemoteMatches out;
        const std::string term = searchTerm.toStdString();
        for (const auto& r : account_manager_->search_backend().query(term))
        {
            RemoteMatch m;
            m.id = QString::fromStdString(r.id);
            m.text = QString::fromStdString(r.title);
            m.iconName = r.kind == tesseract::SearchBackend::ResultKind::Room
                             ? QStringLiteral("tesseract-matrix")
                             : QStringLiteral("avatar-default");
            const QVariant icon_data =
                avatar_icon_data(*account_manager_, r.avatar_url);
            if (icon_data.isValid())
                m.properties[QStringLiteral("icon-data")] = icon_data;
            const bool exact_prefix =
                m.text.startsWith(searchTerm, Qt::CaseInsensitive);
            m.type = exact_prefix ? kExactMatch : kPossibleMatch;
            m.relevance = exact_prefix ? 1.0 : 0.5;
            out.push_back(std::move(m));
        }
        return out;
    }

    void Run(const QString& matchId, const QString& /*actionId*/)
    {
        const std::string id = matchId.toStdString();
        const auto kind = id.starts_with('@')
                              ? tesseract::SearchBackend::ResultKind::Contact
                              : tesseract::SearchBackend::ResultKind::Room;
        account_manager_->search_backend().activate(id, kind);
    }

    RemoteActions Actions()
    {
        return {}; // no auxiliary actions in v1 — bare Match/Run only
    }

private:
    tesseract::AccountManager* account_manager_;
};

// ---------------------------------------------------------------------------
// QtKRunnerPlugin
// ---------------------------------------------------------------------------

struct QtKRunnerPlugin::Impl
{
    QObject* host = nullptr;

    ~Impl()
    {
        if (host)
        {
            QDBusConnection::sessionBus().unregisterObject(
                QStringLiteral("/krunner"));
            delete host;
            QDBusConnection::sessionBus().interface()->unregisterService(
                QStringLiteral("org.tesseract.qt"));
        }
    }
};

QtKRunnerPlugin::QtKRunnerPlugin(tesseract::AccountManager& account_manager)
    : impl_(std::make_unique<Impl>())
{
    qDBusRegisterMetaType<RemoteMatch>();
    qDBusRegisterMetaType<RemoteMatches>();
    qDBusRegisterMetaType<RemoteAction>();
    qDBusRegisterMetaType<RemoteActions>();
    qDBusRegisterMetaType<IconData>();

    auto reg = QDBusConnection::sessionBus().interface()->registerService(
        QStringLiteral("org.tesseract.qt"),
        QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::DontAllowReplacement);
    if (!reg.isValid() ||
        reg.value() != QDBusConnectionInterface::ServiceRegistered)
    {
        return;
    }

    impl_->host = new QObject();
    new KRunner1Adaptor(impl_->host, &account_manager);
    if (!QDBusConnection::sessionBus().registerObject(
            QStringLiteral("/krunner"), impl_->host,
            QDBusConnection::ExportAdaptors))
    {
        delete impl_->host;
        impl_->host = nullptr;
        QDBusConnection::sessionBus().interface()->unregisterService(
            QStringLiteral("org.tesseract.qt"));
        return;
    }

    available_ = true;
}

QtKRunnerPlugin::~QtKRunnerPlugin() = default;

#include "QtKRunnerPlugin.moc"
