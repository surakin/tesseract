//! Media fetch helpers and FFI methods (avatar, mxc URI, encrypted source,
//! generic URL, URL preview) plus the notification-image preview helper used
//! by the sync notification handlers.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::ClientFfi;

#[cfg(not(test))]
use super::{stop_fut, SendHandler};

#[cfg(not(test))]
use matrix_sdk::{
    ruma::OwnedRoomId,
    Client, Room,
};

#[cfg(not(test))]
use std::sync::{Arc, Mutex};

/// Hard upper bound on a single media download (64 MiB). A malicious or
/// buggy homeserver can serve an arbitrarily large payload for any `mxc://`
/// URI; matrix-sdk buffers the whole body into memory, so without a cap a
/// single fetch can OOM the process. Oversized content is dropped (returns
/// empty) rather than propagated into image decoders.
#[cfg(not(test))]
pub(super) const MAX_MEDIA_BYTES: usize = 64 * 1024 * 1024;

/// Upper bound on a single arbitrary-URL fetch (1 MiB). Used by
/// `fetch_url_bytes` and `get_url_preview` to fetch OSM tiles and
/// OpenGraph metadata from untrusted third-party servers. Bigger than this
/// is almost certainly a misbehaving server (or worse, a deliberate DoS),
/// so reject it early. Far tighter than `MAX_MEDIA_BYTES` because the
/// content here is always small: a 256×256 PNG tile is ~30 KB; OG JSON
/// metadata is a few KB at most.
#[cfg(not(test))]
pub(super) const MAX_URL_BYTES: usize = 1024 * 1024;

/// Per-request timeout for media fetches. Every fetch runs as
/// `block_on(select!{ op / stop_fut })`, and `stop_fut` only resolves at app
/// shutdown — so without these bounds a dead or stalled server response hangs
/// the call forever, holding an `InFlightGuard` (the activity dot sticks) and
/// pinning one of only four read-pool worker threads. Thumbnails/avatars/URL
/// fetches are small and should arrive quickly; full-file downloads get a more
/// generous bound. On timeout the fetch returns its empty default and the next
/// trigger (room update, reopen) re-fetches, since nothing was cached.
#[cfg(not(test))]
pub(super) const THUMBNAIL_FETCH_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(30);
#[cfg(not(test))]
pub(super) const FULL_MEDIA_FETCH_TIMEOUT: std::time::Duration =
    std::time::Duration::from_secs(120);

#[cfg(not(test))]
pub(super) fn cap_media_bytes(bytes: Vec<u8>) -> Vec<u8> {
    if bytes.len() > MAX_MEDIA_BYTES {
        tracing::warn!(
            "media download {} bytes exceeds {} byte cap; discarding",
            bytes.len(),
            MAX_MEDIA_BYTES,
        );
        return Vec::new();
    }
    bytes
}

/// Upper bound on a notification preview image. Notification transports are
/// tight — D-Bus image-data, WinRT toast (~3 MB) and UNNotificationAttachment
/// all dislike large payloads — so anything bigger is dropped (the
/// notification still shows, just without the picture).
#[cfg(not(test))]
pub(super) const NOTIF_IMAGE_CAP: usize = 2 * 1024 * 1024;

/// Best-effort fetch of a message/sticker image for a notification preview.
/// matrix-sdk decrypts encrypted `MediaSource`s transparently. Any failure
/// or an over-cap payload yields an empty Vec — the notification then falls
/// back to text + room avatar.
#[cfg(not(test))]
pub(super) async fn fetch_notification_image(
    client: &Client,
    source: matrix_sdk::ruma::events::room::MediaSource,
) -> Vec<u8> {
    use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
    let request = MediaRequestParameters {
        source,
        format: MediaFormat::File,
    };
    match client.media().get_media_content(&request, true).await {
        Ok(b) if b.len() <= NOTIF_IMAGE_CAP => b,
        _ => Vec::new(),
    }
}

