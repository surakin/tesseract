// MSC4391 in-room bot commands: FFI struct conversion (ffi_convert.h's
// parse_param_schema/from_ffi), Client::match_bot_command_arguments's
// positional-matching convention, and views/SlashCommands.cpp's
// dispatch_bot_command_send coercion + error paths.

#include <catch2/catch_test_macros.hpp>

#include "views/SlashCommands.h"
#include "ffi_convert.h"
#include <tesseract/bot_command.h>
#include <tesseract/client.h>
#include "tesseract_sdk_bridge_cxx/bridge.h"

#include <nlohmann/json.hpp>

using tesseract::CommandDescription;
using tesseract::CommandParameter;
using tesseract::ParamLiteralType;
using tesseract::ParamPrimitiveType;
using tesseract::ParamSchema;
using tesseract::ParamSchemaKind;

// ---------------------------------------------------------------------------
// parse_param_schema
// ---------------------------------------------------------------------------

TEST_CASE("parse_param_schema decodes every primitive subtype", "[bot_commands][schema]")
{
    auto decode = [](const char* type)
    {
        return tesseract::parse_param_schema(
            nlohmann::json{{"schema_type", "primitive"}, {"type", type}});
    };
    CHECK(decode("string").primitive_type == ParamPrimitiveType::String);
    CHECK(decode("integer").primitive_type == ParamPrimitiveType::Integer);
    CHECK(decode("boolean").primitive_type == ParamPrimitiveType::Boolean);
    CHECK(decode("user_id").primitive_type == ParamPrimitiveType::UserId);
    CHECK(decode("server_name").primitive_type == ParamPrimitiveType::ServerName);
    CHECK(decode("room_alias").primitive_type == ParamPrimitiveType::RoomAlias);
    for (const char* t : {"string", "integer", "boolean", "user_id", "server_name", "room_alias"})
    {
        CHECK(decode(t).kind == ParamSchemaKind::Primitive);
    }
}

TEST_CASE("parse_param_schema rejects an unrecognized primitive type", "[bot_commands][schema]")
{
    auto s = tesseract::parse_param_schema(
        nlohmann::json{{"schema_type", "primitive"}, {"type", "float"}});
    REQUIRE(s.kind == ParamSchemaKind::Invalid);
}

TEST_CASE("parse_param_schema decodes room_id/event_id primitives", "[bot_commands][schema]")
{
    auto room = tesseract::parse_param_schema(
        nlohmann::json{{"schema_type", "primitive"}, {"type", "room_id"}});
    REQUIRE(room.kind == ParamSchemaKind::Primitive);
    REQUIRE(room.primitive_type == ParamPrimitiveType::RoomId);

    auto ev = tesseract::parse_param_schema(
        nlohmann::json{{"schema_type", "primitive"}, {"type", "event_id"}});
    REQUIRE(ev.kind == ParamSchemaKind::Primitive);
    REQUIRE(ev.primitive_type == ParamPrimitiveType::EventId);
}

TEST_CASE("parse_param_schema maps legacy object schemas to primitives", "[bot_commands][schema]")
{
    auto room = tesseract::parse_param_schema(
        nlohmann::json{{"schema_type", "object"}, {"type", "room_id"}});
    REQUIRE(room.kind == ParamSchemaKind::Primitive);
    REQUIRE(room.primitive_type == ParamPrimitiveType::RoomId);

    auto ev = tesseract::parse_param_schema(
        nlohmann::json{{"schema_type", "object"}, {"type", "event_id"}});
    REQUIRE(ev.kind == ParamSchemaKind::Primitive);
    REQUIRE(ev.primitive_type == ParamPrimitiveType::EventId);
}

TEST_CASE("parse_param_schema decodes an array of primitives", "[bot_commands][schema]")
{
    auto s = tesseract::parse_param_schema(nlohmann::json{
        {"schema_type", "array"},
        {"items", {{"schema_type", "primitive"}, {"type", "user_id"}}},
    });
    REQUIRE(s.kind == ParamSchemaKind::Array);
    REQUIRE(s.array_item != nullptr);
    REQUIRE(s.array_item->kind == ParamSchemaKind::Primitive);
    REQUIRE(s.array_item->primitive_type == ParamPrimitiveType::UserId);
}

