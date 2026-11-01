//! MSC2666 mutual rooms ("rooms in common" with another user).
//!
//! `get_server_info` records which endpoint the server offers in
//! `ClientFfi::mutual_rooms_mode`; `get_mutual_rooms_async` sends the matching
//! ruma request through `client.send()` and reports a JSON object
//! `{joined, count, next_batch, error}` via `on_mutual_rooms_ready`.

use super::ClientFfi;

/// Server does not advertise MSC2666 in any form.
pub(super) const MUTUAL_ROOMS_NONE: u8 = 0;
/// Only the `uk.half-shot.msc2666` unstable endpoint (no `count`).
pub(super) const MUTUAL_ROOMS_UNSTABLE: u8 = 1;
/// Stable `/v1/mutual_rooms` (spec 1.19 or the `.stable` feature flag).
pub(super) const MUTUAL_ROOMS_STABLE: u8 = 2;

/// Unstable `unstable_features` keys that mean the proposal is implemented.
const UNSTABLE_FLAGS: [&str; 3] = [
    "uk.half-shot.msc2666.query_mutual_rooms",
    "uk.half-shot.msc2666.mutual_rooms",
    "uk.half-shot.msc2666",
];
const STABLE_FLAG: &str = "uk.half-shot.msc2666.query_mutual_rooms.stable";

/// Which MSC2666 endpoint the server offers, from its feature flags and its
/// advertised spec versions (`major`, `minor`): the stable endpoint when the
/// `.stable` flag is set or a version >= 1.19 is listed, the unstable one when
/// only an unstable flag is, otherwise none.
pub(super) fn mutual_rooms_mode_for(
    has_feature: impl Fn(&str) -> bool,
    spec_versions: &[crate::ffi::SpecVersionFfi],
) -> u8 {
    let spec_stable = spec_versions.iter().any(|v| (v.major, v.minor) >= (1, 19));
    if spec_stable || has_feature(STABLE_FLAG) {
        MUTUAL_ROOMS_STABLE
    } else if UNSTABLE_FLAGS.iter().any(|f| has_feature(f)) {
        MUTUAL_ROOMS_UNSTABLE
    } else {
        MUTUAL_ROOMS_NONE
    }
}

/// The JSON delivered to `on_mutual_rooms_ready`. `count` is the server's total
/// when it reports one (stable endpoint), else the number of rooms returned.
pub(super) fn mutual_rooms_json(
    joined: &[String],
    count: Option<u64>,
    next_batch: Option<&str>,
    error: &str,
) -> String {
    serde_json::json!({
        "joined": joined,
        "count": count.unwrap_or(joined.len() as u64),
        "next_batch": next_batch.unwrap_or(""),
        "error": error,
    })
    .to_string()
}

