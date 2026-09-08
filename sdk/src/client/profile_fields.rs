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
                summary: s.to_owned(),
                grammatical_gender: String::new(),
            }];
        }
        if let Some(arr) = v.as_array() {
            return arr
                .iter()
                .filter_map(|o| {
                    let summary = o["summary"].as_str()?;
                    Some(PronounEntry {
                        language: o["language"].as_str().unwrap_or("").to_owned(),
                        summary: summary.to_owned(),
                        grammatical_gender: o["grammatical_gender"]
                            .as_str()
                            .unwrap_or("")
                            .to_owned(),
                    })
                })
                .collect();
        }
    }
    Vec::new()
}

/// Parse the timezone from either `m.tz` or `us.cloke.msc4175.tz`.
fn parse_tz(j: &serde_json::Value) -> String {
    for key in &["m.tz", "us.cloke.msc4175.tz"] {
        if let Some(s) = j[*key].as_str() {
            return s.to_owned();
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
                        return body.to_owned();
                    }
                }
            }
        }
        // Plain-string fallback for servers that omit the m.text wrapper.
        if let Some(s) = v.as_str() {
            return s.to_owned();
        }
    }
    String::new()
}

/// One `m.status` (MSC4426) value: a short free-text status and/or a status
/// emoji. Either part may be an empty string; `parse_status` returns `None`
/// only when neither is present.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct UserStatus {
    pub text: String,
    pub emoji: String,
}

/// Parse `m.status` from either the stable `m.status` key or the unstable
/// `org.matrix.msc4426.status` key (stable wins). Value shape:
/// `{ "text": string, "emoji": string }`, both optional. Returns `None` when
/// the key is absent, not an object, or an object with neither field.
fn parse_status(j: &serde_json::Value) -> Option<UserStatus> {
    for key in &["m.status", "org.matrix.msc4426.status"] {
        let v = &j[*key];
        if v.is_null() {
            continue;
        }
        let obj = v.as_object()?;
        let text = obj.get("text").and_then(|t| t.as_str()).unwrap_or("");
        let emoji = obj.get("emoji").and_then(|e| e.as_str()).unwrap_or("");
        if text.is_empty() && emoji.is_empty() {
            return None;
        }
        return Some(UserStatus {
            text: text.to_owned(),
            emoji: emoji.to_owned(),
        });
    }
    None
}

