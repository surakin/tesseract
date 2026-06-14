//! MSC4133 extended profile fields: pronouns, timezone, biography.
//!
//! Reading uses the standard `GET /_matrix/client/v3/profile/{user_id}`
//! endpoint (already returns custom keys). Writing / deleting uses the
//! MSC4133 unstable prefix `/_matrix/client/unstable/uk.tcpip.msc4133`
//! when the server advertises `unstable_features["uk.tcpip.msc4133"] == true`;
//! otherwise writes are rejected with an error result.

use super::{err, ok, ClientFfi};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Parse the pronouns summary from either the stable `m.pronouns` or the
/// unstable `io.fsky.nyx.pronouns` key in `j`.
///
/// The field value is either:
/// - A JSON array of `{summary, language, grammatical_gender?}` objects
///   → return `[0].summary` as a string.
/// - A plain string (graceful future-compat)
///   → return it directly.
fn parse_pronouns(j: &serde_json::Value) -> String {
    for key in &["m.pronouns", "io.fsky.nyx.pronouns"] {
        let v = &j[*key];
        if v.is_null() {
            continue;
        }
        if let Some(s) = v.as_str() {
            return s.to_owned();
        }
        if let Some(arr) = v.as_array() {
            if let Some(summary) = arr.first().and_then(|o| o["summary"].as_str()) {
                return summary.to_owned();
            }
        }
    }
    String::new()
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
    /// Fetch extended profile fields for a user via the standard
    /// `GET /_matrix/client/v3/profile/{user_id}` endpoint.
    /// Returns a fully populated `UserProfile` (displayname + avatar_url +
    /// pronouns + tz + biography). Falls back to `exists: false` on network
    /// error, parse failure, or when not logged in.
    /// Blocks — call from a worker thread.
    pub fn get_extended_profile(&self, user_id: &str) -> crate::ffi::UserProfile {
        let empty = || crate::ffi::UserProfile {
            exists: false,
            user_id: String::new(),
            display_name: String::new(),
            avatar_url: String::new(),
            pronouns: String::new(),
            tz: String::new(),
            biography: String::new(),
        };

        if user_id.is_empty() {
            return empty();
        }

        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "profile_fields/get_extended_profile".to_string(),
        );

        let Some(client) = self.client.as_ref() else {
            return empty();
        };

        let base = {
            let url = client.homeserver().to_string();
            url.trim_end_matches('/').to_owned()
        };
        let access_token = client.access_token().unwrap_or_default();
        let http = self.http_client.clone();
        let encoded_uid = percent_encode_user_id(user_id);
        let url = format!("{base}/_matrix/client/v3/profile/{encoded_uid}");

        self.rt.block_on(async move {
            let mut req = http.get(&url);
            if !access_token.is_empty() {
                req = req.bearer_auth(&access_token);
            }
            let resp = match req.send().await {
                Ok(r) => r,
                Err(_) => return empty(),
            };
            if !resp.status().is_success() {
                return empty();
            }
            let j: serde_json::Value = match resp.json().await {
                Ok(v) => v,
                Err(_) => return empty(),
            };

            let display_name = j["displayname"]
                .as_str()
                .filter(|s| !s.is_empty())
                .map(str::to_owned)
                .unwrap_or_else(|| {
                    // Fall back to localpart extracted from the mxid
                    user_id
                        .split(':')
                        .next()
                        .and_then(|s| s.strip_prefix('@'))
                        .unwrap_or(user_id)
                        .to_owned()
                });
            let avatar_url = j["avatar_url"]
                .as_str()
                .unwrap_or("")
                .to_owned();

            let pronouns = parse_pronouns(&j);
            let tz = parse_tz(&j);
            let biography = parse_biography(&j);

            crate::ffi::UserProfile {
                exists: true,
                user_id: user_id.to_owned(),
                display_name,
                avatar_url,
                pronouns,
                tz,
                biography,
            }
        })
    }

    /// Write a single extended profile field via MSC4133.
    ///
    /// `key` is the unstable key (e.g. `"io.fsky.nyx.pronouns"`).
    /// `value_json` is a JSON string encoding the field value.
    ///
    /// Errors if the server does not advertise `uk.tcpip.msc4133`.
    /// Blocks — call from a worker thread.
    pub fn set_profile_field(&self, key: &str, value_json: &str) -> crate::ffi::OpResult {
        if key.is_empty() {
            return crate::ffi::OpResult { ok: false, message: "key must not be empty".to_owned() };
        }

        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] format!("profile_fields/set/{key}"),
        );

        let prefix = match self.profile_fields_prefix.read().unwrap().clone() {
            Some(p) => p,
            None => return err("server does not support MSC4133 profile field writes"),
        };

        let value: serde_json::Value = match serde_json::from_str(value_json) {
            Ok(v) => v,
            Err(e) => return err(format!("invalid JSON for field value: {e}")),
        };

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let Some(uid) = client.user_id() else {
            return err("no user_id available");
        };
        let base = {
            let url = client.homeserver().to_string();
            url.trim_end_matches('/').to_owned()
        };
        let access_token = match client.access_token() {
            Some(t) => t,
            None => return err("no access token"),
        };
        let http = self.http_client.clone();
        let encoded_uid = percent_encode_user_id(uid.as_str());
        let url = format!("{base}{prefix}/profile/{encoded_uid}/{key}");
        let body = serde_json::json!({ key: value });

        self.rt.block_on(async move {
            let resp = match http
                .put(&url)
                .bearer_auth(&access_token)
                .json(&body)
                .send()
                .await
            {
                Ok(r) => r,
                Err(e) => return err(format!("network error: {e}")),
            };
            if resp.status().is_success() {
                ok("")
            } else {
                let status = resp.status();
                let error_text = resp.text().await.unwrap_or_default();
                err(format!("server error {status}: {error_text}"))
            }
        })
    }

    /// Delete a single extended profile field via MSC4133.
    ///
    /// `key` is the unstable key (e.g. `"io.fsky.nyx.pronouns"`).
    ///
    /// Errors if the server does not advertise `uk.tcpip.msc4133`.
    /// Blocks — call from a worker thread.
    pub fn delete_profile_field(&self, key: &str) -> crate::ffi::OpResult {
        if key.is_empty() {
            return crate::ffi::OpResult { ok: false, message: "key must not be empty".to_owned() };
        }

        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "profile_fields/delete".to_string(),
        );

        let prefix = match self.profile_fields_prefix.read().unwrap().clone() {
            Some(p) => p,
            None => return err("server does not support MSC4133 profile field writes"),
        };

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let Some(uid) = client.user_id() else {
            return err("no user_id available");
        };
        let base = {
            let url = client.homeserver().to_string();
            url.trim_end_matches('/').to_owned()
        };
        let access_token = match client.access_token() {
            Some(t) => t,
            None => return err("no access token"),
        };
        let http = self.http_client.clone();
        let encoded_uid = percent_encode_user_id(uid.as_str());
        let url = format!("{base}{prefix}/profile/{encoded_uid}/{key}");

        self.rt.block_on(async move {
            let resp = match http
                .delete(&url)
                .bearer_auth(&access_token)
                .send()
                .await
            {
                Ok(r) => r,
                Err(e) => return err(format!("network error: {e}")),
            };
            if resp.status().is_success() {
                ok("")
            } else {
                let status = resp.status();
                let body = resp.text().await.unwrap_or_default();
                err(format!("server error {status}: {body}"))
            }
        })
    }
}