TEST_CASE("parse_param_schema decodes an array of a union", "[bot_commands][schema]")
{
    auto s = tesseract::parse_param_schema(nlohmann::json{
        {"schema_type", "array"},
        {"items",
         {{"schema_type", "union"},
          {"variants",
           nlohmann::json::array({nlohmann::json{{"schema_type", "primitive"}, {"type", "string"}},
                                  nlohmann::json{{"schema_type", "literal"},
                                                 {"value", "yes"},
                                                 {"literal_type", "string"}}})}}},
    });
    REQUIRE(s.kind == ParamSchemaKind::Array);
    REQUIRE(s.array_item->kind == ParamSchemaKind::Union);
    REQUIRE(s.array_item->union_variants.size() == 2);
    CHECK(s.array_item->union_variants[0].kind == ParamSchemaKind::Primitive);
    CHECK(s.array_item->union_variants[1].kind == ParamSchemaKind::Literal);
}

TEST_CASE("parse_param_schema decodes a literal and keeps its raw JSON value",
          "[bot_commands][schema]")
{
    auto s = tesseract::parse_param_schema(
        nlohmann::json{{"schema_type", "literal"}, {"value", 42}, {"literal_type", "integer"}});
    REQUIRE(s.kind == ParamSchemaKind::Literal);
    REQUIRE(s.literal_type == ParamLiteralType::Integer);
    REQUIRE(s.literal_value_json == "42");
}

TEST_CASE("parse_param_schema marks a non-object as invalid", "[bot_commands][schema]")
{
    REQUIRE(tesseract::parse_param_schema(nlohmann::json("not an object")).kind ==
            ParamSchemaKind::Invalid);
}

TEST_CASE("parse_param_schema marks an unknown schema_type as invalid", "[bot_commands][schema]")
{
    REQUIRE(tesseract::parse_param_schema(nlohmann::json{{"schema_type", "wat"}}).kind ==
            ParamSchemaKind::Invalid);
}

// ---------------------------------------------------------------------------
// from_ffi(CommandParameterFfi) / from_ffi(CommandDescriptionFfi)
// ---------------------------------------------------------------------------

TEST_CASE("from_ffi decodes a CommandParameterFfi's schema_json and flat fields",
          "[bot_commands][ffi]")
{
    tesseract_ffi::CommandParameterFfi p{};
    p.key = "timeout_seconds";
    p.schema_json = R"({"schema_type":"primitive","type":"integer"})";
    p.description_text = "The timeout in seconds";
    p.optional = true;

    CommandParameter out = tesseract::from_ffi(p);
    CHECK(out.key == "timeout_seconds");
    CHECK(out.description == "The timeout in seconds");
    CHECK(out.optional);
    REQUIRE(out.schema.kind == ParamSchemaKind::Primitive);
    CHECK(out.schema.primitive_type == ParamPrimitiveType::Integer);
}

TEST_CASE("from_ffi falls back to Invalid schema on malformed schema_json", "[bot_commands][ffi]")
{
    tesseract_ffi::CommandParameterFfi p{};
    p.key = "x";
    p.schema_json = "{not json";
    CommandParameter out = tesseract::from_ffi(p);
    CHECK(out.schema.kind == ParamSchemaKind::Invalid);
}

