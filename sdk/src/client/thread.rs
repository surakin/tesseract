//! Thread timeline subscriptions and thread-list service.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::{OpResult, PaginateResult};

#[cfg(not(test))]
use super::timeline::TimelineChannel;
#[cfg(not(test))]
use super::timeline_convert::msglike_snippet;
#[cfg(test)]
use super::ClientFfi;
#[cfg(not(test))]
use super::{err, ok, ClientFfi, ThreadListHandle, TimelineHandle};

#[cfg(not(test))]
use crate::ffi::{ThreadInfo, TimelineEvent};

#[cfg(not(test))]
use futures_util::StreamExt;
#[cfg(not(test))]
use matrix_sdk::ruma::OwnedRoomId;
#[cfg(not(test))]
use matrix_sdk_ui::timeline::{RoomExt, TimelineDetails, TimelineFocus};
#[cfg(not(test))]
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(not(test))]
use std::sync::Arc;

impl ClientFfi {
    /// Subscribe to the thread rooted at `root_event_id` in `room_id`. Tears
    /// down any previous subscription for this (room, root) pair, builds a
    /// `TimelineFocus::Thread` timeline, fires an immediate `on_thread_reset`
    /// (empty), then live `on_thread_*` callbacks as replies arrive.
    /// Call `paginate_thread_back` for older replies.
    #[cfg(not(test))]
    pub fn subscribe_thread(&self, room_id: &str, root_event_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.clone() else {
            return err("sync not started");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let root: matrix_sdk::ruma::OwnedEventId = match root_event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid thread root id: {e}")),
        };
        let key = (room_id.clone(), root.clone());
        // Brief write guard: remove the previous handle, then drop the guard
        // before the timeline build / abort calls below (never held across
        // `block_on`).
        let prev = self.thread_timelines.write().remove(&key);
        if let Some(prev) = prev {
            prev.cancelled.store(true, Ordering::Release);
            for h in prev.abort_tasks {
                h.abort();
            }
        }
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let focus = TimelineFocus::Thread {
            root_event_id: root.clone(),
        };
        let room_for_build = room.clone();
        let timeline = match self.rt.block_on(self.rt.spawn(async move {
            room_for_build
                .timeline_builder()
                .with_focus(focus)
                .build()
                .await
        })) {
            Ok(Ok(t)) => Arc::new(t),
            Ok(Err(e)) => return err(format!("build thread timeline: {e}")),
            Err(e) => return err(format!("build thread timeline task: {e}")),
        };
        let room_id_str = room_id.to_string();
        let root_str = root.to_string();
        {
            let guard = handler.lock();
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_thread_reset(&room_id_str, &root_str, &empty);
        }
        let cancelled = Arc::new(AtomicBool::new(false));
        let (abort, fetch_abort) = Self::spawn_timeline_tasks(
            &timeline,
            &room,
            room_id_str.clone(),
            &handler,
            &client,
            &self.rt,
            TimelineChannel::Thread(root_str),
            Arc::clone(&cancelled),
            self.search_index_ctx(),
            None,
            Arc::clone(&self.show_membership_events),
        );
        // Kick an initial backwards pagination so the thread view is populated
        // immediately from the server. Results arrive as VectorDiff events in
        // the streaming task above and are forwarded to C++ via on_thread_inserted.
        let tl_for_paginate = Arc::clone(&timeline);
        let paginate_abort = self
            .rt
            .spawn(async move {
                let _ = tl_for_paginate.paginate_backwards(50).await;
            })
            .abort_handle();

        // Install atomically: if a concurrent subscribe for the same key raced
        // in after our early remove, insert returns its handle — cancel + abort
        // it so its streaming tasks don't outlive this subscription.
        if let Some(prev) = self.thread_timelines.write().insert(
            key,
            TimelineHandle {
                timeline,
                abort_tasks: vec![abort, fetch_abort, paginate_abort],
                is_focused: true,
                cancelled,
            },
        ) {
            prev.cancelled.store(true, Ordering::Release);
            for h in prev.abort_tasks {
                h.abort();
            }
        }
        self.sync_room_subscriptions();
        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_thread(&self, _room_id: &str, _root_event_id: &str) -> OpResult {
        super::err("not logged in")
    }

    /// Unsubscribe from a thread timeline and cancel its background tasks.
    #[cfg(not(test))]
    pub fn unsubscribe_thread(&self, room_id: &str, root_event_id: &str) {
        if let (Ok(rid), Ok(root)) = (
            room_id.parse::<OwnedRoomId>(),
            root_event_id.parse::<matrix_sdk::ruma::OwnedEventId>(),
        ) {
            let removed = self.thread_timelines.write().remove(&(rid, root));
            if let Some(h) = removed {
                h.cancelled.store(true, Ordering::Release);
                for abort in h.abort_tasks {
                    abort.abort();
                }
            }
        }
        self.sync_room_subscriptions();
    }

    #[cfg(test)]
    pub fn unsubscribe_thread(&self, _room_id: &str, _root_event_id: &str) {}

    /// Paginate backwards in a subscribed thread timeline. Older replies
    /// arrive as `on_thread_inserted` callbacks at the front of the thread.
    /// `reached_start` is `true` when there are no more replies to fetch.
    #[cfg(not(test))]
    pub fn paginate_thread_back(
        &self,
        room_id: &str,
        root_event_id: &str,
        count: u16,
    ) -> PaginateResult {
        let rid: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                return PaginateResult {
                    ok: false,
                    message: format!("invalid room id: {e}"),
                    reached_start: false,
                    reached_end: false,
                }
            }
        };
        let root: matrix_sdk::ruma::OwnedEventId = match root_event_id.parse() {
            Ok(id) => id,
            Err(e) => {
                return PaginateResult {
                    ok: false,
                    message: format!("invalid thread root id: {e}"),
                    reached_start: false,
                    reached_end: false,
                }
            }
        };
        // Clone the timeline Arc out from under the read guard so the guard is
        // not held across the `block_on` below.
        let tl = self
            .thread_timelines
            .read()
            .get(&(rid, root))
            .map(|h| Arc::clone(&h.timeline));
        let Some(tl) = tl else {
            return PaginateResult {
                ok: false,
                message: "thread not subscribed".to_owned(),
                reached_start: false,
                reached_end: false,
            };
        };
        match self
            .rt
            .block_on(async move { tl.paginate_backwards(count).await })
        {
            Ok(reached_start) => PaginateResult {
                ok: true,
                message: String::new(),
                reached_start,
                reached_end: false,
            },
            Err(e) => PaginateResult {
                ok: false,
                message: e.to_string(),
                reached_start: false,
                reached_end: false,
            },
        }
    }

    #[cfg(test)]
    pub fn paginate_thread_back(
        &self,
        _room_id: &str,
        _root_event_id: &str,
        _count: u16,
    ) -> PaginateResult {
        PaginateResult {
            ok: false,
            message: "not logged in".into(),
            reached_start: false,
            reached_end: false,
        }
    }

    // -----------------------------------------------------------------------
    // Thread list subscription (ThreadListService)
    // -----------------------------------------------------------------------

    /// Subscribe to the thread list for `room_id`. Spawns an initial pagination
    /// and a watcher task that fires `on_threads_updated` on every change.
    #[cfg(not(test))]
    pub fn subscribe_room_threads(&self, room_id: &str) -> OpResult {
        use matrix_sdk_ui::timeline::ThreadListService;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(handler) = self.handler.clone() else {
            return err("sync not started");
        };
        let rid: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&rid) else {
            return err("room not found");
        };
        // The previous subscription for this room is aborted via the atomic
        // insert at the end (insert returns the replaced handle), not removed up
        // front — so two concurrent subscribes for the same room can't both
        // insert and orphan a live watcher task.
        // ThreadListService::new spawns a background task via tokio::task::spawn,
        // which requires a runtime context on the calling thread.
        let _rt_guard = self.rt.enter();
        let service = std::sync::Arc::new(ThreadListService::new(room));
        // Kick an initial pagination so the first list is populated.
        {
            let svc = std::sync::Arc::clone(&service);
            self.rt.spawn(async move {
                let _ = svc.paginate().await;
            });
        }
        // Watcher task: fire on_threads_updated once immediately, then on
        // every subsequent change to the items vector. After each tick,
        // also drive the per-row "N replies" chip on the room timeline via
        // `apply_thread_chips`. Capture the room timeline once at spawn —
        // it's None if subscribe_room_threads ran without a live room
        // subscription (e.g. user opened the threads panel without
        // entering the room first), in which case we just skip the chip
        // pass on each tick and let `on_threads_updated` drive the panel
        // by itself.
        let rid_str = rid.to_string();
        let svc_for_watch = std::sync::Arc::clone(&service);
        let h = handler.clone();
        let room_for_chips = client.get_room(&rid);
        let room_timeline_for_chips = self
            .timelines
            .read()
            .get(&rid)
            .map(|hh| std::sync::Arc::clone(&hh.timeline));
        let me_for_chips = client.user_id().map(|u| u.to_owned());
        let abort = self
            .rt
            .spawn(async move {
                // matrix-sdk-ui 0.17 `ThreadListService` double-counts on
                // every reply: its event-cache observer treats Insert (local
                // echo) and Set (local echo → server-confirmed) as two
                // separate events, bumping `num_replies` once per diff. The
                // server's bundled count is correct, so we cache per-root
                // and re-fetch `room.event(root)` whenever a thread's
                // `latest_event` event_id changes, using its
                // `thread_summary.num_replies` as the authoritative count.
                let mut chip_cache: std::collections::HashMap<String, ChipCount> =
                    std::collections::HashMap::new();

                let (_initial, mut stream) = svc_for_watch.subscribe_to_items_updates();
                {
                    let g = h.lock();
                    g.on_threads_updated(&rid_str);
                }
                if let (Some(ref tl), Some(ref room)) = (&room_timeline_for_chips, &room_for_chips)
                {
                    apply_thread_chips(
                        tl,
                        room,
                        &h,
                        &rid_str,
                        me_for_chips.as_deref(),
                        &svc_for_watch.items(),
                        &mut chip_cache,
                    )
                    .await;
                }
                while stream.next().await.is_some() {
                    {
                        let g = h.lock();
                        g.on_threads_updated(&rid_str);
                    }
                    if let (Some(ref tl), Some(ref room)) =
                        (&room_timeline_for_chips, &room_for_chips)
                    {
                        apply_thread_chips(
                            tl,
                            room,
                            &h,
                            &rid_str,
                            me_for_chips.as_deref(),
                            &svc_for_watch.items(),
                            &mut chip_cache,
                        )
                        .await;
                    }
                }
            })
            .abort_handle();
        // Atomic swap: install the new handle and abort whatever it replaced.
        // A single guarded insert (vs. a separate remove up front) means a
        // concurrent subscribe for the same room aborts the loser instead of
        // leaking its watcher task.
        if let Some(prev) = self
            .thread_lists
            .write()
            .insert(rid, ThreadListHandle { service, abort })
        {
            prev.abort.abort();
        }
        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_room_threads(&self, _room_id: &str) -> OpResult {
        super::err("not logged in")
    }

    /// Unsubscribe from the thread list for `room_id` and cancel the watcher.
    #[cfg(not(test))]
    pub fn unsubscribe_room_threads(&self, room_id: &str) {
        if let Ok(rid) = room_id.parse::<OwnedRoomId>() {
            let removed = self.thread_lists.write().remove(&rid);
            if let Some(h) = removed {
                h.abort.abort();
            }
        }
    }

    #[cfg(test)]
    pub fn unsubscribe_room_threads(&self, _room_id: &str) {}

    /// Snapshot of the current thread list for `room_id` (order as returned
    /// by the SDK). Empty when not subscribed or no threads known yet.
    ///
    /// Each row is enriched with `unread` (the latest reply is newer than the
    /// user's MSC3771 threaded read receipt for that thread) and a
    /// best-effort `mentions_me` (the latest reply carries an `m.mentions`
    /// naming the user). The receipt load is async and needs the `Room`
    /// handle, so it runs as one pass on a runtime worker thread — same
    /// 512 KB-libdispatch-stack reasoning as `paginate_room_threads`.
    #[cfg(not(test))]
    pub fn list_room_threads(&self, room_id: &str) -> Vec<ThreadInfo> {
        use matrix_sdk::ruma::events::receipt::{ReceiptThread, ReceiptType};
        use matrix_sdk::ruma::OwnedEventId;

        let Ok(rid) = room_id.parse::<OwnedRoomId>() else {
            return Vec::new();
        };
        // Clone the service Arc out from under the read guard before walking
        // its items (keeps the guard window minimal).
        let Some(service) = self
            .thread_lists
            .read()
            .get(&rid)
            .map(|h| h.service.clone())
        else {
            return Vec::new();
        };
        let items = service.items();
        let mut out: Vec<ThreadInfo> = items.iter().map(thread_info_from_item).collect();
        if out.is_empty() {
            return out;
        }

        // best-effort mention flag off the latest reply's `m.mentions`.
        let me = self
            .client
            .as_ref()
            .and_then(|c| c.user_id())
            .map(|u| u.to_owned());
        if let Some(ref me) = me {
            let me_str = me.as_str();
            for (info, item) in out.iter_mut().zip(items.iter()) {
                info.mentions_me = item
                    .latest_event
                    .as_ref()
                    .and_then(|e| e.content.as_ref())
                    .and_then(|c| c.as_message())
                    .and_then(|m| m.mentions())
                    .map(|men| {
                        let ids: Vec<&str> = men.user_ids.iter().map(|u| u.as_str()).collect();
                        any_mention(&ids, men.room, me_str)
                    })
                    .unwrap_or(false);
            }
        }

        let (Some(client), Some(me)) = (self.client.clone(), me) else {
            return out;
        };
        let Some(room) = client.get_room(&rid) else {
            return out;
        };

        let markers = self.thread_read_markers.read().clone();
        let cache_snapshot = self.thread_receipt_cache.read().clone();

        // Only hit the receipt store for threads whose latest reply changed
        // since we last probed (cache miss / different latest event_id) and
        // that actually need a receipt — a thread with no reply, or whose
        // newest reply is the user's own, is trivially read. In steady state
        // this loop probes nothing.
        let mut need: Vec<(usize, OwnedEventId)> = Vec::new();
        for (idx, item) in items.iter().enumerate() {
            let Some(latest) = item.latest_event.as_ref() else {
                continue;
            };
            if latest.is_own {
                continue;
            }
            let root = item.root_event.event_id.clone();
            let cached = cache_snapshot.get(&(rid.clone(), root.clone()));
            let cached_latest = cached.and_then(|(l, _, _)| l.as_deref());
            if cached_latest != Some(latest.event_id.as_str()) {
                need.push((idx, root));
            }
        }

        let room_task = room.clone();
        let me_task = me.clone();
        let need_roots: Vec<OwnedEventId> = need.iter().map(|(_, r)| r.clone()).collect();
        // Fresh (receipt_event_id, receipt_ts) for each `need` root, index-aligned.
        let fresh: Vec<(Option<String>, u64)> = if need_roots.is_empty() {
            Vec::new()
        } else {
            self.rt
                .block_on(self.rt.spawn(async move {
                    let mut v = Vec::with_capacity(need_roots.len());
                    for root in &need_roots {
                        let load = |rt: ReceiptType| {
                            room_task.load_user_receipt(
                                rt,
                                ReceiptThread::Thread(root.clone()),
                                &me_task,
                            )
                        };
                        let pub_r = load(ReceiptType::Read).await.ok().flatten();
                        let priv_r = load(ReceiptType::ReadPrivate).await.ok().flatten();
                        let ts_of = |r: &matrix_sdk::ruma::events::receipt::Receipt| {
                            r.ts.map(|t| u64::from(t.get())).unwrap_or(0)
                        };
                        let best = [pub_r, priv_r]
                            .into_iter()
                            .flatten()
                            .max_by_key(|(_, r)| ts_of(r));
                        v.push(match best {
                            Some((eid, r)) => (Some(eid.to_string()), ts_of(&r)),
                            None => (None, 0),
                        });
                    }
                    v
                }))
                .unwrap_or_default()
        };

        // idx → fresh receipt result.
        let mut fresh_by_idx: std::collections::HashMap<usize, (Option<String>, u64)> =
            std::collections::HashMap::new();
        for ((idx, _), r) in need.iter().zip(fresh) {
            fresh_by_idx.insert(*idx, r);
        }

        let mut stale_markers: Vec<(OwnedRoomId, OwnedEventId)> = Vec::new();
        let mut cache_w = self.thread_receipt_cache.write();
        for (idx, (info, item)) in out.iter_mut().zip(items.iter()).enumerate() {
            let root = &item.root_event.event_id;
            let key = (rid.clone(), root.clone());
            let latest = item.latest_event.as_ref();
            let latest_eid = latest.map(|e| e.event_id.as_str());
            let latest_ts = latest.map(|e| u64::from(e.timestamp.get())).unwrap_or(0);
            let latest_is_own = latest.map(|e| e.is_own).unwrap_or(false);

            // Receipt values: this call's fresh probe, else the cached probe,
            // else "none".
            let (receipt_eid_owned, receipt_ts) = match fresh_by_idx.remove(&idx) {
                Some(r) => {
                    cache_w.insert(
                        key.clone(),
                        (latest_eid.map(str::to_owned), r.0.clone(), r.1),
                    );
                    r
                }
                None => cache_snapshot
                    .get(&key)
                    .map(|(_, e, t)| (e.clone(), *t))
                    .unwrap_or((None, 0)),
            };
            let receipt_eid = receipt_eid_owned.as_deref();
            let marker_ts = markers.get(&key).copied().unwrap_or(0);

            info.unread = thread_is_unread(
                latest_eid,
                latest_ts,
                latest_is_own,
                receipt_eid,
                receipt_ts,
                marker_ts,
            );

            // Drop an optimistic marker once the real threaded receipt has
            // caught up to the current tip, so the map can't grow unbounded.
            let receipt_covers_tip =
                receipt_eid == latest_eid || (latest_ts != 0 && receipt_ts >= latest_ts);
            if marker_ts != 0 && receipt_covers_tip {
                stale_markers.push(key);
            }
        }

        // Evict cache entries for threads no longer in the list.
        {
            let live: std::collections::HashSet<&OwnedEventId> =
                items.iter().map(|it| &it.root_event.event_id).collect();
            cache_w.retain(|(r, root), _| *r != rid || live.contains(root));
        }
        drop(cache_w);

        if !stale_markers.is_empty() {
            let mut w = self.thread_read_markers.write();
            for k in stale_markers {
                w.remove(&k);
            }
        }

        out
    }

    #[cfg(test)]
    pub fn list_room_threads(&self, _room_id: &str) -> Vec<crate::ffi::ThreadInfo> {
        Vec::new()
    }

    /// Paginate older threads for `room_id`. `reached_start == true` means the
    /// server reports no further pages.
    #[cfg(not(test))]
    pub fn paginate_room_threads(&self, room_id: &str) -> PaginateResult {
        let rid: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                return PaginateResult {
                    ok: false,
                    message: format!("invalid room id: {e}"),
                    reached_start: false,
                    reached_end: false,
                }
            }
        };
        // Clone the service Arc out from under the read guard so the guard is
        // not held across the `block_on` below.
        let Some(svc) = self
            .thread_lists
            .read()
            .get(&rid)
            .map(|h| h.service.clone())
        else {
            return PaginateResult {
                ok: false,
                message: "room threads not subscribed".to_owned(),
                reached_start: false,
                reached_end: false,
            };
        };
        // Spawn on a runtime worker thread (8 MB stack) rather than polling
        // the future directly on the calling thread, which may be a libdispatch
        // thread with only 512 KB of stack. svc.paginate() converts a Vec<T>
        // into an imbl vector via deep push_back/promote_front recursion that
        // overflows the smaller stack (same class of crash as the timeline
        // subscribe fix in 789eb2b).
        let svc_for_spawn = std::sync::Arc::clone(&svc);
        let join = self.rt.spawn(async move { svc_for_spawn.paginate().await });
        match self.rt.block_on(join) {
            Ok(Ok(())) => {
                use matrix_sdk_ui::timeline::ThreadListPaginationState;
                let reached_start = matches!(
                    svc.pagination_state(),
                    ThreadListPaginationState::Idle { end_reached: true }
                );
                PaginateResult {
                    ok: true,
                    message: String::new(),
                    reached_start,
                    reached_end: false,
                }
            }
            Ok(Err(e)) => PaginateResult {
                ok: false,
                message: e.to_string(),
                reached_start: false,
                reached_end: false,
            },
            Err(e) => PaginateResult {
                ok: false,
                message: format!("paginate task panicked: {e}"),
                reached_start: false,
                reached_end: false,
            },
        }
    }

    #[cfg(test)]
    pub fn paginate_room_threads(&self, _room_id: &str) -> PaginateResult {
        PaginateResult {
            ok: false,
            message: "room threads not subscribed".to_owned(),
            reached_start: false,
            reached_end: false,
        }
    }
}

