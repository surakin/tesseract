//! Async priority gate guarding a bounded media-download lane.
//!
//! Replaces the per-lane `tokio::sync::Semaphore`. A semaphore grants permits
//! strictly FIFO and a parked waiter can never be reordered — so a fetch for a
//! row the user has scrolled to cannot overtake the backlog of off-screen
//! fetches queued ahead of it. This gate instead parks waiters in a
//! [`MediaQueue`] keyed by `(priority, seq)`; when a permit frees it grants the
//! **highest-priority** waiter, and [`PriorityGate::prioritize`] can raise a
//! still-parked waiter so the next freed permit goes to it.
//!
//! Cancellation is unchanged from the semaphore design: the caller still aborts
//! a group's tasks via their `AbortHandle`s. An aborted *waiting* task drops its
//! one-shot receiver; the gate reclaims that slot lazily when `dispatch` next
//! pops the dead entry. [`PriorityGate::cancel_group`] additionally drops parked
//! waiters eagerly so their heap entries clear at once.
//!
//! ## Stall reclamation
//!
//! matrix-sdk media downloads are a single opaque await with no progress hook,
//! so a stuck download would otherwise hold its slot until the outer 30s/60s
//! timeout, freezing the lane — priority can't help because nothing frees a
//! slot. To prevent this, a slot held past [`STALL_DEADLINE`] stops counting
//! against the lane `dynamic_limit`: it is presumed stuck, so the gate grants
//! its effective capacity to the next (highest-priority) waiter while the stuck
//! download keeps draining in the background under its own hard timeout. A hard
//! `ceiling` on total in-flight (held fresh + held stale) bounds how many hung
//! connections a dead homeserver can accumulate. Because no release fires when a
//! slot merely goes stale, each parked waiter re-evaluates on a
//! [`RECHECK_INTERVAL`] tick.
//!
//! ## Adaptive concurrency (AIMD)
//!
//! HTTP/2 multiplexes all streams over one connection, so the lane limit is no
//! longer constrained by TCP connection pressure and can start high. However,
//! an unresponsive homeserver should trigger backoff rather than pile-on.
//! [`PriorityGate`] implements additive-increase / multiplicative-decrease:
//!
//! - **Additive increase** — every slot release with no stalled peers earns one
//!   more fresh slot (up to `max_limit`). This slowly recovers capacity after a
//!   stall episode and lets a healthy lane saturate its ceiling over time.
//! - **Multiplicative decrease** — when more than half of active slots are
//!   stalled at release time the limit is halved (floored at `min_limit`). This
//!   rapidly backs off a lane whose homeserver is struggling.
//! - **Hold steady** — a minority of stalls (≤ 50 %) leaves the limit unchanged;
//!   occasional slow transfers on an otherwise healthy connection don't penalise
//!   the whole lane.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use parking_lot::Mutex;
use tokio::sync::oneshot;
use tokio::time::Instant;

use super::media_queue::MediaQueue;

/// A slot held longer than this is presumed stuck and no longer counts against
/// the lane `limit`, so the queue keeps flowing past it. Comfortably shorter
/// than the outer media-fetch timeouts (30s/60s) yet long enough that a merely
/// slow-but-progressing download is not prematurely treated as stuck.
pub(super) const STALL_DEADLINE: Duration = Duration::from_secs(8);
/// How often a parked waiter re-checks for reclaimed capacity when no slot has
/// been released (a held slot crossing `STALL_DEADLINE` produces no event).
pub(super) const RECHECK_INTERVAL: Duration = Duration::from_secs(1);

