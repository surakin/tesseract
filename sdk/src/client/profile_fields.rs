//! MSC4133 extended profile fields: pronouns, timezone, biography.
//!
//! Reading uses the standard `GET /_matrix/client/v3/profile/{user_id}`
//! endpoint (already returns custom keys). Writing / deleting uses the
//! MSC4133 unstable prefix `/_matrix/client/unstable/uk.tcpip.msc4133`
//! when the server advertises `unstable_features["uk.tcpip.msc4133"] == true`;
//! otherwise writes are rejected with an error result.

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

// ---------------------------------------------------------------------------
// Production implementations
// ---------------------------------------------------------------------------

#[cfg(not(test))]
impl ClientFfi {
    pub fn get_extended_profile_async(&self, request_id: u64, user_id: &str) {
        let empty_json = r#"{"exists":false,"user_id":"","display_name":"","avatar_url":"","pronouns":[],"tz":"","biography":""}"#;

        if user_id.is_empty() {
            if let Some(ref h) = self.handler {
                h.lock().on_extended_profile_ready(request_id, empty_json);
            }
            return;
        }
        let Some(client) = self.client.as_ref() else {
            if let Some(ref h) = self.handler {
                h.lock().on_extended_profile_ready(request_id, empty_json);
            }
            return;
        };
        let handler = self.handler.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        let http = self.http_client.clone();
        let access_token = client.access_token().unwrap_or_default();
        let base = {
            let url = client.homeserver().to_string();
            url.trim_end_matches('/').to_owned()
        };
        let encoded_uid = percent_encode_user_id(user_id);
        let url = format!("{base}/_matrix/client/v3/profile/{encoded_uid}");
        let user_id_owned = user_id.to_owned();

        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "profile_fields/get_extended_profile".to_string(),
            );

            let payload = async {
                let mut req = http.get(&url);
                if !access_token.is_empty() {
                    req = req.bearer_auth(&access_token);
                }
                let resp = req.send().await.ok()?;
                if !resp.status().is_success() {
                    return None;
                }
                let j: serde_json::Value = resp.json().await.ok()?;

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

                Some(
                    serde_json::json!({
                        "exists": true,
                        "user_id": user_id_owned,
                        "display_name": display_name,
                        "avatar_url": avatar_url,
                        "pronouns": pronouns,
                        "tz": tz,
                        "biography": biography,
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

        let prefix = match self.profile_fields_prefix.read().unwrap().clone() {
            Some(p) => p,
            None => {
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
        };

        let Some(client) = self.client.as_ref() else {
            if let Some(ref h) = self.handler {
                h.lock()
                    .on_profile_field_result(request_id, key, false, "not logged in");
            }
            return;
        };
        let Some(uid) = client.user_id() else {
            if let Some(ref h) = self.handler {
                h.lock()
                    .on_profile_field_result(request_id, key, false, "no user_id available");
            }
            return;
        };
        let base = client.homeserver().to_string();
        let base = base.trim_end_matches('/').to_owned();
        let Some(access_token) = client.access_token() else {
            if let Some(ref h) = self.handler {
                h.lock()
                    .on_profile_field_result(request_id, key, false, "no access token");
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

        let handler = self.handler.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        let http = self.http_client.clone();
        let encoded_uid = percent_encode_user_id(uid.as_str());
        let url = format!("{base}{prefix}/profile/{encoded_uid}/{key}");
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

            let result = if is_delete {
                match http.delete(&url).bearer_auth(&access_token).send().await {
                    Ok(r) if r.status().is_success() => ok(""),
                    Ok(r) => {
                        let status = r.status();
                        let body = r.text().await.unwrap_or_default();
                        err(format!("server error {status}: {body}"))
                    }
                    Err(e) => err(format!("network error: {e}")),
                }
            } else {
                let body = serde_json::json!({ &key_owned: value });
                match http
                    .put(&url)
                    .bearer_auth(&access_token)
                    .json(&body)
                    .send()
                    .await
                {
                    Ok(r) if r.status().is_success() => ok(""),
                    Ok(r) => {
                        let status = r.status();
                        let text = r.text().await.unwrap_or_default();
                        err(format!("server error {status}: {text}"))
                    }
                    Err(e) => err(format!("network error: {e}")),
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
// Internal helpers
// ---------------------------------------------------------------------------

/// Percent-encode a Matrix user ID for use in a URL path segment.
/// The `@` prefix and `:` separator are encoded as `%40` and `%3A`.
fn percent_encode_user_id(user_id: &str) -> String {
    user_id
        .chars()
        .flat_map(|c| match c {
            '@' => vec!['%', '4', '0'],
            ':' => vec!['%', '3', 'A'],
            _ => vec![c],
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::{parse_biography, parse_pronouns, parse_tz, PronounEntry};

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
}
