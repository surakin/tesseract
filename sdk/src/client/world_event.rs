//! A room's 3D world: the `dev.tesseract.world` state event, the upload map that
//! keeps the same bundle from being uploaded twice, and the verified download.
//!
//! The state event (state_key `""`) is
//! `{ "name": ..., "bundle": { "hash", "url" (mxc), "size", "format" } }`.
//! There is deliberately no author field: whoever published the world is the
//! event's `sender`, which the homeserver sets on the envelope. Anything the
//! content says about authorship is ignored.
//!
//! Everything that does not need a network lives here as plain functions taking
//! closures for "does this media still exist", "upload" and "download", so the
//! tests run offline. The `ClientFfi` methods at the bottom wire them to
//! matrix-sdk.

use std::collections::BTreeMap;

use serde_json::{json, Value};

/// The room state event type; state_key is always `""`.
pub const TYPE_WORLD: &str = "dev.tesseract.world";
/// Account data: `hash -> {mxc, size}` of bundles this account already uploaded.
pub const TYPE_UPLOADS: &str = "dev.tesseract.world_uploads";
/// Largest bundle we upload or download (the same cap as other media).
pub const MAX_BUNDLE_BYTES: u64 = 64 * 1024 * 1024;

/// The zip a world was published as.
#[derive(Clone, Debug, PartialEq)]
pub struct Bundle {
    pub hash: String,
    pub url: String,
    pub size: u64,
    pub format: u32,
}

/// A room's world as read back: the event's content plus its verified sender.
#[derive(Clone, Debug, PartialEq)]
pub struct RoomWorldInfo {
    pub name: String,
    pub hash: String,
    pub url: String,
    pub sender: String,
    pub size: u64,
    pub format: u32,
}

/// One entry of the `dev.tesseract.world_uploads` map.
#[derive(Clone, Debug, PartialEq)]
pub struct UploadEntry {
    pub mxc: String,
    pub size: u64,
}

pub type Uploads = BTreeMap<String, UploadEntry>;

/// The state event content for publishing `name` as `bundle`.
pub fn build_content(name: &str, bundle: &Bundle) -> Value {
    json!({
        "name": name,
        "bundle": {
            "hash": bundle.hash,
            "url": bundle.url,
            "size": bundle.size,
            "format": bundle.format,
        },
    })
}

