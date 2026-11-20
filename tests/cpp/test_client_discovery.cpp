// Pure parsing of the homeserver discovery JSON (no network).

#include <catch2/catch_test_macros.hpp>

#include <tesseract/client.h>

using tesseract::Client;

TEST_CASE("Client::parse_discovery_json reads SSO providers", "[client][discovery]")
{
    const auto r = Client::parse_discovery_json(R"({
        "base_url": "https://hs.example.org",
        "error": "",
        "supports_password": true,
        "supports_sso": true,
        "identity_providers": [
            {"id": "oidc-github", "name": "GitHub", "icon": "mxc://hs/abc", "brand": "github"},
            {"id": "saml", "name": "Corp", "icon": "", "brand": ""}
        ]
    })");
    CHECK(r.ok);
    CHECK(r.base_url == "https://hs.example.org");
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    CHECK(r.supports_password);
    CHECK(r.supports_sso);
    REQUIRE(r.identity_providers.size() == 2);
    CHECK(r.identity_providers[0].id == "oidc-github");
    CHECK(r.identity_providers[0].name == "GitHub");
    CHECK(r.identity_providers[0].icon_mxc == "mxc://hs/abc");
    CHECK(r.identity_providers[0].brand == "github");
    CHECK(r.identity_providers[1].id == "saml");
    CHECK(r.identity_providers[1].icon_mxc.empty());
#endif
}

TEST_CASE("Client::parse_discovery_json reads legacy JSON without SSO keys", "[client][discovery]")
{
    const auto r = Client::parse_discovery_json(
        R"({"base_url": "https://hs.example.org", "error": "", "supports_password": true})");
    CHECK(r.ok);
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    CHECK_FALSE(r.supports_sso);
    CHECK(r.identity_providers.empty());
#endif
}
