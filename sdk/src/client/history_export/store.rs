//! `room_export_progress` SQLite table: persists enough state to resume a
//! history export after a crash or network drop without re-walking from
//! the room's newest event. Mirrors the shape of `media_backoff` /
//! `room_summary_backoff` in `client/backfill.rs`, including the
//! un-gated-for-testability pattern — these helpers carry no `ClientFfi`
//! dependency, so the SQL itself is covered by `cargo test` without a live
//! client or C++ toolchain.
//!
//! One row per room: a second export of the same room (fresh or resumed)
//! simply replaces the row, matching the app-wide single-export-at-a-time
//! rule (`HistoryExportController`/`start_room_export_async` both refuse a
//! second concurrent export, so there is never more than one export in
//! flight to race this table).

/// A persisted export checkpoint for one room.
#[derive(Debug, Clone, PartialEq)]
pub(super) struct Checkpoint {
    pub room_id: String,
    pub out_path: String,
    pub format: String,
    /// Oldest event id successfully written so far — feed back as
    /// `resume_from_event_id` to continue from exactly this point.
    pub oldest_event_id: String,
    pub oldest_ts_ms: u64,
    pub events_written: u64,
    pub segments_written: u32,
    pub updated_at_secs: i64,
    /// Time-range cutoff the original export used, Unix ms; 0 = none (all
    /// history). Persisted so a resumed export silently reapplies the same
    /// bound instead of drifting forward from a freshly recomputed "now".
    pub stop_at_ts_ms: u64,
}

/// Adds the `room_export_progress` table to an already-open app-cache
/// connection. Spliced into `backfill::open_app_cache_db`'s batch alongside
/// its other `CREATE TABLE IF NOT EXISTS` statements, rather than
/// duplicated there, so the two can't drift out of sync.
pub(crate) const CREATE_TABLE_SQL: &str = "
    CREATE TABLE IF NOT EXISTS room_export_progress (
        room_id          TEXT    NOT NULL PRIMARY KEY,
        out_path         TEXT    NOT NULL,
        format           TEXT    NOT NULL,
        oldest_event_id  TEXT    NOT NULL,
        oldest_ts_ms     INTEGER NOT NULL,
        events_written   INTEGER NOT NULL,
        segments_written INTEGER NOT NULL,
        updated_at_secs  INTEGER NOT NULL,
        stop_at_ts_ms    INTEGER NOT NULL DEFAULT 0
    );";

/// Migration for DBs created before the time-range cutoff was persisted:
/// adds the `stop_at_ts_ms` column to `room_export_progress` if it isn't
/// already there. Mirrors `search::ensure_thread_root_column`'s
/// check-then-`ALTER TABLE` shape, since `ALTER TABLE ADD COLUMN` errors on
/// a column that already exists.
pub(crate) fn ensure_stop_at_ts_column(conn: &rusqlite::Connection) -> rusqlite::Result<()> {
    let mut stmt = conn.prepare("PRAGMA table_info(room_export_progress)")?;
    let has_column = stmt
        .query_map([], |row| row.get::<_, String>(1))?
        .filter_map(Result::ok)
        .any(|name| name == "stop_at_ts_ms");
    drop(stmt);
    if !has_column {
        conn.execute(
            "ALTER TABLE room_export_progress ADD COLUMN stop_at_ts_ms INTEGER NOT NULL DEFAULT 0",
            [],
        )?;
    }
    Ok(())
}

/// Same 30-day window as `backfill::prune_stale_backoff_and_cache_rows`'s
/// sweep, for consistency — an export nobody has resumed in a month is
/// abandoned, not mid-retry.
pub(super) const PRUNE_STALE_SQL: &str =
    "DELETE FROM room_export_progress WHERE updated_at_secs < strftime('%s','now') - 2592000;";

pub(super) fn query_checkpoint(
    conn: &rusqlite::Connection,
    room_id: &str,
) -> Option<Checkpoint> {
    conn.query_row(
        "SELECT room_id, out_path, format, oldest_event_id, oldest_ts_ms, \
                events_written, segments_written, updated_at_secs, stop_at_ts_ms \
         FROM room_export_progress WHERE room_id = ?1",
        [room_id],
        |row| {
            Ok(Checkpoint {
                room_id: row.get(0)?,
                out_path: row.get(1)?,
                format: row.get(2)?,
                oldest_event_id: row.get(3)?,
                oldest_ts_ms: row.get::<_, i64>(4)? as u64,
                events_written: row.get::<_, i64>(5)? as u64,
                segments_written: row.get::<_, i64>(6)? as u32,
                updated_at_secs: row.get(7)?,
                stop_at_ts_ms: row.get::<_, i64>(8)? as u64,
            })
        },
    )
    .ok()
}

pub(super) fn upsert_checkpoint(conn: &rusqlite::Connection, cp: &Checkpoint) {
    let _ = conn.execute(
        "INSERT OR REPLACE INTO room_export_progress \
         (room_id, out_path, format, oldest_event_id, oldest_ts_ms, \
          events_written, segments_written, updated_at_secs, stop_at_ts_ms) \
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        rusqlite::params![
            cp.room_id,
            cp.out_path,
            cp.format,
            cp.oldest_event_id,
            cp.oldest_ts_ms as i64,
            cp.events_written as i64,
            cp.segments_written as i64,
            cp.updated_at_secs,
            cp.stop_at_ts_ms as i64,
        ],
    );
}

