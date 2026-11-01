#include "LinuxUpConnectorQt.h"
#include <tesseract/client.h>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFutureWatcher>
#include <QStringList>
#include <QtConcurrent/QtConcurrent>
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
                 const QString& /*id*/); // out-of-line: UpSharedBusQt is incomplete here
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
            QDBusInterface iface(svc, QStringLiteral("/org/unifiedpush/Distributor"),
                                 QStringLiteral("org.freedesktop.DBus.Introspectable"),
                                 QDBusConnection::sessionBus());
            QDBusReply<QString> xml = iface.call(QStringLiteral("Introspect"));
            if (xml.isValid() &&
                xml.value().contains(QStringLiteral("org.unifiedpush.Distributor1")))
            {
                return svc;
            }
        }
        return {};
    }

    // Non-blocking: runs find_distributor() via the supplied run_async thunk
    // (so the worker is drained on shutdown) and hops the result back to the
    // main thread via post_to_ui. Falls back to QtConcurrent + QFutureWatcher
    // when no thunks are wired (e.g. tests).
    void find_distributor_async(const std::string& token,
                                std::function<void(std::function<void()>)> run_async,
                                std::function<void(std::function<void()>)> post_to_ui)
    {
        std::string tok = token;
        if (run_async && post_to_ui)
        {
            auto pti = post_to_ui;
            run_async(
                [this, tok, pti]()
                {
                    QString dist = find_distributor();
                    if (dist.isEmpty())
                    {
                        return;
                    }
                    pti(
                        [this, tok, dist = dist.toStdString()]()
                        {
                            auto it = routes_.find(tok);
                            if (it == routes_.end())
                            {
                                return; // connector stopped before scan finished
                            }
                            it->second->set_distributor(dist);
                            distributor_register(QString::fromStdString(dist), tok);
                        });
                });
            return;
        }

        auto* watcher = new QFutureWatcher<QString>(this);
        QObject::connect(watcher, &QFutureWatcher<QString>::finished, this,
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
        dist.asyncCall(QStringLiteral("Register"), QStringLiteral("im.gnomos.Tesseract"),
                       QString::fromStdString(token), QStringLiteral("Tesseract"));
    }

    void distributor_unregister(const QString& svc, const std::string& token)
    {
        QDBusInterface dist(svc, QStringLiteral("/org/unifiedpush/Distributor"),
                            QStringLiteral("org.unifiedpush.Distributor1"),
                            QDBusConnection::sessionBus());
        dist.asyncCall(QStringLiteral("Unregister"), QString::fromStdString(token));
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
        QStringLiteral("im.gnomos.Tesseract"), QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::DontAllowReplacement);
    active_ = reg.isValid() && reg.value() == QDBusConnectionInterface::ServiceRegistered;
    if (active_)
    {
        host_ = new QObject(this);
        new UpConnector1Adaptor(host_, this);
        QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/unifiedpush/Connector"),
                                                     host_, QDBusConnection::ExportAdaptors);
    }
    return active_;
}

void UpConnector1Adaptor::Message(const QString& token, const QByteArray& message,
                                  const QString& /*id*/)
{
    bus_->dispatch_message(token, message);
}

void UpConnector1Adaptor::NewEndpoint(const QString& token, const QString& endpoint)
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

LinuxUpConnectorQt::LinuxUpConnectorQt() = default;

void LinuxUpConnectorQt::set_distributor(const std::string& service)
{
    distributor_service_ = service;
}

LinuxUpConnectorQt::~LinuxUpConnectorQt()
{
    stop();
}

void LinuxUpConnectorQt::start(tesseract::Client* client, const std::string& user_id)
{
    if (!core_.begin(client, user_id))
    {
        return; // already started
    }

    UpSharedBusQt& bus = UpSharedBusQt::get();
    if (!bus.acquire())
    {
        core_.end();
        return; // another process owns the bus name
    }

    bus.add_route(core_.token(), this);
    bus.find_distributor_async(core_.token(), run_async_,
                               post_to_ui_); // non-blocking; callback sets distributor
}

void LinuxUpConnectorQt::stop()
{
    if (!core_.active())
    {
        return;
    }
    UpSharedBusQt& bus = UpSharedBusQt::get();
    bus.remove_route(core_.token());
    bus.release();
    distributor_service_.clear();
    core_.end();
}

void LinuxUpConnectorQt::logout()
{
    if (!core_.active())
    {
        return;
    }
    UpSharedBusQt& bus = UpSharedBusQt::get();
    if (!distributor_service_.empty())
    {
        bus.distributor_unregister(QString::fromStdString(distributor_service_), core_.token());
    }
    core_.remove_pusher();
    stop();
}

void LinuxUpConnectorQt::on_new_endpoint(const std::string& endpoint)
{
    core_.on_new_endpoint(endpoint);
}

void LinuxUpConnectorQt::on_unregistered()
{
    if (!core_.active())
    {
        return;
    }
    core_.remove_pusher();
    // Re-register so the distributor issues a fresh endpoint.
    if (!distributor_service_.empty())
    {
        UpSharedBusQt::get().distributor_register(QString::fromStdString(distributor_service_),
                                                  core_.token());
    }
}

void LinuxUpConnectorQt::set_enabled(bool enabled)
{
    core_.set_enabled(enabled);
}

void LinuxUpConnectorQt::on_message(const QByteArray& message)
{
    core_.on_message(
        std::string_view(message.constData(), static_cast<std::size_t>(message.size())));
}

#include "LinuxUpConnectorQt.moc"
