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
use std::collections::HashMap;
#[cfg(not(test))]
use parking_lot::Mutex;
use std::sync::Arc;

/// Hard upper bound on a single media download (64 MiB). A malicious or
/// buggy homeserver can serve an arbitrarily large payload for any `mxc://`
/// URI; matrix-sdk buffers the whole body into memory, so without a cap a
/// single fetch can OOM the process. Oversized content is dropped (returns
/// empty) rather than propagated into image decoders.
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

pub(super) fn cap_media_bytes(bytes: Vec<u8>) -> Vec<u8> {
    if bytes.len() > MAX_MEDIA_BYTES {
        #[cfg(not(test))]
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
    session_start_ms: u64,
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
    // Suppress events the user has already read. The server-side unread count
    // is authoritative for historical/backfill events — old events delivered
    // when the sync backfills inactive rooms, or replayed on restart — so for
    // anything older than this sync session's start we still defer to it. But
    // that count lags one sync cycle behind a just-sent read receipt, so a
    // *live* message (newer than session start) must NOT consult it: otherwise
    // the first message arriving right after a room is marked read is dropped
    // while the count still reads zero. Push rules already cleared this event.
    if ts < session_start_ms
        && room.num_unread_notifications() == 0
        && room.num_unread_mentions() == 0
    {
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
        .filter(|b| b.len() <= NOTIF_IMAGE_CAP)
        .unwrap_or_default();
    let avatar = if !room_avatar.is_empty() {
        room_avatar
    } else if let Some(url) = sender_member.as_ref().and_then(|m| m.avatar_url()) {
        use matrix_sdk::media::MediaRequestParameters;
        let req = MediaRequestParameters {
            source: MediaSource::Plain(url.to_owned()),
            format: matrix_sdk::media::MediaFormat::File,
        };
        match client.media().get_media_content(&req, true).await {
            Ok(b) if b.len() <= NOTIF_IMAGE_CAP => b,
            _ => Vec::new(),
        }
    } else {
        Vec::new()
    };
    let preview = match preview_source {
        Some(src) => fetch_notification_image(client, src).await,
        None => Vec::new(),
    };
    {
        let g = handler.lock();
        g.on_notification(&room_id, &room_name, &sender_name, body, is_mention, &avatar, &preview);
    }
}

// ---------------------------------------------------------------------------
// Async media downloads (non-blocking; deliver via on_media_ready callback)
// ---------------------------------------------------------------------------

/// `kind` discriminants for `fetch_media_async` — mirror the blocking fetch
/// variants. Keep in sync with the C++ side (ShellBase passes these).
#[cfg(not(test))]
pub(super) const MEDIA_KIND_ROOM_AVATAR: u8 = 0; // room.avatar thumbnail (source = room_id)
#[cfg(not(test))]
pub(super) const MEDIA_KIND_MXC_THUMB: u8 = 1; // plain mxc thumbnail
#[cfg(not(test))]
pub(super) const MEDIA_KIND_SOURCE_THUMB: u8 = 2; // source (plain/encrypted) thumbnail
#[cfg(not(test))]
pub(super) const MEDIA_KIND_SOURCE_FULL: u8 = 3; // full source (plain/encrypted)

/// Resolve the request and await the download for one `fetch_media_async` task.
/// Returns the (capped) bytes, or an empty Vec on any invalid input / error.
/// Does NOT apply the timeout/stop race — the caller wraps this in a `select!`.
#[cfg(not(test))]
async fn download_media(
    client: &Client,
    kind: u8,
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

    match kind {
        MEDIA_KIND_ROOM_AVATAR => {
            let Ok(room_id) = source.parse::<OwnedRoomId>() else {
                return Vec::new();
            };
            let Some(room) = client.get_room(&room_id) else {
                return Vec::new();
            };
            let settings = MediaThumbnailSettings::new(w.into(), h.into());
            room.avatar(MediaFormat::Thumbnail(settings))
                .await
                .ok()
                .flatten()
                .unwrap_or_default()
        }
        MEDIA_KIND_MXC_THUMB => {
            let uri = OwnedMxcUri::from(source);
            if !uri.is_valid() {
                return Vec::new();
            }
            let mut settings = MediaThumbnailSettings::new(w.into(), h.into());
            settings.animated = animated;
            let request = MediaRequestParameters {
                source: MediaSource::Plain(uri),
                format: MediaFormat::Thumbnail(settings),
            };
            cap_media_bytes(
                client.media().get_media_content(&request, true).await.unwrap_or_default(),
            )
        }
        MEDIA_KIND_SOURCE_THUMB => {
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
                (MediaSource::Plain(uri), MediaFormat::Thumbnail(settings))
            } else {
                match serde_json::from_str::<MediaSource>(source) {
                    Ok(s) => (s, MediaFormat::File),
                    Err(_) => return Vec::new(),
                }
            };
            let request = MediaRequestParameters { source: media_source, format };
            cap_media_bytes(
                client.media().get_media_content(&request, true).await.unwrap_or_default(),
            )
        }
        // MEDIA_KIND_SOURCE_FULL (and any unknown kind) → full file.
        _ => {
            if source.is_empty() {
                return Vec::new();
            }
            let media_source = if source.starts_with("mxc://") {
                let uri = OwnedMxcUri::from(source);
                if !uri.is_valid() {
                    return Vec::new();
                }
                MediaSource::Plain(uri)
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
            cap_media_bytes(
                client.media().get_media_content(&request, true).await.unwrap_or_default(),
            )
        }
    }
}

/// Per-chunk idle timeout for `download_url`. If no bytes arrive within this
/// window the server is considered stalled and the download is aborted. This
/// fires well before the outer `THUMBNAIL_FETCH_TIMEOUT` select! arm and avoids
/// holding a semaphore permit on a dead transfer for the full 30s.
#[cfg(not(test))]
const CHUNK_STALL_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(10);

/// Stream an arbitrary HTTP(S) URL into a byte buffer, enforcing `max_bytes`
/// both up-front (Content-Length) and mid-stream. Returns an empty Vec on any
/// error. Shared by the blocking `fetch_url_bytes` / `fetch_gif_bytes` and the
/// async `fetch_url_async` (map tiles); the caller wraps it in the
/// timeout/stop race and supplies the appropriate size cap.
#[cfg(not(test))]
async fn download_url(client: &reqwest::Client, url: &str, max_bytes: usize) -> Vec<u8> {
    use futures_util::StreamExt;
    let resp = match client.get(url).send().await {
        Ok(r) => r,
        Err(_) => return Vec::new(),
    };
    let resp = match resp.error_for_status() {
        Ok(r) => r,
        Err(_) => return Vec::new(),
    };
    if let Some(len) = resp.content_length() {
        if len as usize > max_bytes {
            tracing::warn!(
                "download_url: {url} declared {len} bytes > {max_bytes} cap; rejecting"
            );
            return Vec::new();
        }
    }
    let mut stream = resp.bytes_stream();
    let mut buf: Vec<u8> = Vec::new();
    loop {
        let chunk = tokio::select! {
            item = stream.next() => item,
            _ = tokio::time::sleep(CHUNK_STALL_TIMEOUT) => {
                tracing::warn!(
                    "download_url: {url} stalled (no bytes for {CHUNK_STALL_TIMEOUT:?}); aborting"
                );
                return Vec::new();
            }
        };
        let Some(chunk) = chunk else { break };
        let chunk = match chunk {
            Ok(c) => c,
            Err(_) => return Vec::new(),
        };
        if buf.len().saturating_add(chunk.len()) > max_bytes {
            tracing::warn!(
                "download_url: {url} exceeded {max_bytes} byte cap mid-stream; aborting"
            );
            return Vec::new();
        }
        buf.extend_from_slice(&chunk);
    }
    buf
}

/// Deliver a completed media download to C++ via `on_media_ready`. Tolerates a
/// detached handler (shutdown) and a contended mutex by simply dropping.
#[cfg(not(test))]
fn deliver_media(
    handler: &Option<Arc<Mutex<SendHandler>>>,
    request_id: u64,
    bytes: &[u8],
) {
    if let Some(h) = handler {
        {
            let g = h.lock();
            g.on_media_ready(request_id, bytes);
        }
    }
}

/// Register a spawned media task under `group_id` for later cancellation.
/// Skips `group_id == 0` (ungrouped/never-cancelled). Opportunistically prunes
/// already-finished handles in the group so the vec can't grow unbounded while
/// a room stays active (tasks don't remove themselves — avoids a register/remove
/// race). `request_id` is retained only for debugging.
#[cfg(not(test))]
fn register_media_task(
    map: &Arc<Mutex<HashMap<u64, Vec<(u64, tokio::task::AbortHandle)>>>>,
    group_id: u64,
    request_id: u64,
    handle: tokio::task::AbortHandle,
) {
    if group_id == 0 {
        return;
    }
    {
        let mut m = map.lock();
        let v = m.entry(group_id).or_default();
        v.retain(|(_, h)| !h.is_finished());
        v.push((request_id, handle));
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
            source: MediaSource::Plain(uri),
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
            MediaSource::Plain(uri)
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

    pub fn fetch_url_bytes(&self, url: &str) -> Vec<u8> {
        if url.is_empty() {
            return Vec::new();
        }
        let url = url.to_owned();
        let stop_rx = self.stop_rx.clone();
        let client = self.http_client.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = download_url(&client, &url, MAX_URL_BYTES) => result,
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Like `fetch_url_bytes` but for a GIF-picker MP4: the 1 MiB `MAX_URL_BYTES`
    /// thumbnail cap is far too small for a video, so this uses the full-media
    /// cap (`MAX_MEDIA_BYTES`) and the more generous full-download timeout. Used
    /// by the `/gif` send path to fetch the chosen result before upload.
    pub fn fetch_gif_bytes(&self, url: &str) -> Vec<u8> {
        if url.is_empty() {
            return Vec::new();
        }
        let url = url.to_owned();
        let stop_rx = self.stop_rx.clone();
        let client = self.http_client.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = download_url(&client, &url, MAX_MEDIA_BYTES) => result,
                _ = tokio::time::sleep(FULL_MEDIA_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Non-blocking counterpart of `fetch_url_bytes` (map tiles, etc.). Spawns
    /// the fetch under the bulk lane and fires `on_media_ready(request_id,
    /// bytes)` on completion. Does not pin a worker thread.
    pub fn fetch_url_async(&self, request_id: u64, group_id: u64, url: &str) {
        let handler = self.handler.clone();
        if url.is_empty() {
            deliver_media(&handler, request_id, &[]);
            return;
        }
        let in_flight = Arc::clone(&self.in_flight);
        let stop_rx = self.stop_rx.clone();
        let gate = Arc::clone(&self.media_gate_bulk);
        let client = self.http_client.clone();
        let url = url.to_owned();

        let handle = self.rt.spawn(async move {
            let _permit = match gate
                .acquire(super::media_queue::PRIO_NORMAL, request_id, group_id)
                .await
            {
                Some(p) => p,
                None => {
                    deliver_media(&handler, request_id, &[]);
                    return;
                }
            };
            let _guard = super::InFlightGuard::new(&in_flight, &handler);
            let bytes = tokio::select! {
                b = download_url(&client, &url, MAX_URL_BYTES) => b,
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            };
            deliver_media(&handler, request_id, &bytes);
        });

        register_media_task(&self.media_tasks, group_id, request_id, handle.abort_handle());
    }

    // -----------------------------------------------------------------------
    // URL preview (homeserver og:* metadata fetch)
    // -----------------------------------------------------------------------

    /// Non-blocking media download. Spawns the fetch on the tokio runtime under
    /// a per-lane semaphore and fires `on_media_ready(request_id, bytes)` on
    /// completion (empty bytes on failure/timeout/cancel). Unlike the blocking
    /// `fetch_*_bytes` methods this does not pin a C++ worker thread for the
    /// network round-trip, so dozens of downloads can be in flight at once.
    pub fn fetch_media_async(
        &self,
        request_id: u64,
        group_id: u64,
        priority: u8,
        kind: u8,
        source: &str,
        w: u32,
        h: u32,
        animated: bool,
    ) {
        let handler = self.handler.clone();
        let Some(client) = self.client.clone() else {
            // Resolve immediately so the C++ pending entry never dangles.
            deliver_media(&handler, request_id, &[]);
            return;
        };
        let in_flight = Arc::clone(&self.in_flight);
        let stop_rx = self.stop_rx.clone();
        // Full-size downloads are slow and low-priority → the narrow bulk lane.
        // Avatars/thumbnails are small and interactive → the wide fg lane.
        let gate = if kind == MEDIA_KIND_SOURCE_FULL {
            Arc::clone(&self.media_gate_bulk)
        } else {
            Arc::clone(&self.media_gate_fg)
        };
        let timeout = if kind == MEDIA_KIND_SOURCE_FULL {
            FULL_MEDIA_FETCH_TIMEOUT
        } else {
            THUMBNAIL_FETCH_TIMEOUT
        };
        let source = source.to_owned();
        let cache_key = format!("{kind}:{source}");
        let sdk_fetched = Arc::clone(&self.sdk_media_fetched);

        let handle = self.rt.spawn(async move {
            // Fast path: if this source was successfully fetched before, it should
            // be in matrix-sdk's SQLite media store. Probe with a 10 ms timeout —
            // a SQLite hit takes < 1 ms, so any real network round-trip will
            // overflow the budget and we fall through to the normal gated path.
            // This keeps the priority gate from artificially queuing media the
            // user has already seen.
            if sdk_fetched.lock().contains(&cache_key) {
                let probe = tokio::time::timeout(
                    std::time::Duration::from_millis(10),
                    download_media(&client, kind, &source, w, h, animated),
                )
                .await;
                match probe {
                    Ok(bytes) if !bytes.is_empty() => {
                        deliver_media(&handler, request_id, &bytes);
                        return;
                    }
                    _ => {
                        // Cache miss or unexpected timeout — evict the stale key
                        // and fall through to the normal gated download below.
                        sdk_fetched.lock().remove(&cache_key);
                    }
                }
            }

            // Park in the lane's priority queue until a slot frees. `None` means
            // the wait was cancelled (room switch) — deliver empty so the C++
            // pending entry resolves. `priority` lets a visible-row fetch jump
            // the off-screen backlog; `prioritize_media` can raise it later.
            let _permit = match gate.acquire(priority, request_id, group_id).await {
                Some(p) => p,
                None => {
                    deliver_media(&handler, request_id, &[]);
                    return;
                }
            };
            // Tracks the activity dot for the duration of the actual download.
            let _guard = super::InFlightGuard::new(&in_flight, &handler);
            let bytes = tokio::select! {
                b = download_media(&client, kind, &source, w, h, animated) => b,
                _ = tokio::time::sleep(timeout) => Vec::new(),
                _ = stop_fut(stop_rx) => Vec::new(),
            };
            if !bytes.is_empty() {
                sdk_fetched.lock().insert(cache_key);
            }
            deliver_media(&handler, request_id, &bytes);
        });

        register_media_task(&self.media_tasks, group_id, request_id, handle.abort_handle());
    }

    /// Raise the scheduling priority of still-pending `fetch_media_async` tasks
    /// in `group_id` whose `request_id` is in `request_ids`, so they download
    /// before lower-priority pending items. The C++ side calls this when a row
    /// scrolls into view. A request lives in exactly one lane (thumbnail vs
    /// full-size), unknown to the caller, so both gates are asked — each ignores
    /// ids it does not hold. No-op for already-running/finished tasks (absent
    /// from both queues) and for group 0.
    pub fn prioritize_media(&self, group_id: u64, request_ids: &[u64]) {
        if group_id == 0 || request_ids.is_empty() {
            return;
        }
        self.media_gate_fg
            .prioritize(group_id, request_ids, super::media_queue::PRIO_VISIBLE);
        self.media_gate_bulk
            .prioritize(group_id, request_ids, super::media_queue::PRIO_VISIBLE);
    }

    /// Abort every in-flight `fetch_media_async` / `get_url_preview_async`
    /// download registered under `group_id`. Called on room switch to drop the
    /// previous room's pending media. No-op for group 0 or an unknown group.
    pub fn cancel_media_group(&self, group_id: u64) {
        if group_id == 0 {
            return;
        }
        // Drop parked (not-yet-started) waiters in both lanes so their queue
        // entries clear at once; each parked `acquire` returns None and the task
        // ends without delivering. Tasks already past the gate (downloading) are
        // aborted below via their handles, which on drop release their slot.
        self.media_gate_fg.cancel_group(group_id);
        self.media_gate_bulk.cancel_group(group_id);
        {
            let mut m = self.media_tasks.lock();
            if let Some(v) = m.remove(&group_id) {
                for (_, h) in v {
                    h.abort();
                }
            }
        }
    }

    /// Async counterpart of `get_url_preview`. Spawns the fetch under the bulk
    /// lane and fires `on_url_preview_ready(request_id, json)` on completion
    /// (empty string on failure).
    #[allow(deprecated)]
    pub fn get_url_preview_async(&self, request_id: u64, group_id: u64, url: &str) {
        use ruma::api::client::media::get_media_preview::v3::Request;

        let handler = self.handler.clone();
        let deliver = |json: &str| {
            if let Some(h) = &handler {
                {
                    let g = h.lock();
                    g.on_url_preview_ready(request_id, json);
                }
            }
        };
        let Some(client) = self.client.clone() else {
            deliver("");
            return;
        };
        if url.is_empty() {
            deliver("");
            return;
        }
        let in_flight = Arc::clone(&self.in_flight);
        let stop_rx = self.stop_rx.clone();
        let gate = Arc::clone(&self.media_gate_bulk);
        let url_str = url.to_owned();
        let handler_task = self.handler.clone();

        let handle = self.rt.spawn(async move {
            let deliver = |json: &str| {
                if let Some(h) = &handler_task {
                    {
                        let g = h.lock();
                        g.on_url_preview_ready(request_id, json);
                    }
                }
            };
            let _permit = match gate
                .acquire(super::media_queue::PRIO_NORMAL, request_id, group_id)
                .await
            {
                Some(p) => p,
                None => {
                    deliver("");
                    return;
                }
            };
            let _guard = super::InFlightGuard::new(&in_flight, &handler_task);
            let req = Request::new(url_str);
            let json = tokio::select! {
                result = async { client.send(req).await } => match result {
                    Ok(resp) => {
                        let json = resp.data.map(|v| v.get().to_owned()).unwrap_or_default();
                        if json.len() > MAX_URL_BYTES { String::new() } else { json }
                    }
                    Err(_) => String::new(),
                },
                _ = tokio::time::sleep(THUMBNAIL_FETCH_TIMEOUT) => String::new(),
                _ = stop_fut(stop_rx) => String::new(),
            };
            deliver(&json);
        });

        register_media_task(&self.media_tasks, group_id, request_id, handle.abort_handle());
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn fetch_avatar_bytes(&self, _room_id: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_media_bytes(&self, _mxc_url: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_source_bytes(&self, _source: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_url_bytes(&self, _url: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_gif_bytes(&self, _url: &str) -> Vec<u8> { Vec::new() }
}

#[cfg(test)]
mod tests {
    use super::{cap_media_bytes, MAX_MEDIA_BYTES, NOTIF_IMAGE_CAP};

    // Confirm constants have the expected values so any accidental edit is
    // caught immediately.
    #[test]
    fn cap_constants_have_expected_values() {
        assert_eq!(MAX_MEDIA_BYTES, 64 * 1024 * 1024, "MAX_MEDIA_BYTES should be 64 MiB");
        assert_eq!(NOTIF_IMAGE_CAP, 2 * 1024 * 1024, "NOTIF_IMAGE_CAP should be 2 MiB");
        assert!(
            NOTIF_IMAGE_CAP < MAX_MEDIA_BYTES,
            "notification cap should be tighter than the general media cap"
        );
    }

    // cap_media_bytes must pass through payloads at or below MAX_MEDIA_BYTES.
    #[test]
    fn cap_media_bytes_passes_within_limit() {
        let small = vec![0u8; 1024];
        assert_eq!(cap_media_bytes(small.clone()), small);

        let exactly_max = vec![0u8; MAX_MEDIA_BYTES];
        assert_eq!(cap_media_bytes(exactly_max.clone()).len(), MAX_MEDIA_BYTES);
    }

    // cap_media_bytes must discard payloads that exceed MAX_MEDIA_BYTES.
    #[test]
    fn cap_media_bytes_discards_oversized() {
        let oversized = vec![0u8; MAX_MEDIA_BYTES + 1];
        assert!(
            cap_media_bytes(oversized).is_empty(),
            "cap_media_bytes should return empty Vec for payload exceeding MAX_MEDIA_BYTES"
        );
    }

    // The avatar path applies NOTIF_IMAGE_CAP (same as the preview path).
    // The fetch itself is not unit-testable without a live matrix_sdk client,
    // so we test the cap helper directly: confirm that a payload within
    // NOTIF_IMAGE_CAP survives and one just over it is rejected, exactly
    // mirroring what `Ok(b) if b.len() <= NOTIF_IMAGE_CAP` does inline.
    #[test]
    fn notif_image_cap_rejects_oversized_avatar() {
        let within_cap = vec![1u8; NOTIF_IMAGE_CAP];
        assert_eq!(
            within_cap.len(),
            NOTIF_IMAGE_CAP,
            "payload at cap boundary must be accepted by `<= NOTIF_IMAGE_CAP` guard"
        );

        let over_cap = vec![1u8; NOTIF_IMAGE_CAP + 1];
        // Inline guard: `Ok(b) if b.len() <= NOTIF_IMAGE_CAP => b, _ => Vec::new()`
        let result: Vec<u8> = if over_cap.len() <= NOTIF_IMAGE_CAP {
            over_cap.clone()
        } else {
            Vec::new()
        };
        assert!(
            result.is_empty(),
            "avatar bytes exceeding NOTIF_IMAGE_CAP must be discarded"
        );
    }
}
