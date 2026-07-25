//! MSC4391 simplified in-room bot commands.
//!
//! Bots advertise commands via `m.bot.command_description` (unstable:
//! `org.matrix.msc4391.command_description`) room state events, one event per
//! `(command, sender)` pair. This module parses/validates a single such
//! event's `content` into a [`CommandDescription`], merges/dedupes the
//! stable+unstable event-type reads, and builds the `m.bot.command`
//! (unstable: `org.matrix.msc4391.command`) invocation content block clients
//! attach to an outgoing `m.room.message`.
//!
//! Unlike MSC2545 image packs (see `image_packs.rs`), a command description's
//! `state_key` is a content hash the client can't predict ahead of time, so
//! the per-known-state_key read `fetch_room_pack` uses doesn't apply here —
//! callers must bulk-read `get_state_events` for both event types (see
//! `client/mod.rs`) and pass each event's `(sender, state_key, room_id,
//! content)` through [`parse_command_description`].
//!
//! Validation is deliberately split by layer (see CLAUDE.md's architecture
//! notes for the equivalent split on other features): this module only
//! enforces *structural* validity — illegal schema nesting is rejected at
//! JSON-parse time by the type system below (see [`ArrayItemSchema`] /
//! [`UnionVariantSchema`] not admitting the nested-illegal variants at all),
//! and duplicate parameter keys or literal value/type mismatches set
//! [`CommandDescription::valid`] to `false` rather than discarding the event.
//! Positional-argument-to-schema *matching* (required params stay ordered,
//! optional params don't) and user-input *coercion* are UI/client-layer
//! concerns and live in `client/` and `ui/shared/app/SlashCommands.cpp`.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashSet;

/// Stable event type for command-description state events.
pub const TYPE_COMMAND_DESCRIPTION_STABLE: &str = "m.bot.command_description";
/// Unstable (MSC4391-prefixed) event type for command-description state
/// events. Read alongside the stable type for the lifetime of the unstable
/// period; see [`COMMAND_DESCRIPTION_TYPES`].
pub const TYPE_COMMAND_DESCRIPTION_UNSTABLE: &str = "org.matrix.msc4391.command_description";

/// Both command-description event types, unstable first — matches
/// `image_packs.rs`'s `ROOM_PACK_TYPES` convention of listing the unstable
/// name first since most of the ecosystem still only writes it. Reads probe
/// both and merge via [`merge_command_descriptions`], which prefers the
/// *stable* side on a `(command, sender)` collision regardless of read
/// order.
pub const COMMAND_DESCRIPTION_TYPES: [&str; 2] = [
    TYPE_COMMAND_DESCRIPTION_UNSTABLE,
    TYPE_COMMAND_DESCRIPTION_STABLE,
];

/// Stable content key carrying a command invocation on an outgoing
/// `m.room.message`.
pub const KEY_COMMAND_STABLE: &str = "m.bot.command";
/// Unstable (MSC4391-prefixed) content key. Written alongside the stable key
/// during the unstable period (see [`build_invocation_content`]) to maximize
/// compatibility with bots still on the unstable name; drop this write path
/// once MSC4391 stabilizes. Unstable *reads* are not relevant here since
/// invocation content is never read back by the client.
pub const KEY_COMMAND_UNSTABLE: &str = "org.matrix.msc4391.command";

/// MSC1767-style extensible plain-text representation used for
/// `description` fields: `{"m.text": [{"body": "..."}]}`. Multiple entries
/// support future mimetype/language variants; v1 only needs the first
/// entry's `body` (see [`plain_text`](RichText::plain_text)).
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct RichText {
    #[serde(rename = "m.text", default)]
    pub m_text: Vec<TextRepr>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TextRepr {
    pub body: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lang: Option<String>,
}

impl RichText {
    /// The first representation's body, or `""` when absent. Sufficient for
    /// v1's plain-text hint/description rendering.
    pub fn plain_text(&self) -> &str {
        self.m_text.first().map(|t| t.body.as_str()).unwrap_or("")
    }
}

/// The six primitive parameter value types MSC4391 defines. `String`/
/// `Integer`/`Boolean` are unconstrained; `UserId`/`ServerName`/`RoomAlias`
/// are strings additionally constrained to valid Matrix identifier grammar
/// (validated by the caller when coercing user input, not here).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PrimitiveType {
    String,
    Integer,
    Boolean,
    UserId,
    ServerName,
    RoomAlias,
}

