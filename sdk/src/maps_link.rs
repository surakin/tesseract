//! Detect Google Maps / OpenStreetMap links in composed message text, and
//! resolve maps shortlinks (goo.gl/maps, maps.app.goo.gl, osm.org/go) to
//! direct coordinates via a best-effort HTTP redirect-follow.
//!
//! Kept free of any `ClientFfi`/matrix-sdk dependency so it's usable and
//! unit-testable in isolation; `sdk/src/client/maps.rs` wires this into the
//! FFI surface (`classify_maps_link` / `resolve_maps_shortlink` /
//! `send_location`).

use std::future::Future;
use std::pin::Pin;
use std::time::Duration;
use url::Url;

/// Outcome of classifying a piece of text as a maps link.
#[derive(Debug, Clone, PartialEq)]
pub enum MapsLinkKind {
    NotAMapsLink,
    /// Coordinates were extracted directly from the URL — no network needed.
    Direct { lat: f64, lon: f64 },
    /// A shortlink that needs an HTTP redirect-follow to resolve coordinates.
    Shortlink { url: String },
}

/// Classify `text` (expected to be the entire trimmed composed message body)
/// as a Google Maps or OpenStreetMap URL. Only matches when the WHOLE string
/// parses as a single recognized URL — `url::Url::parse` rejects any
/// leading/trailing garbage (e.g. a scheme can't contain a space), which is
/// what naturally enforces "URL and nothing else" without extra bookkeeping.
pub fn classify_maps_link(text: &str) -> MapsLinkKind {
    let Ok(url) = Url::parse(text) else {
        return MapsLinkKind::NotAMapsLink;
    };
    if url.scheme() != "http" && url.scheme() != "https" {
        return MapsLinkKind::NotAMapsLink;
    }
    let Some(host) = url.host_str() else {
        return MapsLinkKind::NotAMapsLink;
    };
    let host = host.strip_prefix("www.").unwrap_or(host);
    let path = url.path();

    match host {
        "maps.google.com" => {
            if let Some((lat, lon)) = parse_google_maps(&url) {
                return MapsLinkKind::Direct { lat, lon };
            }
        }
        "google.com" if path.starts_with("/maps") => {
            if let Some((lat, lon)) = parse_google_maps(&url) {
                return MapsLinkKind::Direct { lat, lon };
            }
        }
        "goo.gl" if path.starts_with("/maps/") => {
            return MapsLinkKind::Shortlink {
                url: text.to_string(),
            };
        }
        "maps.app.goo.gl" => {
            return MapsLinkKind::Shortlink {
                url: text.to_string(),
            };
        }
        "openstreetmap.org" => {
            if let Some((lat, lon)) = parse_osm(&url) {
                return MapsLinkKind::Direct { lat, lon };
            }
        }
        "osm.org" if path.starts_with("/go/") => {
            return MapsLinkKind::Shortlink {
                url: text.to_string(),
            };
        }
        _ => {}
    }
    MapsLinkKind::NotAMapsLink
}

/// Parse `lat,lon[,...]` from a comma-separated prefix, ignoring any further
/// comma-separated fields (e.g. a trailing Google Maps zoom level `15z`).
fn parse_lat_lon_prefix(s: &str) -> Option<(f64, f64)> {
    let mut parts = s.split(',');
    let lat: f64 = parts.next()?.trim().parse().ok()?;
    let lon: f64 = parts.next()?.trim().parse().ok()?;
    Some((lat, lon))
}

/// Google Maps direct-coordinate forms: `?q=lat,lon`, a path segment
/// `@lat,lon,zoom` (covers both the short `/maps/@lat,lon,zoom` form and the
/// `/maps/place/Name/@lat,lon,zoom` form), or `/maps/search/lat,lon` — the
/// form goo.gl/maps and maps.app.goo.gl shortlinks actually redirect to.
/// The longitude in that last form may carry a literal leading `+` (e.g.
/// `/maps/search/59.916239,+10.789837`); `f64`'s parser already accepts a
/// leading `+`/`-` sign, so no extra stripping is needed.
fn parse_google_maps(url: &Url) -> Option<(f64, f64)> {
    if let Some((_, q)) = url.query_pairs().find(|(k, _)| k.as_ref() == "q") {
        if let Some(coords) = parse_lat_lon_prefix(&q) {
            return Some(coords);
        }
    }
    let path = url.path();
    if let Some(at) = path.find('@') {
        let segment = path[at + 1..].split('/').next().unwrap_or_default();
        if let Some(coords) = parse_lat_lon_prefix(segment) {
            return Some(coords);
        }
    }
    if let Some(rest) = path.strip_prefix("/maps/search/") {
        let segment = rest.split(['/', '?']).next().unwrap_or_default();
        if let Some(coords) = parse_lat_lon_prefix(segment) {
            return Some(coords);
        }
    }
    None
}

