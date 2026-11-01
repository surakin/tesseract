//! Pure, dependency-free scheduling core for the async media download lanes.
//!
//! The live fetch path in `media.rs` is `#[cfg(not(test))]` and a `ClientFfi`
//! cannot be constructed in unit tests (no live `matrix_sdk` client), so the
//! orderable data structure lives here with **no `matrix_sdk` / runtime
//! dependency** and is unit-tested directly.
//!
//! A `MediaQueue<T>` is a max-priority queue of pending download requests. .
//! `T` is an opaque per-entry payload (the wake-up channel in the live gate; a
//! sentinel in tests) and never participates in ordering. Ordering is:
//!   1. higher `priority` first (Visible before Normal), then
//!   2. lower `seq` first (FIFO within a priority — oldest enqueued wins).

use std::cmp::Reverse;
use std::collections::{BTreeMap, HashMap};

/// Lowest priority: a retry for a URL that has previously failed and is in
/// exponential backoff. Never raised by `prioritize` — stays below normal
/// prefetch even when the row is visible. Mirrors `MediaPriority::Backoff`.
pub(super) const PRIO_BACKOFF: u8 = 0;
/// Background priority for the eager whole-timeline prefetch.
pub(super) const PRIO_NORMAL: u8 = 1;
/// Priority for media backing a currently-visible row. Mirrors the C++
/// `tesseract::MediaPriority::Visible`.
pub(super) const PRIO_VISIBLE: u8 = 2;

/// One queued download request plus its opaque wake-up payload.
pub(super) struct Entry<T> {
    pub priority: u8,
    pub seq: u64,
    pub request_id: u64,
    pub group_id: u64,
    pub payload: T,
}

/// Position of a queued entry. The map is ascending, so its last key is the
/// next grant: highest priority, then lowest `seq` (`Reverse` flips the order).
type OrderKey = (u8, Reverse<u64>);

/// Entries live in a `BTreeMap` ordered by [`OrderKey`], so a grant is
/// `O(log n)` and `prioritize` moves only the entries it bumps — a
/// whole-timeline prefetch can park thousands of waiters, and a visible-range
/// change must not rebuild all of them.
pub(super) struct MediaQueue<T> {
    order: BTreeMap<OrderKey, Entry<T>>,
    /// `(group_id, request_id)` → keys of the queued entries for that request.
    by_request: HashMap<(u64, u64), Vec<OrderKey>>,
}

impl<T> MediaQueue<T> {
    pub(super) fn new() -> Self {
        Self {
            order: BTreeMap::new(),
            by_request: HashMap::new(),
        }
    }

