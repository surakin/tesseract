//! User account methods: identity (user_id, display name, avatar), profile
//! editing, device management, presence, DM lookup, homeserver discovery,
//! ignored-user list, recent-emoji ranking, and application prefs.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::{err, ok, ClientFfi};

use crate::ffi::OpResult;

#[cfg(not(test))]
use super::build_uia_fallback_url;

#[cfg(not(test))]
use matrix_sdk::Client;

#[cfg(not(test))]
use std::sync::Arc;

// ---------------------------------------------------------------------------
// Free helpers (called by methods in this module and from sync.rs)
// ---------------------------------------------------------------------------

/// Read the user's MSC4356 recent-emoji blob with stable → unstable →
/// legacy precedence. Returns an empty Vec if no blob exists, the client
/// is in a fresh-login state, or every parse path errors out.
#[cfg(not(test))]
pub(super) async fn read_recent_emoji_entries(
    client: &Client,
) -> Vec<crate::recent_emoji::Entry> {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    use serde_json::Value;

    async fn fetch(client: &Client, ty: &str) -> Option<Value> {
        let et = GlobalAccountDataEventType::from(ty);
        let raw = client.account().account_data_raw(et).await.ok().flatten()?;
        serde_json::from_str::<Value>(raw.json().get()).ok()
    }

    if let Some(v) = fetch(client, crate::recent_emoji::TYPE_STABLE).await {
        let entries = crate::recent_emoji::parse_msc4356(&v);
        if !entries.is_empty() {
            return entries;
        }
    }
    if let Some(v) = fetch(client, crate::recent_emoji::TYPE_UNSTABLE).await {
        let entries = crate::recent_emoji::parse_msc4356(&v);
        if !entries.is_empty() {
            return entries;
        }
    }
    if let Some(v) = fetch(client, crate::recent_emoji::TYPE_LEGACY).await {
        return crate::recent_emoji::parse_legacy_element(&v);
    }
    Vec::new()
}

/// Raw media upload — bytes → mxc:// URI. Does not mutate any user profile.
/// Called by `upload_avatar` (upload + set global avatar) and by the
/// `upload_media` FFI (bare upload for room-member avatar, etc.).
#[cfg(not(test))]
pub(super) async fn upload_bytes(
    client: &Client,
    bytes: Vec<u8>,
    mime: &mime::Mime,
) -> matrix_sdk::Result<matrix_sdk::ruma::OwnedMxcUri> {
    Ok(client.media().upload(mime, bytes, None).await?.content_uri)
}

/// Read the raw JSON content of the `im.gnomos.tesseract` account-data event
/// from the SDK's local sync cache. Returns `"{}"` when missing or on error.
#[cfg(not(test))]
pub(super) async fn read_prefs_json(client: &Client) -> String {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    let et = GlobalAccountDataEventType::from("im.gnomos.tesseract");
    client
        .account()
        .account_data_raw(et)
        .await
        .ok()
        .flatten()
        .map(|r| r.json().get().to_owned())
        .unwrap_or_else(|| "{}".to_owned())
}

/// Read the MSC4278 global media-preview config from the local sync cache,
/// with stable → unstable precedence. Returns the MSC defaults (previews on,
/// invite avatars on) when no event exists or on any parse error.
#[cfg(not(test))]
pub(super) async fn read_media_preview_config(client: &Client) -> crate::media_preview::Config {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    use serde_json::Value;

    async fn fetch(client: &Client, ty: &str) -> Option<Value> {
        let et = GlobalAccountDataEventType::from(ty);
        let raw = client.account().account_data_raw(et).await.ok().flatten()?;
        serde_json::from_str::<Value>(raw.json().get()).ok()
    }

    if let Some(v) = fetch(client, crate::media_preview::TYPE_STABLE).await {
        return crate::media_preview::parse_global(&v);
    }
    if let Some(v) = fetch(client, crate::media_preview::TYPE_UNSTABLE).await {
        return crate::media_preview::parse_global(&v);
    }
    crate::media_preview::Config::default()
}

