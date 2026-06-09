//! GIF search provider abstraction.
//!
//! v1 ships a Klipy backend; the `GifProvider` trait keeps the door
//! open for Giphy / self-hosted deployments without touching the FFI or UI
//! layers. The provider is intentionally pure (URL building + response
//! parsing) so it can be unit-tested headlessly — the actual HTTP round-trip
//! lives in the async FFI entry point, which reuses `Client::http_client`.

use serde_json::Value;

/// One provider-agnostic GIF search result. URLs point at a provider CDN: the
/// C++ layer fetches `preview_url` for the inline result strip, and
/// `image_url` at send time.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct GifResult {
    pub id: String,
    /// Small static form (JPEG) shown in the result strip.
    pub preview_url: String,
    pub preview_w: u32,
    pub preview_h: u32,
    /// Animated form re-uploaded into the room (the *send* format). Priority:
    /// MP4 (broadest bridge compatibility) → WebP (smaller) → GIF (universal).
    pub image_url: String,
    pub image_w: u32,
    pub image_h: u32,
    /// MIME of `image_url` — "video/mp4", "image/webp", or "image/gif".
    pub image_mime: String,
    /// Animated form the *strip* displays. The strip animates this through the
    /// native image decoder (no video pipeline), so it prefers WebP → GIF and
    /// only falls back to `image_url` for MP4-only entries. Decoding a small
    /// WebP/GIF is far cheaper than running a GStreamer/MF/AV pipeline per cell.
    pub strip_url: String,
    /// MIME of `strip_url` — "image/webp", "image/gif", or (fallback) the
    /// `image_mime` value.
    pub strip_mime: String,
}

/// A GIF search backend.
pub trait GifProvider {
    /// Build the HTTP GET URL for `query`, capped at `limit` results.
    fn search_url(&self, query: &str, limit: u32) -> String;
    /// Parse a successful response body into results. Malformed individual
    /// entries are skipped; only a non-JSON / wrong-shape body is an error.
    fn parse(&self, body: &str) -> Result<Vec<GifResult>, String>;
}

/// Klipy GIF provider. `api_key` is the credential (a URL **path segment**);
/// `customer_id` is Klipy's per-end-user analytics/monetisation identifier.
/// Endpoint: `GET https://api.klipy.com/api/v1/{api_key}/gifs/search`.
pub struct KlipyProvider {
    pub api_key: String,
    pub customer_id: String,
}

impl KlipyProvider {
    pub fn new(api_key: String, customer_id: String) -> Self {
        Self { api_key, customer_id }
    }
}

// Content rating filter — `g` keeps the picker SFW by default. Klipy also
// accepts `pg`, `pg-13`, `r`.
const KLIPY_RATING: &str = "g";

impl GifProvider for KlipyProvider {
    fn search_url(&self, query: &str, limit: u32) -> String {
        // per_page is clamped to Klipy's documented [8, 50] range.
        let per_page = limit.clamp(8, 50);
        format!(
            "https://api.klipy.com/api/v1/{key}/gifs/search\
             ?q={q}&per_page={pp}&rating={rating}&customer_id={cid}",
            key = urlencode(&self.api_key),
            q = urlencode(query),
            pp = per_page,
            rating = KLIPY_RATING,
            cid = urlencode(&self.customer_id),
        )
    }

    fn parse(&self, body: &str) -> Result<Vec<GifResult>, String> {
        let v: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
        let arr = v
            .get("data")
            .and_then(|d| d.get("data"))
            .and_then(|d| d.as_array())
            .ok_or_else(|| "klipy response missing data.data array".to_string())?;
        Ok(arr.iter().filter_map(parse_klipy_result).collect())
    }
}