struct GateInner {
    /// AIMD upper bound on fresh concurrency. `dynamic_limit` never exceeds this.
    max_limit: usize,
    /// Current effective fresh-slot limit, adjusted at runtime by AIMD. Starts
    /// at `max_limit` (optimistic) and walks between `min_limit` and `max_limit`.
    dynamic_limit: usize,
    /// AIMD floor: `(max_limit / 8).max(1)`. Prevents the limit from collapsing
    /// to zero under sustained stall pressure while scaling with lane size.
    min_limit: usize,
    /// Hard cap on total in-flight (fresh + stalled) so a mass stall can't
    /// accumulate unbounded hung downloads.
    ceiling: usize,
    /// Monotonic enqueue counter, for FIFO ordering within a priority.
    seq: u64,
    /// Monotonic permit id, the key into `active`.
    next_permit_id: u64,
    /// Held slots: permit id → the instant it was granted. A permit older than
    /// `STALL_DEADLINE` is "stale" and excluded from the `limit` count.
    active: HashMap<u64, Instant>,
    /// Parked waiters; payload is the one-shot that delivers the granted id.
    queue: MediaQueue<oneshot::Sender<u64>>,
    /// True while a single gate-owned recheck task is running. A held slot
    /// crossing `STALL_DEADLINE` fires no event, so reclamation needs a timer —
    /// but one per gate (driving `dispatch` for all parked waiters), not one per
    /// waiter. The task is spawned when the first waiter parks and exits once the
    /// queue drains.
    recheck_running: bool,
}

impl GateInner {
    /// Count of held slots not yet past the stall deadline.
    fn fresh(&self, now: Instant) -> usize {
        self.active
            .values()
            .filter(|&&t| now.saturating_duration_since(t) < STALL_DEADLINE)
            .count()
    }

    /// Count of held slots past the stall deadline.
    fn stale(&self, now: Instant) -> usize {
        self.active.len() - self.fresh(now)
    }

    /// Capacity to start one more download: room under `dynamic_limit` (stale
    /// slots don't count) and under the hard `ceiling` on total in-flight.
    fn can_grant(&self, now: Instant) -> bool {
        self.fresh(now) < self.dynamic_limit && self.active.len() < self.ceiling
    }

    /// Record a freshly granted slot and return its id.
    fn admit(&mut self, now: Instant) -> u64 {
        let id = self.next_permit_id;
        self.next_permit_id = self.next_permit_id.wrapping_add(1);
        self.active.insert(id, now);
        id
    }

    /// Grant slots to the highest-priority waiters while capacity allows.
    fn dispatch(&mut self, now: Instant) {
        while self.can_grant(now) {
            match self.queue.pop_highest() {
                Some(entry) => {
                    let id = self.admit(now);
                    // A dead receiver means the waiter was aborted; take its slot
                    // back and try the next entry.
                    if entry.payload.send(id).is_err() {
                        self.active.remove(&id);
                    }
                }
                None => break,
            }
        }
    }
}

pub(crate) struct PriorityGate {
    inner: Mutex<GateInner>,
}

/// RAII handle for a held download slot. Returning the slot on drop (including
/// when a downloading task is aborted) lets the gate grant the next waiter.
pub(super) struct GatePermit {
    gate: Arc<PriorityGate>,
    id: u64,
}

impl PriorityGate {
    /// `max_limit` = AIMD upper bound on fresh concurrency; `ceiling` = hard cap
    /// on total in-flight including presumed-stuck downloads (`ceiling >=
    /// max_limit`). The gate starts at `max_limit` (optimistic) and adjusts
    /// downward under stall pressure, recovering upward on clean releases.
    pub(super) fn new(max_limit: usize, ceiling: usize) -> Arc<Self> {
        let min_limit = (max_limit / 8).max(1);
        Arc::new(Self {
            inner: Mutex::new(GateInner {
                max_limit,
                dynamic_limit: max_limit,
                min_limit,
                ceiling: ceiling.max(max_limit),
                seq: 0,
                next_permit_id: 0,
                active: HashMap::new(),
                queue: MediaQueue::new(),
                recheck_running: false,
            }),
        })
    }

    /// Acquire a slot, parking at `priority` until one is free. Returns `None`
    /// if the wait is cancelled (the gate dropped this waiter via
    /// `cancel_group`), in which case the caller should deliver empty bytes.
    pub(super) async fn acquire(
        self: &Arc<Self>,
        priority: u8,
        request_id: u64,
        group_id: u64,
    ) -> Option<GatePermit> {
        let rx = {
            let mut inner = self.inner.lock();
            let now = Instant::now();
            // Fast path: capacity is available and no one is ahead of us in line.
            if inner.queue.is_empty() && inner.can_grant(now) {
                let id = inner.admit(now);
                return Some(GatePermit { gate: self.clone(), id });
            }
            let seq = inner.seq;
            inner.seq = inner.seq.wrapping_add(1);
            let (tx, rx) = oneshot::channel();
            inner.queue.push(priority, seq, request_id, group_id, tx);
            // A held slot can cross STALL_DEADLINE with no release to wake us, so
            // a timer must re-run dispatch. One gate-owned task drives that for
            // all waiters (not one timer per waiter); spawned now if not already.
            if !inner.recheck_running {
                inner.recheck_running = true;
                self.spawn_recheck_task();
            }
            rx
        };
        // Park until granted (rx delivers our slot id) or cancelled (rx errors).
        match rx.await {
            Ok(id) => Some(GatePermit { gate: self.clone(), id }),
            Err(_) => None,
        }
    }

