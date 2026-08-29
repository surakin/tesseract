#include "tesseract/launch_args.h"

#include "tesseract/client.h"

#include <string_view>
#include <utility>

namespace tesseract
{

LaunchArgs parse_launch_args(const std::vector<std::string>& args)
{
    LaunchArgs result;

    for (const auto& arg : args)
    {
        if (arg == "--autostart")
        {
            result.autostart = true;
            continue;
        }

        if (result.action == LaunchAction::None)
        {
            constexpr std::string_view room_prefix = "--open-room=";
            if (arg.starts_with(room_prefix) && arg.size() > room_prefix.size())
            {
                result.action = LaunchAction::Room;
                result.room_id = arg.substr(room_prefix.size());
                continue;
            }
            if (arg == "--open-quick-switcher")
            {
                result.action = LaunchAction::QuickSwitcher;
                continue;
            }
            if (arg == "--open-message-search")
            {
                result.action = LaunchAction::MessageSearch;
                continue;
            }
            if (arg == "--open-settings")
            {
                result.action = LaunchAction::Settings;
                continue;
            }
        }

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
        constexpr std::string_view screenshot_prefix = "--screenshot-dir=";
        if (arg.starts_with(screenshot_prefix))
        {
            auto path = arg.substr(screenshot_prefix.size());
            if (!path.empty())
                result.screenshot_dir = std::move(path);
            continue;
        }
#endif

        if (!result.matrix_uri &&
            Client::parse_matrix_link(arg).kind !=
                Client::MatrixLink::Kind::Unknown)
        {
            result.matrix_uri = arg;
        }
    }

    return result;
}

} // namespace tesseract
