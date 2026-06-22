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

use std::cmp::Ordering;
use std::collections::BinaryHeap;

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

// Ordering is over (priority, seq) only — `T` need not be `Ord`. `BinaryHeap`
// is a max-heap, so "greater" = popped first: higher priority is greater, and
// within a priority a *lower* seq is greater (FIFO).
impl<T> Ord for Entry<T> {
    fn cmp(&self, other: &Self) -> Ordering {
        self.priority
            .cmp(&other.priority)
            .then_with(|| other.seq.cmp(&self.seq))
    }
}
impl<T> PartialOrd for Entry<T> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl<T> PartialEq for Entry<T> {
    fn eq(&self, other: &Self) -> bool {
        self.priority == other.priority && self.seq == other.seq
    }
}
impl<T> Eq for Entry<T> {}

pub(super) struct MediaQueue<T> {
    heap: BinaryHeap<Entry<T>>,
}

impl<T> MediaQueue<T> {
    pub(super) fn new() -> Self {
        Self { heap: BinaryHeap::new() }
    }

    pub(super) fn is_empty(&self) -> bool {
        self.heap.is_empty()
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn len(&self) -> usize {
        self.heap.len()
    }

    pub(super) fn push(
        &mut self,
        priority: u8,
        seq: u64,
        request_id: u64,
        group_id: u64,
        payload: T,
    ) {
        self.heap.push(Entry { priority, seq, request_id, group_id, payload });
    }

    /// Remove and return the highest-priority entry, or `None` if empty.
    pub(super) fn pop_highest(&mut self) -> Option<Entry<T>> {
        self.heap.pop()
    }

    /// Raise the priority of every queued entry whose `group_id` matches and
    /// whose `request_id` is in `request_ids` to `new_priority`. Returns the
    /// number of entries actually bumped (already-running/unknown ids are
    /// absent and ignored). No-op when `new_priority` is not higher.
    ///
    /// `BinaryHeap` has no cheap key-change, so this drains to a `Vec`, bumps
    /// matching entries, and rebuilds. The heaps are bounded by the number of
    /// still-pending fetches (tens at most), so the rebuild is negligible.
    pub(super) fn prioritize(
        &mut self,
        group_id: u64,
        request_ids: &[u64],
        new_priority: u8,
    ) -> usize {
        let mut bumped = 0;
        let mut items: Vec<Entry<T>> = self.heap.drain().collect();
        for e in &mut items {
            if e.group_id == group_id
                && e.priority != PRIO_BACKOFF
                && e.priority < new_priority
                && request_ids.contains(&e.request_id)
            {
                e.priority = new_priority;
                bumped += 1;
            }
        }
        self.heap = items.into_iter().collect();
        bumped
    }

    /// Remove and return every queued entry in `group_id` (room-switch cancel).
    pub(super) fn drain_group(&mut self, group_id: u64) -> Vec<Entry<T>> {
        let mut keep: Vec<Entry<T>> = Vec::with_capacity(self.heap.len());
        let mut drained: Vec<Entry<T>> = Vec::new();
        for e in self.heap.drain() {
            if e.group_id == group_id {
                drained.push(e);
            } else {
                keep.push(e);
            }
        }
        self.heap = keep.into_iter().collect();
        drained
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
        assert_eq!(q.pop_highest().unwrap().request_id, 3, "bumped req pops first");
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
        assert_eq!(q.pop_highest().unwrap().request_id, 1, "backoff stays lowest");
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
}
