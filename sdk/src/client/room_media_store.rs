//! Per-room image/video index living in the per-account `app_cache.db`.
//!
//! `RoomMediaView` (the full-screen room media gallery) otherwise has to
//! back-paginate the room's SDK timeline from scratch every time it opens. This
//! table lets it paint the newest page instantly from SQLite and page older
//! history without a network round-trip.
//!
//! Population is a hybrid:
//!   * **lazy seed** — the first time a room's gallery opens, enumerate the
//!     SDK's own persisted event-cache store (`EventCacheStore::get_room_events`,
//!     no network) and upsert every image/video message (`seed_room_media`);
//!   * **incremental** — `MediaCtx` is threaded into the timeline streaming /
//!     pagination tasks (mirrors `search::SearchIndexCtx`) and upserts every
//!     image/video that flows past, deleting rows on redaction.
//!
//! The pure SQL helpers are ungated so they unit-test against an in-memory
//! `Connection`; the live entry points are `#[cfg(not(test))]`.

use rusqlite::{params, Connection};

#[cfg(not(test))]
use parking_lot::Mutex;
#[cfg(not(test))]
use std::sync::Arc;

/// DDL spliced into `backfill::open_app_cache_db`'s `execute_batch`.
pub(crate) const CREATE_TABLE_SQL: &str = "\
CREATE TABLE IF NOT EXISTS room_media (
    room_id             TEXT    NOT NULL,
    event_id            TEXT    NOT NULL,
    ts_ms               INTEGER NOT NULL,
    sender              TEXT    NOT NULL DEFAULT '',
    sender_name         TEXT    NOT NULL DEFAULT '',
    sender_avatar_mxc   TEXT    NOT NULL DEFAULT '',
    kind                INTEGER NOT NULL,
    caption             TEXT    NOT NULL DEFAULT '',
    src_mxc             TEXT    NOT NULL DEFAULT '',
    src_encrypted       INTEGER NOT NULL DEFAULT 0,
    src_json            TEXT    NOT NULL DEFAULT '',
    thumb_mxc           TEXT    NOT NULL DEFAULT '',
    thumb_encrypted     INTEGER NOT NULL DEFAULT 0,
    thumb_json          TEXT    NOT NULL DEFAULT '',
    media_w             INTEGER NOT NULL DEFAULT 0,
    media_h             INTEGER NOT NULL DEFAULT 0,
    blurhash            TEXT    NOT NULL DEFAULT '',
    duration_ms         INTEGER NOT NULL DEFAULT 0,
    video_mime          TEXT    NOT NULL DEFAULT '',
    video_autoplay      INTEGER NOT NULL DEFAULT 0,
    video_loop          INTEGER NOT NULL DEFAULT 0,
    video_no_audio      INTEGER NOT NULL DEFAULT 0,
    video_hide_controls INTEGER NOT NULL DEFAULT 0,
    video_gif           INTEGER NOT NULL DEFAULT 0,
    image_animated      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (room_id, event_id)
);
CREATE INDEX IF NOT EXISTS idx_room_media_room_ts ON room_media (room_id, ts_ms);
CREATE TABLE IF NOT EXISTS room_media_seeded (
    room_id        TEXT    NOT NULL PRIMARY KEY,
    seeded_at_secs INTEGER NOT NULL
);";

pub(crate) const KIND_IMAGE: i64 = 0;
pub(crate) const KIND_VIDEO: i64 = 1;

/// Every column after `room_id` / `event_id`, in the one canonical order shared
/// by the upsert and the select so the two can never drift.
const VALUE_COLS: &str = "ts_ms, sender, sender_name, sender_avatar_mxc, kind, caption, \
src_mxc, src_encrypted, src_json, thumb_mxc, thumb_encrypted, thumb_json, media_w, media_h, \
blurhash, duration_ms, video_mime, video_autoplay, video_loop, video_no_audio, \
video_hide_controls, video_gif, image_animated";

