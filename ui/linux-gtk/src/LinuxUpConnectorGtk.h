#pragma once
#include <tesseract/up_connector.h>
#include <gio/gio.h>
#include <string>

class LinuxUpConnectorGtk final : public tesseract::IUpConnector {
public:
    LinuxUpConnectorGtk();
    ~LinuxUpConnectorGtk() override;

    void start(tesseract::Client* client, const std::string& user_id) override;
    void stop() override;
    void logout() override;

    // Called by the shared GDBus vtable on the UI thread.
    void on_new_endpoint(const std::string& endpoint);
    void on_unregistered();

private:
    tesseract::Client* client_              = nullptr;
    std::string        token_;
    std::string        distributor_service_;
};