/// Shared logic for message and sticker notification handlers. Evaluates push
/// rules, resolves display names and avatars, fetches the preview image, and
/// calls `on_notification`. Callers extract the type-specific `body`,
/// `msg_type_str`, and `preview_source` then delegate here.
#[cfg(not(test))]
pub(super) async fn emit_notification(
    client: &Client,
    room: Room,
    sender_id: &matrix_sdk::ruma::UserId,
    body: &str,
    msg_type_str: &str,
    event_id: &str,
    ts: u64,
    preview_source: Option<matrix_sdk::ruma::events::room::MediaSource>,
    handler: &Arc<Mutex<SendHandler>>,
) {
    use matrix_sdk::ruma::events::room::MediaSource;
    use super::notifications;
    if client.user_id() == Some(sender_id) {
        return;
    }
    let room_id = room.room_id().as_str().to_owned();
    let synthetic = notifications::build_push_rule_json(
        &room_id, event_id, sender_id.as_str(), body, msg_type_str, ts,
    );
    let (should_notify, is_mention) =
        notifications::evaluate_push_rules(client, &room, &synthetic).await;
    if !should_notify {
        return;
    }
    // Suppress if the room is already read. This catches backfill-subscription
    // events (old events delivered when the sync backfills inactive rooms) and
    // any other case where the event arrived but the user's read receipt is
    // already at or past it. The server-side unread count is authoritative.
    if room.num_unread_notifications() == 0 && room.num_unread_mentions() == 0 {
        return;
    }
    let room_name = room
        .display_name()
        .await
        .map(|n| n.to_string())
        .unwrap_or_else(|_| room_id.clone());
    let sender_member = room.get_member_no_sync(sender_id).await.ok().flatten();
    let sender_name = sender_member
        .as_ref()
        .and_then(|m| m.display_name().map(str::to_owned))
        .unwrap_or_else(|| sender_id.localpart().to_string());
    let room_avatar = room
        .avatar(matrix_sdk::media::MediaFormat::File)
        .await
        .ok()
        .flatten()
        .unwrap_or_default();
    let avatar = if !room_avatar.is_empty() {
        room_avatar
    } else if let Some(url) = sender_member.as_ref().and_then(|m| m.avatar_url()) {
        use matrix_sdk::media::MediaRequestParameters;
        let req = MediaRequestParameters {
            source: MediaSource::Plain(url.to_owned()),
            format: matrix_sdk::media::MediaFormat::File,
        };
        client
            .media()
            .get_media_content(&req, true)
            .await
            .unwrap_or_default()
    } else {
        Vec::new()
    };
    let preview = match preview_source {
        Some(src) => fetch_notification_image(client, src).await,
        None => Vec::new(),
    };
    if let Ok(g) = handler.lock() {
        g.on_notification(&room_id, &room_name, &sender_name, body, is_mention, &avatar, &preview);
    }
}

// ---------------------------------------------------------------------------
// FFI media fetch methods
// ---------------------------------------------------------------------------

#[cfg(not(test))]
impl ClientFfi {
    /// Authoritative count of extra in-flight HTTP operations (media + back-
    /// pagination), excluding the sync long-poll. The UI re-reads this on every
    /// `on_inflight_changed` notification so the activity dot reflects the live
    /// atomic rather than a possibly-reordered or dropped snapshot value.
    pub fn in_flight_count(&self) -> u32 {
        self.in_flight.load(std::sync::atomic::Ordering::Relaxed)
    }