/// One indexed image/video — the field-for-field subset `RoomMediaView` reads.
#[derive(Debug, Clone, PartialEq, Default)]
pub(crate) struct RoomMediaRow {
    pub event_id: String,
    pub ts_ms: i64,
    pub sender: String,
    pub sender_name: String,
    pub sender_avatar_mxc: String,
    pub kind: i64,
    pub caption: String,
    pub src_mxc: String,
    pub src_encrypted: bool,
    pub src_json: String,
    pub thumb_mxc: String,
    pub thumb_encrypted: bool,
    pub thumb_json: String,
    pub media_w: i64,
    pub media_h: i64,
    pub blurhash: String,
    pub duration_ms: i64,
    pub video_mime: String,
    pub video_autoplay: bool,
    pub video_loop: bool,
    pub video_no_audio: bool,
    pub video_hide_controls: bool,
    pub video_gif: bool,
    pub image_animated: bool,
}

// ── pure SQL helpers ─────────────────────────────────────────────────────────

fn upsert_on(conn: &Connection, room_id: &str, row: &RoomMediaRow) -> rusqlite::Result<()> {
    conn.execute(
        &format!(
            "INSERT OR REPLACE INTO room_media (room_id, event_id, {VALUE_COLS}) \
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, \
             ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25)"
        ),
        params![
            room_id,
            row.event_id,
            row.ts_ms,
            row.sender,
            row.sender_name,
            row.sender_avatar_mxc,
            row.kind,
            row.caption,
            row.src_mxc,
            row.src_encrypted as i64,
            row.src_json,
            row.thumb_mxc,
            row.thumb_encrypted as i64,
            row.thumb_json,
            row.media_w,
            row.media_h,
            row.blurhash,
            row.duration_ms,
            row.video_mime,
            row.video_autoplay as i64,
            row.video_loop as i64,
            row.video_no_audio as i64,
            row.video_hide_controls as i64,
            row.video_gif as i64,
            row.image_animated as i64,
        ],
    )?;
    Ok(())
}

pub(crate) fn upsert(conn: &Connection, room_id: &str, row: &RoomMediaRow) {
    let _ = upsert_on(conn, room_id, row);
}

pub(crate) fn upsert_many(conn: &mut Connection, room_id: &str, rows: &[RoomMediaRow]) {
    let Ok(tx) = conn.transaction() else { return };
    for row in rows {
        let _ = upsert_on(&tx, room_id, row);
    }
    let _ = tx.commit();
}

pub(crate) fn delete_event(conn: &Connection, room_id: &str, event_id: &str) {
    let _ = conn.execute(
        "DELETE FROM room_media WHERE room_id = ?1 AND event_id = ?2",
        params![room_id, event_id],
    );
}

fn row_from_sql(r: &rusqlite::Row<'_>) -> rusqlite::Result<RoomMediaRow> {
    Ok(RoomMediaRow {
        event_id: r.get(0)?,
        ts_ms: r.get(1)?,
        sender: r.get(2)?,
        sender_name: r.get(3)?,
        sender_avatar_mxc: r.get(4)?,
        kind: r.get(5)?,
        caption: r.get(6)?,
        src_mxc: r.get(7)?,
        src_encrypted: r.get::<_, i64>(8)? != 0,
        src_json: r.get(9)?,
        thumb_mxc: r.get(10)?,
        thumb_encrypted: r.get::<_, i64>(11)? != 0,
        thumb_json: r.get(12)?,
        media_w: r.get(13)?,
        media_h: r.get(14)?,
        blurhash: r.get(15)?,
        duration_ms: r.get(16)?,
        video_mime: r.get(17)?,
        video_autoplay: r.get::<_, i64>(18)? != 0,
        video_loop: r.get::<_, i64>(19)? != 0,
        video_no_audio: r.get::<_, i64>(20)? != 0,
        video_hide_controls: r.get::<_, i64>(21)? != 0,
        video_gif: r.get::<_, i64>(22)? != 0,
        image_animated: r.get::<_, i64>(23)? != 0,
    })
}

