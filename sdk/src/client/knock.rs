//! Room knocking (MSC2403) — admin-side pending knock-request subscription.
//!
//! Requester-side actions (`knock_room_async`) and the accept/decline/
//! decline-and-ban actions live in `room_list.rs` alongside the invite
//! actions they mirror; this file only owns the live per-room "who is
//! currently knocking on this room" watcher, since
//! `Room::subscribe_to_knock_requests` has the same stream-plus-background-
//! task shape as the thread-list subscription in `thread.rs`.

use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{err, ok, ClientFfi, KnockRequestsHandle};
#[cfg(test)]
use super::ClientFfi;

#[cfg(not(test))]
use futures_util::StreamExt;
#[cfg(not(test))]
use matrix_sdk::ruma::OwnedRoomId;
#[cfg(not(test))]
use std::sync::Arc;

impl ClientFfi {
    /// Subscribe to the live list of pending knock requests for `room_id`.
    /// Blocks briefly on local-cache setup (member/power-level reads, no
    /// network roundtrip), then spawns a watcher task that updates the
    /// cache and fires `on_knock_requests_updated(room_id)` on every
    /// subsequent change. Mirrors `subscribe_room_threads`. Re-subscribing
    /// the same room aborts the previous watcher.
    #[cfg(not(test))]
    pub fn subscribe_room_knock_requests(&self, room_id: &str) -> OpResult {
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

        // Room::subscribe_to_knock_requests spawns a background task via
        // tokio::spawn, which requires a runtime context on the calling
        // thread — same requirement as ThreadListService::new.
        let _rt_guard = self.rt.enter();
        let (stream, clear_seen_ids_handle) =
            match self.rt.block_on(room.subscribe_to_knock_requests()) {
                Ok(pair) => pair,
                Err(e) => return err(e.to_string()),
            };

        let requests_cell: Arc<parking_lot::RwLock<Vec<crate::ffi::KnockRequestInfo>>> =
            Arc::new(parking_lot::RwLock::new(Vec::new()));
        let requests_cell_for_task = Arc::clone(&requests_cell);
        let rid_str = rid.to_string();
        let h = handler.clone();
        let abort = self
            .rt
            .spawn(async move {
                tokio::pin!(stream);
                while let Some(requests) = stream.next().await {
                    let infos: Vec<crate::ffi::KnockRequestInfo> = requests
                        .iter()
                        .map(|r| crate::ffi::KnockRequestInfo {
                            room_id: rid_str.clone(),
                            user_id: r.member_info.user_id.to_string(),
                            display_name: r.member_info.display_name.clone().unwrap_or_default(),
                            avatar_url: r
                                .member_info
                                .avatar_url
                                .as_ref()
                                .map(|u| u.to_string())
                                .unwrap_or_default(),
                            reason: r.member_info.reason.clone().unwrap_or_default(),
                            timestamp_ts: r.timestamp.map(u64::from).unwrap_or(0),
                        })
                        .collect();
                    *requests_cell_for_task.write() = infos;
                    let g = h.lock();
                    g.on_knock_requests_updated(&rid_str);
                }
            })
            .abort_handle();

        // Atomic swap: install the new handle and abort whatever it replaced,
        // same discipline as subscribe_room_threads.
        if let Some(prev) = self.knock_requests.write().insert(
            rid,
            KnockRequestsHandle {
                requests: requests_cell,
                abort,
                clear_seen_ids_abort: clear_seen_ids_handle.abort_handle(),
            },
        ) {
            prev.abort.abort();
            prev.clear_seen_ids_abort.abort();
        }
        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_room_knock_requests(&self, _room_id: &str) -> OpResult {
        super::err("not logged in")
    }

    /// Unsubscribe from `room_id`'s knock-request watcher and cancel both of
    /// its background tasks. No-op if not subscribed.
    #[cfg(not(test))]
    pub fn unsubscribe_room_knock_requests(&self, room_id: &str) {
        if let Ok(rid) = room_id.parse::<OwnedRoomId>() {
            if let Some(h) = self.knock_requests.write().remove(&rid) {
                h.abort.abort();
                h.clear_seen_ids_abort.abort();
            }
        }
    }

    #[cfg(test)]
    pub fn unsubscribe_room_knock_requests(&self, _room_id: &str) {}

    /// Snapshot of the pending knock requests for `room_id` from the cache
    /// populated by `subscribe_room_knock_requests`. Empty if not
    /// subscribed.
    #[cfg(not(test))]
    pub fn list_knock_requests(&self, room_id: &str) -> Vec<crate::ffi::KnockRequestInfo> {
        let Ok(rid) = room_id.parse::<OwnedRoomId>() else {
            return Vec::new();
        };
        // cxx-bridge shared structs cannot derive Clone, so field-by-field
        // reconstruction stands in for `.clone()` here.
        self.knock_requests
            .read()
            .get(&rid)
            .map(|h| {
                h.requests
                    .read()
                    .iter()
                    .map(|r| crate::ffi::KnockRequestInfo {
                        room_id: r.room_id.clone(),
                        user_id: r.user_id.clone(),
                        display_name: r.display_name.clone(),
                        avatar_url: r.avatar_url.clone(),
                        reason: r.reason.clone(),
                        timestamp_ts: r.timestamp_ts,
                    })
                    .collect()
            })
            .unwrap_or_default()
    }

    #[cfg(test)]
    pub fn list_knock_requests(&self, _room_id: &str) -> Vec<crate::ffi::KnockRequestInfo> {
        Vec::new()
    }
}