/// Per-root cache for `apply_thread_chips`: remembers the last seen
/// `latest_event` event_id and the authoritative count we last pulled
/// from the server's bundled `m.thread` summary. Used to suppress the
/// matrix-sdk-ui 0.17 ThreadListService bug where local-echo +
/// server-confirm causes `num_replies` to bump by 2 per reply.
#[cfg(not(test))]
struct ChipCount {
    last_latest_eid: String,
    server_count: u64,
}

/// Walk the room timeline once and re-emit any thread root row whose chip
/// fields differ from the corresponding `ThreadListItem`. Driven by the
/// `ThreadListService` watcher task; this is the single path that gets the
/// "N replies" chip onto the root row on the main timeline.
///
/// Count comes from `room.event(root)`'s server-bundled `thread_summary`
/// (re-fetched only when `latest_event` event_id changes — see `ChipCount`
/// cache) because matrix-sdk-ui 0.17's `ThreadListService` double-counts
/// every reply. `latest_event` preview fields are taken straight from the
/// `ThreadListItem` since matrix-sdk-ui already tracks the freshest reply
/// for that field in real time.
#[cfg(not(test))]
async fn apply_thread_chips(
    room_timeline: &matrix_sdk_ui::Timeline,
    room: &matrix_sdk::Room,
    handler: &std::sync::Arc<parking_lot::Mutex<super::SendHandler>>,
    room_id: &str,
    me: Option<&matrix_sdk::ruma::UserId>,
    items: &[matrix_sdk_ui::timeline::thread_list_service::ThreadListItem],
    cache: &mut std::collections::HashMap<String, ChipCount>,
) {
    use super::timeline::{emit_updated, TimelineChannel};
    use super::timeline_convert::timeline_item_to_ffi;
    use std::collections::HashMap;

    if items.is_empty() {
        return;
    }

    // First pass: refresh the per-root authoritative count from the server
    // for any thread whose `latest_event` event_id has changed (or is new
    // to us). Bootstrap entries take the ThreadListService's `num_replies`
    // as-is — the initial value comes from the server's /threads response
    // which is already correct; the doubling bug only manifests on later
    // live updates.
    for item in items {
        let root_eid = item.root_event.event_id.to_string();
        let current_latest = item
            .latest_event
            .as_ref()
            .map(|e| e.event_id.to_string())
            .unwrap_or_default();
        match cache.get(&root_eid) {
            None => {
                cache.insert(
                    root_eid,
                    ChipCount {
                        last_latest_eid: current_latest,
                        server_count: item.num_replies as u64,
                    },
                );
            }
            Some(prev) if prev.last_latest_eid == current_latest => {
                // No change in latest event → server count can't have moved.
            }
            Some(_) => {
                // Latest event changed — re-fetch the authoritative count
                // from the server's bundled m.thread summary. matrix-sdk's
                // `Room::event` only writes to the store via `save_events`
                // (doc'd as not notifying observers), so it can't cause
                // the room timeline to gain a duplicate row.
                let server_count = match room.event(&item.root_event.event_id, None).await {
                    Ok(fetched) => fetched
                        .thread_summary
                        .summary()
                        .map(|s| s.num_replies as u64)
                        .unwrap_or_else(|| item.num_replies as u64),
                    // Network/decryption failure — fall back to the
                    // (possibly inflated) value rather than zero, so the
                    // chip never appears to lose replies.
                    Err(_) => item.num_replies as u64,
                };
                cache.insert(
                    root_eid,
                    ChipCount {
                        last_latest_eid: current_latest,
                        server_count,
                    },
                );
            }
        }
    }

    // Second pass: walk the room timeline and emit chip updates.
    // Use a cheap synchronous visibility predicate so the full async FFI
    // conversion (which may issue member-lookup network requests) only fires
    // for the K thread-root candidates instead of all N timeline items.
    let by_root: HashMap<&str, &_> = items
        .iter()
        .map(|it| (it.root_event.event_id.as_str(), it))
        .collect();

    let timeline_items = room_timeline.items().await;
    let mut visible_idx: u64 = 0;
    for tl_item in timeline_items.iter() {
        use matrix_sdk_ui::timeline::{
            AnyOtherStateEventContentChange, MsgLikeContent, MsgLikeKind, TimelineItemContent,
            TimelineItemKind,
        };

        let event_item = match tl_item.kind() {
            TimelineItemKind::Virtual(_) => {
                // Date dividers, read markers, and timeline-start are always
                // visible in the Room channel.
                visible_idx += 1;
                continue;
            }
            TimelineItemKind::Event(e) => e,
        };

        // Cheap visibility predicate replicating filter_for_channel(Room)
        // without the async timeline_item_to_ffi call.
        let visible_in_room = match event_item.content() {
            TimelineItemContent::OtherState(state) => matches!(
                state.content(),
                AnyOtherStateEventContentChange::RoomPinnedEvents(
                    matrix_sdk::ruma::events::StateEventContentChange::Original { .. }
                )
            ),
            TimelineItemContent::MsgLike(MsgLikeContent {
                kind, thread_root, ..
            }) => match kind {
                MsgLikeKind::UnableToDecrypt(_)
                | MsgLikeKind::Redacted
                | MsgLikeKind::Sticker(_) => true,
                // Thread replies are excluded from the Room channel.
                MsgLikeKind::Message(_) => thread_root.is_none(),
                // Reactions, polls, and other kinds are filtered out.
                _ => false,
            },
            _ => false,
        };

        if !visible_in_room {
            continue;
        }

        // Full FFI conversion only for thread-root candidates.
        if let Some(eid) = event_item.event_id() {
            if let Some(item) = by_root.get(eid.as_str()) {
                let Some(mut ev) = timeline_item_to_ffi(tl_item, room_id, room, me).await else {
                    visible_idx += 1;
                    continue;
                };
                let count = cache
                    .get(ev.event_id.as_str())
                    .map(|c| c.server_count)
                    .unwrap_or(item.num_replies as u64);
                let (latest_name, latest_body, latest_ts) = match &item.latest_event {
                    Some(le) => {
                        let (_id, n, b, t) = thread_list_event_preview(le);
                        (n, b, t)
                    }
                    None => (String::new(), String::new(), 0u64),
                };
                // Skip if chip is already aligned — re-emitting an identical
                // row would trigger a needless C++ relayout on every sync tick.
                let already_aligned = ev.is_thread_root
                    && ev.thread_reply_count == count
                    && ev.thread_latest_sender_name == latest_name
                    && ev.thread_latest_body == latest_body
                    && ev.thread_latest_ts == latest_ts;
                if !already_aligned {
                    ev.is_thread_root = true;
                    ev.thread_reply_count = count;
                    ev.thread_latest_sender_name = latest_name;
                    ev.thread_latest_body = latest_body;
                    ev.thread_latest_ts = latest_ts;
                    {
                        let g = handler.lock();
                        emit_updated(&g, &TimelineChannel::Room, room_id, visible_idx, &ev);
                    }
                }
            }
        }

        visible_idx += 1;
    }
}