/// Parse a single Klipy `data.data[]` entry. Returns `None` (skip) when the
/// entry has no usable animated form. Send-form priority: MP4 (broadest bridge
/// compatibility) → WebP (smaller than GIF) → GIF (universal fallback). The
/// strip thumbnail is the small static JPEG (tiny, no WebP/GIF codec needed).
fn parse_klipy_result(r: &Value) -> Option<GifResult> {
    let file = r.get("file")?;
    // Priority: MP4 first (most bridge-compatible), WebP second (smaller),
    // GIF last. An entry with none of these is unsendable and skipped.
    let (image_url, image_w, image_h, image_mime) =
        if let Some((u, w, h)) = pick_format(file, "mp4", &["md", "sm", "hd"]) {
            (u, w, h, "video/mp4".to_string())
        } else if let Some((u, w, h)) = pick_format(file, "webp", &["md", "sm", "hd"]) {
            (u, w, h, "image/webp".to_string())
        } else {
            let (u, w, h) = pick_format(file, "gif", &["md", "sm", "hd"])?;
            (u, w, h, "image/gif".to_string())
        };
    if image_url.is_empty() {
        return None;
    }
    // Strip-display form: animated via the native image decoder, so prefer
    // WebP (small) → GIF (universal). Falls back to the send form only when the
    // entry has neither (an MP4-only result), in which case the strip decodes
    // the MP4 through the video pipeline.
    let (strip_url, strip_mime) =
        if let Some((u, _, _)) = pick_format(file, "webp", &["md", "sm", "hd"]) {
            (u, "image/webp".to_string())
        } else if let Some((u, _, _)) = pick_format(file, "gif", &["md", "sm", "hd"]) {
            (u, "image/gif".to_string())
        } else {
            (image_url.clone(), image_mime.clone())
        };
    let (preview_url, preview_w, preview_h) =
        pick_format(file, "jpg", &["sm", "xs", "md"]).unwrap_or_default();

    // Klipy ids are JSON numbers; stringify for the provider-agnostic struct.
    let id = r
        .get("id")
        .map(|i| {
            i.as_u64()
                .map(|n| n.to_string())
                .unwrap_or_else(|| i.as_str().unwrap_or("").to_string())
        })
        .unwrap_or_default();

    Some(GifResult {
        id,
        preview_url,
        preview_w,
        preview_h,
        image_url,
        image_w,
        image_h,
        image_mime,
        strip_url,
        strip_mime,
    })
}

/// Resolve `file.<tier>.<fmt>.{url,width,height}` for the first `tier` in the
/// priority list that carries a non-empty URL for format `fmt`.
fn pick_format(file: &Value, fmt: &str, tiers: &[&str])
    -> Option<(String, u32, u32)>
{
    for tier in tiers {
        let f = match file.get(tier).and_then(|t| t.get(fmt)) {
            Some(f) => f,
            None => continue,
        };
        let url = f.get("url").and_then(|u| u.as_str()).unwrap_or("");
        if url.is_empty() {
            continue;
        }
        let w = f.get("width").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
        let h = f.get("height").and_then(|x| x.as_u64()).unwrap_or(0) as u32;
        return Some((url.to_string(), w, h));
    }
    None
}

/// Derive Klipy's `customer_id` from a user's MXID without leaking the
/// identity: SHA-256 the MXID and hex-encode the first 16 bytes (128 bits).
/// Stable per-user (same MXID → same id) but not reversible to the MXID.
fn hashed_customer_id(mxid: &str) -> String {
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(mxid.as_bytes());
    digest.iter().take(16).map(|b| format!("{b:02x}")).collect()
}

/// Percent-encode a query/credential component (RFC 3986 unreserved set kept
/// literal, everything else `%`-escaped). Avoids pulling in a url crate for a
/// few request components.
fn urlencode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char)
            }
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

/// Where a piece of media lives after upload: a plaintext `mxc://` URL
/// (unencrypted room) or a serialized ruma `EncryptedFile` JSON object
/// (encrypted room). Built by the caller after up-loading; consumed by
/// [`build_gif_video_content`].
#[derive(Debug, Clone)]
pub(crate) enum GifMedia {
    Plain(String),
    Encrypted(Value),
}

