//! Sender-side MSC4095 bundled URL previews (`com.beeper.linkpreviews`),
//! behind two opt-ins set via `set_bundled_url_previews`:
//!
//! 1. `enabled`: preview the first few http(s) URLs of an outgoing `m.text`
//!    send/reply/thread message/edit through the user's own homeserver
//!    (`/preview_url`) so the result can be bundled into the event.
//! 2. `direct` (only meaningful with 1): fetch the page from Tesseract itself
//!    through the SSRF-guarded client (`crate::net_guard`) and parse it with
//!    `crate::url_preview`, falling back to the homeserver when that fails.
//!    Sender-side only — previews for received links always go through the
//!    homeserver (`get_url_preview_async`).
//!
//! The homeserver is skipped entirely when it advertises MSC4452's
//! `m.preview_url` capability as disabled (`homeserver_preview_url_enabled`).
//!
//! Generation is its own FFI call (`generate_url_previews`, returning JSON)
//! rather than part of the send, so the shell can run the slow fetch on its
//! read pool and pass the result to the send calls' `url_previews_json`
//! argument — keeping the single worker that serialises `&mut` FFI calls
//! (room switches, reactions, …) free while a preview is fetched.
//!
//! Preview images are re-uploaded encrypted in E2EE rooms so the image isn't
//! left readable in plaintext on the media repo. Every failure is silent: the
//! message is sent with whatever previews succeeded, or none at all (in which
//! case `url_previews` stays absent and receivers fetch their own preview).

use matrix_sdk::ruma::events::room::message::{MessageType, RoomMessageEventContent, UrlPreview};
use std::sync::atomic::Ordering;

use super::ClientFfi;

/// At most this many URLs per message get a bundled preview.
pub(super) const MAX_PREVIEWS_PER_MESSAGE: usize = 3;

/// Candidate URLs for bundled previews in a message `body`: http(s) links as
/// detected by `linkify` (same detection as `text_utils::find_url_spans`),
/// excluding any URL that appears only inside a Markdown code region (fenced
/// block or inline code span, via `text_utils::find_preview_url_spans`) — a
/// URL shown as code was not meant as a link to preview. Kept verbatim so
/// each `matched_url` is a substring of the body (receivers drop previews
/// whose `matched_url` isn't), de-duplicated, capped at
/// `MAX_PREVIEWS_PER_MESSAGE`. `matrix.to` permalinks are skipped — they're
/// rendered as pills, never as preview cards.
pub(super) fn preview_candidate_urls(body: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for span in crate::text_utils::find_preview_url_spans(body) {
        let s = span.url.as_str();
        let Ok(parsed) = url::Url::parse(s) else {
            continue;
        };
        if !matches!(parsed.scheme(), "http" | "https") || parsed.host_str() == Some("matrix.to") {
            continue;
        }
        if !out.iter().any(|u| u == s) {
            out.push(s.to_owned());
        }
        if out.len() == MAX_PREVIEWS_PER_MESSAGE {
            break;
        }
    }
    out
}

/// Attach `previews` to an `m.text` content. No-op for other msgtypes or an
/// empty list — an empty `url_previews` array would tell receivers "no
/// preview, don't fetch one", which is not what a failed generation means.
pub(super) fn set_url_previews(content: &mut RoomMessageEventContent, previews: Vec<UrlPreview>) {
    if previews.is_empty() {
        return;
    }
    if let MessageType::Text(text) = &mut content.msgtype {
        text.url_previews = Some(previews);
    }
}

/// Serialize generated previews for the `url_previews_json` FFI argument;
/// empty string for none.
pub(super) fn url_previews_to_json(previews: &[UrlPreview]) -> String {
    if previews.is_empty() {
        return String::new();
    }
    serde_json::to_string(previews).unwrap_or_default()
}

/// Inverse of `url_previews_to_json`. Empty or malformed input yields no
/// previews (the send goes out without them rather than failing).
pub(super) fn parse_url_previews_json(json: &str) -> Vec<UrlPreview> {
    if json.trim().is_empty() {
        return Vec::new();
    }
    serde_json::from_str(json).unwrap_or_default()
}

