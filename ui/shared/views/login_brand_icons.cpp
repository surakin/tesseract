#include "login_brand_icons.h"

#include "icons.h"

namespace tesseract::views
{

std::span<const std::uint8_t> brand_icon_svg(std::string_view brand)
{
    if (brand == "apple")
    {
        return kBrandAppleSvg;
    }
    if (brand == "facebook")
    {
        return kBrandFacebookSvg;
    }
    if (brand == "github")
    {
        return kBrandGithubSvg;
    }
    if (brand == "gitlab")
    {
        return kBrandGitlabSvg;
    }
    if (brand == "google")
    {
        return kBrandGoogleSvg;
    }
    if (brand == "twitter")
    {
        return kBrandXSvg;
    }
    return {};
}

} // namespace tesseract::views
