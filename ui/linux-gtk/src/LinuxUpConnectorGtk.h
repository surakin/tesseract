#pragma once
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
    tesseract::Client* client_ = nullptr;
    std::string token_;
    std::string distributor_service_;
    // Last gateway URL derived from a distributor endpoint. Cached so a
    // re-enable after the user toggled notifications off can re-register the
    // pusher without waiting for a fresh distributor callback.
    std::string gateway_url_;
    bool enabled_ = true;
};