/// Build the raw `m.room.message` content for a GIF sent as an `m.video`,
/// carrying the `fi.mau.gif` vendor hint (in `content.info`) so capable clients
/// — including Tesseract's own parser, `timeline_convert.rs` — autoplay, loop,
/// mute and hide controls. The video source goes in `content.url` (plain) or
/// `content.file` (encrypted); the poster thumbnail in `info.thumbnail_url` /
/// `info.thumbnail_file`. A non-empty `thread_root` emits an MSC3440 thread
/// relation, otherwise a non-empty `reply_event_id` emits a bare reply.
pub(crate) fn build_gif_video_content(
    video: GifMedia,
    thumbnail: Option<GifMedia>,
    body: &str,
    mime_type: &str,
    width: u32,
    height: u32,
    size: usize,
    duration_ms: u64,
    thumb_width: u32,
    thumb_height: u32,
    reply_event_id: &str,
    thread_root: &str,
) -> Value {
    let mut info = serde_json::json!({
        "mimetype": mime_type,
        "size": size,
        "fi.mau.gif": true,
    });
    if width != 0 {
        info["w"] = serde_json::json!(width);
    }
    if height != 0 {
        info["h"] = serde_json::json!(height);
    }
    if duration_ms != 0 {
        info["duration"] = serde_json::json!(duration_ms);
    }
    if let Some(thumb) = thumbnail {
        if thumb_width != 0 {
            info["thumbnail_info"] = serde_json::json!({ "w": thumb_width, "h": thumb_height });
        }
        match thumb {
            GifMedia::Plain(url) => info["thumbnail_url"] = serde_json::json!(url),
            GifMedia::Encrypted(file) => info["thumbnail_file"] = file,
        }
    }

    let mut content = serde_json::json!({
        "msgtype": "m.video",
        "body": body,
        "info": info,
    });
    match video {
        GifMedia::Plain(url) => content["url"] = serde_json::json!(url),
        GifMedia::Encrypted(file) => content["file"] = file,
    }

    if !thread_root.is_empty() {
        let in_reply_to_id = if reply_event_id.is_empty() { thread_root } else { reply_event_id };
        let mut relates = serde_json::json!({
            "rel_type": "m.thread",
            "event_id": thread_root,
            "m.in_reply_to": { "event_id": in_reply_to_id },
        });
        if reply_event_id.is_empty() {
            relates["is_falling_back"] = serde_json::json!(true);
        }
        content["m.relates_to"] = relates;
    } else if !reply_event_id.is_empty() {
        content["m.relates_to"] = serde_json::json!({
            "m.in_reply_to": { "event_id": reply_event_id }
        });
    }
    content
}

#[cfg(not(test))]
use super::{err, ok, require_room, try_op, ClientFfi, SendHandler};
#[cfg(not(test))]
use crate::ffi::OpResult;
#[cfg(not(test))]
use parking_lot::Mutex;
use std::sync::Arc;

/// Deliver an async GIF search outcome to C++ via the event handler. Tolerates
/// a detached handler (shutdown) and a contended mutex by dropping.
#[cfg(not(test))]
fn deliver_gif(
    handler: &Option<Arc<Mutex<SendHandler>>>,
    request_id: u64,
    result: Result<Vec<GifResult>, String>,
) {
    let Some(h) = handler else { return };
    let g = h.lock();
    match result {
        Ok(items) => {
            let ffi_items: Vec<crate::ffi::GifResult> = items
                .into_iter()
                .map(|r| crate::ffi::GifResult {
                    id: r.id,
                    preview_url: r.preview_url,
                    preview_w: r.preview_w,
                    preview_h: r.preview_h,
                    image_url: r.image_url,
                    image_w: r.image_w,
                    image_h: r.image_h,
                    image_mime: r.image_mime,
                    strip_url: r.strip_url,
                    strip_mime: r.strip_mime,
                })
                .collect();
            g.on_gif_results(request_id, &ffi_items);
        }
        Err(e) => g.on_gif_search_failed(request_id, &e),
    }
}

