//! Background backfill orchestration and per-account app-cache DB.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

#[cfg(not(test))]
use crate::ffi::OpResult;

#[cfg(not(test))]
use super::ok;
#[cfg(not(test))]
use super::{build_room_infos, err, first_line, html_first_line, ClientFfi, LatestPreview};

/// Returned by `backfill_room_silent`: the preview extracted from the most
/// recent timeline item after back-pagination, for rooms where
/// `room.latest_event()` is still `None` (sync loop hasn't seen the event).
#[cfg(not(test))]
#[derive(Clone)]
pub(crate) struct BackfillPreview {
    pub(crate) room_id: String,
    pub(crate) kind: String,
    pub(crate) text: String,
    pub(crate) sticker_url: String,
    pub(crate) thumbnail_url: String,
    pub(crate) sender_name: String,
    pub(crate) timestamp_ms: u64,
}

#[cfg(not(test))]
use futures_util::StreamExt;
#[cfg(not(test))]
use matrix_sdk::ruma::OwnedRoomId;
#[cfg(not(test))]
use matrix_sdk::Client;
#[cfg(not(test))]
use matrix_sdk_ui::timeline::{
    MsgLikeContent, MsgLikeKind, RoomExt, TimelineDetails, TimelineItemContent, TimelineItemKind,
};
#[cfg(not(test))]
use parking_lot::Mutex;
#[cfg(not(test))]
use std::collections::HashMap;
#[cfg(not(test))]
use std::sync::Arc;

/// Concurrent `/messages?limit=1` timestamp fetches in the all-rooms backfill.
const TIMESTAMP_FETCH_CONCURRENCY: usize = 32;
/// Concurrent full-room-state fetches in the bridge-status check; each response
/// can be large, but the homeserver handles this level comfortably.
const BRIDGE_CHECK_CONCURRENCY: usize = 16;

/// Shortest gap between two backfill progress updates. Each update rebuilds
/// every room's summary and pushes the whole list, so a long backfill must not
/// send one per batch.
const BACKFILL_UPDATE_MIN_INTERVAL: std::time::Duration = std::time::Duration::from_secs(1);

/// First wait before retrying a room whose backfill failed. Doubles per failure.
const BACKFILL_RETRY_BASE: std::time::Duration = std::time::Duration::from_secs(30);
/// Longest wait between two attempts on the same room.
const BACKFILL_RETRY_CAP: std::time::Duration = std::time::Duration::from_secs(10 * 60);

/// How long a room waits before its next backfill attempt, after `attempts`
/// failed attempts in a row: 30 s, doubling, capped at 10 minutes.
fn backfill_retry_delay(attempts: u32) -> std::time::Duration {
    let shift = attempts.saturating_sub(1).min(16);
    BACKFILL_RETRY_BASE
        .saturating_mul(1u32 << shift)
        .min(BACKFILL_RETRY_CAP)
}

/// A room whose backfill attempt failed: how many attempts have failed, and the
/// earliest time the next attempt may run.
#[cfg(not(test))]
pub(super) struct BackfillRetry {
    pub(super) attempts: u32,
    pub(super) next_at: std::time::Instant,
}

/// Records one more failed backfill attempt for `rid`, and when it may be tried again.
#[cfg(not(test))]
fn note_backfill_failure(retry: &Mutex<HashMap<String, BackfillRetry>>, rid: &str) {
    let now = std::time::Instant::now();
    let mut map = retry.lock();
    let entry = map.entry(rid.to_owned()).or_insert(BackfillRetry {
        attempts: 0,
        next_at: now,
    });
    entry.attempts = entry.attempts.saturating_add(1);
    entry.next_at = now + backfill_retry_delay(entry.attempts);
}

/// Rooms whose last backfill attempt failed and whose retry time has not come yet.
#[cfg(not(test))]
fn backfill_waiting_rooms(
    retry: &Mutex<HashMap<String, BackfillRetry>>,
) -> std::collections::HashSet<String> {
    let now = std::time::Instant::now();
    retry
        .lock()
        .iter()
        .filter(|(_, r)| r.next_at > now)
        .map(|(k, _)| k.clone())
        .collect()
}

/// Whether a backfill progress update is due: `every` rooms have finished since
/// the last one, and the minimum interval has passed since it went out
/// (`since_last_emit` is `None` before the first).
fn backfill_update_due(
    completed: usize,
    last_update_at: usize,
    every: usize,
    since_last_emit: Option<std::time::Duration>,
) -> bool {
    completed - last_update_at >= every
        && since_last_emit.map_or(true, |d| d >= BACKFILL_UPDATE_MIN_INTERVAL)
}

#[cfg(not(test))]
impl ClientFfi {
    // -----------------------------------------------------------------------
    // Background backfill of non-active rooms
    // -----------------------------------------------------------------------

    // Shared orchestrator: spawn a bounded-concurrency backfill task for the
    // given room list. Callers are responsible for the idempotency guard and
    // for ensuring `to_backfill` is already filtered. No-op on empty input.
    #[cfg(not(test))]
    fn launch_backfill_task_(&self, client: matrix_sdk::Client, to_backfill: Vec<OwnedRoomId>) {
        if to_backfill.is_empty() {
            return;
        }

        let handler = self.handler.clone();
        let preview_cache = Arc::clone(&self.backfill_previews);
        let db_conn = Arc::clone(&self.app_cache_db);
        let in_flight = Arc::clone(&self.in_flight);
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        // Shared with the unread prefetch so combined warm-up concurrency is
        // bounded, not summed.
        let semaphore = Arc::clone(&self.warm_semaphore);
        let retry = Arc::clone(&self.backfill_retry);

        // Emit on_rooms_updated every UPDATE_EVERY completions so the
        // inactive-room count ticks up visibly during a long backfill run,
        // plus once at completion if the total isn't an exact multiple.
        const UPDATE_EVERY: usize = 5;

        let abort = self
            .rt
            .spawn(async move {
                let _job = super::activity::begin(
                    "inactive-room-timestamp-backfill",
                    "Backfill",
                    super::activity::JobKind::OneShot,
                );
                let mut joinset = tokio::task::JoinSet::new();

                for rid in to_backfill {
                    let client = client.clone();
                    let sem = semaphore.clone();
                    let preview_cache = preview_cache.clone();
                    let retry = retry.clone();
                    let db_conn = Arc::clone(&db_conn);
                    let in_flight = Arc::clone(&in_flight);
                    #[cfg(debug_assertions)]
                    let in_flight_urls = Arc::clone(&in_flight_urls);
                    let handler_ref = handler.clone();
                    #[cfg(debug_assertions)]
                    let rid_label = rid.to_string();
                    joinset.spawn(async move {
                        let _permit = match sem.acquire_owned().await {
                            Ok(p) => p,
                            Err(_) => return,
                        };
                        let _guard = super::InFlightGuard::new(
                            &in_flight,
                            &handler_ref,
                            #[cfg(debug_assertions)]
                            &in_flight_urls,
                            #[cfg(debug_assertions)]
                            format!("backfill/{}", rid_label),
                        );
                        let Ok(preview) = backfill_room_silent(&client, &rid, 50).await else {
                            // Not cached as "no timestamp": a later request retries
                            // this room once its backoff has passed.
                            note_backfill_failure(&retry, rid.as_str());
                            return;
                        };
                        retry.lock().remove(rid.as_str());
                        // A room that resolved to no timestamp is still cached, so it
                        // is not re-queued on every on_rooms_updated tick. The
                        // zero-timestamp sentinel is not written to backfill_ts SQLite
                        // (guarded by ts != 0 below).
                        let bp_to_cache = preview.clone().unwrap_or(BackfillPreview {
                            room_id: rid.to_string(),
                            kind: String::new(),
                            text: String::new(),
                            sticker_url: String::new(),
                            thumbnail_url: String::new(),
                            sender_name: String::new(),
                            timestamp_ms: 0,
                        });
                        {
                            let mut cache = preview_cache.lock();
                            cache.insert(bp_to_cache.room_id.clone(), bp_to_cache);
                        }
                        if let Some(ref bp) = preview {
                            // Write the timestamp immediately so it survives
                            // even if the task is aborted before the batch update.
                            if bp.timestamp_ms != 0 {
                                {
                                    let db = db_conn.lock();
                                    if let Some(ref conn) = *db {
                                        let _ = conn.execute(
                                            "INSERT OR REPLACE INTO backfill_ts \
                                             (room_id, ts_ms) VALUES (?1, ?2)",
                                            rusqlite::params![&bp.room_id, bp.timestamp_ms as i64],
                                        );
                                    }
                                }
                            }
                        }
                    });
                }

                // Drive the joinset, emitting an update every UPDATE_EVERY rooms.
                let mut completed: usize = 0;
                let mut last_update_at: usize = 0;
                let mut last_emit: Option<std::time::Instant> = None;
                while joinset.join_next().await.is_some() {
                    completed += 1;
                    let since_last = last_emit.map(|t| t.elapsed());
                    if backfill_update_due(completed, last_update_at, UPDATE_EVERY, since_last) {
                        last_update_at = completed;
                        last_emit = Some(std::time::Instant::now());
                        if let Some(ref h) = handler {
                            let mut rooms = build_room_infos(&client, &db_conn).await;
                            apply_backfill_previews(&mut rooms, &preview_cache);
                            {
                                let guard = h.lock();
                                guard.on_rooms_updated(&rooms);
                            }
                        }
                    }
                }
                // Final update if the last batch didn't land on a boundary.
                if completed != last_update_at {
                    if let Some(ref h) = handler {
                        let mut rooms = build_room_infos(&client, &db_conn).await;
                        apply_backfill_previews(&mut rooms, &preview_cache);
                        {
                            let guard = h.lock();
                            guard.on_rooms_updated(&rooms);
                        }
                    }
                }
            })
            .abort_handle();

        *self.backfill_task.lock() = Some(abort);
    }

    #[cfg(not(test))]
    pub fn start_background_backfill(&self, room_ids: &cxx::CxxVector<cxx::CxxString>) -> OpResult {
        // Idempotent: if a previous orchestrator is still running, leave it
        // alone. Finished/aborted handles can be replaced.
        {
            let guard = self.backfill_task.lock();
            if let Some(h) = guard.as_ref() {
                if !h.is_finished() {
                    return ok("");
                }
            }
        }
        *self.backfill_task.lock() = None;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        // Snapshot the work set up-front so the orchestrator owns no
        // borrows of `self`. Skip rooms that already have a foreground
        // Timeline (the user-active one).
        let skip: std::collections::HashSet<OwnedRoomId> =
            self.timelines.read().keys().cloned().collect();

        let cached: std::collections::HashSet<String> = {
            let guard = self.backfill_previews.lock();
            guard.keys().cloned().collect()
        };
        let waiting = backfill_waiting_rooms(&self.backfill_retry);

        let mut to_backfill: Vec<OwnedRoomId> = Vec::new();
        for id_cxx in room_ids {
            let Ok(id_str) = id_cxx.to_str() else {
                continue;
            };
            let Ok(id) = OwnedRoomId::try_from(id_str) else {
                continue;
            };
            if skip.contains(&id) {
                continue;
            }
            if cached.contains(id.as_str()) || waiting.contains(id.as_str()) {
                continue;
            }
            if let Some(room) = client.get_room(&id) {
                if room.is_tombstoned() {
                    continue;
                }
                if room.latest_event_timestamp().is_some() {
                    continue;
                }
                to_backfill.push(id);
            }
        }

        self.launch_backfill_task_(client, to_backfill);
        ok("")
    }

