#pragma once
#include <tesseract/up_connector.h>
#include <QObject>
#include <string>

// Forward-declared; defined in .cpp to hide Qt/D-Bus headers from users.
class UpSharedBusQt;

class LinuxUpConnectorQt final : public tesseract::IUpConnector {
public:
    LinuxUpConnectorQt();
    ~LinuxUpConnectorQt() override;

    void start(tesseract::Client* client, const std::string& user_id) override;
    void stop() override;

    // Called by UpSharedBusQt when the distributor fires callbacks for our token.
    void on_new_endpoint(const std::string& endpoint);
    void on_unregistered();

private:
    tesseract::Client* client_              = nullptr;
    std::string        token_;
    std::string        distributor_service_;
};
