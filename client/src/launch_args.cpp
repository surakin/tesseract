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
