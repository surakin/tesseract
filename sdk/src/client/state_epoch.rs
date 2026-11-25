//! Invalidation epochs for the per-room full-state cache: a fetch that was in flight when the room
//! was invalidated (a state event arrived, a local change) must not put its now-stale answer into
//! the cache, or the next read would see the world from before the change.

use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

static EPOCHS: OnceLock<Mutex<HashMap<String, u64>>> = OnceLock::new();

fn epochs() -> &'static Mutex<HashMap<String, u64>> {
    EPOCHS.get_or_init(|| Mutex::new(HashMap::new()))
}

/// The room's invalidation count now; capture it before a fetch.
pub fn current(room_id: &str) -> u64 {
    epochs()
        .lock()
        .map(|m| m.get(room_id).copied().unwrap_or(0))
        .unwrap_or(0)
}

/// Marks the room invalidated: fetches that started earlier must not be cached.
pub fn bump(room_id: &str) {
    if let Ok(mut m) = epochs().lock() {
        *m.entry(room_id.to_string()).or_insert(0) += 1;
    }
}

/// Whether a fetch that captured `epoch` before it started may still be cached.
pub fn still_current(room_id: &str, epoch: u64) -> bool {
    current(room_id) == epoch
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fetch_overtaken_by_an_invalidation_is_not_cacheable() {
        let room = "!epoch-test-a:example.org";
        let before = current(room);
        assert!(still_current(room, before));
        bump(room); // the invalidation arrives while the fetch is on the network
        assert!(!still_current(room, before));
        // A fetch that starts after it is fine again.
        let after = current(room);
        assert!(still_current(room, after));
    }

    #[test]
    fn other_rooms_are_unaffected() {
        let a = "!epoch-test-b:example.org";
        let b = "!epoch-test-c:example.org";
        let e = current(a);
        bump(b);
        assert!(still_current(a, e));
    }
}