fn is_hex_hash(h: &str) -> bool {
    h.len() == 64 && h.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// Parses event `content`; `sender` comes from the event envelope, never from
/// the content. An emptied event (`{}`, how a world is removed) is an error.
pub fn parse_content(content: &Value, sender: &str) -> Result<RoomWorldInfo, String> {
    let bundle = content
        .get("bundle")
        .and_then(Value::as_object)
        .ok_or("world event has no bundle")?;
    let hash = bundle
        .get("hash")
        .and_then(Value::as_str)
        .filter(|h| !h.is_empty())
        .ok_or("world event has no hash")?;
    if !is_hex_hash(hash) {
        return Err("world event hash is not a SHA-256".to_string());
    }
    let url = bundle
        .get("url")
        .and_then(Value::as_str)
        .filter(|u| !u.is_empty())
        .ok_or("world event has no url")?;
    let media_id_ok = url.rsplit_once('/').is_some_and(|(_, id)| !id.is_empty());
    if !url.starts_with("mxc://")
        || !media_id_ok
        || !matrix_sdk::ruma::OwnedMxcUri::from(url).is_valid()
    {
        return Err("world event url is not an mxc:// uri".to_string());
    }
    let size = bundle
        .get("size")
        .and_then(Value::as_u64)
        .ok_or("world event has no size")?;
    if size > MAX_BUNDLE_BYTES {
        return Err("world bundle is too large".to_string());
    }
    let format = bundle
        .get("format")
        .and_then(Value::as_u64)
        .and_then(|f| u32::try_from(f).ok())
        .filter(|f| *f != 0)
        .ok_or("world event has no format")?;
    Ok(RoomWorldInfo {
        name: content
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string(),
        hash: hash.to_string(),
        url: url.to_string(),
        sender: sender.to_string(),
        size,
        format,
    })
}

/// What a room's world event reads as: no world (absent, or emptied: how a world is removed), a
/// usable world, or an event that is there but cannot be used. `Error` carries the `format` read
/// leniently from the content (0 when unreadable), so a shape this build does not know can still
/// be told apart as "needs a newer Tesseract".
#[derive(Clone, Debug, PartialEq)]
pub enum WorldRead {
    None,
    World(RoomWorldInfo),
    Error { message: String, format: u32 },
}

/// Classifies event `content` (see `WorldRead`); `sender` is the envelope's.
pub fn classify_content(content: &Value, sender: &str) -> WorldRead {
    if content.as_object().is_some_and(|m| m.is_empty()) {
        return WorldRead::None;
    }
    let format = content
        .get("bundle")
        .and_then(|b| b.get("format"))
        .and_then(Value::as_u64)
        .map(|f| u32::try_from(f).unwrap_or(u32::MAX))
        .unwrap_or(0);
    if format > crate::world_bundle::FORMAT {
        return WorldRead::Error {
            message: "this world needs a newer Tesseract".to_string(),
            format,
        };
    }
    match parse_content(content, sender) {
        Ok(w) => WorldRead::World(w),
        Err(message) => WorldRead::Error { message, format },
    }
}

/// `None` when `ev` is not the room's world event (type and empty state_key).
pub fn classify_state_event(ev: &Value) -> Option<WorldRead> {
    if ev.get("type").and_then(Value::as_str) != Some(TYPE_WORLD)
        || ev.get("state_key").and_then(Value::as_str) != Some("")
    {
        return None;
    }
    let sender = ev.get("sender").and_then(Value::as_str).unwrap_or_default();
    let content = ev.get("content").unwrap_or(&Value::Null);
    if sender.is_empty() {
        return Some(WorldRead::Error {
            message: "world event has no sender".to_string(),
            format: 0,
        });
    }
    Some(classify_content(content, sender))
}

/// Reads a whole state event (type, state_key, sender, content); `None` when it
/// is not the room's world event, `Some(Err)` when it is but is unusable.
#[cfg(test)]
pub fn parse_state_event(ev: &Value) -> Option<Result<RoomWorldInfo, String>> {
    if ev.get("type").and_then(Value::as_str) != Some(TYPE_WORLD)
        || ev.get("state_key").and_then(Value::as_str) != Some("")
    {
        return None;
    }
    let sender = ev.get("sender").and_then(Value::as_str).unwrap_or_default();
    if sender.is_empty() {
        return Some(Err("world event has no sender".to_string()));
    }
    Some(parse_content(
        ev.get("content").unwrap_or(&Value::Null),
        sender,
    ))
}

/// Tolerant: whatever is unreadable is simply not in the map (it is only a cache).
pub fn parse_uploads(content: Option<&Value>) -> Uploads {
    let mut out = Uploads::new();
    if let Some(obj) = content.and_then(Value::as_object) {
        for (hash, v) in obj {
            if let (Some(mxc), Some(size)) = (
                v.get("mxc").and_then(Value::as_str),
                v.get("size").and_then(Value::as_u64),
            ) {
                out.insert(
                    hash.clone(),
                    UploadEntry {
                        mxc: mxc.to_string(),
                        size,
                    },
                );
            }
        }
    }
    out
}

pub fn uploads_to_value(uploads: &Uploads) -> Value {
    Value::Object(
        uploads
            .iter()
            .map(|(h, e)| (h.clone(), json!({ "mxc": e.mxc, "size": e.size })))
            .collect(),
    )
}

/// Where the bundle with `hash` lives: the recorded upload if the server still
/// serves the same bytes, otherwise a fresh upload (recorded in `uploads`). The second value is
/// whether `uploads` changed and must be saved.
pub fn resolve_upload(
    uploads: &mut Uploads,
    hash: &str,
    size: u64,
    fetch_media: &mut dyn FnMut(&str) -> Option<Vec<u8>>,
    upload: &mut dyn FnMut() -> Result<String, String>,
) -> Result<(String, bool), String> {
    // A recorded upload is trusted only if the server still serves exactly these
    // bytes: a gone, replaced or poisoned entry is uploaded again.
    if let Some(e) = uploads.get(hash) {
        if e.size == size {
            if let Some(bytes) = fetch_media(&e.mxc) {
                if bytes.len() as u64 == size && crate::world_bundle::sha256_hex(&bytes) == hash {
                    return Ok((e.mxc.clone(), false));
                }
            }
        }
    }
    let mxc = upload()?;
    uploads.insert(
        hash.to_string(),
        UploadEntry {
            mxc: mxc.clone(),
            size,
        },
    );
    Ok((mxc, true))
}

/// Checks downloaded bytes against what the event promised: size, then hash.
pub fn verify_bundle(bytes: &[u8], info: &RoomWorldInfo) -> Result<(), String> {
    if bytes.len() as u64 != info.size {
        return Err(format!(
            "world bundle size mismatch: expected {} bytes, got {}",
            info.size,
            bytes.len()
        ));
    }
    let got = crate::world_bundle::sha256_hex(bytes);
    if got != info.hash {
        return Err("world bundle hash mismatch".to_string());
    }
    Ok(())
}

/// Downloads through `download` (given the mxc url) and verifies the result.
pub fn download_verified(
    info: &RoomWorldInfo,
    download: &mut dyn FnMut(&str) -> Result<Vec<u8>, String>,
) -> Result<Vec<u8>, String> {
    if info.size > MAX_BUNDLE_BYTES {
        return Err("world bundle is too large".to_string());
    }
    let bytes = download(&info.url)?;
    verify_bundle(&bytes, info)?;
    Ok(bytes)
}

/// Runs `op` but gives up after `timeout`, or when `stop` resolves (app
/// shutdown), so a stalled server cannot hang a worker thread forever.
pub async fn bounded<T>(
    op: impl std::future::Future<Output = T>,
    timeout: std::time::Duration,
    stop: impl std::future::Future<Output = ()>,
) -> Result<T, String> {
    tokio::select! {
        r = op => Ok(r),
        _ = tokio::time::sleep(timeout) => Err("timed out".to_string()),
        _ = stop => Err("cancelled".to_string()),
    }
}

/// What sync does when a state event of the world type arrives: only the room's
/// own world (state_key `""`) raises `on_room_world_changed`.
pub fn on_world_state_event(state_key: &str, room_id: &str, notify: &mut dyn FnMut(&str)) -> bool {
    if state_key.is_empty() {
        notify(room_id);
        true
    } else {
        false
    }
}

/// `dev.tesseract.world` as received, kept as raw JSON. Only used to route the
/// live-refresh handler in `start_sync`; the room's state is re-read on demand.
#[cfg(not(test))]
#[derive(
    Clone,
    Debug,
    serde::Serialize,
    serde::Deserialize,
    matrix_sdk::ruma::events::macros::EventContent,
)]
#[ruma_event(type = "dev.tesseract.world", kind = State, state_key_type = String)]
pub struct WorldRawEventContent {
    #[serde(default, flatten)]
    pub fields: serde_json::Map<String, Value>,
}

