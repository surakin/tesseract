//! Long-lived sync orchestration extracted from `mod.rs`.
//!
//! `start_sync` spawns every background watcher task the running session
//! needs (session-refresh, notification handlers, presence polling, room/pack
//! watcher, recovery/backup/imported-key watchers, room-list state watcher,
//! verification state + request handlers, and the SyncService monitor with
//! reconnect backoff). `stop_sync` tears them all down. The presence-polling
//! toggle lives here too because the watcher it gates is set up by `start_sync`.

#![cfg(not(test))]

use super::*;

use matrix_sdk::SessionChange;
use matrix_sdk_ui::sync_service::SyncService;

impl ClientFfi {
    /// Spawn a long-lived sync task and record its abort handle so
    /// `stop_sync` can cancel it before the C++ handler is destroyed.
    fn spawn_tracked<F>(&mut self, fut: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        let h = self.rt.spawn(fut).abort_handle();
        self.sync_tasks.push(h);
    }

    pub fn start_sync(&mut self, handler: UniquePtr<EventHandlerBridge>) {
        let Some(client) = self.client.clone() else {
            return;
        };

        let (stop_tx, stop_rx) = watch::channel(false);
        let stop_tx_auth = stop_tx.clone();
        self.stop_rx = Some(stop_rx.clone());
        self.stop_tx = Some(stop_tx);

        let handler = Arc::new(Mutex::new(SendHandler(handler)));
        self.handler = Some(Arc::clone(&handler));

        // Subscribe the event cache before SyncService starts so every sync
        // response is persisted to SQLite from the very first update.
        // subscribe() is synchronous but internally calls tokio::task::spawn,
        // so it must be called within the runtime context.
        let _rt_guard = self.rt.enter();
        if let Err(e) = client.event_cache().subscribe() {
            tracing::error!("event cache subscribe failed: {e}");
            {
                let guard = handler.lock();
                guard.on_error("event_cache_init", &e.to_string(), false);
            }
            return;
        }
        drop(_rt_guard);

        // Session refresh watcher.
        {
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            self.spawn_tracked(watch_session_changes(h, client_clone, stop_tx_auth));
        }

        // Wall-clock start of this sync session, used by the notification
        // handlers to tell a live message from backfilled/replayed history:
        // events older than this defer to the (sync-lagged) server unread
        // count, newer ones do not. See `emit_notification`.
        let session_start_ms: u64 = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as u64)
            .unwrap_or(0);

        // Global notification handler — fires for every room on every sync
        // response, without requiring a per-room subscribe_room call.
        {
            use matrix_sdk::ruma::events::OriginalSyncMessageLikeEvent;
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            self.event_handler_handles.push(client.add_event_handler(
                move |ev: OriginalSyncMessageLikeEvent<RoomMessageEventContent>, room: Room| {
                    let h = Arc::clone(&h);
                    let client_clone = client_clone.clone();
                    handle_message_notification(ev, room, client_clone, h, session_start_ms)
                },
            ));
        }

        // Sticker notification handler. `m.sticker` is a distinct event type
        // (StickerEventContent, not RoomMessageEventContent) so it is invisible
        // to the message handler above — without this, sticker messages never
        // notify at all.
        {
            use matrix_sdk::ruma::events::sticker::StickerEventContent;
            use matrix_sdk::ruma::events::OriginalSyncMessageLikeEvent;
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            self.event_handler_handles.push(client.add_event_handler(
                move |ev: OriginalSyncMessageLikeEvent<StickerEventContent>, room: Room| {
                    let h = Arc::clone(&h);
                    let client_clone = client_clone.clone();
                    handle_sticker_notification(ev, room, client_clone, h, session_start_ms)
                },
            ));
        }

        // Global typing-notification handler. Fires for every room whenever
        // the set of typing users changes. Self is filtered out so the UI
        // never shows the local user in the typing bar.
        {
            use matrix_sdk::ruma::events::typing::SyncTypingEvent;
            let h = Arc::clone(&handler);
            let me = client.user_id().map(|u| u.to_owned());
            self.event_handler_handles.push(client.add_event_handler(
                move |ev: SyncTypingEvent, room: Room| {
                    let h = Arc::clone(&h);
                    let me = me.clone();
                    handle_typing_notification(ev, room, h, me)
                },
            ));
        }

        // Presence polling task.
        // SyncService uses SlidingSync (MSC3575) which does not include the
        // presence extension, so add_event_handler(PresenceEvent) never fires.
        // Instead poll GET /_matrix/client/v3/presence/{userId}/status for each
        // DM counterpart on a 60-second interval.
        {
            let h = Arc::clone(&handler);
            let client_p = client.clone();
            let stop_rx_presence = stop_rx.clone();
            let presence_enabled_p = std::sync::Arc::clone(&self.presence_polling_enabled);
            let dm_counterparts = Arc::clone(&self.dm_counterparts);
            let forbidden_presence = Arc::clone(&self.forbidden_presence);
            let app_cache_db_p = Arc::clone(&self.app_cache_db);
            self.spawn_tracked(watch_presence(
                h,
                client_p,
                stop_rx_presence,
                presence_enabled_p,
                dm_counterparts,
                forbidden_presence,
                app_cache_db_p,
            ));
        }

        // Build SyncService. `with_offline_mode` lets matrix-sdk-ui handle
        // transient transport failures itself: instead of failing into
        // State::Error, the supervisor moves to State::Offline and polls
        // `/_matrix/client/versions` in the background until the server is
        // reachable, then transitions back to State::Running and restarts the
        // sync child tasks — no manual stop/start cycle or exponential
        // backoff needed on our side.
        let sync_service = match self
            .rt
            .block_on(
                SyncService::builder(client.clone())
                    .with_offline_mode()
                    .build(),
            )
        {
            Ok(s) => Arc::new(s),
            Err(e) => {
                {
                    let guard = handler.lock();
                    guard.on_error("sync_init", &e.to_string(), false);
                }
                return;
            }
        };
        self.sync_service = Some(Arc::clone(&sync_service));