/// Map a joined room's local join rule to the MSC4278 wire string. Mirrors
/// the mapping used for MSC3266 summaries in `room_list.rs`.
#[cfg(not(test))]
fn join_rule_str(room: &matrix_sdk::Room) -> String {
    use matrix_sdk::ruma::events::room::join_rules::JoinRule;
    match room.join_rule() {
        Some(JoinRule::Public) => "public",
        Some(JoinRule::Invite) => "invite",
        Some(JoinRule::Knock) => "knock",
        Some(JoinRule::KnockRestricted(_)) => "knock_restricted",
        Some(JoinRule::Restricted(_)) => "restricted",
        Some(JoinRule::Private) => "private",
        Some(_) => "unknown",
        None => "",
    }
    .to_owned()
}

/// Raw JSON of whichever MSC4278 global event exists (stable preferred), or
/// `"{}"` when none does. Used by the sync watcher to detect changes.
#[cfg(not(test))]
pub(super) async fn read_media_preview_config_json(client: &Client) -> String {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    for ty in [
        crate::media_preview::TYPE_STABLE,
        crate::media_preview::TYPE_UNSTABLE,
    ] {
        let et = GlobalAccountDataEventType::from(ty);
        if let Some(raw) = client.account().account_data_raw(et).await.ok().flatten() {
            return raw.json().get().to_owned();
        }
    }
    "{}".to_owned()
}

// ---------------------------------------------------------------------------
// FFI impl
// ---------------------------------------------------------------------------

impl ClientFfi {
    /// Top-N glyphs from the user's MSC4356 `recent_emoji` account-data,
    /// ordered by `total` desc. Reads with the precedence stable
    /// (`m.recent_emoji`) → unstable
    /// (`io.github.johennes.msc4356.recent_emoji`) → legacy
    /// (`io.element.recent_emoji`) so existing Element users see their
    /// historical history on the first MSC4356 run without losing it. Reads
    /// the local sync cache only — no network roundtrip. Returns an empty
    /// vec when not logged in, when no blob has ever been written, or on
    /// any deserialization error: a broken blob never stalls the picker.
    #[cfg(not(test))]
    pub fn recent_emoji_top(&mut self, n: u32) -> Vec<String> {
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        let entries = self
            .rt
            .block_on(async move { read_recent_emoji_entries(&client).await });
        crate::recent_emoji::top_by_count(&entries, n as usize)
    }

    #[cfg(test)]
    pub fn recent_emoji_top(&mut self, _n: u32) -> Vec<String> {
        Vec::new()
    }