/// The two object parameter value types. The wire *value* shape for each is
/// fixed by the MSC (`room_id`: `{id, via?}`; `event_id`: `{id, via?,
/// event_id}`) rather than being schema-configurable, so this enum only
/// records which shape applies.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ObjectType {
    RoomId,
    EventId,
}

/// Restricted value type for a `literal` schema's `literal_type` — only
/// `string`/`integer`/`boolean` per the MSC, never the constrained-string or
/// object subtypes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LiteralType {
    String,
    Integer,
    Boolean,
}

fn literal_value_matches(value: &Value, ty: LiteralType) -> bool {
    match ty {
        LiteralType::String => value.is_string(),
        // Explicitly excludes `is_f64()` — MSC4391 has no float type because
        // matrix.org canonical JSON doesn't support encoding floats.
        LiteralType::Integer => value.is_i64() || value.is_u64(),
        LiteralType::Boolean => value.is_boolean(),
    }
}

/// A parameter's declared type, restricted to what's legal at the top level
/// of a parameter (`primitive`, `object`, `array`, `literal`). `union` is
/// deliberately absent from this enum — per the MSC it is "only permitted
/// outside top-level within arrays" — so a top-level `"schema_type":"union"`
/// fails to deserialize into `ParamSchema` and the whole command description
/// is dropped by [`parse_command_description`] (structurally malformed, not
/// merely `valid = false`).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "schema_type", rename_all = "snake_case")]
pub enum ParamSchema {
    Primitive {
        #[serde(rename = "type")]
        ty: PrimitiveType,
    },
    Object {
        #[serde(rename = "type")]
        ty: ObjectType,
    },
    /// Top-level only per the MSC. `items` is restricted to
    /// [`ArrayItemSchema`], which has no `array` variant, so an
    /// array-of-arrays is a parse error rather than a runtime check.
    Array { items: ArrayItemSchema },
    Literal {
        value: Value,
        literal_type: LiteralType,
    },
}

/// The schema types legal inside an array's `items` — `union`, `primitive`,
/// or `literal` per the MSC. Notably excludes `object`: MSC4391's array
/// schema section restricts items to these three, so a `room_id`/`event_id`
/// parameter can never be array-typed.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "schema_type", rename_all = "snake_case")]
pub enum ArrayItemSchema {
    Primitive {
        #[serde(rename = "type")]
        ty: PrimitiveType,
    },
    /// Nested unions and unions-of-arrays are prohibited by the MSC; both
    /// are unrepresentable here since [`UnionVariantSchema`] has no `union`
    /// or `array` variant, so they're parse errors, not runtime checks.
    Union { variants: Vec<UnionVariantSchema> },
    Literal {
        value: Value,
        literal_type: LiteralType,
    },
}

/// The schema types legal as a `union`'s variant — `primitive` or `literal`
/// only.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "schema_type", rename_all = "snake_case")]
pub enum UnionVariantSchema {
    Primitive {
        #[serde(rename = "type")]
        ty: PrimitiveType,
    },
    Literal {
        value: Value,
        literal_type: LiteralType,
    },
}

fn array_item_well_formed(item: &ArrayItemSchema) -> bool {
    match item {
        ArrayItemSchema::Literal { value, literal_type } => {
            literal_value_matches(value, *literal_type)
        }
        ArrayItemSchema::Union { variants } => variants.iter().all(union_variant_well_formed),
        ArrayItemSchema::Primitive { .. } => true,
    }
}

fn union_variant_well_formed(variant: &UnionVariantSchema) -> bool {
    match variant {
        UnionVariantSchema::Literal { value, literal_type } => {
            literal_value_matches(value, *literal_type)
        }
        UnionVariantSchema::Primitive { .. } => true,
    }
}

