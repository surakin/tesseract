#pragma once
#include "platform/linux/linux_up_connector_core.h"
#include <tesseract/up_connector.h>
#include <gio/gio.h>
#include <string>

class LinuxUpConnectorGtk final : public tesseract::IUpConnector
{
public:
    LinuxUpConnectorGtk();
    ~LinuxUpConnectorGtk() override;

    void start(tesseract::Client* client, const std::string& user_id) override;
    void stop() override;
    void logout() override;
    void set_enabled(bool enabled) override;

    // Called by the shared GDBus vtable on the UI thread.
    void on_new_endpoint(const std::string& endpoint);
    void on_unregistered();
    void on_message(const guint8* data, gsize len);

private:
    tesseract::up::PusherCore core_;
    std::string distributor_service_;
};