    /// Backfill every joined room whose timestamp is absent from the in-memory
    /// backfill cache (`backfill_previews`) and whose SDK event cache has no
    /// `latest_event_timestamp`. Used when the "group inactive rooms" feature
    /// is active so that off-screen rooms get a `last_activity_ts` for correct
    /// inactive-section classification.
    #[cfg(not(test))]
    pub fn start_background_backfill_all_uncached(&self) -> OpResult {
        // Same idempotency guard as start_background_backfill, on this pass's
        // own handle so the visible-rooms backfill never cancels or blocks it.
        {
            let guard = self.uncached_backfill_task.lock();
            if let Some(h) = guard.as_ref() {
                if !h.is_finished() {
                    return ok("");
                }
            }
        }
        *self.uncached_backfill_task.lock() = None;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        // Rooms already in backfill_previews have a cached timestamp (either
        // loaded from backfill_ts on startup or written during this session).
        let cached: std::collections::HashSet<String> = {
            let guard = self.backfill_previews.lock();
            guard.keys().cloned().collect()
        };
        let waiting = backfill_waiting_rooms(&self.backfill_retry);

        // Skip the room currently open in the foreground timeline.
        let skip: std::collections::HashSet<OwnedRoomId> =
            self.timelines.read().keys().cloned().collect();

        let mut to_backfill: Vec<OwnedRoomId> = Vec::new();
        let (mut n_joined, mut n_open_or_tombstoned, mut n_cached, mut n_waiting, mut n_has_ts) =
            (0usize, 0usize, 0usize, 0usize, 0usize);
        for room in client.joined_rooms() {
            n_joined += 1;
            let id = room.room_id().to_owned();
            if skip.contains(&id) || room.is_tombstoned() {
                n_open_or_tombstoned += 1;
                continue;
            }
            // Already cached — timestamp known from backfill_ts or this session.
            if cached.contains(id.as_str()) {
                n_cached += 1;
                continue;
            }
            if waiting.contains(id.as_str()) {
                n_waiting += 1;
                continue;
            }
            // SDK already has a timestamp from live sync — no backfill needed.
            if room.latest_event_timestamp().is_some() {
                n_has_ts += 1;
                continue;
            }
            to_backfill.push(id);
        }
        crate::request_log::note(format!(
            "uncached-backfill pass: joined={n_joined} to_fetch={} already_cached={n_cached} \
             in_backoff={n_waiting} sdk_has_ts={n_has_ts} open_or_tombstoned={n_open_or_tombstoned}",
            to_backfill.len()
        ));

        let handler = self.handler.clone();
        let preview_cache = Arc::clone(&self.backfill_previews);
        let retry = Arc::clone(&self.backfill_retry);
        let db_conn = Arc::clone(&self.app_cache_db);
        let in_flight = self.in_flight_ctx();

        let abort = self
            .rt
            .spawn(async move {
                let _job = super::activity::begin(
                    "uncached-room-backfill",
                    "Backfill",
                    super::activity::JobKind::OneShot,
                );
                // The SDK's own latest-event timestamp is no use for these
                // rooms: sync delivers one event per room and the SDK ignores
                // encrypted (not yet decryptable), reaction, redaction and most
                // state events, so a room whose newest event is one of those
                // stays without a timestamp indefinitely — always the case for
                // undecryptable rooms on a freshly signed-in device. Ask the
                // server for the newest event of any kind instead: one small
                // /messages request per room, no timeline built.
                const UPDATE_EVERY: usize = 5;
                const MAX_ROUNDS: u32 = 4;
                let permits = Arc::new(tokio::sync::Semaphore::new(TIMESTAMP_FETCH_CONCURRENCY));
                let mut todo = to_backfill;
                let mut round: u32 = 0;
                while !todo.is_empty() {
                    round += 1;
                    let mut joinset = tokio::task::JoinSet::<Option<OwnedRoomId>>::new();

                    for rid in std::mem::take(&mut todo) {
                        let client = client.clone();
                        let permits = Arc::clone(&permits);
                        let preview_cache = Arc::clone(&preview_cache);
                        let retry = Arc::clone(&retry);
                        let db_conn = Arc::clone(&db_conn);
                        let in_flight = in_flight.clone();
                        joinset.spawn(async move {
                            let Ok(_permit) = permits.acquire().await else {
                                return Some(rid);
                            };
                            let _in_flight = in_flight.guard(format!("backfill-ts/{rid}"));
                            let ts_ms = match newest_event_timestamp_ms(&client, &rid).await {
                                Ok(ts) => ts,
                                Err(e) => {
                                    tracing::warn!("backfill of {rid} failed: {e:#}");
                                    note_backfill_failure(&retry, rid.as_str());
                                    return Some(rid);
                                }
                            };
                            retry.lock().remove(rid.as_str());
                            // A room with no events at all is still cached (with
                            // a zero timestamp) so it isn't re-queued each pass.
                            {
                                let mut cache = preview_cache.lock();
                                cache.insert(
                                    rid.to_string(),
                                    BackfillPreview {
                                        room_id: rid.to_string(),
                                        kind: String::new(),
                                        text: String::new(),
                                        sticker_url: String::new(),
                                        thumbnail_url: String::new(),
                                        sender_name: String::new(),
                                        timestamp_ms: ts_ms,
                                    },
                                );
                            }
                            if ts_ms != 0 {
                                let db = db_conn.lock();
                                if let Some(ref conn) = *db {
                                    let _ = conn.execute(
                                        "INSERT OR REPLACE INTO backfill_ts \
                                         (room_id, ts_ms) VALUES (?1, ?2)",
                                        rusqlite::params![rid.as_str(), ts_ms as i64],
                                    );
                                }
                            }
                            None
                        });
                    }

                    let mut failed: Vec<OwnedRoomId> = Vec::new();
                    let mut completed: usize = 0;
                    let mut last_update_at: usize = 0;
                    let mut last_emit: Option<std::time::Instant> = None;
                    while let Some(res) = joinset.join_next().await {
                        if let Ok(Some(rid)) = res {
                            failed.push(rid);
                        }
                        completed += 1;
                        let since_last = last_emit.map(|t| t.elapsed());
                        if backfill_update_due(completed, last_update_at, UPDATE_EVERY, since_last)
                        {
                            last_update_at = completed;
                            last_emit = Some(std::time::Instant::now());
                            if let Some(ref h) = handler {
                                let mut rooms = build_room_infos(&client, &db_conn).await;
                                apply_backfill_previews(&mut rooms, &preview_cache);
                                {
                                    let guard = h.lock();
                                    guard.on_rooms_updated(&rooms);
                                }
                            }
                        }
                    }
                    if completed != last_update_at {
                        if let Some(ref h) = handler {
                            let mut rooms = build_room_infos(&client, &db_conn).await;
                            apply_backfill_previews(&mut rooms, &preview_cache);
                            {
                                let guard = h.lock();
                                guard.on_rooms_updated(&rooms);
                            }
                        }
                    }

                    crate::request_log::note(format!(
                        "uncached-backfill round {round}: fetched={completed} failed={}",
                        failed.len()
                    ));
                    if failed.is_empty() {
                        break;
                    }
                    if round >= MAX_ROUNDS {
                        tracing::warn!(
                            "backfill gave up on {} room(s) this pass; they retry on a later pass",
                            failed.len()
                        );
                        break;
                    }
                    // Sleep until the earliest backoff expires, then retry just
                    // the rooms that failed.
                    let next_at = {
                        let map = retry.lock();
                        failed.iter().filter_map(|r| map.get(r.as_str()).map(|e| e.next_at)).min()
                    };
                    if let Some(t) = next_at {
                        tokio::time::sleep_until(tokio::time::Instant::from_std(t)).await;
                    }
                    todo = failed;
                }

            })
            .abort_handle();

        *self.uncached_backfill_task.lock() = Some(abort);
        ok("")
    }

    #[cfg(not(test))]
    pub fn stop_background_backfill(&self) {
        self.stop_visible_backfill();
        if let Some(h) = self.uncached_backfill_task.lock().take() {
            h.abort();
        }
    }

    /// Cancel only the visible-rooms backfill, leaving the all-rooms
    /// (inactive-grouping) pass running.
    #[cfg(not(test))]
    pub fn stop_visible_backfill(&self) {
        if let Some(h) = self.backfill_task.lock().take() {
            h.abort();
        }
    }

    // -----------------------------------------------------------------------
    // On-demand bridge-status check
    // -----------------------------------------------------------------------

    /// For each room_id in the list that has no entry in the bridge_status
    /// SQLite cache, fetch GET /rooms/{id}/state, filter for uk.half-shot.bridge,
    /// and persist the result. If any new bridged room is found, emits
    /// on_rooms_updated so the call button / threads UI updates immediately.
    /// Idempotent: if a check is still in flight the call is a no-op.
    /// Called by the C++ shell whenever the visible room set changes.
    #[cfg(not(test))]
    pub fn start_bridge_status_check(&self, room_ids: &cxx::CxxVector<cxx::CxxString>) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        let ids: Vec<matrix_sdk::ruma::OwnedRoomId> = room_ids
            .iter()
            .filter_map(|s| s.to_str().ok()?.parse().ok())
            .collect();

        // A check is still running: hand it the newest set to process when its
        // current pass finishes (coalesced, only the latest is kept) so rooms
        // scrolled into view mid-check are not dropped.
        {
            let guard = self.bridge_check_task.lock();
            if let Some(h) = guard.as_ref() {
                if !h.is_finished() {
                    *self.pending_bridge_check.lock() = Some(ids);
                    return ok("");
                }
            }
        }
        *self.bridge_check_task.lock() = None;
        *self.pending_bridge_check.lock() = None;

        let uncached = uncached_bridge_rooms(&self.app_cache_db, ids);
        if uncached.is_empty() {
            return ok("");
        }
        let pending = Arc::clone(&self.pending_bridge_check);

