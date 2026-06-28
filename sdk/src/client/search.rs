//! Local full-text message search index.
//!
//! matrix-sdk ships no search, and the Matrix server-side `/search` endpoint
//! cannot see into encrypted rooms. The only way to search E2EE history is to
//! index the already-decrypted message bodies locally. This module owns a
//! SQLite FTS5 index living in the per-account `search_index.db` (created in
//! `backfill::open_search_db`).
//!
//! The functions here are pure over a `&rusqlite::Connection` so they unit-test
//! against an in-memory DB without the live client (mirrors the `#[cfg(test)]`
//! split used throughout `client/`). Indexing is gated by an opt-in setting at
//! the `ClientFfi` layer; this module just reads and writes the tables.

use rusqlite::Connection;

#[cfg(not(test))]
use parking_lot::Mutex;
#[cfg(not(test))]
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(not(test))]
use std::sync::Arc;

/// One search hit returned to the FFI/UI layer. Mirrors the `SearchHit` shared
/// struct declared in the cxx bridge; the room display name is resolved by the
/// caller (this module only knows the room id).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SearchHit {
    pub event_id: String,
    pub room_id: String,
    pub sender: String,
    pub sender_name: String,
    pub body: String,
    pub timestamp_ms: u64,
}

/// Create the index schema if absent. Safe to call on every DB open.
///
/// `message_index` is the source-of-truth row table; `message_fts` is an
/// external-content FTS5 table mirroring `body`, kept in sync by triggers so
/// edits (`UPDATE`) and redactions (`DELETE`) stay O(1) and we can still
/// `SELECT` full metadata back out by joining on rowid.
pub fn ensure_schema(conn: &Connection) -> rusqlite::Result<()> {
    conn.execute_batch(
        "CREATE TABLE IF NOT EXISTS message_index (
             event_id    TEXT NOT NULL PRIMARY KEY,
             room_id     TEXT NOT NULL,
             sender      TEXT NOT NULL DEFAULT '',
             sender_name TEXT NOT NULL DEFAULT '',
             ts          INTEGER NOT NULL,
             body        TEXT NOT NULL
         );
         CREATE INDEX IF NOT EXISTS idx_msg_room ON message_index(room_id);
         CREATE VIRTUAL TABLE IF NOT EXISTS message_fts USING fts5(
             body,
             content='message_index',
             content_rowid='rowid',
             tokenize='unicode61 remove_diacritics 2'
         );
         CREATE TRIGGER IF NOT EXISTS message_ai AFTER INSERT ON message_index BEGIN
             INSERT INTO message_fts(rowid, body) VALUES (new.rowid, new.body);
         END;
         CREATE TRIGGER IF NOT EXISTS message_ad AFTER DELETE ON message_index BEGIN
             INSERT INTO message_fts(message_fts, rowid, body)
                 VALUES('delete', old.rowid, old.body);
         END;
         CREATE TRIGGER IF NOT EXISTS message_au AFTER UPDATE ON message_index BEGIN
             INSERT INTO message_fts(message_fts, rowid, body)
                 VALUES('delete', old.rowid, old.body);
             INSERT INTO message_fts(rowid, body) VALUES (new.rowid, new.body);
         END;
         -- One-row table tracking whether the one-time history backfill has run
         -- to completion, so an interrupted crawl resumes on the next enable
         -- rather than being mistaken for 'done' the moment it writes a row.
         CREATE TABLE IF NOT EXISTS search_state (
             id            INTEGER PRIMARY KEY CHECK (id = 0),
             backfill_done INTEGER NOT NULL DEFAULT 0
         );
         INSERT OR IGNORE INTO search_state (id, backfill_done) VALUES (0, 0);",
    )
}

/// True when the one-time history backfill has previously run to completion.
pub fn is_backfill_complete(conn: &Connection) -> bool {
    conn.query_row(
        "SELECT backfill_done FROM search_state WHERE id = 0",
        [],
        |r| r.get::<_, i64>(0),
    )
    .map(|n| n != 0)
    .unwrap_or(false)
}

/// Record that the history backfill finished a full pass.
pub fn mark_backfill_complete(conn: &Connection) -> rusqlite::Result<()> {
    conn.execute("UPDATE search_state SET backfill_done = 1 WHERE id = 0", [])?;
    Ok(())
}

