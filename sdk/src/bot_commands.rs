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
//! A command name outside `[a-z0-9_-]{1,32}` or more than
//! [`MAX_COMMAND_PARAMETERS`] parameters also clears it.
//! Positional-argument-to-schema *matching* (required params stay ordered,
//! optional params don't) and user-input *coercion* are UI/client-layer
//! concerns and live in `client/` and `ui/shared/views/SlashCommands.cpp`.

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
/// once MSC4391 stabilizes. Received invocations are read back only for
/// timeline display, not through this module.
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

/// The primitive parameter value types MSC4391 defines. `String`/`Integer`/
/// `Boolean` are unconstrained; `UserId`/`ServerName`/`RoomAlias` are strings
/// additionally constrained to valid Matrix identifier grammar (validated by
/// the caller when coercing user input, not here). `RoomId`/`EventId` are
/// reference types whose wire *value* shape is fixed by the MSC rather than
/// being schema-configurable.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PrimitiveType {
    String,
    Integer,
    Boolean,
    UserId,
    ServerName,
    RoomAlias,
    RoomId,
    EventId,
}

/// Legacy `schema_type: "object"` value types, read-only. Earlier MSC4391
/// drafts declared room/event references as `{"schema_type":"object",
/// "type":"room_id"}`; the current draft makes them primitives. Such schemas
/// are normalised to [`PrimitiveType`] in [`parse_command_description`], so
/// the legacy form is never emitted.
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

/// A parameter's declared type, as legal at the top level of a parameter
/// (`primitive`, `array`, `literal`, `union`). A top-level `union` is allowed;
/// the MSC only prohibits *nested* unions, which are unrepresentable here
/// since [`UnionVariantSchema`] has no `union` variant (a parse error, not
/// merely `valid = false`).
///
/// `Object` is the legacy pre-primitive form of room/event references; it is
/// only ever produced by deserialising and is normalised away by
/// [`parse_command_description`].
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "schema_type", rename_all = "snake_case")]
pub enum ParamSchema {
    Primitive {
        #[serde(rename = "type")]
        ty: PrimitiveType,
    },
    /// Legacy read-only form; see [`ObjectType`].
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
    /// Allowed at the top level. Nested unions are prohibited by the MSC and
    /// unrepresentable here, see [`UnionVariantSchema`].
    Union { variants: Vec<UnionVariantSchema> },
}

/// The schema types legal inside an array's `items` — `union`, `primitive`,
/// or `literal` per the MSC. `room_id`/`event_id` are primitives, so arrays
/// of them are legal.
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
        ArrayItemSchema::Literal {
            value,
            literal_type,
        } => literal_value_matches(value, *literal_type),
        ArrayItemSchema::Union { variants } => variants.iter().all(union_variant_well_formed),
        ArrayItemSchema::Primitive { .. } => true,
    }
}

fn union_variant_well_formed(variant: &UnionVariantSchema) -> bool {
    match variant {
        UnionVariantSchema::Literal {
            value,
            literal_type,
        } => literal_value_matches(value, *literal_type),
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
        ParamSchema::Literal {
            value,
            literal_type,
        } => literal_value_matches(value, *literal_type),
        ParamSchema::Array { items } => array_item_well_formed(items),
        ParamSchema::Union { variants } => variants.iter().all(union_variant_well_formed),
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
    /// value-level invariant (duplicate parameter keys, a `literal` node
    /// whose `value` doesn't match its `literal_type`, a command name outside
    /// `[a-z0-9_-]{1,32}`, or more than [`MAX_COMMAND_PARAMETERS`]
    /// parameters). Callers should
    /// filter these out before offering the command for autocomplete/
    /// dispatch, but still surface them if ever showing raw diagnostics.
    pub valid: bool,
}

/// Longest command name accepted as valid.
const MAX_COMMAND_NAME_LEN: usize = 32;
/// Most parameters a command may declare and still be offered as valid.
pub const MAX_COMMAND_PARAMETERS: usize = 16;

/// `true` for a name of 1 to 32 characters drawn from `[a-z0-9_-]`. Any
/// joined sender can publish a description, so the name is kept to a plain
/// shape that cannot shadow a built-in with lookalike text or break the
/// slash popup's layout.
fn command_name_well_formed(name: &str) -> bool {
    (1..=MAX_COMMAND_NAME_LEN).contains(&name.len())
        && name
            .bytes()
            .all(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'_' | b'-'))
}