/// Text/image fields of a homeserver `/preview_url` response.
#[derive(Debug, Default, PartialEq)]
pub(super) struct HsPreview {
    pub title: Option<String>,
    pub description: Option<String>,
    pub canonical_url: Option<String>,
    pub image_mxc: Option<matrix_sdk::ruma::OwnedMxcUri>,
    pub image_width: Option<u32>,
    pub image_height: Option<u32>,
    pub image_type: Option<String>,
    pub image_size: Option<u64>,
}

/// Parse a homeserver `/preview_url` JSON body (an OpenGraph key/value map
/// with `og:image` rewritten to an mxc URI). `None` when it carries nothing
/// displayable — mirrors `Client::parse_url_preview` on the C++ side.
pub(super) fn parse_hs_preview_json(json: &str) -> Option<HsPreview> {
    let v: serde_json::Value = serde_json::from_str(json).ok()?;
    let obj = v.as_object()?;
    let text = |k: &str, max: usize| {
        obj.get(k)
            .and_then(|x| x.as_str())
            .and_then(|s| crate::url_preview::clean_text(s, max))
    };
    // Synapse emits dimensions as numbers; some servers pass the page's
    // string values through untouched.
    let num = |k: &str| {
        obj.get(k).and_then(|x| {
            x.as_u64()
                .or_else(|| x.as_str().and_then(|s| s.trim().parse().ok()))
        })
    };
    let image_mxc = obj
        .get("og:image")
        .and_then(|x| x.as_str())
        .filter(|s| s.starts_with("mxc://"))
        .map(matrix_sdk::ruma::OwnedMxcUri::from)
        .filter(|m| m.is_valid());
    let dim = |k: &str| {
        num(k)
            .and_then(|d| u32::try_from(d).ok())
            .filter(|&d| d > 0)
    };
    let has_image = image_mxc.is_some();
    let p = HsPreview {
        title: text("og:title", crate::url_preview::MAX_TITLE_CHARS),
        description: text("og:description", crate::url_preview::MAX_DESCRIPTION_CHARS),
        canonical_url: obj
            .get("og:url")
            .and_then(|x| x.as_str())
            .filter(|s| s.starts_with("https://") || s.starts_with("http://"))
            .map(str::to_owned),
        image_width: dim("og:image:width").filter(|_| has_image),
        image_height: dim("og:image:height").filter(|_| has_image),
        image_type: obj
            .get("og:image:type")
            .and_then(|x| x.as_str())
            .filter(|s| s.starts_with("image/") && has_image)
            .map(str::to_owned),
        image_size: num("matrix:image:size").filter(|_| has_image),
        image_mxc,
    };
    (p.title.is_some() || p.description.is_some() || p.image_mxc.is_some()).then_some(p)
}

impl ClientFfi {
    /// Configure sender-side bundled URL previews. `enabled` turns
    /// generation on (via the homeserver's `/preview_url`); `direct` makes
    /// Tesseract fetch pages itself first (homeserver fallback). Thread-safe;
    /// takes effect on the next send.
    pub fn set_bundled_url_previews(&self, enabled: bool, direct: bool) {
        self.bundled_url_previews.store(enabled, Ordering::Relaxed);
        self.bundled_url_previews_direct
            .store(enabled && direct, Ordering::Relaxed);
    }
}

#[cfg(test)]
impl ClientFfi {
    /// Test stub: no client, so never any previews.
    pub fn generate_url_previews(&self, _room_id: &str, _body: &str) -> String {
        String::new()
    }
}

#[cfg(not(test))]
mod net {
    use super::*;
    use crate::net_guard::{guarded_client, guarded_get};
    use crate::url_preview::{decode_html, extract, is_html_content_type};
    use matrix_sdk::ruma::events::room::message::PreviewImage;
    use matrix_sdk::ruma::UInt;
    use matrix_sdk::Client;
    use std::time::Duration;
    use url::Url;

