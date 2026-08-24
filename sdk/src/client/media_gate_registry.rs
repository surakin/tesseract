//! Per-origin registry wrapping `PriorityGate` (`media_gate.rs`) so that a
//! dead or flaky origin's AIMD collapse only ever throttles requests to that
//! same origin, never unrelated ones sharing the lane.
//!
//! Each origin (see `media_origin.rs`) gets its own, otherwise-unmodified
//! `PriorityGate` — same AIMD/stall/ceiling mechanics as before, just scoped
//! — created lazily the first time that origin is seen and reaped once it's
//! been idle for a while so ephemeral origins (one-off URL-preview domains, a
//! rarely-touched federated server) don't accumulate state forever. A small
//! lane-wide semaphore sits on top as a coarse aggregate cap, so a burst of
//! many simultaneously-busy origins still can't accumulate unbounded
//! connections/memory — the common case of one (or a couple of) active
//! homeserver(s) never contends it and behaves identically to today's single
//! shared gate.

use std::collections::HashMap;
use std::sync::Arc;

use parking_lot::Mutex;
use tokio::sync::{OwnedSemaphorePermit, Semaphore};
use tokio::time::Instant;

use super::media_gate::{GatePermit, PriorityGate};
use super::media_origin::OriginKey;

/// How long an origin may sit with no active or parked requests before its
/// gate (and its accumulated AIMD state) is reaped. Comfortably longer than
/// typical gaps between requests to a room being actively viewed.
const IDLE_EVICT_AFTER: std::time::Duration = std::time::Duration::from_secs(120);
/// How often the reaper sweeps for idle origins. A registry-level timer
/// separate from each gate's own `RECHECK_INTERVAL` tick, since it operates
/// across every origin's gate at once.
const REAP_INTERVAL: std::time::Duration = std::time::Duration::from_secs(30);

struct OriginEntry {
    gate: Arc<PriorityGate>,
    last_seen: Instant,
}

struct RegistryInner {
    origins: HashMap<OriginKey, OriginEntry>,
    /// True while a single registry-owned reaper task is running. Spawned
    /// when the first origin is created, exits once no origins remain
    /// (mirrors `PriorityGate::spawn_recheck_task`'s lazy-respawn pattern).
    reaper_running: bool,
}

pub(crate) struct GateRegistry {
    inner: Mutex<RegistryInner>,
    /// Passed through unchanged to every per-origin `PriorityGate::new` — the
    /// common case of one (or a couple of) active homeserver(s) behaves
    /// identically to today's single shared gate.
    origin_max_limit: usize,
    origin_ceiling: usize,
    /// Lane-wide aggregate cap across all origins combined.
    global: Arc<Semaphore>,
}

/// RAII handle bundling one origin's `PriorityGate` permit with one slot of
/// the lane-wide aggregate semaphore. Dropping releases both.
pub(super) struct GatedPermit {
    _origin: GatePermit,
    _global: OwnedSemaphorePermit,
}

impl GateRegistry {
    pub(super) fn new(origin_max_limit: usize, origin_ceiling: usize, global_permits: usize) -> Arc<Self> {
        Arc::new(Self {
            inner: Mutex::new(RegistryInner {
                origins: HashMap::new(),
                reaper_running: false,
            }),
            origin_max_limit,
            origin_ceiling,
            global: Arc::new(Semaphore::new(global_permits)),
        })
    }

    /// Get-or-create the gate for `origin`, refreshing its last-seen stamp on
    /// every lookup (not just on grant) so the reaper never evicts an origin
    /// that still has traffic merely parked, not yet admitted.
    fn gate_for(self: &Arc<Self>, origin: &OriginKey) -> Arc<PriorityGate> {
        let mut inner = self.inner.lock();
        let now = Instant::now();
        if let Some(entry) = inner.origins.get_mut(origin) {
            entry.last_seen = now;
            return Arc::clone(&entry.gate);
        }
        let gate = PriorityGate::new(self.origin_max_limit, self.origin_ceiling);
        inner.origins.insert(
            origin.clone(),
            OriginEntry {
                gate: Arc::clone(&gate),
                last_seen: now,
            },
        );
        if !inner.reaper_running {
            inner.reaper_running = true;
            self.spawn_reaper();
        }
        gate
    }