    pub(super) fn is_empty(&self) -> bool {
        self.order.is_empty()
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn len(&self) -> usize {
        self.order.len()
    }

    pub(super) fn push(
        &mut self,
        priority: u8,
        seq: u64,
        request_id: u64,
        group_id: u64,
        payload: T,
    ) {
        let key = (priority, Reverse(seq));
        self.by_request
            .entry((group_id, request_id))
            .or_default()
            .push(key);
        self.order.insert(
            key,
            Entry {
                priority,
                seq,
                request_id,
                group_id,
                payload,
            },
        );
    }

    /// Remove and return the highest-priority entry, or `None` if empty.
    pub(super) fn pop_highest(&mut self) -> Option<Entry<T>> {
        let (key, entry) = self.order.pop_last()?;
        self.unindex(&key, entry.group_id, entry.request_id);
        Some(entry)
    }

    /// Raise the priority of every queued entry whose `group_id` matches and
    /// whose `request_id` is in `request_ids` to `new_priority`. Returns the
    /// number of entries actually bumped (already-running/unknown ids are
    /// absent and ignored). No-op when `new_priority` is not higher.
    ///
    /// Costs `O(k log n)` for `k` listed ids — only the bumped entries move.
    pub(super) fn prioritize(
        &mut self,
        group_id: u64,
        request_ids: &[u64],
        new_priority: u8,
    ) -> usize {
        let mut bumped = 0;
        for &request_id in request_ids {
            let Some(keys) = self.by_request.get(&(group_id, request_id)).cloned() else {
                continue;
            };
            for key in keys {
                let raisable = self
                    .order
                    .get(&key)
                    .is_some_and(|e| e.priority != PRIO_BACKOFF && e.priority < new_priority);
                if !raisable {
                    continue;
                }
                let Some(mut entry) = self.order.remove(&key) else {
                    continue;
                };
                entry.priority = new_priority;
                let new_key = (new_priority, Reverse(entry.seq));
                if let Some(keys) = self.by_request.get_mut(&(group_id, request_id)) {
                    for k in keys.iter_mut().filter(|k| **k == key) {
                        *k = new_key;
                    }
                }
                self.order.insert(new_key, entry);
                bumped += 1;
            }
        }
        bumped
    }

    /// Remove and return every queued entry in `group_id` (room-switch cancel).
    pub(super) fn drain_group(&mut self, group_id: u64) -> Vec<Entry<T>> {
        let keys: Vec<OrderKey> = self
            .order
            .iter()
            .filter(|(_, e)| e.group_id == group_id)
            .map(|(key, _)| *key)
            .collect();
        let mut drained = Vec::with_capacity(keys.len());
        for key in keys {
            if let Some(entry) = self.order.remove(&key) {
                self.unindex(&key, entry.group_id, entry.request_id);
                drained.push(entry);
            }
        }
        drained
    }

    /// Forget `key` for the request it belongs to, dropping the request's
    /// index slot once it has no queued entries left.
    fn unindex(&mut self, key: &OrderKey, group_id: u64, request_id: u64) {
        if let Some(keys) = self.by_request.get_mut(&(group_id, request_id)) {
            keys.retain(|k| k != key);
            if keys.is_empty() {
                self.by_request.remove(&(group_id, request_id));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{MediaQueue, PRIO_BACKOFF, PRIO_NORMAL, PRIO_VISIBLE};

    // Payload sentinel = request_id, so tests can assert on what was popped.
    fn q() -> MediaQueue<u64> {
        MediaQueue::new()
    }

    #[test]
    fn pop_returns_highest_priority_first() {
        let mut q = q();
        q.push(PRIO_NORMAL, 0, 1, 1, 1);
        q.push(PRIO_VISIBLE, 1, 2, 1, 2);
        q.push(PRIO_NORMAL, 2, 3, 1, 3);
        assert_eq!(q.pop_highest().unwrap().request_id, 2, "visible pops first");
        assert_eq!(q.pop_highest().unwrap().request_id, 1, "then oldest normal");
        assert_eq!(q.pop_highest().unwrap().request_id, 3);
        assert!(q.pop_highest().is_none());
    }

    #[test]
    fn equal_priority_pops_in_fifo_seq_order() {
        let mut q = q();
        q.push(PRIO_NORMAL, 5, 10, 1, 10);
        q.push(PRIO_NORMAL, 6, 11, 1, 11);
        q.push(PRIO_NORMAL, 7, 12, 1, 12);
        assert_eq!(q.pop_highest().unwrap().request_id, 10);
        assert_eq!(q.pop_highest().unwrap().request_id, 11);
        assert_eq!(q.pop_highest().unwrap().request_id, 12);
    }

    #[test]
    fn prioritize_bumps_matching_request_ids_above_normal() {
        let mut q = q();
        q.push(PRIO_NORMAL, 0, 1, 5, 1);
        q.push(PRIO_NORMAL, 1, 2, 5, 2);
        q.push(PRIO_NORMAL, 2, 3, 5, 3);
        let bumped = q.prioritize(5, &[3], PRIO_VISIBLE);
        assert_eq!(bumped, 1);
        assert_eq!(
            q.pop_highest().unwrap().request_id,
            3,
            "bumped req pops first"
        );
        // Remaining keep FIFO order.
        assert_eq!(q.pop_highest().unwrap().request_id, 1);
        assert_eq!(q.pop_highest().unwrap().request_id, 2);
    }

    #[test]
    fn prioritize_ignores_unknown_or_running_ids() {
        let mut q = q();
        q.push(PRIO_NORMAL, 0, 1, 5, 1);
        q.push(PRIO_NORMAL, 1, 2, 5, 2);
        let bumped = q.prioritize(5, &[999], PRIO_VISIBLE);
        assert_eq!(bumped, 0, "unknown id bumps nothing");
        assert_eq!(q.pop_highest().unwrap().request_id, 1, "order unchanged");
        assert_eq!(q.pop_highest().unwrap().request_id, 2);
    }

    #[test]
    fn prioritize_scopes_to_group_id() {
        let mut q = q();
        q.push(PRIO_NORMAL, 0, 1, 5, 1);
        q.push(PRIO_NORMAL, 1, 2, 6, 2); // different group
        let bumped = q.prioritize(5, &[2], PRIO_VISIBLE);
        assert_eq!(bumped, 0, "request 2 is in group 6, not 5");
        assert_eq!(q.pop_highest().unwrap().request_id, 1);
        assert_eq!(q.pop_highest().unwrap().request_id, 2);
    }

    #[test]
    fn prioritize_never_raises_backoff_entries() {
        let mut q = q();
        q.push(PRIO_BACKOFF, 0, 1, 5, 1);
        q.push(PRIO_NORMAL, 1, 2, 5, 2);
        let bumped = q.prioritize(5, &[1, 2], PRIO_VISIBLE);
        assert_eq!(bumped, 1, "only the normal entry is bumped");
        assert_eq!(q.pop_highest().unwrap().request_id, 2, "visible pops first");
        assert_eq!(
            q.pop_highest().unwrap().request_id,
            1,
            "backoff stays lowest"
        );
    }

    #[test]
    fn drain_group_removes_only_matching_group() {
        let mut q = q();
        q.push(PRIO_NORMAL, 0, 1, 5, 1);
        q.push(PRIO_NORMAL, 1, 2, 6, 2);
        q.push(PRIO_NORMAL, 2, 3, 5, 3);
        let drained = q.drain_group(5);
        let mut ids: Vec<u64> = drained.iter().map(|e| e.request_id).collect();
        ids.sort();
        assert_eq!(ids, vec![1, 3]);
        assert_eq!(q.len(), 1, "group 6 entry remains");
        assert_eq!(q.pop_highest().unwrap().request_id, 2);
    }

    #[test]
    fn index_stays_in_step_across_bumps_pops_and_drains() {
        let mut q = q();
        // Two entries share one request id (same group), one is bumped twice.
        q.push(PRIO_NORMAL, 0, 7, 5, 1);
        q.push(PRIO_NORMAL, 1, 7, 5, 2);
        q.push(PRIO_NORMAL, 2, 8, 5, 3);
        assert_eq!(q.prioritize(5, &[7, 7], PRIO_VISIBLE), 2);
        assert_eq!(q.prioritize(5, &[7], PRIO_VISIBLE), 0, "already visible");
        assert_eq!(q.pop_highest().unwrap().request_id, 7);
        assert_eq!(q.pop_highest().unwrap().request_id, 7);
        assert_eq!(q.pop_highest().unwrap().request_id, 8);
        assert!(q.by_request.is_empty(), "no stale index slots after pops");

        q.push(PRIO_NORMAL, 3, 9, 5, 4);
        q.push(PRIO_NORMAL, 4, 9, 6, 5);
        assert_eq!(q.drain_group(5).len(), 1);
        assert_eq!(q.by_request.len(), 1, "group 6's slot is untouched");
        assert_eq!(q.drain_group(6).len(), 1);
        assert!(q.by_request.is_empty() && q.is_empty());
    }
}