/// Extract (event_id_str, sender_name, body_snippet, timestamp_ms) from a
/// `ThreadListItemEvent`. Used by both root and latest-event fields.
#[cfg(not(test))]
pub(super) fn thread_list_event_preview(
    ev: &matrix_sdk_ui::timeline::thread_list_service::ThreadListItemEvent,
) -> (String, String, String, u64) {
    let name = match &ev.sender_profile {
        TimelineDetails::Ready(p) => p
            .display_name
            .clone()
            .unwrap_or_else(|| ev.sender.to_string()),
        _ => ev.sender.to_string(),
    };
    let body = match &ev.content {
        Some(content) => msglike_snippet(content),
        None => String::new(),
    };
    let ts: u64 = ev.timestamp.get().into();
    (ev.event_id.to_string(), name, body, ts)
}

/// Convert a `ThreadListItem` into the flat `ThreadInfo` FFI struct.
#[cfg(not(test))]
pub(super) fn thread_info_from_item(
    item: &matrix_sdk_ui::timeline::thread_list_service::ThreadListItem,
) -> crate::ffi::ThreadInfo {
    use crate::ffi::ThreadInfo;
    let (root_event_id, root_sender_name, root_body, root_timestamp) =
        thread_list_event_preview(&item.root_event);
    let (latest_event_id, latest_sender_name, latest_body, latest_timestamp) =
        match &item.latest_event {
            Some(ev) => thread_list_event_preview(ev),
            None => (String::new(), String::new(), String::new(), 0),
        };
    ThreadInfo {
        root_event_id,
        root_sender_name,
        root_body,
        root_timestamp,
        latest_event_id,
        latest_sender_name,
        latest_body,
        latest_timestamp,
        num_replies: item.num_replies as u64,
        // Enriched by `list_room_threads` after this conversion (needs the
        // Room handle + own user id, which this free fn does not have).
        unread: false,
        mentions_me: false,
    }
}