        let handler = self.handler.clone();
        let preview_cache = Arc::clone(&self.backfill_previews);
        let db_conn = Arc::clone(&self.app_cache_db);
        let in_flight = Arc::clone(&self.in_flight);
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);

        let handle = self.rt.spawn(async move {
            let _job = super::activity::begin("bridge-status-check", "Backfill", super::activity::JobKind::OneShot);
            use matrix_sdk::ruma::api::client::state::get_state_events::v3 as state_api;

            let mut uncached = uncached;
            loop {
                // Fetch all rooms in parallel — room state is independent,
                // no ordering needed.
                #[allow(clippy::type_complexity)]
                let mut joinset: tokio::task::JoinSet<
                    Option<(
                        matrix_sdk::ruma::OwnedRoomId,
                        bool,
                        Option<String>,
                        Option<String>,
                        crate::ffi::BridgeCapabilities,
                    )>,
                > = tokio::task::JoinSet::new();
                // Full room state can be tens of megabytes per room, so only a few
                // lookups run at once.
                let permits = Arc::new(tokio::sync::Semaphore::new(BRIDGE_CHECK_CONCURRENCY));
                for room_id in uncached {
                    let client = client.clone();
                    let permits = Arc::clone(&permits);
                    let in_flight = Arc::clone(&in_flight);
                    let handler = handler.clone();
                    #[cfg(debug_assertions)]
                    let in_flight_urls = Arc::clone(&in_flight_urls);
                    #[cfg(debug_assertions)]
                    let label = format!("bridge/{room_id}");
                    joinset.spawn(async move {
                        let _guard = super::InFlightGuard::new(
                            &in_flight,
                            &handler,
                            #[cfg(debug_assertions)]
                            &in_flight_urls,
                            #[cfg(debug_assertions)]
                            label,
                        );
                        let Ok(_permit) = permits.acquire().await else {
                            return None;
                        };
                        // A failed lookup is not an answer: returning None keeps it out of
                        // the table, so the next check asks again instead of recording
                        // "not bridged" for a room whose state was never read.
                        let room_state = match client.send(state_api::Request::new(room_id.clone())).await {
                            Ok(resp) => resp.room_state,
                            Err(e) => {
                                tracing::warn!("bridge status lookup for {room_id} failed (not recorded): {e}");
                                return None;
                            }
                        };
                        let find_event = |event_type: &str| {
                            room_state.iter().find(|raw| {
                                raw.get_field::<String>("type").ok().flatten().as_deref()
                                    == Some(event_type)
                            })
                        };
                        let bridge_event = find_event("uk.half-shot.bridge");
                        let bridged = bridge_event.is_some();
                        let bridge_content = bridge_event.and_then(|raw| {
                            raw.get_field::<serde_json::Value>("content").ok().flatten()
                        });
                        let (network_name, network_avatar_url) = bridge_content
                            .as_ref()
                            .map(super::parse_bridge_network_info)
                            .unwrap_or((None, None));
                        let capabilities = find_event("com.beeper.room_features")
                            .and_then(|raw| {
                                raw.get_field::<serde_json::Value>("content").ok().flatten()
                            })
                            .map(|c| super::parse_bridge_capabilities(&c))
                            .unwrap_or_default();
                        Some((room_id, bridged, network_name, network_avatar_url, capabilities))
                    });
                }

                let mut any_bridged = false;
                while let Some(joined) = joinset.join_next().await {
                    let Ok(Some((room_id, bridged, network_name, network_avatar_url, capabilities))) =
                        joined
                    else {
                        continue;
                    };
                    {
                        let guard = db_conn.lock();
                        if let Some(conn) = guard.as_ref() {
                            let _ = conn.execute(
                                "INSERT OR REPLACE INTO bridge_status \
                                 (room_id, is_bridged, network_name, network_avatar_url, cap_threads, checked_at) \
                                 VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                                rusqlite::params![
                                    room_id.as_str(),
                                    bridged as i32,
                                    network_name,
                                    network_avatar_url,
                                    capabilities.threads as i32,
                                    unix_now_secs()
                                ],
                            );
                        }
                    }
                    if bridged {
                        any_bridged = true;
                    }
                }

                // Always re-emit after the check so the UI picks up the confirmed
                // is_bridged values (even if none are bridged, the C++ side needs
                // the updated data to re-evaluate the call button / badge).
                let _ = any_bridged; // suppress unused-variable warning when no rooms bridged
                if let Some(ref h) = handler {
                    let mut rooms = super::build_room_infos(&client, &db_conn).await;
                    apply_backfill_previews(&mut rooms, &preview_cache);
                    let guard = h.lock();
                    guard.on_rooms_updated(&rooms);
                }

                // Pick up a set requested while this pass ran.
                match pending.lock().take() {
                    Some(next) => {
                        uncached = uncached_bridge_rooms(&db_conn, next);
                        if uncached.is_empty() {
                            break;
                        }
                    }
                    None => break,
                }
            }
        });

        *self.bridge_check_task.lock() = Some(handle.abort_handle());
        ok("")
    }

    // -----------------------------------------------------------------------
    // One-shot unread prefetch
    // -----------------------------------------------------------------------

    // Spawn a bounded-concurrency, silent prefetch over `initial` and return its
    // abort handle (the caller places it into `prefetch_task`). Unlike
    // `launch_backfill_task_` this does NOT write the handle into `self`, so the
    // unread prefetch and the inactive-grouping backfill keep independent
    // handles and never abort one another. After each batch it drains
    // `pending_prefetch` and loops, so a set requested while it was running (new
    // messages arriving mid-prefetch) is picked up instead of being dropped.
    #[cfg(not(test))]
    fn spawn_prefetch_task_(
        &self,
        client: matrix_sdk::Client,
        initial: Vec<OwnedRoomId>,
    ) -> tokio::task::AbortHandle {
        let in_flight = Arc::clone(&self.in_flight);
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler = self.handler.clone();
        // Shared with the inactive-grouping backfill so combined warm-up
        // concurrency is bounded, not summed.
        let semaphore = Arc::clone(&self.warm_semaphore);
        let pending = Arc::clone(&self.pending_prefetch);

        self.rt
            .spawn(async move {
                let _job = super::activity::begin(
                    "unread-prefetch",
                    "Backfill",
                    super::activity::JobKind::OneShot,
                );
                let mut batch = initial;
                loop {
                    let mut joinset = tokio::task::JoinSet::new();
                    for rid in batch {
                        let client = client.clone();
                        let sem = semaphore.clone();
                        let in_flight = Arc::clone(&in_flight);
                        #[cfg(debug_assertions)]
                        let in_flight_urls = Arc::clone(&in_flight_urls);
                        let handler_ref = handler.clone();
                        #[cfg(debug_assertions)]
                        let rid_label = rid.to_string();
                        joinset.spawn(async move {
                            let _permit = match sem.acquire_owned().await {
                                Ok(p) => p,
                                Err(_) => return,
                            };
                            let _guard = super::InFlightGuard::new(
                                &in_flight,
                                &handler_ref,
                                #[cfg(debug_assertions)]
                                &in_flight_urls,
                                #[cfg(debug_assertions)]
                                format!("backfill/{}", rid_label),
                            );
                            // Warm the SDK event cache so the next open of this
                            // unread room renders from cache without a fetch
                            // hitch. The returned preview is intentionally unused
                            // — prefetch is silent (no on_rooms_updated, no
                            // backfill_ts write) to stay independent of the
                            // inactive-grouping bookkeeping.
                            let _ = backfill_room_silent(&client, &rid, 50).await;
                        });
                    }
                    while joinset.join_next().await.is_some() {}

                    // A sync that grew the unread set while this batch ran stashes
                    // the newest set in `pending`; pick it up so freshly-arrived
                    // messages get warmed too. Bounded — only the latest set is
                    // kept, so this loops at most once per intervening sync.
                    match pending.lock().take() {
                        Some(next) if !next.is_empty() => batch = next,
                        _ => break,
                    }
                }
            })
            .abort_handle()
    }

    // One-shot prefetch of recent messages for the given unread rooms into the
    // SDK event cache. The shell passes the already capped + LRU-ordered set, so
    // there is no sorting here. Unlike `start_background_backfill` this does NOT
    // skip rooms that already have a cached timestamp / latest_event — unread
    // rooms always have a timestamp yet still need their *newest* messages.
    // Skips rooms with a live foreground timeline (open room + pop-outs).
    // Idempotent while a prefetch is in flight.
    #[cfg(not(test))]
    pub fn start_unread_prefetch(&self, room_ids: &cxx::CxxVector<cxx::CxxString>) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        let skip: std::collections::HashSet<OwnedRoomId> =
            self.timelines.read().keys().cloned().collect();

        let ids: Vec<String> = room_ids
            .iter()
            .filter_map(|s| s.to_str().ok().map(str::to_owned))
            .collect();

        let to_prefetch = select_prefetch_rooms(&ids, &skip, |id| {
            client.get_room(id).map(|room| room.is_tombstoned())
        });

        if to_prefetch.is_empty() {
            return ok("");
        }

        // If a prefetch is still running, hand it the newest set to process when
        // its current batch finishes (coalesced — only the latest set is kept),
        // so messages that arrived mid-prefetch aren't lost rather than dropping
        // this request outright.
        {
            let guard = self.prefetch_task.lock();
            if let Some(h) = guard.as_ref() {
                if !h.is_finished() {
                    *self.pending_prefetch.lock() = Some(to_prefetch);
                    return ok("");
                }
            }
        }
        *self.prefetch_task.lock() = None;
        *self.pending_prefetch.lock() = None;

        let handle = self.spawn_prefetch_task_(client, to_prefetch);
        *self.prefetch_task.lock() = Some(handle);
        ok("")
    }

    #[cfg(not(test))]
    pub fn stop_unread_prefetch(&self) {
        if let Some(h) = self.prefetch_task.lock().take() {
            h.abort();
        }
        // Drop any coalesced follow-up set so a later task doesn't pick up a
        // stale request after an explicit stop.
        *self.pending_prefetch.lock() = None;
    }

    // ── Media backoff persistence ─────────────────────────────────────────

    pub fn load_media_backoff(&self) -> Vec<crate::ffi::MediaBackoffEntry> {
        let guard = self.app_cache_db.lock();
        let Some(conn) = guard.as_ref() else {
            return vec![];
        };
        query_media_backoff(conn)
            .into_iter()
            .map(
                |(url, attempts, deadline_secs)| crate::ffi::MediaBackoffEntry {
                    url,
                    attempts,
                    deadline_secs,
                },
            )
            .collect()
    }

    pub fn note_media_backoff_failed(&self, url: &str, attempts: u32, deadline_secs: i64) {
        let guard = self.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            upsert_media_backoff(conn, url, attempts, deadline_secs);
        }
    }

    pub fn note_media_backoff_ok(&self, url: &str) {
        let guard = self.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            delete_media_backoff(conn, url);
        }
    }

    pub fn clear_media_backoff_db(&self) {
        let guard = self.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            clear_media_backoff(conn);
        }
    }

    // ── Room summary backoff persistence ─────────────────────────────────

    pub fn load_room_summary_backoff(&self) -> Vec<crate::ffi::RoomSummaryBackoffEntry> {
        let guard = self.app_cache_db.lock();
        let Some(conn) = guard.as_ref() else {
            return vec![];
        };
        query_room_summary_backoff(conn)
            .into_iter()
            .map(
                |(room_id, attempts, deadline_secs)| crate::ffi::RoomSummaryBackoffEntry {
                    room_id,
                    attempts,
                    deadline_secs,
                },
            )
            .collect()
    }

    pub fn note_room_summary_backoff_failed(
        &self,
        room_id: &str,
        attempts: u32,
        deadline_secs: i64,
    ) {
        let guard = self.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            upsert_room_summary_backoff(conn, room_id, attempts, deadline_secs);
        }
    }

    pub fn note_room_summary_backoff_ok(&self, room_id: &str) {
        let guard = self.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            delete_room_summary_backoff(conn, room_id);
        }
    }
}

