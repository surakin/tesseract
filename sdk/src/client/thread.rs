//! Thread timeline subscriptions and thread-list service.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::{OpResult, PaginateResult};

#[cfg(not(test))]
use super::{err, ok, ClientFfi, ThreadListHandle, TimelineHandle};
#[cfg(not(test))]
use super::timeline::TimelineChannel;
#[cfg(not(test))]
use super::timeline_convert::msglike_snippet;
#[cfg(test)]
use super::ClientFfi;

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
    pub fn subscribe_thread(
        &mut self,
        room_id: &str,
        root_event_id: &str,
    ) -> OpResult {
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
        if let Some(prev) = self.thread_timelines.remove(&key) {
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
        if let Ok(guard) = handler.lock() {
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
        );
        // Kick an initial backwards pagination so the thread view is populated
        // immediately from the server. Results arrive as VectorDiff events in
        // the streaming task above and are forwarded to C++ via on_thread_inserted.
        let tl_for_paginate = Arc::clone(&timeline);
        let paginate_abort = self.rt.spawn(async move {
            let _ = tl_for_paginate.paginate_backwards(50).await;
        }).abort_handle();

        self.thread_timelines.insert(
            key,
            TimelineHandle {
                timeline,
                abort_tasks: vec![abort, fetch_abort, paginate_abort],
                is_focused: true,
                cancelled,
            },
        );
        self.sync_room_subscriptions();
        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_thread(
        &mut self,
        _room_id: &str,
        _root_event_id: &str,
    ) -> OpResult {
        super::err("not logged in")
    }

    /// Unsubscribe from a thread timeline and cancel its background tasks.
    #[cfg(not(test))]
    pub fn unsubscribe_thread(&mut self, room_id: &str, root_event_id: &str) {
        if let (Ok(rid), Ok(root)) = (
            room_id.parse::<OwnedRoomId>(),
            root_event_id.parse::<matrix_sdk::ruma::OwnedEventId>(),
        ) {
            if let Some(h) = self.thread_timelines.remove(&(rid, root)) {
                h.cancelled.store(true, Ordering::Release);
                for abort in h.abort_tasks {
                    abort.abort();
                }
            }
        }
        self.sync_room_subscriptions();
    }

    #[cfg(test)]
    pub fn unsubscribe_thread(&mut self, _room_id: &str, _root_event_id: &str) {}

    /// Paginate backwards in a subscribed thread timeline. Older replies
    /// arrive as `on_thread_inserted` callbacks at the front of the thread.
    /// `reached_start` is `true` when there are no more replies to fetch.
    #[cfg(not(test))]
    pub fn paginate_thread_back(
        &mut self,
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
        let Some(handle) = self.thread_timelines.get(&(rid, root)) else {
            return PaginateResult {
                ok: false,
                message: "thread not subscribed".to_owned(),
                reached_start: false,
                reached_end: false,
            };
        };
        let tl = Arc::clone(&handle.timeline);
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
        &mut self,
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
    pub fn subscribe_room_threads(&mut self, room_id: &str) -> OpResult {
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
        // Abort any previous subscription for this room.
        if let Some(prev) = self.thread_lists.remove(&rid) {
            prev.abort.abort();
        }
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
            .ok()
            .and_then(|g| g.get(&rid).map(|hh| std::sync::Arc::clone(&hh.timeline)));
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
                let mut chip_cache: std::collections::HashMap<
                    String,
                    ChipCount,
                > = std::collections::HashMap::new();

                let (_initial, mut stream) =
                    svc_for_watch.subscribe_to_items_updates();
                if let Ok(g) = h.lock() {
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
                while stream.next().await.is_some() {
                    if let Ok(g) = h.lock() {
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
        self.thread_lists
            .insert(rid, ThreadListHandle { service, abort });
        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_room_threads(&mut self, _room_id: &str) -> OpResult {
        super::err("not logged in")
    }

    /// Unsubscribe from the thread list for `room_id` and cancel the watcher.
    #[cfg(not(test))]
    pub fn unsubscribe_room_threads(&mut self, room_id: &str) {
        if let Ok(rid) = room_id.parse::<OwnedRoomId>() {
            if let Some(h) = self.thread_lists.remove(&rid) {
                h.abort.abort();
            }
        }
    }

    #[cfg(test)]
    pub fn unsubscribe_room_threads(&mut self, _room_id: &str) {}

    /// Snapshot of the current thread list for `room_id` (order as returned
    /// by the SDK). Empty when not subscribed or no threads known yet.
    #[cfg(not(test))]
    pub fn list_room_threads(&self, room_id: &str) -> Vec<ThreadInfo> {
        let Ok(rid) = room_id.parse::<OwnedRoomId>() else {
            return Vec::new();
        };
        let Some(handle) = self.thread_lists.get(&rid) else {
            return Vec::new();
        };
        handle
            .service
            .items()
            .iter()
            .map(thread_info_from_item)
            .collect()
    }

    #[cfg(test)]
    pub fn list_room_threads(&self, _room_id: &str) -> Vec<crate::ffi::ThreadInfo> {
        Vec::new()
    }

    /// Paginate older threads for `room_id`. `reached_start == true` means the
    /// server reports no further pages.
    #[cfg(not(test))]
    pub fn paginate_room_threads(&mut self, room_id: &str) -> PaginateResult {
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
        let Some(handle) = self.thread_lists.get(&rid) else {
            return PaginateResult {
                ok: false,
                message: "room threads not subscribed".to_owned(),
                reached_start: false,
                reached_end: false,
            };
        };
        let svc = std::sync::Arc::clone(&handle.service);
        // Spawn on a runtime worker thread (8 MB stack) rather than polling
        // the future directly on the calling thread, which may be a libdispatch
        // thread with only 512 KB of stack. svc.paginate() converts a Vec<T>
        // into an imbl vector via deep push_back/promote_front recursion that
        // overflows the smaller stack (same class of crash as the timeline
        // subscribe fix in 789eb2b).
        let join = self.rt.spawn(async move { svc.paginate().await });
        match self.rt.block_on(join) {
            Ok(Ok(())) => {
                use matrix_sdk_ui::timeline::ThreadListPaginationState;
                let reached_start = matches!(
                    handle.service.pagination_state(),
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
    pub fn paginate_room_threads(&mut self, _room_id: &str) -> PaginateResult {
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
    handler: &std::sync::Arc<std::sync::Mutex<super::SendHandler>>,
    room_id: &str,
    me: Option<&matrix_sdk::ruma::UserId>,
    items: &[matrix_sdk_ui::timeline::thread_list_service::ThreadListItem],
    cache: &mut std::collections::HashMap<String, ChipCount>,
) {
    use super::timeline::{emit_updated, filter_for_channel, TimelineChannel};
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
                let server_count = match room
                    .event(&item.root_event.event_id, None)
                    .await
                {
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

    // Second pass: walk the room timeline and emit chip updates. Index
    // map keyed by event_id avoids re-finding the same root across passes.
    let by_root: HashMap<&str, &_> = items
        .iter()
        .map(|it| (it.root_event.event_id.as_str(), it))
        .collect();

    let timeline_items = room_timeline.items().await;
    let mut visible_idx: u64 = 0;
    for tl_item in timeline_items.iter() {
        let Some(mut ev) = filter_for_channel(
            timeline_item_to_ffi(tl_item, room_id, room, me).await,
            &TimelineChannel::Room,
        ) else {
            continue;
        };
        if let Some(item) = by_root.get(ev.event_id.as_str()) {
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
            // Skip if the chip is already aligned — re-emitting an
            // identical row would trigger a needless C++ relayout pass
            // for every sync tick.
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
                if let Ok(g) = handler.lock() {
                    emit_updated(&g, &TimelineChannel::Room, room_id, visible_idx, &ev);
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
    }
}
