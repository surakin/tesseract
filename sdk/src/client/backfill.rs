//! Background backfill orchestration and per-account app-cache DB.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

#[cfg(not(test))]
use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{build_room_infos, err, first_line, html_first_line, ClientFfi, LatestPreview};
#[cfg(not(test))]
use super::ok;

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
use matrix_sdk::ruma::OwnedRoomId;
#[cfg(not(test))]
use matrix_sdk::Client;
#[cfg(not(test))]
use matrix_sdk_ui::timeline::{
    MsgLikeContent, MsgLikeKind, RoomExt, TimelineDetails, TimelineItemContent,
    TimelineItemKind,
};
#[cfg(not(test))]
use std::collections::HashMap;
#[cfg(not(test))]
use parking_lot::Mutex;
use std::sync::Arc;

#[cfg(not(test))]
impl ClientFfi {
    // -----------------------------------------------------------------------
    // Background backfill of non-active rooms
    // -----------------------------------------------------------------------

    // Shared orchestrator: spawn a bounded-concurrency backfill task for the
    // given room list. Callers are responsible for the idempotency guard and
    // for ensuring `to_backfill` is already filtered. No-op on empty input.
    #[cfg(not(test))]
    fn launch_backfill_task_(
        &mut self,
        client: matrix_sdk::Client,
        to_backfill: Vec<OwnedRoomId>,
    ) {
        if to_backfill.is_empty() {
            return;
        }

        let handler = self.handler.clone();
        let preview_cache = Arc::clone(&self.backfill_previews);
        let db_conn = Arc::clone(&self.app_cache_db);
        let in_flight = Arc::clone(&self.in_flight);

        // Emit on_rooms_updated every UPDATE_EVERY completions so the
        // inactive-room count ticks up visibly during a long backfill run,
        // plus once at completion if the total isn't an exact multiple.
        const UPDATE_EVERY: usize = 5;

        let abort = self
            .rt
            .spawn(async move {
                let semaphore = Arc::new(tokio::sync::Semaphore::new(3));
                let mut joinset = tokio::task::JoinSet::new();

                for rid in to_backfill {
                    let client = client.clone();
                    let sem = semaphore.clone();
                    let preview_cache = preview_cache.clone();
                    let db_conn = Arc::clone(&db_conn);
                    let in_flight = Arc::clone(&in_flight);
                    let handler_ref = handler.clone();
                    joinset.spawn(async move {
                        let _permit = match sem.acquire_owned().await {
                            Ok(p) => p,
                            Err(_) => return,
                        };
                        let _guard = super::InFlightGuard::new(&in_flight, &handler_ref);
                        let preview =
                            backfill_room_silent(&client, &rid, 50).await.ok().flatten();
                        // Always record in preview_cache — even on failure — so
                        // start_background_backfill_all_uncached skips this room
                        // for the rest of the session and we don't re-queue it on
                        // every on_rooms_updated tick. A zero-timestamp sentinel is
                        // used for failed rooms; it is not written to backfill_ts
                        // SQLite (guarded by ts != 0 below), so next session retries.
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
                                            rusqlite::params![
                                                &bp.room_id,
                                                bp.timestamp_ms as i64
                                            ],
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
                while joinset.join_next().await.is_some() {
                    completed += 1;
                    if completed - last_update_at >= UPDATE_EVERY {
                        last_update_at = completed;
                        if let Some(ref h) = handler {
                            let mut rooms = build_room_infos(&client).await;
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
                        let mut rooms = build_room_infos(&client).await;
                        apply_backfill_previews(&mut rooms, &preview_cache);
                        {
                            let guard = h.lock();
                            guard.on_rooms_updated(&rooms);
                        }
                    }
                }
            })
            .abort_handle();

        self.backfill_task = Some(abort);
    }

    #[cfg(not(test))]
    pub fn start_background_backfill(
        &mut self,
        room_ids: &cxx::CxxVector<cxx::CxxString>,
    ) -> OpResult {
        // Idempotent: if a previous orchestrator is still running, leave it
        // alone. Finished/aborted handles can be replaced.
        if let Some(h) = self.backfill_task.as_ref() {
            if !h.is_finished() {
                return ok("");
            }
        }
        self.backfill_task = None;

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
            if cached.contains(id.as_str()) {
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
    pub fn start_background_backfill_all_uncached(&mut self) -> OpResult {
        // Same idempotency guard as start_background_backfill.
        if let Some(h) = self.backfill_task.as_ref() {
            if !h.is_finished() {
                return ok("");
            }
        }
        self.backfill_task = None;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        // Rooms already in backfill_previews have a cached timestamp (either
        // loaded from backfill_ts on startup or written during this session).
        let cached: std::collections::HashSet<String> = {
            let guard = self.backfill_previews.lock();
            guard.keys().cloned().collect()
        };

        // Skip the room currently open in the foreground timeline.
        let skip: std::collections::HashSet<OwnedRoomId> =
            self.timelines.read().keys().cloned().collect();

        let mut to_backfill: Vec<OwnedRoomId> = Vec::new();
        for room in client.joined_rooms() {
            let id = room.room_id().to_owned();
            if skip.contains(&id) || room.is_tombstoned() {
                continue;
            }
            // Already cached — timestamp known from backfill_ts or this session.
            if cached.contains(id.as_str()) {
                continue;
            }
            // SDK already has a timestamp from live sync — no backfill needed.
            if room.latest_event_timestamp().is_some() {
                continue;
            }
            to_backfill.push(id);
        }

        let Some(sync_service) = self.sync_service.clone() else {
            return err("sync not started");
        };

        let handler       = self.handler.clone();
        let preview_cache = Arc::clone(&self.backfill_previews);
        let db_conn       = Arc::clone(&self.app_cache_db);

        let abort = self.rt.spawn(async move {
            // Process in batches of 50 so the event cache's internal broadcast
            // channel is never overwhelmed. Subscribing all rooms at once
            // produces a sync payload large enough to overflow the channel,
            // which causes matrix-sdk to clear all cached events and reset
            // latest_event_timestamp() — making the polling loop stuck.
            //
            // Each batch: subscribe → wait up to 30 s for sync to deliver
            // timestamps → resolve what resolved → move on to next batch.
            // Rooms not resolved within 30 s fall back to /messages.
            const BATCH_SIZE: usize = 50;
            let mut fallback: Vec<OwnedRoomId> = Vec::new();

            for chunk in to_backfill.chunks(BATCH_SIZE) {
                // Subscribe this batch only. latest_event_timestamp() and
                // latest_event() are populated by the next sync response;
                // build_room_info() already reads both, so no manual
                // BackfillPreview construction is needed for resolved rooms.
                {
                    let refs: Vec<&matrix_sdk::ruma::RoomId> =
                        chunk.iter().map(|id| id.as_ref()).collect();
                    sync_service.room_list_service().subscribe_to_rooms(&refs).await;
                }

                let mut pending: Vec<OwnedRoomId> = chunk.to_vec();
                for _ in 0u32..6 {
                    tokio::time::sleep(std::time::Duration::from_secs(5)).await;

                    let mut still_missing: Vec<OwnedRoomId> = Vec::new();
                    let mut any_new = false;
                    for rid in &pending {
                        let Some(room) = client.get_room(rid) else { continue; };
                        if let Some(ts) = room.latest_event_timestamp() {
                            let ts_ms = u64::from(ts.0);
                            if ts_ms != 0 {
                                {
                                    let db = db_conn.lock();
                                    if let Some(ref conn) = *db {
                                        let _ = conn.execute(
                                            "INSERT OR REPLACE INTO backfill_ts \
                                             (room_id, ts_ms) VALUES (?1, ?2)",
                                            rusqlite::params![rid.as_str(), ts_ms as i64],
                                        );
                                    }
                                }
                            }
                            any_new = true;
                        } else {
                            still_missing.push(rid.clone());
                        }
                    }
                    pending = still_missing;

                    if any_new {
                        if let Some(ref h) = handler {
                            let mut rooms = build_room_infos(&client).await;
                            apply_backfill_previews(&mut rooms, &preview_cache);
                            {
                                let guard = h.lock();
                                guard.on_rooms_updated(&rooms);
                            }
                        }
                    }
                    if pending.is_empty() {
                        break;
                    }
                }

                fallback.extend(pending);
            }

            let remaining = fallback;

            // Rooms still without a timestamp after all batch attempts.
            // Write zero-timestamp sentinels so they aren't re-queued within
            // this session, then fall back to per-room /messages pagination.
            for rid in &remaining {
                {
                    let mut cache = preview_cache.lock();
                    cache.entry(rid.to_string()).or_insert(BackfillPreview {
                        room_id:           rid.to_string(),
                        kind:              String::new(),
                        text:              String::new(),
                        sticker_url:       String::new(),
                        thumbnail_url:     String::new(),
                        sender_name:       String::new(),
                        timestamp_ms:      0,
                    });
                }
            }
            if !remaining.is_empty() {
                const UPDATE_EVERY: usize = 5;
                let semaphore = Arc::new(tokio::sync::Semaphore::new(3));
                let mut joinset = tokio::task::JoinSet::<()>::new();

                for rid in remaining {
                    let client        = client.clone();
                    let sem           = semaphore.clone();
                    let preview_cache = Arc::clone(&preview_cache);
                    let db_conn       = Arc::clone(&db_conn);
                    joinset.spawn(async move {
                        let _permit = match sem.acquire_owned().await {
                            Ok(p)  => p,
                            Err(_) => return,
                        };
                        let preview =
                            backfill_room_silent(&client, &rid, 50).await.ok().flatten();
                        let bp = preview.unwrap_or(BackfillPreview {
                            room_id:       rid.to_string(),
                            kind:          String::new(),
                            text:          String::new(),
                            sticker_url:   String::new(),
                            thumbnail_url: String::new(),
                            sender_name:   String::new(),
                            timestamp_ms:  0,
                        });
                        {
                            let mut cache = preview_cache.lock();
                            cache.insert(bp.room_id.clone(), bp.clone());
                        }
                        if bp.timestamp_ms != 0 {
                            {
                                let db = db_conn.lock();
                                if let Some(ref conn) = *db {
                                    let _ = conn.execute(
                                        "INSERT OR REPLACE INTO backfill_ts \
                                         (room_id, ts_ms) VALUES (?1, ?2)",
                                        rusqlite::params![
                                            &bp.room_id,
                                            bp.timestamp_ms as i64
                                        ],
                                    );
                                }
                            }
                        }
                    });
                }

                let mut completed: usize = 0;
                let mut last_update_at: usize = 0;
                while joinset.join_next().await.is_some() {
                    completed += 1;
                    if completed - last_update_at >= UPDATE_EVERY {
                        last_update_at = completed;
                        if let Some(ref h) = handler {
                            let mut rooms = build_room_infos(&client).await;
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
                        let mut rooms = build_room_infos(&client).await;
                        apply_backfill_previews(&mut rooms, &preview_cache);
                        {
                            let guard = h.lock();
                            guard.on_rooms_updated(&rooms);
                        }
                    }
                }
            }
        }).abort_handle();

        self.backfill_task = Some(abort);
        ok("")
    }

    #[cfg(not(test))]
    pub fn stop_background_backfill(&mut self) {
        if let Some(h) = self.backfill_task.take() {
            h.abort();
        }
    }
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
        let line = match formatted {
            Some(html) if !html.trim().is_empty() => html_first_line(html),
            _ => String::new(),
        };
        let line = if line.is_empty() { first_line(body) } else { line };
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
            _ => LatestPreview::default(),
        },
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Sticker(_),
            ..
        }) => LatestPreview {
            kind: "sticker".to_owned(),
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

/// Open (or create) the per-account app-cache database and ensure the schema
/// exists. `data_dir` is already the per-account SDK store directory
/// (`<data>/accounts/<uid>/matrix-store/`), so the file lands there directly.
/// Returns `None` on any I/O or SQL error (treated as a soft failure).
#[cfg(not(test))]
pub(super) fn open_app_cache_db(data_dir: &std::path::Path) -> Option<rusqlite::Connection> {
    let path = data_dir.join("app_cache.db");
    let conn = rusqlite::Connection::open(&path).ok()?;
    conn.execute_batch(
        "PRAGMA journal_mode=WAL;
         CREATE TABLE IF NOT EXISTS backfill_ts (
             room_id TEXT NOT NULL PRIMARY KEY,
             ts_ms   INTEGER NOT NULL
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

// ──────────────────────────────────────────────────────────────────────────

/// Warm the SDK's event-cache for one room without surfacing anything to
/// the UI. Builds a temporary `Timeline`, drops the live diff stream, and
/// paginates backwards in 50-event batches until either the room has at
/// least `target_events` event items locally or matrix-sdk reports that
/// we've reached the start of the timeline.
///
/// The `Timeline` is dropped on return; rows committed to the SDK's sqlite
/// event cache during pagination persist, so the next foreground
/// `subscribe_room` for this room paints from cache without a /messages
/// round-trip.
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

    // We don't propagate items to the UI, so subscribe + drop the stream.
    // The initial snapshot tells us how much history is already cached.
    let (initial, _stream) = timeline.subscribe().await;
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