TEST_CASE("from_ffi decodes a full CommandDescriptionFfi", "[bot_commands][ffi]")
{
    tesseract_ffi::CommandDescriptionFfi d{};
    d.command = "ban";
    d.sender = "@draupnir:example.org";
    d.sender_display_name = "Draupnir";
    d.state_key = "state1";
    d.room_id = "!room:example.org";
    d.description_text = "Ban a user";
    d.valid = true;

    tesseract_ffi::CommandParameterFfi p{};
    p.key = "target_users";
    p.schema_json =
        R"({"schema_type":"array","items":{"schema_type":"primitive","type":"user_id"}})";
    p.description_text = "The user ID(s)";
    p.optional = false;
    d.parameters.push_back(p);

    CommandDescription out = tesseract::from_ffi(d);
    CHECK(out.command == "ban");
    CHECK(out.sender == "@draupnir:example.org");
    CHECK(out.sender_display_name == "Draupnir");
    CHECK(out.valid);
    REQUIRE(out.parameters.size() == 1);
    CHECK(out.parameters[0].key == "target_users");
    CHECK(out.parameters[0].schema.kind == ParamSchemaKind::Array);
}

// ---------------------------------------------------------------------------
// Client::match_bot_command_arguments
// ---------------------------------------------------------------------------

namespace
{

CommandDescription make_desc(std::vector<std::pair<std::string, bool>> params)
{
    CommandDescription d;
    d.command = "cmd";
    for (auto& [key, optional] : params)
    {
        CommandParameter p;
        p.key = key;
        p.schema.kind = ParamSchemaKind::Primitive;
        p.schema.primitive_type = ParamPrimitiveType::String;
        p.optional = optional;
        d.parameters.push_back(p);
    }
    return d;
}

} // namespace

TEST_CASE("match_bot_command_arguments fills every parameter when token count matches",
          "[bot_commands][match]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", false}, {"b", true}});
    auto r = client.match_bot_command_arguments(desc, {"x", "y"});
    REQUIRE(r.ok);
    REQUIRE(r.tokens_by_parameter.size() == 2);
    CHECK(r.tokens_by_parameter[0] == "x");
    CHECK(r.tokens_by_parameter[1] == "y");
}

TEST_CASE("match_bot_command_arguments allows omitting trailing optional parameters",
          "[bot_commands][match]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", false}, {"b", true}, {"c", true}});
    auto r = client.match_bot_command_arguments(desc, {"x"});
    REQUIRE(r.ok);
    REQUIRE(r.tokens_by_parameter.size() == 3);
    CHECK(r.tokens_by_parameter[0] == "x");
    CHECK_FALSE(r.tokens_by_parameter[1].has_value());
    CHECK_FALSE(r.tokens_by_parameter[2].has_value());
}

TEST_CASE("match_bot_command_arguments rejects a missing required parameter",
          "[bot_commands][match]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", false}, {"b", false}});
    auto r = client.match_bot_command_arguments(desc, {"x"});
    REQUIRE_FALSE(r.ok);
    CHECK(r.error_parameter_key == "b");
}

TEST_CASE("match_bot_command_arguments rejects too many tokens", "[bot_commands][match]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", false}});
    auto r = client.match_bot_command_arguments(desc, {"x", "y"});
    REQUIRE_FALSE(r.ok);
    CHECK(r.error_parameter_key.empty());
}

TEST_CASE("match_bot_command_arguments accepts zero tokens for an all-optional command",
          "[bot_commands][match]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", true}});
    auto r = client.match_bot_command_arguments(desc, {});
    REQUIRE(r.ok);
    CHECK_FALSE(r.tokens_by_parameter[0].has_value());
}

// ---------------------------------------------------------------------------
// dispatch_bot_command_send (coercion + validation error surfacing)
// ---------------------------------------------------------------------------

TEST_CASE("dispatch_bot_command_send rejects too many arguments before touching the SDK",
          "[bot_commands][dispatch]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", false}});
    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "one two");
    REQUIRE_FALSE(r.ok);
    CHECK(r.error == "Too many arguments");
}

TEST_CASE("dispatch_bot_command_send reports a missing required argument by key",
          "[bot_commands][dispatch]")
{
    tesseract::Client client;
    auto desc = make_desc({{"target_room", false}});
    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "");
    REQUIRE_FALSE(r.ok);
    CHECK(r.error == "Missing required argument: target_room");
}

