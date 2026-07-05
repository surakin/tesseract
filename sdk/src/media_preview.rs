//! MSC4278 media-preview controls.
//!
//! Wire format (per [MSC4278]) — the `content` of the
//! `m.media_preview_config` global account-data event (unstable type
//! `io.element.msc4278.media_preview_config`):
//!
//! ```json
//! { "media_previews": "off" | "private" | "on",
//!   "invite_avatars": "off" | "on" }
//! ```
//!
//! - `media_previews` (default `on`): `off` never auto-loads media,
//!   `private` only in non-public rooms, `on` always.
//! - `invite_avatars` (default `on`): `off` hides room/inviter avatars on
//!   pending invites.
//! - An unrecognised value for a *present* key is treated as the safest
//!   option (`off`); an *absent* key falls back to the MSC default.
//!
//! The same content may also appear as a per-room account-data event, in
//! which case a present `media_previews` field overrides the global value
//! for that room's timeline.
//!
//! [MSC4278]: https://github.com/matrix-org/matrix-spec-proposals/pull/4278
//!
//! Self-contained: depends only on `serde_json::Value`, so parse/serialize
//! can be unit-tested without a matrix-sdk client.

use serde_json::{json, Value};

pub const TYPE_STABLE: &str = "m.media_preview_config";
pub const TYPE_UNSTABLE: &str = "io.element.msc4278.media_preview_config";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MediaPreviews {
    Off,
    Private,
    On,
}

impl MediaPreviews {
    /// FFI encoding: 0 = off, 1 = private, 2 = on.
    pub fn to_u8(self) -> u8 {
        match self {
            MediaPreviews::Off => 0,
            MediaPreviews::Private => 1,
            MediaPreviews::On => 2,
        }
    }

    pub fn from_u8(v: u8) -> Self {
        match v {
            0 => MediaPreviews::Off,
            1 => MediaPreviews::Private,
            _ => MediaPreviews::On,
        }
    }

    /// Decode a wire string. Unknown values map to `Off` (MSC: treat
    /// unrecognised values as the safest option).
    pub fn from_wire(s: &str) -> Self {
        match s {
            "off" => MediaPreviews::Off,
            "private" => MediaPreviews::Private,
            "on" => MediaPreviews::On,
            _ => MediaPreviews::Off,
        }
    }

    pub fn to_wire(self) -> &'static str {
        match self {
            MediaPreviews::Off => "off",
            MediaPreviews::Private => "private",
            MediaPreviews::On => "on",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Config {
    pub media_previews: MediaPreviews,
    pub invite_avatars: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            media_previews: MediaPreviews::On,
            invite_avatars: true,
        }
    }
}

/// Read the `media_previews` field from a content object. Returns `None`
/// when the key is absent (caller applies a default), `Some(Off)` when the
/// key is present but not a recognised string.
pub fn parse_media_previews_field(raw: &Value) -> Option<MediaPreviews> {
    raw.get("media_previews").map(|v| match v.as_str() {
        Some(s) => MediaPreviews::from_wire(s),
        None => MediaPreviews::Off,
    })
}

/// Read the `invite_avatars` field. `None` when absent; `Some(true)` only
/// for the exact string `"on"`, `Some(false)` otherwise (including unknown).
pub fn parse_invite_avatars_field(raw: &Value) -> Option<bool> {
    raw.get("invite_avatars")
        .map(|v| matches!(v.as_str(), Some("on")))
}

/// Parse a global config, applying MSC defaults for absent fields. Any
/// structural error (e.g. a non-object root) yields the default config.
pub fn parse_global(raw: &Value) -> Config {
    Config {
        media_previews: parse_media_previews_field(raw).unwrap_or(MediaPreviews::On),
        invite_avatars: parse_invite_avatars_field(raw).unwrap_or(true),
    }
}

/// Serialize a config into the MSC4278 content object.
pub fn serialize(cfg: Config) -> Value {
    json!({
        "media_previews": cfg.media_previews.to_wire(),
        "invite_avatars": if cfg.invite_avatars { "on" } else { "off" },
    })
}