#[cfg(not(test))]
fn info_to_ffi(w: RoomWorldInfo) -> crate::ffi::RoomWorldFfi {
    crate::ffi::RoomWorldFfi {
        present: true,
        error: String::new(),
        name: w.name,
        hash: w.hash,
        url: w.url,
        sender: w.sender,
        size: w.size,
        format: w.format,
    }
}

/// `present` for a world; else `error` empty = no world, non-empty = unusable (`format` as read).
#[cfg(not(test))]
fn read_to_ffi(r: WorldRead) -> crate::ffi::RoomWorldFfi {
    match r {
        WorldRead::World(w) => info_to_ffi(w),
        WorldRead::None => read_error_ffi(String::new(), 0),
        WorldRead::Error { message, format } => read_error_ffi(message, format),
    }
}

#[cfg(not(test))]
fn read_error_ffi(error: String, format: u32) -> crate::ffi::RoomWorldFfi {
    crate::ffi::RoomWorldFfi {
        present: false,
        error,
        name: String::new(),
        hash: String::new(),
        url: String::new(),
        sender: String::new(),
        size: 0,
        format,
    }
}

/// Classifies a state event's content and envelope sender (see `classify_content`).
#[cfg(not(test))]
pub fn parse_event_ffi(content_json: &str, sender: &str) -> crate::ffi::RoomWorldFfi {
    match serde_json::from_str::<Value>(content_json) {
        Ok(c) => read_to_ffi(classify_content(&c, sender)),
        Err(e) => read_error_ffi(format!("world event is not JSON: {e}"), 0),
    }
}