/// OpenStreetMap direct-coordinate forms: `?mlat=&mlon=` query params, or a
/// `#map=zoom/lat/lon` fragment.
fn parse_osm(url: &Url) -> Option<(f64, f64)> {
    let mut mlat = None;
    let mut mlon = None;
    for (k, v) in url.query_pairs() {
        match k.as_ref() {
            "mlat" => mlat = v.parse::<f64>().ok(),
            "mlon" => mlon = v.parse::<f64>().ok(),
            _ => {}
        }
    }
    if let (Some(lat), Some(lon)) = (mlat, mlon) {
        return Some((lat, lon));
    }
    let frag = url.fragment()?;
    let rest = frag.strip_prefix("map=")?;
    let mut parts = rest.split('/');
    let _zoom = parts.next()?;
    let lat: f64 = parts.next()?.parse().ok()?;
    let lon: f64 = parts.next()?.parse().ok()?;
    Some((lat, lon))
}

// ---------------------------------------------------------------------------
// Shortlink resolution
// ---------------------------------------------------------------------------

/// Abstraction over "follow this URL's HTTP redirects and return the final
/// URL", so the resolve-then-classify logic below can be unit-tested with a
/// fake implementation instead of hitting real network in CI.
pub trait ShortlinkFetcher: Send + Sync {
    fn fetch_final_url<'a>(
        &'a self,
        url: &'a str,
        timeout: Duration,
    ) -> Pin<Box<dyn Future<Output = Option<String>> + Send + 'a>>;
}

/// Maximum redirect hops to follow manually before giving up.
const MAX_REDIRECTS: u8 = 10;

/// Production fetcher: manually walks the HTTP redirect chain (GET with
/// redirects disabled, reading the raw `Location` header at each hop)
/// instead of relying on reqwest's automatic redirect-following.
///
/// This matters because OpenStreetMap's `osm.org/go/...` shortlinks
/// terminate in a `Location` header carrying a URL *fragment*
/// (`https://www.openstreetmap.org/#map=18/lat/lon` — OSM's frontend routes
/// off the fragment client-side, there's no server-side page for it).
/// Fragments are never transmitted over HTTP, and reqwest's own redirect
/// follower reports the terminal response's `response.url()` with the
/// fragment already dropped (verified empirically). Reading the `Location`
/// header text ourselves — and never re-deriving the "current" URL from a
/// response's own `url()` — is the only way to keep it. A GET (rather than
/// HEAD) is used because some short-link redirectors don't reliably support
/// HEAD.
pub struct ReqwestShortlinkFetcher;

impl ShortlinkFetcher for ReqwestShortlinkFetcher {
    fn fetch_final_url<'a>(
        &'a self,
        url: &'a str,
        timeout: Duration,
    ) -> Pin<Box<dyn Future<Output = Option<String>> + Send + 'a>> {
        Box::pin(async move {
            let client = reqwest::Client::builder()
                .timeout(timeout)
                .redirect(reqwest::redirect::Policy::none())
                .build()
                .ok()?;
            let mut current = Url::parse(url).ok()?;
            for _ in 0..MAX_REDIRECTS {
                let resp = client.get(current.clone()).send().await.ok()?;
                let Some(location) = resp.headers().get(reqwest::header::LOCATION) else {
                    // Terminal (non-redirect) response — `current` already
                    // holds the last-known target, fragment intact.
                    return Some(current.to_string());
                };
                let location_str = location.to_str().ok()?;
                let next = current.join(location_str).ok()?;
                if next.scheme() != "http" && next.scheme() != "https" {
                    return None;
                }
                current = next;
            }
            None
        })
    }
}

