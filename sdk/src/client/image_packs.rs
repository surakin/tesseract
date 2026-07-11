//! MSC2545 image-pack listings, favourites, and personal-pack mutations.
//!
//! `send_sticker` / `send_thread_sticker` remain in `mod.rs` for now —
//! they belong to a future `send.rs` extraction.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{err, ok, ClientFfi};
#[cfg(test)]
use super::{err, ClientFfi};

#[cfg(not(test))]
use std::sync::Arc;

impl ClientFfi {
    // -----------------------------------------------------------------------
    // MSC2545 image packs (Step 8)
    // -----------------------------------------------------------------------

    /// Snapshot of every MSC2545 image pack the client currently knows about.
    /// Reads the in-memory `image_packs` cache populated by the sync watcher
    /// and the explicit `refresh_image_packs_async` rebuilds; no network
    /// roundtrip.
    #[cfg(not(test))]
    pub fn list_image_packs(&self) -> Vec<crate::ffi::ImagePackFfi> {
        let cache = self.image_packs.lock();
        cache
            .iter()
            .map(|p| crate::ffi::ImagePackFfi {
                id: p.id.clone(),
                display_name: p.display_name.clone(),
                avatar_url: p.avatar_url.clone(),
                attribution: p.attribution.clone(),
                usage_mask: p.usage,
                source_kind: p.source_kind().to_owned(),
                source_room: p.source_room().to_owned(),
                source_state_key: p.source_state_key().to_owned(),
                is_subscribed: p.is_subscribed,
            })
            .collect()
    }

    #[cfg(test)]
    pub fn list_image_packs(&self) -> Vec<crate::ffi::ImagePackFfi> {
        Vec::new()
    }

    /// Every room-sourced MSC2545 pack known so far, for the "Known Packs"
    /// settings page (browse rooms with a pack, subscribe/unsubscribe). A
    /// fast local read — no network I/O: rooms are discovered lazily as the
    /// user visits them (or as soon as they're in `m.image_pack.rooms` /
    /// `im.ponies.emote_rooms`, prefilled at session start), and each
    /// discovery persists to the `room_image_pack_cache` table in
    /// `app_cache.db` via `rebuild_image_packs`. `is_subscribed` is
    /// recomputed fresh from live account data on every call since it can
    /// change independently of whether a room still has a pack.
    #[cfg(not(test))]
    pub fn list_known_room_packs(&self) -> Vec<crate::ffi::ImagePackFfi> {
        let cached = {
            let db = self.app_cache_db.lock();
            db.as_ref()
                .map(super::backfill::read_known_room_packs)
                .unwrap_or_default()
        };
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        let subscribed_set = self.rt.block_on(super::collect_subscribed_room_refs(&client));
        cached
            .into_iter()
            .map(|(room_id, state_key, display_name)| {
                let is_subscribed = subscribed_set.contains(&(room_id.clone(), state_key.clone()));
                let id = crate::image_packs::pack_id_for(&crate::image_packs::PackSource::Room {
                    room_id: room_id.clone(),
                    state_key: state_key.clone(),
                });
                crate::ffi::ImagePackFfi {
                    id,
                    display_name,
                    avatar_url: String::new(),
                    attribution: String::new(),
                    usage_mask: crate::image_packs::USAGE_ANY,
                    source_kind: "room".to_owned(),
                    source_room: room_id,
                    source_state_key: state_key,
                    is_subscribed,
                }
            })
            .collect()
    }

    #[cfg(test)]
    pub fn list_known_room_packs(&self) -> Vec<crate::ffi::ImagePackFfi> {
        Vec::new()
    }

    /// Return every entry in `pack_id` whose usage mask intersects
    /// `usage_filter` ("sticker" | "emoticon" | "any" — anything else is
    /// treated as "any"). When `pack_id` doesn't exist, returns empty.
    #[cfg(not(test))]
    pub fn list_pack_images(
        &self,
        pack_id: &str,
        usage_filter: &str,
    ) -> Vec<crate::ffi::ImageEntryFfi> {
        let needed = match usage_filter {
            "sticker" => crate::image_packs::USAGE_STICKER,
            "emoticon" => crate::image_packs::USAGE_EMOTICON,
            _ => crate::image_packs::USAGE_ANY,
        };
        let cache = self.image_packs.lock();
        let Some(pack) = cache.iter().find(|p| p.id == pack_id) else {
            return Vec::new();
        };
        pack.images
            .iter()
            .filter(|e| e.usage & needed != 0)
            .map(|e| image_entry_to_ffi(&pack.id, e))
            .collect()
    }