    pub fn fetch_avatar_bytes(&self, room_id: &str) -> Vec<u8> {
        use matrix_sdk::media::MediaFormat;
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return Vec::new(),
        };
        let Some(room) = client.get_room(&room_id) else {
            return Vec::new();
        };
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = room.avatar(MediaFormat::File) =>
                    result.ok().flatten().unwrap_or_default(),
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    pub fn fetch_media_bytes(&self, mxc_url: &str) -> Vec<u8> {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        let uri = OwnedMxcUri::from(mxc_url);
        if !uri.is_valid() {
            return Vec::new();
        }
        let request = MediaRequestParameters {
            source: MediaSource::Plain(uri.into()),
            format: MediaFormat::File,
        };
        let stop_rx = self.stop_rx.clone();
        let in_flight = std::sync::Arc::clone(&self.in_flight);
        let handler = self.handler.clone();
        self.rt.block_on(async move {
            let _guard = super::InFlightGuard::new(&in_flight, &handler);
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = tokio::time::sleep(FULL_MEDIA_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Download media from either a plain `mxc://` URI or a JSON-serialised
    /// `MediaSource` carrying an `EncryptedFile`. The two shapes are detected
    /// by the leading `mxc:` prefix: plain URIs go through `MediaSource::Plain`
    /// and JSON payloads are deserialised as a full `MediaSource` so the SDK
    /// can decrypt encrypted attachments (MSC2545 stickers, encrypted images,
    /// etc.). Returns an empty Vec on any failure.
    pub fn fetch_source_bytes(&self, source: &str) -> Vec<u8> {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        if source.is_empty() {
            return Vec::new();
        }

        let media_source = if source.starts_with("mxc://") {
            let uri = OwnedMxcUri::from(source);
            if !uri.is_valid() {
                return Vec::new();
            }
            MediaSource::Plain(uri.into())
        } else {
            match serde_json::from_str::<MediaSource>(source) {
                Ok(s) => s,
                Err(_) => return Vec::new(),
            }
        };

        let request = MediaRequestParameters {
            source: media_source,
            format: MediaFormat::File,
        };
        let stop_rx = self.stop_rx.clone();
        let in_flight = std::sync::Arc::clone(&self.in_flight);
        let handler = self.handler.clone();
        self.rt.block_on(async move {
            let _guard = super::InFlightGuard::new(&in_flight, &handler);
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = tokio::time::sleep(FULL_MEDIA_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Like `fetch_avatar_bytes` but requests a server-scaled thumbnail
    /// (`size`×`size`) instead of the original. Avatars render at ≤80 px, so
    /// the full upload is wasteful. Static (non-animated) thumbnail.
    pub fn fetch_avatar_thumbnail_bytes(&self, room_id: &str, size: u32) -> Vec<u8> {
        use matrix_sdk::media::{MediaFormat, MediaThumbnailSettings};
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return Vec::new(),
        };
        let Some(room) = client.get_room(&room_id) else {
            return Vec::new();
        };
        let settings = MediaThumbnailSettings::new(size.into(), size.into());
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = room.avatar(MediaFormat::Thumbnail(settings)) =>
                    result.ok().flatten().unwrap_or_default(),
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Like `fetch_media_bytes` but requests a server-scaled thumbnail
    /// (`w`×`h`). `animated` requests an animated thumbnail (MSC2705) for
    /// sources that may move; servers without support return a static frame.
    pub fn fetch_media_thumbnail_bytes(
        &self,
        mxc_url: &str,
        w: u32,
        h: u32,
        animated: bool,
    ) -> Vec<u8> {
        use matrix_sdk::media::{
            MediaFormat, MediaRequestParameters, MediaThumbnailSettings,
        };
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        let uri = OwnedMxcUri::from(mxc_url);
        if !uri.is_valid() {
            return Vec::new();
        }
        let mut settings = MediaThumbnailSettings::new(w.into(), h.into());
        settings.animated = animated;
        let request = MediaRequestParameters {
            source: MediaSource::Plain(uri.into()),
            format: MediaFormat::Thumbnail(settings),
        };
        let stop_rx = self.stop_rx.clone();
        let in_flight = std::sync::Arc::clone(&self.in_flight);
        let handler = self.handler.clone();
        self.rt.block_on(async move {
            let _guard = super::InFlightGuard::new(&in_flight, &handler);
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Like `fetch_source_bytes` but requests a server-scaled thumbnail
    /// (`w`×`h`) for plain `mxc://` sources. Encrypted sources (serialized
    /// JSON) cannot be thumbnailed server-side, so they fall back to the full
    /// `File` — their embedded thumbnail mxc is already small.
    pub fn fetch_source_thumbnail_bytes(
        &self,
        source: &str,
        w: u32,
        h: u32,
        animated: bool,
    ) -> Vec<u8> {
        use matrix_sdk::media::{
            MediaFormat, MediaRequestParameters, MediaThumbnailSettings,
        };
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        if source.is_empty() {
            return Vec::new();
        }

        let (media_source, format) = if source.starts_with("mxc://") {
            let uri = OwnedMxcUri::from(source);
            if !uri.is_valid() {
                return Vec::new();
            }
            let mut settings = MediaThumbnailSettings::new(w.into(), h.into());
            settings.animated = animated;
            (
                MediaSource::Plain(uri.into()),
                MediaFormat::Thumbnail(settings),
            )
        } else {
            match serde_json::from_str::<MediaSource>(source) {
                Ok(s) => (s, MediaFormat::File),
                Err(_) => return Vec::new(),
            }
        };

        let request = MediaRequestParameters {
            source: media_source,
            format,
        };
        let stop_rx = self.stop_rx.clone();
        let in_flight = std::sync::Arc::clone(&self.in_flight);
        let handler = self.handler.clone();
        self.rt.block_on(async move {
            let _guard = super::InFlightGuard::new(&in_flight, &handler);
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    pub fn fetch_url_bytes(&self, url: &str) -> Vec<u8> {
        if url.is_empty() {
            return Vec::new();
        }
        let url = url.to_owned();
        let stop_rx = self.stop_rx.clone();
        let client = self.http_client.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = async {
                    use futures_util::StreamExt;
                    let resp = match client.get(&url).send().await {
                        Ok(r) => r,
                        Err(_) => return Vec::new(),
                    };
                    let resp = match resp.error_for_status() {
                        Ok(r) => r,
                        Err(_) => return Vec::new(),
                    };
                    // Reject up-front when the server advertises a body
                    // larger than the cap, before we allocate anything.
                    if let Some(len) = resp.content_length() {
                        if len as usize > MAX_URL_BYTES {
                            tracing::warn!(
                                "fetch_url_bytes: {url} declared {len} bytes \
                                 > {MAX_URL_BYTES} cap; rejecting"
                            );
                            return Vec::new();
                        }
                    }
                    // Stream chunks and bail mid-flight if the body lies
                    // about its length (or omits Content-Length entirely).
                    let mut stream = resp.bytes_stream();
                    let mut buf: Vec<u8> = Vec::new();
                    while let Some(chunk) = stream.next().await {
                        let chunk = match chunk {
                            Ok(c) => c,
                            Err(_) => return Vec::new(),
                        };
                        if buf.len().saturating_add(chunk.len()) > MAX_URL_BYTES {
                            tracing::warn!(
                                "fetch_url_bytes: {url} exceeded {MAX_URL_BYTES} \
                                 byte cap mid-stream; aborting"
                            );
                            return Vec::new();
                        }
                        buf.extend_from_slice(&chunk);
                    }
                    buf
                } => result,
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    // -----------------------------------------------------------------------
    // URL preview (homeserver og:* metadata fetch)
    // -----------------------------------------------------------------------

    #[allow(deprecated)]
    pub fn get_url_preview(&self, url: &str) -> String {
        use ruma::api::client::media::get_media_preview::v3::Request;

        let Some(client) = self.client.clone() else {
            return String::new();
        };
        if url.is_empty() {
            return String::new();
        }

        let url_str = url.to_owned();
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            let req = Request::new(url_str);
            tokio::select! {
                result = async { client.send(req).await } => {
                    match result {
                        Ok(resp) => {
                            let json = resp.data
                                .map(|v| v.get().to_owned())
                                .unwrap_or_default();
                            // OG metadata is always a few KB; anything
                            // bigger is misconfigured / hostile and would
                            // bloat the JSON parser downstream.
                            if json.len() > MAX_URL_BYTES {
                                tracing::warn!(
                                    "get_url_preview: response {} bytes \
                                     > {MAX_URL_BYTES} cap; discarding",
                                    json.len()
                                );
                                return String::new();
                            }
                            json
                        }
                        Err(_) => String::new(),
                    }
                }
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => String::new(),
                _ = stop_fut(stop_rx) => String::new(),
            }
        })
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn fetch_avatar_bytes(&self, _room_id: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_media_bytes(&self, _mxc_url: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_source_bytes(&self, _source: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_url_bytes(&self, _url: &str) -> Vec<u8> { Vec::new() }
    pub fn get_url_preview(&self, _url: &str) -> String { String::new() }
}