/// Newest-first page. `before_ts_ms == 0` → newest page; otherwise strictly
/// older than that timestamp (the cursor is the oldest `ts_ms` of the prior
/// page). `(ts_ms, event_id)` compound ordering keeps paging deterministic when
/// several events share a millisecond.
pub(crate) fn query_page(
    conn: &Connection,
    room_id: &str,
    before_ts_ms: i64,
    limit: i64,
) -> Vec<RoomMediaRow> {
    let sql = format!(
        "SELECT event_id, {VALUE_COLS} FROM room_media \
         WHERE room_id = ?1 AND (?2 = 0 OR ts_ms < ?2) \
         ORDER BY ts_ms DESC, event_id DESC LIMIT ?3"
    );
    let Ok(mut stmt) = conn.prepare(&sql) else {
        return Vec::new();
    };
    let Ok(rows) = stmt.query_map(params![room_id, before_ts_ms, limit], row_from_sql) else {
        return Vec::new();
    };
    rows.filter_map(Result::ok).collect()
}

pub(crate) fn count_for_room(conn: &Connection, room_id: &str) -> i64 {
    conn.query_row(
        "SELECT COUNT(*) FROM room_media WHERE room_id = ?1",
        params![room_id],
        |r| r.get(0),
    )
    .unwrap_or(0)
}

pub(crate) fn is_seeded(conn: &Connection, room_id: &str) -> bool {
    conn.query_row(
        "SELECT 1 FROM room_media_seeded WHERE room_id = ?1",
        params![room_id],
        |_| Ok(()),
    )
    .is_ok()
}

pub(crate) fn mark_seeded(conn: &Connection, room_id: &str) {
    let _ = conn.execute(
        "INSERT OR REPLACE INTO room_media_seeded (room_id, seeded_at_secs) \
         VALUES (?1, strftime('%s','now'))",
        params![room_id],
    );
}

// ── conversion from the already-converted FFI event (incremental path) ────────

/// `Some` iff `ev` is an `m.image` / `m.video` message. Field mapping mirrors
/// `timeline_convert.rs`'s per-msgtype arms.
pub(crate) fn row_from_ffi(ev: &crate::ffi::TimelineEvent) -> Option<RoomMediaRow> {
    let (kind, thumb_mxc, thumb_json) = match ev.msg_type.as_str() {
        "m.image" => (
            KIND_IMAGE,
            ev.image_thumbnail_url.clone(),
            ev.image_thumbnail_encrypted_json.clone(),
        ),
        "m.video" => (
            KIND_VIDEO,
            ev.video_thumbnail_url.clone(),
            ev.video_thumbnail_encrypted_json.clone(),
        ),
        _ => return None,
    };
    // MSC2530: a non-empty `image_filename` means `body` is a user caption.
    let caption = if ev.image_filename.is_empty() {
        String::new()
    } else {
        ev.body.clone()
    };
    Some(RoomMediaRow {
        event_id: ev.event_id.clone(),
        ts_ms: ev.timestamp as i64,
        sender: ev.sender.clone(),
        sender_name: ev.sender_name.clone(),
        sender_avatar_mxc: ev.sender_avatar_url.clone(),
        kind,
        caption,
        src_mxc: ev.source_url.clone(),
        src_encrypted: !ev.source_encrypted_json.is_empty(),
        src_json: ev.source_encrypted_json.clone(),
        thumb_mxc,
        thumb_encrypted: !thumb_json.is_empty(),
        thumb_json,
        media_w: ev.width as i64,
        media_h: ev.height as i64,
        blurhash: ev.blurhash.clone(),
        duration_ms: ev.video_duration_ms as i64,
        video_mime: ev.video_mime.clone(),
        video_autoplay: ev.video_autoplay,
        video_loop: ev.video_loop,
        video_no_audio: ev.video_no_audio,
        video_hide_controls: ev.video_hide_controls,
        video_gif: ev.video_gif,
        image_animated: ev.image_animated,
    })
}

#[cfg(not(test))]
pub(crate) use live::*;

