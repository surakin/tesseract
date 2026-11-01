pub use crate::ffi::MatrixLinkResult;

/// Kind constants matching `MatrixLinkResult.kind`.
pub const KIND_UNKNOWN: u8 = 0;
pub const KIND_ROOM: u8 = 1;
pub const KIND_ROOM_ALIAS: u8 = 2;
pub const KIND_USER: u8 = 3;
pub const KIND_EVENT: u8 = 4;

/// Parse a `https://matrix.to/#/…` URL or a `matrix:` URI (MSC2312) into a
/// structured result.  Returns `kind == 0` on unrecognised input.
///
/// Accepted forms (percent-encoding is decoded before classification):
///
/// matrix.to:
///   `https://matrix.to/#/!room:server`            → Room
///   `https://matrix.to/#/#alias:server`           → RoomAlias
///   `https://matrix.to/#/@user:server`            → User
///   `https://matrix.to/#/!room:server/$evt:server`→ Event
///   `https://matrix.to/#/#alias:server/$evt:server` → RoomAlias with `event_id`
///     set; the shell does not navigate to the event yet (Event needs a room id)
///
/// matrix: URI (MSC2312):
///   `matrix:roomid/<id>`               → Room       (primary = `!<id>`)
///   `matrix:roomid/<id>/e/<eventid>`   → Event
///   `matrix:r/<alias>`                 → RoomAlias  (primary = `#<alias>`)
///   `matrix:r/<alias>/e/<eventid>`     → RoomAlias with `event_id` (as above)
///   `matrix:u/<user>`                  → User       (primary = `@<user>`)
///
/// Every identifier is validated with ruma; anything else is `kind == 0`.
/// `?action=…` is ignored; `?via=…` params are retained in `via` (in order).
pub fn parse_matrix_link(uri: &str) -> MatrixLinkResult {
    try_matrix_to(uri)
        .or_else(|| try_matrix_uri(uri))
        .unwrap_or(MatrixLinkResult {
            kind: KIND_UNKNOWN,
            primary: String::new(),
            event_id: String::new(),
            via: Vec::new(),
        })
}

// Split `path?query` into its path and optional query parts.
fn split_query(s: &str) -> (&str, Option<&str>) {
    match s.split_once('?') {
        Some((path, query)) => (path, Some(query)),
        None => (s, None),
    }
}

// Collect the (percent-decoded) values of every `via=` param in a query string,
// in the order they appear. `matrix:` also historically used `server_name=`.
fn parse_via(query: Option<&str>) -> Vec<String> {
    let Some(query) = query else {
        return Vec::new();
    };
    query
        .split('&')
        .filter_map(|pair| {
            let (k, v) = pair.split_once('=')?;
            (k == "via" || k == "server_name").then(|| percent_decode(v))
        })
        .filter(|s| !s.is_empty())
        .collect()
}

// Strip the `https://matrix.to/#/` prefix. Returns the raw sigil-prefixed
// target, query string included (the caller splits it off).
fn matrix_to_target(href: &str) -> Option<&str> {
    // https only: an http permalink would invite a downgrade when pills are shown.
    for prefix in ["https://matrix.to/#/"] {
        if let Some(rest) = href.strip_prefix(prefix) {
            return Some(rest);
        }
    }
    None
}

fn result(kind: u8, primary: &str, event_id: &str, via: Vec<String>) -> Option<MatrixLinkResult> {
    Some(MatrixLinkResult {
        kind,
        primary: primary.to_string(),
        event_id: event_id.to_string(),
        via,
    })
}

// ── matrix.to parser ─────────────────────────────────────────────────────────

fn try_matrix_to(uri: &str) -> Option<MatrixLinkResult> {
    let (target, query) = split_query(matrix_to_target(uri)?);
    let via = parse_via(query);

    // The target may contain a second path component separated by `/` which
    // encodes an event ID: `!room:server/$event:server`.
    let (first, second) = match target.find('/') {
        Some(pos) => (&target[..pos], Some(percent_decode(&target[pos + 1..]))),
        None => (target, None),
    };
    let first = percent_decode(first);

    classify_sigil(&first, second.as_deref(), via)
}