/// Event types that count as a room being active: what a person would call a
/// message. Membership, bridge and bot state, redactions and reactions are
/// left out, so a room whose last real message is months old is not kept
/// "active" by a ghost user joining or a bridge updating its state.
const ACTIVITY_EVENT_TYPES: &[&str] = &[
    "m.room.message",
    "m.room.encrypted",
    "m.sticker",
    "org.matrix.msc3381.poll.start",
    "m.poll.start",
    "m.call.invite",
    "m.rtc.notification",
];

/// Timestamp (ms since epoch) of the room's newest message-like event, from a
/// single server-side filtered `/messages?dir=b&limit=1`. A room with no
/// message-like event at all falls back to its newest event of any kind (one
/// more request); 0 when the room has no events.
#[cfg(not(test))]
async fn newest_event_timestamp_ms(client: &Client, room_id: &OwnedRoomId) -> anyhow::Result<u64> {
    use matrix_sdk::room::MessagesOptions;
    let Some(room) = client.get_room(room_id) else {
        return Ok(0);
    };
    let newest = |messages: matrix_sdk::room::Messages| {
        messages
            .chunk
            .first()
            .and_then(|ev| ev.timestamp())
            .map(|ts| u64::from(ts.0))
    };

    let mut filtered = MessagesOptions::backward();
    filtered.limit = matrix_sdk::ruma::uint!(1);
    filtered.filter.types = Some(ACTIVITY_EVENT_TYPES.iter().map(|t| (*t).to_owned()).collect());
    if let Some(ts) = newest(room.messages(filtered).await?) {
        return Ok(ts);
    }

    let mut any = MessagesOptions::backward();
    any.limit = matrix_sdk::ruma::uint!(1);
    Ok(newest(room.messages(any).await?).unwrap_or(0))
}

/// The subset of `ids` with no usable `bridge_status` row. A recorded answer
/// is kept: only a successful lookup writes a row, so there is no stale "not
/// bridged" from a failed fetch to expire.
#[cfg(not(test))]
fn uncached_bridge_rooms(
    app_cache_db: &parking_lot::Mutex<Option<rusqlite::Connection>>,
    ids: Vec<matrix_sdk::ruma::OwnedRoomId>,
) -> Vec<matrix_sdk::ruma::OwnedRoomId> {
    let guard = app_cache_db.lock();
    ids.into_iter()
        .filter(|id| {
            guard
                .as_ref()
                .and_then(|conn| {
                    conn.query_row(
                        // A bridged row cached before `cap_threads` existed has
                        // NULL there; treat it as uncached so it's re-fetched once.
                        "SELECT 1 FROM bridge_status WHERE room_id = ?1 \
                         AND (is_bridged = 0 OR cap_threads IS NOT NULL)",
                        rusqlite::params![id.as_str()],
                        |_| Ok(()),
                    )
                    .ok()
                })
                .is_none()
        })
        .collect()
}

/// Pure selection logic for unread prefetch, factored out so it can be unit
/// tested (the live orchestrator above is `#[cfg(not(test))]`). Given the
/// caller-supplied candidate room ids (already capped + LRU-ordered by the
/// shell), the set of rooms with a live foreground timeline to skip, and a
/// per-room status probe, return the rooms to warm — preserving input order.
///
/// `room_status` returns `None` when the room is unknown to the client and
/// `Some(tombstoned)` otherwise. Invalid room ids and unknown/tombstoned rooms
/// are dropped. Crucially — and unlike `start_background_backfill` — this does
/// NOT filter on cached timestamp: an unread room is prefetched even when it
/// already has a `latest_event_timestamp`, because we want its newest events.
fn select_prefetch_rooms<F>(
    room_ids: &[String],
    skip: &std::collections::HashSet<matrix_sdk::ruma::OwnedRoomId>,
    mut room_status: F,
) -> Vec<matrix_sdk::ruma::OwnedRoomId>
where
    F: FnMut(&matrix_sdk::ruma::OwnedRoomId) -> Option<bool>,
{
    let mut out = Vec::new();
    for id_str in room_ids {
        let Ok(id) = id_str.parse::<matrix_sdk::ruma::OwnedRoomId>() else {
            continue;
        };
        if skip.contains(&id) {
            continue;
        }
        if let Some(false) = room_status(&id) {
            out.push(id);
        }
    }
    out
}

/// Extract a room-list preview from a timeline item's content.
/// Mirrors `latest_event_preview` but operates on `TimelineItemContent`
/// instead of `LatestEventValue`, for use after back-pagination when the
/// sync-backed `room.latest_event()` hasn't been populated yet.
#[cfg(not(test))]
pub(super) fn preview_from_timeline_content(content: &TimelineItemContent) -> LatestPreview {
    use matrix_sdk::ruma::events::room::message::MessageType;
    use matrix_sdk::ruma::events::room::MediaSource;

    let text_kind = |body: &str, formatted: Option<&str>| -> LatestPreview {
        // A formatted body that sanitises to nothing (all spoiler) must not
        // fall back to the plain body, which has the text.
        let line = match formatted {
            Some(html) if !html.trim().is_empty() => html_first_line(html),
            _ => first_line(body),
        };
        if line.is_empty() {
            LatestPreview::default()
        } else {
            LatestPreview {
                kind: "text".to_owned(),
                text: line,
                ..Default::default()
            }
        }
    };

    match content {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(m),
            ..
        }) => match m.msgtype() {
            MessageType::Text(t) => {
                text_kind(&t.body, t.formatted.as_ref().map(|f| f.body.as_str()))
            }
            MessageType::Notice(n) => {
                text_kind(&n.body, n.formatted.as_ref().map(|f| f.body.as_str()))
            }
            MessageType::Emote(e) => {
                text_kind(&e.body, e.formatted.as_ref().map(|f| f.body.as_str()))
            }
            MessageType::Image(img) => {
                let url = match &img.source {
                    MediaSource::Plain(uri) => uri.to_string(),
                    MediaSource::Encrypted(_) => {
                        serde_json::to_string(&img.source).unwrap_or_default()
                    }
                };
                LatestPreview {
                    kind: "image".to_owned(),
                    thumbnail_url: url,
                    ..Default::default()
                }
            }
            MessageType::Video(_) => LatestPreview {
                kind: "video".to_owned(),
                ..Default::default()
            },
            MessageType::File(_) => LatestPreview {
                kind: "file".to_owned(),
                ..Default::default()
            },
            MessageType::Audio(_) => LatestPreview {
                kind: "audio".to_owned(),
                ..Default::default()
            },
            MessageType::Gallery(_) => LatestPreview {
                kind: "gallery".to_owned(),
                ..Default::default()
            },
            _ => LatestPreview::default(),
        },
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Sticker(_),
            ..
        }) => LatestPreview {
            kind: "sticker".to_owned(),
            ..Default::default()
        },
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Poll(state),
            ..
        }) => LatestPreview {
            kind: "poll".to_owned(),
            text: state.results().question,
            ..Default::default()
        },
        _ => LatestPreview::default(),
    }
}

/// Patch `rooms` in place: for any room whose `last_message_kind` is still
/// empty (i.e. `room.latest_event()` returned `None` in `build_room_infos`),
/// apply the cached preview from a previous back-pagination run.
#[cfg(not(test))]
pub(super) fn apply_backfill_previews(
    rooms: &mut Vec<crate::ffi::RoomInfo>,
    cache: &Mutex<HashMap<String, BackfillPreview>>,
) {
    let guard = cache.lock();
    for ri in rooms.iter_mut() {
        if let Some(bp) = guard.get(&ri.id) {
            if ri.last_message_kind.is_empty() && !bp.kind.is_empty() {
                ri.last_message_kind = bp.kind.clone();
                ri.last_message_body = bp.text.clone();
                ri.last_message_sticker_url = bp.sticker_url.clone();
                ri.last_message_thumbnail_url = bp.thumbnail_url.clone();
                ri.last_message_sender_name = bp.sender_name.clone();
            }
            if bp.timestamp_ms > ri.last_activity_ts {
                ri.last_activity_ts = bp.timestamp_ms;
            }
        }
    }
}

// ── App cache DB (per-account SQLite for persistent UI state) ─────────────

/// Makes the app-cache file owner-only (0600), creating it first if needed, so
/// it never exists wider than that. SQLite gives its WAL and SHM files the mode
/// of the database file, so they follow.
#[cfg(unix)]
fn restrict_app_cache_to_owner(path: &std::path::Path) {
    use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
    let _ = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .mode(0o600)
        .open(path);
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
    // Journal files left by an earlier run keep the mode they were created with.
    for suffix in ["-wal", "-shm"] {
        let mut side = path.as_os_str().to_owned();
        side.push(suffix);
        let side = std::path::PathBuf::from(side);
        if side.exists() {
            let _ = std::fs::set_permissions(&side, std::fs::Permissions::from_mode(0o600));
        }
    }
}

