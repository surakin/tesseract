#include "tesseract/launch_args.h"

#include "tesseract/client.h"

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