TEST_CASE("dispatch_bot_command_send rejects a non-integer token for an integer parameter",
          "[bot_commands][dispatch]")
{
    tesseract::Client client;
    CommandDescription desc;
    desc.command = "cmd";
    CommandParameter p;
    p.key = "timeout_seconds";
    p.schema.kind = ParamSchemaKind::Primitive;
    p.schema.primitive_type = ParamPrimitiveType::Integer;
    desc.parameters.push_back(p);

    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "not_a_number");
    REQUIRE_FALSE(r.ok);
    CHECK(r.error.find("timeout_seconds") != std::string::npos);
}

TEST_CASE(
    "dispatch_bot_command_send rejects a float-shaped token for an integer parameter (no float type)",
    "[bot_commands][dispatch]")
{
    tesseract::Client client;
    CommandDescription desc;
    desc.command = "cmd";
    CommandParameter p;
    p.key = "n";
    p.schema.kind = ParamSchemaKind::Primitive;
    p.schema.primitive_type = ParamPrimitiveType::Integer;
    desc.parameters.push_back(p);

    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "3.14");
    REQUIRE_FALSE(r.ok);
}

TEST_CASE("dispatch_bot_command_send accepts a well-formed command and reaches the SDK call",
          "[bot_commands][dispatch]")
{
    // The test-cfg Rust FFI stub for send_bot_command always returns
    // {ok:false, message:"not logged in"} — reaching that specific error
    // (rather than a validation error) proves coercion succeeded and the
    // call was actually attempted.
    tesseract::Client client;
    CommandDescription desc;
    desc.command = "ban";
    desc.sender = "@bot:h";
    CommandParameter target;
    target.key = "target_room";
    target.schema.kind = ParamSchemaKind::Primitive;
    target.schema.primitive_type = ParamPrimitiveType::RoomId;
    desc.parameters.push_back(target);
    CommandParameter timeout;
    timeout.key = "timeout_seconds";
    timeout.schema.kind = ParamSchemaKind::Primitive;
    timeout.schema.primitive_type = ParamPrimitiveType::Integer;
    desc.parameters.push_back(timeout);

    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "!target:h 42");
    REQUIRE_FALSE(r.ok);
    CHECK(r.error == "not logged in");
}

TEST_CASE("tokenize_args: edge cases", "[bot_commands][tokenize]")
{
    using V = std::vector<std::string>;
    CHECK(tesseract::tokenize_args("").empty());
    CHECK(tesseract::tokenize_args("  \t ").empty());
    CHECK(tesseract::tokenize_args("a  b") == V{"a", "b"});
    CHECK(tesseract::tokenize_args("a\tb") == V{"a", "b"});
    CHECK(tesseract::tokenize_args("\"hello world\" x") == V{"hello world", "x"});
    CHECK(tesseract::tokenize_args("'it has \"dq\"'") == V{"it has \"dq\""});
    // Unterminated quote runs to the end.
    CHECK(tesseract::tokenize_args("a \"b c") == V{"a", "b c"});
    // Adjacent quoted/unquoted pieces form one token.
    CHECK(tesseract::tokenize_args("\"a b\"\"c d\"") == V{"a bc d"});
    CHECK(tesseract::tokenize_args("pre\"a b\"post") == V{"prea bpost"});
    // Empty quotes make an empty token.
    CHECK(tesseract::tokenize_args("\"\" x") == V{"", "x"});
}

TEST_CASE("dispatch_bot_command_send treats a quoted span as one argument",
          "[bot_commands][dispatch]")
{
    tesseract::Client client;
    auto desc = make_desc({{"a", false}});
    desc.sender = "@bot:h";
    // Unquoted: two tokens for one parameter.
    auto r1 = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "hello world");
    REQUIRE_FALSE(r1.ok);
    CHECK(r1.error == "Too many arguments");
    // Quoted: one argument, reaches the SDK stub.
    auto r2 = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "\"hello world\"");
    REQUIRE_FALSE(r2.ok);
    CHECK(r2.error == "not logged in");
    // Quoted string containing commas is a single string argument.
    auto r3 = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "'a, b, c'");
    REQUIRE_FALSE(r3.ok);
    CHECK(r3.error == "not logged in");
}

