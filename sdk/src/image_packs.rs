//! MSC2545 image pack aggregation.
//!
//! Surfaces four sources as a single `Vec<ImagePack>` cache:
//!   * account_data `im.ponies.user_emotes` / `m.image_pack` — the user's
//!     personal pack (de facto extension; not in merged MSC2545 text but used
//!     by Element/Cinny and our "Saved Stickers" feature relies on it).
//!   * account_data `im.ponies.emote_rooms` / `m.image_pack.rooms` — the list
//!     of (room_id, state_key) pairs the user has globally enabled.
//!   * room state `im.ponies.room_emotes` / `m.room.image_pack` at each
//!     referenced state_key.
//!   * room state `im.ponies.room_emotes` / `m.room.image_pack` from ALL other
//!     joined rooms (implicit membership packs, deduped against the explicit
//!     subscription list above). Display name falls back to the room name when
//!     the pack doesn't carry its own `pack.display_name`.
//!
//! Per spec: when `pack.usage` is absent/empty, BOTH `sticker` and `emoticon`
//! are allowed. Per-image `usage` overrides pack-level `usage` when present.

use serde::{Deserialize, Serialize};
use serde_json::Value;

fn empty_object() -> Value {
    Value::Object(serde_json::Map::new())
}

/// Minimal `mxc://server/media-id` validator. Pack URLs come from arbitrary
/// (possibly hostile) homeservers and flow over the FFI to the C++ image
/// cache; anything that is not a well-formed mxc URI (`http://`,
/// `javascript:`, …) must never reach that layer.
pub fn is_valid_mxc(s: &str) -> bool {
    let Some(rest) = s.strip_prefix("mxc://") else { return false };
    let mut parts = rest.splitn(2, '/');
    match (parts.next(), parts.next()) {
        (Some(server), Some(media_id)) => {
            !server.is_empty() && !media_id.is_empty() && !media_id.contains('/')
        }
        _ => false,
    }
}

/// Wire representation of a single image entry in an MSC2545 image pack
/// (`images.<shortcode>`). Unknown keys are round-tripped through `extra`
/// so data written by other clients survives a Tesseract upsert.
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct PackImage {
    pub url: String,

    #[serde(skip_serializing_if = "String::is_empty", default)]
    pub body: String,

    #[serde(default = "empty_object")]
    pub info: Value,

    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub usage: Option<Vec<String>>,

    #[serde(rename = "im.tesseract.favorite",
            skip_serializing_if = "Option::is_none", default)]
    pub favorite: Option<bool>,

    #[serde(flatten)]
    pub extra: serde_json::Map<String, Value>,
}

/// Convert a `usage` array of strings (from `PackImage`) to a bitmask.
fn usage_strs_to_mask(strs: &[String]) -> u8 {
    if strs.is_empty() { return USAGE_ANY; }
    let mut mask = 0u8;
    for s in strs {
        match s.as_str() {
            "sticker"  => mask |= USAGE_STICKER,
            "emoticon" => mask |= USAGE_EMOTICON,
            _ => {}
        }
    }
    if mask == 0 { USAGE_ANY } else { mask }
}

pub const USAGE_STICKER:  u8 = 1 << 0;
pub const USAGE_EMOTICON: u8 = 1 << 1;
pub const USAGE_ANY:      u8 = USAGE_STICKER | USAGE_EMOTICON;

pub const TYPE_USER_PACK_UNSTABLE:   &str = "im.ponies.user_emotes";
pub const TYPE_USER_PACK_STABLE:     &str = "m.image_pack";
pub const TYPE_EMOTE_ROOMS_UNSTABLE: &str = "im.ponies.emote_rooms";
pub const TYPE_EMOTE_ROOMS_STABLE:   &str = "m.image_pack.rooms";
pub const TYPE_ROOM_PACK_UNSTABLE:   &str = "im.ponies.room_emotes";
pub const TYPE_ROOM_PACK_STABLE:     &str = "m.room.image_pack";

/// Tesseract-private flag stored alongside image entries to mark favorites.
/// Spec is open about unknown keys; other clients ignore it.
pub const FAVORITE_KEY: &str = "im.tesseract.favorite";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PackSource {
    User,
    Room { room_id: String, state_key: String },
}