/// Rewrite a legacy `object` schema to the equivalent room/event primitive.
fn normalise_schema(schema: &mut ParamSchema) {
    if let ParamSchema::Object { ty } = schema {
        let ty = match ty {
            ObjectType::RoomId => PrimitiveType::RoomId,
            ObjectType::EventId => PrimitiveType::EventId,
        };
        *schema = ParamSchema::Primitive { ty };
    }
}

/// Parse one command-description state event's `content` into a
/// [`CommandDescription`]. Returns `None` when the content doesn't even
/// match the base wire shape (missing `command`, or a schema using illegal
/// nesting/an unrecognized `schema_type`) — those are structurally
/// unparseable, not merely invalid, and are not listed at all. Duplicate
/// parameter keys, literal value/type mismatches, a malformed command name
/// and too many parameters are less severe: the description is still
/// returned, with [`CommandDescription::valid`] set to `false`.
pub fn parse_command_description(
    sender: &str,
    state_key: &str,
    room_id: &str,
    content: &Value,
) -> Option<CommandDescription> {
    let mut raw: CommandDescriptionContent = serde_json::from_value(content.clone()).ok()?;
    for p in &mut raw.parameters {
        normalise_schema(&mut p.schema);
    }

    let mut valid =
        command_name_well_formed(&raw.command) && raw.parameters.len() <= MAX_COMMAND_PARAMETERS;

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

/// True if `v` is a `room_id` / `event_id` reference object (`"type"` tag).
fn is_reference_object(v: &Value) -> bool {
    matches!(
        v.get("type").and_then(Value::as_str),
        Some("room_id" | "event_id")
    )
}

/// Every reference object among the top-level argument values (directly or as
/// array items), in argument order.
fn reference_objects_mut(arguments: &mut Value) -> Vec<&mut Value> {
    let mut out = Vec::new();
    let Some(map) = arguments.as_object_mut() else {
        return out;
    };
    for v in map.values_mut() {
        match v {
            Value::Array(items) => out.extend(items.iter_mut().filter(|i| is_reference_object(i))),
            other if is_reference_object(other) => out.push(other),
            _ => {}
        }
    }
    out
}

/// The `id` strings of all reference objects that are room aliases (`#…`).
pub fn reference_aliases(arguments: &mut Value) -> Vec<String> {
    reference_objects_mut(arguments)
        .into_iter()
        .filter_map(|o| o.get("id").and_then(Value::as_str))
        .filter(|id| id.starts_with('#'))
        .map(str::to_owned)
        .collect()
}

/// Resolve the room/event references in an invocation's `arguments` before
/// sending (MSC4391 `room_id` / `event_id` objects):
///
///  * an `id` that is a room alias is replaced by the resolved room id and
///    `via` is seeded from the lookup's server list;
///  * an empty or missing `via` is filled from `route_for_room`.
///
/// Pure: the lookups are injected. `resolve_alias` maps an alias to
/// `(room_id, via)` or an error message; `route_for_room` maps a room id to a
/// via list. Non-reference values are left untouched. The first alias that
/// fails to resolve aborts with its error.
pub fn resolve_reference_arguments(
    arguments: &mut Value,
    resolve_alias: &mut dyn FnMut(&str) -> Result<(String, Vec<String>), String>,
    route_for_room: &mut dyn FnMut(&str) -> Vec<String>,
) -> Result<(), String> {
    for obj in reference_objects_mut(arguments) {
        let Some(id) = obj.get("id").and_then(Value::as_str).map(str::to_owned) else {
            continue;
        };
        let mut room_id = id.clone();
        let mut via: Vec<Value> = obj
            .get("via")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        if id.starts_with('#') {
            let (resolved, servers) = resolve_alias(&id)?;
            room_id = resolved;
            if via.is_empty() {
                via = servers.into_iter().map(Value::String).collect();
            }
        }
        if via.is_empty() {
            via = route_for_room(&room_id)
                .into_iter()
                .map(Value::String)
                .collect();
        }
        obj["id"] = Value::String(room_id);
        if via.is_empty() {
            if let Some(m) = obj.as_object_mut() {
                m.remove("via");
            }
        } else {
            obj["via"] = Value::Array(via);
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Received invocations

/// Whether raw event JSON could carry an MSC4391 invocation. A cheap substring
/// check done before any JSON parsing.
pub fn might_be_invocation(raw_json: &str) -> bool {
    raw_json.contains(KEY_COMMAND_STABLE) || raw_json.contains(KEY_COMMAND_UNSTABLE)
}

/// The invocation `(command, arguments)` carried by an event `content`, when
/// it has a well-formed one. The stable key wins over the unstable one; a
/// malformed block under the stable key falls through to the unstable one.
/// `arguments` is the JSON object, or `{}` when absent.
pub fn parse_invocation(content: &Value) -> Option<(String, Value)> {
    [KEY_COMMAND_STABLE, KEY_COMMAND_UNSTABLE]
        .iter()
        .find_map(|key| {
            let block = content.get(*key)?;
            let command = block.get("command")?.as_str()?;
            if command.is_empty() {
                return None;
            }
            let arguments = match block.get("arguments") {
                None | Some(Value::Null) => Value::Object(Default::default()),
                Some(a @ Value::Object(_)) => a.clone(),
                Some(_) => return None,
            };
            Some((command.to_owned(), arguments))
        })
}

// ---------------------------------------------------------------------------
// Live refresh decisions (pure; the event handlers live in client/sync.rs)

/// Quiet window the refresh consumer waits for after the last trigger in a
/// room before refetching its state, so a join burst or a bot publishing many
/// descriptions costs one fetch.
pub const REFRESH_DEBOUNCE: std::time::Duration = std::time::Duration::from_millis(500);

/// Whether a member event for `state_key` should trigger a description
/// refresh: the user must already be a description sender in the room's cached
/// list, and their membership must have actually changed. `prev_membership` is
/// `None` when the server omitted `prev_content`; treated as a change.
pub fn member_event_needs_refresh(
    cached: Option<&[CommandDescription]>,
    state_key: &str,
    prev_membership: Option<&str>,
    new_membership: &str,
) -> bool {
    let Some(cached) = cached else {
        return false;
    };
    if !cached.iter().any(|d| d.sender == state_key) {
        return false;
    }
    prev_membership != Some(new_membership)
}

/// Whether a refresh request for a room should be served: only rooms that
/// already have a cache entry or are active, so a bot event in some unvisited
/// room never costs a full-state fetch.
pub fn should_refresh_room(has_cache_entry: bool, is_active: bool) -> bool {
    has_cache_entry || is_active
}

/// Per-room trailing-edge debounce: each [`note`](Self::note) restarts that
/// room's quiet window; [`take_due`](Self::take_due) yields rooms whose window
/// has elapsed. Time is passed in so the logic is testable.
#[derive(Debug, Default)]
pub struct RefreshDebouncer {
    quiet: std::time::Duration,
    pending: std::collections::HashMap<String, std::time::Instant>,
}

impl RefreshDebouncer {
    pub fn new(quiet: std::time::Duration) -> Self {
        Self {
            quiet,
            pending: Default::default(),
        }
    }

    /// Record a trigger for `room` at `now`, restarting its quiet window.
    pub fn note(&mut self, room: String, now: std::time::Instant) {
        self.pending.insert(room, now + self.quiet);
    }

    /// Earliest deadline among pending rooms, if any.
    pub fn next_deadline(&self) -> Option<std::time::Instant> {
        self.pending.values().min().copied()
    }

    /// Remove and return every room whose deadline is at or before `now`.
    pub fn take_due(&mut self, now: std::time::Instant) -> Vec<String> {
        let due: Vec<String> = self
            .pending
            .iter()
            .filter(|(_, d)| **d <= now)
            .map(|(r, _)| r.clone())
            .collect();
        for r in &due {
            self.pending.remove(r);
        }
        due
    }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn resolve(args: &mut Value) -> Result<(), String> {
        resolve_reference_arguments(
            args,
            &mut |a| match a {
                "#ok:h" => Ok(("!resolved:h".to_owned(), vec!["h".to_owned()])),
                _ => Err(format!("no such alias {a}")),
            },
            &mut |r| vec![format!("route-{}", r.trim_start_matches('!'))],
        )
    }

    #[test]
    fn reference_alias_is_replaced_and_via_seeded() {
        let mut a = json!({"t": {"type": "room_id", "id": "#ok:h"}});
        resolve(&mut a).unwrap();
        assert_eq!(
            a["t"],
            json!({"type": "room_id", "id": "!resolved:h", "via": ["h"]})
        );
    }

    #[test]
    fn reference_missing_via_is_filled_from_route() {
        let mut a = json!({"t": {"type": "room_id", "id": "!r:h", "via": []}});
        resolve(&mut a).unwrap();
        assert_eq!(a["t"]["via"], json!(["route-r:h"]));
    }

    #[test]
    fn reference_existing_via_is_kept() {
        let mut a = json!({"t": {"type": "room_id", "id": "!r:h", "via": ["x"]}});
        resolve(&mut a).unwrap();
        assert_eq!(a["t"]["via"], json!(["x"]));
    }

    #[test]
    fn reference_event_object_keeps_event_id() {
        let mut a = json!({"e": {"type": "event_id", "id": "!r:h", "event_id": "$e:h"}});
        resolve(&mut a).unwrap();
        assert_eq!(a["e"]["event_id"], "$e:h");
        assert_eq!(a["e"]["id"], "!r:h");
        assert_eq!(a["e"]["via"], json!(["route-r:h"]));
    }

    #[test]
    fn reference_arrays_are_walked_and_other_values_untouched() {
        let mut a = json!({
            "rooms": [{"type": "room_id", "id": "#ok:h"}, {"type": "room_id", "id": "!b:h"}],
            "n": 42,
            "s": "#not-a-ref",
            "o": {"type": "other", "id": "#ok:h"},
            "users": ["@a:h"],
        });
        resolve(&mut a).unwrap();
        assert_eq!(a["rooms"][0]["id"], "!resolved:h");
        assert_eq!(a["rooms"][1]["via"], json!(["route-b:h"]));
        assert_eq!(a["n"], 42);
        assert_eq!(a["s"], "#not-a-ref");
        assert_eq!(a["o"], json!({"type": "other", "id": "#ok:h"}));
        assert_eq!(a["users"], json!(["@a:h"]));
    }

    #[test]
    fn reference_alias_failure_is_an_error() {
        let mut a = json!({"t": {"type": "room_id", "id": "#bad:h"}});
        assert_eq!(resolve(&mut a).unwrap_err(), "no such alias #bad:h");
    }

    #[test]
    fn reference_empty_route_leaves_no_via_key() {
        let mut a = json!({"t": {"type": "room_id", "id": "!r:h", "via": []}});
        resolve_reference_arguments(&mut a, &mut |_| Err(String::new()), &mut |_| vec![]).unwrap();
        assert!(a["t"].get("via").is_none());
    }

    #[test]
    fn reference_aliases_lists_only_alias_ids() {
        let mut a =
            json!({"r": [{"type": "room_id", "id": "#a:h"}, {"type": "room_id", "id": "!b:h"}]});
        assert_eq!(reference_aliases(&mut a), vec!["#a:h".to_owned()]);
    }

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
                    "schema": { "schema_type": "primitive", "type": "room_id" },
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
        let d =
            parse_command_description("@draupnir:draupnir.space", "state1", "!room:h", &c).unwrap();
        assert!(d.valid);
        assert_eq!(d.command, "ban");
        assert_eq!(d.parameters.len(), 4);
        assert_eq!(d.description, "An example command with arguments");
        assert!(d.parameters[2].optional); // apply_to_policy
        assert!(!d.parameters[0].optional); // target_room
        assert!(matches!(
            d.parameters[0].schema,
            ParamSchema::Primitive {
                ty: PrimitiveType::RoomId
            }
        ));
    }

    #[test]
    fn legacy_object_schema_is_normalised_to_primitive() {
        let c = json!({
            "command": "x",
            "parameters": [
                { "key": "r", "schema": { "schema_type": "object", "type": "room_id" } },
                { "key": "e", "schema": { "schema_type": "object", "type": "event_id" } }
            ]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(d.valid);
        assert!(matches!(
            d.parameters[0].schema,
            ParamSchema::Primitive {
                ty: PrimitiveType::RoomId
            }
        ));
        assert!(matches!(
            d.parameters[1].schema,
            ParamSchema::Primitive {
                ty: PrimitiveType::EventId
            }
        ));
        let out = serde_json::to_value(&d.parameters[1].schema).unwrap();
        assert_eq!(
            out,
            json!({ "schema_type": "primitive", "type": "event_id" })
        );
    }

    #[test]
    fn array_of_room_id_is_legal() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": {
                    "schema_type": "array",
                    "items": { "schema_type": "primitive", "type": "room_id" }
                }
            }]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(d.valid);
    }

    #[test]
    fn top_level_union_is_legal_and_checked() {
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": { "schema_type": "union", "variants": [
                    { "schema_type": "primitive", "type": "room_id" },
                    { "schema_type": "literal", "value": "all", "literal_type": "string" }
                ] }
            }]
        });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(d.valid);
        assert!(matches!(
            d.parameters[0].schema,
            ParamSchema::Union { ref variants } if variants.len() == 2
        ));

        let bad = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": { "schema_type": "union", "variants": [
                    { "schema_type": "literal", "value": "nope", "literal_type": "integer" }
                ] }
            }]
        });
        assert!(
            !parse_command_description("@bot:h", "s", "!r:h", &bad)
                .unwrap()
                .valid
        );
    }

    #[test]
    fn missing_command_field_fails_to_parse() {
        let c = json!({ "parameters": [] });
        assert!(parse_command_description("@bot:h", "s", "!r:h", &c).is_none());
    }

    #[test]
    fn nested_union_in_top_level_union_fails_to_parse() {
        // Only top-level unions are legal; a union inside a union is not.
        let c = json!({
            "command": "x",
            "parameters": [{
                "key": "p",
                "schema": { "schema_type": "union", "variants": [
                    { "schema_type": "union", "variants": [] }
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
        // The legacy `object` form is only read at the top level of a
        // parameter; it is not accepted as an array item.
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
    fn command_name_outside_the_grammar_marks_invalid() {
        for name in [
            "",
            "Ban",
            "ban user",
            "ban\nnow",
            "\u{202e}ban",
            &"a".repeat(33),
        ] {
            let c = json!({ "command": name });
            let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
            assert!(!d.valid, "{name:?} should be invalid");
        }
    }

    #[test]
    fn command_name_within_the_grammar_is_valid() {
        for name in ["ban", "set-topic", "x_1", &"a".repeat(32)] {
            let c = json!({ "command": name });
            let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
            assert!(d.valid, "{name:?} should be valid");
        }
    }

    #[test]
    fn too_many_parameters_marks_invalid_but_still_parses() {
        let params: Vec<Value> = (0..=MAX_COMMAND_PARAMETERS)
            .map(|i| {
                json!({
                    "key": format!("p{i}"),
                    "schema": { "schema_type": "primitive", "type": "string" }
                })
            })
            .collect();
        let c = json!({ "command": "x", "parameters": params });
        let d = parse_command_description("@bot:h", "s", "!r:h", &c).unwrap();
        assert!(!d.valid);
        assert_eq!(d.parameters.len(), MAX_COMMAND_PARAMETERS + 1);
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
        let content =
            build_invocation_content("plain", Some("<b>html</b>"), &[], false, "cmd", json!({}));
        assert_eq!(content["formatted_body"], "<b>html</b>");
        assert_eq!(content["format"], "org.matrix.custom.html");
    }

    #[test]
    fn build_invocation_content_omits_formatted_body_when_empty() {
        let content = build_invocation_content("plain", Some(""), &[], false, "cmd", json!({}));
        assert!(content.get("formatted_body").is_none());
    }

    fn desc_from(sender: &str) -> CommandDescription {
        parse_command_description(
            sender,
            "k",
            "!r:h",
            &json!({"command": "ban", "description": {"m.text": [{"body": "x"}]}}),
        )
        .expect("valid description")
    }

    #[test]
    fn member_refresh_requires_known_sender_and_change() {
        let cached = vec![desc_from("@bot:h")];
        assert!(member_event_needs_refresh(
            Some(&cached),
            "@bot:h",
            Some("join"),
            "leave"
        ));
        assert!(member_event_needs_refresh(
            Some(&cached),
            "@bot:h",
            None,
            "join"
        ));
        // Same membership (e.g. a displayname-only update).
        assert!(!member_event_needs_refresh(
            Some(&cached),
            "@bot:h",
            Some("join"),
            "join"
        ));
        // Unknown user, and no cache entry at all.
        assert!(!member_event_needs_refresh(
            Some(&cached),
            "@human:h",
            Some("join"),
            "leave"
        ));
        assert!(!member_event_needs_refresh(
            None,
            "@bot:h",
            Some("join"),
            "leave"
        ));
    }

    #[test]
    fn refresh_only_for_cached_or_active_rooms() {
        assert!(should_refresh_room(true, false));
        assert!(should_refresh_room(false, true));
        assert!(!should_refresh_room(false, false));
    }

    #[test]
    fn debouncer_coalesces_and_restarts_window() {
        use std::time::{Duration, Instant};
        let q = Duration::from_millis(500);
        let t0 = Instant::now();
        let mut d = RefreshDebouncer::new(q);
        assert_eq!(d.next_deadline(), None);
        d.note("!a".into(), t0);
        d.note("!b".into(), t0 + Duration::from_millis(100));
        // Re-noting !a restarts its window and does not duplicate it.
        d.note("!a".into(), t0 + Duration::from_millis(300));
        assert_eq!(d.next_deadline(), Some(t0 + Duration::from_millis(600)));
        assert!(d.take_due(t0 + Duration::from_millis(599)).is_empty());
        assert_eq!(d.take_due(t0 + Duration::from_millis(600)), vec!["!b"]);
        assert_eq!(d.take_due(t0 + Duration::from_millis(800)), vec!["!a"]);
        assert_eq!(d.next_deadline(), None);
        assert!(d.take_due(t0 + Duration::from_secs(5)).is_empty());
    }

    #[test]
    fn invocation_stable_key() {
        let c = json!({"m.bot.command": {"command": "ban", "arguments": {"n": 1}}});
        assert_eq!(
            parse_invocation(&c),
            Some(("ban".to_owned(), json!({"n": 1})))
        );
    }

    #[test]
    fn invocation_unstable_key() {
        let c = json!({"org.matrix.msc4391.command": {"command": "ban", "arguments": {}}});
        assert_eq!(parse_invocation(&c), Some(("ban".to_owned(), json!({}))));
    }

    #[test]
    fn invocation_prefers_stable_key() {
        let c = json!({
            "m.bot.command": {"command": "stable", "arguments": {}},
            "org.matrix.msc4391.command": {"command": "unstable", "arguments": {}},
        });
        assert_eq!(parse_invocation(&c).unwrap().0, "stable");
    }

    #[test]
    fn invocation_malformed_stable_falls_back_to_unstable() {
        let c = json!({
            "m.bot.command": {"command": 5},
            "org.matrix.msc4391.command": {"command": "unstable", "arguments": {}},
        });
        assert_eq!(parse_invocation(&c).unwrap().0, "unstable");
    }

    #[test]
    fn invocation_malformed_is_ignored() {
        assert!(parse_invocation(&json!({"m.bot.command": "ban"})).is_none());
        assert!(parse_invocation(&json!({"m.bot.command": {"command": 3}})).is_none());
        assert!(parse_invocation(&json!({"m.bot.command": {"command": ""}})).is_none());
        assert!(
            parse_invocation(&json!({"m.bot.command": {"command": "x", "arguments": [1]}}))
                .is_none()
        );
    }

    #[test]
    fn invocation_missing_is_none_and_missing_arguments_is_empty() {
        assert!(parse_invocation(&json!({"body": "hi"})).is_none());
        let c = json!({"m.bot.command": {"command": "ping"}});
        assert_eq!(parse_invocation(&c), Some(("ping".to_owned(), json!({}))));
    }

    #[test]
    fn might_be_invocation_spots_either_key() {
        assert!(might_be_invocation(r#"{"content":{"m.bot.command":{}}}"#));
        assert!(might_be_invocation(
            r#"{"content":{"org.matrix.msc4391.command":{}}}"#
        ));
        assert!(!might_be_invocation(r#"{"content":{"body":"hi"}}"#));
    }
}