/// Open (or create) the per-account app-cache database and ensure the schema
/// exists. `data_dir` is already the per-account SDK store directory
/// (`<data>/accounts/<uid>/matrix-store/`), so the file lands there directly.
/// Returns `None` on any I/O or SQL error (treated as a soft failure).
#[cfg(not(test))]
pub(super) fn open_app_cache_db(data_dir: &std::path::Path) -> Option<rusqlite::Connection> {
    let path = data_dir.join("app_cache.db");
    #[cfg(unix)]
    restrict_app_cache_to_owner(&path);
    let conn = rusqlite::Connection::open(&path).ok()?;
    conn.execute_batch(
        "PRAGMA journal_mode=WAL;
         CREATE TABLE IF NOT EXISTS backfill_ts (
             room_id TEXT NOT NULL PRIMARY KEY,
             ts_ms   INTEGER NOT NULL
         );
         CREATE TABLE IF NOT EXISTS media_backoff (
             url      TEXT    NOT NULL PRIMARY KEY,
             attempts INTEGER NOT NULL,
             deadline INTEGER NOT NULL
         );
         CREATE TABLE IF NOT EXISTS room_summary_backoff (
             room_id  TEXT    NOT NULL PRIMARY KEY,
             attempts INTEGER NOT NULL,
             deadline INTEGER NOT NULL
         );
         CREATE TABLE IF NOT EXISTS room_summary_cache (
             room_id         TEXT    NOT NULL PRIMARY KEY,
             summary_json    TEXT    NOT NULL,
             fetched_at_secs INTEGER NOT NULL
         );
         CREATE TABLE IF NOT EXISTS presence_forbidden (
             user_id TEXT NOT NULL PRIMARY KEY
         );
         CREATE TABLE IF NOT EXISTS bridge_status (
             room_id    TEXT    NOT NULL PRIMARY KEY,
             is_bridged INTEGER NOT NULL
         );
         CREATE TABLE IF NOT EXISTS room_image_pack_cache (
             room_id      TEXT    NOT NULL,
             state_key    TEXT    NOT NULL,
             display_name TEXT    NOT NULL DEFAULT '',
             checked_at   INTEGER NOT NULL,
             PRIMARY KEY (room_id, state_key)
         );",
    )
    .ok()?;
    conn.execute_batch(super::history_export::store::CREATE_TABLE_SQL)
        .ok()?;
    super::history_export::store::ensure_stop_at_ts_column(&conn).ok()?;
    ensure_bridge_status_columns(&conn).ok()?;
    conn.execute_batch(super::room_media_store::CREATE_TABLE_SQL)
        .ok()?;
    reset_backfill_ts_once(&conn);
    prune_stale_backoff_and_cache_rows(&conn);
    Some(conn)
}

/// Timestamps cached before the activity filter (`ACTIVITY_EVENT_TYPES`) came
/// from the newest event of any kind, which kept rooms with only recent state
/// changes looking active. Drop them once so every room is fetched again.
fn reset_backfill_ts_once(conn: &rusqlite::Connection) {
    const SCHEMA_VERSION: i64 = 1;
    let version: i64 = conn
        .query_row("PRAGMA user_version", [], |r| r.get(0))
        .unwrap_or(SCHEMA_VERSION);
    if version < SCHEMA_VERSION {
        let _ = conn.execute("DELETE FROM backfill_ts", []);
        let _ = conn.execute_batch(&format!("PRAGMA user_version = {SCHEMA_VERSION}"));
    }
}

/// `bridge_status` shipped with only `is_bridged`; `network_name` and
/// `network_avatar_url` were added later so the UI can show which platform a
/// room is bridged to instead of a generic "Bridged" label, and `cap_threads`
/// (one nullable column per bridge capability, NULL = not fetched yet) after
/// that. Same
/// check-then-`ALTER TABLE` shape as `ensure_stop_at_ts_column` in
/// `history_export::store`, since `ALTER TABLE ADD COLUMN` errors on a column
/// that already exists.
fn ensure_bridge_status_columns(conn: &rusqlite::Connection) -> rusqlite::Result<()> {
    let mut stmt = conn.prepare("PRAGMA table_info(bridge_status)")?;
    let existing: std::collections::HashSet<String> = stmt
        .query_map([], |row| row.get::<_, String>(1))?
        .filter_map(Result::ok)
        .collect();
    drop(stmt);
    if !existing.contains("network_name") {
        conn.execute("ALTER TABLE bridge_status ADD COLUMN network_name TEXT", [])?;
    }
    if !existing.contains("network_avatar_url") {
        conn.execute(
            "ALTER TABLE bridge_status ADD COLUMN network_avatar_url TEXT",
            [],
        )?;
    }
    if !existing.contains("cap_threads") {
        conn.execute(
            "ALTER TABLE bridge_status ADD COLUMN cap_threads INTEGER",
            [],
        )?;
    }
    if !existing.contains("checked_at") {
        conn.execute(
            "ALTER TABLE bridge_status ADD COLUMN checked_at INTEGER",
            [],
        )?;
    }
    Ok(())
}

fn unix_now_secs() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// Deletes rows old enough that they're almost certainly abandoned rather
/// than mid-retry, from all three tables that would otherwise grow for the
/// life of the account (nothing else prunes a `media_backoff`/
/// `room_summary_backoff` row for a URL/room that's simply never retried
/// again — success is the only other path that removes one, via
/// `delete_media_backoff`/`delete_room_summary_backoff`). The exponential
/// backoff schedule in `ShellBase::note_media_fetch_failed_` (and its
/// room-summary equivalent) caps the deadline at 30 minutes into the
/// future, so any `deadline` this far in the past means nothing has
/// retried in a very long time. Same 30-day window as the pre-existing
/// `room_summary_cache` content-staleness sweep, for consistency. Not
/// gated by `#[cfg(not(test))]` (unlike its caller) so the SQL itself stays
/// unit-testable below. Best-effort: a failure here just means the sweep
/// runs again next time the DB is opened.
pub(super) fn prune_stale_backoff_and_cache_rows(conn: &rusqlite::Connection) {
    let _ = conn.execute_batch(
        "DELETE FROM media_backoff
             WHERE deadline < strftime('%s','now') - 2592000;
         DELETE FROM room_summary_backoff
             WHERE deadline < strftime('%s','now') - 2592000;
         DELETE FROM room_summary_cache
             WHERE fetched_at_secs < strftime('%s','now') - 2592000;",
    );
    super::history_export::store::prune_stale_checkpoints(conn);
}

/// Open (or create) the per-account search-index database at
/// `data_dir/search_index.db` and ensure the FTS schema exists.
/// Separate from `app_cache.db` so the FTS5 tables live in their own file
/// and can be cleared independently of the backfill state.
/// Returns `None` on any I/O or SQL error (treated as a soft failure).
#[cfg(not(test))]
pub(super) fn open_search_db(data_dir: &std::path::Path) -> Option<rusqlite::Connection> {
    let path = data_dir.join("search_index.db");
    let conn = rusqlite::Connection::open(&path).ok()?;
    conn.execute_batch("PRAGMA journal_mode=WAL;").ok()?;
    super::search::ensure_schema(&conn).ok()?;
    Some(conn)
}

/// Open (or create) the per-account thread-read-state database at
/// `data_dir/thread_read_state.db` and ensure its schema exists.
///
/// Deliberately its own file, not a table in `app_cache.db`: it holds user
/// read-state, not cache, and must survive "Clear all caches"
/// (`wipe_local_stores` deletes `app_cache.db` but leaves this one). It only
/// exists because the homeserver's MSC3960 sliding-sync receipts extension
/// does not reliably echo the user's own threaded read receipts back into the
/// SDK state store (element-hq/synapse#17247), so the optimistic marker
/// `ClientFfi::thread_read_markers` writes here has to be durable.
/// Returns `None` on any I/O or SQL error (treated as a soft failure).
#[cfg(not(test))]
pub(super) fn open_thread_read_db(data_dir: &std::path::Path) -> Option<rusqlite::Connection> {
    let path = data_dir.join("thread_read_state.db");
    let conn = rusqlite::Connection::open(&path).ok()?;
    conn.execute_batch(
        "PRAGMA journal_mode=WAL;
         CREATE TABLE IF NOT EXISTS thread_read_marker (
             room_id TEXT    NOT NULL,
             root_id TEXT    NOT NULL,
             ts_ms   INTEGER NOT NULL,
             PRIMARY KEY (room_id, root_id)
         );",
    )
    .ok()?;
    Some(conn)
}

/// Populate `cache` from the already-open `conn`. Uses `or_insert` so a
/// live-sync entry (with full preview content) is never overwritten by the
/// leaner persisted-only entry.
#[cfg(not(test))]
pub(super) fn load_backfill_ts_conn(
    conn: &rusqlite::Connection,
    cache: &Mutex<HashMap<String, BackfillPreview>>,
) {
    let Ok(mut stmt) = conn.prepare("SELECT room_id, ts_ms FROM backfill_ts") else {
        return;
    };
    let Ok(rows) = stmt.query_map([], |row| {
        Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
    }) else {
        return;
    };
    let mut guard = cache.lock();
    for row in rows.flatten() {
        let (room_id, ts_ms) = row;
        if ts_ms <= 0 {
            continue;
        }
        guard.entry(room_id.clone()).or_insert(BackfillPreview {
            room_id,
            kind: String::new(),
            text: String::new(),
            sticker_url: String::new(),
            thumbnail_url: String::new(),
            sender_name: String::new(),
            timestamp_ms: ts_ms as u64,
        });
    }
}

// ── media_backoff DB helpers ──────────────────────────────────────────────
// Available in test builds (no cfg gate) so the unit-test module can call them.

pub(super) fn query_media_backoff(conn: &rusqlite::Connection) -> Vec<(String, u32, i64)> {
    let Ok(mut stmt) = conn.prepare("SELECT url, attempts, deadline FROM media_backoff") else {
        return vec![];
    };
    let Ok(rows) = stmt.query_map([], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get::<_, u32>(1)?,
            row.get::<_, i64>(2)?,
        ))
    }) else {
        return vec![];
    };
    rows.filter_map(|r| r.ok()).collect()
}

pub(super) fn upsert_media_backoff(
    conn: &rusqlite::Connection,
    url: &str,
    attempts: u32,
    deadline: i64,
) {
    let _ = conn.execute(
        "INSERT OR REPLACE INTO media_backoff (url, attempts, deadline) \
         VALUES (?1, ?2, ?3)",
        rusqlite::params![url, attempts, deadline],
    );
}

pub(super) fn delete_media_backoff(conn: &rusqlite::Connection, url: &str) {
    let _ = conn.execute("DELETE FROM media_backoff WHERE url = ?1", [url]);
}

pub(super) fn clear_media_backoff(conn: &rusqlite::Connection) {
    let _ = conn.execute("DELETE FROM media_backoff", []);
}

// ── room_summary_backoff DB helpers ───────────────────────────────────────

pub(super) fn query_room_summary_backoff(conn: &rusqlite::Connection) -> Vec<(String, u32, i64)> {
    let Ok(mut stmt) = conn.prepare("SELECT room_id, attempts, deadline FROM room_summary_backoff")
    else {
        return vec![];
    };
    let Ok(rows) = stmt.query_map([], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get::<_, u32>(1)?,
            row.get::<_, i64>(2)?,
        ))
    }) else {
        return vec![];
    };
    rows.filter_map(|r| r.ok()).collect()
}

