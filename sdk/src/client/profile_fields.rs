//! Extended profile fields: pronouns / timezone / biography (MSC4133) and
//! status / call (MSC4426).
//!
//! Read and write both go through matrix-sdk's typed extended-profile-field
//! API (`Account::fetch_user_profile_of` / `set_profile_field` /
//! `delete_profile_field`), which negotiates the stable-vs-`uk.tcpip.msc4133`
//! path itself. `profile_fields_prefix` (populated in `session.rs`) is still
//! consulted as a cheap "server supports profile-field writes" gate — a `None`
//! there rejects writes with an error result before any request is sent.

use super::ClientFfi;
#[cfg(not(test))]
use super::{err, ok};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Bounds on remote profile values, applied as they are parsed. A profile can
// carry tens of kilobytes of extended fields, and the profile card and the
// timeline status line only ever show a short prefix of each. The status
// limits are the ones ruma documents for MSC4426.
const MAX_PRONOUN_ENTRIES: usize = 16;
const MAX_PRONOUN_BYTES: usize = 128;
const MAX_TAG_BYTES: usize = 32;
const MAX_TZ_BYTES: usize = 64;
const MAX_BIOGRAPHY_BYTES: usize = 2048;
const MAX_STATUS_TEXT_BYTES: usize = 256;
const MAX_STATUS_EMOJI_BYTES: usize = 32;

/// The longest prefix of `s` that is at most `max` bytes, cut on a char boundary.
fn clamp_bytes(s: &str, max: usize) -> String {
    let mut end = s.len().min(max);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}

/// One `m.pronouns` array entry: a language-tagged pronoun summary plus an
/// optional grammatical gender (MSC4247). `grammatical_gender` is an empty
/// string when the entry doesn't specify one.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct PronounEntry {
    pub language: String,
    pub summary: String,
    pub grammatical_gender: String,
}

/// Parse every pronoun entry from either the stable `m.pronouns` or the
/// unstable `io.fsky.nyx.pronouns` key in `j` (stable takes priority; the two
/// are never merged).
///
/// The field value is either:
/// - A JSON array of `{summary, language, grammatical_gender?}` objects
///   → return every entry, in order.
/// - A plain string (graceful future-compat)
///   → return it as a single entry with an empty `language`.
fn parse_pronouns(j: &serde_json::Value) -> Vec<PronounEntry> {
    for key in &["m.pronouns", "io.fsky.nyx.pronouns"] {
        let v = &j[*key];
        if v.is_null() {
            continue;
        }
        if let Some(s) = v.as_str() {
            if s.is_empty() {
                return Vec::new();
            }
            return vec![PronounEntry {
                language: String::new(),
                summary: clamp_bytes(s, MAX_PRONOUN_BYTES),
                grammatical_gender: String::new(),
            }];
        }
        if let Some(arr) = v.as_array() {
            return arr
                .iter()
                .filter_map(|o| {
                    let summary = o["summary"].as_str()?;
                    Some(PronounEntry {
                        language: clamp_bytes(o["language"].as_str().unwrap_or(""), MAX_TAG_BYTES),
                        summary: clamp_bytes(summary, MAX_PRONOUN_BYTES),
                        grammatical_gender: clamp_bytes(
                            o["grammatical_gender"].as_str().unwrap_or(""),
                            MAX_TAG_BYTES,
                        ),
                    })
                })
                .take(MAX_PRONOUN_ENTRIES)
                .collect();
        }
    }
    Vec::new()
}

/// Parse the timezone from either `m.tz` or `us.cloke.msc4175.tz`.
fn parse_tz(j: &serde_json::Value) -> String {
    for key in &["m.tz", "us.cloke.msc4175.tz"] {
        if let Some(s) = j[*key].as_str() {
            return clamp_bytes(s, MAX_TZ_BYTES);
        }
    }
    String::new()
}

/// Parse the biography body from either `m.biography` or `gay.fomx.biography`.
///
/// Structure: `{m.text: [{body, mimetype?}]}`. We take the first entry where
/// `mimetype` is absent or `"text/plain"`.
fn parse_biography(j: &serde_json::Value) -> String {
    for key in &["m.biography", "gay.fomx.biography"] {
        let v = &j[*key];
        if v.is_null() {
            continue;
        }
        if let Some(arr) = v["m.text"].as_array() {
            for entry in arr {
                let mime = entry.get("mimetype").and_then(|m| m.as_str());
                if mime.is_none() || mime == Some("text/plain") {
                    if let Some(body) = entry["body"].as_str() {
                        return clamp_bytes(body, MAX_BIOGRAPHY_BYTES);
                    }
                }
            }
        }
        // Plain-string fallback for servers that omit the m.text wrapper.
        if let Some(s) = v.as_str() {
            return clamp_bytes(s, MAX_BIOGRAPHY_BYTES);
        }
    }
    String::new()
}

