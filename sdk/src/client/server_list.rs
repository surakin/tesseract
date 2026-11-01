/// Homeserver lists for the login view's server browser.
///
/// A `ServerListProvider` knows where one list lives and how to parse it;
/// everything else (HTTPS fetch with a size cap, disk cache, stale fallback)
/// is shared. `JoinMatrix` (servers.joinmatrix.org) is the only provider so
/// far; add another by implementing the trait and returning it from
/// `default_provider()`. Parsing is defensive (every field optional) and only
/// servers that advertise sliding sync are kept, since Tesseract can't sync
/// without it.
use std::path::Path;
use std::time::{Duration, SystemTime};

use serde::Deserialize;

const MAX_BODY_BYTES: usize = 1024 * 1024;
const CACHE_TTL: Duration = Duration::from_secs(24 * 60 * 60);

/// One source of homeserver suggestions.
trait ServerListProvider: Sync {
    /// Name shown to the user ("Server list from {name}").
    fn display_name(&self) -> &'static str;
    /// `https://` page for the list's own site, opened from the browser.
    fn homepage(&self) -> &'static str;
    /// `https://` URL the list is fetched from.
    fn list_url(&self) -> &'static str;
    /// File name for the on-disk cache, inside the cache directory.
    fn cache_file_name(&self) -> &'static str;
    /// Turns a fetched body into entries; garbage yields an empty list.
    fn parse(&self, body: &str) -> Vec<crate::ffi::ServerListEntryFfi>;
}

/// The community list at servers.joinmatrix.org.
struct JoinMatrix;

impl ServerListProvider for JoinMatrix {
    fn display_name(&self) -> &'static str {
        "servers.joinmatrix.org"
    }
    fn homepage(&self) -> &'static str {
        "https://servers.joinmatrix.org/"
    }
    fn list_url(&self) -> &'static str {
        "https://servers.joinmatrix.org/servers.json"
    }
    fn cache_file_name(&self) -> &'static str {
        "server_list.json"
    }
    fn parse(&self, body: &str) -> Vec<crate::ffi::ServerListEntryFfi> {
        parse_server_list(body)
    }
}

/// The provider the login view uses.
fn default_provider() -> &'static dyn ServerListProvider {
    &JoinMatrix
}

#[derive(Deserialize, Default)]
#[serde(default)]
struct RawEntry {
    name: Option<String>,
    client_domain: Option<String>,
    homepage: Option<String>,
    description: Option<String>,
    staff_jur: Option<String>,
    languages: Vec<String>,
    software: Option<String>,
    reg_method: Option<String>,
    reg_link: Option<String>,
    rules: Option<String>,
    privacy: Option<String>,
    longstanding: Option<bool>,
    online_status: Option<u8>,
    sliding_sync: Option<bool>,
}

#[derive(Deserialize, Default)]
#[serde(default)]
struct RawList {
    public_servers: Vec<RawEntry>,
    private_servers: Vec<RawEntry>,
}

/// Keeps only absolute `https://` URLs; the strings end up opened in the
/// system browser, so anything else (javascript:, file:, http:) is dropped.
fn https_only(url: Option<String>) -> String {
    match url {
        Some(u) if u.trim().starts_with("https://") => u.trim().to_owned(),
        _ => String::new(),
    }
}

/// `client_domain` is shown and typed into the homeserver field, so reject
/// anything that isn't a plain host[:port].
fn valid_domain(d: &str) -> bool {
    !d.is_empty()
        && d.len() <= 255
        && d.chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | ':'))
}

fn convert(raw: RawEntry, is_public: bool) -> Option<crate::ffi::ServerListEntryFfi> {
    // `false` and "unknown" both drop out: no sliding sync, no usable server.
    if raw.sliding_sync != Some(true) {
        return None;
    }
    let client_domain = raw.client_domain?.trim().to_owned();
    if !valid_domain(&client_domain) {
        return None;
    }
    Some(crate::ffi::ServerListEntryFfi {
        name: raw.name.unwrap_or_else(|| client_domain.clone()),
        client_domain,
        homepage: https_only(raw.homepage),
        description: raw.description.unwrap_or_default(),
        jurisdiction: raw.staff_jur.unwrap_or_default(),
        languages: raw.languages,
        software: raw.software.unwrap_or_default(),
        reg_method: raw.reg_method.unwrap_or_default(),
        reg_link: https_only(raw.reg_link),
        rules: https_only(raw.rules),
        privacy: https_only(raw.privacy),
        longstanding: raw.longstanding.unwrap_or(false),
        online: raw.online_status.map_or(true, |s| s == 2),
        is_public,
    })
}

