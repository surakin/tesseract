use parking_lot::Mutex;
use rusqlite::{params, Connection};
use std::path::Path;
use std::sync::OnceLock;

static DB: OnceLock<Mutex<Connection>> = OnceLock::new();
const MAX_ROWS: i64 = 2000;

const SCHEMA: &str = "CREATE TABLE IF NOT EXISTS voice_waveforms (
    mxc_uri   TEXT PRIMARY KEY,
    waveform  BLOB NOT NULL,
    stored_at INTEGER NOT NULL
);";

/// Opens the on-disk cache, falling back to an in-memory one when the file
/// can't be opened or initialised. This is only a cache, so a failure here
/// (unwritable or missing directory, corrupt file) must never take the app
/// down — a panic in the FFI entry point aborts the whole process.
fn open_db(path: &Path) -> Option<Connection> {
    if let Some(parent) = path.parent() {
        if let Err(e) = std::fs::create_dir_all(parent) {
            tracing::warn!("waveforms.db: cannot create {}: {e}", parent.display());
        }
    }
    let on_disk = Connection::open(path)
        .map_err(|e| e.to_string())
        .and_then(|conn| {
            conn.execute_batch(SCHEMA)
                .map(|_| conn)
                .map_err(|e| e.to_string())
        });
    match on_disk {
        Ok(conn) => return Some(conn),
        Err(e) => tracing::warn!(
            "waveforms.db: cannot open {} ({e}); using an in-memory cache",
            path.display()
        ),
    }
    match Connection::open_in_memory().and_then(|conn| conn.execute_batch(SCHEMA).map(|_| conn)) {
        Ok(conn) => Some(conn),
        Err(e) => {
            tracing::warn!("waveforms.db: in-memory fallback failed ({e}); cache disabled");
            None
        }
    }
}

pub fn init(path: &Path) {
    if DB.get().is_some() {
        return;
    }
    if let Some(conn) = open_db(path) {
        // A concurrent init may have won the race; either connection is fine.
        let _ = DB.set(Mutex::new(conn));
    }
}

fn waveform_to_bytes(waveform: &[u16]) -> Vec<u8> {
    waveform.iter().flat_map(|&v| v.to_le_bytes()).collect()
}

fn bytes_to_waveform(bytes: Vec<u8>) -> Vec<u16> {
    bytes
        .chunks_exact(2)
        .map(|b| u16::from_le_bytes([b[0], b[1]]))
        .collect()
}

pub fn load(mxc_uri: &str) -> Vec<u16> {
    let Some(db) = DB.get() else { return vec![] };
    // parking_lot locks do not poison, so a panic by a prior holder leaves the
    // mutex usable. This matters because we are reached through the C++ FFI,
    // where a panic unwinding across the boundary is undefined behavior.
    let db = db.lock();
    db.query_row(
        "SELECT waveform FROM voice_waveforms WHERE mxc_uri = ?1",
        params![mxc_uri],
        |row| row.get::<_, Vec<u8>>(0),
    )
    .map(bytes_to_waveform)
    .unwrap_or_default()
}

pub fn store(mxc_uri: &str, waveform: &[u16]) {
    let Some(db) = DB.get() else { return };
    let bytes = waveform_to_bytes(waveform);
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs() as i64;
    let db = db.lock();
    db.execute(
        "INSERT OR REPLACE INTO voice_waveforms (mxc_uri, waveform, stored_at) \
         VALUES (?1, ?2, ?3)",
        params![mxc_uri, bytes, now],
    )
    .ok();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM voice_waveforms", [], |r| r.get(0))
        .unwrap_or(0);
    if count > MAX_ROWS {
        db.execute(
            "DELETE FROM voice_waveforms WHERE mxc_uri NOT IN \
             (SELECT mxc_uri FROM voice_waveforms ORDER BY stored_at DESC LIMIT ?1)",
            params![MAX_ROWS],
        )
        .ok();
    }
}

pub fn evict(mxc_uri: &str) {
    let Some(db) = DB.get() else { return };
    db.lock()
        .execute(
            "DELETE FROM voice_waveforms WHERE mxc_uri = ?1",
            params![mxc_uri],
        )
        .ok();
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_conn() -> Connection {
        let conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(
            "CREATE TABLE voice_waveforms (
                mxc_uri   TEXT PRIMARY KEY,
                waveform  BLOB NOT NULL,
                stored_at INTEGER NOT NULL
            );",
        )
        .unwrap();
        conn
    }

    fn insert(conn: &Connection, mxc: &str, waveform: &[u16], stored_at: i64) {
        let bytes = waveform_to_bytes(waveform);
        conn.execute(
            "INSERT OR REPLACE INTO voice_waveforms (mxc_uri, waveform, stored_at) \
             VALUES (?1, ?2, ?3)",
            params![mxc, bytes, stored_at],
        )
        .unwrap();
    }

    fn fetch(conn: &Connection, mxc: &str) -> Vec<u16> {
        conn.query_row(
            "SELECT waveform FROM voice_waveforms WHERE mxc_uri = ?1",
            params![mxc],
            |row| row.get::<_, Vec<u8>>(0),
        )
        .map(bytes_to_waveform)
        .unwrap_or_default()
    }

    #[test]
    fn open_db_falls_back_when_path_is_unusable() {
        // A regular file where the parent directory should be: neither
        // create_dir_all nor Connection::open can succeed.
        let dir =
            std::env::temp_dir().join(format!("tesseract-waveform-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let blocker = dir.join("not-a-dir");
        std::fs::write(&blocker, b"x").unwrap();

        let conn = open_db(&blocker.join("waveforms.db")).expect("in-memory fallback");
        insert(&conn, "mxc://example.org/a.ogg", &[1, 2, 3], 1);
        assert_eq!(fetch(&conn, "mxc://example.org/a.ogg"), vec![1, 2, 3]);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn round_trip() {
        let conn = make_conn();
        let waveform: Vec<u16> = vec![0, 256, 512, 1024, 512, 256, 0];
        insert(&conn, "mxc://example.org/voice.ogg", &waveform, 1);
        assert_eq!(fetch(&conn, "mxc://example.org/voice.ogg"), waveform);
    }

    #[test]
    fn eviction_keeps_newest() {
        let conn = make_conn();
        // Insert MAX_ROWS + 1 entries; the oldest (stored_at=1) should be evicted.
        for i in 0i64..=MAX_ROWS {
            let mxc = format!("mxc://example.org/{i}.ogg");
            let waveform = vec![i as u16; 4];
            insert(&conn, &mxc, &waveform, i + 1);
            conn.execute(
                "DELETE FROM voice_waveforms WHERE mxc_uri NOT IN \
                 (SELECT mxc_uri FROM voice_waveforms ORDER BY stored_at DESC LIMIT ?1)",
                params![MAX_ROWS],
            )
            .unwrap();
        }
        // Entry 0 (stored_at=1) should be gone; entry 1 should survive.
        assert!(fetch(&conn, "mxc://example.org/0.ogg").is_empty());
        assert!(!fetch(&conn, "mxc://example.org/1.ogg").is_empty());
    }
}