/// The MSC4426 status profile key written by the settings editor.
const STATUS_KEY: &str = "org.matrix.msc4426.status";

/// `(emoji, text)` from the editor's `{ "text": .., "emoji": .. }` value;
/// either part defaults to an empty string.
fn status_parts(v: &serde_json::Value) -> (String, String) {
    let part = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_owned();
    (part("emoji"), part("text"))
}

/// The user's MSC4426 `status` via the typed ruma accessor. `None` when the
/// field is absent, malformed, or has neither text nor emoji. Only the
/// unstable `org.matrix.msc4426.status` key is read.
fn extract_status(
    profile: &matrix_sdk::ruma::profile::UserProfile,
) -> Option<matrix_sdk::ruma::profile::StatusProfileField> {
    use matrix_sdk::ruma::profile::Status;
    profile
        .get_static::<Status>()
        .ok()
        .flatten()
        .filter(|s| !(s.text.is_empty() && s.emoji.is_empty()))
        .map(|s| {
            matrix_sdk::ruma::profile::StatusProfileField::new(
                clamp_bytes(&s.text, MAX_STATUS_TEXT_BYTES),
                clamp_bytes(&s.emoji, MAX_STATUS_EMOJI_BYTES),
            )
        })
}

/// `call_joined_ts` (unix seconds) from the typed MSC4426 `call` field.
fn extract_call_joined_ts(profile: &matrix_sdk::ruma::profile::UserProfile) -> Option<u64> {
    use matrix_sdk::ruma::profile::Call;
    profile
        .get_static::<Call>()
        .ok()
        .flatten()
        .and_then(|c| c.call_joined_ts)
        .map(|t| u64::from(t.get()))
}

// ---------------------------------------------------------------------------
// Production implementations
// ---------------------------------------------------------------------------

