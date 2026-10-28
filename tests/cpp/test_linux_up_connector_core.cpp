#include <catch2/catch_test_macros.hpp>

#include "platform/linux/linux_up_connector_core.h"

using tesseract::up::extract_push_room_id;
using tesseract::up::normalize_gateway_url;
using tesseract::up::sanitize_token;

TEST_CASE("sanitize_token keeps alphanumerics and replaces the rest",
          "[linux_up]")
{
    CHECK(sanitize_token("@alice:example.org") == "_alice_example_org");
    CHECK(sanitize_token("abc123") == "abc123");
    CHECK(sanitize_token("") == "");
}

TEST_CASE("normalize_gateway_url accepts https endpoints", "[linux_up]")
{
    CHECK(normalize_gateway_url("https://push.example.org") ==
          "https://push.example.org/_matrix/push/v1/notify");
    CHECK(normalize_gateway_url("https://push.example.org/") ==
          "https://push.example.org/_matrix/push/v1/notify");
}

TEST_CASE("normalize_gateway_url keeps the port and drops path/query/fragment",
          "[linux_up]")
{
    CHECK(normalize_gateway_url("https://push.example.org:8443/up?x=1#f") ==
          "https://push.example.org:8443/_matrix/push/v1/notify");
    CHECK(normalize_gateway_url("https://push.example.org?x=1") ==
          "https://push.example.org/_matrix/push/v1/notify");
}

TEST_CASE("normalize_gateway_url accepts boundary ports and IPv6 literals",
          "[linux_up]")
{
    CHECK(normalize_gateway_url("https://push.example.org:65535/up") ==
          "https://push.example.org:65535/_matrix/push/v1/notify");
    CHECK(normalize_gateway_url("https://[::1]:8443/up") ==
          "https://[::1]:8443/_matrix/push/v1/notify");
}

TEST_CASE("normalize_gateway_url accepts an uppercase scheme", "[linux_up]")
{
    CHECK(normalize_gateway_url("HTTPS://push.example.org/up") ==
          "https://push.example.org/_matrix/push/v1/notify");
}

TEST_CASE("normalize_gateway_url accepts underscores in reg-name hosts",
          "[linux_up]")
{
    CHECK(normalize_gateway_url("https://my_host.lan/up") ==
          "https://my_host.lan/_matrix/push/v1/notify");
}

TEST_CASE("normalize_gateway_url rejects untrusted endpoints", "[linux_up]")
{
    CHECK_FALSE(normalize_gateway_url("http://push.example.org/up"));
    CHECK_FALSE(normalize_gateway_url("https:///up"));
    CHECK_FALSE(normalize_gateway_url("https://"));
    CHECK_FALSE(normalize_gateway_url("https://:8443/up"));
    CHECK_FALSE(normalize_gateway_url("https://user:pw@push.example.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.example.org/u p"));
    CHECK_FALSE(normalize_gateway_url("https://push.exa\nmple.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.example.org:80x/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.example.org:0/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.example.org:65536/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.example.org:99999/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.exa%mple.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.exa<mple.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://push.exa\"mple.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://push..example.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://-push.example.org/up"));
    CHECK_FALSE(normalize_gateway_url("https://[::1/up"));
    CHECK_FALSE(normalize_gateway_url("https://[zz::1]/up"));
    CHECK_FALSE(normalize_gateway_url("https://[abcd]/up"));
    CHECK_FALSE(normalize_gateway_url("javascript:alert(1)"));
    CHECK_FALSE(normalize_gateway_url(""));
}

TEST_CASE("extract_push_room_id reads notification.room_id", "[linux_up]")
{
    CHECK(extract_push_room_id(
              R"({"notification":{"room_id":"!abc:example.org","counts":{}}})") ==
          "!abc:example.org");
}

TEST_CASE("extract_push_room_id rejects malformed payloads", "[linux_up]")
{
    CHECK_FALSE(extract_push_room_id(R"({"other":1})"));
    CHECK_FALSE(extract_push_room_id(R"({"notification":{}})"));
    CHECK_FALSE(extract_push_room_id(R"({"notification":"x"})"));
    CHECK_FALSE(extract_push_room_id(R"({"notification":{"room_id":42}})"));
    CHECK_FALSE(extract_push_room_id(R"({"notification":{"room_id":""}})"));
    CHECK_FALSE(extract_push_room_id("not json"));
    CHECK_FALSE(extract_push_room_id(""));
}