    /// Wall-clock budget for one URL's preview (fetch, optional fallback and
    /// image upload). URLs are previewed concurrently, so this bounds how
    /// long a send waits before going out.
    const PREVIEW_BUDGET: Duration = Duration::from_secs(8);
    /// Share of `PREVIEW_BUDGET` the direct fetch may use, leaving the rest
    /// for the homeserver fallback.
    const DIRECT_ATTEMPT_BUDGET: Duration = Duration::from_secs(5);
    /// Cap on a preview image, both fetched directly and re-uploaded from
    /// the homeserver in encrypted rooms.
    const MAX_PREVIEW_IMAGE_BYTES: usize = 5 * 1024 * 1024;

    impl ClientFfi {
        /// Generate bundled previews for `body` (an outgoing `m.text` body
        /// for `room_id`) and return them as JSON for the send calls'
        /// `url_previews_json` argument. Empty string — without touching the
        /// network — when the opt-in is off or there are no candidate links;
        /// also empty when nothing could be previewed. Blocks the calling
        /// (C++ worker) thread for at most `PREVIEW_BUDGET`, and is cut
        /// short by `stop_sync`. `&self` only, so the shell runs it on its
        /// read pool rather than the thread that serialises `&mut` calls.
        pub fn generate_url_previews(&self, room_id: &str, body: &str) -> String {
            if !self.bundled_url_previews.load(Ordering::Relaxed) {
                return String::new();
            }
            let urls = preview_candidate_urls(body);
            if urls.is_empty() {
                return String::new();
            }
            let Some(client) = self.client.clone() else {
                return String::new();
            };
            let Some(room) = super::super::parse_room_id(room_id)
                .ok()
                .and_then(|rid| client.get_room(&rid))
            else {
                return String::new();
            };
            // A proxy the user configured resolves and connects for us, so the
            // address checks in the direct path cannot see the real target. No
            // direct previews are attempted then; the homeserver's still are.
            // (The OS/env proxy of the default mode is not treated this way.)
            let explicit_proxy = !matches!(
                crate::net_proxy::current(),
                crate::net_proxy::ProxyMode::System | crate::net_proxy::ProxyMode::None
            );
            let direct_http =
                if self.bundled_url_previews_direct.load(Ordering::Relaxed) && !explicit_proxy {
                    guarded_client(DIRECT_ATTEMPT_BUDGET)
                } else {
                    None
                };
            // MSC4452: a homeserver that turned `/preview_url` off would only
            // answer 403, so don't ask it; direct fetches still run.
            // In an encrypted room the homeserver must not learn the links, so
            // the homeserver fallback is off there; only the direct fetch runs.
            let encrypted = room.encryption_state().is_encrypted();
            let homeserver_ok =
                self.homeserver_preview_url_enabled.load(Ordering::Relaxed) && !encrypted;
            if direct_http.is_none() && !homeserver_ok {
                return String::new();
            }
            let _in_flight = self.in_flight_guard("url-preview");
            let previews: Vec<UrlPreview> = self
                .block_on_cancellable(async move {
                    let tasks = urls.into_iter().map(|url| {
                        let client = client.clone();
                        let http = direct_http.clone();
                        async move {
                            tokio::time::timeout(
                                PREVIEW_BUDGET,
                                preview_one(&client, encrypted, http.as_ref(), homeserver_ok, url),
                            )
                            .await
                            .ok()
                            .flatten()
                        }
                    });
                    futures_util::future::join_all(tasks)
                        .await
                        .into_iter()
                        .flatten()
                        .collect()
                })
                .unwrap_or_default();
            url_previews_to_json(&previews)
        }
    }

    async fn preview_one(
        client: &Client,
        encrypted: bool,
        direct_http: Option<&reqwest::Client>,
        homeserver_ok: bool,
        url: String,
    ) -> Option<UrlPreview> {
        if let Some(http) = direct_http {
            let direct = tokio::time::timeout(
                DIRECT_ATTEMPT_BUDGET,
                direct_preview(client, encrypted, http, &url),
            )
            .await
            .ok()
            .flatten();
            if direct.is_some() {
                return direct;
            }
            if !homeserver_ok {
                return None;
            }
            tracing::debug!(
                "url preview: direct fetch failed for {url}; falling back to homeserver"
            );
        }
        homeserver_preview(client, encrypted, &url).await
    }