/// Pure decision: does thread `root` have replies the user has not read?
///
/// Inputs are primitives so this is unit-testable without a `Room` or the
/// async store. `latest_*` describe `ThreadListItem::latest_event`; the
/// `receipt_*` pair is the newest of the user's public/private **threaded**
/// (`ReceiptThread::Thread`) read receipts for this root, as loaded from the
/// state store; `local_marker_ts` is an optimistic mark written by
/// `send_thread_read_receipt` so the dot clears before the sync echo lands.
///
/// The receipt store only holds explicit receipts, never the implicit
/// "latest event I sent" one — so `latest_is_own` must short-circuit here,
/// otherwise a thread you just replied in reads as unread until someone
/// else replies.
///
/// `receipt_ts` is the receipt's *send* time, not the acked event's
/// `origin_server_ts` (ruma's `Receipt` exposes no position/ordering API and
/// the timeline's `compare_events_positions` is private). The event-id
/// equality check covers the common "read to the tip" case exactly; the
/// timestamp comparison is the fallback and carries bounded clock-skew risk.
pub(super) fn thread_is_unread(
    latest_event_id: Option<&str>,
    latest_ts: u64,
    latest_is_own: bool,
    receipt_event_id: Option<&str>,
    receipt_ts: u64,
    local_marker_ts: u64,
) -> bool {
    // No replies yet — the thread dot is about *replies*, not the root.
    let Some(latest_id) = latest_event_id else {
        return false;
    };
    // We sent the newest reply.
    if latest_is_own {
        return false;
    }
    // Optimistically marked read locally (receipt sent, echo not yet back).
    if local_marker_ts != 0 && local_marker_ts >= latest_ts {
        return false;
    }
    // Receipt sits exactly on the newest reply.
    if receipt_event_id == Some(latest_id) {
        return false;
    }
    // Never acked this thread at all.
    if receipt_event_id.is_none() && receipt_ts == 0 {
        return true;
    }
    // Fallback: a receipt exists but doesn't point at the newest reply.
    // Treat as unread unless the receipt's send time is at least as recent as
    // the reply. `latest_ts == 0` means we don't know the reply's timestamp —
    // a foreign reply we haven't explicitly acked, so err towards unread.
    latest_ts == 0 || receipt_ts < latest_ts
}

