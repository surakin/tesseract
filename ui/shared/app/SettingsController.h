#pragma once

#include "tesseract/client.h"
#include "tesseract/image_pack.h"
#include "tesseract/up_connector.h"
#include "views/settings/UserPackEditor.h"

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

    // Emojis & Stickers (global image packs).
    void load_image_packs();
    void save_user_pack_changes(tesseract::views::UserPackEditor::Result diff);
    void set_pack_subscribed(std::string room_id, std::string state_key,
                             bool subscribed);

    std::function<void(std::vector<tesseract::ImagePack>)> on_image_packs_loaded;
    std::function<void(std::vector<tesseract::ImagePackImage>)>
        on_user_pack_images_loaded;
    std::function<void(bool ok, std::string error)> on_user_pack_save_result;

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

    // ── Room key export / import ─────────────────────────────────────────────
    // These callbacks must be set by the platform shell before calling
    // export_room_keys() / import_room_keys(). Each receives a completion
    // callback it must invoke with the user's input (or call with an empty
    // string to signal cancellation).

    // Show a native password-entry dialog with `title`. Invoke `cb` with the
    // entered passphrase, or with an empty string on cancel.
    std::function<void(std::string                          title,
                       std::function<void(std::string)>    cb)>
        show_passphrase_prompt;

    // Show a native save-file dialog with `suggested_name` as the default
    // filename. Invoke `cb` with the chosen path, or empty on cancel.
    std::function<void(std::string                          suggested_name,
                       std::function<void(std::string)>    cb)>
        show_save_file_dialog;

    // Show a native open-file dialog. Invoke `cb` with the chosen path, or
    // empty on cancel.
    std::function<void(std::function<void(std::string)>    cb)>
        show_open_file_dialog;

    // Fired on the UI thread with the result of export_room_keys().
    std::function<void(bool ok, std::string error)> on_export_keys_result;
    // Fired on the UI thread with the result of import_room_keys().
    std::function<void(bool ok, std::string error)> on_import_keys_result;

    // Kick off the export flow: passphrase → save-file dialog → async SDK call.
    void export_room_keys();
    // Kick off the import flow: open-file dialog → passphrase → async SDK call.
    void import_room_keys();

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
    std::atomic<bool> image_packs_loading_{false};

    std::mutex device_ops_mu_;
    std::set<std::string> device_ops_in_flight_;
};

} // namespace tesseract