#[cfg(not(test))]
impl ClientFfi {
    /// Non-blocking GIF search. Spawns the HTTP round-trip on the runtime and
    /// fires `on_gif_results(request_id, …)` (or `on_gif_search_failed`) on
    /// completion. The C++ controller debounces and drops stale `request_id`s.
    pub fn gif_search_async(
        &self,
        request_id: u64,
        query: &str,
        api_key: &str,
        client_key: &str,
        limit: u32,
    ) {
        // The `client_key` hook is no longer used for identity: Klipy's
        // customer_id is derived from the logged-in user's MXID, SHA-256 hashed
        // so the raw matrix identity never leaves the device — only a stable
        // pseudonymous token reaches Klipy.
        let _ = client_key;
        let handler = self.handler.clone();
        let customer_id = self
            .client
            .as_ref()
            .and_then(|c| c.user_id().map(|u| u.to_string()))
            .map(|mxid| hashed_customer_id(&mxid))
            .unwrap_or_default();
        let provider = KlipyProvider::new(api_key.to_owned(), customer_id);
        let url = provider.search_url(query, limit);
        let http = self.http_client.clone();
        self.rt.spawn(async move {
            let result = async {
                let resp = http.get(&url).send().await.map_err(|e| e.to_string())?;
                if !resp.status().is_success() {
                    return Err(format!("gif provider returned HTTP {}", resp.status()));
                }
                let body = resp.text().await.map_err(|e| e.to_string())?;
                provider.parse(&body)
            }
            .await;
            deliver_gif(&handler, request_id, result);
        });
    }

    /// Send a pre-fetched GIF MP4 into `room_id` as an `m.video` carrying the
    /// `fi.mau.gif` vendor hint. The MP4 (and optional poster thumbnail) are
    /// encrypted + uploaded when the room is encrypted, otherwise uploaded
    /// plaintext; the raw event is built by [`build_gif_video_content`].
    pub fn send_gif_video(
        &mut self,
        room_id: &str,
        mp4_bytes: &[u8],
        mime_type: &str,
        body: &str,
        width: u32,
        height: u32,
        duration_ms: u64,
        thumb_bytes: &[u8],
        thumb_mime: &str,
        thumb_width: u32,
        thumb_height: u32,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };
        let thumb_mime_parsed: Option<mime::Mime> = if thumb_bytes.is_empty() {
            None
        } else {
            thumb_mime.parse().ok()
        };

        let mp4 = mp4_bytes.to_vec();
        let thumb = thumb_bytes.to_vec();
        let body = body.to_owned();
        let mime_str = mime_type.to_owned();
        let reply = reply_event_id.to_owned();
        let thread = thread_root.to_owned();

        let result: Result<(), String> = self.rt.block_on(async move {
            let size = mp4.len();
            let (video_media, thumb_media) = if room.encryption_state().is_encrypted() {
                let mut cur = std::io::Cursor::new(mp4);
                let file = client
                    .upload_encrypted_file(&mut cur)
                    .await
                    .map_err(|e| e.to_string())?;
                let video = GifMedia::Encrypted(
                    serde_json::to_value(&file).map_err(|e| e.to_string())?,
                );
                let thumb_media = if thumb.is_empty() {
                    None
                } else {
                    let mut tc = std::io::Cursor::new(thumb);
                    let tf = client
                        .upload_encrypted_file(&mut tc)
                        .await
                        .map_err(|e| e.to_string())?;
                    Some(GifMedia::Encrypted(
                        serde_json::to_value(&tf).map_err(|e| e.to_string())?,
                    ))
                };
                (video, thumb_media)
            } else {
                let mxc = super::account::upload_bytes(&client, mp4, &mime)
                    .await
                    .map_err(|e| e.to_string())?
                    .to_string();
                let thumb_media = match thumb_mime_parsed {
                    Some(tm) if !thumb.is_empty() => {
                        let tmxc = super::account::upload_bytes(&client, thumb, &tm)
                            .await
                            .map_err(|e| e.to_string())?
                            .to_string();
                        Some(GifMedia::Plain(tmxc))
                    }
                    _ => None,
                };
                (GifMedia::Plain(mxc), thumb_media)
            };

            let content = build_gif_video_content(
                video_media,
                thumb_media,
                &body,
                &mime_str,
                width,
                height,
                size,
                duration_ms,
                thumb_width,
                thumb_height,
                &reply,
                &thread,
            );
            room.send_raw("m.room.message", content)
                .await
                .map_err(|e| e.to_string())?;
            Ok(())
        });