TEST_CASE("bot_command_fallback_body re-quotes tokens with whitespace", "[bot_commands][dispatch]")
{
    CHECK(tesseract::bot_command_fallback_body("cmd", {}) == "/cmd");
    CHECK(tesseract::bot_command_fallback_body("cmd", {"a", "b"}) == "/cmd a b");
    CHECK(tesseract::bot_command_fallback_body("cmd", {"hello world", "x"}) ==
          "/cmd \"hello world\" x");
    CHECK(tesseract::bot_command_fallback_body("cmd", {"say \"hi\" now"}) ==
          "/cmd 'say \"hi\" now'");
    CHECK(tesseract::bot_command_fallback_body("cmd", {""}) == "/cmd \"\"");
}

namespace
{
ParamSchema primitive_schema(ParamPrimitiveType t)
{
    ParamSchema s;
    s.kind = ParamSchemaKind::Primitive;
    s.primitive_type = t;
    return s;
}

nlohmann::json coerce_arg(const std::string& token, ParamPrimitiveType t,
                          const std::string& room = "!cur:h")
{
    auto r = tesseract::coerce_bot_command_argument(token, primitive_schema(t), room);
    REQUIRE(r.has_value());
    return nlohmann::json::parse(*r);
}
} // namespace

TEST_CASE("room_id argument: bare room id has no via", "[bot_commands][coerce]")
{
    CHECK(coerce_arg("!r:h", ParamPrimitiveType::RoomId) ==
          nlohmann::json{{"type", "room_id"}, {"id", "!r:h"}});
}

TEST_CASE("room_id argument: alias is passed through for the SDK to resolve",
          "[bot_commands][coerce]")
{
    CHECK(coerce_arg("#a:h", ParamPrimitiveType::RoomId) ==
          nlohmann::json{{"type", "room_id"}, {"id", "#a:h"}});
}

TEST_CASE("room_id argument: room permalink carries via", "[bot_commands][coerce]")
{
    CHECK(coerce_arg("https://matrix.to/#/!r:h?via=a.org&via=b.org", ParamPrimitiveType::RoomId) ==
          nlohmann::json{{"type", "room_id"}, {"id", "!r:h"}, {"via", {"a.org", "b.org"}}});
}

TEST_CASE("event_id argument: event permalink gives room, event and via", "[bot_commands][coerce]")
{
    CHECK(coerce_arg("https://matrix.to/#/!r:h/$e:h?via=a.org", ParamPrimitiveType::EventId) ==
          nlohmann::json{
              {"type", "event_id"}, {"id", "!r:h"}, {"event_id", "$e:h"}, {"via", {"a.org"}}});
}

TEST_CASE("event_id argument: bare event uses the current room", "[bot_commands][coerce]")
{
    CHECK(coerce_arg("$e:h", ParamPrimitiveType::EventId) ==
          nlohmann::json{{"type", "event_id"}, {"id", "!cur:h"}, {"event_id", "$e:h"}});
}

TEST_CASE("room_id/event_id arguments reject bad tokens", "[bot_commands][coerce]")
{
    const auto room = primitive_schema(ParamPrimitiveType::RoomId);
    const auto ev = primitive_schema(ParamPrimitiveType::EventId);
    CHECK_FALSE(tesseract::coerce_bot_command_argument("nonsense", room, "!cur:h"));
    CHECK_FALSE(tesseract::coerce_bot_command_argument("@user:h", room, "!cur:h"));
    CHECK_FALSE(tesseract::coerce_bot_command_argument("$e:h", room, "!cur:h"));
    CHECK_FALSE(tesseract::coerce_bot_command_argument("!r:h", ev, "!cur:h"));
    CHECK_FALSE(tesseract::coerce_bot_command_argument("https://matrix.to/#/!r:h", ev, "!cur:h"));
    CHECK_FALSE(tesseract::coerce_bot_command_argument("https://matrix.to/#/@u:h", room, "!cur:h"));
}