/// Parses the servers.json body. Garbage input yields an empty list.
pub fn parse_server_list(json: &str) -> Vec<crate::ffi::ServerListEntryFfi> {
    let Ok(raw) = serde_json::from_str::<RawList>(json) else {
        return Vec::new();
    };
    let public = raw
        .public_servers
        .into_iter()
        .filter_map(|e| convert(e, true));
    let private = raw
        .private_servers
        .into_iter()
        .filter_map(|e| convert(e, false));
    public.chain(private).collect()
}

async fn fetch_server_list(http: &reqwest::Client, url: &str, version: &str) -> Option<String> {
    let response = http
        .get(url)
        .header("User-Agent", format!("Tesseract/{}", version))
        .timeout(Duration::from_secs(10))
        .send()
        .await
        .ok()?;
    if !response.status().is_success() {
        return None;
    }
    if response
        .content_length()
        .is_some_and(|n| n > MAX_BODY_BYTES as u64)
    {
        return None;
    }
    let bytes = response.bytes().await.ok()?;
    if bytes.len() > MAX_BODY_BYTES {
        return None;
    }
    String::from_utf8(bytes.to_vec()).ok()
}

fn read_cache(path: &Path) -> Option<(String, bool)> {
    let body = std::fs::read_to_string(path).ok()?;
    let fresh = std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| SystemTime::now().duration_since(t).ok())
        .is_some_and(|age| age < CACHE_TTL);
    Some((body, fresh))
}

fn write_cache(path: &Path, body: &str) {
    let tmp = path.with_extension("json.tmp");
    if std::fs::write(&tmp, body).is_ok() && std::fs::rename(&tmp, path).is_err() {
        let _ = std::fs::remove_file(&tmp);
    }
}

const ICON_MAX_BYTES: usize = 64 * 1024;
const ICON_TIMEOUT: Duration = Duration::from_secs(8);

/// `https://<host>[:port]/favicon.ico` for a server's homepage; `None` unless
/// the homepage is an https URL with a host.
fn icon_url(homepage: &str) -> Option<String> {
    let url = url::Url::parse(homepage).ok()?;
    if url.scheme() != "https" {
        return None;
    }
    let mut out = format!("https://{}", url.host_str()?);
    if let Some(port) = url.port() {
        out.push_str(&format!(":{port}"));
    }
    out.push_str("/favicon.ico");
    Some(out)
}

/// Sites often answer a missing favicon with a 200 HTML page; keep only bytes
/// that start like an ICO, PNG, GIF, JPEG or WebP file.
fn looks_like_image(b: &[u8]) -> bool {
    b.starts_with(&[0, 0, 1, 0])
        || b.starts_with(&[0x89, b'P', b'N', b'G'])
        || b.starts_with(b"GIF8")
        || b.starts_with(&[0xFF, 0xD8, 0xFF])
        || (b.len() > 12 && &b[..4] == b"RIFF" && &b[8..12] == b"WEBP")
}

#[cfg(not(test))]
impl super::ClientFfi {
    /// The favicon of a listed server's site, or empty when there is none.
    /// Blocks the calling thread; the caller owns caching.
    pub fn fetch_server_icon(&self, homepage: &str) -> Vec<u8> {
        let Some(url) = icon_url(homepage) else {
            return Vec::new();
        };
        let bytes = self.rt.block_on(super::media::download_guarded(
            &url,
            ICON_MAX_BYTES,
            ICON_TIMEOUT,
        ));
        if looks_like_image(&bytes) {
            bytes
        } else {
            Vec::new()
        }
    }

