#include "SettingsController.h"

#include <thread>

namespace tesseract
{

SettingsController::SettingsController(
    tesseract::Client* client,
    std::function<void(std::function<void()>)>                          post_to_ui,
    std::function<void(std::function<void(std::vector<uint8_t>,
                                          std::string)>)>               open_file_picker)
    : client_(client)
    , post_to_ui_(std::move(post_to_ui))
    , open_file_picker_(std::move(open_file_picker))
{
}

void SettingsController::set_client(tesseract::Client* client)
{
    client_ = client;
    avatar_in_flight_.store(false);
    name_in_flight_.store(false);
    devices_loading_.store(false);
    {
        std::lock_guard<std::mutex> lock(device_ops_mu_);
        device_ops_in_flight_.clear();
    }
}

bool SettingsController::acquire_device_op_(const std::string& device_id)
{
    std::lock_guard<std::mutex> lock(device_ops_mu_);
    return device_ops_in_flight_.insert(device_id).second;
}

void SettingsController::release_device_op_(const std::string& device_id)
{
    std::lock_guard<std::mutex> lock(device_ops_mu_);
    device_ops_in_flight_.erase(device_id);
}

void SettingsController::upload_avatar()
{
    if (avatar_in_flight_.exchange(true))
        return;

    if (!client_)
    {
        avatar_in_flight_.store(false);
        post_to_ui_([this]()
        {
            if (on_avatar_result)
                on_avatar_result(false, "not logged in");
        });
        return;
    }

    // Guard: if the picker callback is destroyed without being invoked (user
    // cancels the dialog), the RAII cleaner fires and clears the in-flight flag.
    struct FlagCleaner {
        std::atomic<bool>& flag;
        bool armed = true;
        explicit FlagCleaner(std::atomic<bool>& f) : flag(f) {}
        ~FlagCleaner() { if (armed) flag.store(false); }
        void disarm() { armed = false; }
    };
    auto cleaner = std::make_shared<FlagCleaner>(avatar_in_flight_);

    open_file_picker_(
        [this, client_snap = client_, cleaner](std::vector<uint8_t> bytes,
                                                std::string mime) mutable
        {
            // The callback was invoked (cancel sends empty bytes).
            // Disarm the RAII guard — we manage the flag from here.
            cleaner->disarm();

            if (bytes.empty())
            {
                avatar_in_flight_.store(false);
                return;
            }
            if (client_snap != client_)
            {
                avatar_in_flight_.store(false);
                return;
            }
            std::thread(
                [this, client_snap,
                 bytes = std::move(bytes),
                 mime  = std::move(mime)]() mutable
                {
                    auto result = client_snap->upload_avatar(bytes, mime);
                    post_to_ui_(
                        [this, client_snap, result = std::move(result)]()
                        {
                            avatar_in_flight_.store(false);
                            if (client_snap != client_)
                                return;
                            if (on_avatar_result)
                                on_avatar_result(result.ok, result.message);
                            if (result.ok && on_avatar_changed)
                                on_avatar_changed(result.message);
                        });
                })
                .detach();
        });
}

void SettingsController::remove_avatar()
{
    if (avatar_in_flight_.exchange(true))
        return;

    auto* c = client_;

    if (!c)
    {
        // No client: immediately report error via post_to_ui_.
        avatar_in_flight_.store(false);
        post_to_ui_([this]()
        {
            if (on_avatar_result)
                on_avatar_result(false, "not logged in");
        });
        return;
    }

    std::thread(
        [this, c]()
        {
            auto result = c->remove_avatar();
            post_to_ui_(
                [this, c, result = std::move(result)]()
                {
                    avatar_in_flight_.store(false);
                    if (c != client_)
                        return;
                    if (on_avatar_result)
                        on_avatar_result(result.ok, result.message);
                    if (result.ok && on_avatar_changed)
                        on_avatar_changed("");
                });
        })
        .detach();
}

void SettingsController::load_devices()
{
    if (devices_loading_.exchange(true))
        return;

    auto* c = client_;
    if (!c)
    {
        devices_loading_.store(false);
        post_to_ui_([this]()
        {
            if (on_devices_loaded)
                on_devices_loaded({});
        });
        return;
    }

    std::thread(
        [this, c]()
        {
            auto list = c->list_devices();
            post_to_ui_(
                [this, c, list = std::move(list)]() mutable
                {
                    devices_loading_.store(false);
                    if (c != client_)
                        return;
                    if (on_devices_loaded)
                        on_devices_loaded(std::move(list));
                });
        })
        .detach();
}

void SettingsController::rename_device(std::string device_id, std::string name)
{
    if (!acquire_device_op_(device_id))
        return;

    auto* c = client_;
    if (!c)
    {
        release_device_op_(device_id);
        post_to_ui_([this, device_id]() mutable
        {
            if (on_device_renamed)
                on_device_renamed(std::move(device_id), false, "not logged in");
        });
        return;
    }

    std::thread(
        [this, c, device_id = std::move(device_id),
         name = std::move(name)]() mutable
        {
            auto result = c->set_device_display_name(device_id, name);
            post_to_ui_(
                [this, c, device_id = std::move(device_id),
                 result = std::move(result)]() mutable
                {
                    release_device_op_(device_id);
                    if (c != client_)
                        return;
                    if (on_device_renamed)
                        on_device_renamed(std::move(device_id), result.ok,
                                          result.message);
                });
        })
        .detach();
}

void SettingsController::delete_device(std::string device_id)
{
    if (!acquire_device_op_(device_id))
        return;

    auto* c = client_;
    if (!c)
    {
        release_device_op_(device_id);
        post_to_ui_([this, device_id]() mutable
        {
            if (on_device_deleted)
                on_device_deleted(std::move(device_id), false, "not logged in");
        });
        return;
    }

    std::thread(
        [this, c, device_id = std::move(device_id)]() mutable
        {
            auto begin = c->begin_delete_device(device_id);
            post_to_ui_(
                [this, c, device_id = std::move(device_id),
                 begin = std::move(begin)]() mutable
                {
                    if (c != client_)
                    {
                        release_device_op_(device_id);
                        return;
                    }
                    if (!begin.ok)
                    {
                        release_device_op_(device_id);
                        if (on_device_deleted)
                            on_device_deleted(std::move(device_id), false,
                                              std::move(begin.message));
                        return;
                    }
                    if (begin.needs_uia)
                    {
                        // Leave the op slot held — the second call from
                        // confirm_device_deletion will release it. The view is
                        // expected to keep the row in its "awaiting UIA" state.
                        if (on_device_needs_uia)
                            on_device_needs_uia(std::move(device_id),
                                                std::move(begin.fallback_url),
                                                std::move(begin.session));
                        return;
                    }
                    // Clean delete with no UIA challenge (rare but possible).
                    release_device_op_(device_id);
                    if (on_device_deleted)
                        on_device_deleted(std::move(device_id), true, "");
                });
        })
        .detach();
}

void SettingsController::cancel_device_deletion(std::string device_id)
{
    release_device_op_(device_id);
}

void SettingsController::confirm_device_deletion(std::string device_id,
                                                 std::string session)
{
    auto* c = client_;
    if (!c)
    {
        release_device_op_(device_id);
        post_to_ui_([this, device_id]() mutable
        {
            if (on_device_deleted)
                on_device_deleted(std::move(device_id), false, "not logged in");
        });
        return;
    }

    std::thread(
        [this, c, device_id = std::move(device_id),
         session = std::move(session)]() mutable
        {
            auto result = c->complete_delete_device(device_id, session);
            post_to_ui_(
                [this, c, device_id = std::move(device_id),
                 result = std::move(result)]() mutable
                {
                    release_device_op_(device_id);
                    if (c != client_)
                        return;
                    if (on_device_deleted)
                        on_device_deleted(std::move(device_id), result.ok,
                                          result.message);
                });
        })
        .detach();
}

void SettingsController::set_display_name(std::string name)
{
    if (name_in_flight_.exchange(true))
        return;

    auto* c = client_;

    if (!c)
    {
        // No client: immediately report error via post_to_ui_.
        name_in_flight_.store(false);
        post_to_ui_([this]()
        {
            if (on_name_result)
                on_name_result(false, "not logged in");
        });
        return;
    }

    std::thread(
        [this, c, name = std::move(name)]() mutable
        {
            auto result = c->set_display_name(name);
            post_to_ui_(
                [this, c, result = std::move(result),
                 name = std::move(name)]() mutable
                {
                    name_in_flight_.store(false);
                    if (c != client_)
                        return;
                    if (on_name_result)
                        on_name_result(result.ok, result.message);
                    if (result.ok && on_name_changed)
                        on_name_changed(std::move(name));
                });
        })
        .detach();
}

} // namespace tesseract