TEST_CASE("dispatch_bot_command_send accepts a comma-separated array token",
          "[bot_commands][dispatch]")
{
    tesseract::Client client;
    CommandDescription desc;
    desc.command = "ban";
    desc.sender = "@bot:h";
    CommandParameter users;
    users.key = "target_users";
    users.schema.kind = ParamSchemaKind::Array;
    users.schema.array_item = std::make_shared<ParamSchema>();
    users.schema.array_item->kind = ParamSchemaKind::Primitive;
    users.schema.array_item->primitive_type = ParamPrimitiveType::UserId;
    desc.parameters.push_back(users);

    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "@alice:h,@bob:h");
    REQUIRE_FALSE(r.ok);
    CHECK(r.error == "not logged in"); // reached the SDK call — coercion succeeded
}

TEST_CASE("dispatch_bot_command_send rejects a malformed array element", "[bot_commands][dispatch]")
{
    tesseract::Client client;
    CommandDescription desc;
    desc.command = "ban";
    CommandParameter users;
    users.key = "target_users";
    users.schema.kind = ParamSchemaKind::Array;
    users.schema.array_item = std::make_shared<ParamSchema>();
    users.schema.array_item->kind = ParamSchemaKind::Primitive;
    users.schema.array_item->primitive_type = ParamPrimitiveType::UserId;
    desc.parameters.push_back(users);

    // "bob" has no '@' sigil or ':' domain — not a valid user_id.
    auto r = tesseract::dispatch_bot_command_send(client, "!room:h", desc, "@alice:h,bob");
    REQUIRE_FALSE(r.ok);
    CHECK(r.error != "not logged in");
}

// ---------------------------------------------------------------------------
// describe_param_type / next_bot_command_arg_hint
// ---------------------------------------------------------------------------

TEST_CASE("describe_param_type never leaks a raw schema type string", "[bot_commands][hint]")
{
    ParamSchema s;
    s.kind = ParamSchemaKind::Primitive;
    s.primitive_type = ParamPrimitiveType::UserId;
    CHECK(tesseract::describe_param_type(s) == "user");
}

TEST_CASE("next_bot_command_arg_hint points at the next parameter", "[bot_commands][hint]")
{
    auto desc = make_desc({{"a", false}, {"b", true}});
    CHECK(tesseract::next_bot_command_arg_hint(desc, 0) == "a: text");
    CHECK(tesseract::next_bot_command_arg_hint(desc, 1) == "b: text (optional)");
}

TEST_CASE("next_bot_command_arg_hint signals readiness once every parameter is typed",
          "[bot_commands][hint]")
{
    auto desc = make_desc({{"a", false}});
    CHECK(tesseract::next_bot_command_arg_hint(desc, 1) == "Press Enter to send");
}

TEST_CASE("format_bot_command_display renders a received invocation", "[bot_commands][display]")
{
    using tesseract::format_bot_command_display;
    CHECK(format_bot_command_display("", R"({"a":1})").empty());
    CHECK(format_bot_command_display("ping", "") == "/ping");
    CHECK(format_bot_command_display("ping", "not json") == "/ping");
    CHECK(format_bot_command_display("ping", "[1]") == "/ping");
    CHECK(format_bot_command_display("say", R"({"text":"hi there","n":42,"ok":true})") ==
          "/say n: 42 ok: true text: hi there");
    CHECK(format_bot_command_display("ban", R"({"target_room":{"type":"room_id","id":"!room:x",)"
                                            R"("via":["x"]},"timeout_seconds":42})") ==
          "/ban target_room: !room:x timeout_seconds: 42");
    CHECK(format_bot_command_display(
              "pin", R"({"ev":{"type":"event_id","id":"!r:x","event_id":"$e"}})") == "/pin ev: $e");
    CHECK(format_bot_command_display("add", R"({"users":["@a:x","@b:x"]})") ==
          "/add users: @a:x, @b:x");
}