    pub fn load_server_list(&self, cache_dir: &str) -> crate::ffi::ServerListFfi {
        let provider = default_provider();
        let path = Path::new(cache_dir).join(provider.cache_file_name());
        let result = |entries, from_cache| crate::ffi::ServerListFfi {
            entries,
            ok: true,
            from_cache,
            source_name: provider.display_name().to_owned(),
            source_url: provider.homepage().to_owned(),
        };
        let cached = read_cache(&path);
        if let Some((body, true)) = &cached {
            let entries = provider.parse(body);
            if !entries.is_empty() {
                return result(entries, true);
            }
        }
        let http = self.http_client.clone();
        let version = env!("CARGO_PKG_VERSION");
        if let Some(body) = self
            .rt
            .block_on(fetch_server_list(&http, provider.list_url(), version))
        {
            let entries = provider.parse(&body);
            if !entries.is_empty() {
                write_cache(&path, &body);
                return result(entries, false);
            }
        }
        // Fetch failed: a stale cache beats nothing.
        if let Some((body, _)) = cached {
            let entries = provider.parse(&body);
            if !entries.is_empty() {
                return result(entries, true);
            }
        }
        crate::ffi::ServerListFfi {
            entries: Vec::new(),
            ok: false,
            from_cache: false,
            source_name: String::new(),
            source_url: String::new(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"{
      "public_servers": [
        {"name":"a.example","client_domain":"matrix.a.example","homepage":"https://a.example",
         "description":"A","staff_jur":"Germany","languages":["English"],"software":"Synapse",
         "reg_method":"In-house Element","reg_link":"javascript:alert(1)","rules":"http://a.example/r",
         "privacy":"https://a.example/p","longstanding":true,"online_status":2,"sliding_sync":true},
        {"name":"nosync","client_domain":"nosync.example","sliding_sync":false},
        {"name":"unknown","client_domain":"unknown.example","sliding_sync":null},
        {"name":"missing","client_domain":"missing.example"}
      ],
      "private_servers": [
        {"name":"b.example","client_domain":"b.example:8448","sliding_sync":true,"online_status":0,
         "unexpected_field":{"x":1}},
        {"name":"nodomain","sliding_sync":true},
        {"name":"evil","client_domain":"bad host/<x>","sliding_sync":true}
      ]
    }"#;

    #[test]
    fn keeps_only_sliding_sync_servers() {
        let list = parse_server_list(SAMPLE);
        let names: Vec<_> = list.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["a.example", "b.example"]);
    }

    #[test]
    fn public_flag_and_fields() {
        let list = parse_server_list(SAMPLE);
        assert!(list[0].is_public);
        assert!(!list[1].is_public);
        assert_eq!(list[0].client_domain, "matrix.a.example");
        assert_eq!(list[0].jurisdiction, "Germany");
        assert!(list[0].longstanding);
        assert!(list[0].online);
        assert!(!list[1].online);
    }

    #[test]
    fn non_https_links_dropped() {
        let list = parse_server_list(SAMPLE);
        assert_eq!(list[0].homepage, "https://a.example");
        assert_eq!(list[0].reg_link, "");
        assert_eq!(list[0].rules, "");
        assert_eq!(list[0].privacy, "https://a.example/p");
    }

    #[test]
    fn garbage_yields_empty() {
        assert!(parse_server_list("not json").is_empty());
        assert!(parse_server_list("[]").is_empty());
        assert!(parse_server_list("{}").is_empty());
    }

    #[test]
    fn icon_url_is_origin_favicon_over_https() {
        assert_eq!(
            icon_url("https://a.example/some/path?q=1").as_deref(),
            Some("https://a.example/favicon.ico")
        );
        assert_eq!(
            icon_url("https://a.example:8448/").as_deref(),
            Some("https://a.example:8448/favicon.ico")
        );
        assert_eq!(icon_url("http://a.example"), None);
        assert_eq!(icon_url("javascript:alert(1)"), None);
        assert_eq!(icon_url(""), None);
    }

    #[test]
    fn only_image_bytes_pass() {
        assert!(looks_like_image(&[0, 0, 1, 0, 1, 0]));
        assert!(looks_like_image(&[0x89, b'P', b'N', b'G', 0, 0]));
        assert!(looks_like_image(b"RIFF\0\0\0\0WEBPVP8 "));
        assert!(!looks_like_image(b"<!doctype html><html>"));
        assert!(!looks_like_image(b""));
    }

    #[test]
    fn default_provider_metadata_is_sane() {
        let p = default_provider();
        assert!(p.homepage().starts_with("https://"));
        assert!(p.list_url().starts_with("https://"));
        assert!(!p.display_name().is_empty());
        assert!(p.cache_file_name().ends_with(".json"));
        assert_eq!(p.parse(SAMPLE).len(), 2);
    }

    #[test]
    fn cache_round_trip_is_fresh() {
        let dir = std::env::temp_dir().join(format!("tess-sl-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("server_list.json");
        write_cache(&path, SAMPLE);
        let (body, fresh) = read_cache(&path).unwrap();
        assert_eq!(body, SAMPLE);
        assert!(fresh);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