        // Room info watcher: maintains a per-room cache and re-emits the room
        // list on every notable update. The notable_update_receiver carries the
        // `room_id` of the room that actually changed; we use that to rebuild
        // only that room's `RoomInfo` instead of walking `joined_rooms()` and
        // fanning out into per-room SQLite queries on every read-receipt /
        // latest-event burst.
        //
        // `m.call.member` state events are not notable updates in matrix-sdk,
        // so a dedicated channel carries the affected room ID into the watcher
        // loop when call-member state changes.
        let (call_member_tx, call_member_rx) =
            tokio::sync::mpsc::unbounded_channel::<OwnedRoomId>();
        {
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            let mut notable_rx = client.room_info_notable_update_receiver();
            let mut stop_rx_rooms = stop_rx.clone();
            let packs_cache = Arc::clone(&self.image_packs);
            let write_pending = Arc::clone(&self.user_pack_write_pending);
            let packs_dirty = Arc::clone(&self.packs_dirty);
            // Open the per-account cache DBs for this session.
            // Load persisted backfill timestamps now so the first
            // on_rooms_updated() already classifies rooms correctly;
            // both connections stay open until the next start_sync
            // (account switch) or app exit.
            {
                // data_dir is already the per-account SDK store dir; only
                // open if we're actually logged in (user_id is known).
                if client.user_id().is_some() {
                    {
                        let mut db = self.app_cache_db.lock();
                        *db = None; // close any previous session's connection
                    }
                    {
                        let mut db = self.search_db.lock();
                        *db = None; // close any previous session's connection
                    }
                    self.sdk_media_fetched.lock().clear();
                    if let Some(conn) = open_app_cache_db(&self.data_dir) {
                        load_backfill_ts_conn(&conn, &self.backfill_previews);
                        // Restore persisted presence-forbidden set so users
                        // that returned 403 in a previous session are not
                        // re-polled after restart.
                        {
                            let persisted = backfill::load_presence_forbidden_conn(&conn);
                            let mut fp = self.forbidden_presence.lock();
                            for uid_str in persisted {
                                if let Ok(uid) = uid_str.parse() {
                                    fp.insert(uid);
                                }
                            }
                        }
                        {
                            let mut db = self.app_cache_db.lock();
                            *db = Some(conn);
                        }
                    }
                    if let Some(conn) = open_search_db(&self.data_dir) {
                        let mut db = self.search_db.lock();
                        *db = Some(conn);
                    }
                }
            }
            let previews = Arc::clone(&self.backfill_previews);
            let dm_counterparts_w = Arc::clone(&self.dm_counterparts);
            let mut call_member_rx = call_member_rx;

            self.spawn_tracked(async move {
                use matrix_sdk::RoomState;

                // Initial cache fill. `room_info_notable_update_receiver`
                // only fires on *notable* room transitions (display-name,
                // member changes, encryption state, …). After
                // `restore_session()` the matrix-sdk has already
                // repopulated `joined_rooms()` from the SQLite cache, but
                // no notable update is emitted until the server actually
                // sends one — so on a quiet restored session the UI
                // would sit forever on an empty room list. Walk the
                // cached set once into our per-room cache before entering
                // the recv loop. On a fresh login `joined_rooms()` is
                // empty here and we emit an empty list, which is then
                // overwritten as the first sync's notable updates arrive.
                let mut cache: std::collections::HashMap<
                    OwnedRoomId,
                    crate::ffi::RoomInfo,
                > = std::collections::HashMap::new();
                let mut prev_sort_keys = super::room_list_fingerprint(&[]);
                for room in client_clone.joined_rooms() {
                    if let Some(info) = super::build_room_info(&client_clone, &room).await {
                        cache.insert(room.room_id().to_owned(), info);
                    }
                }
                refresh_dm_counterparts(&dm_counterparts_w, &cache);
                emit_snapshot(&cache, &previews, &h);

                let invites = build_invite_infos(&client_clone).await;
                {
                    let guard = h.lock();
                    guard.on_invites_updated(&invites);
                }
                // Initial prefs snapshot — fired BEFORE on_rooms_updated so
                // the UI has pendingRestoreRoom_ set when the room list
                // arrives and can navigate immediately on first paint.
                let mut prev_prefs = account::read_prefs_json(&client_clone).await;
                {
                    let guard = h.lock();
                    guard.on_account_prefs_updated(&prev_prefs);
                }

                // Initial MSC4278 media-preview config snapshot.
                let mut prev_media_preview =
                    account::read_media_preview_config_json(&client_clone).await;
                {
                    let guard = h.lock();
                    guard.on_media_preview_config_updated(&prev_media_preview);
                }

                // Initial image-pack snapshot, same reasoning as for the
                // room list: piggy-back on the same wakeup channel so
                // both lists arrive together after every notable
                // sync delta.
                //
                // The HTTP fetch cache lives here so it persists across
                // rebuilds for the lifetime of this sync session.  Each
                // (room_id, state_key) pair is fetched at most once —
                // subsequent notable-update rebuilds reuse the result.
                let mut http_pack_cache: std::collections::HashMap<
                    (OwnedRoomId, String),
                    Option<serde_json::Value>,
                > = std::collections::HashMap::new();
                let pks = rebuild_image_packs(&client_clone, &mut http_pack_cache).await;
                { let mut g = packs_cache.lock(); *g = pks; }
                { let guard = h.lock(); guard.on_image_packs_updated(); }

                loop {
                    use matrix_sdk_base::RoomInfoNotableUpdateReasons;
                    tokio::select! {
                        _ = stop_rx_rooms.changed() => {
                            if *stop_rx_rooms.borrow() { break; }
                        }
                        Some(room_id) = call_member_rx.recv() => {
                            // A m.call.member state event arrived for this room.
                            // Rebuild its RoomInfo so has_active_call reflects
                            // the current call state, then emit.
                            if let Some(room) = client_clone.get_room(&room_id) {
                                if room.state() == RoomState::Joined {
                                    if let Some(info) =
                                        super::build_room_info(&client_clone, &room).await
                                    {
                                        cache.insert(room_id, info);
                                    }
                                }
                            }
                            refresh_dm_counterparts(&dm_counterparts_w, &cache);
                            let sort_keys = {
                                let mut tmp: Vec<crate::ffi::RoomInfo> =
                                    cache.values().cloned().collect();
                                apply_backfill_previews(&mut tmp, &previews);
                                super::room_list_fingerprint(&tmp)
                            };
                            if sort_keys != prev_sort_keys {
                                prev_sort_keys = sort_keys;
                                emit_snapshot(&cache, &previews, &h);
                            }
                        }
                        result = notable_rx.recv() => {
                            use tokio::sync::broadcast::error::{RecvError, TryRecvError};
                            // Accumulate reasons + the set of changed room
                            // IDs across a short debounce window. matrix-sdk
                            // emits 5-30 notable updates per second during
                            // read-receipt / latest-event bursts; we coalesce
                            // them into one apply pass so the UI sees one
                            // emit per burst, not 30. The set is a HashSet
                            // so the same room mentioned multiple times in
                            // the window only triggers one rebuild.
                            let mut combined = RoomInfoNotableUpdateReasons::empty();
                            let mut changed: std::collections::HashSet<OwnedRoomId> =
                                std::collections::HashSet::new();
                            let mut full_resync = false;
                            match result {
                                Ok(update) => {
                                    combined |= update.reasons;
                                    changed.insert(update.room_id);
                                }
                                Err(RecvError::Lagged(_)) => {
                                    // Missed updates — assume the worst so
                                    // we rebuild everything that might
                                    // have changed.
                                    combined = RoomInfoNotableUpdateReasons::all();
                                    full_resync = true;
                                }
                                Err(RecvError::Closed) => break,
                            }
                            tokio::time::sleep(std::time::Duration::from_millis(150)).await;
                            loop {
                                match notable_rx.try_recv() {
                                    Ok(u) => {
                                        combined |= u.reasons;
                                        changed.insert(u.room_id);
                                    }
                                    Err(TryRecvError::Empty)  => break,
                                    Err(TryRecvError::Closed) => break,
                                    Err(TryRecvError::Lagged(_)) => {
                                        combined = RoomInfoNotableUpdateReasons::all();
                                        full_resync = true;
                                    }
                                }
                            }
                            if full_resync {
                                // Recover from a missed-update overflow by
                                // re-walking joined_rooms(). Replaces the
                                // cache wholesale so rooms that left while
                                // we lagged are dropped.
                                changed.clear();
                                for room in client_clone.joined_rooms() {
                                    changed.insert(room.room_id().to_owned());
                                }
                                let stale: Vec<OwnedRoomId> = cache
                                    .keys()
                                    .filter(|rid| !changed.contains(*rid))
                                    .cloned()
                                    .collect();
                                for rid in stale {
                                    cache.remove(&rid);
                                }
                            }
                            // Apply: per-room rebuild. Rooms whose membership
                            // moved out of `Joined` (left/kicked/tombstoned)
                            // are dropped from the cache, so the next emit
                            // omits them.
                            for room_id in &changed {
                                let room = client_clone.get_room(room_id);
                                let still_joined = room
                                    .as_ref()
                                    .map(|r| r.state() == RoomState::Joined)
                                    .unwrap_or(false);
                                if let (Some(room), true) = (room, still_joined) {
                                    if let Some(info) =
                                        super::build_room_info(&client_clone, &room).await
                                    {
                                        cache.insert(room_id.clone(), info);
                                    } else {
                                        // Tombstoned — exclude from the UI.
                                        cache.remove(room_id);
                                    }
                                } else {
                                    cache.remove(room_id);
                                }
                            }
                            refresh_dm_counterparts(&dm_counterparts_w, &cache);
                            let sort_keys = {
                                let mut tmp: Vec<crate::ffi::RoomInfo> =
                                    cache.values().cloned().collect();
                                apply_backfill_previews(&mut tmp, &previews);
                                super::room_list_fingerprint(&tmp)
                            };
                            if sort_keys != prev_sort_keys {
                                prev_sort_keys = sort_keys;
                                emit_snapshot(&cache, &previews, &h);
                            }

                            let invites = build_invite_infos(&client_clone).await;
                            {
                                let guard = h.lock();
                                guard.on_invites_updated(&invites);
                            }
                            // Image packs only change on membership events
                            // (joining/leaving changes which room packs are
                            // visible), when the account-data watcher set
                            // `packs_dirty` (user pack or emote-rooms list
                            // changed via any client), or while our own write
                            // echo is still in flight.  Skip the O(all-rooms)
                            // SQLite sweep on pure read-receipt / recency /
                            // typing bursts that only carry the NONE reason.
                            let pack_dirty =
                                packs_dirty.swap(false, std::sync::atomic::Ordering::AcqRel);
                            let membership_changed = combined
                                .contains(RoomInfoNotableUpdateReasons::MEMBERSHIP);
                            let echo_pending =
                                write_pending.load(std::sync::atomic::Ordering::Acquire);
                            if !membership_changed && !pack_dirty && !echo_pending {
                                continue;
                            }
                            // Refresh image packs on the same tick.
                            // Account-data and state-event changes that
                            // matter for image packs flow through the
                            // same sync deltas that produce notable
                            // room updates; piggy-backing keeps us off
                            // a polling timer and out of the event
                            // handler machinery.
                            let pks = rebuild_image_packs(&client_clone, &mut http_pack_cache).await;
                            {
                                let mut g = packs_cache.lock();
                                use std::sync::atomic::Ordering;
                                use crate::image_packs::PackSource;

                                let has_user = pks.iter().any(|p| p.source == PackSource::User);
                                let pending  = write_pending.load(Ordering::Acquire);

                                // Determine whether to preserve the cached user pack
                                // over the rebuild result. We only preserve when our
                                // own write is still in flight (pending = true) AND the
                                // rebuild result appears stale (fewer images than cache,
                                // or no user pack at all). Once rebuild finds the user
                                // pack the echo has arrived; clear the flag so a later
                                // external deletion is not incorrectly preserved.
                                let cached_user = g.iter()
                                    .find(|p| p.source == PackSource::User)
                                    .cloned();
                                let rebuilt_images = pks.iter()
                                    .find(|p| p.source == PackSource::User)
                                    .map(|p| p.images.len())
                                    .unwrap_or(0);
                                let cached_images = cached_user.as_ref()
                                    .map(|p| p.images.len())
                                    .unwrap_or(0);

                                // Preserve when rebuild is stale: no user pack (echo not
                                // yet in state store) or fewer images than cached (state
                                // store reflects an older write). Use >= so a
                                // toggle_favorite_sticker write (which doesn't change image
                                // count) is also preserved across rebuilds before its echo.
                                let should_preserve = pending && match (cached_user.is_some(), has_user) {
                                    (true, false) => true,
                                    (true, true)  => cached_images >= rebuilt_images,
                                    _             => false,
                                };

                                if should_preserve {
                                    let mut merged: Vec<_> = pks.into_iter()
                                        .filter(|p| p.source != PackSource::User)
                                        .collect();
                                    if let Some(cu) = cached_user {
                                        merged.insert(0, cu);
                                    }
                                    *g = merged;
                                } else {
                                    if has_user {
                                        write_pending.store(false, Ordering::Release);
                                    }
                                    *g = pks;
                                }
                            }
                            {
                                let guard = h.lock();
                                guard.on_image_packs_updated();
                            }
                            // Check for prefs changes on the same tick.
                            // `save_prefs` does a fire-and-forget PUT which
                            // echoes back as an account-data event in the
                            // next sync, triggering a notable update.
                            let cur_prefs = account::read_prefs_json(&client_clone).await;
                            if cur_prefs != prev_prefs {
                                {
                                    let guard = h.lock();
                                    guard.on_account_prefs_updated(&cur_prefs);
                                }
                                prev_prefs = cur_prefs;
                            }

                            // MSC4278 media-preview config on the same tick.
                            let cur_media_preview =
                                account::read_media_preview_config_json(&client_clone).await;
                            if cur_media_preview != prev_media_preview {
                                {
                                    let guard = h.lock();
                                    guard.on_media_preview_config_updated(&cur_media_preview);
                                }
                                prev_media_preview = cur_media_preview;
                            }
                        }
                    }
                }
            });
        }

