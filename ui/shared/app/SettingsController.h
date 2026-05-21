#pragma once

#include "tesseract/client.h"
#include "tesseract/up_connector.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace tesseract
{

class SettingsController
{
public:
    SettingsController(
        tesseract::Client* client,
        std::function<void(std::function<void()>)>                          post_to_ui,
        std::function<void(std::function<void()>)>                          run_async,
        std::function<void(std::function<void(std::vector<uint8_t>,
                                              std::string)>)>               open_file_picker);

    void set_client(tesseract::Client* client);

    tesseract::Client* client() const { return client_; }

    // Bind the per-account UnifiedPush connector so notification-toggle changes
    // can drive `register_pusher` / `remove_pusher`. May be null — passing
    // nullptr is the expected case on Win32 and macOS, where push delivery
    // happens purely through local sync and only the `Settings` flag matters.
    void set_up_connector(tesseract::IUpConnector* connector);

    void upload_avatar();
    void remove_avatar();
    void set_display_name(std::string name);

    // Persist the Notifications toggle and, if an `IUpConnector` was injected,
    // register or unregister this device's pusher with the homeserver. Local
    // OS notifications are gated separately by each shell's
    // `handle_notification_ui_` early-return on `Settings::notifications_enabled`.
    void set_notifications_enabled(bool enabled);

    // Devices / sessions.
    void load_devices();
    void rename_device(std::string device_id, std::string name);
    void delete_device(std::string device_id);
    void confirm_device_deletion(std::string device_id, std::string session);
    // Abandon a pending UIA flow for `device_id` without contacting the
    // homeserver. Releases the per-device in-flight slot held by
    // delete_device's UIA branch so a fresh delete can be initiated later.
    void cancel_device_deletion(std::string device_id);

    std::function<void(bool ok, std::string error)> on_avatar_result;
    std::function<void(bool ok, std::string error)> on_name_result;
    std::function<void(std::string new_mxc_url)>    on_avatar_changed;
    std::function<void(std::string new_name)>        on_name_changed;

    std::function<void(std::vector<tesseract::Client::Device>)> on_devices_loaded;
    std::function<void(std::string device_id, bool ok, std::string error)>
                                                                 on_device_renamed;
    std::function<void(std::string device_id,
                       std::string fallback_url,
                       std::string session)>                     on_device_needs_uia;
    std::function<void(std::string device_id, bool ok, std::string error)>
                                                                 on_device_deleted;

private:
    bool acquire_device_op_(const std::string& device_id);
    void release_device_op_(const std::string& device_id);

    tesseract::Client* client_;
    tesseract::IUpConnector* up_connector_ = nullptr;
    std::function<void(std::function<void()>)>                       post_to_ui_;
    std::function<void(std::function<void()>)>                       run_async_;
    std::function<void(std::function<void(std::vector<uint8_t>,
                                          std::string)>)>            open_file_picker_;

    std::atomic<bool> avatar_in_flight_{false};
    std::atomic<bool> name_in_flight_{false};
    std::atomic<bool> devices_loading_{false};

    std::mutex device_ops_mu_;
    std::set<std::string> device_ops_in_flight_;
};

} // namespace tesseract