    /// Acquire a slot for `origin`: parks on that origin's own priority queue
    /// until one of its slots frees, then on the lane-wide aggregate
    /// semaphore. Returns `None` if the origin-level wait was cancelled
    /// (room switch) — the caller should deliver empty bytes, same as today.
    pub(super) async fn acquire(
        self: &Arc<Self>,
        origin: &OriginKey,
        priority: u8,
        request_id: u64,
        group_id: u64,
    ) -> Option<GatedPermit> {
        let gate = self.gate_for(origin);
        let origin_permit = gate.acquire(priority, request_id, group_id).await?;
        let global_permit = Arc::clone(&self.global).acquire_owned().await.ok()?;
        Some(GatedPermit {
            _origin: origin_permit,
            _global: global_permit,
        })
    }

    /// Fan out to every currently-known origin's gate — a room's members can
    /// span multiple homeservers under one `group_id`, which carries no
    /// origin info of its own, so every origin must be asked (each ignores
    /// ids it doesn't hold, exactly as before).
    pub(super) fn prioritize(&self, group_id: u64, request_ids: &[u64], new_priority: u8) {
        let inner = self.inner.lock();
        for entry in inner.origins.values() {
            entry.gate.prioritize(group_id, request_ids, new_priority);
        }
    }

    /// Fan out `cancel_group` to every currently-known origin's gate.
    pub(super) fn cancel_group(&self, group_id: u64) {
        let inner = self.inner.lock();
        for entry in inner.origins.values() {
            entry.gate.cancel_group(group_id);
        }
    }

    /// Number of currently-tracked origins (test/diagnostics).
    #[cfg(test)]
    fn origin_count(&self) -> usize {
        self.inner.lock().origins.len()
    }

    /// Direct access to one origin's underlying gate, for tests that need to
    /// assert on its AIMD state without going through a full acquire/release.
    #[cfg(test)]
    fn peek_gate(&self, origin: &OriginKey) -> Option<Arc<PriorityGate>> {
        self.inner.lock().origins.get(origin).map(|e| Arc::clone(&e.gate))
    }