        // Propagate m.call.member state changes into the room-info watcher.
        // matrix-sdk does not emit a RoomInfoNotableUpdate for call-member
        // state events, so without this handler the room list would never
        // refresh has_active_call mid-session (icon persists after call ends).
        {
            use matrix_sdk::ruma::events::{call::member::CallMemberEventContent, SyncStateEvent};
            self.event_handler_handles.push(client.add_event_handler(
                move |_ev: SyncStateEvent<CallMemberEventContent>, room: Room| {
                    let tx = call_member_tx.clone();
                    async move {
                        let _ = tx.send(room.room_id().to_owned());
                    }
                },
            ));
        }

        // Recovery state watcher (Step 6).
        //
        // `client.encryption().recovery().state()` starts as `Unknown` and is
        // only populated once the relevant account-data events arrive during
        // the first sync cycle. Without this watcher, a UI that calls
        // `needs_recovery()` right after `start_sync()` always sees `false`.
        //
        // Every state transition triggers an extra `on_backup_progress` so the
        // UI gets a chance to re-evaluate `needs_recovery()`. Reusing that
        // callback (instead of adding a dedicated one) keeps the FFI small —
        // the UI was already re-checking via this slot.
        {
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            let state_code = Arc::clone(&self.backup_state_code);
            let imported = Arc::clone(&self.imported_keys);
            let stop_rx = stop_rx.clone();

            self.spawn_tracked(watch_recovery_state(h, client_clone, state_code, imported, stop_rx));
        }