pub(super) fn delete_checkpoint(conn: &rusqlite::Connection, room_id: &str) {
    let _ = conn.execute("DELETE FROM room_export_progress WHERE room_id = ?1", [room_id]);
}

pub(crate) fn prune_stale_checkpoints(conn: &rusqlite::Connection) {
    let _ = conn.execute_batch(PRUNE_STALE_SQL);
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::Connection;

    fn make_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(CREATE_TABLE_SQL).unwrap();
        conn
    }

    fn sample(room_id: &str) -> Checkpoint {
        Checkpoint {
            room_id: room_id.to_string(),
            out_path: "/tmp/out.html".to_string(),
            format: "html".to_string(),
            oldest_event_id: "$abc".to_string(),
            oldest_ts_ms: 1_000,
            events_written: 42,
            segments_written: 1,
            updated_at_secs: 1_700_000_000,
            stop_at_ts_ms: 0,
        }
    }

    #[test]
    fn upsert_and_query_round_trip() {
        let conn = make_conn();
        let cp = sample("!room:example.org");
        upsert_checkpoint(&conn, &cp);
        let loaded = query_checkpoint(&conn, "!room:example.org").unwrap();
        assert_eq!(loaded, cp);
    }

    #[test]
    fn query_missing_room_is_none() {
        let conn = make_conn();
        assert!(query_checkpoint(&conn, "!nope:example.org").is_none());
    }

    #[test]
    fn upsert_replaces_existing_row_for_same_room() {
        let conn = make_conn();
        let mut cp = sample("!room:example.org");
        upsert_checkpoint(&conn, &cp);
        cp.events_written = 999;
        cp.oldest_event_id = "$newer".to_string();
        upsert_checkpoint(&conn, &cp);
        let loaded = query_checkpoint(&conn, "!room:example.org").unwrap();
        assert_eq!(loaded.events_written, 999);
        assert_eq!(loaded.oldest_event_id, "$newer");
    }

    #[test]
    fn delete_removes_checkpoint() {
        let conn = make_conn();
        let cp = sample("!room:example.org");
        upsert_checkpoint(&conn, &cp);
        delete_checkpoint(&conn, "!room:example.org");
        assert!(query_checkpoint(&conn, "!room:example.org").is_none());
    }

    #[test]
    fn delete_nonexistent_is_noop() {
        let conn = make_conn();
        delete_checkpoint(&conn, "!nope:example.org"); // must not panic
    }

    #[test]
    fn prune_removes_only_stale_rows() {
        let conn = make_conn();
        let mut old = sample("!old:example.org");
        old.updated_at_secs = 1; // far past
        upsert_checkpoint(&conn, &old);
        let mut fresh = sample("!fresh:example.org");
        fresh.updated_at_secs = i64::MAX / 2; // far future, always "fresh"
        upsert_checkpoint(&conn, &fresh);

        prune_stale_checkpoints(&conn);

        assert!(query_checkpoint(&conn, "!old:example.org").is_none());
        assert!(query_checkpoint(&conn, "!fresh:example.org").is_some());
    }

    #[test]
    fn independent_rooms_do_not_collide() {
        let conn = make_conn();
        upsert_checkpoint(&conn, &sample("!a:example.org"));
        upsert_checkpoint(&conn, &sample("!b:example.org"));
        delete_checkpoint(&conn, "!a:example.org");
        assert!(query_checkpoint(&conn, "!a:example.org").is_none());
        assert!(query_checkpoint(&conn, "!b:example.org").is_some());
    }

    #[test]
    fn stop_at_ts_ms_round_trips() {
        let conn = make_conn();
        let mut cp = sample("!room:example.org");
        cp.stop_at_ts_ms = 1_650_000_000_000;
        upsert_checkpoint(&conn, &cp);
        let loaded = query_checkpoint(&conn, "!room:example.org").unwrap();
        assert_eq!(loaded.stop_at_ts_ms, 1_650_000_000_000);
    }

    #[test]
    fn ensure_stop_at_ts_column_migrates_a_pre_existing_table() {
        let conn = Connection::open_in_memory().unwrap();
        // Pre-migration schema: no `stop_at_ts_ms` column at all.
        conn.execute_batch(
            "CREATE TABLE room_export_progress (
                room_id          TEXT    NOT NULL PRIMARY KEY,
                out_path         TEXT    NOT NULL,
                format           TEXT    NOT NULL,
                oldest_event_id  TEXT    NOT NULL,
                oldest_ts_ms     INTEGER NOT NULL,
                events_written   INTEGER NOT NULL,
                segments_written INTEGER NOT NULL,
                updated_at_secs  INTEGER NOT NULL
            );",
        )
        .unwrap();
        conn.execute(
            "INSERT INTO room_export_progress \
             (room_id, out_path, format, oldest_event_id, oldest_ts_ms, \
              events_written, segments_written, updated_at_secs) \
             VALUES ('!old:example.org', '/tmp/out.html', 'html', '$abc', 1000, 42, 1, 1700000000)",
            [],
        )
        .unwrap();

        ensure_stop_at_ts_column(&conn).expect("migration must add stop_at_ts_ms without erroring");

        let loaded = query_checkpoint(&conn, "!old:example.org").unwrap();
        assert_eq!(loaded.stop_at_ts_ms, 0);

        // Running it again on an already-migrated table must not error.
        ensure_stop_at_ts_column(&conn).expect("re-running the migration must be a no-op");
    }
}