/// Parse `call_joined_ts` (unix seconds) from either the stable `m.call` key
/// or the unstable `org.matrix.msc4426.call` key (stable wins). Returns `None`
/// when the key is absent, not an object, or `call_joined_ts` is missing/not a
/// non-negative integer.
fn parse_call_joined_ts(j: &serde_json::Value) -> Option<u64> {
    for key in &["m.call", "org.matrix.msc4426.call"] {
        let v = &j[*key];
        if v.is_null() {
            continue;
        }
        return v.get("call_joined_ts").and_then(|t| t.as_u64());
    }
    None
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
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "profile_fields/get_extended_profile".to_string(),
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
                let status = parse_status(&j);
                let call_joined_ts = parse_call_joined_ts(&j).unwrap_or(0);

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

            let result = if is_delete {
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

    /// Publish or clear the caller's own `m.call` (MSC4426) profile field.
    /// `Some(ts)` sets `{ "call_joined_ts": ts }` (unix seconds); `None`
    /// deletes the field. Fire-and-forget — MSC4426 says applications set
    /// `m.call` programmatically, and a failure here must never block or
    /// surface in the call UI. No-op when the server doesn't advertise
    /// MSC4133 profile-field writes.
    pub(crate) fn publish_own_call_field(&self, joined_ts: Option<u64>) {
        if self.profile_fields_prefix.read().unwrap().is_none() {
            return;
        }
        let Some(client) = self.client.as_ref() else {
            return;
        };
        let client = client.clone();
        self.rt.spawn(async move {
            use matrix_sdk::ruma::profile::{ProfileFieldName, ProfileFieldValue};
            const KEY: &str = "org.matrix.msc4426.call";
            let result = match joined_ts {
                Some(ts) => {
                    match ProfileFieldValue::new(KEY, serde_json::json!({ "call_joined_ts": ts })) {
                        Ok(v) => client.account().set_profile_field(v).await,
                        Err(e) => {
                            tracing::warn!("MSC4426 m.call encode failed: {e}");
                            return;
                        }
                    }
                }
                None => {
                    client
                        .account()
                        .delete_profile_field(ProfileFieldName::from(KEY))
                        .await
                }
            };
            if let Err(e) = result {
                tracing::warn!("MSC4426 m.call publish failed: {e}");
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

    pub(crate) fn publish_own_call_field(&self, _joined_ts: Option<u64>) {}
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::{
        parse_biography, parse_call_joined_ts, parse_pronouns, parse_status, parse_tz, PronounEntry,
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
            vec![
                entry("en", "she/her", "feminine"),
                entry("fr", "elle", ""),
            ]
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

    // --- parse_status (MSC4426) ---

    #[test]
    fn parse_status_stable_key() {
        let profile = serde_json::json!({
            "m.status": {"text": "On holiday", "emoji": "🌴"}
        });
        let s = parse_status(&profile).unwrap();
        assert_eq!(s.text, "On holiday");
        assert_eq!(s.emoji, "🌴");
    }

    #[test]
    fn parse_status_unstable_key() {
        let profile = serde_json::json!({
            "org.matrix.msc4426.status": {"text": "AFK", "emoji": "🏃"}
        });
        let s = parse_status(&profile).unwrap();
        assert_eq!(s.text, "AFK");
        assert_eq!(s.emoji, "🏃");
    }

    #[test]
    fn parse_status_stable_takes_priority() {
        let profile = serde_json::json!({
            "m.status": {"text": "stable", "emoji": "✅"},
            "org.matrix.msc4426.status": {"text": "unstable", "emoji": "❌"}
        });
        let s = parse_status(&profile).unwrap();
        assert_eq!(s.text, "stable");
        assert_eq!(s.emoji, "✅");
    }

    #[test]
    fn parse_status_text_only() {
        let profile = serde_json::json!({ "m.status": {"text": "just text"} });
        let s = parse_status(&profile).unwrap();
        assert_eq!(s.text, "just text");
        assert_eq!(s.emoji, "");
    }

    #[test]
    fn parse_status_emoji_only() {
        let profile = serde_json::json!({ "m.status": {"emoji": "🎯"} });
        let s = parse_status(&profile).unwrap();
        assert_eq!(s.text, "");
        assert_eq!(s.emoji, "🎯");
    }

    #[test]
    fn parse_status_empty_object_is_none() {
        let profile = serde_json::json!({ "m.status": {} });
        assert!(parse_status(&profile).is_none());
    }

    #[test]
    fn parse_status_wrong_type_is_none() {
        let profile = serde_json::json!({ "m.status": "a bare string" });
        assert!(parse_status(&profile).is_none());
    }

    #[test]
    fn parse_status_missing_key_is_none() {
        assert!(parse_status(&serde_json::json!({})).is_none());
    }

    // --- parse_call_joined_ts (MSC4426) ---

    #[test]
    fn parse_call_joined_ts_stable_key() {
        let profile = serde_json::json!({ "m.call": {"call_joined_ts": 1_770_140_640_u64} });
        assert_eq!(parse_call_joined_ts(&profile), Some(1_770_140_640));
    }

    #[test]
    fn parse_call_joined_ts_unstable_key() {
        let profile = serde_json::json!({ "org.matrix.msc4426.call": {"call_joined_ts": 42} });
        assert_eq!(parse_call_joined_ts(&profile), Some(42));
    }

    #[test]
    fn parse_call_joined_ts_stable_takes_priority() {
        let profile = serde_json::json!({
            "m.call": {"call_joined_ts": 1},
            "org.matrix.msc4426.call": {"call_joined_ts": 2}
        });
        assert_eq!(parse_call_joined_ts(&profile), Some(1));
    }

    #[test]
    fn parse_call_joined_ts_missing_key_is_none() {
        assert_eq!(parse_call_joined_ts(&serde_json::json!({})), None);
    }

    #[test]
    fn parse_call_joined_ts_malformed_is_none() {
        let profile = serde_json::json!({ "m.call": {"call_joined_ts": "not a number"} });
        assert_eq!(parse_call_joined_ts(&profile), None);
    }

    #[test]
    fn parse_call_joined_ts_empty_object_is_none() {
        assert_eq!(parse_call_joined_ts(&serde_json::json!({ "m.call": {} })), None);
    }
}
