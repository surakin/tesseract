#include "LinuxUpConnectorQt.h"
#include <tesseract/client.h>
#include <QDBusAbstractAdaptor>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFutureWatcher>
#include <QStringList>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>
#include <cctype>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class UpSharedBusQt;

// ---------------------------------------------------------------------------
// UpConnector1Adaptor — exports org.unifiedpush.Connector1 on D-Bus.
// Defined before UpSharedBusQt so acquire() can instantiate it.
// ---------------------------------------------------------------------------

class UpConnector1Adaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.unifiedpush.Connector1")
public:
    explicit UpConnector1Adaptor(QObject* parent, UpSharedBusQt* bus)
        : QDBusAbstractAdaptor(parent), bus_(bus)
    {
    }

public slots:
    void Message(const QString& token, const QByteArray& message,
                 const QString& /*id*/);  // out-of-line: UpSharedBusQt is incomplete here
    void NewEndpoint(const QString& token, const QString& endpoint);
    void Unregistered(const QString& token);

private:
    UpSharedBusQt* bus_;
};

// ---------------------------------------------------------------------------
// UpSharedBusQt — process singleton.
//
// Owns the D-Bus service name "im.gnomos.Tesseract" and the Connector1
// object at /org/unifiedpush/Connector.  Routes distributor callbacks to
// the correct per-account LinuxUpConnectorQt by token.
// ---------------------------------------------------------------------------

class UpSharedBusQt : public QObject
{
    Q_OBJECT
public:
    static UpSharedBusQt& get()
    {
        static UpSharedBusQt inst;
        return inst;
    }

    bool acquire();

    void release()
    {
        if (--ref_ <= 0)
        {
            ref_ = 0;
            if (active_)
            {
                QDBusConnection::sessionBus().unregisterObject(
                    QStringLiteral("/org/unifiedpush/Connector"));
                QDBusConnection::sessionBus().interface()->unregisterService(
                    QStringLiteral("im.gnomos.Tesseract"));
                delete host_;
                host_ = nullptr;
                active_ = false;
            }
        }
    }

    void add_route(const std::string& token, LinuxUpConnectorQt* conn)
    {
        routes_[token] = conn;
    }
    void remove_route(const std::string& token)
    {
        routes_.erase(token);
    }

    // Scan the session bus for the first service exposing Distributor1.
    // Safe to call from a worker thread — uses the thread's own D-Bus connection.
    QString find_distributor()
    {
        QDBusReply<QStringList> names =
            QDBusConnection::sessionBus().interface()->registeredServiceNames();
        if (!names.isValid())
        {
            return {};
        }
        for (const QString& svc : names.value())
        {
            if (svc.startsWith(QChar(':')))
            {
                continue; // skip unique names
            }
            QDBusInterface iface(
                svc, QStringLiteral("/org/unifiedpush/Distributor"),
                QStringLiteral("org.freedesktop.DBus.Introspectable"),
                QDBusConnection::sessionBus());
            QDBusReply<QString> xml = iface.call(QStringLiteral("Introspect"));
            if (xml.isValid() && xml.value().contains(QStringLiteral(
                                     "org.unifiedpush.Distributor1")))
            {
                return svc;
            }
        }
        return {};
    }

    // Non-blocking: runs find_distributor() on a thread pool worker and delivers
    // the result via QFutureWatcher::finished back on the main thread.
    void find_distributor_async(const std::string& token)
    {
        auto* watcher = new QFutureWatcher<QString>(this);
        std::string tok = token;
        QObject::connect(
            watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, tok]()
            {
                QString dist = watcher->result();
                watcher->deleteLater();
                if (dist.isEmpty())
                {
                    return;
                }
                auto it = routes_.find(tok);
                if (it == routes_.end())
                {
                    return; // connector stopped before scan finished
                }
                it->second->set_distributor(dist.toStdString());
                distributor_register(dist, tok);
            });
        watcher->setFuture(QtConcurrent::run(
            [this]()
            {
                return find_distributor();
            }));
    }

    void distributor_register(const QString& svc, const std::string& token)
    {
        QDBusInterface dist(svc, QStringLiteral("/org/unifiedpush/Distributor"),
                            QStringLiteral("org.unifiedpush.Distributor1"),
                            QDBusConnection::sessionBus());
        dist.asyncCall(
            QStringLiteral("Register"), QStringLiteral("im.gnomos.Tesseract"),
            QString::fromStdString(token), QStringLiteral("Tesseract"));
    }

    void distributor_unregister(const QString& svc, const std::string& token)
    {
        QDBusInterface dist(svc, QStringLiteral("/org/unifiedpush/Distributor"),
                            QStringLiteral("org.unifiedpush.Distributor1"),
                            QDBusConnection::sessionBus());
        dist.asyncCall(QStringLiteral("Unregister"),
                       QString::fromStdString(token));
    }

    void dispatch_new_endpoint(const QString& token, const QString& endpoint)
    {
        auto it = routes_.find(token.toStdString());
        if (it != routes_.end())
        {
            it->second->on_new_endpoint(endpoint.toStdString());
        }
    }
    void dispatch_unregistered(const QString& token)
    {
        auto it = routes_.find(token.toStdString());
        if (it != routes_.end())
        {
            it->second->on_unregistered();
        }
    }
    void dispatch_message(const QString& token, const QByteArray& message)
    {
        auto it = routes_.find(token.toStdString());
        if (it != routes_.end())
        {
            it->second->on_message(message);
        }
    }