#[derive(Debug, Clone)]
pub struct ImageEntry {
    pub shortcode: String,
    pub url:       String,
    pub body:      String,
    /// JSON-serialised `info` object — `"{}"` when the entry has no info.
    pub info_json: String,
    /// Bitmask of `USAGE_STICKER` and `USAGE_EMOTICON`. Always non-zero.
    pub usage:     u8,
    pub favorite:  bool,
}

#[derive(Debug, Clone)]
pub struct ImagePack {
    pub id:           String,
    pub display_name: String,
    pub avatar_url:   String,
    pub attribution:  String,
    pub usage:        u8,
    pub source:       PackSource,
    pub images:       Vec<ImageEntry>,
}

impl ImagePack {
    pub fn source_kind(&self) -> &'static str {
        match self.source { PackSource::User => "user", PackSource::Room { .. } => "room" }
    }
    pub fn source_room(&self) -> &str {
        match &self.source { PackSource::Room { room_id, .. } => room_id, _ => "" }
    }
    pub fn source_state_key(&self) -> &str {
        match &self.source { PackSource::Room { state_key, .. } => state_key, _ => "" }
    }
}

/// Decode a `usage` JSON array into a bitmask. An empty array is treated as
/// "any usage allowed" per MSC2545; an array containing only unknown values
/// is treated the same way (forwards-compat).
fn usage_array_to_mask(arr: &[Value]) -> u8 {
    if arr.is_empty() { return USAGE_ANY; }
    let mut mask = 0u8;
    for v in arr {
        if let Some(s) = v.as_str() {
            match s {
                "sticker"  => mask |= USAGE_STICKER,
                "emoticon" => mask |= USAGE_EMOTICON,
                _ => {}
            }
        }
    }
    if mask == 0 { USAGE_ANY } else { mask }
}

/// Parse the `content` of an image pack event (user account_data, or
/// `m.room.image_pack` state) into an `ImagePack`.
///
/// Returns `None` when `images` is missing or not an object — per spec
/// `images` is required; a content lacking it is malformed and the pack is
/// discarded rather than surfaced as empty.
pub fn parse_pack_content(
    id:      String,
    source:  PackSource,
    content: &Value,
) -> Option<ImagePack> {
    let images_obj = content.get("images")?.as_object()?;
    let pack_obj   = content.get("pack").and_then(Value::as_object);

    let display_name = pack_obj
        .and_then(|p| p.get("display_name").and_then(Value::as_str))
        .unwrap_or("")
        .to_owned();
    let avatar_url = pack_obj
        .and_then(|p| p.get("avatar_url").and_then(Value::as_str))
        .filter(|u| is_valid_mxc(u))
        .unwrap_or("")
        .to_owned();
    let attribution = pack_obj
        .and_then(|p| p.get("attribution").and_then(Value::as_str))
        .unwrap_or("")
        .to_owned();

    let pack_usage = pack_obj
        .and_then(|p| p.get("usage").and_then(Value::as_array))
        .map(|a| usage_array_to_mask(a.as_slice()))
        .unwrap_or(USAGE_ANY);

    let mut entries: Vec<ImageEntry> = Vec::with_capacity(images_obj.len());
    for (shortcode, img) in images_obj {
        let Ok(pack_img) = serde_json::from_value::<PackImage>(img.clone()) else { continue };
        if !is_valid_mxc(&pack_img.url) { continue; }
        let info_json = serde_json::to_string(&pack_img.info).unwrap_or_else(|_| "{}".to_owned());
        let usage = pack_img.usage.as_deref()
            .map(usage_strs_to_mask)
            .unwrap_or(pack_usage);
        entries.push(ImageEntry {
            shortcode: shortcode.clone(),
            url:       pack_img.url,
            body:      pack_img.body,
            info_json,
            usage,
            favorite:  pack_img.favorite.unwrap_or(false),
        });
    }

    Some(ImagePack {
        id,
        display_name,
        avatar_url,
        attribution,
        usage: pack_usage,
        source,
        images: entries,
    })
}

/// Iterate every `(room_id, state_key)` pair referenced by an
/// `im.ponies.emote_rooms` / `m.image_pack.rooms` content. Tolerates both the
/// formal MSC2545 shape (`rooms: { "!room": { "state_key": {} } }`) and the
/// degenerate shape some clients write where the inner value is `null`.
pub fn iter_emote_rooms(content: &Value) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let Some(rooms) = content.get("rooms").and_then(Value::as_object) else { return out; };
    for (room_id, val) in rooms {
        if let Some(inner) = val.as_object() {
            for state_key in inner.keys() {
                out.push((room_id.clone(), state_key.clone()));
            }
        } else {
            // Non-object inner value — treat as a single empty-state-key
            // reference. Element historically wrote this shape briefly.
            out.push((room_id.clone(), String::new()));
        }
    }
    out
}