        // Backup-state watcher (Step 6).
        //
        // Subscribes to `Backups::state_stream()` for high-level transitions
        // (Unknown → Enabling → Downloading → Enabled, etc.) and emits an
        // `on_backup_progress` callback on every change.
        //
        // `total_keys` is left at 0 because matrix-sdk does not expose a cheap
        // "how many keys does the backup contain" query.
        {
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            let state_code = Arc::clone(&self.backup_state_code);
            let imported = Arc::clone(&self.imported_keys);
            let stop_rx = stop_rx.clone();

            self.spawn_tracked(watch_backup_state(h, client_clone, state_code, imported, stop_rx));
        }

        // Imported-room-keys watcher (Step 6).
        //
        // `Encryption::room_keys_received_stream()` only becomes available once
        // the OlmMachine is initialised — which can lag a beat behind login. If
        // we were to call it once at start_sync time it might return `None`,
        // and we'd silently miss every batch (this is exactly what made the
        // recovery dialog show "0 keys imported").
        //
        // So we poll with a short backoff until the stream becomes available,
        // then forward each batch's `.len()` into the shared imported_keys
        // counter and re-emit an `on_backup_progress` so the UI updates live.
        {
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            let state_code = Arc::clone(&self.backup_state_code);
            let imported = Arc::clone(&self.imported_keys);
            let stop_rx = stop_rx.clone();

            self.spawn_tracked(watch_imported_keys(h, client_clone, state_code, imported, stop_rx));
        }

        // RoomListService state watcher.
        //
        // Surfaces the high-level sliding-sync phases (Init → SettingUp →
        // Running, plus Recovering on reconnect) so the UI can show a
        // "Syncing rooms…" status while the joined-room set is still being
        // hydrated. The SyncService itself only exposes Idle/Running/Error;
        // the room-list service is where the actually-interesting transitions
        // live.
        //
        // Emits an initial snapshot before the recv loop so a UI that opens
        // before the first transition still has a starting value, matching
        // the backup-state watcher above.
        {
            let h = Arc::clone(&handler);
            let svc_clone = Arc::clone(&sync_service);
            let stop_rx = stop_rx.clone();

            self.spawn_tracked(watch_room_list_state(h, svc_clone, stop_rx));
        }

        // Verification state watcher.
        //
        // Subscribes to `client.encryption().verification_state()` so the UI
        // is notified whenever the cross-signing verified status of the current
        // account changes (Unknown → Unverified → Verified). Fires an initial
        // snapshot on startup so the UI always has a starting value.
        {
            let h = Arc::clone(&handler);
            let client_clone = client.clone();
            let stop_rx = stop_rx.clone();

            self.spawn_tracked(watch_verification_state(h, client_clone, stop_rx));
        }

        // Incoming verification request handler.
        //
        // Registers a to-device event handler for `m.key.verification.request`
        // so the UI is notified when another device initiates a SAS flow with
        // this one. After the SDK processes the event internally, we retrieve
        // the `VerificationRequest` object and store the flow_id → user_id
        // mapping so subsequent API calls can do the lookup.
        {
            use matrix_sdk::ruma::events::{
                key::verification::request::ToDeviceKeyVerificationRequestEventContent,
                ToDeviceEvent,
            };
            let h = Arc::clone(&handler);
            let flow_users = Arc::clone(&self.verification_flow_users);
            let emoji_cache = Arc::clone(&self.sas_emoji_cache);
            let tasks = Arc::clone(&self.verification_tasks);

            self.event_handler_handles.push(client.add_event_handler(
                move |ev: ToDeviceEvent<ToDeviceKeyVerificationRequestEventContent>,
                      client: Client| {
                    let h = Arc::clone(&h);
                    let flow_users = Arc::clone(&flow_users);
                    let emoji_cache = Arc::clone(&emoji_cache);
                    let tasks = Arc::clone(&tasks);
                    async move {
                        let flow_id = ev.content.transaction_id.to_string();
                        let user_id = ev.sender.as_str().to_owned();
                        let device_id = ev.content.from_device.as_str().to_owned();

                        // Ignore the request this device just broadcast itself.
                        // request_self_verification() sends an
                        // m.key.verification.request to all of our other
                        // sessions; the homeserver can echo that to-device
                        // event back to the sender. Without this guard the echo
                        // is treated as an incoming request and pops a "verify
                        // this device" prompt on the very device that started
                        // the flow. A request is our own echo when it comes
                        // from our user *and* our own device_id.
                        if user_id == client.user_id().map(|u| u.as_str().to_owned()).unwrap_or_default()
                            && client
                                .device_id()
                                .map(|d| d.as_str() == device_id)
                                .unwrap_or(false)
                        {
                            return;
                        }

                        // The OlmMachine processes the to-device event and
                        // adds the request to its internal map asynchronously.
                        // A single fixed sleep silently drops the request on
                        // slow hardware / under sync load, so poll with a
                        // bounded backoff (≈ 50+100+200+400+800+1600 ≈ 3.15s
                        // total) instead.
                        let mut req = None;
                        let mut delay_ms = 50u64;
                        for _ in 0..6 {
                            tokio::time::sleep(std::time::Duration::from_millis(delay_ms)).await;
                            req = client
                                .encryption()
                                .get_verification_request(&ev.sender, &flow_id)
                                .await;
                            if req.is_some() {
                                break;
                            }
                            delay_ms *= 2;
                        }
                        if let Some(req) = req {
                            lock_or_recover(&flow_users).insert(flow_id.clone(), user_id.clone());
                            {
                                let guard = h.lock();
                                guard.on_verification_request(&flow_id, &user_id, &device_id, true);
                            }
                            // Spawn a watcher so we can surface request-level
                            // transitions (Done / Cancelled) that occur before
                            // start_sas is called.
                            let h2 = Arc::clone(&h);
                            let flow_users2 = Arc::clone(&flow_users);
                            let emoji_cache2 = Arc::clone(&emoji_cache);
                            let flow_id2 = flow_id.clone();
                            let tasks2 = Arc::clone(&tasks);
                            let handle = tokio::spawn(verification::watch_verification_request(
                                req,
                                flow_id2,
                                h2,
                                flow_users2,
                                emoji_cache2,
                                tasks,
                            ));
                            lock_or_recover(&tasks2).push(handle.abort_handle());
                        } else {
                            tracing::warn!(
                                "verification request {flow_id} from {user_id} \
                                 not visible after retries; dropped",
                            );
                        }
                    }
                },
            ));
        }