/// Serialize a per-room MSC4278 override. `Some(mode)` yields the single-key
/// content object `{"media_previews": "<wire>"}`; `None` yields an empty
/// object, clearing any existing override so the room inherits the global
/// config again.
///
/// Intentionally never emits `invite_avatars` — Tesseract has no per-room
/// override for that field. Room-account-data PUT is a full-content
/// replace, not a merge; if a future feature adds a per-room `invite_avatars`
/// override, this function must switch to read-modify-write (mirroring
/// `pins.rs`'s `pin_event` pattern for `m.room.pinned_events`) so it doesn't
/// clobber whichever field it doesn't touch.
pub fn serialize_room_override(mode: Option<MediaPreviews>) -> Value {
    match mode {
        Some(m) => json!({ "media_previews": m.to_wire() }),
        None => json!({}),
    }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parse_global_well_formed() {
        let cfg = parse_global(&json!({
            "media_previews": "private",
            "invite_avatars": "off",
        }));
        assert_eq!(cfg.media_previews, MediaPreviews::Private);
        assert!(!cfg.invite_avatars);
    }

    #[test]
    fn parse_global_applies_defaults_for_missing_keys() {
        let cfg = parse_global(&json!({}));
        assert_eq!(cfg.media_previews, MediaPreviews::On);
        assert!(cfg.invite_avatars);
    }

    #[test]
    fn parse_global_treats_unknown_values_as_off() {
        // Present-but-unrecognised string → off / hidden.
        let cfg = parse_global(&json!({
            "media_previews": "sometimes",
            "invite_avatars": "maybe",
        }));
        assert_eq!(cfg.media_previews, MediaPreviews::Off);
        assert!(!cfg.invite_avatars);
    }

    #[test]
    fn parse_global_treats_wrong_type_as_off() {
        let cfg = parse_global(&json!({
            "media_previews": 3,
            "invite_avatars": true,
        }));
        assert_eq!(cfg.media_previews, MediaPreviews::Off);
        assert!(!cfg.invite_avatars);
    }

    #[test]
    fn parse_global_tolerates_non_object_root() {
        assert_eq!(parse_global(&Value::Null), Config::default());
        assert_eq!(parse_global(&json!("nope")), Config::default());
    }

    #[test]
    fn media_previews_field_distinguishes_absent_from_off() {
        assert_eq!(parse_media_previews_field(&json!({})), None);
        assert_eq!(
            parse_media_previews_field(&json!({ "media_previews": "off" })),
            Some(MediaPreviews::Off)
        );
        assert_eq!(
            parse_media_previews_field(&json!({ "media_previews": "on" })),
            Some(MediaPreviews::On)
        );
    }

    #[test]
    fn round_trips_through_serialize() {
        for mp in [
            MediaPreviews::Off,
            MediaPreviews::Private,
            MediaPreviews::On,
        ] {
            for inv in [true, false] {
                let cfg = Config {
                    media_previews: mp,
                    invite_avatars: inv,
                };
                assert_eq!(parse_global(&serialize(cfg)), cfg);
            }
        }
    }

    #[test]
    fn u8_round_trip() {
        for mp in [
            MediaPreviews::Off,
            MediaPreviews::Private,
            MediaPreviews::On,
        ] {
            assert_eq!(MediaPreviews::from_u8(mp.to_u8()), mp);
        }
        // Out-of-range u8 decodes to On (the permissive default).
        assert_eq!(MediaPreviews::from_u8(7), MediaPreviews::On);
    }

    #[test]
    fn serialize_room_override_some_is_single_key() {
        let v = serialize_room_override(Some(MediaPreviews::Private));
        assert_eq!(v, json!({"media_previews": "private"}));
        assert!(v.get("invite_avatars").is_none());
    }

    #[test]
    fn serialize_room_override_none_is_empty() {
        assert_eq!(serialize_room_override(None), json!({}));
    }
}