// ---------------------------------------------------------------------------
// Test stubs
// ---------------------------------------------------------------------------

#[cfg(test)]
impl ClientFfi {
    pub fn get_extended_profile(&self, _user_id: &str) -> crate::ffi::UserProfile {
        crate::ffi::UserProfile {
            exists: false,
            user_id: String::new(),
            display_name: String::new(),
            avatar_url: String::new(),
            pronouns: String::new(),
            tz: String::new(),
            biography: String::new(),
        }
    }

    pub fn set_profile_field(&self, _key: &str, _value_json: &str) -> crate::ffi::OpResult {
        ok("")
    }

    pub fn delete_profile_field(&self, _key: &str) -> crate::ffi::OpResult {
        ok("")
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
    use super::{parse_biography, parse_pronouns, parse_tz};

    // --- parse_pronouns ---

    #[test]
    fn parse_pronouns_stable_key_array() {
        let profile = serde_json::json!({
            "m.pronouns": [{"summary": "she/her", "language": "en"}]
        });
        assert_eq!(parse_pronouns(&profile), "she/her");
    }

    #[test]
    fn parse_pronouns_unstable_key_array() {
        let profile = serde_json::json!({
            "io.fsky.nyx.pronouns": [{"summary": "she/her", "language": "en"}]
        });
        assert_eq!(parse_pronouns(&profile), "she/her");
    }

    #[test]
    fn parse_pronouns_stable_takes_priority() {
        let profile = serde_json::json!({
            "m.pronouns": [{"summary": "she/her", "language": "en"}],
            "io.fsky.nyx.pronouns": [{"summary": "they/them", "language": "en"}]
        });
        assert_eq!(parse_pronouns(&profile), "she/her");
    }

    #[test]
    fn parse_pronouns_string_fallback() {
        let profile = serde_json::json!({
            "m.pronouns": "they/them"
        });
        assert_eq!(parse_pronouns(&profile), "they/them");
    }

    #[test]
    fn parse_pronouns_empty_array() {
        let profile = serde_json::json!({
            "m.pronouns": []
        });
        assert_eq!(parse_pronouns(&profile), "");
    }

    #[test]
    fn parse_pronouns_missing_key() {
        let profile = serde_json::json!({});
        assert_eq!(parse_pronouns(&profile), "");
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