    /// Spawn the single reaper task that periodically evicts idle origins.
    /// Self-terminating once no origins remain, mirroring
    /// `PriorityGate::spawn_recheck_task`'s lazy-respawn pattern. Must be
    /// called from within the tokio runtime — the only caller, `gate_for`, is
    /// always reached from a spawned fetch task.
    fn spawn_reaper(self: &Arc<Self>) {
        let registry = Arc::clone(self);
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(REAP_INTERVAL).await;
                let mut inner = registry.inner.lock();
                let now = Instant::now();
                inner.origins.retain(|_, entry| {
                    let idle = now.saturating_duration_since(entry.last_seen) >= IDLE_EVICT_AFTER;
                    let quiescent = entry.gate.active_len() == 0 && entry.gate.pending_len() == 0;
                    !(idle && quiescent)
                });
                if inner.origins.is_empty() {
                    inner.reaper_running = false;
                    return;
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::media_queue::{PRIO_NORMAL, PRIO_VISIBLE};

    fn origin(s: &str) -> OriginKey {
        super::super::media_origin::origin_from_url(&format!("https://{s}/"))
    }

    #[tokio::test(start_paused = true)]
    async fn stalling_origin_does_not_throttle_a_healthy_origin() {
        let registry = GateRegistry::new(4, 8, 64);
        let dead = origin("dead.example");
        let healthy = origin("healthy.example");

        // Saturate `dead`'s gate and drive it into mass stall / MD collapse.
        let p0 = registry.acquire(&dead, PRIO_NORMAL, 0, 1).await.unwrap();
        let p1 = registry.acquire(&dead, PRIO_NORMAL, 1, 1).await.unwrap();
        let p2 = registry.acquire(&dead, PRIO_NORMAL, 2, 1).await.unwrap();
        let p3 = registry.acquire(&dead, PRIO_NORMAL, 3, 1).await.unwrap();
        tokio::time::advance(std::time::Duration::from_secs(9)).await; // past STALL_DEADLINE
        drop(p0); // >50% stale among remaining 3 -> MD halves dead's dynamic_limit

        let dead_gate = registry.peek_gate(&dead).unwrap();
        assert!(dead_gate.dynamic_limit() < 4, "dead origin's limit collapsed");

        // `healthy` shares the registry/global semaphore but not `dead`'s gate.
        let h = registry.acquire(&healthy, PRIO_VISIBLE, 4, 2).await;
        assert!(h.is_some(), "healthy origin admitted immediately, unaffected by dead's collapse");
        let healthy_gate = registry.peek_gate(&healthy).unwrap();
        assert_eq!(healthy_gate.dynamic_limit(), 4, "healthy origin's own limit is untouched");

        drop(p1);
        drop(p2);
        drop(p3);
    }

    #[tokio::test(start_paused = true)]
    async fn global_semaphore_caps_aggregate_across_origins() {
        // Each origin could individually admit up to 4, but the global cap
        // is 3 total, shared across two origins each requesting 2.
        let registry = GateRegistry::new(4, 8, 3);
        let a = origin("a.example");
        let b = origin("b.example");

        let _p0 = registry.acquire(&a, PRIO_NORMAL, 0, 1).await.unwrap();
        let _p1 = registry.acquire(&a, PRIO_NORMAL, 1, 1).await.unwrap();
        let _p2 = registry.acquire(&b, PRIO_NORMAL, 2, 2).await.unwrap();

        let g = Arc::clone(&registry);
        let bk = b.clone();
        let h = tokio::spawn(async move { g.acquire(&bk, PRIO_NORMAL, 3, 2).await });
        tokio::task::yield_now().await;
        // The 4th request can't get a global permit even though origin b's
        // own gate has room; it stays pending until one of the first three
        // permits (any origin) is dropped.
        assert!(!h.is_finished(), "4th acquire blocked on the global cap");
        drop(h);
    }

    #[tokio::test(start_paused = true)]
    async fn idle_origin_is_reaped_and_restarts_clean() {
        let registry = GateRegistry::new(4, 8, 64);
        let a = origin("idle.example");

        let p0 = registry.acquire(&a, PRIO_NORMAL, 0, 1).await.unwrap();
        drop(p0);
        assert_eq!(registry.origin_count(), 1);
        // Let the freshly-spawned reaper task get polled once so it registers
        // its first sleep before we advance the clock past it.
        tokio::task::yield_now().await;

        let mut elapsed = std::time::Duration::ZERO;
        let deadline = IDLE_EVICT_AFTER + REAP_INTERVAL + std::time::Duration::from_secs(1);
        while elapsed < deadline && registry.origin_count() > 0 {
            tokio::time::advance(REAP_INTERVAL).await;
            tokio::task::yield_now().await;
            elapsed += REAP_INTERVAL;
        }
        assert_eq!(registry.origin_count(), 0, "idle origin reaped");

        let _p1 = registry.acquire(&a, PRIO_NORMAL, 1, 1).await.unwrap();
        let fresh_gate = registry.peek_gate(&a).unwrap();
        assert_eq!(fresh_gate.dynamic_limit(), 4, "reaped origin restarts with clean AIMD state");
    }

    #[tokio::test]
    async fn cancel_group_reaches_every_origin() {
        let registry = GateRegistry::new(1, 3, 64);
        let a = origin("a.example");
        let b = origin("b.example");
        let _hold_a = registry.acquire(&a, PRIO_NORMAL, 0, 1).await.unwrap();
        let _hold_b = registry.acquire(&b, PRIO_NORMAL, 1, 1).await.unwrap();

        let ga = Arc::clone(&registry);
        let ak = a.clone();
        let ha = tokio::spawn(async move { ga.acquire(&ak, PRIO_NORMAL, 2, 7).await });
        let gb = Arc::clone(&registry);
        let bk = b.clone();
        let hb = tokio::spawn(async move { gb.acquire(&bk, PRIO_NORMAL, 3, 7).await });

        while registry.peek_gate(&a).unwrap().pending_len() < 1
            || registry.peek_gate(&b).unwrap().pending_len() < 1
        {
            tokio::task::yield_now().await;
        }
        registry.cancel_group(7);
        assert!(ha.await.unwrap().is_none(), "parked waiter on origin a cancelled");
        assert!(hb.await.unwrap().is_none(), "parked waiter on origin b cancelled");
    }
}