pub(super) fn upsert_room_summary_backoff(
    conn: &rusqlite::Connection,
    room_id: &str,
    attempts: u32,
    deadline: i64,
) {
    let _ = conn.execute(
        "INSERT OR REPLACE INTO room_summary_backoff (room_id, attempts, deadline) \
         VALUES (?1, ?2, ?3)",
        rusqlite::params![room_id, attempts, deadline],
    );
}

pub(super) fn delete_room_summary_backoff(conn: &rusqlite::Connection, room_id: &str) {
    let _ = conn.execute(
        "DELETE FROM room_summary_backoff WHERE room_id = ?1",
        rusqlite::params![room_id],
    );
}

// ── presence_forbidden DB helpers ─────────────────────────────────────────

pub(super) fn load_presence_forbidden_conn(conn: &rusqlite::Connection) -> Vec<String> {
    let Ok(mut stmt) = conn.prepare("SELECT user_id FROM presence_forbidden") else {
        return vec![];
    };
    let Ok(rows) = stmt.query_map([], |row| row.get::<_, String>(0)) else {
        return vec![];
    };
    rows.flatten().collect()
}

pub(super) fn upsert_presence_forbidden_conn(conn: &rusqlite::Connection, user_id: &str) {
    let _ = conn.execute(
        "INSERT OR IGNORE INTO presence_forbidden (user_id) VALUES (?1)",
        rusqlite::params![user_id],
    );
}

// ── thread_read_marker DB helpers ────────────────────────────────────────
// Back `ClientFfi::thread_read_markers` with `thread_read_state.db` so the
// per-thread unread dot survives a restart. Not cfg-gated so the unit-test
// module can call them against an in-memory connection.

pub(super) fn load_thread_read_markers_conn(
    conn: &rusqlite::Connection,
) -> Vec<(String, String, i64)> {
    let Ok(mut stmt) = conn.prepare("SELECT room_id, root_id, ts_ms FROM thread_read_marker")
    else {
        return vec![];
    };
    let Ok(rows) = stmt.query_map([], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get::<_, String>(1)?,
            row.get::<_, i64>(2)?,
        ))
    }) else {
        return vec![];
    };
    rows.flatten().collect()
}

/// Upsert one marker, keeping the newest `ts_ms` if the row already exists
/// (markers only ever move forward).
pub(super) fn upsert_thread_read_marker_conn(
    conn: &rusqlite::Connection,
    room_id: &str,
    root_id: &str,
    ts_ms: i64,
) {
    let _ = conn.execute(
        "INSERT INTO thread_read_marker (room_id, root_id, ts_ms) VALUES (?1, ?2, ?3) \
         ON CONFLICT(room_id, root_id) DO UPDATE SET ts_ms = MAX(ts_ms, excluded.ts_ms)",
        rusqlite::params![room_id, root_id, ts_ms],
    );
}

pub(super) fn delete_thread_read_marker_conn(
    conn: &rusqlite::Connection,
    room_id: &str,
    root_id: &str,
) {
    let _ = conn.execute(
        "DELETE FROM thread_read_marker WHERE room_id = ?1 AND root_id = ?2",
        rusqlite::params![room_id, root_id],
    );
}

// ── room_summary_cache DB helpers ─────────────────────────────────────────

pub(super) fn store_room_summary_conn(
    conn: &rusqlite::Connection,
    room_id: &str,
    summary_json: &str,
    fetched_at_secs: i64,
) {
    let _ = conn.execute(
        "INSERT OR REPLACE INTO room_summary_cache \
         (room_id, summary_json, fetched_at_secs) VALUES (?1, ?2, ?3)",
        rusqlite::params![room_id, summary_json, fetched_at_secs],
    );
}

pub(super) fn load_room_summary_conn(conn: &rusqlite::Connection, room_id: &str) -> String {
    let Ok(mut stmt) =
        conn.prepare("SELECT summary_json FROM room_summary_cache WHERE room_id = ?1")
    else {
        return String::new();
    };
    stmt.query_row(rusqlite::params![room_id], |row| row.get::<_, String>(0))
        .unwrap_or_default()
}

// ── room_image_pack_cache DB helpers ──────────────────────────────────────
//
// Lazily-built local record of which rooms have an MSC2545 image pack.
// Sliding sync's `required_state` doesn't include custom event types like
// `m.room.image_pack`/`im.ponies.room_emotes`, so there's no cheap way to
// know which of an account's joined rooms have a pack without fetching
// each one's state individually — sweeping every joined room up front does
// that fetch for rooms the user may never even look at, and for an account
// with many rooms can take minutes. Instead this table is populated
// incrementally by `rebuild_image_packs` as it processes each room in the
// subscribed+active set (see that function) — written to on every rebuild,
// so a room's pack appearing/changing/disappearing is reflected the next
// time that room is processed (subscribed rooms every session, visited
// rooms whenever `set_active_room` marks them newly active). The "Known
// Packs" settings page reads only this table (`read_known_room_packs`) —
// a fast local query, never a network operation.

pub(super) fn upsert_known_room_pack(
    conn: &rusqlite::Connection,
    room_id: &str,
    state_key: &str,
    display_name: &str,
    checked_at: i64,
) {
    let _ = conn.execute(
        "INSERT OR REPLACE INTO room_image_pack_cache \
         (room_id, state_key, display_name, checked_at) VALUES (?1, ?2, ?3, ?4)",
        rusqlite::params![room_id, state_key, display_name, checked_at],
    );
}

pub(super) fn delete_known_room_pack(conn: &rusqlite::Connection, room_id: &str, state_key: &str) {
    let _ = conn.execute(
        "DELETE FROM room_image_pack_cache WHERE room_id = ?1 AND state_key = ?2",
        rusqlite::params![room_id, state_key],
    );
}

/// Replace every persisted row for `room_id` with exactly the packs given —
/// unlike `upsert_known_room_pack`/`delete_known_room_pack` (one specific
/// state_key), this is a whole-room resync: used after a full-state
/// discovery of every state_key a room defines, so state_keys that
/// disappeared since the last discovery are dropped along with the ones
/// that changed or stayed the same.
pub(super) fn replace_known_room_packs(
    conn: &rusqlite::Connection,
    room_id: &str,
    packs: &[crate::image_packs::ImagePack],
    checked_at: i64,
) {
    let _ = conn.execute(
        "DELETE FROM room_image_pack_cache WHERE room_id = ?1",
        rusqlite::params![room_id],
    );
    for pack in packs {
        let _ = conn.execute(
            "INSERT OR REPLACE INTO room_image_pack_cache \
             (room_id, state_key, display_name, checked_at) VALUES (?1, ?2, ?3, ?4)",
            rusqlite::params![
                room_id,
                pack.source_state_key(),
                pack.display_name,
                checked_at
            ],
        );
    }
}

pub(super) fn read_known_room_packs(conn: &rusqlite::Connection) -> Vec<(String, String, String)> {
    let Ok(mut stmt) =
        conn.prepare("SELECT room_id, state_key, display_name FROM room_image_pack_cache")
    else {
        return vec![];
    };
    let Ok(rows) = stmt.query_map([], |row| {
        Ok((
            row.get::<_, String>(0)?,
            row.get::<_, String>(1)?,
            row.get::<_, String>(2)?,
        ))
    }) else {
        return vec![];
    };
    rows.filter_map(|r| r.ok()).collect()
}

// ──────────────────────────────────────────────────────────────────────────

/// Warm the SDK's event-cache for one room without surfacing anything to
/// the UI. Builds a temporary `Timeline` and paginates backwards in
/// 50-event batches until either the room has at least `target_events`
/// event items locally or matrix-sdk reports that we've reached the start
/// of the timeline.
///
/// The `Timeline` is dropped on return; rows committed to the SDK's sqlite
/// event cache during pagination persist, so the next foreground
/// `subscribe_room` for this room paints from cache without a /messages
/// round-trip. Dropping the last `Timeline` reference aborts matrix-sdk-ui's
/// internal task that applies diffs from our own pagination calls onto the
/// timeline; we keep the diff stream alive and drain it to quiescence
/// first so that task always finishes its current batch before we let it
/// go — otherwise it can be cut off mid-batch, which trips a benign but
/// noisy `error!` in matrix-sdk-ui ("a DateDividerAdjuster has not been
/// consumed with run()").
///
/// Returns a `BackfillPreview` for the room's most recent event if one could
/// be extracted from the timeline items — needed because `room.latest_event()`
/// is only updated by the live sync loop and stays `None` for rooms whose
/// events arrived solely through back-pagination.
#[cfg(not(test))]
pub(super) async fn backfill_room_silent(
    client: &Client,
    room_id: &OwnedRoomId,
    target_events: usize,
) -> anyhow::Result<Option<BackfillPreview>> {
    let Some(room) = client.get_room(room_id) else {
        return Ok(None);
    };

    let timeline = room.timeline().await?;

    // We don't propagate items to the UI, but we keep the stream (rather
    // than dropping it immediately) so we can drain it to quiescence below
    // before `timeline` goes out of scope. The initial snapshot tells us
    // how much history is already cached.
    let (initial, stream) = timeline.subscribe().await;
    tokio::pin!(stream);
    let mut have = initial
        .iter()
        .filter(|i| matches!(i.kind(), TimelineItemKind::Event(_)))
        .count();

    while have < target_events {
        match timeline.paginate_backwards(50).await {
            Ok(true) => break, // reached the start
            Ok(false) => {}
            Err(_) => break, // soft-fail: no point spinning
        }
        have = timeline
            .items()
            .await
            .iter()
            .filter(|i| matches!(i.kind(), TimelineItemKind::Event(_)))
            .count();
    }

    // Let matrix-sdk-ui's internal diff-processing task (spawned when we
    // subscribed above) catch up on whatever our own pagination just fed
    // into the room's event cache. That task mirrors its committed output
    // onto this stream, so once it's quiet for a short window we know it
    // isn't mid-batch — safe for `timeline` to drop below without cutting
    // it off.
    while tokio::time::timeout(std::time::Duration::from_millis(200), stream.next())
        .await
        .is_ok_and(|item| item.is_some())
    {}

    // Extract a preview from the most recent event in the timeline.
    // Scope the items borrow so it ends before the get_member_no_sync await.
    // Also capture the most-recent event's timestamp (any event type) so it
    // can be stored in last_activity_ts even for rooms with no previewable event.
    let (preview_data, latest_ts) = {
        let items = timeline.items().await;
        let mut found: Option<(LatestPreview, Option<String>, matrix_sdk::ruma::OwnedUserId)> =
            None;
        let mut first_ts: u64 = 0;
        for item in items.iter().rev() {
            if let TimelineItemKind::Event(ev) = item.kind() {
                if first_ts == 0 {
                    first_ts = u64::from(ev.timestamp().0);
                }
                if found.is_none() {
                    let mut preview = preview_from_timeline_content(ev.content());
                    if preview.kind == "video" {
                        let is_gif = ev
                            .original_json()
                            .and_then(|raw| {
                                serde_json::from_str::<serde_json::Value>(raw.json().get()).ok()
                            })
                            .and_then(|v| {
                                v.pointer("/content/info/fi.mau.gif")
                                    .and_then(|v| v.as_bool())
                            })
                            .unwrap_or(false);
                        if is_gif {
                            preview.kind = "gif".to_owned();
                        }
                    }
                    if !preview.kind.is_empty() {
                        let sender_id = ev.sender().to_owned();
                        let profile_name = match ev.sender_profile() {
                            TimelineDetails::Ready(p) => p.display_name.clone(),
                            _ => None,
                        };
                        found = Some((preview, profile_name, sender_id));
                    }
                }
                if first_ts != 0 && found.is_some() {
                    break;
                }
            }
        }
        (found, first_ts)
    };

    if latest_ts == 0 {
        return Ok(None);
    }

    let (preview, sender_name) = if let Some((preview, profile_name, sender_id)) = preview_data {
        let is_mine = client.user_id().is_some_and(|me| me == &*sender_id);
        let name = if is_mine {
            String::new()
        } else {
            match profile_name.filter(|n| !n.is_empty()) {
                Some(n) => n,
                None => super::member_display_name_local(&room, &sender_id).await,
            }
        };
        (preview, name)
    } else {
        (LatestPreview::default(), String::new())
    };

    Ok(Some(BackfillPreview {
        room_id: room_id.to_string(),
        kind: preview.kind,
        text: preview.text,
        sticker_url: preview.sticker_url,
        thumbnail_url: preview.thumbnail_url,
        sender_name,
        timestamp_ms: latest_ts,
    }))
}

