#include <catch2/catch_test_macros.hpp>

#include "views/login_brand_icons.h"

using tesseract::views::brand_icon_svg;

TEST_CASE("brand_icon_svg maps the spec brands", "[login][sso]")
{
    for (const char* brand : {"apple", "facebook", "github", "gitlab", "google", "twitter"})
    {
        INFO(brand);
        CHECK_FALSE(brand_icon_svg(brand).empty());
    }
    CHECK(brand_icon_svg("").empty());
    CHECK(brand_icon_svg("keycloak").empty());
    CHECK(brand_icon_svg("GitHub").empty());
}