/// Pure decision: does `me` appear in an `m.mentions` block?
pub(super) fn any_mention(user_ids: &[&str], room: bool, me: &str) -> bool {
    room || user_ids.contains(&me)
}

#[cfg(test)]
mod thread_unread_tests {
    use super::{any_mention, thread_is_unread};

    const L: Option<&str> = Some("$latest:server");
    const R_OLD: Option<&str> = Some("$older:server");

    #[test]
    fn no_replies_is_read() {
        assert!(!thread_is_unread(None, 0, false, None, 0, 0));
    }

    #[test]
    fn own_latest_reply_is_read() {
        assert!(!thread_is_unread(L, 100, true, None, 0, 0));
    }

    #[test]
    fn never_acked_with_foreign_reply_is_unread() {
        assert!(thread_is_unread(L, 100, false, None, 0, 0));
    }

    #[test]
    fn receipt_on_latest_is_read() {
        assert!(!thread_is_unread(L, 100, false, L, 90, 0));
    }

    #[test]
    fn stale_receipt_by_id_and_newer_ts_is_read() {
        // Receipt points at an older event but was *sent* after the reply's
        // origin ts (e.g. read on another device just now) → treat as read.
        assert!(!thread_is_unread(L, 100, false, R_OLD, 150, 0));
    }