/// Recursive well-formedness check for a single parameter's schema: every
/// `literal` node's `value` must match its declared `literal_type` (rejects
/// e.g. `literal_type: "integer"` paired with `value: "not a number"`, and
/// rejects float-shaped values against `integer` per the MSC's no-float
/// rule). Illegal *nesting* is already unrepresentable by construction (see
/// [`ParamSchema`]/[`ArrayItemSchema`]/[`UnionVariantSchema`]'s doc
/// comments), so this only needs to check literal value/type agreement.
fn schema_well_formed(schema: &ParamSchema) -> bool {
    match schema {
        ParamSchema::Literal { value, literal_type } => literal_value_matches(value, *literal_type),
        ParamSchema::Array { items } => array_item_well_formed(items),
        ParamSchema::Primitive { .. } | ParamSchema::Object { .. } => true,
    }
}

/// A single parameter as declared in a command description's `parameters`
/// array. `optional` parameters are not positionally significant when a
/// client matches typed user input against this list (MSC invariant;
/// enforced by the client-layer matcher, not here).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CommandParameter {
    pub key: String,
    pub schema: ParamSchema,
    #[serde(default)]
    pub description: RichText,
    #[serde(default)]
    pub optional: bool,
}

/// Wire shape of a `m.bot.command_description` state event's `content`,
/// before validation.
#[derive(Debug, Clone, Serialize, Deserialize)]
struct CommandDescriptionContent {
    command: String,
    #[serde(default)]
    parameters: Vec<CommandParameter>,
    #[serde(default)]
    description: RichText,
}

/// A parsed, room-scoped bot command advertisement.
#[derive(Debug, Clone)]
pub struct CommandDescription {
    pub command: String,
    pub parameters: Vec<CommandParameter>,
    /// Flattened `description.m.text[0].body` — see [`RichText::plain_text`].
    pub description: String,
    /// The state event's `sender` (the bot's mxid), not part of the wire
    /// content itself.
    pub sender: String,
    /// The sender's room display name, for popup disambiguation. Empty from
    /// [`parse_command_description`] (which only sees the state event, not
    /// room membership) — filled in by [`filter_joined_senders`], the only
    /// place a joined-member map is available.
    pub sender_display_name: String,
    pub state_key: String,
    pub room_id: String,
    /// `false` when the content is structurally parseable but violates a
    /// value-level invariant (duplicate parameter keys, or a `literal`
    /// node whose `value` doesn't match its `literal_type`). Callers should
    /// filter these out before offering the command for autocomplete/
    /// dispatch, but still surface them if ever showing raw diagnostics.
    pub valid: bool,
}

/// Parse one command-description state event's `content` into a
/// [`CommandDescription`]. Returns `None` when the content doesn't even
/// match the base wire shape (missing `command`, or a schema using illegal
/// nesting/an unrecognized `schema_type`) — those are structurally
/// unparseable, not merely invalid, and are not listed at all. Duplicate
/// parameter keys and literal value/type mismatches are less severe: the
/// description is still returned, with [`CommandDescription::valid`] set to
/// `false`.
pub fn parse_command_description(
    sender: &str,
    state_key: &str,
    room_id: &str,
    content: &Value,
) -> Option<CommandDescription> {
    let raw: CommandDescriptionContent = serde_json::from_value(content.clone()).ok()?;

    let mut valid = true;

    let mut seen_keys = HashSet::with_capacity(raw.parameters.len());
    for p in &raw.parameters {
        if !seen_keys.insert(p.key.clone()) {
            valid = false;
        }
        if !schema_well_formed(&p.schema) {
            valid = false;
        }
    }

    Some(CommandDescription {
        command: raw.command,
        parameters: raw.parameters,
        description: raw.description.plain_text().to_owned(),
        sender: sender.to_owned(),
        sender_display_name: String::new(),
        state_key: state_key.to_owned(),
        room_id: room_id.to_owned(),
        valid,
    })
}