        // Account-data image-pack watcher: set `packs_dirty` whenever the user
        // pack or emote-rooms subscription changes so the notable-update loop
        // can rebuild image packs without relying on the catch-all NONE reason
        // (which also fires on read-receipts, typing, and recency stamps).
        {
            use matrix_sdk::ruma::events::AnyGlobalAccountDataEvent;
            let packs_dirty_eh = Arc::clone(&self.packs_dirty);
            self.event_handler_handles.push(client.add_event_handler(
                move |ev: AnyGlobalAccountDataEvent| {
                    let packs_dirty_eh = Arc::clone(&packs_dirty_eh);
                    async move {
                        let type_str = ev.event_type().to_string();
                        if matches!(
                            type_str.as_str(),
                            crate::image_packs::TYPE_USER_PACK
                                | crate::image_packs::TYPE_EMOTE_ROOMS_STABLE
                                | crate::image_packs::TYPE_EMOTE_ROOMS_UNSTABLE
                        ) {
                            packs_dirty_eh.store(true, std::sync::atomic::Ordering::Release);
                        }
                    }
                },
            ));
        }

        // Start SyncService and observe state. Transient transport failures
        // are handled by the supervisor itself (Running → Offline → Running
        // via the `/_matrix/client/versions` poller, enabled by
        // `with_offline_mode` above), so the monitor's only job is to surface
        // a single "we're offline" notification per outage to the UI and let
        // the supervisor recover on its own. State::Error from the supervisor
        // is now terminal — it only fires when the offline poller's own
        // `/versions` request returned a non-recoverable error.
        let svc_clone = Arc::clone(&sync_service);
        let h_state = Arc::clone(&handler);
        let mut stop_rx_sv = stop_rx.clone();

        // MatrixRTC: register the CXX event sink (once, keyed to this handler)
        // and the global invitation watcher so incoming call slots surface to the
        // UI even when we haven't joined a call yet.
        #[cfg(feature = "calls")]
        {
            use crate::bridge::RtcCxxBridgeSink;
            use crate::client::rtc;
            let sink = std::sync::Arc::new(RtcCxxBridgeSink {
                handler: std::sync::Arc::clone(&handler),
            });
            rtc::register_sink(sink);
            rtc::session::register_invitation_handler(&client);
            rtc::session::register_msc3401_invitation_handler(&client);
            rtc::session::register_rtc_notification_handler(&client);
        }