private:
    UpSharedBusQt() = default;

    int ref_ = 0;
    bool active_ = false;
    QObject* host_ = nullptr;
    std::unordered_map<std::string, LinuxUpConnectorQt*> routes_;
};

// ---------------------------------------------------------------------------
// Out-of-line definitions that require both class bodies to be complete.
// ---------------------------------------------------------------------------

bool UpSharedBusQt::acquire()
{
    if (ref_++ > 0)
    {
        return active_;
    }
    auto reg = QDBusConnection::sessionBus().interface()->registerService(
        QStringLiteral("im.gnomos.Tesseract"),
        QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::DontAllowReplacement);
    active_ = reg.isValid() &&
              reg.value() == QDBusConnectionInterface::ServiceRegistered;
    if (active_)
    {
        host_ = new QObject(this);
        new UpConnector1Adaptor(host_, this);
        QDBusConnection::sessionBus().registerObject(
            QStringLiteral("/org/unifiedpush/Connector"), host_,
            QDBusConnection::ExportAdaptors);
    }
    return active_;
}

void UpConnector1Adaptor::Message(const QString& token,
                                   const QByteArray& message,
                                   const QString& /*id*/)
{
    bus_->dispatch_message(token, message);
}

void UpConnector1Adaptor::NewEndpoint(const QString& token,
                                      const QString& endpoint)
{
    bus_->dispatch_new_endpoint(token, endpoint);
}

void UpConnector1Adaptor::Unregistered(const QString& token)
{
    bus_->dispatch_unregistered(token);
}

// ---------------------------------------------------------------------------
// LinuxUpConnectorQt
// ---------------------------------------------------------------------------

static std::string sanitize_token(const std::string& user_id)
{
    std::string t;
    t.reserve(user_id.size());
    for (char c : user_id)
    {
        t += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return t;
}

LinuxUpConnectorQt::LinuxUpConnectorQt() = default;

void LinuxUpConnectorQt::set_distributor(const std::string& service)
{
    distributor_service_ = service;
}

LinuxUpConnectorQt::~LinuxUpConnectorQt()
{
    stop();
}

void LinuxUpConnectorQt::start(tesseract::Client* client,
                               const std::string& user_id)
{
    if (client_)
    {
        return; // already started
    }
    client_ = client;
    token_ = sanitize_token(user_id);

    UpSharedBusQt& bus = UpSharedBusQt::get();
    if (!bus.acquire())
    {
        return; // another process owns the bus name
    }

    bus.add_route(token_, this);
    bus.find_distributor_async(
        token_); // non-blocking; callback sets distributor
}

void LinuxUpConnectorQt::stop()
{
    if (!client_)
    {
        return;
    }
    UpSharedBusQt& bus = UpSharedBusQt::get();
    bus.remove_route(token_);
    bus.release();
    distributor_service_.clear();
    client_ = nullptr;
}

void LinuxUpConnectorQt::logout()
{
    if (!client_)
    {
        return;
    }
    UpSharedBusQt& bus = UpSharedBusQt::get();
    if (!distributor_service_.empty())
    {
        bus.distributor_unregister(QString::fromStdString(distributor_service_),
                                   token_);
    }
    client_->remove_pusher(token_, "im.gnomos.tesseract");
    stop();
}

void LinuxUpConnectorQt::on_new_endpoint(const std::string& endpoint)
{
    if (!client_)
    {
        return;
    }
    // The endpoint string is supplied by the UnifiedPush distributor over
    // D-Bus — untrusted. Reject anything that isn't a valid https URL before
    // registering it as this account's Matrix push gateway, otherwise a
    // malicious/buggy distributor could redirect push traffic anywhere.
    QUrl url(QString::fromStdString(endpoint), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
        url.host().isEmpty())
    {
        return;
    }
    // Matrix HTTP pushers require the URL path to be /_matrix/push/v1/notify.
    // By UP convention the push provider exposes a Matrix gateway at that path.
    url.setPath(QStringLiteral("/_matrix/push/v1/notify"));
    url.setQuery(QString{});
    url.setFragment(QString{});
    client_->register_pusher(token_, "im.gnomos.tesseract", "Tesseract",
                             "Linux Desktop", url.toString().toStdString(),
                             "en");
}

void LinuxUpConnectorQt::on_unregistered()
{
    if (!client_)
    {
        return;
    }
    client_->remove_pusher(token_, "im.gnomos.tesseract");
    // Re-register so the distributor issues a fresh endpoint.
    if (!distributor_service_.empty())
    {
        UpSharedBusQt::get().distributor_register(
            QString::fromStdString(distributor_service_), token_);
    }
}

void LinuxUpConnectorQt::on_message(const QByteArray& message)
{
    if (!client_)
    {
        return;
    }
    const auto doc = QJsonDocument::fromJson(message);
    const auto room_id = doc["notification"]["room_id"].toString();
    if (!room_id.isEmpty())
    {
        client_->hint_push_room(room_id.toStdString());
    }
}

#include "LinuxUpConnectorQt.moc"
