#pragma once
/// MSC4391 in-room bot command C++ types — surfaces from the Rust
/// aggregator (`sdk/src/bot_commands.rs`). Field names mirror the FFI shape
/// where practical. `ParamSchema` is reconstructed from the FFI's
/// `schema_json` string by `ffi_convert.h`'s `parse_param_schema` — cxx
/// cannot carry a recursive enum-with-payload across the bridge, so the
/// bridge only carries the JSON text and C++ decodes it once here.

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tesseract
{

/// The six primitive parameter value types MSC4391 defines. `String`/
/// `Integer`/`Boolean` are unconstrained; `UserId`/`ServerName`/`RoomAlias`
/// are strings additionally constrained to valid Matrix identifier grammar.
enum class ParamPrimitiveType
{
    String,
    Integer,
    Boolean,
    UserId,
    ServerName,
    RoomAlias,
};

/// The two object parameter value types. The wire *value* shape for each is
/// fixed by the MSC (`room_id`: `{id, via?}`; `event_id`: `{id, via?,
/// event_id}`).
enum class ParamObjectType
{
    RoomId,
    EventId,
};

/// Restricted value type for a `literal` schema's `literal_type` — only
/// `string`/`integer`/`boolean` per the MSC.
enum class ParamLiteralType
{
    String,
    Integer,
    Boolean,
};

enum class ParamSchemaKind
{
    Primitive,
    Object,
    /// Top-level only per the MSC.
    Array,
    Literal,
    /// Only legal as an array's item schema or nested inside one, never at
    /// the top level of a parameter's own schema.
    Union,
    /// `schema_json` failed to parse or used illegal nesting — the caller
    /// should skip this parameter rather than guess a type for it.
    Invalid,
};

/// A parameter's declared type. Only the field(s) matching `kind` are
/// meaningful; the rest hold their default value.
struct ParamSchema
{
    ParamSchemaKind kind = ParamSchemaKind::Invalid;
    ParamPrimitiveType primitive_type = ParamPrimitiveType::String; ///< Primitive
    ParamObjectType object_type = ParamObjectType::RoomId;          ///< Object
    std::shared_ptr<ParamSchema> array_item;                        ///< Array
    std::vector<ParamSchema> union_variants;                        ///< Union
    std::string literal_value_json;                                 ///< Literal — raw JSON scalar
    ParamLiteralType literal_type = ParamLiteralType::String;       ///< Literal
};

/// A single parameter as declared in a command description's `parameters`
/// array. `optional` parameters are not positionally significant when
/// matching typed user input against this list (MSC invariant; enforced by
/// `Client::match_bot_command_arguments`, not here).
struct CommandParameter
{
    std::string key;
    ParamSchema schema;
    std::string description;
    bool optional = false;
};

/// A parsed, room-scoped bot command advertisement (one
/// `m.bot.command_description` state event).
struct CommandDescription
{
    std::string command;
    /// The bot's mxid (the state event's `sender`).
    std::string sender;
    std::string sender_display_name;
    std::string state_key;
    std::string room_id;
    std::vector<CommandParameter> parameters;
    std::string description;
    /// `false` when the content is structurally parseable but violates a
    /// value-level invariant (duplicate parameter keys, or a `literal`
    /// node whose value doesn't match its `literal_type`). Callers should
    /// filter these out before offering the command for autocomplete/
    /// dispatch.
    bool valid = false;
};

/// Result of `Client::match_bot_command_arguments` — see that method's doc
/// comment (client.h) for the v1 positional-matching convention.
struct BotCommandMatchResult
{
    bool ok = false;
    /// Parallel to the `CommandDescription::parameters` passed in —
    /// `tokens_by_parameter[i]` is the raw token assigned to `parameters[i]`,
    /// or `nullopt` when that (necessarily trailing, necessarily optional)
    /// parameter was omitted. Only meaningful when `ok` is true.
    std::vector<std::optional<std::string>> tokens_by_parameter;
    /// Set when `!ok`: which parameter failed and why ("too many arguments"
    /// or "missing required argument: <key>"). Not localized — callers
    /// building user-facing text should format their own message from the
    /// parameter key rather than display this directly.
    std::string error;
    /// Set alongside `error` when the failure names a specific parameter
    /// (e.g. a missing required one) — empty for whole-command errors like
    /// "too many arguments".
    std::string error_parameter_key;
};

} // namespace tesseract