#[cfg(not(test))]
use super::{err, ok, ClientFfi};
#[cfg(not(test))]
use crate::ffi::OpResult;

#[cfg(not(test))]
impl ClientFfi {
    fn world_room(
        &self,
        room_id: &str,
    ) -> Result<(matrix_sdk::Room, matrix_sdk::ruma::OwnedRoomId), String> {
        let Some(client) = self.client.as_ref() else {
            return Err("not logged in".to_string());
        };
        let rid = room_id
            .parse::<matrix_sdk::ruma::OwnedRoomId>()
            .map_err(|e| format!("invalid room id: {e}"))?;
        let room = client.get_room(&rid).ok_or("room not found")?;
        Ok((room, rid))
    }

    /// Publishes `bundle_bytes` as the room's world: hashes it, reuses an earlier
    /// upload of the same bytes if the server still has it (account data
    /// `dev.tesseract.world_uploads`), else uploads, then sends the state event.
    /// Blocks: worker thread.
    pub fn set_room_world(&self, room_id: &str, name: &str, bundle_bytes: &[u8]) -> OpResult {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;

        let (room, rid) = match self.world_room(room_id) {
            Ok(r) => r,
            Err(e) => return err(e),
        };
        let client = self.client.clone().expect("checked by world_room");
        if bundle_bytes.is_empty() {
            return err("world bundle is empty");
        }
        if bundle_bytes.len() as u64 > MAX_BUNDLE_BYTES {
            return err("world bundle is too large");
        }
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "world/set_room_world".to_string(),
        );

        let hash = crate::world_bundle::sha256_hex(bundle_bytes);
        let size = bundle_bytes.len() as u64;

        let ev_type = GlobalAccountDataEventType::from(TYPE_UPLOADS);
        let read_uploads = || -> Result<Uploads, String> {
            let existing = self
                .rt
                .block_on(client.account().account_data_raw(ev_type.clone()))
                .map_err(|e| format!("read world uploads: {e}"))?;
            let value: Option<Value> =
                existing.and_then(|raw| serde_json::from_str(raw.json().get()).ok());
            Ok(parse_uploads(value.as_ref()))
        };
        // The check and the upload run without the account-data lock.
        let mut uploads = match read_uploads() {
            Ok(u) => u,
            Err(e) => return err(e),
        };

        let mut fetch_media = |mxc: &str| {
            let uri = matrix_sdk::ruma::OwnedMxcUri::from(mxc);
            if !uri.is_valid() {
                return None;
            }
            // matrix-sdk has no HEAD for media; fetching bypasses the local cache
            // so a deleted upload really reads as gone.
            let request = MediaRequestParameters {
                source: MediaSource::Plain(uri),
                format: MediaFormat::File,
            };
            let stop_rx = self.stop_rx.clone();
            self.rt
                .block_on(bounded(
                    client.media().get_media_content(&request, false),
                    super::media::FULL_MEDIA_FETCH_TIMEOUT,
                    super::stop_fut(stop_rx),
                ))
                .ok()?
                .ok()
        };
        let mime: mime::Mime = "application/zip".parse().expect("static mime");
        let mut upload = || {
            self.rt
                .block_on(super::account::upload_bytes(
                    &client,
                    bundle_bytes.to_vec(),
                    &mime,
                ))
                .map(|u| u.to_string())
                .map_err(|e| format!("upload world bundle: {e}"))
        };
        let (mxc, changed) =
            match resolve_upload(&mut uploads, &hash, size, &mut fetch_media, &mut upload) {
                Ok(r) => r,
                Err(e) => return err(e),
            };
        if changed {
            // Only the read-merge-write of the map is serialised; the lock is
            // dropped before the state event is sent.
            let _ad_guard = {
                let l = std::sync::Arc::clone(&self.account_data_lock);
                self.rt.block_on(async move { l.lock_owned().await })
            };
            let mut latest = read_uploads().unwrap_or(uploads);
            latest.insert(
                hash.clone(),
                UploadEntry {
                    mxc: mxc.clone(),
                    size,
                },
            );
            match Raw::new(&uploads_to_value(&latest)) {
                Ok(r) => {
                    // Only a cache: failing to record it must not fail the publish.
                    if let Err(e) = self.rt.block_on(
                        client
                            .account()
                            .set_account_data_raw(ev_type.clone(), r.cast_unchecked()),
                    ) {
                        tracing::warn!("saving world uploads failed: {e}");
                    }
                }
                Err(e) => tracing::warn!("serialize world uploads: {e}"),
            }
        }