        match result {
            Ok(()) => ok(""),
            Err(e) => err(e),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn klipy() -> KlipyProvider {
        KlipyProvider::new("KEY123".into(), "cust42".into())
    }

    #[test]
    fn search_url_encodes_query_key_and_customer() {
        let url = klipy().search_url("funny cat", 24);
        assert!(
            url.starts_with("https://api.klipy.com/api/v1/KEY123/gifs/search?"),
            "url: {url}"
        );
        assert!(url.contains("q=funny%20cat"), "url: {url}");
        assert!(url.contains("per_page=24"));
        assert!(url.contains("rating=g"));
        assert!(url.contains("customer_id=cust42"));
    }

    #[test]
    fn search_url_clamps_per_page() {
        assert!(klipy().search_url("x", 4).contains("per_page=8")); // min 8
        assert!(klipy().search_url("x", 99).contains("per_page=50")); // max 50
    }

    // Trimmed Klipy response covering all four send-form cases: mp4-preferred,
    // webp-fallback, gif-fallback, and an entry with no animated form (skipped).
    const SAMPLE: &str = r#"{
        "result": true,
        "data": {
            "data": [
                {
                    "id": 8041071659142944,
                    "title": "Hello",
                    "file": {
                        "md": {
                            "webp": { "url": "https://cdn/md.webp", "width": 498, "height": 498, "size": 70000 },
                            "gif":  { "url": "https://cdn/md.gif",  "width": 498, "height": 498, "size": 119294 }
                        },
                        "sm": { "jpg": { "url": "https://cdn/sm.jpg", "width": 220, "height": 220, "size": 8560 } }
                    }
                },
                {
                    "id": 2,
                    "file": { "sm": { "gif": { "url": "https://cdn/y.gif", "width": 220, "height": 220 } } }
                },
                {
                    "id": 3,
                    "file": {
                        "md": { "mp4": { "url": "https://cdn/only.mp4", "width": 100, "height": 100, "size": 1234 } },
                        "sm": { "jpg": { "url": "https://cdn/only.jpg", "width": 100, "height": 100, "size": 99 } }
                    }
                },
                {
                    "id": 4,
                    "file": { "sm": { "jpg": { "url": "https://cdn/still.jpg", "width": 100, "height": 100 } } }
                }
            ],
            "current_page": 1, "per_page": 24, "has_next": true
        }
    }"#;

