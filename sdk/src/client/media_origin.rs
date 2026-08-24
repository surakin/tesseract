//! Origin-key extraction for the per-origin media gate registry
//! (`media_gate_registry.rs`).
//!
//! An "origin" is the remote server a media fetch's stalls/failures actually
//! correlate with — the homeserver behind an `mxc://` URI, or the target host
//! of a direct URL fetch. Keying gate state by origin instead of one shared
//! scalar per lane means a dead/flaky server can only ever throttle itself.
//!
//! Pure and dependency-light like `media_queue.rs`, so it's unit-tested
//! directly without a live `matrix_sdk::Client`.

use matrix_sdk::ruma::events::room::MediaSource;
use matrix_sdk::ruma::{OwnedMxcUri, OwnedRoomId};

// Mirrors media.rs's `#[cfg(not(test))] MEDIA_KIND_*` constants, duplicated
// (rather than imported) so this module stays compilable and testable under
// `cfg(test)` too. Keep these values in sync with media.rs if it ever changes.
const KIND_ROOM_AVATAR: u8 = 0;
const KIND_MXC_THUMB: u8 = 1;
// KIND_SOURCE_THUMB (2) and KIND_SOURCE_FULL (3) share the same parsing (the
// `_` arm below) — both carry a plain mxc:// URI or a JSON MediaSource blob.

/// Opaque, ASCII-lowercased key identifying a media gate's origin. Two inputs
/// that resolve to the same host (case-insensitively, default-port-collapsed)
/// share one key; anything unparseable keys on its own raw string rather than
/// a shared "unknown" bucket, so malformed input still gets its own isolation
/// instead of colliding with unrelated origins.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(super) struct OriginKey(std::sync::Arc<str>);

impl OriginKey {
    fn new(raw: impl AsRef<str>) -> Self {
        Self(raw.as_ref().to_ascii_lowercase().into())
    }
}

impl std::fmt::Display for OriginKey {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

fn mxc_server_name(uri: &OwnedMxcUri) -> Option<String> {
    uri.parts().ok().map(|(server_name, _media_id)| server_name.to_string())
}

/// Origin key for `fetch_media_async`'s `source`, dispatching on `kind`
/// exactly as `download_media_outcome` (media.rs) does, so the two stay easy
/// to keep in sync. Falls back to keying on the raw `source` string whenever
/// it can't be parsed into a server name.
pub(super) fn origin_for_media_kind(kind: u8, source: &str) -> OriginKey {
    let resolved = match kind {
        KIND_ROOM_AVATAR => source
            .parse::<OwnedRoomId>()
            .ok()
            .and_then(|id| id.server_name().map(|s| s.to_string())),
        KIND_MXC_THUMB => mxc_server_name(&OwnedMxcUri::from(source)),
        // KIND_SOURCE_THUMB / KIND_SOURCE_FULL / any unknown kind.
        _ => {
            if source.starts_with("mxc://") {
                mxc_server_name(&OwnedMxcUri::from(source))
            } else {
                match serde_json::from_str::<MediaSource>(source) {
                    Ok(MediaSource::Plain(uri)) => mxc_server_name(&uri),
                    Ok(MediaSource::Encrypted(file)) => mxc_server_name(&file.url),
                    Err(_) => None,
                }
            }
        }
    };
    OriginKey::new(resolved.unwrap_or_else(|| source.to_owned()))
}

/// Origin key for a plain HTTP(S) URL fetch (`fetch_url_async`, map tiles):
/// the URL's host, with an explicit non-default port kept as part of the key
/// (a different port is a different service, not the same congestion
/// domain). Falls back to keying on the whole URL string if it doesn't parse.
pub(super) fn origin_from_url(url: &str) -> OriginKey {
    match url::Url::parse(url) {
        Ok(parsed) => match parsed.host_str() {
            Some(host) => match parsed.port() {
                Some(port) => OriginKey::new(format!("{host}:{port}")),
                None => OriginKey::new(host),
            },
            None => OriginKey::new(url),
        },
        Err(_) => OriginKey::new(url),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn url_origin_is_case_insensitive() {
        assert_eq!(
            origin_from_url("HTTPS://Tiles.Example.com/a.png"),
            origin_from_url("https://tiles.example.com/b.png")
        );
    }

    #[test]
    fn url_origin_collapses_default_port() {
        assert_eq!(
            origin_from_url("https://example.com/a"),
            origin_from_url("https://example.com:443/a")
        );
    }

    #[test]
    fn url_origin_keeps_nondefault_port_distinct() {
        assert_ne!(
            origin_from_url("https://example.com/a"),
            origin_from_url("https://example.com:8080/a")
        );
    }

    #[test]
    fn url_origin_falls_back_to_whole_string_on_parse_failure() {
        assert_eq!(origin_from_url("not a url").to_string(), "not a url");
    }

    #[test]
    fn distinct_malformed_urls_get_distinct_origins() {
        assert_ne!(origin_from_url("not a url"), origin_from_url("also not a url"));
    }

    #[test]
    fn mxc_thumb_extracts_server_name() {
        let key = origin_for_media_kind(KIND_MXC_THUMB, "mxc://matrix.org/abc123");
        assert_eq!(key.to_string(), "matrix.org");
    }

    #[test]
    fn source_full_extracts_server_name_from_plain_mxc() {
        let key = origin_for_media_kind(3, "mxc://example.org/xyz");
        assert_eq!(key.to_string(), "example.org");
    }

    #[test]
    fn source_thumb_extracts_server_name_from_encrypted_media_source_json() {
        let json = serde_json::json!({
            "url": "mxc://enc.example.org/file1",
            "key": {"kty": "oct", "key_ops": ["encrypt", "decrypt"], "alg": "A256CTR", "k": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "ext": true},
            "iv": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            "hashes": {"sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
            "v": "v2",
        })
        .to_string();
        let key = origin_for_media_kind(2, &json);
        assert_eq!(key.to_string(), "enc.example.org");
    }

    #[test]
    fn room_avatar_extracts_server_name_from_room_id() {
        let key = origin_for_media_kind(KIND_ROOM_AVATAR, "!abc123:example.org");
        assert_eq!(key.to_string(), "example.org");
    }

    #[test]
    fn malformed_source_falls_back_to_raw_string_not_shared_bucket() {
        let a = origin_for_media_kind(KIND_MXC_THUMB, "not-a-valid-mxc-uri-a");
        let b = origin_for_media_kind(KIND_MXC_THUMB, "not-a-valid-mxc-uri-b");
        assert_ne!(a, b);
    }
}