/// Merge command descriptions read from the stable and unstable event types
/// for the same room, deduping on `(command, sender)` and preferring the
/// *stable* entry on collision — same tie-break style as
/// `image_packs.rs::merge_pack_contents`, just at the list level rather than
/// the JSON-content level, since a command description isn't merged
/// field-by-field: either the stable event exists for that `(command,
/// sender)` pair or it doesn't.
pub fn merge_command_descriptions(
    stable: Vec<CommandDescription>,
    unstable: Vec<CommandDescription>,
) -> Vec<CommandDescription> {
    let seen: HashSet<(String, String)> = stable
        .iter()
        .map(|d| (d.command.clone(), d.sender.clone()))
        .collect();
    let mut out = stable;
    out.extend(
        unstable
            .into_iter()
            .filter(|d| !seen.contains(&(d.command.clone(), d.sender.clone()))),
    );
    out
}

/// Filter descriptions to only those whose `sender` is a key in
/// `joined_user_display_names` (`user_id -> room display name`) — the MSC
/// invariant that a command description only applies while its author
/// remains joined to the room. Also fills in
/// [`CommandDescription::sender_display_name`] from the same map, since this
/// is the only place a joined-member map is available. Pure function of the
/// already-fetched joined-member map so it needs no live `Room`/network
/// access and is trivially unit-testable.
pub fn filter_joined_senders(
    descriptions: Vec<CommandDescription>,
    joined_user_display_names: &std::collections::HashMap<String, String>,
) -> Vec<CommandDescription> {
    descriptions
        .into_iter()
        .filter_map(|mut d| {
            let name = joined_user_display_names.get(&d.sender)?;
            d.sender_display_name = name.clone();
            Some(d)
        })
        .collect()
}