#[cfg(not(test))]
impl ClientFfi {
    pub fn get_extended_profile_async(&self, request_id: u64, user_id: &str) {
        let empty_json = r#"{"exists":false,"user_id":"","display_name":"","avatar_url":"","pronouns":[],"tz":"","biography":"","status_text":"","status_emoji":"","call_joined_ts":0}"#;

        macro_rules! deliver_empty {
            () => {{
                if let Some(ref h) = self.handler {
                    h.lock().on_extended_profile_ready(request_id, empty_json);
                }
                return;
            }};
        }

        if user_id.is_empty() {
            deliver_empty!();
        }
        let Some(client) = self.client.as_ref() else {
            deliver_empty!();
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(user_id) else {
            deliver_empty!();
        };
        let client = client.clone();
        let handler = self.handler.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        let user_id_owned = user_id.to_owned();

        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "profile_fields/get_extended_profile".to_string(),
            );

            let payload = async {
                let resp = client.account().fetch_user_profile_of(&uid).await.ok()?;
                // Reassemble a plain JSON object so the stable/unstable-key
                // parsers below can index it the way they always have.
                let j = serde_json::Value::Object(
                    resp.iter().map(|(k, v)| (k.clone(), v.clone())).collect(),
                );

                let display_name = j["displayname"]
                    .as_str()
                    .filter(|s| !s.is_empty())
                    .map(str::to_owned)
                    .unwrap_or_else(|| {
                        user_id_owned
                            .split(':')
                            .next()
                            .and_then(|s| s.strip_prefix('@'))
                            .unwrap_or(&user_id_owned)
                            .to_owned()
                    });
                let avatar_url = j["avatar_url"].as_str().unwrap_or("").to_owned();
                let pronouns = parse_pronouns(&j);
                let tz = parse_tz(&j);
                let biography = parse_biography(&j);
                let status = extract_status(&resp.data);
                let call_joined_ts = extract_call_joined_ts(&resp.data).unwrap_or(0);

                Some(
                    serde_json::json!({
                        "exists": true,
                        "user_id": user_id_owned,
                        "display_name": display_name,
                        "avatar_url": avatar_url,
                        "pronouns": pronouns,
                        "tz": tz,
                        "biography": biography,
                        "status_text": status.as_ref().map(|s| s.text.as_str()).unwrap_or(""),
                        "status_emoji": status.as_ref().map(|s| s.emoji.as_str()).unwrap_or(""),
                        "call_joined_ts": call_joined_ts,
                    })
                    .to_string(),
                )
            }
            .await;

            let json = payload.as_deref().unwrap_or(empty_json);
            if let Some(h) = handler {
                h.lock().on_extended_profile_ready(request_id, json);
            }
        });
    }

    /// Single async entry point for both set and delete. Branches on
    /// `value_json == "null"` (delete) vs any other value (set). Spawns the
    /// HTTP call on the tokio runtime and fires
    /// `on_profile_field_result(request_id, key, ok, message)` on completion.
    /// Does not pin a C++ worker thread.
    #[cfg(not(test))]
    pub fn set_or_delete_profile_field_async(&self, request_id: u64, key: &str, value_json: &str) {
        if key.is_empty() {
            if let Some(ref h) = self.handler {
                h.lock()
                    .on_profile_field_result(request_id, key, false, "key must not be empty");
            }
            return;
        }

        // MSC4426 status/call ride on the same MSC4133 write endpoint, so the
        // prefix slot doubles as a "server supports profile-field writes" gate.
        // matrix-sdk negotiates the actual stable-vs-unstable path itself.
        if self.profile_fields_prefix.read().unwrap().is_none() {
            if let Some(ref h) = self.handler {
                h.lock().on_profile_field_result(
                    request_id,
                    key,
                    false,
                    "server does not support MSC4133 profile field writes",
                );
            }
            return;
        }

        let Some(client) = self.client.as_ref() else {
            if let Some(ref h) = self.handler {
                h.lock()
                    .on_profile_field_result(request_id, key, false, "not logged in");
            }
            return;
        };

        let is_delete = value_json == "null";
        let value: Option<serde_json::Value> = if is_delete {
            None
        } else {
            match serde_json::from_str(value_json) {
                Ok(v) => Some(v),
                Err(e) => {
                    if let Some(ref h) = self.handler {
                        h.lock().on_profile_field_result(
                            request_id,
                            key,
                            false,
                            &format!("invalid JSON for field value: {e}"),
                        );
                    }
                    return;
                }
            }
        };

        let client = client.clone();
        let handler = self.handler.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        let key_owned = key.to_owned();

        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                if is_delete {
                    format!("profile_fields/delete/{key_owned}")
                } else {
                    format!("profile_fields/set/{key_owned}")
                },
            );

            use matrix_sdk::ruma::profile::{ProfileFieldName, ProfileFieldValue};

            // MSC4426 status goes through matrix-sdk's typed setters.
            let result = if key_owned == STATUS_KEY {
                let outcome = match &value {
                    None => client.account().clear_status().await,
                    Some(v) => {
                        let (emoji, text) = status_parts(v);
                        client.account().set_status(emoji, text).await
                    }
                };
                match outcome {
                    Ok(()) => ok(""),
                    Err(e) => err(format!("failed to update status: {e}")),
                }
            } else if is_delete {
                match client
                    .account()
                    .delete_profile_field(ProfileFieldName::from(key_owned.as_str()))
                    .await
                {
                    Ok(()) => ok(""),
                    Err(e) => err(format!("failed to delete profile field: {e}")),
                }
            } else {
                let value = value.expect("non-delete path parsed a value above");
                match ProfileFieldValue::new(&key_owned, value) {
                    Ok(v) => match client.account().set_profile_field(v).await {
                        Ok(()) => ok(""),
                        Err(e) => err(format!("failed to set profile field: {e}")),
                    },
                    Err(e) => err(format!("invalid value for profile field: {e}")),
                }
            };

            if let Some(h) = handler {
                h.lock().on_profile_field_result(
                    request_id,
                    &key_owned,
                    result.ok,
                    &result.message,
                );
            }
        });
    }
}

// ---------------------------------------------------------------------------
// Test stubs
// ---------------------------------------------------------------------------

#[cfg(test)]
impl ClientFfi {
    pub fn get_extended_profile_async(&self, _request_id: u64, _user_id: &str) {}

