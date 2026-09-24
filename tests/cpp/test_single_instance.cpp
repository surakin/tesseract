#include <catch2/catch_test_macros.hpp>

// single_instance.cpp is compiled into tesseract_tk for Linux and macOS only.
#if !defined(_WIN32)

#include "tk/single_instance.h"

TEST_CASE("activation payload: round trip")
{
    tk::ActivationRequest req{"tok", "matrix:r/room:example.org",
                              "open-room", "!abc:example.org"};
    const auto back = tk::parse_activation_payload(tk::format_activation_payload(req));
    CHECK(back.token == req.token);
    CHECK(back.uri == req.uri);
    CHECK(back.action == req.action);
    CHECK(back.room_id == req.room_id);
}

TEST_CASE("activation payload: legacy two-line form")
{
    const auto req = tk::parse_activation_payload("tok\nmatrix:u/a:b\n");
    CHECK(req.token == "tok");
    CHECK(req.uri == "matrix:u/a:b");
    CHECK(req.action.empty());
    CHECK(req.room_id.empty());
}

TEST_CASE("activation payload: empty and unterminated input")
{
    CHECK(tk::parse_activation_payload("").token.empty());
    const auto req = tk::parse_activation_payload("\n\nopen-settings");
    CHECK(req.token.empty());
    CHECK(req.uri.empty());
    CHECK(req.action == "open-settings");
}

TEST_CASE("activation payload: embedded newlines cannot inject fields")
{
    tk::ActivationRequest req{"", "matrix:u/a:b\nopen-settings", "", ""};
    const auto back = tk::parse_activation_payload(tk::format_activation_payload(req));
    CHECK(back.uri == "matrix:u/a:bopen-settings");
    CHECK(back.action.empty());
}

#endif