// ── matrix: URI parser (MSC2312) ──────────────────────────────────────────────

fn try_matrix_uri(uri: &str) -> Option<MatrixLinkResult> {
    let (rest, query) = split_query(uri.strip_prefix("matrix:")?);
    let via = parse_via(query);

    // `<kind>/<value>[/e/<event_id>]`. A bare `/<event_id>` tail, written by
    // clients before the `e/` marker was specified, is still accepted.
    let mut parts = rest.splitn(4, '/');
    let segment = parts.next()?;
    let value = percent_decode(parts.next()?);
    let event = match parts.next() {
        None => None,
        // The spec form omits the `$` sigil; a client that wrote it anyway is
        // still accepted.
        Some("e") => {
            let id = percent_decode(parts.next()?);
            if id.is_empty() || id == "$" {
                return None;
            }
            Some(if id.starts_with('$') {
                id
            } else {
                format!("${id}")
            })
        }
        Some(legacy) => Some(percent_decode(legacy)),
    };

    let (primary, event) = match (segment, event) {
        ("roomid", event) => (format!("!{}", value), event),
        ("r", event) => (format!("#{}", value), event),
        ("u", None) => (format!("@{}", value), None),
        _ => return None,
    };
    classify_sigil(&primary, event.as_deref(), via)
}

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Classify a sigil-prefixed string (from matrix.to) into a MatrixLinkResult.
/// `second` is the optional event-ID path component.
fn classify_sigil(
    primary: &str,
    second: Option<&str>,
    via: Vec<String>,
) -> Option<MatrixLinkResult> {
    use matrix_sdk::ruma::{EventId, RoomAliasId, RoomId, UserId};

    // ruma accepts a bare `!` as an opaque room id, so reject empty ids here.
    if primary.len() < 2 {
        return None;
    }
    match primary.chars().next()? {
        '!' => {
            // Validate room ID
            RoomId::parse(primary).ok()?;
            match second {
                Some(ev) if !ev.is_empty() => {
                    EventId::parse(ev).ok()?;
                    result(KIND_EVENT, primary, ev, via)
                }
                _ => result(KIND_ROOM, primary, "", via),
            }
        }
        '#' => {
            RoomAliasId::parse(primary).ok()?;
            // The event is carried, not dropped; the shell only navigates by room id.
            match second {
                Some(ev) if !ev.is_empty() => {
                    EventId::parse(ev).ok()?;
                    result(KIND_ROOM_ALIAS, primary, ev, via)
                }
                _ => result(KIND_ROOM_ALIAS, primary, "", via),
            }
        }
        '@' => {
            UserId::parse(primary).ok()?;
            match second {
                Some(ev) if !ev.is_empty() => None,
                _ => result(KIND_USER, primary, "", via),
            }
        }
        '$' => {
            // Bare event ID (rare in matrix.to but valid): treat as unknown
            // since we don't have a room context.
            None
        }
        _ => None,
    }
}