    pub fn set_or_delete_profile_field_async(
        &self,
        _request_id: u64,
        _key: &str,
        _value_json: &str,
    ) {
    }
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::{
        clamp_bytes, extract_call_joined_ts, extract_status, parse_biography, parse_pronouns,
        parse_tz, PronounEntry, MAX_BIOGRAPHY_BYTES, MAX_PRONOUN_BYTES, MAX_PRONOUN_ENTRIES,
        MAX_STATUS_EMOJI_BYTES, MAX_STATUS_TEXT_BYTES, MAX_TAG_BYTES, MAX_TZ_BYTES,
    };

    fn entry(language: &str, summary: &str, grammatical_gender: &str) -> PronounEntry {
        PronounEntry {
            language: language.to_owned(),
            summary: summary.to_owned(),
            grammatical_gender: grammatical_gender.to_owned(),
        }
    }

    // --- parse_pronouns ---

    #[test]
    fn parse_pronouns_stable_key_array() {
        let profile = serde_json::json!({
            "m.pronouns": [{"summary": "she/her", "language": "en"}]
        });
        assert_eq!(parse_pronouns(&profile), vec![entry("en", "she/her", "")]);
    }

    #[test]
    fn parse_pronouns_unstable_key_array() {
        let profile = serde_json::json!({
            "io.fsky.nyx.pronouns": [{"summary": "she/her", "language": "en"}]
        });
        assert_eq!(parse_pronouns(&profile), vec![entry("en", "she/her", "")]);
    }

    #[test]
    fn parse_pronouns_stable_takes_priority() {
        let profile = serde_json::json!({
            "m.pronouns": [{"summary": "she/her", "language": "en"}],
            "io.fsky.nyx.pronouns": [{"summary": "they/them", "language": "en"}]
        });
        assert_eq!(parse_pronouns(&profile), vec![entry("en", "she/her", "")]);
    }

    #[test]
    fn parse_pronouns_string_fallback() {
        let profile = serde_json::json!({
            "m.pronouns": "they/them"
        });
        assert_eq!(parse_pronouns(&profile), vec![entry("", "they/them", "")]);
    }

    #[test]
    fn parse_pronouns_empty_array() {
        let profile = serde_json::json!({
            "m.pronouns": []
        });
        assert_eq!(parse_pronouns(&profile), Vec::<PronounEntry>::new());
    }

    #[test]
    fn parse_pronouns_missing_key() {
        let profile = serde_json::json!({});
        assert_eq!(parse_pronouns(&profile), Vec::<PronounEntry>::new());
    }

    #[test]
    fn parse_pronouns_multiple_languages() {
        let profile = serde_json::json!({
            "m.pronouns": [
                {"summary": "she/her", "language": "en", "grammatical_gender": "feminine"},
                {"summary": "elle", "language": "fr"}
            ]
        });
        assert_eq!(
            parse_pronouns(&profile),
            vec![entry("en", "she/her", "feminine"), entry("fr", "elle", ""),]
        );
    }

    #[test]
    fn parse_pronouns_grammatical_gender_inanimate() {
        let profile = serde_json::json!({
            "m.pronouns": [{"summary": "it/its", "language": "en", "grammatical_gender": "inanimate"}]
        });
        assert_eq!(
            parse_pronouns(&profile),
            vec![entry("en", "it/its", "inanimate")]
        );
    }

    #[test]
    fn parse_pronouns_entry_missing_summary_is_skipped() {
        let profile = serde_json::json!({
            "m.pronouns": [
                {"language": "en"},
                {"summary": "elle", "language": "fr"}
            ]
        });
        assert_eq!(parse_pronouns(&profile), vec![entry("fr", "elle", "")]);
    }

    // --- parse_tz ---

    #[test]
    fn parse_tz_stable_key() {
        let profile = serde_json::json!({
            "m.tz": "Europe/Madrid"
        });
        assert_eq!(parse_tz(&profile), "Europe/Madrid");
    }

    #[test]
    fn parse_tz_unstable_key() {
        let profile = serde_json::json!({
            "us.cloke.msc4175.tz": "America/New_York"
        });
        assert_eq!(parse_tz(&profile), "America/New_York");
    }

    #[test]
    fn parse_tz_stable_takes_priority() {
        let profile = serde_json::json!({
            "m.tz": "Europe/Madrid",
            "us.cloke.msc4175.tz": "America/New_York"
        });
        assert_eq!(parse_tz(&profile), "Europe/Madrid");
    }