        let content = build_content(
            name,
            &Bundle {
                hash,
                url: mxc,
                size,
                format: crate::world_bundle::FORMAT,
            },
        );
        let result = self
            .rt
            .block_on(async { room.send_state_event_raw(TYPE_WORLD, "", content).await });
        super::invalidate_room_state_cache(&self.room_state_cache, &rid);
        match result {
            Ok(_) => {
                self.notify_world_changed(room_id);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    /// The room's current world with its verified sender (the event's envelope, not its content).
    /// `present == false` with an empty `error`: the room has no world (or it was removed). With
    /// an `error`: the event could not be read (no client, bad room, state fetch failed) or is
    /// malformed; `format` is what the content said, if anything. Reads the cached full room state.
    pub fn room_world(&self, room_id: &str) -> crate::ffi::RoomWorldFfi {
        let fail = |m: &str| read_error_ffi(m.to_string(), 0);
        let Some(client) = self.client.clone() else {
            return fail("not logged in");
        };
        let Ok(rid) = room_id.parse::<matrix_sdk::ruma::OwnedRoomId>() else {
            return fail("invalid room id");
        };
        // Sliding sync never delivers this event, so the cached room state can be stale: every
        // read (the world view opening, a Reload) drops it and asks the homeserver.
        let cache = std::sync::Arc::clone(&self.room_state_cache);
        super::invalidate_room_state_cache(&cache, &rid);
        let state = self
            .rt
            .block_on(super::fetch_room_state_cached(&client, &cache, &rid));
        let Some(state) = state else {
            return fail("could not read the room's state");
        };
        for raw in state.iter() {
            if raw.get_field::<String>("type").ok().flatten().as_deref() != Some(TYPE_WORLD) {
                continue;
            }
            let Ok(ev) = serde_json::from_str::<Value>(raw.json().get()) else {
                return fail("the room's world event is unreadable");
            };
            if let Some(r) = classify_state_event(&ev) {
                return read_to_ffi(r);
            }
        }
        read_error_ffi(String::new(), 0)
    }

    /// Downloads and verifies (size, then SHA-256) the bundle `world` names.
    /// Blocks: worker thread.
    pub fn download_world_bundle(
        &self,
        world: &crate::ffi::RoomWorldFfi,
    ) -> crate::ffi::WorldBundleDownloadFfi {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;

        let fail = |error: String| crate::ffi::WorldBundleDownloadFfi {
            ok: false,
            error,
            bytes: Vec::new(),
        };
        let Some(client) = self.client.clone() else {
            return fail("not logged in".to_string());
        };
        let info = RoomWorldInfo {
            name: world.name.clone(),
            hash: world.hash.clone(),
            url: world.url.clone(),
            sender: world.sender.clone(),
            size: world.size,
            format: world.format,
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "world/download".to_string(),
        );
        let mut download = |url: &str| {
            let uri = matrix_sdk::ruma::OwnedMxcUri::from(url);
            if !uri.is_valid() {
                return Err("world url is not an mxc:// uri".to_string());
            }
            let request = MediaRequestParameters {
                source: MediaSource::Plain(uri),
                format: MediaFormat::File,
            };
            let stop_rx = self.stop_rx.clone();
            self.rt
                .block_on(bounded(
                    client.media().get_media_content(&request, false),
                    super::media::FULL_MEDIA_FETCH_TIMEOUT,
                    super::stop_fut(stop_rx),
                ))
                .map_err(|e| format!("download world bundle: {e}"))?
                .map_err(|e| format!("download world bundle: {e}"))
        };
        match download_verified(&info, &mut download) {
            Ok(bytes) => crate::ffi::WorldBundleDownloadFfi {
                ok: true,
                error: String::new(),
                bytes,
            },
            Err(e) => fail(e),
        }
    }

    /// True iff the current user's power level allows sending
    /// `dev.tesseract.world` here. Cached, no network; false on any uncertainty.
    pub fn can_set_room_world(&self, room_id: &str) -> bool {
        use matrix_sdk::ruma::events::StateEventType;

        let Ok((room, _)) = self.world_room(room_id) else {
            return false;
        };
        let Some(user_id) = self.client.as_ref().and_then(|c| c.user_id()) else {
            return false;
        };
        match self.rt.block_on(room.power_levels()) {
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::from(TYPE_WORLD)),
            Err(_) => false,
        }
    }

    /// Removes the room's world by emptying the state event (a state event cannot
    /// be deleted); `room_world` then reads as none. Blocks: worker thread.
    pub fn remove_room_world(&self, room_id: &str) -> OpResult {
        let (room, rid) = match self.world_room(room_id) {
            Ok(r) => r,
            Err(e) => return err(e),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "world/remove_room_world".to_string(),
        );
        let result = self
            .rt
            .block_on(async { room.send_state_event_raw(TYPE_WORLD, "", json!({})).await });
        super::invalidate_room_state_cache(&self.room_state_cache, &rid);
        match result {
            Ok(_) => {
                self.notify_world_changed(room_id);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    fn notify_world_changed(&self, room_id: &str) {
        if let Some(h) = &self.handler {
            h.lock().on_room_world_changed(room_id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classify_none_world_and_error() {
        let ok = serde_json::from_str::<Value>(&format!(
            r#"{{"name":"A","bundle":{{"hash":"{HASH}","url":"mxc://e.org/a","size":5,"format":1}}}}"#
        ))
        .unwrap();
        assert!(matches!(classify_content(&ok, "@a:e"), WorldRead::World(_)));
        // An emptied event is how a world is removed.
        assert!(matches!(
            classify_content(&json!({}), "@a:e"),
            WorldRead::None
        ));
        // Malformed: a string where a number belongs.
        let bad = json!({"bundle":{"hash":HASH,"url":"mxc://e.org/a","size":"5","format":1}});
        assert!(matches!(
            classify_content(&bad, "@a:e"),
            WorldRead::Error { format: 1, .. }
        ));
        assert!(matches!(
            classify_content(&json!("x"), "@a:e"),
            WorldRead::Error { .. }
        ));
        assert!(matches!(
            classify_content(&Value::Null, "@a:e"),
            WorldRead::Error { .. }
        ));
    }

    #[test]
    fn classify_reads_a_newer_format_even_when_the_shape_is_unknown() {
        let future = json!({"bundle":{"format":2,"blob":{"x":1}}});
        match classify_content(&future, "@a:e") {
            WorldRead::Error { format, .. } => assert_eq!(format, 2),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn classify_state_event_needs_a_sender() {
        let ev = json!({"type":TYPE_WORLD,"state_key":"","content":{"bundle":{}}});
        assert!(matches!(
            classify_state_event(&ev),
            Some(WorldRead::Error { .. })
        ));
        let other = json!({"type":"m.room.name","state_key":"","content":{}});
        assert!(classify_state_event(&other).is_none());
        let removed = json!({"type":TYPE_WORLD,"state_key":"","sender":"@a:e","content":{}});
        assert!(matches!(
            classify_state_event(&removed),
            Some(WorldRead::None)
        ));
    }

    const HASH: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    fn bundle() -> Bundle {
        Bundle {
            hash: HASH.to_string(),
            url: "mxc://example.org/abc".to_string(),
            size: 1234,
            format: 1,
        }
    }

    #[test]
    fn event_content_roundtrip() {
        let content = build_content("Arena", &bundle());
        assert!(content.get("sender").is_none() && content.get("author").is_none());
        let info = parse_content(&content, "@alice:example.org").unwrap();
        assert_eq!(
            info,
            RoomWorldInfo {
                name: "Arena".into(),
                hash: HASH.into(),
                url: "mxc://example.org/abc".into(),
                sender: "@alice:example.org".into(),
                size: 1234,
                format: 1,
            }
        );
    }

    #[test]
    fn sender_comes_from_the_envelope_not_the_content() {
        let mut content = build_content("Arena", &bundle());
        content["sender"] = json!("@mallory:example.org");
        content["author"] = json!("@mallory:example.org");
        let ev = json!({
            "type": TYPE_WORLD, "state_key": "", "sender": "@alice:example.org",
            "content": content,
        });
        let info = parse_state_event(&ev).unwrap().unwrap();
        assert_eq!(info.sender, "@alice:example.org");
        // Other types and other state keys are not the room's world.
        assert!(parse_state_event(&json!({"type": "m.room.name", "state_key": ""})).is_none());
        assert!(parse_state_event(
            &json!({"type": TYPE_WORLD, "state_key": "x", "sender": "@a:b", "content": {}})
        )
        .is_none());
        // A removed world ({}) is unusable, not an old world.
        let removed = json!({"type": TYPE_WORLD, "state_key": "", "sender": "@a:b", "content": {}});
        assert!(parse_state_event(&removed).unwrap().is_err());
    }

    #[test]
    fn rejects_event_without_hash_or_url() {
        let mut no_hash = build_content("A", &bundle());
        no_hash["bundle"].as_object_mut().unwrap().remove("hash");
        assert!(parse_content(&no_hash, "@a:b")
            .unwrap_err()
            .contains("hash"));
        let mut empty_hash = build_content("A", &bundle());
        empty_hash["bundle"]["hash"] = json!("");
        assert!(parse_content(&empty_hash, "@a:b").is_err());
        let mut no_url = build_content("A", &bundle());
        no_url["bundle"].as_object_mut().unwrap().remove("url");
        assert!(parse_content(&no_url, "@a:b").unwrap_err().contains("url"));
        assert!(parse_content(&json!({"name": "A"}), "@a:b").is_err());
        let mut zero = build_content("A", &bundle());
        zero["bundle"]["format"] = json!(0);
        assert!(parse_content(&zero, "@a:b").is_err());
    }

    #[test]
    fn rejects_non_mxc_url() {
        for bad in [
            "https://example.org/x",
            "http://evil/x",
            "mxc://",
            "mxc://server",
            "mxc://example.org/",
            "file:///etc/passwd",
        ] {
            let mut c = build_content("A", &bundle());
            c["bundle"]["url"] = json!(bad);
            assert!(parse_content(&c, "@a:b").is_err(), "{bad}");
        }
    }

    fn info_for(bytes: &[u8]) -> RoomWorldInfo {
        RoomWorldInfo {
            name: "A".into(),
            hash: crate::world_bundle::sha256_hex(bytes),
            url: "mxc://example.org/abc".into(),
            sender: "@a:b".into(),
            size: bytes.len() as u64,
            format: 1,
        }
    }

    const ZIP: &[u8] = b"zip-bytes";

    fn zip_hash() -> String {
        crate::world_bundle::sha256_hex(ZIP)
    }

    fn map_with(hash: &str, mxc: &str, size: u64) -> Uploads {
        let mut u = Uploads::new();
        u.insert(
            hash.into(),
            UploadEntry {
                mxc: mxc.into(),
                size,
            },
        );
        u
    }

    #[test]
    fn dedup_map_reuses_existing_upload_when_media_present() {
        let hash = zip_hash();
        let mut uploads = map_with(&hash, "mxc://example.org/old", ZIP.len() as u64);
        let mut checked = Vec::new();
        let mut uploaded = 0;
        let (mxc, changed) = resolve_upload(
            &mut uploads,
            &hash,
            ZIP.len() as u64,
            &mut |m| {
                checked.push(m.to_string());
                Some(ZIP.to_vec())
            },
            &mut || {
                uploaded += 1;
                Ok("mxc://example.org/new".into())
            },
        )
        .unwrap();
        assert_eq!(mxc, "mxc://example.org/old");
        assert!(!changed);
        assert_eq!(uploaded, 0);
        assert_eq!(checked, vec!["mxc://example.org/old"]);
    }

    #[test]
    fn dedup_reuploads_when_media_missing() {
        let hash = zip_hash();
        let mut uploads = map_with(&hash, "mxc://example.org/old", ZIP.len() as u64);
        let (mxc, changed) = resolve_upload(
            &mut uploads,
            &hash,
            ZIP.len() as u64,
            &mut |_| None,
            &mut || Ok("mxc://example.org/new".into()),
        )
        .unwrap();
        assert_eq!(mxc, "mxc://example.org/new");
        assert!(changed);
        assert_eq!(uploads[&hash].mxc, "mxc://example.org/new");
        // Never seen before: uploaded without asking the server.
        let mut fresh = Uploads::new();
        let (mxc, changed) = resolve_upload(
            &mut fresh,
            &hash,
            10,
            &mut |_| panic!("nothing to check"),
            &mut || Ok("mxc://example.org/n2".into()),
        )
        .unwrap();
        assert_eq!((mxc.as_str(), changed), ("mxc://example.org/n2", true));
        // A failed upload leaves the map alone.
        let mut u = Uploads::new();
        assert!(
            resolve_upload(&mut u, &hash, 10, &mut |_| None, &mut || Err("boom".into())).is_err()
        );
        assert!(u.is_empty());
    }

    #[test]
    fn dedup_reuploads_when_fetched_bytes_hash_differently() {
        // A stale or poisoned entry: same size, other bytes.
        let hash = zip_hash();
        let other = vec![b'x'; ZIP.len()];
        let mut uploads = map_with(&hash, "mxc://example.org/poison", ZIP.len() as u64);
        let (mxc, changed) = resolve_upload(
            &mut uploads,
            &hash,
            ZIP.len() as u64,
            &mut |_| Some(other.clone()),
            &mut || Ok("mxc://example.org/new".into()),
        )
        .unwrap();
        assert_eq!(mxc, "mxc://example.org/new");
        assert!(changed);
    }

    #[test]
    fn bounded_times_out_a_never_finishing_fetch() {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_time()
            .build()
            .unwrap();
        let r = rt.block_on(bounded(
            std::future::pending::<Vec<u8>>(),
            std::time::Duration::from_millis(20),
            std::future::pending::<()>(),
        ));
        assert_eq!(r.unwrap_err(), "timed out");
        let r = rt.block_on(bounded(
            std::future::pending::<Vec<u8>>(),
            std::time::Duration::from_secs(60),
            std::future::ready(()),
        ));
        assert_eq!(r.unwrap_err(), "cancelled");
        let ok = rt.block_on(bounded(
            std::future::ready(5),
            std::time::Duration::from_secs(60),
            std::future::pending::<()>(),
        ));
        assert_eq!(ok.unwrap(), 5);
    }

    #[test]
    fn uploads_map_roundtrip_and_tolerates_junk() {
        let mut u = Uploads::new();
        u.insert(
            HASH.into(),
            UploadEntry {
                mxc: "mxc://a/b".into(),
                size: 5,
            },
        );
        assert_eq!(parse_uploads(Some(&uploads_to_value(&u))), u);
        let junk =
            json!({"x": 1, "y": {"mxc": "mxc://a/b"}, HASH: {"mxc": "mxc://a/b", "size": 5}});
        assert_eq!(parse_uploads(Some(&junk)).len(), 1);
        assert!(parse_uploads(None).is_empty());
    }

    #[test]
    fn download_accepts_matching_bytes() {
        let bytes = b"zip-bytes".to_vec();
        let info = info_for(&bytes);
        let got = download_verified(&info, &mut |url| {
            assert_eq!(url, "mxc://example.org/abc");
            Ok(bytes.clone())
        })
        .unwrap();
        assert_eq!(got, bytes);
    }

    #[test]
    fn download_rejects_hash_mismatch() {
        let info = info_for(b"zip-bytes");
        let err = download_verified(&info, &mut |_| Ok(b"zip-BYTES".to_vec())).unwrap_err();
        assert!(err.contains("hash"), "{err}");
    }

    #[test]
    fn download_rejects_size_mismatch() {
        let info = info_for(b"zip-bytes");
        let err = download_verified(&info, &mut |_| Ok(b"zip-bytes!".to_vec())).unwrap_err();
        assert!(err.contains("size"), "{err}");
        let err = download_verified(&info, &mut |_| Err("offline".into())).unwrap_err();
        assert_eq!(err, "offline");
    }

    #[test]
    fn sync_world_change_raises_callback() {
        let mut seen = Vec::new();
        assert!(on_world_state_event("", "!r:x", &mut |r| seen.push(r.to_string())));
        assert_eq!(seen, vec!["!r:x"]);
        assert!(!on_world_state_event("other", "!r:x", &mut |_| panic!(
            "not the room's world"
        )));
    }
}