/// Decode `%XX` percent-encoding. Invalid sequences are left as-is. Decoding
/// happens on bytes, so multi-byte UTF-8 (`%C3%A9` → `é`) survives intact.
fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(hi), Some(lo)) = (hex_nibble(bytes[i + 1]), hex_nibble(bytes[i + 2])) {
                out.push(hi << 4 | lo);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn hex_nibble(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn room(id: &str) -> MatrixLinkResult {
        MatrixLinkResult {
            kind: KIND_ROOM,
            primary: id.to_string(),
            event_id: String::new(),
            via: Vec::new(),
        }
    }
    fn alias(a: &str) -> MatrixLinkResult {
        MatrixLinkResult {
            kind: KIND_ROOM_ALIAS,
            primary: a.to_string(),
            event_id: String::new(),
            via: Vec::new(),
        }
    }
    fn user(u: &str) -> MatrixLinkResult {
        MatrixLinkResult {
            kind: KIND_USER,
            primary: u.to_string(),
            event_id: String::new(),
            via: Vec::new(),
        }
    }
    fn event(room_id: &str, ev: &str) -> MatrixLinkResult {
        MatrixLinkResult {
            kind: KIND_EVENT,
            primary: room_id.to_string(),
            event_id: ev.to_string(),
            via: Vec::new(),
        }
    }
    fn with_via(mut r: MatrixLinkResult, via: &[&str]) -> MatrixLinkResult {
        r.via = via.iter().map(|s| s.to_string()).collect();
        r
    }

    // ── matrix.to ────────────────────────────────────────────────────────────

    #[test]
    fn matrix_to_room_id() {
        let r = parse_matrix_link("https://matrix.to/#/!abc123:example.com");
        assert_eq!(r, room("!abc123:example.com"));
    }

    #[test]
    fn matrix_to_room_alias() {
        let r = parse_matrix_link("https://matrix.to/#/#general:example.com");
        assert_eq!(r, alias("#general:example.com"));
    }

    #[test]
    fn matrix_to_over_plain_http_is_not_a_permalink() {
        assert_eq!(
            parse_matrix_link("http://matrix.to/#/@alice:example.com").kind,
            KIND_UNKNOWN
        );
    }

    #[test]
    fn matrix_to_user() {
        let r = parse_matrix_link("https://matrix.to/#/@alice:example.com");
        assert_eq!(r, user("@alice:example.com"));
    }

    #[test]
    fn matrix_to_event() {
        let r = parse_matrix_link("https://matrix.to/#/!abc123:example.com/$xyz789:example.com");
        assert_eq!(r, event("!abc123:example.com", "$xyz789:example.com"));
    }

    #[test]
    fn matrix_to_retains_via_params() {
        let r = parse_matrix_link(
            "https://matrix.to/#/!abc123:example.com?via=matrix.org&via=example.com",
        );
        assert_eq!(
            r,
            with_via(room("!abc123:example.com"), &["matrix.org", "example.com"])
        );
    }

    #[test]
    fn matrix_to_retains_via_before_event() {
        // format: !room:server/$event:server?via=matrix.org
        let r = parse_matrix_link(
            "https://matrix.to/#/!abc123:example.com/$xyz789:example.com?via=matrix.org",
        );
        assert_eq!(
            r,
            with_via(
                event("!abc123:example.com", "$xyz789:example.com"),
                &["matrix.org"]
            )
        );
    }

    #[test]
    fn matrix_to_percent_encoded_via() {
        let r = parse_matrix_link("https://matrix.to/#/!abc123:example.com?via=a%2Eb.example.com");
        assert_eq!(
            r,
            with_via(room("!abc123:example.com"), &["a.b.example.com"])
        );
    }

    #[test]
    fn matrix_to_percent_encoded_user() {
        // @ is %40 in some clients
        let r = parse_matrix_link("https://matrix.to/#/%40alice:example.com");
        assert_eq!(r, user("@alice:example.com"));
    }

    #[test]
    fn matrix_to_unknown_sigil() {
        let r = parse_matrix_link("https://matrix.to/#/bogus");
        assert_eq!(r.kind, KIND_UNKNOWN);
    }

    // ── matrix: URI ───────────────────────────────────────────────────────────

    #[test]
    fn matrix_uri_room_id() {
        let r = parse_matrix_link("matrix:roomid/abc123:example.com");
        assert_eq!(r, room("!abc123:example.com"));
    }

    #[test]
    fn matrix_uri_room_alias() {
        let r = parse_matrix_link("matrix:r/general:example.com");
        assert_eq!(r, alias("#general:example.com"));
    }

    #[test]
    fn matrix_uri_user() {
        let r = parse_matrix_link("matrix:u/alice:example.com");
        assert_eq!(r, user("@alice:example.com"));
    }

    #[test]
    fn matrix_uri_roomid_with_event_spec_form() {
        let r = parse_matrix_link("matrix:roomid/abc123:example.com/e/$xyz789:example.com");
        assert_eq!(r, event("!abc123:example.com", "$xyz789:example.com"));
    }

    #[test]
    fn matrix_uri_roomid_with_sigil_less_event() {
        // The spec writes the event id without its `$`.
        let r = parse_matrix_link("matrix:roomid/abc123:example.com/e/xyz789:example.com");
        assert_eq!(r, event("!abc123:example.com", "$xyz789:example.com"));
        let r = parse_matrix_link("matrix:r/general:example.com/e/xyz789:example.com");
        assert_eq!(r.event_id, "$xyz789:example.com");
    }

    #[test]
    fn matrix_uri_roomid_with_legacy_event_tail() {
        let r = parse_matrix_link("matrix:roomid/abc123:example.com/$xyz789:example.com");
        assert_eq!(r, event("!abc123:example.com", "$xyz789:example.com"));
    }

    #[test]
    fn matrix_uri_alias_with_event_keeps_event() {
        let r = parse_matrix_link("matrix:r/general:example.com/e/$xyz789:example.com");
        assert_eq!(r.kind, KIND_ROOM_ALIAS);
        assert_eq!(r.primary, "#general:example.com");
        assert_eq!(r.event_id, "$xyz789:example.com");
    }

    #[test]
    fn matrix_uri_made_up_e_form_is_unknown() {
        // `matrix:e/<room>/<event>` is not in any spec.
        let r = parse_matrix_link("matrix:e/abc123:example.com/$xyz789:example.com");
        assert_eq!(r.kind, KIND_UNKNOWN);
    }

    #[test]
    fn matrix_uri_rejects_invalid_identifiers() {
        assert_eq!(parse_matrix_link("matrix:u/bogus").kind, KIND_UNKNOWN);
        assert_eq!(parse_matrix_link("matrix:roomid/").kind, KIND_UNKNOWN);
        assert_eq!(parse_matrix_link("matrix:r/").kind, KIND_UNKNOWN);
        assert_eq!(
            parse_matrix_link("matrix:roomid/abc123:example.com/e/").kind,
            KIND_UNKNOWN
        );
        assert_eq!(parse_matrix_link("matrix:roomid").kind, KIND_UNKNOWN);
    }

    #[test]
    fn matrix_to_alias_with_event_keeps_event() {
        let r = parse_matrix_link("https://matrix.to/#/#general:example.com/$xyz789:example.com");
        assert_eq!(r.kind, KIND_ROOM_ALIAS);
        assert_eq!(r.primary, "#general:example.com");
        assert_eq!(r.event_id, "$xyz789:example.com");
    }

    #[test]
    fn matrix_to_user_with_event_is_unknown() {
        let r = parse_matrix_link("https://matrix.to/#/@alice:example.com/$xyz789:example.com");
        assert_eq!(r.kind, KIND_UNKNOWN);
    }

    #[test]
    fn matrix_uri_ignores_action_join() {
        let r = parse_matrix_link("matrix:r/general:example.com?action=join");
        assert_eq!(r, alias("#general:example.com"));
    }

    #[test]
    fn matrix_uri_retains_via() {
        let r = parse_matrix_link(
            "matrix:roomid/abc123:example.com?via=matrix.org&action=join&via=other.org",
        );
        assert_eq!(
            r,
            with_via(room("!abc123:example.com"), &["matrix.org", "other.org"])
        );
    }

    #[test]
    fn percent_decode_keeps_utf8_sequences() {
        // Encoded and literal UTF-8 must decode to the same character.
        assert_eq!(percent_decode("%C3%A9"), "é");
        assert_eq!(percent_decode("é"), "é");
        assert_eq!(percent_decode("a%2Eb"), "a.b");
        // Malformed escapes are left as they are.
        assert_eq!(percent_decode("%zz%4"), "%zz%4");
    }

    #[test]
    fn matrix_uri_via_decodes_non_ascii() {
        let r = parse_matrix_link("matrix:roomid/abc123:example.com?via=b%C3%BCcher.example");
        assert_eq!(
            r,
            with_via(room("!abc123:example.com"), &["bücher.example"])
        );
    }

    #[test]
    fn matrix_uri_unknown_segment() {
        let r = parse_matrix_link("matrix:bogus/whatever");
        assert_eq!(r.kind, KIND_UNKNOWN);
    }

    #[test]
    fn non_matrix_url_is_unknown() {
        let r = parse_matrix_link("https://example.com/foo");
        assert_eq!(r.kind, KIND_UNKNOWN);
    }

    #[test]
    fn empty_string_is_unknown() {
        let r = parse_matrix_link("");
        assert_eq!(r.kind, KIND_UNKNOWN);
    }
}