        self.spawn_tracked(async move {
            svc_clone.start().await;

            use matrix_sdk_ui::sync_service::State as SyncServiceState;
            let mut state_stream = svc_clone.state();
            // Edge-trigger the notification: only fire on the Running→Offline
            // transition, not on every poll inside the offline loop.
            let mut notified_offline = false;

            loop {
                tokio::select! {
                    _ = stop_rx_sv.changed() => {
                        // Authoritative SyncService::stop() is owned by
                        // stop_sync(); just exit the observer here so the
                        // service is not stopped twice concurrently.
                        if *stop_rx_sv.borrow() { break; }
                    }
                    Some(state) = state_stream.next() => {
                        match state {
                            SyncServiceState::Running => {
                                notified_offline = false;
                            }
                            SyncServiceState::Offline
                                if !notified_offline => {
                                    notified_offline = true;
                                    {
                                        let guard = h_state.lock();
                                        guard.on_error(
                                            "sync_offline",
                                            "Lost contact with server; retrying…",
                                            false,
                                        );
                                    }
                                }
                            SyncServiceState::Error(e) => {
                                {
                                    let guard = h_state.lock();
                                    guard.on_error(
                                        "sync_error",
                                        &e.to_string(),
                                        false,
                                    );
                                }
                            }
                            _ => {}
                        }
                    }
                }
            }
        });
    }

    pub fn stop_sync(&mut self) {
        // Take the handler here so that the Drop impl calling stop_sync a
        // second time (after ~MainWindow has already run) is a no-op.  If the
        // handler is already gone we skip the session flush on the second call
        // rather than calling back into a partially-destroyed C++ object.
        let handler = self.handler.take();
        // Flush the latest OAuth session to disk before tearing down the
        // runtime.  The session-watcher task (spawned in start_sync) saves
        // new tokens whenever TokensRefreshed fires, but its JoinHandle is
        // discarded so it may be cancelled mid-flight when the runtime drops.
        // Saving here, while the C++ EventHandler is still alive, ensures the
        // most recent refresh token is always persisted on clean shutdown.
        if let (Some(client), Some(handler)) = (&self.client, &handler) {
            if let Some(full) = client.oauth().full_session() {
                let persisted = PersistedSession {
                    client_id: full.client_id,
                    user: full.user,
                };
                if let Ok(json) = serde_json::to_string(&persisted) {
                    {
                        let guard = handler.lock();
                        guard.on_session_refreshed(&json);
                    }
                }
            }
        }

        if let Some(tx) = self.stop_tx.take() {
            let _ = tx.send(true);
        }
        // Remove the global event-handler registrations so the notification /
        // typing / verification handlers stop firing into a handler that is
        // about to be dropped.
        if let Some(client) = &self.client {
            for eh in self.event_handler_handles.drain(..) {
                client.remove_event_handler(eh);
            }
        }
        if let Some(h) = self.backfill_task.take() {
            h.abort();
        }
        // Hard-abort every long-lived sync task. The stop signal above lets
        // the ones that select on it exit cleanly; aborting also cancels the
        // session-refresh watcher (which has no stop-channel arm) and closes
        // the use-after-free window where a task could still call back into
        // the C++ handler after self.handler was taken.
        for h in self.sync_tasks.drain(..) {
            h.abort();
        }
        // Verification watchers (and their nested SAS watchers) are spawned
        // outside `sync_tasks` because nested spawns happen from inside the
        // running future. Abort them here so an in-flight SAS does not call
        // back into the C++ handler after shutdown.
        for h in lock_or_recover(&self.verification_tasks).drain(..) {
            h.abort();
        }
        // Async media downloads (fetch_media_async / get_url_preview_async) hold
        // cloned handler Arcs and would call back through on_media_ready after
        // the handler was taken. Abort every group before the slot detaches.
        {
            let mut m = self.media_tasks.lock();
            for (_, v) in m.drain() {
                for (_, h) in v {
                    h.abort();
                }
            }
        }
        if let Some(svc) = self.sync_service.take() {
            self.rt.block_on(async move {
                let _ = svc.stop().await;
            });
        }
    }

    // -----------------------------------------------------------------------
    // Presence polling toggle
    // -----------------------------------------------------------------------

    /// Enable or disable background presence polling. Thread-safe — may be
    /// called from the UI thread while the polling task runs on a worker.
    pub fn set_presence_polling_enabled(&self, enabled: bool) {
        self.presence_polling_enabled
            .store(enabled, std::sync::atomic::Ordering::Relaxed);
    }

    /// Issue one immediate round of DM presence polls, regardless of the
    /// 60s interval cadence. Used by the UI shell when the window returns
    /// to focus so contacts don't appear stale for up to a minute after
    /// un-minimize. No-op if sync hasn't been started, presence polling
    /// is disabled, or there are no DM counterparts cached. Thread-safe.
    pub fn poll_presence_now(&mut self) {
        if !self
            .presence_polling_enabled
            .load(std::sync::atomic::Ordering::Relaxed)
        {
            return;
        }
        let Some(handler) = self.handler.as_ref().map(Arc::clone) else {
            return;
        };
        let Some(client) = self.client.clone() else {
            return;
        };
        let dm_counterparts = Arc::clone(&self.dm_counterparts);
        let forbidden_presence = Arc::clone(&self.forbidden_presence);
        let app_cache_db = Arc::clone(&self.app_cache_db);
        self.spawn_tracked(async move {
            poll_presence_once(
                &client,
                &handler,
                &dm_counterparts,
                &forbidden_presence,
                &app_cache_db,
            )
            .await;
        });
    }
}

/// One pass of the DM presence poll: snapshot the counterpart set, fan out
/// `GET /presence/{user}/status` for each, and deliver `on_presence_changed`
/// callbacks. Records users that return 403 Forbidden into
/// `forbidden_presence` so subsequent passes skip them. The 60s interval
/// loop and `poll_presence_now` both call this.
async fn poll_presence_once(
    client: &matrix_sdk::Client,
    handler: &Arc<Mutex<SendHandler>>,
    dm_counterparts: &Arc<parking_lot::RwLock<std::collections::HashSet<String>>>,
    forbidden_presence: &Arc<
        parking_lot::Mutex<std::collections::HashSet<matrix_sdk::ruma::OwnedUserId>>,
    >,
    app_cache_db: &Arc<parking_lot::Mutex<Option<rusqlite::Connection>>>,
) {
    use matrix_sdk::ruma::api::client::presence::get_presence;
    use matrix_sdk::ruma::presence::PresenceState as RumaPresence;
    if client.user_id().is_none() {
        return;
    }
    // Snapshot the cached counterpart set under the read lock, then drop
    // the lock before any await. The cache is refreshed from
    // RoomInfo.dm_counterpart_user_id by the room-list rebuild path — no
    // per-tick scan of joined_rooms or per-room dm_other_user lookup here.
    let counterparts: Vec<String> = {
        let set = dm_counterparts.read();
        set.iter().cloned().collect()
    };
    if counterparts.is_empty() {
        return;
    }
    let mut futs = Vec::new();
    for uid in counterparts {
        let cp = client.clone();
        let h_ref = Arc::clone(handler);
        let forbidden_clone = Arc::clone(forbidden_presence);
        let db_clone = Arc::clone(app_cache_db);
        futs.push(async move {
            let user_id: matrix_sdk::ruma::OwnedUserId = uid.parse().ok()?;
            // Skip users known to return 403 Forbidden.
            if forbidden_clone.lock().contains(&user_id) {
                return None;
            }
            let req = get_presence::v3::Request::new(user_id.clone());
            match cp.send(req).await {
                Ok(resp) => {
                    let state: u8 = match resp.presence {
                        RumaPresence::Online => 1,
                        RumaPresence::Unavailable => 2,
                        _ => 3,
                    };
                    {
                        let g = h_ref.lock();
                        g.on_presence_changed(&uid, state);
                    }
                    Some(())
                }
                Err(e) => {
                    if is_presence_forbidden(e.client_api_error_kind()) {
                        {
                            let mut set = forbidden_clone.lock();
                            if set.insert(user_id) {
                                tracing::info!(
                                    "presence: stopping polls for {uid} \
                                     (homeserver forbids)"
                                );
                                // Persist so the user is skipped after restart too.
                                let db = db_clone.lock();
                                if let Some(conn) = db.as_ref() {
                                    backfill::upsert_presence_forbidden_conn(conn, &uid);
                                }
                            }
                        }
                    }
                    // Other transient errors (404, 5xx, network) silently
                    // ignored — the next tick will retry.
                    None
                }
            }
        });
    }
    futures_util::future::join_all(futs).await;
}

// ---------------------------------------------------------------------------
// start_sync watcher / handler helpers (extracted, behavior-preserving)
//
// Each of these is the body of a task spawned by `start_sync`, lifted to
// module scope so the registration routine reads as a sequence of named
// spawns. They take exactly the state the inline body captured.
// ---------------------------------------------------------------------------

