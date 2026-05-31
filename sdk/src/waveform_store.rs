use rusqlite::{Connection, params};
use std::path::Path;
use std::sync::{Mutex, OnceLock};

static DB: OnceLock<Mutex<Connection>> = OnceLock::new();
const MAX_ROWS: i64 = 2000;

pub fn init(path: &Path) {
    DB.get_or_init(|| {
        let conn = Connection::open(path).expect("waveforms.db open");
        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS voice_waveforms (
                mxc_uri   TEXT PRIMARY KEY,
                waveform  BLOB NOT NULL,
                stored_at INTEGER NOT NULL
            );",
        )
        .expect("waveforms schema");
        Mutex::new(conn)
    });
}

fn waveform_to_bytes(waveform: &[u16]) -> Vec<u8> {
    waveform.iter().flat_map(|&v| v.to_le_bytes()).collect()
}

fn bytes_to_waveform(bytes: Vec<u8>) -> Vec<u16> {
    bytes.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect()
}

pub fn load(mxc_uri: &str) -> Vec<u16> {
    let Some(db) = DB.get() else { return vec![] };
    // Recover the guard if a prior holder panicked: this is reached through
    // the C++ FFI, where a panic unwinding across the boundary is undefined
    // behavior. The cached connection is unaffected by an unrelated panic.
    let db = db.lock().unwrap_or_else(|p| p.into_inner());
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
    let db = db.lock().unwrap_or_else(|p| p.into_inner());
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
        .unwrap_or_else(|p| p.into_inner())
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