    #[cfg(test)]
    pub fn list_pack_images(
        &self,
        _pack_id: &str,
        _usage_filter: &str,
    ) -> Vec<crate::ffi::ImageEntryFfi> {
        Vec::new()
    }

    /// Flatten every favourite-marked entry across all packs. Sticker-usage
    /// only (Favorites tab is sticker-specific).
    #[cfg(not(test))]
    pub fn list_favorite_stickers(&self) -> Vec<crate::ffi::ImageEntryFfi> {
        let cache = self.image_packs.lock();
        let mut out = Vec::new();
        for pack in cache.iter() {
            for e in &pack.images {
                if e.favorite && (e.usage & crate::image_packs::USAGE_STICKER != 0) {
                    out.push(image_entry_to_ffi(&pack.id, e));
                }
            }
        }
        out
    }

    #[cfg(test)]
    pub fn list_favorite_stickers(&self) -> Vec<crate::ffi::ImageEntryFfi> {
        Vec::new()
    }

    /// Add a sticker to the user's MSC2545 personal pack
    /// (`im.ponies.user_emotes`). Reads the current content (creating an
    /// empty object on first use), inserts the new entry under a
    /// collision-free shortcode derived from `shortcode` or `body`, writes
    /// the result back via `set_account_data_raw`, then triggers a local
    /// cache rebuild.
    #[cfg(not(test))]
    pub fn save_sticker_to_user_pack(
        &self,
        shortcode: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "image_packs/save".to_string(),
        );
        // Encrypted sticker events serialise the full MediaSource as JSON.
        // Decrypt and re-upload as unencrypted media so the saved pack entry
        // is viewable by any client without the original encryption key.
        let image_url_owned;
        // First 8 chars of the original encrypted media ID, used as a
        // collision-resistant shortcode base when body is empty.
        let mut encrypted_media_id: Option<String> = None;
        let image_url = if image_url.starts_with('{') {
            use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
            use matrix_sdk::ruma::events::room::MediaSource;
            let ms: MediaSource = match serde_json::from_str(image_url) {
                Ok(v) => v,
                Err(_) => return err("image_url is not a valid mxc:// uri"),
            };
            match ms {
                MediaSource::Plain(uri) => {
                    image_url_owned = uri.to_string();
                }
                MediaSource::Encrypted(file) => {
                    encrypted_media_id = Some(
                        file.url
                            .as_str()
                            .rsplit('/')
                            .next()
                            .unwrap_or("")
                            .chars()
                            .take(8)
                            .collect(),
                    );

                    let mime: mime::Mime = serde_json::from_str::<serde_json::Value>(info_json)
                        .ok()
                        .and_then(|v| v.get("mimetype")?.as_str()?.parse().ok())
                        .unwrap_or(mime::APPLICATION_OCTET_STREAM);

                    let client_dl = client.clone();
                    let bytes = self.rt.block_on(async move {
                        client_dl
                            .media()
                            .get_media_content(
                                &MediaRequestParameters {
                                    source: serde_json::from_str(image_url).unwrap(),
                                    format: MediaFormat::File,
                                },
                                true,
                            )
                            .await
                    });
                    let bytes = match bytes {
                        Ok(b) => b,
                        Err(e) => return err(format!("download encrypted sticker: {e}")),
                    };

                    let client_ul = client.clone();
                    let upload = self.rt.block_on(async move {
                        client_ul.media().upload(&mime, bytes, None).await
                    });
                    let response = match upload {
                        Ok(r) => r,
                        Err(e) => return err(format!("re-upload sticker: {e}")),
                    };
                    image_url_owned = response.content_uri.to_string();
                }
            }
            image_url_owned.as_str()
        } else {
            image_url
        };
        let uri = matrix_sdk::ruma::OwnedMxcUri::from(image_url);
        if !uri.is_valid() {
            return err("image_url is not a valid mxc:// uri");
        }