/// Refresh the cached DM-counterpart set from the room cache values. The
/// presence polling task reads this snapshot directly so it never has to
/// walk joined_rooms itself.
fn refresh_dm_counterparts(
    cache_w: &parking_lot::RwLock<std::collections::HashSet<String>>,
    cache: &std::collections::HashMap<OwnedRoomId, crate::ffi::RoomInfo>,
) {
    let new_set: std::collections::HashSet<String> = cache
        .values()
        .map(|r| r.dm_counterpart_user_id.clone())
        .filter(|s| !s.is_empty())
        .collect();
    *cache_w.write() = new_set;
}

/// Snapshot helper: clone the cache values into a Vec, apply backfill
/// previews, sort, and push to the UI thread.
fn emit_snapshot(
    cache: &std::collections::HashMap<OwnedRoomId, crate::ffi::RoomInfo>,
    previews: &Mutex<std::collections::HashMap<String, crate::client::backfill::BackfillPreview>>,
    h: &Arc<Mutex<SendHandler>>,
) {
    let mut snapshot: Vec<crate::ffi::RoomInfo> = cache.values().cloned().collect();
    apply_backfill_previews(&mut snapshot, previews);
    super::sort_room_infos(&mut snapshot);
    {
        let guard = h.lock();
        guard.on_rooms_updated(&snapshot);
    }
}

/// Session-refresh watcher: persist refreshed OAuth tokens and surface an
/// auth error (stopping SyncService) when the token is no longer valid.
async fn watch_session_changes(
    h: Arc<Mutex<SendHandler>>,
    client: Client,
    stop_tx_auth: watch::Sender<bool>,
) {
    let mut changes = client.subscribe_to_session_changes();
    loop {
        match changes.recv().await {
            Ok(SessionChange::TokensRefreshed) => {
                let Some(full) = client.oauth().full_session() else {
                    continue;
                };
                let persisted = PersistedSession {
                    client_id: full.client_id,
                    user: full.user,
                };
                let Ok(json) = serde_json::to_string(&persisted) else {
                    continue;
                };
                {
                    let guard = h.lock();
                    guard.on_session_refreshed(&json);
                }
            }
            Ok(SessionChange::UnknownToken(data)) => {
                // Stop SyncService before it can reach State::Error and
                // wipe the SQLite data directory while a fresh login is
                // already in progress.
                let _ = stop_tx_auth.send(true);
                {
                    let guard = h.lock();
                    guard.on_error(
                        "sync_auth_error",
                        "Session token is no longer valid; please log in again.",
                        data.soft_logout,
                    );
                }
                break;
            }
            Err(_) => break,
        }
    }
}

/// Notification-handler body for `m.room.message` events. Lifted out of the
/// `add_event_handler` closure; the closure clones the captured state per
/// invocation and forwards it here along with the event/room.
async fn handle_message_notification(
    ev: matrix_sdk::ruma::events::OriginalSyncMessageLikeEvent<RoomMessageEventContent>,
    room: Room,
    client: Client,
    h: Arc<Mutex<SendHandler>>,
    session_start_ms: u64,
) {
    use matrix_sdk::ruma::events::room::message::MessageType;
    let (body, msg_type_str) = match &ev.content.msgtype {
        MessageType::Text(t) => (t.body.trim().to_owned(), "m.text"),
        MessageType::Image(i) => (i.body.trim().to_owned(), "m.image"),
        MessageType::File(f) => (f.body.trim().to_owned(), "m.file"),
        MessageType::Audio(a) => (a.body.trim().to_owned(), "m.audio"),
        MessageType::Video(v) => (v.body.trim().to_owned(), "m.video"),
        _ => return,
    };
    if body.is_empty() {
        return;
    }
    // Image messages carry a preview picture; other msgtypes get none.
    let preview_source = match &ev.content.msgtype {
        MessageType::Image(i) => Some(i.source.clone()),
        _ => None,
    };
    media::emit_notification(
        &client,
        room,
        &ev.sender,
        &body,
        msg_type_str,
        ev.event_id.as_str(),
        ev.origin_server_ts.get().into(),
        session_start_ms,
        preview_source,
        &h,
    )
    .await;
}

/// Notification-handler body for `m.sticker` events.
async fn handle_sticker_notification(
    ev: matrix_sdk::ruma::events::OriginalSyncMessageLikeEvent<
        matrix_sdk::ruma::events::sticker::StickerEventContent,
    >,
    room: Room,
    client: Client,
    h: Arc<Mutex<SendHandler>>,
    session_start_ms: u64,
) {
    use matrix_sdk::ruma::events::room::MediaSource;
    use matrix_sdk::ruma::events::sticker::StickerMediaSource;
    let body = ev.content.body.trim().to_owned();
    if body.is_empty() {
        return;
    }
    let preview_source = match &ev.content.source {
        StickerMediaSource::Plain(uri) => Some(MediaSource::Plain(uri.clone())),
        StickerMediaSource::Encrypted(f) => Some(MediaSource::Encrypted(f.clone())),
        _ => None,
    };
    media::emit_notification(
        &client,
        room,
        &ev.sender,
        &body,
        "m.sticker",
        ev.event_id.as_str(),
        ev.origin_server_ts.get().into(),
        session_start_ms,
        preview_source,
        &h,
    )
    .await;
}

/// Typing-notification handler body. Filters out self and resolves cached
/// member display names for the typing strip.
async fn handle_typing_notification(
    ev: matrix_sdk::ruma::events::typing::SyncTypingEvent,
    room: Room,
    h: Arc<Mutex<SendHandler>>,
    me: Option<matrix_sdk::ruma::OwnedUserId>,
) {
    let rid = room.room_id().to_string();
    let mut uids: Vec<String> = Vec::new();
    for u in ev.content.user_ids.iter() {
        if me.as_deref() == Some(u.as_ref()) {
            continue;
        }
        // Prefer the cached room-member display name so the typing strip
        // shows a readable name rather than an opaque localpart (e.g.
        // "@78fa3bcde:hs"). Falls back to the localpart when no member
        // profile is cached (no extra network round-trip).
        let name = super::member_display_name_local(&room, u).await;
        uids.push(name);
    }
    {
        let g = h.lock();
        g.on_typing_changed(&rid, &uids);
    }
}

/// Presence polling loop: every 60s, when enabled, run one DM presence pass.
async fn watch_presence(
    h: Arc<Mutex<SendHandler>>,
    client: Client,
    mut stop_rx_presence: watch::Receiver<bool>,
    presence_enabled: Arc<std::sync::atomic::AtomicBool>,
    dm_counterparts: Arc<parking_lot::RwLock<std::collections::HashSet<String>>>,
    forbidden_presence: Arc<Mutex<std::collections::HashSet<matrix_sdk::ruma::OwnedUserId>>>,
    app_cache_db: Arc<parking_lot::Mutex<Option<rusqlite::Connection>>>,
) {
    // 60s tick: matrix-sdk delivers presence EDUs through the sync stream
    // for free; this polling loop is a fallback for servers that omit them
    // for non-following users. The cache population (in the room-list
    // rebuild path) means the very first tick reads no users until
    // build_room_infos has finished its initial sweep — that's fine,
    // presence is not load-bearing for the first frame.
    let mut interval = tokio::time::interval(tokio::time::Duration::from_secs(60));
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop {
        tokio::select! {
            _ = stop_rx_presence.changed() => break,
            _ = interval.tick() => {}
        }
        if !presence_enabled.load(std::sync::atomic::Ordering::Relaxed) {
            continue;
        }
        poll_presence_once(&client, &h, &dm_counterparts, &forbidden_presence, &app_cache_db).await;
    }
}

