#include "QtKRunnerPlugin.h"

#include "app/AccountManager.h"
#include "app/SearchBackend.h"

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
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

Q_DECLARE_METATYPE(RemoteMatch)
Q_DECLARE_METATYPE(RemoteMatches)
Q_DECLARE_METATYPE(RemoteAction)
Q_DECLARE_METATYPE(RemoteActions)

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

namespace
{
// KRunner's Plasma::QueryMatch::Type banding — a title that starts with the
// (whole, untokenised) query is treated as an exact match, everything else
// (topic/alias/mxid substring hits) as merely possible; KRunner's own scorer
// handles finer-grained ranking against other runners' results.
constexpr int kExactMatch = 100;
constexpr int kPossibleMatch = 30;
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