/// Summary stats for the Settings panel: how much is indexed and whether the
/// one-time history crawl has finished.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct IndexStats {
    pub message_count: u64,
    pub room_count: u64,
    /// Timestamp (ms) of the oldest indexed message, or 0 when the index is
    /// empty. Drives the "covers messages since <month year>" line.
    pub oldest_ts_ms: u64,
    pub backfill_done: bool,
}

/// On-disk space consumed by the search index tables and the FTS5 shadow tables.
///
/// Uses the `dbstat` virtual table, which counts live SQLite pages. This is a
/// full B-tree walk (O(pages)), so it should only be called when the Settings
/// panel first opens — not on every 2-second refresh tick.
pub fn index_size_bytes(conn: &Connection) -> u64 {
    conn.query_row(
        "SELECT COALESCE(SUM(pgsize), 0) FROM dbstat \
         WHERE name IN ('message_index', 'search_state') OR name LIKE 'message_fts%'",
        [],
        |r| r.get::<_, i64>(0),
    )
    .map(|n| n.max(0) as u64)
    .unwrap_or(0)
}

/// Compute index stats in a single table scan.
pub fn stats(conn: &Connection) -> IndexStats {
    let (message_count, room_count, oldest_ts_ms) = conn
        .query_row(
            "SELECT COUNT(*), COUNT(DISTINCT room_id), COALESCE(MIN(ts), 0) \
             FROM message_index",
            [],
            |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    r.get::<_, i64>(1)?,
                    r.get::<_, i64>(2)?,
                ))
            },
        )
        .unwrap_or((0, 0, 0));
    IndexStats {
        message_count: message_count.max(0) as u64,
        room_count: room_count.max(0) as u64,
        oldest_ts_ms: oldest_ts_ms.max(0) as u64,
        backfill_done: is_backfill_complete(conn),
    }
}

/// Upsert one message into the index. Re-indexing the same `event_id` (an edit
/// arriving as `m.replace`) updates the stored body in place via the conflict
/// clause, which fires the `message_au` trigger to refresh the FTS row.
pub fn index_record(
    conn: &Connection,
    event_id: &str,
    room_id: &str,
    sender: &str,
    sender_name: &str,
    ts_ms: u64,
    body: &str,
) -> rusqlite::Result<()> {
    conn.execute(
        "INSERT INTO message_index (event_id, room_id, sender, sender_name, ts, body)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6)
         ON CONFLICT(event_id) DO UPDATE SET
             room_id     = excluded.room_id,
             sender      = excluded.sender,
             sender_name = excluded.sender_name,
             ts          = excluded.ts,
             body        = excluded.body",
        rusqlite::params![event_id, room_id, sender, sender_name, ts_ms as i64, body],
    )?;
    Ok(())
}

/// Remove one message from the index (redaction / removal diff). No-op if the
/// event was never indexed.
pub fn remove_record(conn: &Connection, event_id: &str) -> rusqlite::Result<()> {
    conn.execute(
        "DELETE FROM message_index WHERE event_id = ?1",
        rusqlite::params![event_id],
    )?;
    Ok(())
}

/// Drop everything from the index (privacy toggle turned off) and reset the
/// backfill-complete marker so re-enabling re-crawls history.
pub fn clear(conn: &Connection) -> rusqlite::Result<()> {
    // Deleting the rows fires the AD trigger per row, keeping the FTS table in
    // sync; an explicit `DELETE FROM message_fts` is therefore unnecessary.
    conn.execute_batch(
        "DELETE FROM message_index;
         UPDATE search_state SET backfill_done = 0 WHERE id = 0;",
    )
}