#[cfg(not(test))]
impl ClientFfi {
    /// Fetch one page of the rooms shared with `user_id` and fire
    /// `on_mutual_rooms_ready(request_id, json)`. `from` is the previous page's
    /// `next_batch` (empty for the first page). Does not pin a thread.
    pub fn get_mutual_rooms_async(&self, request_id: u64, user_id: &str, from: &str) {
        use matrix_sdk::ruma::api::client::membership::get_mutual_rooms::{unstable, v1};
        use std::sync::atomic::Ordering;

        macro_rules! deliver {
            ($json:expr) => {{
                if let Some(ref h) = self.handler {
                    h.lock().on_mutual_rooms_ready(request_id, &$json);
                }
                return;
            }};
        }

        let Some(client) = self.client.as_ref() else {
            deliver!(mutual_rooms_json(&[], None, None, "not logged in"));
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(user_id) else {
            deliver!(mutual_rooms_json(&[], None, None, "invalid user id"));
        };
        let mode = self.mutual_rooms_mode.load(Ordering::Relaxed);
        if mode == MUTUAL_ROOMS_NONE {
            deliver!(mutual_rooms_json(&[], None, None, "unsupported"));
        }

        let client = client.clone();
        let handler = self.handler.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        let from = (!from.is_empty()).then(|| from.to_owned());

        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "mutual_rooms/get".to_string(),
            );

            let json = if mode == MUTUAL_ROOMS_STABLE {
                let mut req = v1::Request::new(uid);
                req.from = from;
                match client.send(req).await {
                    Ok(r) => {
                        let joined: Vec<String> = r.joined.iter().map(|r| r.to_string()).collect();
                        mutual_rooms_json(
                            &joined,
                            Some(u64::from(r.count)),
                            r.next_batch.as_deref(),
                            "",
                        )
                    }
                    Err(e) => mutual_rooms_json(&[], None, None, &e.to_string()),
                }
            } else {
                let mut req = unstable::Request::new(uid);
                req.from = from;
                match client.send(req).await {
                    Ok(r) => {
                        let joined: Vec<String> = r.joined.iter().map(|r| r.to_string()).collect();
                        mutual_rooms_json(&joined, None, r.next_batch.as_deref(), "")
                    }
                    Err(e) => mutual_rooms_json(&[], None, None, &e.to_string()),
                }
            };

            if let Some(h) = handler {
                h.lock().on_mutual_rooms_ready(request_id, &json);
            }
        });
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn get_mutual_rooms_async(&self, _request_id: u64, _user_id: &str, _from: &str) {}
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::SpecVersionFfi;

    fn ver(major: u32, minor: u32) -> SpecVersionFfi {
        SpecVersionFfi { major, minor }
    }

    #[test]
    fn mode_none_without_flags() {
        assert_eq!(
            mutual_rooms_mode_for(|_| false, &[ver(1, 11)]),
            MUTUAL_ROOMS_NONE
        );
    }

    #[test]
    fn mode_unstable_for_each_unstable_flag() {
        for flag in UNSTABLE_FLAGS {
            assert_eq!(
                mutual_rooms_mode_for(|f| f == flag, &[ver(1, 11)]),
                MUTUAL_ROOMS_UNSTABLE,
                "{flag}"
            );
        }
    }

    #[test]
    fn mode_stable_for_stable_flag_or_spec_1_19() {
        assert_eq!(
            mutual_rooms_mode_for(|f| f == STABLE_FLAG, &[ver(1, 11)]),
            MUTUAL_ROOMS_STABLE
        );
        assert_eq!(
            mutual_rooms_mode_for(|_| false, &[ver(1, 11), ver(1, 19)]),
            MUTUAL_ROOMS_STABLE
        );
        assert_eq!(
            mutual_rooms_mode_for(|_| false, &[ver(2, 0)]),
            MUTUAL_ROOMS_STABLE
        );
        // Stable wins over an unstable flag.
        assert_eq!(
            mutual_rooms_mode_for(|_| true, &[ver(1, 12)]),
            MUTUAL_ROOMS_STABLE
        );
        assert_eq!(
            mutual_rooms_mode_for(|_| false, &[ver(1, 18)]),
            MUTUAL_ROOMS_NONE
        );
    }

    #[test]
    fn json_shapes_a_page() {
        let joined = vec!["!a:x".to_owned(), "!b:x".to_owned()];
        let v: serde_json::Value =
            serde_json::from_str(&mutual_rooms_json(&joined, Some(5), Some("tok"), "")).unwrap();
        assert_eq!(v["joined"], serde_json::json!(["!a:x", "!b:x"]));
        assert_eq!(v["count"], 5);
        assert_eq!(v["next_batch"], "tok");
        assert_eq!(v["error"], "");
    }

    #[test]
    fn json_count_falls_back_to_len_and_error_is_reported() {
        let joined = vec!["!a:x".to_owned()];
        let v: serde_json::Value =
            serde_json::from_str(&mutual_rooms_json(&joined, None, None, "")).unwrap();
        assert_eq!(v["count"], 1);
        assert_eq!(v["next_batch"], "");

        let v: serde_json::Value =
            serde_json::from_str(&mutual_rooms_json(&[], None, None, "boom")).unwrap();
        assert_eq!(v["joined"], serde_json::json!([]));
        assert_eq!(v["error"], "boom");
    }
}