        let ev_type = GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);

        // Hold the account-data lock across the whole read→modify→write so a
        // concurrent toggle_favorite / save cannot clobber this change.
        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        let client_for_read = client.clone();
        let read_result = self.rt.block_on(async move {
            client_for_read
                .account()
                .account_data_raw(ev_type.clone())
                .await
        });

        // Refuse to proceed if the existing pack content fails to parse:
        // upserting onto Value::Object({}) and writing it back would silently
        // overwrite the user's whole sticker pack with `{}`. A transient
        // server-side corruption or future schema we don't understand must
        // not destroy the user's data — surface the error instead.
        let current_content: Value = match read_result {
            Ok(Some(raw)) => match serde_json::from_str(raw.json().get()) {
                Ok(v) => v,
                Err(e) => {
                    return err(format!(
                        "existing user pack failed to parse — refusing to \
                         overwrite (would destroy your saved stickers): {e}"
                    ))
                }
            },
            Ok(None) => Value::Object(serde_json::Map::new()),
            Err(e) => return err(format!("read user pack: {e}")),
        };

        // Compute final shortcode (collision-free) against the existing
        // images map so re-saving the same sticker doesn't shadow a
        // pre-existing entry.
        let existing_images = current_content
            .get("images")
            .and_then(Value::as_object)
            .cloned()
            .unwrap_or_default();

        // For encrypted stickers, derive a deterministic shortcode base from
        // the network name (fi.mau.bridged_sticker.network in info_json) and
        // the first 8 chars of the original encrypted media ID, e.g.
        // "whatsapp_fa96dd5e".  The same sticker always produces the same
        // base, so we can use it as a duplicate key alongside the mxc URL.
        let derived_base: Option<String> = encrypted_media_id.as_deref().map(|media_id| {
            let network = serde_json::from_str::<Value>(info_json).ok().and_then(|v| {
                v.get("fi.mau.bridged_sticker")?
                    .get("network")?
                    .as_str()
                    .map(str::to_owned)
            });
            match network {
                Some(n) => format!("{n}_{media_id}"),
                None => media_id.to_owned(),
            }
        });

        // Already saved? Check by shortcode base (encrypted re-upload gives a
        // new mxc URI each time) and by mxc URL as fallback.
        let already_saved = derived_base
            .as_deref()
            .map(|b| existing_images.contains_key(b))
            .unwrap_or(false)
            || crate::image_packs::pack_contains_url(&current_content, image_url);
        if already_saved {
            self.update_user_pack_in_cache(&current_content);
            return ok("");
        }

        let base = derived_base.as_deref().unwrap_or(if shortcode.is_empty() {
            body
        } else {
            shortcode
        });
        let final_shortcode = crate::image_packs::suggest_shortcode(base, &existing_images);

        let new_content = crate::image_packs::upsert_image_into_user_pack(
            current_content,
            &final_shortcode,
            image_url,
            body,
            info_json,
            None,
            "Saved Stickers",
        );

        let raw = match Raw::new(&new_content) {
            Ok(r) => r.cast_unchecked(),
            Err(e) => return err(format!("serialize user pack: {e}")),
        };

        let client_for_write = client.clone();
        let ev_type_for_write =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        let write_result = self.rt.block_on(async move {
            client_for_write
                .account()
                .set_account_data_raw(ev_type_for_write, raw)
                .await
        });

        match write_result {
            Ok(_) => {
                // Set the pending flag BEFORE updating the cache so the sync
                // watcher cannot observe updated-cache + pending=false in between.
                self.user_pack_write_pending
                    .store(true, std::sync::atomic::Ordering::Release);
                // Directly update the in-memory cache from the new_content we
                // already have — the state store won't reflect the write until
                // the next sync cycle, so rebuild_image_packs would read stale
                // data if we called refresh_image_packs_blocking here.
                self.update_user_pack_in_cache(&new_content);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn save_sticker_to_user_pack(
        &self,
        _shortcode: &str,
        _body: &str,
        _image_url: &str,
        _info_json: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn user_pack_has_sticker(&self, image_url: &str, info_json: &str) -> bool {
        if image_url.is_empty() {
            return false;
        }
        let cache = self.image_packs.lock();
        // For encrypted stickers, image_url is a JSON-encoded MediaSource.
        // Derive the same deterministic shortcode base that save_sticker_to_user_pack
        // uses so the lookup matches even though the re-uploaded mxc URI differs.
        if image_url.starts_with('{') {
            use matrix_sdk::ruma::events::room::MediaSource;
            if let Ok(MediaSource::Encrypted(file)) = serde_json::from_str::<MediaSource>(image_url)
            {
                let encrypted_media_id: String = file
                    .url
                    .as_str()
                    .rsplit('/')
                    .next()
                    .unwrap_or("")
                    .chars()
                    .take(8)
                    .collect();
                let network = serde_json::from_str::<serde_json::Value>(info_json)
                    .ok()
                    .and_then(|v| {
                        v.get("fi.mau.bridged_sticker")?
                            .get("network")?
                            .as_str()
                            .map(str::to_owned)
                    });
                let derived_base = match network {
                    Some(n) => format!("{n}_{encrypted_media_id}"),
                    None => encrypted_media_id,
                };
                return cache
                    .iter()
                    .filter(|p| matches!(p.source, crate::image_packs::PackSource::User))
                    .flat_map(|p| p.images.iter())
                    .any(|e| e.shortcode == derived_base);
            }
        }
        cache
            .iter()
            .filter(|p| matches!(p.source, crate::image_packs::PackSource::User))
            .flat_map(|p| p.images.iter())
            .any(|e| e.url == image_url)
    }

    #[cfg(test)]
    pub fn user_pack_has_sticker(&self, _image_url: &str, _info_json: &str) -> bool {
        false
    }

    #[cfg(not(test))]
    pub fn toggle_favorite_sticker(&self, image_url: &str) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "image_packs/fetch".to_string(),
        );

        let ev_type = GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);

        // Hold the account-data lock across the whole read→modify→write so a
        // concurrent save / favorite toggle cannot clobber this change.
        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        let client_for_read = client.clone();
        let read_result = self
            .rt
            .block_on(async move { client_for_read.account().account_data_raw(ev_type).await });

        // Same data-preservation guard as save_sticker_to_user_pack: refuse
        // to write if the existing content failed to parse, otherwise the
        // toggle would silently overwrite the whole pack with `{}`.
        let current: Value = match read_result {
            Ok(Some(raw)) => match serde_json::from_str(raw.json().get()) {
                Ok(v) => v,
                Err(e) => {
                    return err(format!(
                        "existing user pack failed to parse — refusing to \
                         overwrite (would destroy your saved stickers): {e}"
                    ))
                }
            },
            Ok(None) => return err("sticker is not saved; add it before favoriting"),
            Err(e) => return err(format!("read user pack: {e}")),
        };

        let (new_content, _new_state) =
            crate::image_packs::toggle_favorite_in_user_pack(current, image_url);

        let raw = match Raw::new(&new_content) {
            Ok(r) => r.cast_unchecked(),
            Err(e) => return err(format!("serialize user pack: {e}")),
        };
        let client_for_write = client.clone();
        let ev_type_for_write =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        let write_result = self.rt.block_on(async move {
            client_for_write
                .account()
                .set_account_data_raw(ev_type_for_write, raw)
                .await
        });

        match write_result {
            Ok(_) => {
                self.user_pack_write_pending
                    .store(true, std::sync::atomic::Ordering::Release);
                self.update_user_pack_in_cache(&new_content);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn toggle_favorite_sticker(&self, _image_url: &str) -> OpResult {
        err("not logged in")
    }

    /// Remove `shortcode` from the user's personal pack
    /// (`im.ponies.user_emotes`). No-op (ok:true) if the shortcode doesn't
    /// exist. Same read→modify→write skeleton as `save_sticker_to_user_pack`.
    #[cfg(not(test))]
    pub fn remove_user_pack_image(&self, shortcode: &str) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "image_packs/remove".to_string(),
        );

        let ev_type = GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        let client_for_read = client.clone();
        let read_result = self
            .rt
            .block_on(async move { client_for_read.account().account_data_raw(ev_type).await });

        let current: Value = match read_result {
            Ok(Some(raw)) => match serde_json::from_str(raw.json().get()) {
                Ok(v) => v,
                Err(e) => {
                    return err(format!(
                        "existing user pack failed to parse — refusing to \
                         overwrite (would destroy your saved stickers): {e}"
                    ))
                }
            },
            Ok(None) => return ok(""),
            Err(e) => return err(format!("read user pack: {e}")),
        };

        let new_content = crate::image_packs::remove_image_from_user_pack(current, shortcode);

        let raw = match Raw::new(&new_content) {
            Ok(r) => r.cast_unchecked(),
            Err(e) => return err(format!("serialize user pack: {e}")),
        };
        let client_for_write = client.clone();
        let ev_type_for_write =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        let write_result = self.rt.block_on(async move {
            client_for_write
                .account()
                .set_account_data_raw(ev_type_for_write, raw)
                .await
        });

        match write_result {
            Ok(_) => {
                self.user_pack_write_pending
                    .store(true, std::sync::atomic::Ordering::Release);
                self.update_user_pack_in_cache(&new_content);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn remove_user_pack_image(&self, _shortcode: &str) -> OpResult {
        err("not logged in")
    }

    /// Rename `old_shortcode` to `new_shortcode` in the user's personal pack.
    /// If `new_shortcode` collides with an existing entry, a numeric suffix
    /// is appended (mirrors `save_sticker_to_user_pack`'s collision
    /// handling); the resolved shortcode is reported in the result message
    /// on success so the caller can tell whether a suffix was applied.
    #[cfg(not(test))]
    pub fn rename_user_pack_image(&self, old_shortcode: &str, new_shortcode: &str) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "image_packs/rename".to_string(),
        );

        let ev_type = GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        let client_for_read = client.clone();
        let read_result = self
            .rt
            .block_on(async move { client_for_read.account().account_data_raw(ev_type).await });

        let current: Value = match read_result {
            Ok(Some(raw)) => match serde_json::from_str(raw.json().get()) {
                Ok(v) => v,
                Err(e) => {
                    return err(format!(
                        "existing user pack failed to parse — refusing to \
                         overwrite (would destroy your saved stickers): {e}"
                    ))
                }
            },
            Ok(None) => return err("shortcode does not exist"),
            Err(e) => return err(format!("read user pack: {e}")),
        };

        let (new_content, applied_shortcode) =
            crate::image_packs::rename_image_in_user_pack(current, old_shortcode, new_shortcode);
        if applied_shortcode == old_shortcode && old_shortcode != new_shortcode {
            return err("shortcode does not exist");
        }

        let raw = match Raw::new(&new_content) {
            Ok(r) => r.cast_unchecked(),
            Err(e) => return err(format!("serialize user pack: {e}")),
        };
        let client_for_write = client.clone();
        let ev_type_for_write =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        let write_result = self.rt.block_on(async move {
            client_for_write
                .account()
                .set_account_data_raw(ev_type_for_write, raw)
                .await
        });

        match write_result {
            Ok(_) => {
                self.user_pack_write_pending
                    .store(true, std::sync::atomic::Ordering::Release);
                self.update_user_pack_in_cache(&new_content);
                ok(applied_shortcode)
            }
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn rename_user_pack_image(&self, _old_shortcode: &str, _new_shortcode: &str) -> OpResult {
        err("not logged in")
    }

    /// Explicitly subscribe/unsubscribe `(room_id, state_key)`'s image pack
    /// via the user's `m.image_pack.rooms` / `im.ponies.emote_rooms` account
    /// data (dual-written — both event types are read, modified, and written
    /// independently so a partially-migrated account never loses the other
    /// copy's entries). Forces a synchronous aggregator rebuild before
    /// returning so `ImagePack::is_subscribed` reflects the change
    /// immediately, matching `save_sticker_to_user_pack`'s no-round-trip-lag
    /// guarantee for the personal pack.
    #[cfg(not(test))]
    pub fn set_pack_room_subscribed(
        &self,
        room_id: &str,
        state_key: &str,
        subscribed: bool,
    ) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "image_packs/subscribe".to_string(),
        );

        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        for ev_type_str in crate::image_packs::EMOTE_ROOMS_TYPES {
            let ev_type = GlobalAccountDataEventType::from(ev_type_str);
            let client_for_read = client.clone();
            let ev_type_for_read = ev_type.clone();
            let read_result = self.rt.block_on(async move {
                client_for_read
                    .account()
                    .account_data_raw(ev_type_for_read)
                    .await
            });
            let current_content: Value = match read_result {
                Ok(Some(raw)) => match serde_json::from_str(raw.json().get()) {
                    Ok(v) => v,
                    Err(e) => {
                        return err(format!(
                            "existing pack subscriptions ({ev_type_str}) failed to \
                             parse — refusing to overwrite: {e}"
                        ))
                    }
                },
                Ok(None) => Value::Object(serde_json::Map::new()),
                Err(e) => return err(format!("read {ev_type_str}: {e}")),
            };

            let new_content = if subscribed {
                crate::image_packs::add_room_pack_subscription(current_content, room_id, state_key)
            } else {
                crate::image_packs::remove_room_pack_subscription(
                    current_content,
                    room_id,
                    state_key,
                )
            };

            let raw = match Raw::new(&new_content) {
                Ok(r) => r.cast_unchecked(),
                Err(e) => return err(format!("serialize {ev_type_str}: {e}")),
            };
            let client_for_write = client.clone();
            let ev_type_for_write = ev_type.clone();
            let write_result = self.rt.block_on(async move {
                client_for_write
                    .account()
                    .set_account_data_raw(ev_type_for_write, raw)
                    .await
            });
            if let Err(e) = write_result {
                return err(format!("write {ev_type_str}: {e}"));
            }
        }

        self.refresh_image_packs_blocking();
        ok("")
    }

    #[cfg(test)]
    pub fn set_pack_room_subscribed(
        &self,
        _room_id: &str,
        _state_key: &str,
        _subscribed: bool,
    ) -> OpResult {
        err("not logged in")
    }

    /// Synchronously rebuild the aggregated image-pack cache and replace it
    /// in place, firing `on_image_packs_updated` — used after a subscription
    /// change so `ImagePack::is_subscribed` is correct before the call
    /// returns, rather than waiting for the next sync-driven rebuild.
    #[cfg(not(test))]
    fn refresh_image_packs_blocking(&self) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let mut http_cache: std::collections::HashMap<
            (matrix_sdk::ruma::OwnedRoomId, String),
            Option<serde_json::Value>,
        > = std::collections::HashMap::new();
        let active_rooms_snapshot = self.active_rooms.lock().clone();
        let app_cache_db = Arc::clone(&self.app_cache_db);
        let room_state_cache = Arc::clone(&self.room_state_cache);
        let packs = self.rt.block_on(async move {
            super::rebuild_image_packs(&client, &mut http_cache, &active_rooms_snapshot, &app_cache_db, &room_state_cache).await
        });
        {
            let mut cache = self.image_packs.lock();
            *cache = packs;
        }
        if let Some(h) = &self.handler {
            let g = h.lock();
            g.on_image_packs_updated();
        }
    }

    /// Update only the user pack slot in the in-memory cache from `content`
    /// (already in memory — no state-store round-trip). Fires
    /// `on_image_packs_updated` so the UI refreshes immediately.
    #[cfg(not(test))]
    fn update_user_pack_in_cache(&self, content: &serde_json::Value) {
        let Some(mut pack) = crate::image_packs::parse_pack_content(
            "user".to_owned(),
            crate::image_packs::PackSource::User,
            content,
        ) else {
            return;
        };
        if pack.display_name.is_empty() {
            pack.display_name = "Saved Stickers".to_owned();
        }
        {
            let mut cache = self.image_packs.lock();
            if let Some(slot) = cache
                .iter_mut()
                .find(|p| p.source == crate::image_packs::PackSource::User)
            {
                *slot = pack;
            } else {
                cache.insert(0, pack);
            }
        }
        if let Some(h) = &self.handler {
            {
                let g = h.lock();
                g.on_image_packs_updated();
            }
        }
    }
}

/// Convert an `ImageEntry` from the cache to the FFI shape, attaching the
/// pack id so the UI can group rows back to packs without re-traversing the
/// cache.
#[cfg(not(test))]
pub(super) fn image_entry_to_ffi(
    pack_id: &str,
    e: &crate::image_packs::ImageEntry,
) -> crate::ffi::ImageEntryFfi {
    crate::ffi::ImageEntryFfi {
        pack_id: pack_id.to_owned(),
        shortcode: e.shortcode.clone(),
        url: e.url.clone(),
        body: e.body.clone(),
        info_json: e.info_json.clone(),
        usage_mask: e.usage,
        favorite: e.favorite,
    }
}