#[cfg(test)]
mod tests {
    use super::select_prefetch_rooms;
    use matrix_sdk::ruma::OwnedRoomId;
    use std::collections::HashSet;

    fn rid(s: &str) -> OwnedRoomId {
        s.parse().unwrap()
    }

    #[test]
    fn skips_live_timeline_rooms() {
        let ids = vec!["!a:ex.org".to_owned(), "!b:ex.org".to_owned()];
        let mut skip = HashSet::new();
        skip.insert(rid("!a:ex.org"));
        // Every room exists and is not tombstoned.
        let out = select_prefetch_rooms(&ids, &skip, |_| Some(false));
        assert_eq!(out, vec![rid("!b:ex.org")]);
    }

    #[test]
    fn does_not_skip_cached_or_timestamped_rooms() {
        // Inverse of start_background_backfill: a room that already has a
        // timestamp / is cached is still selected — the probe only reports
        // existence + tombstone, never "already cached".
        let ids = vec!["!a:ex.org".to_owned()];
        let skip = HashSet::new();
        let out = select_prefetch_rooms(&ids, &skip, |_| Some(false));
        assert_eq!(out, vec![rid("!a:ex.org")]);
    }

    #[test]
    fn skips_tombstoned_and_unknown_rooms() {
        let ids = vec![
            "!live:ex.org".to_owned(),
            "!dead:ex.org".to_owned(),
            "!gone:ex.org".to_owned(),
        ];
        let skip = HashSet::new();
        let out = select_prefetch_rooms(&ids, &skip, |id| match id.as_str() {
            "!live:ex.org" => Some(false), // exists, not tombstoned
            "!dead:ex.org" => Some(true),  // tombstoned
            _ => None,                     // unknown to the client
        });
        assert_eq!(out, vec![rid("!live:ex.org")]);
    }

    #[test]
    fn drops_invalid_room_ids_and_preserves_order() {
        let ids = vec![
            "not a room id".to_owned(),
            "!b:ex.org".to_owned(),
            "!a:ex.org".to_owned(),
        ];
        let skip = HashSet::new();
        let out = select_prefetch_rooms(&ids, &skip, |_| Some(false));
        // Invalid id dropped; the LRU order the shell passed in is preserved.
        assert_eq!(out, vec![rid("!b:ex.org"), rid("!a:ex.org")]);
    }
}

#[cfg(test)]
mod bridge_status_migration_tests {
    use super::ensure_bridge_status_columns;
    use rusqlite::Connection;

    fn columns(conn: &Connection) -> Vec<String> {
        let mut stmt = conn.prepare("PRAGMA table_info(bridge_status)").unwrap();
        stmt.query_map([], |row| row.get::<_, String>(1))
            .unwrap()
            .filter_map(Result::ok)
            .collect()
    }

    #[test]
    fn adds_cap_threads_to_old_schema_and_is_idempotent() {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE bridge_status (
                 room_id    TEXT    NOT NULL PRIMARY KEY,
                 is_bridged INTEGER NOT NULL
             );
             INSERT INTO bridge_status (room_id, is_bridged) VALUES ('!a:x', 1);",
        )
        .unwrap();
        ensure_bridge_status_columns(&conn).unwrap();
        ensure_bridge_status_columns(&conn).unwrap();
        let cols = columns(&conn);
        assert_eq!(cols.iter().filter(|c| *c == "cap_threads").count(), 1);
        let cap: Option<i64> = conn
            .query_row(
                "SELECT cap_threads FROM bridge_status WHERE room_id = '!a:x'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(cap, None);
    }
}

#[cfg(test)]
mod thread_read_marker_tests {
    use super::{
        delete_thread_read_marker_conn, load_thread_read_markers_conn,
        upsert_thread_read_marker_conn,
    };
    use rusqlite::Connection;

    fn make_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE thread_read_marker (
                 room_id TEXT    NOT NULL,
                 root_id TEXT    NOT NULL,
                 ts_ms   INTEGER NOT NULL,
                 PRIMARY KEY (room_id, root_id)
             );",
        )
        .unwrap();
        conn
    }

    #[test]
    fn upsert_and_load_round_trip() {
        let conn = make_conn();
        upsert_thread_read_marker_conn(&conn, "!r:s", "$root1", 100);
        upsert_thread_read_marker_conn(&conn, "!r:s", "$root2", 200);
        let mut rows = load_thread_read_markers_conn(&conn);
        rows.sort();
        assert_eq!(
            rows,
            vec![
                ("!r:s".into(), "$root1".into(), 100),
                ("!r:s".into(), "$root2".into(), 200),
            ]
        );
    }

    #[test]
    fn upsert_keeps_the_newest_ts() {
        let conn = make_conn();
        upsert_thread_read_marker_conn(&conn, "!r:s", "$root", 500);
        upsert_thread_read_marker_conn(&conn, "!r:s", "$root", 300); // older, ignored
        upsert_thread_read_marker_conn(&conn, "!r:s", "$root", 900); // newer, wins
        let rows = load_thread_read_markers_conn(&conn);
        assert_eq!(rows, vec![("!r:s".into(), "$root".into(), 900)]);
    }

    #[test]
    fn delete_removes_only_the_named_marker() {
        let conn = make_conn();
        upsert_thread_read_marker_conn(&conn, "!r:s", "$a", 1);
        upsert_thread_read_marker_conn(&conn, "!r:s", "$b", 2);
        delete_thread_read_marker_conn(&conn, "!r:s", "$a");
        let rows = load_thread_read_markers_conn(&conn);
        assert_eq!(rows, vec![("!r:s".into(), "$b".into(), 2)]);
    }

    #[test]
    fn load_on_empty_table_is_empty() {
        let conn = make_conn();
        assert!(load_thread_read_markers_conn(&conn).is_empty());
    }
}

#[cfg(test)]
mod media_backoff_tests {
    use super::{
        clear_media_backoff, delete_media_backoff, query_media_backoff, upsert_media_backoff,
    };
    use rusqlite::Connection;

    fn make_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE media_backoff (
                 url      TEXT    NOT NULL PRIMARY KEY,
                 attempts INTEGER NOT NULL,
                 deadline INTEGER NOT NULL
             );",
        )
        .unwrap();
        conn
    }

    #[test]
    fn upsert_and_load_round_trip() {
        let conn = make_conn();
        upsert_media_backoff(&conn, "mxc://a/b", 3, 9999);
        let rows = query_media_backoff(&conn);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0], ("mxc://a/b".to_owned(), 3, 9999));
    }

    #[test]
    fn upsert_replaces_existing() {
        let conn = make_conn();
        upsert_media_backoff(&conn, "mxc://a/b", 2, 1000);
        upsert_media_backoff(&conn, "mxc://a/b", 4, 3000);
        let rows = query_media_backoff(&conn);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0], ("mxc://a/b".to_owned(), 4, 3000));
    }

    #[test]
    fn delete_removes_entry() {
        let conn = make_conn();
        upsert_media_backoff(&conn, "mxc://a/b", 2, 1000);
        delete_media_backoff(&conn, "mxc://a/b");
        assert!(query_media_backoff(&conn).is_empty());
    }

    #[test]
    fn delete_nonexistent_is_noop() {
        let conn = make_conn();
        delete_media_backoff(&conn, "mxc://x/y"); // must not panic
        assert!(query_media_backoff(&conn).is_empty());
    }

    #[test]
    fn clear_empties_table() {
        let conn = make_conn();
        upsert_media_backoff(&conn, "mxc://a/b", 1, 1000);
        upsert_media_backoff(&conn, "mxc://c/d", 2, 2000);
        clear_media_backoff(&conn);
        assert!(query_media_backoff(&conn).is_empty());
    }

    #[test]
    fn expired_deadline_is_returned_with_attempts_intact() {
        // deadline in the past — entry is still returned; C++ applies max(0, remaining)
        let conn = make_conn();
        upsert_media_backoff(&conn, "mxc://x/y", 5, 1); // epoch second 1 = far past
        let rows = query_media_backoff(&conn);
        assert_eq!(rows[0].1, 5); // attempts preserved
        assert_eq!(rows[0].2, 1); // deadline preserved as-is
    }
}

#[cfg(test)]
mod backoff_sweep_tests {
    use super::prune_stale_backoff_and_cache_rows;
    use rusqlite::Connection;