/// Recovery-state watcher: re-emit a backup-progress snapshot on every
/// `Recovery::state_stream()` transition so the UI re-queries needs_recovery().
async fn watch_recovery_state(
    h: Arc<Mutex<SendHandler>>,
    client: Client,
    state_code: Arc<std::sync::atomic::AtomicU8>,
    imported: Arc<std::sync::atomic::AtomicU64>,
    mut stop_rx: watch::Receiver<bool>,
) {
    use futures_util::StreamExt;
    let mut rec_stream = client.encryption().recovery().state_stream();
    loop {
        tokio::select! {
            _ = stop_rx.changed() => {
                if *stop_rx.borrow() { break; }
            }
            Some(_state) = rec_stream.next() => {
                // Re-emit a snapshot; the UI re-queries needs_recovery().
                {
                    let guard = h.lock();
                    guard.on_backup_progress(&BackupProgress {
                        state:         state_code.load(Ordering::Relaxed),
                        imported_keys: imported.load(Ordering::Relaxed),
                        total_keys:    0,
                    });
                }
            }
            else => break,
        }
    }
}

/// Backup-state watcher: emit an initial snapshot, then an
/// `on_backup_progress` on every `Backups::state_stream()` transition.
async fn watch_backup_state(
    h: Arc<Mutex<SendHandler>>,
    client: Client,
    state_code: Arc<std::sync::atomic::AtomicU8>,
    imported: Arc<std::sync::atomic::AtomicU64>,
    mut stop_rx: watch::Receiver<bool>,
) {
    use futures_util::StreamExt;
    let mut state_stream = client.encryption().backups().state_stream();

    // Emit an initial snapshot so a UI that opens before the first state
    // change still has a starting value.
    {
        let s = backup_state_code(client.encryption().backups().state());
        state_code.store(s, Ordering::Relaxed);
        {
            let guard = h.lock();
            guard.on_backup_progress(&BackupProgress {
                state: s,
                imported_keys: imported.load(Ordering::Relaxed),
                total_keys: 0,
            });
        }
    }

    loop {
        tokio::select! {
            _ = stop_rx.changed() => {
                if *stop_rx.borrow() { break; }
            }
            Some(Ok(state)) = state_stream.next() => {
                let s = backup_state_code(state);
                state_code.store(s, Ordering::Relaxed);
                {
                    let guard = h.lock();
                    guard.on_backup_progress(&BackupProgress {
                        state:         s,
                        imported_keys: imported.load(Ordering::Relaxed),
                        total_keys:    0,
                    });
                }
            }
            else => break,
        }
    }
}

/// Imported-room-keys watcher: poll until `room_keys_received_stream()` is
/// available, then forward each batch's key count into `imported` and re-emit
/// an `on_backup_progress` so the UI updates live.
async fn watch_imported_keys(
    h: Arc<Mutex<SendHandler>>,
    client: Client,
    state_code: Arc<std::sync::atomic::AtomicU8>,
    imported: Arc<std::sync::atomic::AtomicU64>,
    mut stop_rx: watch::Receiver<bool>,
) {
    use futures_util::StreamExt;

    let keys_stream = loop {
        if *stop_rx.borrow() {
            return;
        }
        if let Some(s) = client.encryption().room_keys_received_stream().await {
            break s;
        }
        tokio::select! {
            _ = stop_rx.changed() => {
                if *stop_rx.borrow() { return; }
            }
            _ = tokio::time::sleep(std::time::Duration::from_millis(500)) => {}
        }
    };
    let mut keys_stream = Box::pin(keys_stream);

    loop {
        tokio::select! {
            _ = stop_rx.changed() => {
                if *stop_rx.borrow() { break; }
            }
            Some(batch) = keys_stream.next() => {
                if let Ok(keys) = batch {
                    let n = imported.fetch_add(keys.len() as u64, Ordering::Relaxed)
                        + keys.len() as u64;
                    {
                        let guard = h.lock();
                        guard.on_backup_progress(&BackupProgress {
                            state:         state_code.load(Ordering::Relaxed),
                            imported_keys: n,
                            total_keys:    0,
                        });
                    }
                }
            }
            else => break,
        }
    }
}

/// RoomListService state watcher: emit an initial snapshot, then surface each
/// sliding-sync phase transition via `on_room_list_state`.
async fn watch_room_list_state(
    h: Arc<Mutex<SendHandler>>,
    svc: Arc<SyncService>,
    mut stop_rx: watch::Receiver<bool>,
) {
    let rls = svc.room_list_service();
    let mut state_rx = rls.state();

    // Initial snapshot.
    {
        let s = room_list_state_code(&state_rx.next_now());
        {
            let guard = h.lock();
            guard.on_room_list_state(s);
        }
    }

    loop {
        tokio::select! {
            _ = stop_rx.changed() => {
                if *stop_rx.borrow() { break; }
            }
            Some(state) = state_rx.next() => {
                let s = room_list_state_code(&state);
                {
                    let guard = h.lock();
                    guard.on_room_list_state(s);
                }
            }
            else => break,
        }
    }
}

/// Verification-state watcher: emit an initial snapshot, then notify the UI
/// whenever the cross-signing verified status of this account changes.
async fn watch_verification_state(
    h: Arc<Mutex<SendHandler>>,
    client: Client,
    mut stop_rx: watch::Receiver<bool>,
) {
    use matrix_sdk::encryption::VerificationState;
    let mut state_rx = client.encryption().verification_state();

    // Initial snapshot.
    {
        let s = matches!(state_rx.next_now(), VerificationState::Verified);
        {
            let guard = h.lock();
            guard.on_verification_state_changed(s);
        }
    }

    loop {
        tokio::select! {
            _ = stop_rx.changed() => {
                if *stop_rx.borrow() { break; }
            }
            Some(state) = state_rx.next() => {
                let verified = matches!(state, VerificationState::Verified);
                {
                    let guard = h.lock();
                    guard.on_verification_state_changed(verified);
                }
            }
            else => break,
        }
    }
}
