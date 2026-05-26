//! Thread timeline subscriptions and thread-list service.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::{OpResult, PaginateResult};

#[cfg(not(test))]
use super::{err, ok, ClientFfi, ThreadListHandle, TimelineHandle};
#[cfg(not(test))]
use super::timeline::{refresh_thread_root_chip, TimelineChannel};
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

        // Continuous chip-refresh task: matrix-sdk-ui's room timeline never
        // updates the cached root's bundled `m.thread` summary on its own
        // (in-thread replies are intentionally filtered from the room
        // timeline, so no local aggregation fires for the root). Subscribe
        // to this thread's diff stream as a second observer: every batch
        // with at least one event change triggers a `room.event()` re-fetch
        // of the root so its chip on the room timeline picks up the fresh
        // count + latest reply preview. An in-flight flag coalesces bursts
        // of replies into one fetch.
        let chip_abort = {
            let room_tl_for_chip = self
                .timelines
                .read()
                .ok()
                .and_then(|g| g.get(&room_id).map(|h| Arc::clone(&h.timeline)));
            if let Some(room_tl) = room_tl_for_chip {
                let tl_for_chip = Arc::clone(&timeline);
                let room_chip = room.clone();
                let rid_chip = room_id_str.clone();
                let root_chip = root.clone();
                let handler_chip = Arc::clone(&handler);
                let me_chip = client.user_id().map(|u| u.to_owned());
                let cancelled_chip = Arc::clone(&cancelled);
                Some(
                    self.rt
                        .spawn(async move {
                            use futures_util::StreamExt;
                            use matrix_sdk_ui::eyeball_im::VectorDiff;
                            use std::sync::atomic::AtomicBool as ChipBool;

                            // One-shot refresh up front so previously-existing
                            // threads (where the cached root never had a bundle)
                            // gain their chip the moment the user opens them.
                            refresh_thread_root_chip(
                                Arc::clone(&room_tl),
                                Arc::clone(&handler_chip),
                                room_chip.clone(),
                                rid_chip.clone(),
                                me_chip.clone(),
                                root_chip.clone(),
                            )
                            .await;

                            let (_initial, mut stream) = tl_for_chip.subscribe().await;
                            let in_flight = Arc::new(ChipBool::new(false));

                            while let Some(diffs) = stream.next().await {
                                if cancelled_chip.load(Ordering::Acquire) {
                                    break;
                                }
                                let touches_events = diffs.iter().any(|d| {
                                    matches!(
                                        d,
                                        VectorDiff::Append { .. }
                                            | VectorDiff::PushBack { .. }
                                            | VectorDiff::PushFront { .. }
                                            | VectorDiff::Insert { .. }
                                            | VectorDiff::Set { .. }
                                            | VectorDiff::Reset { .. }
                                    )
                                });
                                if !touches_events {
                                    continue;
                                }
                                if in_flight.swap(true, Ordering::Acquire) {
                                    // A refresh is already running; it will
                                    // observe whatever state lands by the
                                    // time it issues `room.event()`.
                                    continue;
                                }
                                let room_tl_inner = Arc::clone(&room_tl);
                                let handler_inner = Arc::clone(&handler_chip);
                                let room_inner = room_chip.clone();
                                let rid_inner = rid_chip.clone();
                                let me_inner = me_chip.clone();
                                let root_inner = root_chip.clone();
                                let in_flight_inner = Arc::clone(&in_flight);
                                tokio::spawn(async move {
                                    refresh_thread_root_chip(
                                        room_tl_inner,
                                        handler_inner,
                                        room_inner,
                                        rid_inner,
                                        me_inner,
                                        root_inner,
                                    )
                                    .await;
                                    in_flight_inner.store(false, Ordering::Release);
                                });
                            }
                        })
                        .abort_handle(),
                )
            } else {
                None
            }
        };

        let mut abort_tasks = vec![abort, fetch_abort, paginate_abort];
        if let Some(h) = chip_abort {
            abort_tasks.push(h);
        }
        self.thread_timelines.insert(
            key,
            TimelineHandle {
                timeline,
                abort_tasks,
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
        // every subsequent change to the items vector.
        let rid_str = rid.to_string();
        let svc_for_watch = std::sync::Arc::clone(&service);
        let h = handler.clone();
        let abort = self
            .rt
            .spawn(async move {
                let (_initial, mut stream) =
                    svc_for_watch.subscribe_to_items_updates();
                if let Ok(g) = h.lock() {
                    g.on_threads_updated(&rid_str);
                }
                while stream.next().await.is_some() {
                    if let Ok(g) = h.lock() {
                        g.on_threads_updated(&rid_str);
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
        match self.rt.block_on(async move { svc.paginate().await }) {
            Ok(()) => {
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
            Err(e) => PaginateResult {
                ok: false,
                message: e.to_string(),
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