    fn make_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE media_backoff (
                 url      TEXT    NOT NULL PRIMARY KEY,
                 attempts INTEGER NOT NULL,
                 deadline INTEGER NOT NULL
             );
             CREATE TABLE room_summary_backoff (
                 room_id  TEXT    NOT NULL PRIMARY KEY,
                 attempts INTEGER NOT NULL,
                 deadline INTEGER NOT NULL
             );
             CREATE TABLE room_summary_cache (
                 room_id         TEXT    NOT NULL PRIMARY KEY,
                 summary_json    TEXT    NOT NULL,
                 fetched_at_secs INTEGER NOT NULL
             );",
        )
        .unwrap();
        conn
    }

    // Row age is measured against real wall-clock time (`strftime('%s','now')`
    // in the swept SQL), so "ancient" uses a small literal epoch second
    // (always > 30 days in the past for any realistic `now`) and "recent"
    // computes an actual near-now timestamp via SQLite itself — avoids
    // needing any time-mocking machinery in the test.
    fn row_count(conn: &Connection, table: &str) -> i64 {
        conn.query_row(&format!("SELECT COUNT(*) FROM {table}"), [], |r| r.get(0))
            .unwrap()
    }

    #[test]
    fn prunes_media_backoff_rows_older_than_30_days() {
        let conn = make_conn();
        conn.execute(
            "INSERT INTO media_backoff (url, attempts, deadline) VALUES ('mxc://old', 7, 1000)",
            [],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO media_backoff (url, attempts, deadline) \
             VALUES ('mxc://fresh', 1, strftime('%s','now') + 60)",
            [],
        )
        .unwrap();

        prune_stale_backoff_and_cache_rows(&conn);

        assert_eq!(row_count(&conn, "media_backoff"), 1);
        let remaining: String = conn
            .query_row("SELECT url FROM media_backoff", [], |r| r.get(0))
            .unwrap();
        assert_eq!(remaining, "mxc://fresh");
    }

    #[test]
    fn prunes_room_summary_backoff_rows_older_than_30_days() {
        let conn = make_conn();
        conn.execute(
            "INSERT INTO room_summary_backoff (room_id, attempts, deadline) \
             VALUES ('!old:example.org', 7, 1000)",
            [],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO room_summary_backoff (room_id, attempts, deadline) \
             VALUES ('!fresh:example.org', 1, strftime('%s','now') + 60)",
            [],
        )
        .unwrap();

        prune_stale_backoff_and_cache_rows(&conn);

        assert_eq!(row_count(&conn, "room_summary_backoff"), 1);
        let remaining: String = conn
            .query_row("SELECT room_id FROM room_summary_backoff", [], |r| r.get(0))
            .unwrap();
        assert_eq!(remaining, "!fresh:example.org");
    }

    #[test]
    fn prunes_room_summary_cache_rows_older_than_30_days() {
        let conn = make_conn();
        conn.execute(
            "INSERT INTO room_summary_cache (room_id, summary_json, fetched_at_secs) \
             VALUES ('!old:example.org', '{}', 1000)",
            [],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO room_summary_cache (room_id, summary_json, fetched_at_secs) \
             VALUES ('!fresh:example.org', '{}', strftime('%s','now'))",
            [],
        )
        .unwrap();

        prune_stale_backoff_and_cache_rows(&conn);

        assert_eq!(row_count(&conn, "room_summary_cache"), 1);
        let remaining: String = conn
            .query_row("SELECT room_id FROM room_summary_cache", [], |r| r.get(0))
            .unwrap();
        assert_eq!(remaining, "!fresh:example.org");
    }

    #[test]
    fn empty_tables_are_a_harmless_noop() {
        let conn = make_conn();
        prune_stale_backoff_and_cache_rows(&conn); // must not panic
        assert_eq!(row_count(&conn, "media_backoff"), 0);
        assert_eq!(row_count(&conn, "room_summary_backoff"), 0);
        assert_eq!(row_count(&conn, "room_summary_cache"), 0);
    }
}

#[cfg(test)]
mod known_room_pack_tests {
    use super::{
        delete_known_room_pack, read_known_room_packs, replace_known_room_packs,
        upsert_known_room_pack,
    };
    use crate::image_packs::{ImagePack, PackSource};
    use rusqlite::Connection;

    fn room_pack(state_key: &str, display_name: &str) -> ImagePack {
        ImagePack {
            id: format!("room:!a:ex.org/{state_key}"),
            display_name: display_name.to_owned(),
            avatar_url: String::new(),
            attribution: String::new(),
            usage: crate::image_packs::USAGE_ANY,
            source: PackSource::Room {
                room_id: "!a:ex.org".to_owned(),
                state_key: state_key.to_owned(),
            },
            images: Vec::new(),
            is_subscribed: false,
        }
    }

    fn make_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE room_image_pack_cache (
                 room_id      TEXT NOT NULL,
                 state_key    TEXT NOT NULL,
                 display_name TEXT NOT NULL DEFAULT '',
                 checked_at   INTEGER NOT NULL,
                 PRIMARY KEY (room_id, state_key)
             );",
        )
        .unwrap();
        conn
    }

    #[test]
    fn upsert_and_read_round_trip() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!a:ex.org", "", "Room Pack", 1000);
        let rows = read_known_room_packs(&conn);
        assert_eq!(rows.len(), 1);
        assert_eq!(
            rows[0],
            (
                "!a:ex.org".to_owned(),
                "".to_owned(),
                "Room Pack".to_owned()
            )
        );
    }

    #[test]
    fn upsert_replaces_existing_row_for_same_key() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!a:ex.org", "", "Old Name", 1000);
        upsert_known_room_pack(&conn, "!a:ex.org", "", "New Name", 2000);
        let rows = read_known_room_packs(&conn);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].2, "New Name");
    }

    #[test]
    fn delete_removes_entry() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!a:ex.org", "", "Room Pack", 1000);
        delete_known_room_pack(&conn, "!a:ex.org", "");
        assert!(read_known_room_packs(&conn).is_empty());
    }

    #[test]
    fn delete_of_absent_key_is_a_noop() {
        let conn = make_conn();
        delete_known_room_pack(&conn, "!missing:ex.org", ""); // must not panic
        assert!(read_known_room_packs(&conn).is_empty());
    }

    #[test]
    fn distinct_state_keys_in_same_room_are_independent_rows() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!a:ex.org", "", "Default Pack", 1000);
        upsert_known_room_pack(&conn, "!a:ex.org", "second", "Second Pack", 1000);
        let mut rows = read_known_room_packs(&conn);
        rows.sort();
        assert_eq!(rows.len(), 2);
    }

    #[test]
    fn replace_overwrites_all_rows_for_room() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!a:ex.org", "", "Stale Default", 1000);
        upsert_known_room_pack(&conn, "!a:ex.org", "stale", "Stale Named", 1000);
        replace_known_room_packs(
            &conn,
            "!a:ex.org",
            &[
                room_pack("", "Fresh Default"),
                room_pack("named", "Fresh Named"),
            ],
            2000,
        );
        let mut rows = read_known_room_packs(&conn);
        rows.sort();
        assert_eq!(
            rows,
            vec![
                (
                    "!a:ex.org".to_owned(),
                    "".to_owned(),
                    "Fresh Default".to_owned()
                ),
                (
                    "!a:ex.org".to_owned(),
                    "named".to_owned(),
                    "Fresh Named".to_owned()
                ),
            ]
        );
    }

    #[test]
    fn replace_with_empty_slice_clears_room() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!a:ex.org", "", "Old Pack", 1000);
        replace_known_room_packs(&conn, "!a:ex.org", &[], 2000);
        assert!(read_known_room_packs(&conn).is_empty());
    }

    #[test]
    fn replace_does_not_touch_other_rooms() {
        let conn = make_conn();
        upsert_known_room_pack(&conn, "!b:ex.org", "", "Other Room Pack", 1000);
        replace_known_room_packs(&conn, "!a:ex.org", &[room_pack("", "A Pack")], 2000);
        let mut rows = read_known_room_packs(&conn);
        rows.sort();
        assert_eq!(rows.len(), 2);
    }
}

#[cfg(all(test, unix))]
mod app_cache_mode_tests {
    use super::restrict_app_cache_to_owner;
    use std::os::unix::fs::PermissionsExt;
    use std::path::PathBuf;

    fn mode_of(path: &std::path::Path) -> u32 {
        std::fs::metadata(path).unwrap().permissions().mode() & 0o777
    }

    fn scratch_dir(name: &str) -> PathBuf {
        let dir =
            std::env::temp_dir().join(format!("tesseract-app-cache-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn a_new_app_cache_file_is_created_owner_only() {
        let dir = scratch_dir("new");
        let path = dir.join("app_cache.db");
        restrict_app_cache_to_owner(&path);
        assert!(path.exists());
        assert_eq!(mode_of(&path), 0o600);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn an_existing_app_cache_file_is_tightened_to_owner_only() {
        let dir = scratch_dir("existing");
        let path = dir.join("app_cache.db");
        std::fs::write(&path, b"").unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).unwrap();
        restrict_app_cache_to_owner(&path);
        assert_eq!(mode_of(&path), 0o600);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}

#[cfg(test)]
mod backfill_update_tests {
    use super::{backfill_update_due, BACKFILL_UPDATE_MIN_INTERVAL};
    use std::time::Duration;

    #[test]
    fn a_progress_update_waits_for_a_full_batch_and_the_interval() {
        // The first full batch goes out at once.
        assert!(backfill_update_due(5, 0, 5, None));
        // A partial batch does not, whatever the time since the last update.
        assert!(!backfill_update_due(4, 0, 5, None));
        // A full batch inside the interval waits for the interval to pass.
        let early = Some(Duration::from_millis(400));
        assert!(!backfill_update_due(10, 5, 5, early));
        let full = Some(BACKFILL_UPDATE_MIN_INTERVAL);
        assert!(backfill_update_due(10, 5, 5, full));
    }
}

#[cfg(test)]
mod retry_tests {
    use super::backfill_retry_delay;
    use std::time::Duration;

    #[test]
    fn backfill_retry_delay_doubles_then_caps() {
        assert_eq!(backfill_retry_delay(1), Duration::from_secs(30));
        assert_eq!(backfill_retry_delay(2), Duration::from_secs(60));
        assert_eq!(backfill_retry_delay(5), Duration::from_secs(480));
        assert_eq!(backfill_retry_delay(6), Duration::from_secs(600));
        assert_eq!(backfill_retry_delay(u32::MAX), Duration::from_secs(600));
    }
}

#[cfg(test)]
mod backfill_ts_reset_tests {
    use super::reset_backfill_ts_once;
    use rusqlite::Connection;

    fn conn() -> Connection {
        let c = Connection::open_in_memory().unwrap();
        c.execute_batch(
            "CREATE TABLE backfill_ts (room_id TEXT NOT NULL PRIMARY KEY, ts_ms INTEGER NOT NULL);
             INSERT INTO backfill_ts VALUES ('!a:x', 1), ('!b:x', 2);",
        )
        .unwrap();
        c
    }

    fn rows(c: &Connection) -> i64 {
        c.query_row("SELECT COUNT(*) FROM backfill_ts", [], |r| r.get(0)).unwrap()
    }

    #[test]
    fn clears_the_cache_once_then_keeps_new_rows() {
        let c = conn();
        reset_backfill_ts_once(&c);
        assert_eq!(rows(&c), 0);
        c.execute("INSERT INTO backfill_ts VALUES ('!c:x', 3)", []).unwrap();
        reset_backfill_ts_once(&c);
        assert_eq!(rows(&c), 1, "a second run must not clear again");
    }
}