    /// Record one use of `glyph` in the user's account-data. Fire-and-forget
    /// against the homeserver: the GET-modify-PUT round-trips would
    /// otherwise stall every emoji click, and the picker's "most used"
    /// ranking tolerates the occasional dropped bump on rapid input
    /// (matrix's last-write-wins account-data semantics merge cleanly on
    /// the next sync). Dual-writes the canonical `m.recent_emoji` and the
    /// unstable `io.github.johennes.msc4356.recent_emoji` so other MSC4356
    /// clients pick the data up regardless of which side has reached
    /// stable yet. The legacy `io.element.recent_emoji` blob is read on
    /// fallback but never written, leaving Element / other clients to
    /// manage their own copy.
    #[cfg(not(test))]
    pub fn recent_emoji_bump(&mut self, glyph: &str) {
        if glyph.is_empty() {
            return;
        }
        let Some(client) = self.client.clone() else {
            return;
        };
        let glyph = glyph.to_owned();
        let ad_lock = Arc::clone(&self.account_data_lock);
        self.rt.spawn(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            use matrix_sdk::ruma::serde::Raw;

            // Serialize the whole GET→modify→PUT against other account-data
            // writers so rapid bumps build on each other instead of racing.
            let _ad_guard = ad_lock.lock().await;
            let entries = read_recent_emoji_entries(&client).await;
            let bumped = crate::recent_emoji::bump(entries, &glyph);
            let content = crate::recent_emoji::serialize_msc4356(&bumped);
            let raw = match Raw::new(&content) {
                Ok(r) => r.cast_unchecked(),
                Err(_) => return,
            };
            // Dual-write to stable + unstable types. Errors are swallowed
            // by the fire-and-forget contract.
            for ty in [
                crate::recent_emoji::TYPE_STABLE,
                crate::recent_emoji::TYPE_UNSTABLE,
            ] {
                let ev_type = GlobalAccountDataEventType::from(ty);
                let _ = client
                    .account()
                    .set_account_data_raw(ev_type, raw.clone())
                    .await;
            }
        });
    }

    #[cfg(test)]
    pub fn recent_emoji_bump(&mut self, _glyph: &str) {}

    // ----- Application prefs (im.gnomos.tesseract global account-data) -----

    #[cfg(not(test))]
    pub fn load_prefs(&mut self) -> String {
        let Some(client) = self.client.clone() else {
            return "{}".to_owned();
        };
        self.rt.block_on(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            let et = GlobalAccountDataEventType::from("im.gnomos.tesseract");
            client
                .account()
                .account_data_raw(et)
                .await
                .ok()
                .flatten()
                .map(|r| r.json().get().to_owned())
                .unwrap_or_else(|| "{}".to_owned())
        })
    }

    #[cfg(test)]
    pub fn load_prefs(&mut self) -> String {
        "{}".to_owned()
    }

    #[cfg(not(test))]
    pub fn save_prefs(&mut self, json: &str) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let json = json.to_owned();
        self.rt.spawn(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            use matrix_sdk::ruma::serde::Raw;
            let Ok(raw_value) = serde_json::from_str::<serde_json::Value>(&json) else {
                return;
            };
            let Ok(raw) = Raw::new(&raw_value) else {
                return;
            };
            let et = GlobalAccountDataEventType::from("im.gnomos.tesseract");
            let _ = client
                .account()
                .set_account_data_raw(et, raw.cast_unchecked())
                .await;
        });
    }

    #[cfg(test)]
    pub fn save_prefs(&mut self, _json: &str) {}

    // ----- MSC4278 media-preview config (m.media_preview_config) -----

    /// Read the global MSC4278 media-preview config from the local sync
    /// cache (stable → unstable precedence). No network roundtrip. Returns
    /// the MSC defaults (previews on, invite avatars on) when not logged in
    /// or before the first sync has populated the cache.
    #[cfg(not(test))]
    pub fn media_preview_config(&mut self) -> crate::ffi::MediaPreviewConfigFfi {
        let Some(client) = self.client.clone() else {
            return crate::ffi::MediaPreviewConfigFfi {
                media_previews: crate::media_preview::MediaPreviews::On.to_u8(),
                invite_avatars: true,
            };
        };
        let cfg = self
            .rt
            .block_on(async move { read_media_preview_config(&client).await });
        crate::ffi::MediaPreviewConfigFfi {
            media_previews: cfg.media_previews.to_u8(),
            invite_avatars: cfg.invite_avatars,
        }
    }

    #[cfg(test)]
    pub fn media_preview_config(&mut self) -> crate::ffi::MediaPreviewConfigFfi {
        crate::ffi::MediaPreviewConfigFfi {
            media_previews: 2,
            invite_avatars: true,
        }
    }

    /// Read a room-level MSC4278 override. `has_media_previews` is true only
    /// when the room's own `m.media_preview_config` account-data event sets
    /// the `media_previews` field, in which case `media_previews` carries the
    /// override; otherwise the caller falls back to the global value. No
    /// network roundtrip — reads the local sync cache.
    #[cfg(not(test))]
    pub fn room_media_preview_override(
        &mut self,
        room_id: &str,
    ) -> crate::ffi::MediaPreviewOverrideFfi {
        fn none() -> crate::ffi::MediaPreviewOverrideFfi {
            crate::ffi::MediaPreviewOverrideFfi {
                has_media_previews: false,
                media_previews: crate::media_preview::MediaPreviews::On.to_u8(),
                join_rule: String::new(),
            }
        }
        let Some(client) = self.client.clone() else {
            return none();
        };
        let Ok(rid) = matrix_sdk::ruma::RoomId::parse(room_id) else {
            return none();
        };
        self.rt.block_on(async move {
            use matrix_sdk::ruma::events::RoomAccountDataEventType;
            use serde_json::Value;

            let Some(room) = client.get_room(&rid) else {
                return none();
            };

            let join_rule = join_rule_str(&room);

            async fn fetch(room: &matrix_sdk::Room, ty: &str) -> Option<Value> {
                let et = RoomAccountDataEventType::from(ty);
                let raw = room.account_data(et).await.ok().flatten()?;
                serde_json::from_str::<Value>(raw.json().get()).ok()
            }

            let v = match fetch(&room, crate::media_preview::TYPE_STABLE).await {
                Some(v) => Some(v),
                None => fetch(&room, crate::media_preview::TYPE_UNSTABLE).await,
            };

            let mp = v
                .as_ref()
                .and_then(crate::media_preview::parse_media_previews_field);
            crate::ffi::MediaPreviewOverrideFfi {
                has_media_previews: mp.is_some(),
                media_previews: mp
                    .unwrap_or(crate::media_preview::MediaPreviews::On)
                    .to_u8(),
                join_rule,
            }
        })
    }

    #[cfg(test)]
    pub fn room_media_preview_override(
        &mut self,
        _room_id: &str,
    ) -> crate::ffi::MediaPreviewOverrideFfi {
        crate::ffi::MediaPreviewOverrideFfi {
            has_media_previews: false,
            media_previews: 2,
            join_rule: String::new(),
        }
    }

    /// Write the global MSC4278 config, dual-writing the stable and unstable
    /// account-data types so other MSC4278 clients pick it up regardless of
    /// which side has reached stable. Fire-and-forget against the homeserver;
    /// the echo arrives on the next sync and triggers
    /// `on_media_preview_config_updated`.
    #[cfg(not(test))]
    pub fn set_media_preview_config(&mut self, media_previews: u8, invite_avatars: bool) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let cfg = crate::media_preview::Config {
            media_previews: crate::media_preview::MediaPreviews::from_u8(media_previews),
            invite_avatars,
        };
        let ad_lock = Arc::clone(&self.account_data_lock);
        self.rt.spawn(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            use matrix_sdk::ruma::serde::Raw;

            let _ad_guard = ad_lock.lock().await;
            let content = crate::media_preview::serialize(cfg);
            let raw = match Raw::new(&content) {
                Ok(r) => r.cast_unchecked(),
                Err(_) => return,
            };
            for ty in [
                crate::media_preview::TYPE_STABLE,
                crate::media_preview::TYPE_UNSTABLE,
            ] {
                let ev_type = GlobalAccountDataEventType::from(ty);
                let _ = client
                    .account()
                    .set_account_data_raw(ev_type, raw.clone())
                    .await;
            }
        });
    }

    #[cfg(test)]
    pub fn set_media_preview_config(&mut self, _media_previews: u8, _invite_avatars: bool) {}

    pub fn user_id(&self) -> String {
        self.client
            .as_ref()
            .and_then(|c| c.user_id())
            .map(|id| id.to_string())
            .unwrap_or_default()
    }

    pub fn current_user_display_name(&self) -> String {
        let Some(client) = self.client.clone() else {
            return String::new();
        };
        self.rt.block_on(async move {
            client
                .account()
                .get_display_name()
                .await
                .ok()
                .flatten()
                .unwrap_or_default()
        })
    }

    pub fn current_user_avatar_url(&self) -> String {
        let Some(client) = self.client.clone() else {
            return String::new();
        };
        self.rt.block_on(async move {
            client
                .account()
                .get_avatar_url()
                .await
                .ok()
                .flatten()
                .map(|u| u.to_string())
                .unwrap_or_default()
        })
    }

    // -----------------------------------------------------------------------
    // Ignored users / profile edits
    // -----------------------------------------------------------------------

    /// Add user_id to m.ignored_user_list account data. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn ignore_user(&mut self, user_id: &str) -> OpResult {
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(user_id) else {
            return err("invalid user id");
        };
        match self.rt.block_on(client.account().ignore_user(&uid)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn ignore_user(&mut self, _user_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Remove user_id from m.ignored_user_list. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn unignore_user(&mut self, user_id: &str) -> OpResult {
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(user_id) else {
            return err("invalid user id");
        };
        match self.rt.block_on(client.account().unignore_user(&uid)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn unignore_user(&mut self, _user_id: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn set_display_name(&mut self, name: &str) -> OpResult {
        let Some(client) = self.client.as_ref() else { return err("not logged in"); };
        match self.rt.block_on(client.account().set_display_name(Some(name))) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn set_display_name(&mut self, _name: &str) -> OpResult { err("not logged in") }

    #[cfg(not(test))]
    pub fn upload_avatar(&mut self, bytes: &[u8], mime_type: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in"); };
        let data = bytes.to_vec();
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(_) => return err(format!("invalid mime type: {mime_type}")),
        };
        match self.rt.block_on(async {
            let mxc = upload_bytes(&client, data, &mime).await?;
            client.account().set_avatar_url(Some(&mxc)).await?;
            Ok::<_, matrix_sdk::Error>(mxc.to_string())
        }) {
            Ok(mxc) => ok(mxc),
            Err(e)  => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn upload_avatar(&mut self, _bytes: &[u8], _mime_type: &str) -> OpResult { err("not logged in") }

    /// Upload bytes to the media server; returns the mxc:// URI in OpResult.message.
    /// Does NOT change the user's global profile avatar.
    #[cfg(not(test))]
    pub fn upload_media(&mut self, bytes: &[u8], mime_type: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in"); };
        let data = bytes.to_vec();
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(_) => return err(format!("invalid mime type: {mime_type}")),
        };
        match self.rt.block_on(upload_bytes(&client, data, &mime)) {
            Ok(mxc) => ok(mxc.to_string()),
            Err(e)  => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn upload_media(&mut self, _bytes: &[u8], _mime_type: &str) -> OpResult { err("not logged in") }

    #[cfg(not(test))]
    pub fn remove_avatar(&mut self) -> OpResult {
        let Some(client) = self.client.as_ref() else { return err("not logged in"); };
        match self.rt.block_on(client.account().set_avatar_url(None)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn remove_avatar(&mut self) -> OpResult { err("not logged in") }

    // -----------------------------------------------------------------------
    // Devices / sessions
    // -----------------------------------------------------------------------

    pub fn device_id(&self) -> String {
        self.client
            .as_ref()
            .and_then(|c| c.device_id())
            .map(|id| id.to_string())
            .unwrap_or_default()
    }

    #[cfg(not(test))]
    pub fn list_devices(&self) -> Vec<crate::ffi::DeviceFfi> {
        let Some(client) = self.client.clone() else { return Vec::new(); };
        let current_device = client.device_id().map(|d| d.to_owned());
        let user_id = client.user_id().map(|u| u.to_owned());
        self.rt.block_on(async move {
            let response = match client.devices().await {
                Ok(r) => r,
                Err(e) => {
                    tracing::warn!("list_devices: {e}");
                    return Vec::new();
                }
            };
            let mut out = Vec::with_capacity(response.devices.len());
            for d in response.devices {
                let id_str = d.device_id.to_string();
                let verification = if let Some(uid) = user_id.as_deref() {
                    match client.encryption().get_device(uid, &d.device_id).await {
                        Ok(Some(dev)) => if dev.is_verified() { 2u8 } else { 1u8 },
                        Ok(None) => 0u8,
                        Err(_) => 0u8,
                    }
                } else {
                    0u8
                };
                let is_current = current_device
                    .as_ref()
                    .map(|c| c.as_str() == id_str.as_str())
                    .unwrap_or(false);
                out.push(crate::ffi::DeviceFfi {
                    device_id: id_str,
                    display_name: d.display_name.unwrap_or_default(),
                    last_seen_ip: d.last_seen_ip.unwrap_or_default(),
                    last_seen_ts: d
                        .last_seen_ts
                        .map(|ts| u64::from(ts.get()))
                        .unwrap_or(0),
                    verification_state: verification,
                    is_current,
                });
            }
            // Current device first; then by last_seen_ts desc; stable on ties.
            out.sort_by(|a, b| {
                b.is_current
                    .cmp(&a.is_current)
                    .then_with(|| b.last_seen_ts.cmp(&a.last_seen_ts))
            });
            out
        })
    }
    #[cfg(test)]
    pub fn list_devices(&self) -> Vec<crate::ffi::DeviceFfi> { Vec::new() }

    #[cfg(not(test))]
    pub fn set_device_display_name(&mut self, device_id: &str, name: &str) -> OpResult {
        use matrix_sdk::ruma::OwnedDeviceId;
        let Some(client) = self.client.clone() else { return err("not logged in"); };
        let owned: OwnedDeviceId = device_id.into();
        let trimmed = name.to_owned();
        let result = self
            .rt
            .block_on(async move { client.rename_device(&owned, &trimmed).await });
        match result {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn set_device_display_name(&mut self, _device_id: &str, _name: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn begin_delete_device(&mut self, device_id: &str) -> crate::ffi::DeleteDeviceBegin {
        use matrix_sdk::ruma::OwnedDeviceId;
        let Some(client) = self.client.clone() else {
            return crate::ffi::DeleteDeviceBegin {
                ok: false,
                message: "not logged in".into(),
                needs_uia: false,
                fallback_url: String::new(),
                session: String::new(),
            };
        };
        let homeserver = client.homeserver().to_string();
        let owned: OwnedDeviceId = device_id.into();
        self.rt.block_on(async move {
            match client.delete_devices(&[owned], None).await {
                Ok(_) => crate::ffi::DeleteDeviceBegin {
                    ok: true,
                    message: String::new(),
                    needs_uia: false,
                    fallback_url: String::new(),
                    session: String::new(),
                },
                Err(http_err) => {
                    if let Some(info) = http_err.as_uiaa_response() {
                        let session = info.session.clone().unwrap_or_default();
                        let stage = info
                            .flows
                            .first()
                            .and_then(|f| f.stages.first())
                            .map(|s| s.as_ref().to_owned())
                            .unwrap_or_default();
                        let fallback_url = build_uia_fallback_url(
                            &homeserver,
                            &stage,
                            &session,
                        );
                        crate::ffi::DeleteDeviceBegin {
                            ok: true,
                            message: String::new(),
                            needs_uia: true,
                            fallback_url,
                            session,
                        }
                    } else {
                        crate::ffi::DeleteDeviceBegin {
                            ok: false,
                            message: http_err.to_string(),
                            needs_uia: false,
                            fallback_url: String::new(),
                            session: String::new(),
                        }
                    }
                }
            }
        })
    }
    #[cfg(test)]
    pub fn begin_delete_device(&mut self, _device_id: &str) -> crate::ffi::DeleteDeviceBegin {
        crate::ffi::DeleteDeviceBegin {
            ok: false,
            message: "not logged in".into(),
            needs_uia: false,
            fallback_url: String::new(),
            session: String::new(),
        }
    }

    #[cfg(not(test))]
    pub fn complete_delete_device(&mut self, device_id: &str, session: &str) -> OpResult {
        use matrix_sdk::ruma::api::client::uiaa;
        use matrix_sdk::ruma::OwnedDeviceId;
        let Some(client) = self.client.clone() else { return err("not logged in"); };
        let owned: OwnedDeviceId = device_id.into();
        let auth = uiaa::AuthData::FallbackAcknowledgement(
            uiaa::FallbackAcknowledgement::new(session.to_owned()),
        );
        match self.rt.block_on(client.delete_devices(&[owned], Some(auth))) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn complete_delete_device(&mut self, _device_id: &str, _session: &str) -> OpResult {
        err("not logged in")
    }

    // -----------------------------------------------------------------------
    // Presence
    // -----------------------------------------------------------------------

    #[cfg(not(test))]
    pub fn set_presence(&mut self, state: u8) -> OpResult {
        use matrix_sdk::ruma::api::client::presence::set_presence::v3;
        use matrix_sdk::ruma::presence::PresenceState;
        let presence = match state {
            1 => PresenceState::Online,
            2 => PresenceState::Unavailable,
            3 => PresenceState::Offline,
            _ => return err(format!("invalid presence state {state}")),
        };
        let Some(client) = self.client.clone() else { return err("not logged in"); };
        let Some(user_id) = client.user_id().map(|u| u.to_owned()) else {
            return err("not logged in");
        };
        let result = self.rt.block_on(async move {
            let req = v3::Request::new(user_id, presence);
            client.send(req).await
        });
        match result {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
    #[cfg(test)]
    pub fn set_presence(&mut self, state: u8) -> OpResult {
        if !matches!(state, 1 | 2 | 3) {
            return err(format!("invalid presence state {state}"));
        }
        err("not logged in")
    }

    /// Return room ID of an existing DM with user_id, or create one.
    /// Returns empty string on error. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn get_or_create_dm(&mut self, user_id: &str) -> String {
        let Some(client) = self.client.as_ref() else {
            return String::new();
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(user_id) else {
            return String::new();
        };
        self.rt.block_on(async {
            // Look for an existing DM with this user. Functional members
            // (bridge bots, per MSC4171) are excluded from the member count
            // so a bridged 1:1 (you + bot + puppet) still matches as a DM.
            for room in client.joined_rooms() {
                if room.is_direct().await.unwrap_or(false) {
                    if let Ok(members) =
                        room.members(matrix_sdk::RoomMemberships::JOIN).await
                    {
                        let functional = room.service_members().unwrap_or_default();
                        let ids: Vec<_> = members
                            .iter()
                            .map(|m| m.user_id())
                            .filter(|id| !functional.contains(*id))
                            .collect();
                        if ids.len() == 2
                            && ids.iter().any(|id| *id == uid)
                            && client
                                .user_id()
                                .is_some_and(|me| ids.iter().any(|id| *id == me))
                        {
                            return room.room_id().to_string();
                        }
                    }
                }
            }
            // Create a new DM
            match client.create_dm(&uid).await {
                Ok(room) => room.room_id().to_string(),
                Err(_) => String::new(),
            }
        })
    }

    #[cfg(test)]
    pub fn get_or_create_dm(&mut self, _user_id: &str) -> String {
        String::new()
    }

    // -----------------------------------------------------------------------
    // Homeserver discovery
    // -----------------------------------------------------------------------

    // Fetch .well-known/matrix/client and return m.homeserver.base_url, or
    // None if the server returned non-2xx or the key is absent.
    #[cfg(not(test))]
    async fn fetch_well_known(http: &reqwest::Client, server: &str) -> Option<String> {
        let url = format!("https://{}/.well-known/matrix/client", server);
        let resp = http.get(&url).send().await.ok()?;
        if !resp.status().is_success() {
            return None;
        }
        let body: serde_json::Value = resp.json().await.ok()?;
        let base = body["m.homeserver"]["base_url"]
            .as_str()?
            .trim_end_matches('/')
            .to_owned();
        Some(base)
    }

    // Confirm the candidate base URL actually speaks Matrix by hitting
    // /_matrix/client/versions and expecting any 2xx response.
    #[cfg(not(test))]
    async fn validate_homeserver(http: &reqwest::Client, base_url: &str) -> bool {
        let url = format!("{}/_matrix/client/versions", base_url);
        http.get(&url)
            .send()
            .await
            .map(|r| r.status().is_success())
            .unwrap_or(false)
    }

    /// Discover the homeserver base URL for a server name or Matrix ID.
    /// Returns JSON: `{"base_url":"https://...","error":""}` on success or
    /// `{"base_url":"","error":"..."}` on failure. Uses raw HTTP — no SDK
    /// Client construction required.
    #[cfg(not(test))]
    pub fn discover_homeserver(&mut self, server_name_or_mxid: &str) -> String {
        let input = server_name_or_mxid.trim();

        // Extract server name from a full MXID (@user:server.org → server.org).
        let server = if input.starts_with('@') {
            match input.find(':') {
                Some(i) => &input[i + 1..],
                None => {
                    return r#"{"base_url":"","error":"Invalid Matrix ID — expected @user:server"}"#
                        .to_owned()
                }
            }
        } else {
            input
        };

        if server.is_empty() {
            return r#"{"base_url":"","error":""}"#.to_owned();
        }

        let server = server.to_owned();
        self.rt.block_on(async move {
            let http = match reqwest::Client::builder()
                .timeout(std::time::Duration::from_secs(5))
                .build()
            {
                Ok(c) => c,
                Err(e) => {
                    let msg = e
                        .to_string()
                        .replace('\\', "\\\\")
                        .replace('"', "\\\"")
                        .replace('\n', " ");
                    return format!(r#"{{"base_url":"","error":"{msg}"}}"#);
                }
            };

            let base_url = if server.starts_with("https://") || server.starts_with("http://") {
                // Caller passed a full URL — validate it directly.
                let candidate = server.trim_end_matches('/').to_owned();
                if Self::validate_homeserver(&http, &candidate).await {
                    Some(candidate)
                } else {
                    None
                }
            } else {
                // Try .well-known first; fall back to https://{server} on failure.
                let candidate = Self::fetch_well_known(&http, &server)
                    .await
                    .unwrap_or_else(|| format!("https://{}", server));
                if Self::validate_homeserver(&http, &candidate).await {
                    Some(candidate)
                } else {
                    None
                }
            };

            match base_url {
                Some(url) => format!(r#"{{"base_url":"{url}","error":""}}"#),
                None => {
                    let msg = format!("Could not reach homeserver at {server}");
                    format!(r#"{{"base_url":"","error":"{msg}"}}"#)
                }
            }
        })
    }

    #[cfg(test)]
    pub fn discover_homeserver(&mut self, _: &str) -> String {
        r#"{"base_url":"","error":""}"#.to_owned()
    }
}