    fn content_type(resp: &reqwest::Response) -> String {
        resp.headers()
            .get(reqwest::header::CONTENT_TYPE)
            .and_then(|v| v.to_str().ok())
            .unwrap_or("")
            .to_owned()
    }

    async fn direct_preview(
        client: &Client,
        encrypted: bool,
        http: &reqwest::Client,
        url: &str,
    ) -> Option<UrlPreview> {
        let (resp, final_url) = guarded_get(http, Url::parse(url).ok()?).await?;
        let ct = content_type(&resp);
        if !is_html_content_type(&ct) {
            return None;
        }
        let bytes =
            super::super::media::read_body_capped(resp, url, super::super::media::MAX_URL_BYTES)
                .await;
        if bytes.is_empty() {
            return None;
        }
        let meta = extract(&decode_html(&bytes, Some(&ct)), &final_url)?;

        let mut preview = UrlPreview::matched_url(url.to_owned());
        preview.title = meta.title;
        preview.description = meta.description;
        preview.url = meta.canonical_url;
        if let Some(image_url) = meta.image_url {
            if let Some((bytes, mime)) = direct_image(http, image_url).await {
                preview.image = upload_preview_image(
                    client,
                    encrypted,
                    bytes,
                    mime,
                    meta.image_width,
                    meta.image_height,
                )
                .await;
            }
        }
        Some(preview)
    }

    async fn direct_image(http: &reqwest::Client, url: Url) -> Option<(Vec<u8>, String)> {
        let label = url.to_string();
        let (resp, _) = guarded_get(http, url).await?;
        let mime = content_type(&resp)
            .split(';')
            .next()
            .unwrap_or("")
            .trim()
            .to_ascii_lowercase();
        // SVG is skipped: it's a script-capable document, not a raster
        // image, and clients generally won't render it as a thumbnail.
        if !mime.starts_with("image/") || mime == "image/svg+xml" {
            return None;
        }
        let bytes =
            super::super::media::read_body_capped(resp, &label, MAX_PREVIEW_IMAGE_BYTES).await;
        // The header is the server's claim. Use the type the bytes sniff as, and
        // only when that is a raster image; a document labelled as an image is dropped.
        let sniffed = infer::get(&bytes)?.mime_type().to_owned();
        if !PREVIEW_IMAGE_TYPES.contains(&sniffed.as_str()) {
            return None;
        }
        (!bytes.is_empty()).then_some((bytes, sniffed))
    }

    /// Image types a preview may carry: what the UI can decode and show. Not
    /// every `image/*` the sniffer knows (it also names drawings, Photoshop and
    /// camera-raw files) and never SVG.
    const PREVIEW_IMAGE_TYPES: [&str; 10] = [
        "image/jpeg",
        "image/png",
        "image/gif",
        "image/webp",
        "image/avif",
        "image/heif",
        "image/jxl",
        "image/bmp",
        "image/tiff",
        "image/vnd.microsoft.icon",
    ];

    async fn homeserver_preview(client: &Client, encrypted: bool, url: &str) -> Option<UrlPreview> {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;

        let json = client.media().get_media_preview(url, None).await.ok()??;
        if json.get().len() > super::super::media::MAX_URL_BYTES {
            return None;
        }
        let hs = parse_hs_preview_json(json.get())?;

        let mut preview = UrlPreview::matched_url(url.to_owned());
        preview.title = hs.title;
        preview.description = hs.description;
        preview.url = hs.canonical_url;
        if let Some(mxc) = hs.image_mxc {
            if !encrypted {
                // Unencrypted room: the homeserver's own copy is already
                // public media, so reference it directly.
                let mut img = PreviewImage::plain(mxc);
                img.width = hs.image_width.map(UInt::from);
                img.height = hs.image_height.map(UInt::from);
                img.mimetype = hs.image_type;
                img.size = hs.image_size.and_then(|s| UInt::try_from(s).ok());
                preview.image = Some(img);
            } else {
                let request = MediaRequestParameters {
                    source: MediaSource::Plain(mxc),
                    format: MediaFormat::File,
                };
                if let Ok(bytes) = client.media().get_media_content(&request, true).await {
                    if !bytes.is_empty() && bytes.len() <= MAX_PREVIEW_IMAGE_BYTES {
                        let mime = hs
                            .image_type
                            .unwrap_or_else(|| "application/octet-stream".to_owned());
                        preview.image = upload_preview_image(
                            client,
                            true,
                            bytes,
                            mime,
                            hs.image_width,
                            hs.image_height,
                        )
                        .await;
                    }
                }
            }
        }
        Some(preview)
    }