/// Turn arbitrary user input into a safe FTS5 MATCH expression.
///
/// Raw user text fed to `MATCH` is a query language: bare punctuation,
/// unbalanced quotes, or operator words (`AND`, `OR`, `NEAR`, `*`) throw a SQL
/// error or do something surprising. We split on whitespace, strip characters
/// that aren't alphanumeric (Unicode) and wrap each surviving token in double
/// quotes, turning every token into a literal phrase. A trailing `*` is
/// appended to the final token so partial words match as the user types.
/// Returns `None` when nothing searchable remains.
pub fn build_match_query(input: &str) -> Option<String> {
    let mut tokens: Vec<String> = Vec::new();
    for raw in input.split_whitespace() {
        // Keep alphanumerics (any script) and intra-token marks SQLite's
        // unicode61 tokenizer would itself split on are dropped; doubled quotes
        // inside are escaped by FTS5's "" convention (none survive the filter,
        // but be defensive).
        let cleaned: String = raw.chars().filter(|c| c.is_alphanumeric()).collect();
        if !cleaned.is_empty() {
            tokens.push(cleaned);
        }
    }
    if tokens.is_empty() {
        return None;
    }
    let last = tokens.len() - 1;
    let mut out = String::new();
    for (i, tok) in tokens.iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        out.push('"');
        out.push_str(tok);
        out.push('"');
        // Prefix-match the final token for incremental-typing UX.
        if i == last {
            out.push('*');
        }
    }
    Some(out)
}

/// Full-text search. `room_filter` of `Some(id)` scopes to one room (in-room
/// find bar); `None` searches the whole account (global overlay). Results are
/// ranked by FTS5 relevance, then most-recent-first, capped at `limit`.
pub fn search(
    conn: &Connection,
    query: &str,
    room_filter: Option<&str>,
    limit: u32,
) -> rusqlite::Result<Vec<SearchHit>> {
    let Some(match_expr) = build_match_query(query) else {
        return Ok(Vec::new());
    };

    let sql = if room_filter.is_some() {
        "SELECT mi.event_id, mi.room_id, mi.sender, mi.sender_name, mi.ts, mi.body
             FROM message_fts f
             JOIN message_index mi ON mi.rowid = f.rowid
             WHERE f.body MATCH ?1 AND mi.room_id = ?2
             ORDER BY f.rank, mi.ts DESC
             LIMIT ?3"
    } else {
        "SELECT mi.event_id, mi.room_id, mi.sender, mi.sender_name, mi.ts, mi.body
             FROM message_fts f
             JOIN message_index mi ON mi.rowid = f.rowid
             WHERE f.body MATCH ?1
             ORDER BY f.rank, mi.ts DESC
             LIMIT ?2"
    };

    let mut stmt = conn.prepare(sql)?;
    let map_row = |row: &rusqlite::Row| {
        Ok(SearchHit {
            event_id: row.get(0)?,
            room_id: row.get(1)?,
            sender: row.get(2)?,
            sender_name: row.get(3)?,
            timestamp_ms: row.get::<_, i64>(4)? as u64,
            body: row.get(5)?,
        })
    };

    let rows = if let Some(room) = room_filter {
        stmt.query_map(rusqlite::params![match_expr, room, limit], map_row)?
            .collect::<rusqlite::Result<Vec<_>>>()?
    } else {
        stmt.query_map(rusqlite::params![match_expr, limit], map_row)?
            .collect::<rusqlite::Result<Vec<_>>>()?
    };
    Ok(rows)
}

/// True when an event of this `msg_type` carries searchable text. Redacted /
/// undecryptable events and virtual timeline items (day dividers, read markers,
/// timeline-start) carry nothing to index. Everything else — `m.text`,
/// `m.notice`, `m.emote`, and media events whose `body` holds a caption — is
/// indexable when its body is non-empty (checked separately).
#[cfg(not(test))]
fn is_indexable(msg_type: &str) -> bool {
    !(msg_type.starts_with("virtual.") || msg_type == "m.redacted" || msg_type == "m.utd")
}

/// Handle to the per-account index plus the opt-in gate, cloned into the
/// timeline streaming/pagination/backfill tasks so they can index events
/// without holding `&ClientFfi`. Cheap to clone (two `Arc`s).
#[cfg(not(test))]
#[derive(Clone)]
pub(crate) struct SearchIndexCtx {
    pub(crate) db: Arc<Mutex<Option<Connection>>>,
    pub(crate) enabled: Arc<AtomicBool>,
}

