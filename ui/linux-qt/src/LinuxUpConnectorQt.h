#pragma once
#include <tesseract/up_connector.h>
#include <QObject>
#include <functional>
#include <string>

// Forward-declared; defined in .cpp to hide Qt/D-Bus headers from users.
class UpSharedBusQt;

class LinuxUpConnectorQt final : public tesseract::IUpConnector
{
public:
    LinuxUpConnectorQt();
    ~LinuxUpConnectorQt() override;

    void start(tesseract::Client* client, const std::string& user_id) override;
    void stop() override;
    void logout() override;
    void set_enabled(bool enabled) override;

    void set_run_async(std::function<void(std::function<void()>)> fn) override
    {
        run_async_ = std::move(fn);
    }
    void set_post_to_ui(std::function<void(std::function<void()>)> fn) override
    {
        post_to_ui_ = std::move(fn);
    }

    // Called by UpSharedBusQt when the distributor fires callbacks for our token.
    void set_distributor(const std::string& service);
    void on_new_endpoint(const std::string& endpoint);
    void on_unregistered();
    void on_message(const QByteArray& message);

private:
    tesseract::Client* client_ = nullptr;
    std::string token_;
    std::string distributor_service_;
    // Last gateway URL derived from a distributor endpoint. Cached so a
    // re-enable after the user toggled notifications off can re-register the
    // pusher without waiting for a fresh distributor callback.
    std::string gateway_url_;
    bool enabled_ = true;
    std::function<void(std::function<void()>)> run_async_;
    std::function<void(std::function<void()>)> post_to_ui_;
};