/// Build the `content` JSON of an outgoing `m.room.message` bot-command
/// invocation: the usual `body`/`msgtype`(/`formatted_body`/`format`) fields
/// plus both the stable and unstable `m.bot.command` blocks (identical
/// payload under each key — see [`KEY_COMMAND_UNSTABLE`]'s doc comment for
/// why both are written). `arguments` must already be the coerced,
/// schema-conformant `{key: value}` map — this function does no validation,
/// it only assembles JSON.
pub fn build_invocation_content(
    body: &str,
    formatted_body: Option<&str>,
    mentioned_user_ids: &[String],
    mention_room: bool,
    command: &str,
    arguments: Value,
) -> Value {
    let mut content = serde_json::json!({
        "body": body,
        "msgtype": "m.text",
        "m.mentions": { "user_ids": mentioned_user_ids },
    });
    if mention_room {
        content["m.mentions"]["room"] = Value::Bool(true);
    }

    if let Some(html) = formatted_body {
        if !html.is_empty() {
            content["formatted_body"] = Value::String(html.to_owned());
            content["format"] = Value::String("org.matrix.custom.html".to_owned());
        }
    }

    let command_block = serde_json::json!({ "command": command, "arguments": arguments });
    content[KEY_COMMAND_STABLE] = command_block.clone();
    content[KEY_COMMAND_UNSTABLE] = command_block;

    content
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn joined(ids: &[&str]) -> std::collections::HashMap<String, String> {
        ids.iter().map(|s| (s.to_string(), String::new())).collect()
    }

    #[test]
    fn type_slices_prefer_unstable_then_stable() {
        assert_eq!(
            COMMAND_DESCRIPTION_TYPES,
            [
                "org.matrix.msc4391.command_description",
                "m.bot.command_description"
            ]
        );
        assert_eq!(KEY_COMMAND_STABLE, "m.bot.command");
        assert_eq!(KEY_COMMAND_UNSTABLE, "org.matrix.msc4391.command");
    }

    #[test]
    fn parses_full_example_from_the_msc() {
        let c = json!({
            "command": "ban",
            "parameters": [
                {
                    "key": "target_room",
                    "schema": { "schema_type": "object", "type": "room_id" },
                    "description": { "m.text": [{ "body": "The room ID" }] }
                },
                {
                    "key": "timeout_seconds",
                    "schema": { "schema_type": "primitive", "type": "integer" },
                    "description": { "m.text": [{ "body": "The timeout in seconds" }] }
                },
                {
                    "key": "apply_to_policy",
                    "schema": { "schema_type": "primitive", "type": "boolean" },
                    "description": { "m.text": [{ "body": "Whether to apply this to policy" }] },
                    "optional": true
                },
                {
                    "key": "target_users",
                    "schema": {
                        "schema_type": "array",
                        "items": { "schema_type": "primitive", "type": "user_id" }
                    },
                    "description": { "m.text": [{ "body": "The user ID(s)" }] }
                }
            ],
            "description": { "m.text": [{ "body": "An example command with arguments" }] }
        });
        let d = parse_command_description("@draupnir:draupnir.space", "state1", "!room:h", &c).unwrap();
        assert!(d.valid);
        assert_eq!(d.command, "ban");
        assert_eq!(d.parameters.len(), 4);
        assert_eq!(d.description, "An example command with arguments");
        assert!(d.parameters[2].optional); // apply_to_policy
        assert!(!d.parameters[0].optional); // target_room
    }

    #[test]
    fn missing_command_field_fails_to_parse() {
        let c = json!({ "parameters": [] });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn top_level_union_schema_fails_to_parse() {
        // Per the MSC, union is only legal inside an array's items, never
        // at the top level of a parameter's schema.
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": { "schema_type": "union", "variants": [
                    { "schema_type": "primitive", "type": "string" }
                ] }
            }]
        });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn array_of_array_fails_to_parse() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": {
                    "schema_type": "array",
                    "items": { "schema_type": "array", "items": { "schema_type": "primitive", "type": "string" } }
                }
            }]
        });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn union_of_arrays_fails_to_parse() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": {
                    "schema_type": "array",
                    "items": {
                        "schema_type": "union",
                        "variants": [
                            { "schema_type": "array", "items": { "schema_type": "primitive", "type": "string" } }
                        ]
                    }
                }
            }]
        });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn nested_union_fails_to_parse() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": {
                    "schema_type": "array",
                    "items": {
                        "schema_type": "union",
                        "variants": [
                            { "schema_type": "union", "variants": [] }
                        ]
                    }
                }
            }]
        });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn array_of_object_fails_to_parse() {
        // MSC restricts array items to union/primitive/literal — object
        // (room_id/event_id) is not a legal array item type.
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": {
                    "schema_type": "array",
                    "items": { "schema_type": "object", "type": "room_id" }
                }
            }]
        });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn duplicate_parameter_keys_marks_invalid_but_still_parses() {
        let c = json!({
            "command": "x",
            "parameters": [
                { "key": "a", "schema": { "schema_type": "primitive", "type": "string" } },
                { "key": "a", "schema": { "schema_type": "primitive", "type": "integer" } }
            ]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(!d.valid);
        assert_eq!(d.parameters.len(), 2);
    }

    #[test]
    fn literal_value_type_mismatch_marks_invalid() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "a",
                "schema": { "schema_type": "literal", "value": "not_a_number", "literal_type": "integer" }
            }]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(!d.valid);
    }

    #[test]
    fn literal_float_value_against_integer_type_marks_invalid() {
        // No float type per the MSC — a literal declared `integer` but
        // carrying a float-shaped JSON number (e.g. `3.14` or `3.0`) is
        // invalid, not silently truncated.
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "a",
                "schema": { "schema_type": "literal", "value": 3.14, "literal_type": "integer" }
            }]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(!d.valid);
    }

    #[test]
    fn literal_value_type_match_is_valid() {
        let c = json!({
            "command": "x",
            "parameters": [
                { "key": "a", "schema": { "schema_type": "literal", "value": 42, "literal_type": "integer" } },
                { "key": "b", "schema": { "schema_type": "literal", "value": "ok", "literal_type": "string" } },
                { "key": "c", "schema": { "schema_type": "literal", "value": true, "literal_type": "boolean" } }
            ]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(d.valid);
    }

    #[test]
    fn literal_inside_array_items_is_checked() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "a",
                "schema": {
                    "schema_type": "array",
                    "items": { "schema_type": "literal", "value": "nope", "literal_type": "integer" }
                }
            }]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(!d.valid);
    }

    #[test]
    fn union_variant_and_array_of_union_are_legal() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "a",
                "schema": {
                    "schema_type": "array",
                    "items": {
                        "schema_type": "union",
                        "variants": [
                            { "schema_type": "primitive", "type": "string" },
                            { "schema_type": "literal", "value": "yes", "literal_type": "string" }
                        ]
                    }
                }
            }]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(d.valid);
    }

    #[test]
    fn missing_parameters_and_description_default_to_empty() {
        let c = json!({ "command": "shrug" });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(d.valid);
        assert!(d.parameters.is_empty());
        assert_eq!(d.description, "");
    }

    fn desc(command: &str, sender: &str) -> CommandDescription {
        CommandDescription {
            command: command.to_owned(),
            parameters: vec![],
            description: String::new(),
            sender: sender.to_owned(),
            sender_display_name: String::new(),
            state_key: "s".to_owned(),
            room_id: "!r:h".to_owned(),
            valid: true,
        }
    }

    #[test]
    fn merge_prefers_stable_on_collision() {
        let stable = vec![{
            let mut d = desc("ban", "@bot:h");
            d.description = "stable".to_owned();
            d
        }];
        let unstable = vec![{
            let mut d = desc("ban", "@bot:h");
            d.description = "unstable".to_owned();
            d
        }];
        let merged = merge_command_descriptions(stable, unstable);
        assert_eq!(merged.len(), 1);
        assert_eq!(merged[0].description, "stable");
    }

    #[test]
    fn merge_keeps_non_colliding_entries_from_both_sides() {
        let stable = vec![desc("ban", "@bot1:h")];
        let unstable = vec![desc("kick", "@bot1:h"), desc("ban", "@bot2:h")];
        let merged = merge_command_descriptions(stable, unstable);
        assert_eq!(merged.len(), 3);
    }

    #[test]
    fn filter_joined_senders_drops_non_joined() {
        let all = vec![desc("ban", "@bot1:h"), desc("kick", "@bot2:h")];
        let filtered = filter_joined_senders(all, &joined(&["@bot1:h"]));
        assert_eq!(filtered.len(), 1);
        assert_eq!(filtered[0].sender, "@bot1:h");
    }

    #[test]
    fn filter_joined_senders_fills_in_display_name() {
        let mut names = std::collections::HashMap::new();
        names.insert("@bot1:h".to_owned(), "Bot One".to_owned());
        let filtered = filter_joined_senders(vec![desc("ban", "@bot1:h")], &names);
        assert_eq!(filtered[0].sender_display_name, "Bot One");
    }

    #[test]
    fn build_invocation_content_writes_both_stable_and_unstable_keys() {
        let content = build_invocation_content(
            "@bot:h ban !room:h 42",
            None,
            &["@bot:h".to_owned()],
            false,
            "ban",
            json!({ "target_room": { "id": "!room:h" }, "timeout_seconds": 42 }),
        );
        assert_eq!(content["msgtype"], "m.text");
        assert_eq!(content["body"], "@bot:h ban !room:h 42");
        assert_eq!(content["m.mentions"]["user_ids"][0], "@bot:h");
        assert!(content["m.mentions"].get("room").is_none());
        assert_eq!(content["m.bot.command"]["command"], "ban");
        assert_eq!(content["org.matrix.msc4391.command"]["command"], "ban");
        assert_eq!(
            content["m.bot.command"]["arguments"],
            content["org.matrix.msc4391.command"]["arguments"]
        );
        assert!(content.get("formatted_body").is_none());
    }

    #[test]
    fn build_invocation_content_sets_room_mention_when_requested() {
        let content = build_invocation_content("x", None, &[], true, "cmd", json!({}));
        assert_eq!(content["m.mentions"]["room"], true);
    }

    #[test]
    fn build_invocation_content_includes_formatted_body_when_present() {
        let content = build_invocation_content(
            "plain",
            Some("<b>html</b>"),
            &[],
            false,
            "cmd",
            json!({}),
        );
        assert_eq!(content["formatted_body"], "<b>html</b>");
        assert_eq!(content["format"], "org.matrix.custom.html");
    }

    #[test]
    fn build_invocation_content_omits_formatted_body_when_empty() {
        let content = build_invocation_content("plain", Some(""), &[], false, "cmd", json!({}));
        assert!(content.get("formatted_body").is_none());
    }
}