#[cfg(not(test))]
impl SearchIndexCtx {
    /// Index one converted timeline event. No-op when indexing is disabled or
    /// the DB isn't open. For an event that is *not* indexable (a redaction
    /// arriving as a `Set` to an `m.redacted` placeholder, or an edit blanking
    /// the body) this instead *removes* any previously-indexed row, so stale
    /// plaintext never lingers after a message is redacted.
    pub(crate) fn index_event(&self, ev: &crate::ffi::TimelineEvent) {
        if !self.enabled.load(Ordering::Relaxed) {
            return;
        }
        let guard = self.db.lock();
        let Some(conn) = guard.as_ref() else { return };
        if is_indexable(&ev.msg_type) && !ev.body.trim().is_empty() {
            let _ = index_record(
                conn,
                &ev.event_id,
                &ev.room_id,
                &ev.sender,
                &ev.sender_name,
                ev.timestamp,
                &ev.body,
            );
        } else if !ev.event_id.is_empty() {
            let _ = remove_record(conn, &ev.event_id);
        }
    }

    /// Remove a previously-indexed event by id. Used when a timeline slot that
    /// held an indexed message converts to something non-renderable for the
    /// channel (a `Set` whose new value yields `None`), so stale plaintext does
    /// not linger. No-op when indexing is disabled or the id was never indexed.
    pub(crate) fn remove_event(&self, event_id: &str) {
        if event_id.is_empty() || !self.enabled.load(Ordering::Relaxed) {
            return;
        }
        let guard = self.db.lock();
        if let Some(conn) = guard.as_ref() {
            let _ = remove_record(conn, event_id);
        }
    }
}

// ── Live FFI entry points ─────────────────────────────────────────────────

#[cfg(not(test))]
use super::{ClientFfi, SendHandler};

/// Deliver an async search outcome to C++ via the event handler. Tolerates a
/// detached handler (shutdown) by dropping.
#[cfg(not(test))]
fn deliver_search(
    handler: &Option<Arc<Mutex<SendHandler>>>,
    request_id: u64,
    result: Result<Vec<crate::ffi::SearchHit>, String>,
) {
    let Some(h) = handler else { return };
    let g = h.lock();
    match result {
        Ok(hits) => g.on_search_results(request_id, &hits),
        Err(e) => g.on_search_failed(request_id, &e),
    }
}

/// Walk one room's locally-available timeline (paginating back up to `target`
/// events) and index every text-bearing event. Used by the lazy backfill that
/// runs when indexing is first enabled, so existing history becomes searchable
/// without a foreground subscribe. Bails early if the gate is flipped off.
#[cfg(not(test))]
async fn index_room_history(
    room: &matrix_sdk::Room,
    db: &Arc<Mutex<Option<Connection>>>,
    enabled: &AtomicBool,
    me: Option<&matrix_sdk::ruma::UserId>,
    target: usize,
) {
    use super::timeline_convert::timeline_item_to_ffi;
    use matrix_sdk_ui::timeline::{RoomExt, TimelineItemKind};

    let Ok(timeline) = room.timeline().await else {
        return;
    };
    let (initial, _stream) = timeline.subscribe().await;
    let mut have = initial
        .iter()
        .filter(|i| matches!(i.kind(), TimelineItemKind::Event(_)))
        .count();
    while have < target {
        if !enabled.load(Ordering::Relaxed) {
            return;
        }
        match timeline.paginate_backwards(100).await {
            Ok(true) => break, // reached the start
            Ok(false) => {}
            Err(_) => break,
        }
        let new_have = timeline
            .items()
            .await
            .iter()
            .filter(|i| matches!(i.kind(), TimelineItemKind::Event(_)))
            .count();
        if new_have == have {
            break; // no forward progress; avoid spinning
        }
        have = new_have;
    }

    let room_id = room.room_id().as_str().to_owned();
    let items = timeline.items().await;
    for item in items.iter() {
        if !enabled.load(Ordering::Relaxed) {
            return;
        }
        if let Some(ev) = timeline_item_to_ffi(item, &room_id, room, me).await {
            if is_indexable(&ev.msg_type) && !ev.body.trim().is_empty() {
                let guard = db.lock();
                if let Some(conn) = guard.as_ref() {
                    let _ = index_record(
                        conn,
                        &ev.event_id,
                        &ev.room_id,
                        &ev.sender,
                        &ev.sender_name,
                        ev.timestamp,
                        &ev.body,
                    );
                }
            }
        }
    }
}