    #[test]
    fn parse_prefers_mp4_falls_back_to_webp_then_gif_and_skips_imageless() {
        let out = klipy().parse(SAMPLE).unwrap();
        // Entry 1 → webp (no mp4 present); entry 2 → gif; entry 3 → mp4; entry 4 skipped.
        assert_eq!(out.len(), 3);

        let a = &out[0];
        assert_eq!(a.id, "8041071659142944"); // numeric id stringified
        assert_eq!(a.image_url, "https://cdn/md.webp");
        assert_eq!(a.image_mime, "image/webp");
        assert_eq!((a.image_w, a.image_h), (498, 498));
        assert_eq!(a.preview_url, "https://cdn/sm.jpg");
        assert_eq!((a.preview_w, a.preview_h), (220, 220));
        // Strip prefers webp; entry 1 has webp.
        assert_eq!(a.strip_url, "https://cdn/md.webp");
        assert_eq!(a.strip_mime, "image/webp");

        let b = &out[1];
        assert_eq!(b.id, "2");
        assert_eq!(b.image_url, "https://cdn/y.gif");
        assert_eq!(b.image_mime, "image/gif");
        // No webp → strip falls back to gif.
        assert_eq!(b.strip_url, "https://cdn/y.gif");
        assert_eq!(b.strip_mime, "image/gif");

        let c = &out[2];
        assert_eq!(c.id, "3");
        assert_eq!(c.image_url, "https://cdn/only.mp4");
        assert_eq!(c.image_mime, "video/mp4");
        assert_eq!((c.image_w, c.image_h), (100, 100));
        assert_eq!(c.preview_url, "https://cdn/only.jpg");
        // MP4-only entry → strip falls back to the mp4 send form.
        assert_eq!(c.strip_url, "https://cdn/only.mp4");
        assert_eq!(c.strip_mime, "video/mp4");
    }