    #[test]
    fn parse_tz_missing_key() {
        let profile = serde_json::json!({});
        assert_eq!(parse_tz(&profile), "");
    }

    // --- parse_biography ---

    #[test]
    fn parse_biography_stable_key() {
        let profile = serde_json::json!({
            "m.biography": {
                "m.text": [{"body": "Hello world", "mimetype": "text/plain"}]
            }
        });
        assert_eq!(parse_biography(&profile), "Hello world");
    }

    #[test]
    fn parse_biography_unstable_key() {
        let profile = serde_json::json!({
            "gay.fomx.biography": {
                "m.text": [{"body": "Hello world", "mimetype": "text/plain"}]
            }
        });
        assert_eq!(parse_biography(&profile), "Hello world");
    }

    #[test]
    fn parse_biography_stable_takes_priority() {
        let profile = serde_json::json!({
            "m.biography": {
                "m.text": [{"body": "stable body", "mimetype": "text/plain"}]
            },
            "gay.fomx.biography": {
                "m.text": [{"body": "unstable body", "mimetype": "text/plain"}]
            }
        });
        assert_eq!(parse_biography(&profile), "stable body");
    }

    #[test]
    fn parse_biography_first_plain_body_returned() {
        let profile = serde_json::json!({
            "m.biography": {
                "m.text": [
                    {"body": "first", "mimetype": "text/plain"},
                    {"body": "second", "mimetype": "text/plain"}
                ]
            }
        });
        assert_eq!(parse_biography(&profile), "first");
    }

    #[test]
    fn parse_biography_skips_non_plain_mimetype() {
        let profile = serde_json::json!({
            "m.biography": {
                "m.text": [
                    {"body": "<b>html</b>", "mimetype": "text/html"},
                    {"body": "plain text", "mimetype": "text/plain"}
                ]
            }
        });
        assert_eq!(parse_biography(&profile), "plain text");
    }

    #[test]
    fn parse_biography_string_fallback() {
        let profile = serde_json::json!({
            "m.biography": "plain string bio"
        });
        assert_eq!(parse_biography(&profile), "plain string bio");
    }

    #[test]
    fn parse_biography_missing_key() {
        let profile = serde_json::json!({});
        assert_eq!(parse_biography(&profile), "");
    }

    // --- extract_status / extract_call_joined_ts (MSC4426, typed) ---

    #[test]
    fn status_parts_reads_both_fields() {
        let v = serde_json::json!({"text": "On holiday", "emoji": "🌴"});
        assert_eq!(
            super::status_parts(&v),
            ("🌴".to_owned(), "On holiday".to_owned())
        );
    }

    #[test]
    fn status_parts_defaults_missing_fields_to_empty() {
        assert_eq!(
            super::status_parts(&serde_json::json!({"emoji": "🌴"})),
            ("🌴".to_owned(), String::new())
        );
        assert_eq!(
            super::status_parts(&serde_json::json!({})),
            (String::new(), String::new())
        );
    }

    fn profile_with(key: &str, value: serde_json::Value) -> matrix_sdk::ruma::profile::UserProfile {
        let mut p = matrix_sdk::ruma::profile::UserProfile::new();
        p.set(key.to_owned(), value);
        p
    }

    #[test]
    fn extract_status_unstable_key() {
        let p = profile_with(
            "org.matrix.msc4426.status",
            serde_json::json!({"text": "AFK", "emoji": "🏃"}),
        );
        let s = extract_status(&p).unwrap();
        assert_eq!(s.text, "AFK");
        assert_eq!(s.emoji, "🏃");
    }

    #[test]
    fn extract_status_missing_key_is_none() {
        assert!(extract_status(&matrix_sdk::ruma::profile::UserProfile::new()).is_none());
    }

    #[test]
    fn extract_status_wrong_type_is_none() {
        let p = profile_with(
            "org.matrix.msc4426.status",
            serde_json::json!("a bare string"),
        );
        assert!(extract_status(&p).is_none());
    }

    #[test]
    fn extract_status_empty_text_and_emoji_is_none() {
        let p = profile_with(
            "org.matrix.msc4426.status",
            serde_json::json!({"text": "", "emoji": ""}),
        );
        assert!(extract_status(&p).is_none());
    }