#[cfg(not(test))]
impl ClientFfi {
    /// Build the indexing context handed to the timeline streaming/pagination
    /// tasks. Always `Some` once constructed — the per-write `enabled` check
    /// inside `SearchIndexCtx` is what actually gates indexing — so callers can
    /// pass it unconditionally.
    pub(super) fn search_index_ctx(&self) -> Option<SearchIndexCtx> {
        Some(SearchIndexCtx {
            db: Arc::clone(&self.search_db),
            enabled: Arc::clone(&self.search_indexing_enabled),
        })
    }

    /// Flip the opt-in indexing gate. Enabling kicks off a one-shot lazy
    /// backfill that indexes existing history; disabling clears the on-disk
    /// index so no decrypted plaintext remains at rest. No-op if unchanged.
    pub fn set_search_indexing_enabled(&self, enabled: bool) {
        let was = self
            .search_indexing_enabled
            .swap(enabled, Ordering::Relaxed);
        if was == enabled {
            return;
        }
        if enabled {
            // The backfill checks the completion marker off-thread and skips a
            // crawl that has already finished.
            self.spawn_index_backfill();
        } else {
            // Clear the on-disk index off the UI thread: a large DELETE + the
            // per-row FTS 'delete' trigger cascade (and lock contention with
            // the indexing/search tasks) must not block the settings window.
            let db = Arc::clone(&self.search_db);
            self.rt.spawn(async move {
                let guard = db.lock();
                if let Some(conn) = guard.as_ref() {
                    let _ = clear(conn);
                }
            });
        }
    }

    /// On-disk size of the search index tables — a `dbstat` B-tree walk. Must
    /// only be called once when the Settings panel opens (not on the 2s poll).
    pub fn search_index_size_bytes(&self) -> u64 {
        let guard = self.search_db.lock();
        match guard.as_ref() {
            Some(conn) => index_size_bytes(conn),
            None => 0,
        }
    }

    /// Synchronous index-stats read for the Settings panel. A single aggregate
    /// scan, called only while that panel is open (one-shot + a slow poll), so
    /// it is not on any hot path. Returns zeros when the DB isn't open.
    pub fn search_index_stats(&self) -> crate::ffi::SearchIndexStats {
        let guard = self.search_db.lock();
        match guard.as_ref() {
            Some(conn) => {
                let s = stats(conn);
                crate::ffi::SearchIndexStats {
                    message_count: s.message_count,
                    room_count: s.room_count,
                    oldest_ts_ms: s.oldest_ts_ms,
                    backfill_done: s.backfill_done,
                    index_bytes: 0, // populated C++-side from search_index_size_bytes
                }
            }
            None => crate::ffi::SearchIndexStats {
                message_count: 0,
                room_count: 0,
                oldest_ts_ms: 0,
                backfill_done: false,
                index_bytes: 0,
            },
        }
    }

    /// Spawn a bounded-concurrency pass that indexes the recent history of every
    /// joined room. Shares `warm_semaphore` with the other warm-up tasks so the
    /// combined pagination load stays bounded. Skips rooms with a live
    /// foreground timeline (building a second timeline for them would paginate
    /// history into the stream the user is viewing); their current messages are
    /// indexed by the live path instead. Runs only once per index — the
    /// completion marker, set after a full pass, makes re-enables a no-op while
    /// still resuming an interrupted crawl.
    fn spawn_index_backfill(&self) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let db = Arc::clone(&self.search_db);
        let enabled = Arc::clone(&self.search_indexing_enabled);
        let semaphore = Arc::clone(&self.warm_semaphore);
        let me = client.user_id().map(|u| u.to_owned());
        // Fast in-memory snapshot of the foreground rooms to skip.
        let skip: std::collections::HashSet<matrix_sdk::ruma::OwnedRoomId> =
            self.timelines.read().keys().cloned().collect();