    /// Spawn the single recheck task that periodically re-runs `dispatch` so a
    /// slot that silently goes stale is reclaimed for a parked waiter. It exits
    /// once no waiters remain (clearing `recheck_running` so the next parking
    /// waiter re-spawns it). Must be called from within the tokio runtime — the
    /// only caller, `acquire`, always runs inside a spawned fetch task.
    fn spawn_recheck_task(self: &Arc<Self>) {
        let gate = self.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(RECHECK_INTERVAL).await;
                let mut inner = gate.inner.lock();
                inner.dispatch(Instant::now());
                if inner.queue.is_empty() {
                    inner.recheck_running = false;
                    return;
                }
            }
        });
    }

    /// Raise the priority of still-parked waiters in `group_id` whose
    /// `request_id` is listed, so the next granted slot goes to them first.
    /// Reordering alone is enough — the next `release` or recheck tick dispatches
    /// in the new order.
    pub(super) fn prioritize(&self, group_id: u64, request_ids: &[u64], new_priority: u8) {
        let mut inner = self.inner.lock();
        inner.queue.prioritize(group_id, request_ids, new_priority);
    }

    /// Drop every parked waiter in `group_id`; each one's `acquire` returns
    /// `None`. Slots held by already-running tasks are untouched (the caller
    /// aborts those separately).
    pub(super) fn cancel_group(&self, group_id: u64) {
        let mut inner = self.inner.lock();
        let _dropped = inner.queue.drain_group(group_id); // senders drop → waiters wake with Err
    }

    /// Number of parked waiters (test/diagnostics).
    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn pending_len(&self) -> usize {
        self.inner.lock().queue.len()
    }

    /// Number of held slots, fresh and stale (test/diagnostics).
    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn active_len(&self) -> usize {
        self.inner.lock().active.len()
    }

    /// Number of held slots past the stall deadline (test/diagnostics).
    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn stale_len(&self) -> usize {
        let inner = self.inner.lock();
        inner.stale(Instant::now())
    }

    /// Current adaptive concurrency limit (test/diagnostics).
    #[cfg_attr(not(test), allow(dead_code))]
    pub(super) fn dynamic_limit(&self) -> usize {
        self.inner.lock().dynamic_limit
    }

    /// Release a held slot, apply one AIMD step, then grant capacity to the
    /// highest-priority waiter.
    ///
    /// AIMD rules (evaluated after removing the released slot):
    /// - **AI**: no stalls visible → increment `dynamic_limit` by 1 (up to
    ///   `max_limit`). Covers both "all slots idle" and "busy but fully fresh".
    /// - **MD**: stalled > 50 % of remaining active slots → halve
    ///   `dynamic_limit` (down to `min_limit`). Backs off quickly when the
    ///   homeserver is struggling.
    /// - **Hold**: stalls present but ≤ 50 % → leave `dynamic_limit` unchanged.
    fn release(&self, id: u64) {
        let mut inner = self.inner.lock();
        inner.active.remove(&id);
        let now = Instant::now();
        let stale = inner.stale(now);
        let total = inner.active.len();
        if total == 0 || stale == 0 {
            // AI: earn one more slot when there is nothing stalled.
            inner.dynamic_limit = (inner.dynamic_limit + 1).min(inner.max_limit);
        } else if stale * 2 > total {
            // MD: more than half stalled — back off quickly.
            inner.dynamic_limit = (inner.dynamic_limit / 2).max(inner.min_limit);
        }
        // else: minority of stalls — hold steady.
        inner.dispatch(now);
    }
}

impl Drop for GatePermit {
    fn drop(&mut self) {
        self.gate.release(self.id);
    }
}

