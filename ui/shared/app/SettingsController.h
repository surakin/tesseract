#pragma once

#include "tesseract/client.h"

#include <atomic>
#include <functional>
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
        std::function<void(std::function<void(std::vector<uint8_t>,
                                              std::string)>)>               open_file_picker);

    void set_client(tesseract::Client* client);

    void upload_avatar();
    void remove_avatar();
    void set_display_name(std::string name);

    std::function<void(bool ok, std::string error)> on_avatar_result;
    std::function<void(bool ok, std::string error)> on_name_result;
    std::function<void(std::string new_mxc_url)>    on_avatar_changed;
    std::function<void(std::string new_name)>        on_name_changed;

private:
    tesseract::Client* client_;
    std::function<void(std::function<void()>)>                       post_to_ui_;
    std::function<void(std::function<void(std::vector<uint8_t>,
                                          std::string)>)>            open_file_picker_;

    std::atomic<bool> avatar_in_flight_{false};
    std::atomic<bool> name_in_flight_{false};
};

} // namespace tesseract