/// Resolve `url` via `fetcher` and classify the redirect target. Returns
/// `Some((lat, lon))` only when resolution succeeds AND the resolved URL is
/// itself a direct-coordinate maps link. `fetcher` is responsible for
/// chasing the underlying HTTP 3xx chain — no further shortlink hop is
/// attempted here.
pub async fn resolve_shortlink_with(
    fetcher: &dyn ShortlinkFetcher,
    url: &str,
    timeout: Duration,
) -> Option<(f64, f64)> {
    let resolved = fetcher.fetch_final_url(url, timeout).await?;
    match classify_maps_link(&resolved) {
        MapsLinkKind::Direct { lat, lon } => Some((lat, lon)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn direct(lat: f64, lon: f64) -> MapsLinkKind {
        MapsLinkKind::Direct { lat, lon }
    }

    #[test]
    fn google_maps_at_form() {
        assert_eq!(
            classify_maps_link("https://www.google.com/maps/@41.403380,-2.174040,15z"),
            direct(41.403380, -2.174040)
        );
    }

    #[test]
    fn google_maps_place_at_form() {
        assert_eq!(
            classify_maps_link(
                "https://www.google.com/maps/place/Somewhere/@41.40338,-2.17404,17z"
            ),
            direct(41.40338, -2.17404)
        );
    }

    #[test]
    fn google_maps_query_form() {
        assert_eq!(
            classify_maps_link("https://www.google.com/maps?q=41.40338,-2.17404"),
            direct(41.40338, -2.17404)
        );
    }

    #[test]
    fn google_maps_search_form_with_plus_longitude() {
        // The actual shape a maps.app.goo.gl shortlink redirects to (verified
        // by curl -L against a real shortlink): a `/maps/search/lat,lon` path
        // with a literal `+` before the longitude.
        assert_eq!(
            classify_maps_link(
                "https://www.google.com/maps/search/59.916239,+10.789837?entry=tts&g_ep=x"
            ),
            direct(59.916239, 10.789837)
        );
    }

    #[test]
    fn google_maps_search_form_without_plus() {
        assert_eq!(
            classify_maps_link("https://www.google.com/maps/search/59.916239,10.789837"),
            direct(59.916239, 10.789837)
        );
    }

    #[test]
    fn google_maps_dot_domain_query_form() {
        assert_eq!(
            classify_maps_link("https://maps.google.com/?q=-33.8688,151.2093"),
            direct(-33.8688, 151.2093)
        );
    }

    #[test]
    fn google_maps_query_with_extra_params() {
        assert_eq!(
            classify_maps_link("https://www.google.com/maps?q=41.40338,-2.17404&hl=en&z=15"),
            direct(41.40338, -2.17404)
        );
    }

    #[test]
    fn google_root_domain_without_maps_path_does_not_match() {
        assert_eq!(
            classify_maps_link("https://www.google.com/search?q=41.40338,-2.17404"),
            MapsLinkKind::NotAMapsLink
        );
    }

    #[test]
    fn osm_query_form() {
        assert_eq!(
            classify_maps_link(
                "https://www.openstreetmap.org/?mlat=41.40338&mlon=-2.17404#map=15/41.40338/-2.17404"
            ),
            direct(41.40338, -2.17404)
        );
    }

    #[test]
    fn osm_fragment_only_form() {
        assert_eq!(
            classify_maps_link("https://www.openstreetmap.org/#map=15/-33.8688/151.2093"),
            direct(-33.8688, 151.2093)
        );
    }

    #[test]
    fn goo_gl_maps_shortlink() {
        assert_eq!(
            classify_maps_link("https://goo.gl/maps/abcd1234"),
            MapsLinkKind::Shortlink {
                url: "https://goo.gl/maps/abcd1234".to_string()
            }
        );
    }

    #[test]
    fn maps_app_goo_gl_shortlink() {
        assert_eq!(
            classify_maps_link("https://maps.app.goo.gl/xyz789"),
            MapsLinkKind::Shortlink {
                url: "https://maps.app.goo.gl/xyz789".to_string()
            }
        );
    }

    #[test]
    fn osm_go_shortlink() {
        assert_eq!(
            classify_maps_link("https://osm.org/go/0EEQjE-"),
            MapsLinkKind::Shortlink {
                url: "https://osm.org/go/0EEQjE-".to_string()
            }
        );
    }

    #[test]
    fn shortlink_with_trailing_slash() {
        assert_eq!(
            classify_maps_link("https://goo.gl/maps/abcd1234/"),
            MapsLinkKind::Shortlink {
                url: "https://goo.gl/maps/abcd1234/".to_string()
            }
        );
    }

    #[test]
    fn plain_text_does_not_match() {
        assert_eq!(
            classify_maps_link("check out this restaurant"),
            MapsLinkKind::NotAMapsLink
        );
    }

    #[test]
    fn link_with_extra_surrounding_text_does_not_match() {
        assert_eq!(
            classify_maps_link("meet me here: https://www.google.com/maps/@41.4,-2.1,15z"),
            MapsLinkKind::NotAMapsLink
        );
    }

    #[test]
    fn unrelated_url_does_not_match() {
        assert_eq!(
            classify_maps_link("https://example.com/@41.4,-2.1,15z"),
            MapsLinkKind::NotAMapsLink
        );
    }

    struct FakeShortlinkFetcher {
        responses: std::collections::HashMap<String, Option<String>>,
    }

    impl ShortlinkFetcher for FakeShortlinkFetcher {
        fn fetch_final_url<'a>(
            &'a self,
            url: &'a str,
            _timeout: Duration,
        ) -> Pin<Box<dyn Future<Output = Option<String>> + Send + 'a>> {
            let result = self.responses.get(url).cloned().flatten();
            Box::pin(async move { result })
        }
    }

    #[tokio::test]
    async fn resolve_shortlink_success() {
        let mut responses = std::collections::HashMap::new();
        responses.insert(
            "https://goo.gl/maps/abcd1234".to_string(),
            Some("https://www.google.com/maps/@41.40338,-2.17404,15z".to_string()),
        );
        let fetcher = FakeShortlinkFetcher { responses };
        let result = resolve_shortlink_with(
            &fetcher,
            "https://goo.gl/maps/abcd1234",
            Duration::from_secs(1),
        )
        .await;
        assert_eq!(result, Some((41.40338, -2.17404)));
    }

    // Hits the real network — not run in CI. Run manually with:
    //   cargo test -p tesseract-sdk-ffi -- --ignored real_network_osm_go_shortlink
    #[tokio::test]
    #[ignore]
    async fn real_network_osm_go_shortlink_resolves_with_fragment_intact() {
        let fetcher = ReqwestShortlinkFetcher;
        let result = resolve_shortlink_with(
            &fetcher,
            "https://osm.org/go/0TuPSmBti--",
            Duration::from_secs(10),
        )
        .await;
        assert!(result.is_some(), "expected coordinates, got {result:?}");
    }

    #[tokio::test]
    async fn resolve_shortlink_success_real_search_form() {
        // Regression test for the actual maps.app.goo.gl redirect shape
        // (verified by curl -L against a real shortlink), which resolves to
        // /maps/search/lat,+lon rather than an @lat,lon path segment.
        let mut responses = std::collections::HashMap::new();
        responses.insert(
            "https://maps.app.goo.gl/KmEWjc5T2jB24hw16".to_string(),
            Some(
                "https://www.google.com/maps/search/59.916239,+10.789837?entry=tts&g_ep=x"
                    .to_string(),
            ),
        );
        let fetcher = FakeShortlinkFetcher { responses };
        let result = resolve_shortlink_with(
            &fetcher,
            "https://maps.app.goo.gl/KmEWjc5T2jB24hw16",
            Duration::from_secs(1),
        )
        .await;
        assert_eq!(result, Some((59.916239, 10.789837)));
    }

    #[tokio::test]
    async fn resolve_shortlink_fetch_failure_returns_none() {
        let fetcher = FakeShortlinkFetcher {
            responses: std::collections::HashMap::new(),
        };
        let result = resolve_shortlink_with(
            &fetcher,
            "https://goo.gl/maps/doesnotexist",
            Duration::from_secs(1),
        )
        .await;
        assert_eq!(result, None);
    }

    #[tokio::test]
    async fn resolve_shortlink_non_coordinate_target_returns_none() {
        let mut responses = std::collections::HashMap::new();
        responses.insert(
            "https://goo.gl/maps/abcd1234".to_string(),
            Some("https://consent.google.com/ml?continue=x".to_string()),
        );
        let fetcher = FakeShortlinkFetcher { responses };
        let result = resolve_shortlink_with(
            &fetcher,
            "https://goo.gl/maps/abcd1234",
            Duration::from_secs(1),
        )
        .await;
        assert_eq!(result, None);
    }
}