/// Build a synthetic pack id. `user` for the global user pack; `room:!id/key`
/// for room packs so each (room, state_key) maps to a unique id stable across
/// rebuilds.
pub fn pack_id_for(source: &PackSource) -> String {
    match source {
        PackSource::User => "user".to_owned(),
        PackSource::Room { room_id, state_key } => {
            // Length-prefix room_id so no (room_id, state_key) pair can be
            // confused with another even when state_key contains the
            // separator. (Matrix room IDs never contain `/`, but state keys
            // are arbitrary UTF-8.)
            format!("room:{}:{}/{}", room_id.len(), room_id, state_key)
        }
    }
}

/// Merge an image into the user-pack JSON content under `shortcode`, creating
/// the `images` and `pack` objects when absent. Sets `pack.display_name` to
/// `default_pack_name` when the user pack does not already carry one — this
/// implements the "Saved Stickers" pack name from the user-facing flow.
///
/// Used by both `save_sticker_to_user_pack` and `toggle_favorite_sticker`.
/// Returns the mutated JSON.
pub fn upsert_image_into_user_pack(
    mut content: Value,
    shortcode: &str,
    url: &str,
    body: &str,
    info_json: &str,
    favorite: Option<bool>,
    default_pack_name: &str,
) -> Value {
    let info: Value = serde_json::from_str(info_json)
        .unwrap_or_else(|_| Value::Object(serde_json::Map::new()));

    if !content.is_object() {
        content = Value::Object(serde_json::Map::new());
    }
    let obj = content.as_object_mut().expect("just ensured object");

    // Ensure `pack.display_name` is set on first write.
    {
        let pack_entry = obj.entry("pack").or_insert_with(|| Value::Object(serde_json::Map::new()));
        if let Some(pack) = pack_entry.as_object_mut() {
            pack.entry("display_name").or_insert_with(|| Value::String(default_pack_name.to_owned()));
        }
    }

    let images_entry = obj.entry("images").or_insert_with(|| Value::Object(serde_json::Map::new()));
    let Some(images) = images_entry.as_object_mut() else {
        return content;
    };

    // Read the existing entry so unknown keys written by other clients survive
    // the round-trip. Then overwrite only the fields we own.
    let mut pack_img: PackImage = images
        .get(shortcode)
        .and_then(|v| serde_json::from_value(v.clone()).ok())
        .unwrap_or_default();

    pack_img.url  = url.to_owned();
    if !body.is_empty() { pack_img.body = body.to_owned(); }
    pack_img.info = info;
    if let Some(fav) = favorite { pack_img.favorite = Some(fav); }

    images.insert(
        shortcode.to_owned(),
        serde_json::to_value(pack_img).expect("PackImage is always serialisable"),
    );

    content
}

/// Return true if any entry in the `images` map of `content` already has `url`.
pub fn pack_contains_url(content: &Value, url: &str) -> bool {
    content
        .get("images")
        .and_then(Value::as_object)
        .map(|imgs| imgs.values().any(|v| v.get("url").and_then(Value::as_str) == Some(url)))
        .unwrap_or(false)
}

/// Reverse operation of `upsert_image_into_user_pack` for the favorite flag.
/// Toggles `im.tesseract.favorite` on the first entry whose `url` matches.
/// Returns `(new_content, new_state)`. When no matching entry exists, the
/// content is returned unchanged with `false`.
pub fn toggle_favorite_in_user_pack(
    mut content: Value,
    url: &str,
) -> (Value, bool) {
    let Some(obj) = content.as_object_mut() else { return (content, false) };
    let Some(images) = obj.get_mut("images").and_then(Value::as_object_mut) else {
        return (content, false);
    };
    let mut new_state = false;
    for (_short, img) in images.iter_mut() {
        let Some(img_obj) = img.as_object_mut() else { continue };
        let Some(entry_url) = img_obj.get("url").and_then(Value::as_str) else { continue };
        if entry_url != url { continue; }
        let cur = img_obj.get(FAVORITE_KEY).and_then(Value::as_bool).unwrap_or(false);
        new_state = !cur;
        img_obj.insert(FAVORITE_KEY.to_owned(), Value::Bool(new_state));
        return (content, new_state);
    }
    (content, new_state)
}