        self.rt.spawn(async move {
            // Completion check off the UI thread — skip a crawl that already
            // finished in a prior session.
            {
                let guard = db.lock();
                if let Some(conn) = guard.as_ref() {
                    if is_backfill_complete(conn) {
                        return;
                    }
                }
            }

            let mut joinset = tokio::task::JoinSet::new();
            for room in client.joined_rooms() {
                if !enabled.load(Ordering::Relaxed) {
                    break;
                }
                if skip.contains(room.room_id()) {
                    continue;
                }
                let db = Arc::clone(&db);
                let enabled = Arc::clone(&enabled);
                let sem = semaphore.clone();
                let me = me.clone();
                joinset.spawn(async move {
                    let _permit = match sem.acquire_owned().await {
                        Ok(p) => p,
                        Err(_) => return,
                    };
                    if !enabled.load(Ordering::Relaxed) {
                        return;
                    }
                    index_room_history(&room, &db, &enabled, me.as_deref(), 500).await;
                });
            }
            while joinset.join_next().await.is_some() {}

            // Mark complete only if we finished a full pass without being
            // disabled mid-crawl, so an interrupted crawl is retried next time.
            if enabled.load(Ordering::Relaxed) {
                let guard = db.lock();
                if let Some(conn) = guard.as_ref() {
                    let _ = mark_backfill_complete(conn);
                }
            }
        });
    }

    /// Non-blocking full-text search. Spawns the FTS query on the runtime,
    /// resolves room display names, and fires `on_search_results(request_id, …)`
    /// (or `on_search_failed`) on completion. The C++ controller debounces and
    /// drops stale `request_id`s.
    pub fn search_messages_async(&self, request_id: u64, query: &str, room_id: &str, limit: u32) {
        let handler = self.handler.clone();
        let db = Arc::clone(&self.search_db);
        let query = query.to_owned();
        let room_filter = if room_id.is_empty() {
            None
        } else {
            Some(room_id.to_owned())
        };
        let limit = limit.clamp(1, 500);

        self.rt.spawn(async move {
            // Pure synchronous FTS read — no room-name resolution here. The C++
            // shell fills SearchHit::room_name from its already-cached room list
            // (RoomInfo.name) so we avoid a per-result member-store walk on every
            // keystroke. room_name is left empty on the bridge.
            let result: Result<Vec<crate::ffi::SearchHit>, String> = {
                let guard = db.lock();
                match guard.as_ref() {
                    Some(conn) => search(conn, &query, room_filter.as_deref(), limit)
                        .map(|hits| {
                            hits.into_iter()
                                .map(|h| crate::ffi::SearchHit {
                                    event_id: h.event_id,
                                    room_id: h.room_id,
                                    room_name: String::new(),
                                    sender: h.sender,
                                    sender_name: h.sender_name,
                                    body: h.body,
                                    timestamp_ms: h.timestamp_ms,
                                })
                                .collect()
                        })
                        .map_err(|e| e.to_string()),
                    None => Err("search index not open".to_string()),
                }
            };
            deliver_search(&handler, request_id, result);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        ensure_schema(&conn).unwrap();
        conn
    }

    #[test]
    fn fts5_is_available() {
        // The smoke test the plan calls for: if the linked SQLite lacks FTS5,
        // `ensure_schema` fails here and the whole feature is a non-starter.
        let conn = Connection::open_in_memory().unwrap();
        ensure_schema(&conn).expect("FTS5 must be compiled into the linked SQLite");
    }

    #[test]
    fn indexes_and_finds_a_message() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "Alice", 1000, "hello world").unwrap();
        let hits = search(&conn, "hello", None, 10).unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].event_id, "$e1");
        assert_eq!(hits[0].body, "hello world");
    }

    #[test]
    fn match_is_case_and_diacritic_insensitive() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "Café Crème").unwrap();
        assert_eq!(search(&conn, "cafe", None, 10).unwrap().len(), 1);
        assert_eq!(search(&conn, "CREME", None, 10).unwrap().len(), 1);
    }

    #[test]
    fn prefix_matches_final_token() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "deployment pipeline").unwrap();
        // Typing "deploy" should already surface "deployment".
        assert_eq!(search(&conn, "deploy", None, 10).unwrap().len(), 1);
    }

    #[test]
    fn room_filter_scopes_results() {
        let conn = db();
        index_record(&conn, "$e1", "!a:x", "@u:x", "U", 1, "shared token").unwrap();
        index_record(&conn, "$e2", "!b:x", "@u:x", "U", 2, "shared token").unwrap();
        assert_eq!(search(&conn, "shared", None, 10).unwrap().len(), 2);
        let scoped = search(&conn, "shared", Some("!a:x"), 10).unwrap();
        assert_eq!(scoped.len(), 1);
        assert_eq!(scoped[0].room_id, "!a:x");
    }

    #[test]
    fn edit_reindexes_in_place() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "original text").unwrap();
        // Same event id, new body (an m.replace edit).
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "edited content").unwrap();
        assert!(search(&conn, "original", None, 10).unwrap().is_empty());
        assert_eq!(search(&conn, "edited", None, 10).unwrap().len(), 1);
    }

    #[test]
    fn redaction_removes_from_index() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "secret message").unwrap();
        remove_record(&conn, "$e1").unwrap();
        assert!(search(&conn, "secret", None, 10).unwrap().is_empty());
    }

    #[test]
    fn clear_empties_the_index() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "alpha").unwrap();
        index_record(&conn, "$e2", "!r:x", "@a:x", "A", 2, "beta").unwrap();
        clear(&conn).unwrap();
        assert!(search(&conn, "alpha", None, 10).unwrap().is_empty());
        assert!(search(&conn, "beta", None, 10).unwrap().is_empty());
    }

    #[test]
    fn punctuation_query_does_not_error() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "ship it now").unwrap();
        // A query of pure punctuation reduces to no tokens → empty result, no SQL error.
        assert!(search(&conn, "!@#$%", None, 10).unwrap().is_empty());
        // Mixed punctuation + word still matches the word.
        assert_eq!(search(&conn, "ship!!!", None, 10).unwrap().len(), 1);
    }

    #[test]
    fn ranks_then_orders_recent_first() {
        let conn = db();
        // Two equally-ranked single-term matches; the newer one should sort first.
        index_record(&conn, "$old", "!r:x", "@a:x", "A", 100, "release notes").unwrap();
        index_record(&conn, "$new", "!r:x", "@a:x", "A", 200, "release notes").unwrap();
        let hits = search(&conn, "release", None, 10).unwrap();
        assert_eq!(hits.len(), 2);
        assert_eq!(hits[0].event_id, "$new");
    }

    #[test]
    fn empty_query_returns_nothing() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "content").unwrap();
        assert!(search(&conn, "   ", None, 10).unwrap().is_empty());
    }

    #[test]
    fn backfill_marker_defaults_false_and_can_be_set() {
        let conn = db();
        assert!(!is_backfill_complete(&conn));
        mark_backfill_complete(&conn).unwrap();
        assert!(is_backfill_complete(&conn));
    }

    #[test]
    fn stats_counts_messages_rooms_and_oldest() {
        let conn = db();
        assert_eq!(stats(&conn), IndexStats::default());
        index_record(&conn, "$e1", "!a:x", "@u:x", "U", 100, "alpha").unwrap();
        index_record(&conn, "$e2", "!a:x", "@u:x", "U", 300, "beta").unwrap();
        index_record(&conn, "$e3", "!b:x", "@u:x", "U", 200, "gamma").unwrap();
        let s = stats(&conn);
        assert_eq!(s.message_count, 3);
        assert_eq!(s.room_count, 2);
        assert_eq!(s.oldest_ts_ms, 100);
        assert!(!s.backfill_done);
        mark_backfill_complete(&conn).unwrap();
        assert!(stats(&conn).backfill_done);
    }

    #[test]
    fn clear_resets_backfill_marker() {
        let conn = db();
        index_record(&conn, "$e1", "!r:x", "@a:x", "A", 1, "alpha").unwrap();
        mark_backfill_complete(&conn).unwrap();
        assert!(is_backfill_complete(&conn));
        clear(&conn).unwrap();
        // Index emptied AND the marker reset, so a re-enable re-crawls history.
        assert!(search(&conn, "alpha", None, 10).unwrap().is_empty());
        assert!(!is_backfill_complete(&conn));
    }
}