    /// Upload a preview image — encrypted (`beeper:image:encryption`) in E2EE
    /// rooms, plain (`og:image` mxc) otherwise — and describe it. `None` on
    /// upload failure; the caller keeps the text-only preview.
    async fn upload_preview_image(
        client: &Client,
        encrypted: bool,
        bytes: Vec<u8>,
        mime: String,
        width: Option<u32>,
        height: Option<u32>,
    ) -> Option<PreviewImage> {
        let size = UInt::try_from(bytes.len() as u64).ok();
        let mut img = if encrypted {
            let mut cursor = std::io::Cursor::new(bytes);
            let file = client.upload_encrypted_file(&mut cursor).await.ok()?;
            PreviewImage::encrypted(file)
        } else {
            let parsed: mime::Mime = mime.parse().ok()?;
            let mxc = super::super::account::upload_bytes(client, bytes, &parsed)
                .await
                .ok()?;
            PreviewImage::plain(mxc)
        };
        img.size = size;
        img.width = width.map(UInt::from);
        img.height = height.map(UInt::from);
        img.mimetype = Some(mime);
        Some(img)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn candidate_urls_are_verbatim_deduped_and_capped() {
        let body = "see https://a.example/x?y=1, http://b.example and https://a.example/x?y=1 \
                    plus https://c.example https://d.example";
        assert_eq!(
            preview_candidate_urls(body),
            vec![
                "https://a.example/x?y=1",
                "http://b.example",
                "https://c.example"
            ]
        );
        for u in preview_candidate_urls(body) {
            assert!(body.contains(&u));
        }
    }

    #[test]
    fn candidate_urls_skip_non_http_matrix_to_and_bare_domains() {
        let body = "ftp://files.example mailto:a@b.example example.com \
                    https://matrix.to/#/@alice:example.org";
        assert!(preview_candidate_urls(body).is_empty());
        assert!(preview_candidate_urls("no links here").is_empty());
    }

    #[test]
    fn candidate_urls_skip_fenced_and_inline_code() {
        let fenced = "see\n```\nhttp://example.com\n```\nthanks";
        assert!(preview_candidate_urls(fenced).is_empty());

        let inline = "try `http://example.com` sometime";
        assert!(preview_candidate_urls(inline).is_empty());

        let mixed = "see https://example.org but not ```\nhttp://example.com\n```";
        assert_eq!(preview_candidate_urls(mixed), vec!["https://example.org"]);
    }

    #[test]
    fn set_url_previews_only_touches_nonempty_text() {
        let mut text = RoomMessageEventContent::text_plain("https://a.example");
        set_url_previews(&mut text, Vec::new());
        let MessageType::Text(t) = &text.msgtype else {
            panic!()
        };
        assert!(t.url_previews.is_none());

        set_url_previews(
            &mut text,
            vec![UrlPreview::matched_url("https://a.example".into())],
        );
        let MessageType::Text(t) = &text.msgtype else {
            panic!()
        };
        assert_eq!(t.url_previews.as_ref().map(Vec::len), Some(1));

        let mut notice = RoomMessageEventContent::notice_plain("https://a.example");
        set_url_previews(
            &mut notice,
            vec![UrlPreview::matched_url("https://a.example".into())],
        );
        let json = serde_json::to_value(&notice).unwrap();
        assert!(json.get("com.beeper.linkpreviews").is_none());
    }

    #[test]
    fn bundled_preview_serializes_under_beeper_key() {
        let mut content = RoomMessageEventContent::text_plain("look https://a.example");
        let mut p = UrlPreview::matched_url("https://a.example".into());
        p.title = Some("A".into());
        set_url_previews(&mut content, vec![p]);
        let json = serde_json::to_value(&content).unwrap();
        let arr = json["com.beeper.linkpreviews"].as_array().unwrap();
        assert_eq!(arr[0]["matched_url"], "https://a.example");
        assert_eq!(arr[0]["og:title"], "A");
    }

    #[test]
    fn previews_json_round_trips_and_tolerates_garbage() {
        let mut p = UrlPreview::matched_url("https://a.example".into());
        p.title = Some("A".into());
        p.image = Some(
            matrix_sdk::ruma::events::room::message::PreviewImage::plain(
                "mxc://example.org/abc".into(),
            ),
        );
        let json = url_previews_to_json(&[p]);
        let back = parse_url_previews_json(&json);
        assert_eq!(back.len(), 1);
        assert_eq!(back[0].matched_url.as_deref(), Some("https://a.example"));
        assert_eq!(back[0].title.as_deref(), Some("A"));
        assert!(back[0].image.is_some());

        assert_eq!(url_previews_to_json(&[]), "");
        assert!(parse_url_previews_json("").is_empty());
        assert!(parse_url_previews_json("{not json").is_empty());
    }

    #[test]
    fn generate_stub_is_empty_without_a_client() {
        let c = ClientFfi::new();
        c.set_bundled_url_previews(true, true);
        assert_eq!(
            c.generate_url_previews("!r:example.org", "https://a.example"),
            ""
        );
    }

    #[test]
    fn parses_synapse_preview_json() {
        let json = r#"{
            "og:title": "Matrix.org",
            "og:description": "The   open protocol",
            "og:url": "https://matrix.org/",
            "og:image": "mxc://matrix.org/abc",
            "og:image:width": 800,
            "og:image:height": "400",
            "og:image:type": "image/png",
            "matrix:image:size": 1234
        }"#;
        let p = parse_hs_preview_json(json).unwrap();
        assert_eq!(p.title.as_deref(), Some("Matrix.org"));
        assert_eq!(p.description.as_deref(), Some("The open protocol"));
        assert_eq!(p.canonical_url.as_deref(), Some("https://matrix.org/"));
        assert_eq!(
            p.image_mxc.as_ref().map(|m| m.as_str()),
            Some("mxc://matrix.org/abc")
        );
        assert_eq!((p.image_width, p.image_height), (Some(800), Some(400)));
        assert_eq!(p.image_type.as_deref(), Some("image/png"));
        assert_eq!(p.image_size, Some(1234));
    }