    // One entry from a real api.klipy.com/gifs/search response: every tier
    // (hd/md/sm/xs) carries gif/webp/jpg/mp4/webm forms. Guards against the
    // parser regressing on the live response shape (vs. the trimmed SAMPLE).
    const REAL_ENTRY: &str = r#"{
        "result": true,
        "data": { "data": [
            {
                "id": 2484942301552561,
                "title": "Goatplaybanjo's Chatty Cat",
                "file": {
                    "hd": {
                        "gif":  { "url": "https://static.klipy.com/hd.gif",  "width": 220, "height": 229, "size": 273268 },
                        "webp": { "url": "https://static.klipy.com/hd.webp", "width": 220, "height": 230, "size": 71688 },
                        "jpg":  { "url": "https://static.klipy.com/hd.jpg",  "width": 220, "height": 229, "size": 8064 },
                        "mp4":  { "url": "https://static.klipy.com/hd.mp4",  "width": 220, "height": 230, "size": 75638 },
                        "webm": { "url": "https://static.klipy.com/hd.webm", "width": 220, "height": 229, "size": 60623 }
                    },
                    "md": {
                        "gif":  { "url": "https://static.klipy.com/md.gif",  "width": 220, "height": 229, "size": 271080 },
                        "webp": { "url": "https://static.klipy.com/md.webp", "width": 220, "height": 229, "size": 137582 },
                        "jpg":  { "url": "https://static.klipy.com/md.jpg",  "width": 220, "height": 229, "size": 8291 },
                        "mp4":  { "url": "https://static.klipy.com/md.mp4",  "width": 220, "height": 230, "size": 75638 },
                        "webm": { "url": "https://static.klipy.com/md.webm", "width": 220, "height": 229, "size": 60623 }
                    },
                    "sm": {
                        "gif":  { "url": "https://static.klipy.com/sm.gif",  "width": 220, "height": 229, "size": 271080 },
                        "jpg":  { "url": "https://static.klipy.com/sm.jpg",  "width": 220, "height": 229, "size": 8291 },
                        "mp4":  { "url": "https://static.klipy.com/sm.mp4",  "width": 220, "height": 230, "size": 75638 }
                    },
                    "xs": {
                        "jpg":  { "url": "https://static.klipy.com/xs.jpg",  "width": 87, "height": 90, "size": 2406 },
                        "mp4":  { "url": "https://static.klipy.com/xs.mp4",  "width": 146, "height": 150, "size": 31334 }
                    }
                }
            }
        ], "current_page": 1, "per_page": 4, "has_next": true }
    }"#;

    #[test]
    fn parse_extracts_md_mp4_from_full_live_entry() {
        let out = klipy().parse(REAL_ENTRY).unwrap();
        assert_eq!(out.len(), 1);
        let g = &out[0];
        // Send form = medium mp4 (highest priority); strip thumbnail = small
        // static jpg.
        assert_eq!(g.image_url, "https://static.klipy.com/md.mp4");
        assert_eq!(g.image_mime, "video/mp4");
        assert_eq!((g.image_w, g.image_h), (220, 230));
        assert_eq!(g.preview_url, "https://static.klipy.com/sm.jpg");
        assert_eq!(g.id, "2484942301552561");
        // Strip display = medium webp (prefers webp over the mp4 send form).
        assert_eq!(g.strip_url, "https://static.klipy.com/md.webp");
        assert_eq!(g.strip_mime, "image/webp");
    }

    #[test]
    fn parse_empty_results_is_ok() {
        assert_eq!(
            klipy()
                .parse(r#"{"result":true,"data":{"data":[]}}"#)
                .unwrap(),
            vec![]
        );
    }

    #[test]
    fn parse_non_json_is_err() {
        assert!(klipy().parse("not json").is_err());
    }

    #[test]
    fn parse_missing_data_is_err() {
        assert!(klipy().parse(r#"{"result":true}"#).is_err());
    }

    #[test]
    fn hashed_customer_id_is_stable_and_hides_the_mxid() {
        let h = hashed_customer_id("@alice:example.org");
        assert_eq!(h, hashed_customer_id("@alice:example.org")); // stable
        assert_ne!(h, hashed_customer_id("@bob:example.org")); // per-user
        assert!(!h.contains("alice")); // raw id not present
        assert_eq!(h.len(), 32); // 16 bytes, hex
    }

    #[test]
    fn plain_content_uses_url_and_carries_fi_mau_gif() {
        let c = build_gif_video_content(
            GifMedia::Plain("mxc://srv/vid".into()),
            Some(GifMedia::Plain("mxc://srv/poster".into())),
            "cat.mp4",
            "video/mp4",
            498,
            280,
            51234,
            2500,
            220,
            124,
            "",
            "",
        );
        assert_eq!(c["msgtype"], "m.video");
        assert_eq!(c["url"], "mxc://srv/vid");
        assert!(c.get("file").is_none());
        assert_eq!(c["info"]["fi.mau.gif"], true);
        assert_eq!(c["info"]["mimetype"], "video/mp4");
        assert_eq!(c["info"]["w"], 498);
        assert_eq!(c["info"]["h"], 280);
        assert_eq!(c["info"]["duration"], 2500);
        assert_eq!(c["info"]["thumbnail_url"], "mxc://srv/poster");
        assert_eq!(c["info"]["thumbnail_info"]["w"], 220);
    }

    #[test]
    fn encrypted_content_uses_file_block_and_thumbnail_file() {
        let enc = serde_json::json!({ "url": "mxc://srv/enc", "key": { "k": "…" } });
        let thumb_enc = serde_json::json!({ "url": "mxc://srv/encthumb" });
        let c = build_gif_video_content(
            GifMedia::Encrypted(enc.clone()),
            Some(GifMedia::Encrypted(thumb_enc.clone())),
            "cat.mp4",
            "video/mp4",
            498,
            280,
            51234,
            0,
            0,
            0,
            "",
            "",
        );
        assert_eq!(c["file"], enc);
        assert!(c.get("url").is_none());
        assert_eq!(c["info"]["fi.mau.gif"], true);
        assert_eq!(c["info"]["thumbnail_file"], thumb_enc);
        assert!(c.get("info").unwrap().get("thumbnail_url").is_none());
        // duration 0 is omitted.
        assert!(c["info"].get("duration").is_none());
    }

    #[test]
    fn thread_root_emits_msc3440_relation() {
        let c = build_gif_video_content(
            GifMedia::Plain("mxc://srv/vid".into()),
            None,
            "cat.mp4",
            "video/mp4",
            1,
            1,
            1,
            0,
            0,
            0,
            "",
            "$root",
        );
        assert_eq!(c["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(c["m.relates_to"]["event_id"], "$root");
        assert_eq!(c["m.relates_to"]["is_falling_back"], true);
    }
}