/// Suggest a slugified, collision-free shortcode for a sticker being added
/// to the user pack. Mostly lowercase ASCII alnum + underscore; falls back to
/// `sticker_N` when the body is empty/non-ASCII.
pub fn suggest_shortcode(body: &str, existing: &serde_json::Map<String, Value>) -> String {
    let base: String = body
        .chars()
        .filter_map(|c| {
            if c.is_ascii_alphanumeric() { Some(c.to_ascii_lowercase()) }
            else if c == ' ' || c == '_' || c == '-' { Some('_') }
            else { None }
        })
        .collect();
    let base = if base.is_empty() { "sticker".to_owned() } else { base };
    if !existing.contains_key(&base) { return base; }
    for n in 2..=10_000 {
        let candidate = format!("{base}_{n}");
        if !existing.contains_key(&candidate) { return candidate; }
    }
    // Pathological pack (10k collisions). Fall back to a value that cannot
    // already exist rather than overwriting an entry or looping for billions
    // of iterations.
    format!(
        "{base}_{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    )
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn missing_usage_is_any() {
        let c = json!({ "images": { "a": { "url": "mxc://h/a" } } });
        let p = parse_pack_content("p".into(), PackSource::User, &c).unwrap();
        assert_eq!(p.usage, USAGE_ANY);
        assert_eq!(p.images[0].usage, USAGE_ANY);
    }

    #[test]
    fn empty_usage_array_is_any() {
        let c = json!({ "images": { "a": { "url": "mxc://h/a", "usage": [] } }, "pack": { "usage": [] } });
        let p = parse_pack_content("p".into(), PackSource::User, &c).unwrap();
        assert_eq!(p.usage, USAGE_ANY);
        assert_eq!(p.images[0].usage, USAGE_ANY);
    }

    #[test]
    fn explicit_sticker_only() {
        let c = json!({
            "images": { "a": { "url": "mxc://h/a" } },
            "pack":   { "usage": ["sticker"] }
        });
        let p = parse_pack_content("p".into(), PackSource::User, &c).unwrap();
        assert_eq!(p.usage, USAGE_STICKER);
        // Inherited from pack.
        assert_eq!(p.images[0].usage, USAGE_STICKER);
    }

    #[test]
    fn image_usage_overrides_pack_usage() {
        let c = json!({
            "images": {
                "a": { "url": "mxc://h/a", "usage": ["emoticon"] },
                "b": { "url": "mxc://h/b" }
            },
            "pack": { "usage": ["sticker"] }
        });
        let p = parse_pack_content("p".into(), PackSource::User, &c).unwrap();
        assert_eq!(p.images.len(), 2);
        let a = p.images.iter().find(|i| i.shortcode == "a").unwrap();
        let b = p.images.iter().find(|i| i.shortcode == "b").unwrap();
        assert_eq!(a.usage, USAGE_EMOTICON);
        assert_eq!(b.usage, USAGE_STICKER);
    }

    #[test]
    fn missing_images_object_returns_none() {
        let c = json!({ "pack": { "display_name": "x" } });
        assert!(parse_pack_content("p".into(), PackSource::User, &c).is_none());
    }

    #[test]
    fn entries_without_url_are_dropped() {
        let c = json!({ "images": { "a": {}, "b": { "url": "" }, "c": { "url": "mxc://h/c" } } });
        let p = parse_pack_content("p".into(), PackSource::User, &c).unwrap();
        assert_eq!(p.images.len(), 1);
        assert_eq!(p.images[0].shortcode, "c");
    }

    #[test]
    fn iter_emote_rooms_yields_all_state_keys() {
        let c = json!({
            "rooms": {
                "!a:h": { "": {}, "pack1": {} },
                "!b:h": { "main": {} }
            }
        });
        let mut out = iter_emote_rooms(&c);
        out.sort();
        assert_eq!(out, vec![
            ("!a:h".into(), "".into()),
            ("!a:h".into(), "pack1".into()),
            ("!b:h".into(), "main".into()),
        ]);
    }

    #[test]
    fn iter_emote_rooms_handles_null_inner() {
        let c = json!({ "rooms": { "!a:h": null } });
        let out = iter_emote_rooms(&c);
        assert_eq!(out, vec![("!a:h".into(), "".into())]);
    }

    #[test]
    fn pack_id_user_is_stable() {
        assert_eq!(pack_id_for(&PackSource::User), "user");
        assert_eq!(
            pack_id_for(&PackSource::Room { room_id: "!a:h".into(), state_key: "k".into() }),
            "room:4:!a:h/k"
        );
        // Length prefix keeps ambiguous (room_id, state_key) splits distinct.
        assert_ne!(
            pack_id_for(&PackSource::Room { room_id: "!a:h".into(), state_key: "b/c".into() }),
            pack_id_for(&PackSource::Room { room_id: "!a:h/b".into(), state_key: "c".into() }),
        );
    }

    #[test]
    fn upsert_creates_pack_and_image() {
        let content = upsert_image_into_user_pack(
            Value::Null,
            "happy",
            "mxc://h/happy",
            "Happy",
            r#"{"w":256,"h":256}"#,
            None,
            "Saved Stickers",
        );
        let pack = content.get("pack").unwrap().as_object().unwrap();
        assert_eq!(pack.get("display_name").unwrap().as_str().unwrap(), "Saved Stickers");
        let img = content.pointer("/images/happy").unwrap().as_object().unwrap();
        assert_eq!(img.get("url").unwrap().as_str().unwrap(), "mxc://h/happy");
        assert_eq!(img.get("body").unwrap().as_str().unwrap(), "Happy");
        assert_eq!(img.get("info").unwrap().get("w").unwrap().as_u64().unwrap(), 256);
    }

    #[test]
    fn upsert_preserves_existing_pack_name() {
        let initial = json!({ "pack": { "display_name": "Existing" }, "images": {} });
        let content = upsert_image_into_user_pack(
            initial,
            "x",
            "mxc://h/x",
            "",
            "{}",
            None,
            "Saved Stickers",
        );
        assert_eq!(
            content.pointer("/pack/display_name").unwrap().as_str().unwrap(),
            "Existing"
        );
    }

    #[test]
    fn favorite_toggle_round_trip() {
        let c = json!({ "images": { "a": { "url": "mxc://h/a" } } });
        let (c, on) = toggle_favorite_in_user_pack(c, "mxc://h/a");
        assert!(on);
        assert_eq!(c.pointer("/images/a/im.tesseract.favorite").unwrap().as_bool().unwrap(), true);
        let (c, on) = toggle_favorite_in_user_pack(c, "mxc://h/a");
        assert!(!on);
        assert_eq!(c.pointer("/images/a/im.tesseract.favorite").unwrap().as_bool().unwrap(), false);
    }

    #[test]
    fn favorite_toggle_unknown_url_is_noop() {
        let c = json!({ "images": { "a": { "url": "mxc://h/a" } } });
        let (c2, on) = toggle_favorite_in_user_pack(c.clone(), "mxc://h/zzz");
        assert!(!on);
        assert_eq!(c2, c);
    }

    #[test]
    fn suggest_shortcode_uses_body_when_unique() {
        let m = serde_json::Map::new();
        assert_eq!(suggest_shortcode("Happy Face", &m), "happy_face");
    }

    #[test]
    fn suggest_shortcode_appends_suffix_on_collision() {
        let mut m = serde_json::Map::new();
        m.insert("happy".into(), Value::Null);
        m.insert("happy_2".into(), Value::Null);
        assert_eq!(suggest_shortcode("happy", &m), "happy_3");
    }

    #[test]
    fn pack_contains_url_detects_existing() {
        let c = json!({ "images": { "a": { "url": "mxc://h/a" } } });
        assert!(pack_contains_url(&c, "mxc://h/a"));
        assert!(!pack_contains_url(&c, "mxc://h/b"));
    }

    #[test]
    fn pack_contains_url_empty_pack() {
        assert!(!pack_contains_url(&Value::Object(serde_json::Map::new()), "mxc://h/a"));
        assert!(!pack_contains_url(&Value::Null, "mxc://h/a"));
    }

    #[test]
    fn suggest_shortcode_handles_empty_body() {
        let m = serde_json::Map::new();
        assert_eq!(suggest_shortcode("", &m), "sticker");
        assert_eq!(suggest_shortcode("🎉🎉🎉", &m), "sticker");
    }
}
