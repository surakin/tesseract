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
    pub fn fetch_avatar_bytes(&mut self, room_id: &str) -> Vec<u8> {
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
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    pub fn fetch_media_bytes(&mut self, mxc_url: &str) -> Vec<u8> {
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
        self.rt.block_on(async move {
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
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
    pub fn fetch_source_bytes(&mut self, source: &str) -> Vec<u8> {
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
        self.rt.block_on(async move {
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    pub fn fetch_url_bytes(&mut self, url: &str) -> Vec<u8> {
        if url.is_empty() {
            return Vec::new();
        }
        let url = url.to_owned();
        let stop_rx = self.stop_rx.clone();
        let client = self.http_client.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = async {
                    match client.get(&url).send().await {
                        Ok(resp) => {
                            match resp.error_for_status() {
                                Ok(resp) => resp.bytes().await
                                    .map(|b| b.to_vec())
                                    .unwrap_or_default(),
                                Err(_) => Vec::new(),
                            }
                        }
                        Err(_) => Vec::new(),
                    }
                } => result,
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    // -----------------------------------------------------------------------
    // URL preview (homeserver og:* metadata fetch)
    // -----------------------------------------------------------------------

    #[allow(deprecated)]
    pub fn get_url_preview(&mut self, url: &str) -> String {
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
                        Ok(resp) => resp.data
                            .map(|v| v.get().to_owned())
                            .unwrap_or_default(),
                        Err(_) => String::new(),
                    }
                }
                _ = stop_fut(stop_rx) => String::new(),
            }
        })
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn fetch_avatar_bytes(&mut self, _room_id: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_media_bytes(&mut self, _mxc_url: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_source_bytes(&mut self, _source: &str) -> Vec<u8> { Vec::new() }
    pub fn fetch_url_bytes(&mut self, _url: &str) -> Vec<u8> { Vec::new() }
    pub fn get_url_preview(&mut self, _url: &str) -> String { String::new() }
}
