#pragma once
#include <tesseract/notifier.h>
#include <QDBusInterface>
#include <QObject>
#include <functional>
#include <string>
#include <unordered_map>

class LinuxNotifierQt final : public QObject, public tesseract::INotifier
{
    Q_OBJECT
public:
    explicit LinuxNotifierQt(std::function<void(std::string)> on_activate,
                              QObject* parent = nullptr);
    ~LinuxNotifierQt() override = default;
    void notify(const tesseract::Notification& n) override;

private slots:
    void onActionInvoked(uint id, const QString& action);
    void onNotificationClosed(uint id, uint reason);

private:
    bool use_portal() const;

    QDBusInterface                            iface_;
    QDBusInterface                            portal_;
    std::function<void(std::string)>          on_activate_;
    std::unordered_map<uint32_t, std::string> id_to_room_;
};