    #[test]
    fn extract_status_partial_object_is_dropped() {
        // The typed field requires both `text` and `emoji`.
        let p = profile_with(
            "org.matrix.msc4426.status",
            serde_json::json!({"emoji": "🎯"}),
        );
        assert!(extract_status(&p).is_none());
    }

    #[test]
    fn extract_status_stable_key_is_ignored() {
        let p = profile_with("m.status", serde_json::json!({"text": "x", "emoji": "y"}));
        assert!(extract_status(&p).is_none());
    }

    #[test]
    fn extract_call_joined_ts_unstable_key() {
        let p = profile_with(
            "org.matrix.msc4426.call",
            serde_json::json!({"call_joined_ts": 1_770_140_640_u64}),
        );
        assert_eq!(extract_call_joined_ts(&p), Some(1_770_140_640));
    }

    #[test]
    fn extract_call_joined_ts_missing_key_is_none() {
        assert_eq!(
            extract_call_joined_ts(&matrix_sdk::ruma::profile::UserProfile::new()),
            None
        );
    }

    #[test]
    fn extract_call_joined_ts_malformed_is_none() {
        let p = profile_with(
            "org.matrix.msc4426.call",
            serde_json::json!({"call_joined_ts": "not a number"}),
        );
        assert_eq!(extract_call_joined_ts(&p), None);
    }

    #[test]
    fn extract_call_joined_ts_empty_object_is_none() {
        let p = profile_with("org.matrix.msc4426.call", serde_json::json!({}));
        assert_eq!(extract_call_joined_ts(&p), None);
    }

    #[test]
    fn clamp_bytes_keeps_a_short_value_and_cuts_on_a_char_boundary() {
        assert_eq!(clamp_bytes("abc", 8), "abc");
        assert_eq!(clamp_bytes("aaaa", 2), "aa");
        // "é" is two bytes, so a cut at byte 2 would split it: back off to 1.
        assert_eq!(clamp_bytes("aé", 2), "a");
        assert_eq!(clamp_bytes("", 0), "");
    }

    #[test]
    fn parse_pronouns_clamps_each_value_and_caps_the_entry_count() {
        let entries: Vec<serde_json::Value> = (0..40)
            .map(|_| {
                serde_json::json!({
                    "summary": "s".repeat(1000),
                    "language": "l".repeat(1000),
                    "grammatical_gender": "g".repeat(1000),
                })
            })
            .collect();
        let profile = serde_json::json!({ "m.pronouns": entries });
        let parsed = parse_pronouns(&profile);
        assert_eq!(parsed.len(), MAX_PRONOUN_ENTRIES);
        let first = &parsed[0];
        assert_eq!(first.summary.len(), MAX_PRONOUN_BYTES);
        assert_eq!(first.language.len(), MAX_TAG_BYTES);
        assert_eq!(first.grammatical_gender.len(), MAX_TAG_BYTES);
    }

    #[test]
    fn parse_pronouns_string_fallback_is_clamped() {
        let profile = serde_json::json!({ "m.pronouns": "x".repeat(5000) });
        let parsed = parse_pronouns(&profile);
        assert_eq!(parsed.len(), 1);
        assert_eq!(parsed[0].summary.len(), MAX_PRONOUN_BYTES);
    }

    #[test]
    fn parse_tz_is_clamped() {
        let profile = serde_json::json!({ "m.tz": "Z".repeat(500) });
        assert_eq!(parse_tz(&profile).len(), MAX_TZ_BYTES);
    }

    #[test]
    fn parse_biography_is_clamped_on_a_char_boundary() {
        let body = "é".repeat(5000);
        let profile = serde_json::json!({ "m.biography": { "m.text": [{ "body": body }] } });
        let bio = parse_biography(&profile);
        assert!(bio.len() <= MAX_BIOGRAPHY_BYTES);
        assert!(bio.len() > MAX_BIOGRAPHY_BYTES - 2);
        assert!(bio.chars().all(|c| c == 'é'));
    }

    #[test]
    fn extract_status_is_clamped_to_the_msc4426_limits() {
        let p = profile_with(
            "org.matrix.msc4426.status",
            serde_json::json!({"text": "t".repeat(1000), "emoji": "e".repeat(1000)}),
        );
        let s = extract_status(&p).unwrap();
        assert_eq!(s.text.len(), MAX_STATUS_TEXT_BYTES);
        assert_eq!(s.emoji.len(), MAX_STATUS_EMOJI_BYTES);
    }
}
