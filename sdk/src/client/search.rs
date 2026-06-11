//! Local full-text message search index.
//!
//! matrix-sdk ships no search, and the Matrix server-side `/search` endpoint
//! cannot see into encrypted rooms. The only way to search E2EE history is to
//! index the already-decrypted message bodies locally. This module owns a
//! SQLite FTS5 index living in the per-account `app_cache.db` (created in
//! `backfill::open_app_cache_db`).
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
         END;",
    )
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

/// True when the index already holds at least one row. Used to skip the
/// (potentially expensive) history backfill on app launch when the index was
/// already built in a previous session — live indexing alone keeps it current.
pub fn is_populated(conn: &Connection) -> bool {
    conn.query_row("SELECT EXISTS(SELECT 1 FROM message_index)", [], |r| {
        r.get::<_, i64>(0)
    })
    .map(|n| n != 0)
    .unwrap_or(false)
}

/// Drop everything from the index (privacy toggle turned off).
pub fn clear(conn: &Connection) -> rusqlite::Result<()> {
    // Deleting the rows fires the AD trigger per row, keeping the FTS table in
    // sync; an explicit `DELETE FROM message_fts` is therefore unnecessary.
    conn.execute_batch("DELETE FROM message_index;")
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
        let cleaned: String = raw
            .chars()
            .filter(|c| c.is_alphanumeric())
            .collect();
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

/// Resolve a room's display name from the live client, falling back to the raw
/// room id when the client is gone or the room is unknown.
#[cfg(not(test))]
async fn resolve_room_name(client: Option<&matrix_sdk::Client>, room_id: &str) -> String {
    let Some(client) = client else {
        return room_id.to_string();
    };
    let Ok(rid) = room_id.parse::<matrix_sdk::ruma::OwnedRoomId>() else {
        return room_id.to_string();
    };
    match client.get_room(&rid) {
        Some(room) => room
            .display_name()
            .await
            .map(|n| n.to_string())
            .unwrap_or_else(|_| room_id.to_string()),
        None => room_id.to_string(),
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
    use matrix_sdk_ui::timeline::{RoomExt, TimelineItemKind};
    use super::timeline_convert::timeline_item_to_ffi;

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
            db: Arc::clone(&self.app_cache_db),
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
            // Only backfill history the first time the index is built. On later
            // launches the index already holds rows, so live indexing alone
            // keeps it current and we skip the redundant pagination crawl.
            let already_populated = {
                let guard = self.app_cache_db.lock();
                guard.as_ref().map(is_populated).unwrap_or(false)
            };
            if !already_populated {
                self.spawn_index_backfill();
            }
        } else {
            let guard = self.app_cache_db.lock();
            if let Some(conn) = guard.as_ref() {
                let _ = clear(conn);
            }
        }
    }

    /// Spawn a bounded-concurrency pass that indexes the recent history of every
    /// joined room. Shares `warm_semaphore` with the other warm-up tasks so the
    /// combined pagination load stays bounded.
    fn spawn_index_backfill(&self) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let db = Arc::clone(&self.app_cache_db);
        let enabled = Arc::clone(&self.search_indexing_enabled);
        let semaphore = Arc::clone(&self.warm_semaphore);
        let me = client.user_id().map(|u| u.to_owned());

        self.rt.spawn(async move {
            let mut joinset = tokio::task::JoinSet::new();
            for room in client.joined_rooms() {
                if !enabled.load(Ordering::Relaxed) {
                    break;
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
        });
    }

    /// Non-blocking full-text search. Spawns the FTS query on the runtime,
    /// resolves room display names, and fires `on_search_results(request_id, …)`
    /// (or `on_search_failed`) on completion. The C++ controller debounces and
    /// drops stale `request_id`s.
    pub fn search_messages_async(
        &self,
        request_id: u64,
        query: &str,
        room_id: &str,
        limit: u32,
    ) {
        let handler = self.handler.clone();
        let db = Arc::clone(&self.app_cache_db);
        let client = self.client.clone();
        let query = query.to_owned();
        let room_filter = if room_id.is_empty() {
            None
        } else {
            Some(room_id.to_owned())
        };
        let limit = limit.clamp(1, 500);

        self.rt.spawn(async move {
            let raw: Result<Vec<SearchHit>, String> = {
                let guard = db.lock();
                match guard.as_ref() {
                    Some(conn) => search(conn, &query, room_filter.as_deref(), limit)
                        .map_err(|e| e.to_string()),
                    None => Err("search index not open".to_string()),
                }
            };
            let result = match raw {
                Ok(hits) => {
                    let mut name_cache: std::collections::HashMap<String, String> =
                        std::collections::HashMap::new();
                    let mut out: Vec<crate::ffi::SearchHit> = Vec::with_capacity(hits.len());
                    for h in hits {
                        let room_name = match name_cache.get(&h.room_id) {
                            Some(n) => n.clone(),
                            None => {
                                let n = resolve_room_name(client.as_ref(), &h.room_id).await;
                                name_cache.insert(h.room_id.clone(), n.clone());
                                n
                            }
                        };
                        out.push(crate::ffi::SearchHit {
                            event_id: h.event_id,
                            room_id: h.room_id,
                            room_name,
                            sender: h.sender,
                            sender_name: h.sender_name,
                            body: h.body,
                            timestamp_ms: h.timestamp_ms,
                        });
                    }
                    Ok(out)
                }
                Err(e) => Err(e),
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
}