    #[test]
    fn hs_preview_rejects_non_mxc_image_and_empty_bodies() {
        let p = parse_hs_preview_json(
            r#"{"og:title":"T","og:image":"https://x/y.png","og:image:width":5}"#,
        )
        .unwrap();
        assert_eq!(p.image_mxc, None);
        assert_eq!(p.image_width, None);
        assert_eq!(parse_hs_preview_json("{}"), None);
        assert_eq!(parse_hs_preview_json(r#"{"og:title":"  "}"#), None);
        assert_eq!(parse_hs_preview_json("not json"), None);
        assert_eq!(parse_hs_preview_json("[]"), None);
    }

    #[test]
    fn set_bundled_url_previews_direct_requires_enabled() {
        let c = ClientFfi::new();
        c.set_bundled_url_previews(false, true);
        assert!(!c.bundled_url_previews.load(Ordering::Relaxed));
        assert!(!c.bundled_url_previews_direct.load(Ordering::Relaxed));
        c.set_bundled_url_previews(true, true);
        assert!(c.bundled_url_previews.load(Ordering::Relaxed));
        assert!(c.bundled_url_previews_direct.load(Ordering::Relaxed));
        c.set_bundled_url_previews(true, false);
        assert!(!c.bundled_url_previews_direct.load(Ordering::Relaxed));
    }
}
