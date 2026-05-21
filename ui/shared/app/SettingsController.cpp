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
}

void SettingsController::upload_avatar()
{
    if (avatar_in_flight_.exchange(true))
        return;

    open_file_picker_(
        [this, client_snap = client_](std::vector<uint8_t> bytes,
                                      std::string mime)
        {
            if (bytes.empty())
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