#[cfg(test)]
mod tests {
    use super::{PriorityGate, RECHECK_INTERVAL, STALL_DEADLINE};
    use super::super::media_queue::{PRIO_NORMAL, PRIO_VISIBLE};
    use std::time::Duration;
    use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};
    use std::sync::Arc;

    #[tokio::test]
    async fn release_grants_highest_priority_waiter_first() {
        let gate = PriorityGate::new(1, 3);
        let p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap(); // hold the only slot

        let order = Arc::new(parking_lot::Mutex::new(Vec::<u64>::new()));

        let g1 = gate.clone();
        let o1 = order.clone();
        let h_norm = tokio::spawn(async move {
            let _p = g1.acquire(PRIO_NORMAL, 1, 1).await.unwrap();
            o1.lock().push(1);
        });
        let g2 = gate.clone();
        let o2 = order.clone();
        let h_vis = tokio::spawn(async move {
            let _p = g2.acquire(PRIO_VISIBLE, 2, 1).await.unwrap();
            o2.lock().push(2);
        });

        // Wait until both tasks have parked in the gate.
        while gate.pending_len() < 2 {
            tokio::task::yield_now().await;
        }
        drop(p0); // frees one slot → visible (2) granted before normal (1)

        h_vis.await.unwrap();
        h_norm.await.unwrap();
        assert_eq!(*order.lock(), vec![2, 1]);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn never_exceeds_limit_when_fresh_and_never_leaks() {
        let gate = PriorityGate::new(2, 4);
        let concurrent = Arc::new(AtomicUsize::new(0));
        let max_seen = Arc::new(AtomicUsize::new(0));

        let mut handles = Vec::new();
        for i in 0..50u64 {
            let g = gate.clone();
            let c = concurrent.clone();
            let m = max_seen.clone();
            handles.push(tokio::spawn(async move {
                let _permit = g.acquire(PRIO_NORMAL, i, 1).await.unwrap();
                let now = c.fetch_add(1, SeqCst) + 1;
                m.fetch_max(now, SeqCst);
                tokio::task::yield_now().await;
                c.fetch_sub(1, SeqCst);
            }));
        }
        for h in handles {
            h.await.unwrap();
        }
        // Every download completes far faster than STALL_DEADLINE, so all held
        // slots stay fresh and concurrency never exceeds the base limit.
        assert!(max_seen.load(SeqCst) <= 2, "never more than 2 fresh slots in use");
        assert_eq!(concurrent.load(SeqCst), 0);
        // No slot leaked: fresh acquires still succeed immediately.
        let _a = gate.acquire(PRIO_NORMAL, 100, 1).await.unwrap();
        let _b = gate.acquire(PRIO_NORMAL, 101, 1).await.unwrap();
    }

    #[tokio::test]
    async fn cancel_group_wakes_parked_waiter_with_none() {
        let gate = PriorityGate::new(1, 3);
        let _p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap(); // hold the slot

        let g = gate.clone();
        let h = tokio::spawn(async move { g.acquire(PRIO_NORMAL, 1, 7).await });
        while gate.pending_len() < 1 {
            tokio::task::yield_now().await;
        }
        gate.cancel_group(7);
        assert!(h.await.unwrap().is_none(), "cancelled waiter returns None");
    }

    // A slot held past STALL_DEADLINE stops counting against the limit, so a
    // parked waiter is admitted even though the stuck download never released.
    #[tokio::test(start_paused = true)]
    async fn stale_slot_lets_a_waiter_through_past_the_limit() {
        let gate = PriorityGate::new(1, 3);
        let _p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap(); // held, never released

        let g = gate.clone();
        let h = tokio::spawn(async move { g.acquire(PRIO_VISIBLE, 1, 1).await });

        // Awaiting the parked waiter lets paused time auto-advance through the
        // recheck ticks until p0 crosses the stall deadline and is demoted.
        let permit = h.await.unwrap();
        assert!(permit.is_some(), "waiter admitted on the reclaimed stale slot");
        assert_eq!(gate.active_len(), 2, "stale p0 + the newly admitted waiter");
    }

    // While every held slot is still fresh, extra waiters stay parked — the
    // reclamation does not loosen the limit for healthy downloads.
    #[tokio::test(start_paused = true)]
    async fn fresh_downloads_stay_bounded_by_limit() {
        let gate = PriorityGate::new(2, 4);
        let _p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap();
        let _p1 = gate.acquire(PRIO_NORMAL, 1, 1).await.unwrap();

        let g = gate.clone();
        let h = tokio::spawn(async move { g.acquire(PRIO_NORMAL, 2, 1).await });
        while gate.pending_len() < 1 {
            tokio::task::yield_now().await; // park the third without advancing time
        }
        // No virtual time has elapsed → both held slots are fresh → third parked.
        assert_eq!(gate.active_len(), 2);
        assert_eq!(gate.pending_len(), 1);
        drop(h); // detach the still-parked waiter
    }

    // Releasing a slot when >50% of remaining active slots are stalled halves
    // dynamic_limit (MD step). Releasing the last stale slot (total == 0 after
    // removal) triggers AI instead, incrementing the limit by 1.
    #[tokio::test(start_paused = true)]
    async fn aimd_decreases_limit_under_mass_stall() {
        // max_limit=4, min_limit = (4/8).max(1) = 1
        let gate = PriorityGate::new(4, 8);
        assert_eq!(gate.dynamic_limit(), 4);

        let p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap();
        let p1 = gate.acquire(PRIO_NORMAL, 1, 1).await.unwrap();
        let p2 = gate.acquire(PRIO_NORMAL, 2, 1).await.unwrap();
        let p3 = gate.acquire(PRIO_NORMAL, 3, 1).await.unwrap();

        // All 4 slots go stale.
        tokio::time::advance(STALL_DEADLINE + Duration::from_millis(1)).await;

        // Drop p0: after removal total=3, stale=3 → stale*2=6 > 3 → MD: 4→2.
        drop(p0);
        assert_eq!(gate.dynamic_limit(), 2, "MD halves limit when >50% stalled");

        drop(p1); drop(p2); drop(p3);
    }

    // After MD has lowered dynamic_limit, releasing a slot with no remaining
    // stalls (total == 0) triggers AI and increments the limit by 1.
    #[tokio::test(start_paused = true)]
    async fn aimd_increases_limit_on_clean_release() {
        // max_limit=4, min_limit=1
        let gate = PriorityGate::new(4, 8);

        // Bring dynamic_limit below max via MD: hold 2 slots, let them go stale,
        // release one to trigger MD (stale=1/total=1 → 100% stalled).
        let p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap();
        let p1 = gate.acquire(PRIO_NORMAL, 1, 1).await.unwrap();
        tokio::time::advance(STALL_DEADLINE + Duration::from_millis(1)).await;
        drop(p0); // total=1, stale=1 → MD: 4→2
        assert_eq!(gate.dynamic_limit(), 2);

        // Now drop p1: total becomes 0 after removal → AI: 2→3.
        drop(p1);
        assert_eq!(gate.dynamic_limit(), 3, "AI increments limit when total==0");
    }

    // Once total in-flight reaches the ceiling, no further download starts even
    // when every held slot is stale — bounding hung connections under a stall.
    #[tokio::test(start_paused = true)]
    async fn ceiling_caps_total_inflight_under_mass_stall() {
        let gate = PriorityGate::new(1, 2);
        let _p0 = gate.acquire(PRIO_NORMAL, 0, 1).await.unwrap(); // held forever

        // p0 goes stale → this second download is admitted (active 1 < ceiling 2).
        let g1 = gate.clone();
        let h1 = tokio::spawn(async move { g1.acquire(PRIO_NORMAL, 1, 1).await });
        let _p1 = h1.await.unwrap().expect("admitted on p0's reclaimed slot");
        assert_eq!(gate.active_len(), 2, "p0 (stale) + p1 == ceiling");

        // A third waiter cannot start: active == ceiling, even as p0/p1 go stale.
        let g2 = gate.clone();
        let h2 = tokio::spawn(async move { g2.acquire(PRIO_NORMAL, 2, 1).await });
        while gate.pending_len() < 1 {
            tokio::task::yield_now().await;
        }
        tokio::time::advance(STALL_DEADLINE * 3 + RECHECK_INTERVAL).await;
        tokio::task::yield_now().await;
        assert_eq!(gate.pending_len(), 1, "ceiling blocks a 3rd concurrent download");
        assert_eq!(gate.active_len(), 2);
        drop(h2);
    }
}