#[cfg(not(test))]
mod live {
    use super::*;
    use matrix_sdk_base::event_cache::store::EventCacheStoreLockState;
    use matrix_sdk::ruma::events::room::message::MessageType;
    use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};
    use matrix_sdk::ruma::{OwnedRoomId, OwnedUserId};
    use matrix_sdk::Client;
    use std::collections::HashMap;

    use super::super::timeline_convert::{split_source, split_source_opt};
    use crate::client::ClientFfi;

    /// Convert one raw persisted event (from `EventCacheStore::get_room_events`)
    /// into a row. `sender_name` / avatar are left blank here — the caller fills
    /// them from the room's member cache, and the incremental path refreshes
    /// them whenever the event next flows through a timeline diff.
    pub(crate) fn row_from_raw(
        raw: &matrix_sdk::ruma::serde::Raw<AnySyncTimelineEvent>,
    ) -> Option<RoomMediaRow> {
        let AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(ev)) =
            raw.deserialize().ok()?
        else {
            return None;
        };
        let orig = ev.as_original()?;
        // An edit (`m.replace`) event is a distinct `m.room.message` whose
        // top-level msgtype is only a fallback; matrix-sdk-ui folds it into
        // the original for the live path, so the seed must skip it too rather
        // than index a bogus row keyed by the edit's own event id.
        if matches!(
            orig.content.relates_to,
            Some(matrix_sdk::ruma::events::room::message::Relation::Replacement(_))
        ) {
            return None;
        }
        let event_id = orig.event_id.to_string();
        let ts_ms: i64 = u64::from(orig.origin_server_ts.get()) as i64;
        let sender = orig.sender.to_string();

        let mau = |key: &str| -> bool {
            serde_json::from_str::<serde_json::Value>(raw.json().get())
                .ok()
                .and_then(|v| v.pointer(&format!("/content/info/{key}"))?.as_bool())
                .unwrap_or(false)
        };

        let mut row = RoomMediaRow {
            event_id,
            ts_ms,
            sender,
            ..Default::default()
        };

        match &orig.content.msgtype {
            MessageType::Image(i) => {
                let (src_mxc, src_json) = split_source(&i.source);
                let (thumb_mxc, thumb_json) = split_source_opt(
                    i.info.as_ref().and_then(|info| info.thumbnail_source.as_ref()),
                );
                let (w, h) = i
                    .info
                    .as_ref()
                    .map(|info| {
                        (
                            info.width.map(u64::from).unwrap_or(0) as i64,
                            info.height.map(u64::from).unwrap_or(0) as i64,
                        )
                    })
                    .unwrap_or((0, 0));
                row.kind = KIND_IMAGE;
                row.caption = if i.filename.is_some() {
                    i.body.clone()
                } else {
                    String::new()
                };
                row.src_mxc = src_mxc;
                row.src_encrypted = !src_json.is_empty();
                row.src_json = src_json;
                row.thumb_mxc = thumb_mxc;
                row.thumb_encrypted = !thumb_json.is_empty();
                row.thumb_json = thumb_json;
                row.media_w = w;
                row.media_h = h;
                row.blurhash = i
                    .info
                    .as_ref()
                    .and_then(|info| info.blurhash.clone())
                    .unwrap_or_default();
                row.image_animated = i
                    .info
                    .as_ref()
                    .and_then(|info| info.is_animated)
                    .unwrap_or(false);
            }
            MessageType::Video(v) => {
                let (src_mxc, src_json) = split_source(&v.source);
                let (w, h, dur_ms, mime, thumb_mxc, thumb_json) = v
                    .info
                    .as_ref()
                    .map(|info| {
                        let (tu, te) = split_source_opt(info.thumbnail_source.as_ref());
                        (
                            info.width.map(u64::from).unwrap_or(0) as i64,
                            info.height.map(u64::from).unwrap_or(0) as i64,
                            info.duration.map(|d| d.as_millis() as i64).unwrap_or(0),
                            info.mimetype.clone().unwrap_or_default(),
                            tu,
                            te,
                        )
                    })
                    .unwrap_or_default();
                let gif = mau("fi.mau.gif");
                row.kind = KIND_VIDEO;
                row.src_mxc = src_mxc;
                row.src_encrypted = !src_json.is_empty();
                row.src_json = src_json;
                row.thumb_mxc = thumb_mxc;
                row.thumb_encrypted = !thumb_json.is_empty();
                row.thumb_json = thumb_json;
                row.media_w = w;
                row.media_h = h;
                row.duration_ms = dur_ms;
                row.video_mime = mime;
                row.video_gif = gif;
                row.video_autoplay = mau("fi.mau.autoplay") || gif;
                row.video_loop = mau("fi.mau.loop") || gif;
                row.video_no_audio = mau("fi.mau.no_audio") || gif;
                row.video_hide_controls = mau("fi.mau.hide_controls") || gif;
                row.blurhash = v
                    .info
                    .as_ref()
                    .and_then(|info| info.blurhash.clone())
                    .unwrap_or_default();
            }
            _ => return None,
        }
        Some(row)
    }

    pub(crate) fn to_ffi(row: &RoomMediaRow) -> crate::ffi::MediaIndexRowFfi {
        crate::ffi::MediaIndexRowFfi {
            event_id: row.event_id.clone(),
            ts_ms: row.ts_ms.max(0) as u64,
            sender: row.sender.clone(),
            sender_name: row.sender_name.clone(),
            sender_avatar_mxc: row.sender_avatar_mxc.clone(),
            kind: row.kind as u8,
            caption: row.caption.clone(),
            src_mxc: row.src_mxc.clone(),
            src_encrypted: row.src_encrypted,
            src_json: row.src_json.clone(),
            thumb_mxc: row.thumb_mxc.clone(),
            thumb_encrypted: row.thumb_encrypted,
            thumb_json: row.thumb_json.clone(),
            media_w: row.media_w.max(0) as u32,
            media_h: row.media_h.max(0) as u32,
            blurhash: row.blurhash.clone(),
            duration_ms: row.duration_ms.max(0) as u64,
            video_mime: row.video_mime.clone(),
            video_autoplay: row.video_autoplay,
            video_loop: row.video_loop,
            video_no_audio: row.video_no_audio,
            video_hide_controls: row.video_hide_controls,
            video_gif: row.video_gif,
            image_animated: row.image_animated,
        }
    }

    /// Cloneable handle threaded into the timeline streaming / pagination tasks
    /// (mirrors `search::SearchIndexCtx`). Cheap to clone — one `Arc`.
    #[derive(Clone)]
    pub(crate) struct MediaCtx {
        pub(crate) db: Arc<Mutex<Option<Connection>>>,
    }

    impl MediaCtx {
        /// Index one converted timeline event: upsert an image/video, or delete
        /// any prior row when the slot converts to a redaction / UTD tombstone.
        pub(crate) fn note_event(&self, ev: &crate::ffi::TimelineEvent) {
            let guard = self.db.lock();
            let Some(conn) = guard.as_ref() else { return };
            if let Some(row) = row_from_ffi(ev) {
                upsert(conn, &ev.room_id, &row);
            } else if !ev.event_id.is_empty()
                && (ev.msg_type == "m.redacted" || ev.msg_type == "m.utd")
            {
                delete_event(conn, &ev.room_id, &ev.event_id);
            }
        }

        /// Drop a previously-indexed event whose slot is no longer renderable.
        pub(crate) fn forget_event(&self, room_id: &str, event_id: &str) {
            if event_id.is_empty() {
                return;
            }
            let guard = self.db.lock();
            if let Some(conn) = guard.as_ref() {
                delete_event(conn, room_id, event_id);
            }
        }
    }

    /// One-shot enumeration of the room's persisted events from the SDK
    /// event-cache store (no network) into `room_media`. Best-effort.
    async fn seed_room_media(
        client: &Client,
        db: &Arc<Mutex<Option<Connection>>>,
        room_id: &str,
    ) {
        let Ok(rid) = room_id.parse::<OwnedRoomId>() else {
            return;
        };
        let events = {
            // The cross-process lock guard drops at the end of this block,
            // before we touch sqlite.
            match client.event_cache_store().lock().await {
                Ok(EventCacheStoreLockState::Clean(guard))
                | Ok(EventCacheStoreLockState::Dirty(guard)) => {
                    match guard
                        .get_room_events(&rid, Some("m.room.message"), None)
                        .await
                    {
                        Ok(e) => e,
                        Err(_) => return,
                    }
                }
                Err(_) => return,
            }
        };

        let mut rows: Vec<RoomMediaRow> = Vec::new();
        for ev in &events {
            if let Some(row) = row_from_raw(ev.raw()) {
                rows.push(row);
            }
        }

        // Resolve each distinct sender's display name / avatar once from the
        // room's already-synced member state (no network).
        if let Some(room) = client.get_room(&rid) {
            let mut members: HashMap<OwnedUserId, (String, String)> = HashMap::new();
            for row in &mut rows {
                let Ok(uid) = row.sender.parse::<OwnedUserId>() else {
                    continue;
                };
                if !members.contains_key(&uid) {
                    let resolved = match room.get_member_no_sync(&uid).await {
                        Ok(Some(m)) => (
                            m.name().to_owned(),
                            m.avatar_url().map(|u| u.to_string()).unwrap_or_default(),
                        ),
                        _ => (String::new(), String::new()),
                    };
                    members.insert(uid.clone(), resolved);
                }
                if let Some((name, avatar)) = members.get(&uid) {
                    row.sender_name = name.clone();
                    row.sender_avatar_mxc = avatar.clone();
                }
            }
        }

        let mut guard = db.lock();
        if let Some(conn) = guard.as_mut() {
            upsert_many(conn, room_id, &rows);
        }
    }

    impl ClientFfi {
        /// Handed to the timeline tasks. Always `Some` once constructed — the
        /// per-write "is the DB open?" check inside `MediaCtx` is the real gate.
        pub(crate) fn media_ctx(&self) -> Option<MediaCtx> {
            Some(MediaCtx {
                db: Arc::clone(&self.app_cache_db),
            })
        }

        /// Instant offline page of the per-room media index (newest first).
        /// Seeds the index from the local event-cache store on the first call
        /// for a room (no network). Delivers `on_room_media_page`.
        pub fn load_room_media_page_async(
            &self,
            request_id: u64,
            room_id: &str,
            before_ts_ms: u64,
            limit: u32,
        ) {
            let handler = self.handler.clone();
            let db = Arc::clone(&self.app_cache_db);
            let client = self.client.clone();
            let room_id = room_id.to_owned();
            let before = before_ts_ms as i64;
            let limit = limit.clamp(1, 500) as i64;

            self.rt.spawn(async move {
                let need_seed = {
                    let g = db.lock();
                    g.as_ref().map(|c| !is_seeded(c, &room_id)).unwrap_or(false)
                };
                if need_seed {
                    if let Some(client) = &client {
                        seed_room_media(client, &db, &room_id).await;
                    }
                    if let Some(c) = db.lock().as_ref() {
                        mark_seeded(c, &room_id);
                    }
                }

                let (rows, total) = {
                    let g = db.lock();
                    match g.as_ref() {
                        Some(c) => (
                            query_page(c, &room_id, before, limit),
                            count_for_room(c, &room_id) as u64,
                        ),
                        None => (Vec::new(), 0),
                    }
                };
                let reached_db_end = (rows.len() as i64) < limit;

                if let Some(h) = &handler {
                    let ffi: Vec<crate::ffi::MediaIndexRowFfi> =
                        rows.iter().map(to_ffi).collect();
                    h.lock()
                        .on_room_media_page(request_id, &ffi, reached_db_end, total);
                }
            });
        }

        /// Synchronous `COUNT(*)` of a room's indexed image/video. 0 until the
        /// room's gallery has been opened once. One indexed count under a
        /// non-poisoning `parking_lot` lock — cheap enough for the "Media (N)"
        /// badge's user-paced refresh sites.
        pub fn room_media_count(&self, room_id: &str) -> u64 {
            let g = self.app_cache_db.lock();
            g.as_ref()
                .map(|c| count_for_room(c, room_id).max(0) as u64)
                .unwrap_or(0)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(CREATE_TABLE_SQL).unwrap();
        conn
    }

    fn img(event_id: &str, ts_ms: i64) -> RoomMediaRow {
        RoomMediaRow {
            event_id: event_id.into(),
            ts_ms,
            sender: "@a:x".into(),
            kind: KIND_IMAGE,
            src_mxc: "mxc://x/img".into(),
            media_w: 100,
            media_h: 80,
            ..Default::default()
        }
    }

    #[test]
    fn round_trip_and_ordering() {
        let conn = db();
        upsert(&conn, "!r:x", &img("$c", 300));
        upsert(&conn, "!r:x", &img("$a", 100));
        upsert(&conn, "!r:x", &img("$b", 200));

        let page = query_page(&conn, "!r:x", 0, 10);
        let ids: Vec<_> = page.iter().map(|r| r.event_id.as_str()).collect();
        assert_eq!(ids, ["$c", "$b", "$a"]); // newest first
        assert_eq!(page[0].media_w, 100);
        assert_eq!(count_for_room(&conn, "!r:x"), 3);
    }

    #[test]
    fn upsert_replaces_in_place() {
        let conn = db();
        upsert(&conn, "!r:x", &img("$a", 100));
        let mut updated = img("$a", 100);
        updated.caption = "hi".into();
        upsert(&conn, "!r:x", &updated);
        assert_eq!(count_for_room(&conn, "!r:x"), 1);
        assert_eq!(query_page(&conn, "!r:x", 0, 10)[0].caption, "hi");
    }

    #[test]
    fn paging_cursor_and_limit() {
        let conn = db();
        for i in 0..5 {
            upsert(&conn, "!r:x", &img(&format!("$e{i}"), (i as i64 + 1) * 100));
        }
        let first = query_page(&conn, "!r:x", 0, 2);
        assert_eq!(first.len(), 2);
        assert_eq!(first[0].ts_ms, 500);
        let cursor = first.last().unwrap().ts_ms; // 400
        let second = query_page(&conn, "!r:x", cursor, 2);
        assert_eq!(
            second.iter().map(|r| r.ts_ms).collect::<Vec<_>>(),
            [300, 200]
        );
    }

    #[test]
    fn delete_event_and_two_room_isolation() {
        let conn = db();
        upsert(&conn, "!r1:x", &img("$a", 100));
        upsert(&conn, "!r2:x", &img("$a", 100));
        delete_event(&conn, "!r1:x", "$a");
        assert_eq!(count_for_room(&conn, "!r1:x"), 0);
        assert_eq!(count_for_room(&conn, "!r2:x"), 1);
    }

    #[test]
    fn seeded_marker() {
        let conn = db();
        assert!(!is_seeded(&conn, "!r:x"));
        mark_seeded(&conn, "!r:x");
        assert!(is_seeded(&conn, "!r:x"));
    }

    #[test]
    fn row_from_ffi_plain_image() {
        let ev = crate::ffi::TimelineEvent {
            event_id: "$img".into(),
            room_id: "!r:x".into(),
            sender: "@a:x".into(),
            timestamp: 1234,
            msg_type: "m.image".into(),
            source_url: "mxc://x/i".into(),
            width: 640,
            height: 480,
            image_thumbnail_url: "mxc://x/t".into(),
            ..Default::default()
        };
        let row = row_from_ffi(&ev).unwrap();
        assert_eq!(row.kind, KIND_IMAGE);
        assert_eq!(row.ts_ms, 1234);
        assert!(!row.src_encrypted);
        assert_eq!(row.thumb_mxc, "mxc://x/t");
        assert_eq!(row.media_w, 640);
        assert!(row.caption.is_empty());
    }

    #[test]
    fn row_from_ffi_encrypted_image_with_caption() {
        let ev = crate::ffi::TimelineEvent {
            event_id: "$img".into(),
            msg_type: "m.image".into(),
            body: "look".into(),
            image_filename: "pic.png".into(),
            source_url: "mxc://x/i".into(),
            source_encrypted_json: "{\"url\":\"mxc://x/i\"}".into(),
            ..Default::default()
        };
        let row = row_from_ffi(&ev).unwrap();
        assert!(row.src_encrypted);
        assert_eq!(row.caption, "look");
    }

    #[test]
    fn row_from_ffi_video_hints() {
        let ev = crate::ffi::TimelineEvent {
            event_id: "$v".into(),
            msg_type: "m.video".into(),
            source_url: "mxc://x/v".into(),
            video_duration_ms: 4200,
            video_mime: "video/mp4".into(),
            video_gif: true,
            video_autoplay: true,
            ..Default::default()
        };
        let row = row_from_ffi(&ev).unwrap();
        assert_eq!(row.kind, KIND_VIDEO);
        assert_eq!(row.duration_ms, 4200);
        assert!(row.video_gif);
        assert!(row.thumb_mxc.is_empty());
    }

    #[test]
    fn row_from_ffi_ignores_non_media() {
        let ev = crate::ffi::TimelineEvent {
            msg_type: "m.text".into(),
            body: "hi".into(),
            ..Default::default()
        };
        assert!(row_from_ffi(&ev).is_none());
    }
}