    #[test]
    fn stale_receipt_by_id_and_older_ts_is_unread() {
        assert!(thread_is_unread(L, 100, false, R_OLD, 50, 0));
    }

    #[test]
    fn local_marker_at_or_after_latest_is_read() {
        assert!(!thread_is_unread(L, 100, false, None, 0, 100));
        assert!(!thread_is_unread(L, 100, false, R_OLD, 10, 120));
    }

    #[test]
    fn local_marker_before_latest_falls_through() {
        // Marker older than the reply, no useful receipt → still unread.
        assert!(thread_is_unread(L, 100, false, None, 0, 40));
    }

    #[test]
    fn unknown_reply_ts_with_stale_receipt_is_unread() {
        // latest_ts == 0 (reply ts unknown) + a receipt on an older event
        // must not be reported read via the `receipt_ts < 0` fallback.
        assert!(thread_is_unread(L, 0, false, R_OLD, 50, 0));
    }

    #[test]
    fn mention_by_user_id() {
        assert!(any_mention(&["@me:server", "@other:server"], false, "@me:server"));
        assert!(!any_mention(&["@other:server"], false, "@me:server"));
    }

    #[test]
    fn mention_by_room_ping() {
        assert!(any_mention(&[], true, "@me:server"));
    }

    #[test]
    fn no_mention_when_absent() {
        assert!(!any_mention(&[], false, "@me:server"));
    }
}
