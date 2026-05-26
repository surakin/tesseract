//! Room timeline subscriptions and pagination, including focused-timeline
//! mode (MSC3030 jump-to-date) and the diff-streaming task that pumps
//! matrix-sdk-ui timeline updates onto the UI thread.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::{OpResult, PaginateResult};

#[cfg(not(test))]
use super::{err, ok, ClientFfi, SendHandler, TimelineHandle};

#[cfg(test)]
use super::{err, ClientFfi};

#[cfg(not(test))]
use super::timeline_convert::timeline_item_to_ffi;

#[cfg(not(test))]
use crate::ffi::TimelineEvent;

#[cfg(not(test))]
use futures_util::StreamExt;
#[cfg(not(test))]
use matrix_sdk::{
    ruma::{OwnedEventId, OwnedRoomId, OwnedUserId, UInt, UserId},
    Client, Room,
};
#[cfg(not(test))]
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    timeline::{
        RoomExt, TimelineEventFocusThreadMode, TimelineFocus, TimelineItem,
    },
};
#[cfg(not(test))]
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(not(test))]
use std::sync::{Arc, Mutex};

// ---------------------------------------------------------------------------
// TimelineChannel and emit helpers
// ---------------------------------------------------------------------------

#[cfg(not(test))]
#[derive(Clone)]
pub(crate) enum TimelineChannel {
    Room,
    Thread(String), // thread root event id
}

#[cfg(not(test))]
pub(super) fn emit_reset(
    g: &SendHandler,
    ch: &TimelineChannel,
    id: &str,
    snap: &Vec<TimelineEvent>,
) {
    match ch {
        TimelineChannel::Room => g.on_timeline_reset(id, snap),
        TimelineChannel::Thread(root) => g.on_thread_reset(id, root, snap),
    }
}
#[cfg(not(test))]
pub(super) fn emit_inserted(
    g: &SendHandler,
    ch: &TimelineChannel,
    id: &str,
    idx: u64,
    ev: &TimelineEvent,
) {
    match ch {
        TimelineChannel::Room => g.on_message_inserted(id, idx, ev),
        TimelineChannel::Thread(root) => g.on_thread_inserted(id, root, idx, ev),
    }
}
#[cfg(not(test))]
pub(super) fn emit_updated(
    g: &SendHandler,
    ch: &TimelineChannel,
    id: &str,
    idx: u64,
    ev: &TimelineEvent,
) {
    match ch {
        TimelineChannel::Room => g.on_message_updated(id, idx, ev),
        TimelineChannel::Thread(root) => g.on_thread_updated(id, root, idx, ev),
    }
}
#[cfg(not(test))]
pub(super) fn emit_removed(g: &SendHandler, ch: &TimelineChannel, id: &str, idx: u64) {
    match ch {
        TimelineChannel::Room => g.on_message_removed(id, idx),
        TimelineChannel::Thread(root) => g.on_thread_removed(id, root, idx),
    }
}

// Count visible (event) items strictly before `raw_index` in the
// visibility mirror — this is the visible index that maps to the C++
// row vector for an op at matrix-sdk-ui slot `raw_index`.
pub(crate) fn visible_index_of(visible: &[bool], raw_index: usize) -> u64 {
    // Clamp instead of slicing directly: a buggy / version-mismatched SDK
    // delivering an out-of-range index must not panic on a tokio task thread
    // (which would silently break timeline tracking for the room).
    let end = raw_index.min(visible.len());
    visible[..end].iter().filter(|b| **b).count() as u64
}

pub(crate) fn visible_len(visible: &[bool]) -> u64 {
    visible.iter().filter(|b| **b).count() as u64
}

// `hide_threaded_events: true` prevents most thread replies from entering the
// room timeline stream, but edge cases in matrix-sdk (initial snapshots, state
// transitions) can still let some through. This filter is the safety net that
// keeps the `visible` index mirror in sync with what C++ actually renders, so
// VectorDiff::Set confirmations land on the right slot and local echoes
// transition from "sending" to "sent" correctly.
#[cfg(not(test))]
fn filter_for_channel(
    ev: Option<TimelineEvent>,
    ch: &TimelineChannel,
) -> Option<TimelineEvent> {
    match ch {
        TimelineChannel::Room => ev.filter(|e| e.thread_root_id.is_empty()),
        TimelineChannel::Thread(_) => ev,
    }
}

#[cfg(not(test))]
pub(super) async fn handle_timeline_diff(
    diff: VectorDiff<Arc<TimelineItem>>,
    visible: &mut Vec<bool>,
    handler: &Arc<Mutex<SendHandler>>,
    room_id: &str,
    room: &Room,
    me: Option<&UserId>,
    _client: &Client,
    channel: &TimelineChannel,
    cancelled: &AtomicBool,
) {
    if cancelled.load(Ordering::Acquire) { return; }
    match diff {
        VectorDiff::Append { values } => {
            for item in values {
                let ev = filter_for_channel(
                    timeline_item_to_ffi(&item, room_id, room, me).await,
                    channel,
                );
                if let Some(ev) = ev {
                    let idx = visible_len(visible);
                    visible.push(true);
                    if let Ok(g) = handler.lock() {
                        emit_inserted(&g, channel, room_id, idx, &ev);
                    }
                } else {
                    visible.push(false);
                }
            }
        }
        VectorDiff::PushBack { value } => {
            let ev = filter_for_channel(
                timeline_item_to_ffi(&value, room_id, room, me).await,
                channel,
            );
            if let Some(ev) = ev {
                let idx = visible_len(visible);
                visible.push(true);
                if let Ok(g) = handler.lock() {
                    emit_inserted(&g, channel, room_id, idx, &ev);
                }
            } else {
                visible.push(false);
            }
        }
        VectorDiff::PushFront { value } => {
            let ev = filter_for_channel(
                timeline_item_to_ffi(&value, room_id, room, me).await,
                channel,
            );
            if let Some(ev) = ev {
                visible.insert(0, true);
                if let Ok(g) = handler.lock() {
                    emit_inserted(&g, channel, room_id, 0, &ev);
                }
            } else {
                visible.insert(0, false);
            }
        }
        VectorDiff::Insert { index, value } => {
            // Vec::insert panics if index > len. matrix-sdk-ui should never
            // emit that, but a bad index on a task thread must not crash
            // timeline tracking — clamp to an append.
            let index = index.min(visible.len());
            let ev = filter_for_channel(
                timeline_item_to_ffi(&value, room_id, room, me).await,
                channel,
            );
            if let Some(ev) = ev {
                let v_idx = visible_index_of(visible, index);
                visible.insert(index, true);
                if let Ok(g) = handler.lock() {
                    emit_inserted(&g, channel, room_id, v_idx, &ev);
                }
            } else {
                visible.insert(index, false);
            }
        }
        VectorDiff::Set { index, value } => {
            // `Set` can change the visibility of the slot in either
            // direction: a virtual item can be replaced by an event item
            // (decryption completes, day-divider repositions), or vice
            // versa. Map the four transitions explicitly.
            if index >= visible.len() {
                // Out-of-range Set means our mirror already disagrees with
                // the SDK's vector. Emitting a phantom insert here would
                // desync the C++ row count further; skip and log instead.
                tracing::error!(
                    "VectorDiff::Set index {} out of range (len {}) for {}",
                    index,
                    visible.len(),
                    room_id,
                );
                return;
            }
            let new_ev = filter_for_channel(
                timeline_item_to_ffi(&value, room_id, room, me).await,
                channel,
            );
            let was_visible = visible.get(index).copied().unwrap_or(false);
            match (was_visible, new_ev) {
                (true, Some(ev)) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Ok(g) = handler.lock() {
                        emit_updated(&g, channel, room_id, v_idx, &ev);
                    }
                }
                (false, Some(ev)) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Some(slot) = visible.get_mut(index) {
                        *slot = true;
                    }
                    if let Ok(g) = handler.lock() {
                        emit_inserted(&g, channel, room_id, v_idx, &ev);
                    }
                }
                (true, None) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Some(slot) = visible.get_mut(index) {
                        *slot = false;
                    }
                    if let Ok(g) = handler.lock() {
                        emit_removed(&g, channel, room_id, v_idx);
                    }
                }
                (false, None) => {}
            }
        }
        VectorDiff::Remove { index } => {
            let was_visible = visible.get(index).copied().unwrap_or(false);
            if was_visible {
                let v_idx = visible_index_of(visible, index);
                visible.remove(index);
                if let Ok(g) = handler.lock() {
                    emit_removed(&g, channel, room_id, v_idx);
                }
            } else if index < visible.len() {
                visible.remove(index);
            }
        }
        VectorDiff::Truncate { length } => {
            // Drop highest indices first so each visible-index we report
            // is still valid when the C++ side applies it.
            while visible.len() > length {
                let raw = visible.len() - 1;
                let was = visible[raw];
                visible.pop();
                if was {
                    let v_idx = visible_len(visible);
                    if let Ok(g) = handler.lock() {
                        emit_removed(&g, channel, room_id, v_idx);
                    }
                }
            }
        }
        VectorDiff::PopBack => {
            if let Some(was) = visible.pop() {
                if was {
                    let v_idx = visible_len(visible);
                    if let Ok(g) = handler.lock() {
                        emit_removed(&g, channel, room_id, v_idx);
                    }
                }
            }
        }
        VectorDiff::PopFront => {
            if !visible.is_empty() {
                let was = visible.remove(0);
                if was {
                    if let Ok(g) = handler.lock() {
                        emit_removed(&g, channel, room_id, 0);
                    }
                }
            }
        }
        VectorDiff::Clear => {
            visible.clear();
            if let Ok(g) = handler.lock() {
                let empty: Vec<TimelineEvent> = Vec::new();
                emit_reset(&g, channel, room_id, &empty);
            }
        }
        VectorDiff::Reset { values } => {
            // Atomic snapshot replace. Build the new visibility mirror +
            // snapshot in one pass before the single `emit_reset`
            // call so the UI rebuilds in one shot.
            visible.clear();
            visible.reserve(values.len());
            let mut snapshot: Vec<TimelineEvent> = Vec::new();
            for item in &values {
                let ev = filter_for_channel(
                    timeline_item_to_ffi(item, room_id, room, me).await,
                    channel,
                );
                visible.push(ev.is_some());
                if let Some(ev) = ev {
                    snapshot.push(ev);
                }
            }
            if let Ok(g) = handler.lock() {
                emit_reset(&g, channel, room_id, &snapshot);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Thread root chip refresh
// ---------------------------------------------------------------------------

/// Re-fetch a thread root event from the homeserver and re-emit its row on the
/// room timeline with the freshly-bundled `thread_summary` overlayed onto the
/// FFI event.
///
/// Why this exists: matrix-sdk-ui's room timeline filters in-thread replies
/// (they live in the thread sub-timeline), so the bundled `m.thread` relation
/// on the cached root `EventTimelineItem` is never refreshed locally — its
/// `thread_summary()` keeps returning `None` indefinitely, and the chip on the
/// root never appears. The server, however, does start including the bundle in
/// every `/event/{root_id}` response the moment the first reply lands; a fresh
/// fetch gives us the authoritative count + latest reply preview in one hop.
///
/// Call sites: `send_thread_inner` (after sending) and the per-thread
/// subscriber in `subscribe_thread` (when new replies are observed).
#[cfg(not(test))]
pub(super) async fn refresh_thread_root_chip(
    room_timeline: Arc<matrix_sdk_ui::Timeline>,
    handler: Arc<Mutex<SendHandler>>,
    room: Room,
    room_id_str: String,
    me: Option<OwnedUserId>,
    root_event_id: OwnedEventId,
) {
    use super::timeline_convert::bundled_event_preview;

    // 1. Fetch the root from the server. The response carries
    //    `unsigned.m.relations.m.thread` which matrix-sdk lifts into
    //    `TimelineEvent::thread_summary` + `bundled_latest_thread_event`.
    let fetched = match room.event(&root_event_id, None).await {
        Ok(e) => e,
        Err(_) => return,
    };
    let summary = match fetched.thread_summary.summary() {
        Some(s) => s.clone(),
        None => return, // server hasn't bundled a thread for this root yet.
    };

    // 2. Walk the live room-timeline items to find the root by event_id,
    //    counting visible (FFI-emitting) slots before it so the C++ row
    //    index matches what `handle_timeline_diff` would emit. Stop as soon
    //    as we find it; rebuild the FFI event off the *current* item so we
    //    keep all unrelated fields (reactions, receipts, sender, content)
    //    intact, then overlay just the chip fields.
    let items = room_timeline.items().await;
    let mut visible_idx: u64 = 0;
    let mut found: Option<(u64, TimelineEvent)> = None;
    let root_id_str = root_event_id.as_str();
    for item in items.iter() {
        let ev = filter_for_channel(
            timeline_item_to_ffi(item, &room_id_str, &room, me.as_deref()).await,
            &TimelineChannel::Room,
        );
        if let Some(mut ev) = ev {
            if ev.event_id == root_id_str {
                ev.is_thread_root = true;
                ev.thread_reply_count = summary.num_replies as u64;
                if let Some(latest) = fetched.bundled_latest_thread_event.as_deref() {
                    let (sname, body, ts) = bundled_event_preview(latest, &room).await;
                    ev.thread_latest_sender_name = sname;
                    ev.thread_latest_body = body;
                    ev.thread_latest_ts = ts;
                }
                found = Some((visible_idx, ev));
                break;
            }
            visible_idx += 1;
        }
    }
    let Some((idx, ev)) = found else { return; };

    if let Ok(g) = handler.lock() {
        emit_updated(&g, &TimelineChannel::Room, &room_id_str, idx, &ev);
    }
}

// ---------------------------------------------------------------------------
// ClientFfi timeline methods
// ---------------------------------------------------------------------------

impl ClientFfi {
    /// Spawn the two tasks that are common to both `subscribe_room` and
    /// `subscribe_room_at`: the streaming task that pumps timeline diffs to
    /// the UI, and the `fetch_members` backfill task. Returns their
    /// `AbortHandle`s so the caller can store them in `TimelineHandle`.
    ///
    /// `timeline` and `room` must already be fully constructed; `room_id_str`
    /// is the string form of the room ID used by handler callbacks.
    #[cfg(not(test))]
    pub(super) fn spawn_timeline_tasks(
        timeline: &Arc<matrix_sdk_ui::Timeline>,
        room: &matrix_sdk::Room,
        room_id_str: String,
        handler: &Arc<Mutex<SendHandler>>,
        client: &Client,
        rt: &tokio::runtime::Runtime,
        channel: TimelineChannel,
        cancelled: Arc<AtomicBool>,
    ) -> (tokio::task::AbortHandle, tokio::task::AbortHandle) {
        let tl = Arc::clone(timeline);
        let h = Arc::clone(handler);
        let rid = room_id_str;
        let room_clone = room.clone();
        let me = client.user_id().map(|u| u.to_owned());
        let client_ref = client.clone();
        let ch = channel;

        // Cancellation flag clones — one per task.
        let cancelled_stream  = Arc::clone(&cancelled);
        let cancelled_members = cancelled;

        // Extra clones for the receipt-refresh pass after fetch_members (below).
        let h_for_receipts    = Arc::clone(handler);
        let rid_for_receipts  = rid.clone();
        let room_for_receipts = room_clone.clone();
        let me_for_receipts   = me.clone();
        let ch_for_receipts   = ch.clone();

        let abort = rt
            .spawn(async move {
                let (initial_items, mut stream) = tl.subscribe().await;

                // Build the visibility mirror + initial snapshot in one pass.
                // The mirror is `true` for every matrix-sdk-ui timeline slot
                // whose `timeline_item_to_ffi` yields Some — this covers both
                // real message events and virtual items (day-dividers,
                // read-markers, timeline-start). State events and membership
                // changes remain `false` so they are silently filtered.
                let mut visible: Vec<bool> = Vec::with_capacity(initial_items.len());
                let mut snapshot: Vec<TimelineEvent> = Vec::new();
                for item in initial_items.iter() {
                    let ev = filter_for_channel(
                        timeline_item_to_ffi(item, &rid, &room_clone, me.as_deref()).await,
                        &ch,
                    );
                    visible.push(ev.is_some());
                    if let Some(ev) = ev {
                        snapshot.push(ev);
                    }
                }
                if !cancelled_stream.load(Ordering::Acquire) {
                    if let Ok(guard) = h.lock() {
                        emit_reset(&guard, &ch, &rid, &snapshot);
                    }
                }
                drop(snapshot);

                while let Some(diffs) = stream.next().await {
                    if cancelled_stream.load(Ordering::Acquire) { break; }
                    for diff in diffs {
                        handle_timeline_diff(
                            diff,
                            &mut visible,
                            &h,
                            &rid,
                            &room_clone,
                            me.as_deref(),
                            &client_ref,
                            &ch,
                            &cancelled_stream,
                        )
                        .await;
                    }
                }
            })
            .abort_handle();

        // Backfill sender profiles. `matrix-sdk-ui`'s Timeline does not
        // sync member info on its own — `EventTimelineItem::sender_profile()`
        // stays at `TimelineDetails::Pending` for any user whose
        // membership state wasn't included in the initial sync's room
        // delta, so their messages render with an empty name + avatar.
        // `fetch_members()` runs `sync_members()` then patches every
        // affected timeline item in place, which the streaming task
        // above picks up as `VectorDiff::Set` and re-emits to C++. We
        // spawn it separately so the initial items aren't blocked
        // behind a multi-second member sync on big rooms.
        let tl_for_members = Arc::clone(timeline);
        let fetch_abort = rt
            .spawn(async move {
                tl_for_members.fetch_members().await;

                // `fetch_members()` causes matrix-sdk-ui to emit VectorDiff::Set
                // only for items whose *sender* profile was pending. Items whose
                // sender was already resolved are not re-emitted, even when their
                // read-receipt entries reference users whose profiles were missing
                // at receipt-arrival time (get_member_no_sync returned None →
                // fallback to user_id / empty avatar_url). Re-emit every visible
                // item that carries receipts so the C++ chips pick up the
                // now-available display names and avatar URLs.
                let items = tl_for_members.items().await;
                let mut visible_idx: u64 = 0;
                for item in items.iter() {
                    let ev = timeline_item_to_ffi(
                        item,
                        &rid_for_receipts,
                        &room_for_receipts,
                        me_for_receipts.as_deref(),
                    )
                    .await;
                    let ev = filter_for_channel(ev, &ch_for_receipts);
                    if let Some(ev) = ev {
                        if !ev.read_receipts.is_empty()
                            && !cancelled_members.load(Ordering::Acquire)
                        {
                            if let Ok(g) = h_for_receipts.lock() {
                                emit_updated(
                                    &g,
                                    &ch_for_receipts,
                                    &rid_for_receipts,
                                    visible_idx,
                                    &ev,
                                );
                            }
                        }
                        visible_idx += 1;
                    }
                }
            })
            .abort_handle();

        (abort, fetch_abort)
    }

    /// Schedule a non-blocking refresh of the thread root's chip on the
    /// room timeline. Looks up the room's `Arc<Timeline>` once and spawns
    /// the refresh on the runtime. No-op when the room isn't subscribed,
    /// no handler is attached, or the client isn't logged in — those
    /// states mean there's no UI to update.
    #[cfg(not(test))]
    pub(super) fn schedule_thread_root_refresh(
        &self,
        room_id: &OwnedRoomId,
        root_event_id: &OwnedEventId,
    ) {
        let Some(client) = self.client.clone() else { return; };
        let Some(handler) = self.handler.clone() else { return; };
        let Some(room) = client.get_room(room_id) else { return; };
        let room_timeline = match self.timelines.read() {
            Ok(g) => match g.get(room_id) {
                Some(h) => Arc::clone(&h.timeline),
                None => return,
            },
            Err(_) => return,
        };
        let room_id_str = room_id.to_string();
        let me = client.user_id().map(|u| u.to_owned());
        let root = root_event_id.clone();
        self.rt.spawn(async move {
            refresh_thread_root_chip(
                room_timeline,
                handler,
                room,
                room_id_str,
                me,
                root,
            )
            .await;
        });
    }

    #[cfg(not(test))]
    pub fn subscribe_room(&mut self, room_id: &str) -> OpResult {
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

        // Drop any previous subscription for this room.
        {
            let mut guard = self.timelines.write().unwrap();
            if let Some(prev) = guard.remove(&room_id) {
                prev.cancelled.store(true, Ordering::Release);
                for h in prev.abort_tasks {
                    h.abort();
                }
            }
        }

        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };

        // Build the timeline on a runtime worker thread, not the calling FFI
        // thread. `Timeline::init_focus` collects the cached events into an
        // `imbl::Vector`; chunk promotion for large `TimelineEvent`s recurses
        // deep enough to overflow the small stack of the macOS libdispatch
        // worker that drives this FFI call (EXC_BAD_ACCESS). Worker threads
        // have the widened 8 MB stack configured on the runtime above.
        let room_for_build = room.clone();
        let timeline = match self.rt.block_on(
            self.rt.spawn(async move {
                room_for_build
                    .timeline_builder()
                    .with_focus(TimelineFocus::Live {
                        hide_threaded_events: true,
                    })
                    .build()
                    .await
            }),
        ) {
            Ok(Ok(t)) => Arc::new(t),
            Ok(Err(e)) => return err(format!("build timeline: {e}")),
            Err(e) => return err(format!("build timeline task: {e}")),
        };

        let room_id_str = room_id.to_string();

        // Synchronously clear the UI for this room. The follow-up snapshot
        // reset (with the initial cached items) arrives from the spawned
        // task below. Both go through the UI's post-to-UI queue so they
        // serialize in order and no live diffs can land between them —
        // diffs only flow once the task starts pumping `stream`.
        if let Ok(guard) = handler.lock() {
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_timeline_reset(&room_id_str, &empty);
        }

        let cancelled = Arc::new(AtomicBool::new(false));
        let (abort, fetch_abort) =
            Self::spawn_timeline_tasks(&timeline, &room, room_id_str, &handler, &client, &self.rt, TimelineChannel::Room, Arc::clone(&cancelled));

        self.timelines.write().unwrap().insert(
            room_id,
            TimelineHandle {
                timeline,
                abort_tasks: vec![abort, fetch_abort],
                is_focused: false,
                cancelled,
            },
        );

        self.sync_room_subscriptions();
        ok("")
    }

    #[cfg(not(test))]
    pub fn unsubscribe_room(&mut self, room_id: &str) {
        if let Ok(id) = room_id.parse::<OwnedRoomId>() {
            if let Some(h) = self.timelines.write().unwrap().remove(&id) {
                h.cancelled.store(true, Ordering::Release);
                for abort in h.abort_tasks {
                    abort.abort();
                }
            }
        }
        self.sync_room_subscriptions();
    }

    #[cfg(not(test))]
    pub fn paginate_back(&self, room_id: &str, count: u16) -> OpResult {
        let result = self.paginate_back_with_status(room_id, count);
        OpResult {
            ok: result.ok,
            message: result.message,
        }
    }

    #[cfg(not(test))]
    pub fn paginate_back_with_status(&self, room_id: &str, count: u16) -> PaginateResult {
        let room_id: OwnedRoomId = match room_id.parse() {
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

        let tl = {
            let guard = self.timelines.read().unwrap();
            let Some(handle) = guard.get(&room_id) else {
                return PaginateResult {
                    ok: false,
                    message: "room not subscribed; call subscribe_room first".into(),
                    reached_start: false,
                    reached_end: false,
                };
            };
            Arc::clone(&handle.timeline)
        };
        let stop_rx = self.stop_rx.clone();

        // Race the network round-trip against the shutdown signal so that
        // `stop_sync()` unblocks any worker threads waiting in this call.
        match self.rt.block_on(async move {
            let paginate = tl.paginate_backwards(count);
            if let Some(mut rx) = stop_rx {
                tokio::select! {
                    result = paginate => result.map(Some),
                    _ = async {
                        loop {
                            match rx.changed().await {
                                Ok(()) => { if *rx.borrow() { return; } }
                                Err(_) => return, // sender dropped = shutdown
                            }
                        }
                    } => Ok(None),
                }
            } else {
                paginate.await.map(Some)
            }
        }) {
            Ok(Some(reached_start)) => PaginateResult {
                ok: true,
                message: String::new(),
                reached_start,
                reached_end: false,
            },
            Ok(None) => PaginateResult {
                ok: false,
                message: "shutdown in progress".into(),
                reached_start: false,
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

    // -----------------------------------------------------------------------
    // MSC3030 Jump to Date
    // -----------------------------------------------------------------------

    /// MSC3030: resolve a Unix millisecond timestamp to the nearest event ID
    /// in `room_id`. `dir` is `"f"` (forward) or `"b"` (backward).
    /// On success, `OpResult.message` holds the event ID string.
    #[cfg(not(test))]
    pub fn timestamp_to_event(&mut self, room_id: &str, ts_ms: u64, dir: &str) -> OpResult {
        use matrix_sdk::ruma::{
            api::{client::room::get_event_by_timestamp::v1::Request, Direction},
            MilliSecondsSinceUnixEpoch,
        };

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        let room_id: matrix_sdk::ruma::OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };

        let direction = match dir {
            "f" => Direction::Forward,
            "b" => Direction::Backward,
            _ => return err(format!("invalid dir {:?}: expected \"f\" or \"b\"", dir)),
        };

        let ts = match UInt::try_from(ts_ms) {
            Ok(u) => MilliSecondsSinceUnixEpoch(u),
            Err(_) => {
                return err(format!(
                    "timestamp {ts_ms} is out of range for MilliSecondsSinceUnixEpoch"
                ))
            }
        };

        let req = Request::new(room_id, ts, direction);

        // No shutdown-race here: timestamp lookups are typically fast (single HTTP
        // round-trip) and not in a loop, so blocking the thread is acceptable.
        match self.rt.block_on(async move { client.send(req).await }) {
            Ok(resp) => ok(resp.event_id.to_string()),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn timestamp_to_event(&mut self, _room_id: &str, _ts_ms: u64, _dir: &str) -> OpResult {
        err("not logged in")
    }

    /// MSC3030: subscribe to `room_id`'s timeline focused on `focus_event_id`.
    /// Tears down any previous subscription for this room, then builds a
    /// `TimelineFocus::Event` timeline. Fires `on_timeline_reset` + individual
    /// event callbacks identically to `subscribe_room`. Sets `is_focused = true`
    /// so that `paginate_forward` can gate itself.
    #[cfg(not(test))]
    pub fn subscribe_room_at(&mut self, room_id: &str, focus_event_id: &str) -> OpResult {
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

        let target: matrix_sdk::ruma::OwnedEventId = match focus_event_id.try_into() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        // Drop any previous subscription for this room.
        {
            let mut guard = self.timelines.write().unwrap();
            if let Some(prev) = guard.remove(&room_id) {
                prev.cancelled.store(true, Ordering::Release);
                for h in prev.abort_tasks {
                    h.abort();
                }
            }
        }

        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };

        let focus = TimelineFocus::Event {
            target,
            num_context_events: 50,
            thread_mode: TimelineEventFocusThreadMode::Automatic {
                hide_threaded_events: true,
            },
        };

        // Build off the calling FFI thread — see the note in `subscribe_room`;
        // the focused build runs the same imbl `collect()` over cached events.
        let room_for_build = room.clone();
        let timeline = match self.rt.block_on(self.rt.spawn(async move {
            room_for_build
                .timeline_builder()
                .with_focus(focus)
                .build()
                .await
        })) {
            Ok(Ok(t)) => Arc::new(t),
            Ok(Err(e)) => return err(format!("build focused timeline: {e}")),
            Err(e) => return err(format!("build focused timeline task: {e}")),
        };

        let room_id_str = room_id.to_string();

        // Synchronously clear the UI for this room (same as subscribe_room).
        if let Ok(guard) = handler.lock() {
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_timeline_reset(&room_id_str, &empty);
        }

        let cancelled = Arc::new(AtomicBool::new(false));
        let (abort, fetch_abort) =
            Self::spawn_timeline_tasks(&timeline, &room, room_id_str, &handler, &client, &self.rt, TimelineChannel::Room, Arc::clone(&cancelled));

        self.timelines.write().unwrap().insert(
            room_id,
            TimelineHandle {
                timeline,
                abort_tasks: vec![abort, fetch_abort],
                is_focused: true,
                cancelled,
            },
        );

        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_room_at(&mut self, _room_id: &str, _focus_event_id: &str) -> OpResult {
        err("not logged in")
    }

    /// MSC3030: paginate forward in a focused timeline. Only valid after
    /// `subscribe_room_at`; returns an error for live timelines.
    /// `reached_end = true` when the timeline has reached the live end.
    #[cfg(not(test))]
    pub fn paginate_forward(&mut self, room_id: &str, count: u16) -> PaginateResult {
        let room_id: OwnedRoomId = match room_id.parse() {
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

        let tl = {
            let guard = self.timelines.read().unwrap();
            let Some(handle) = guard.get(&room_id) else {
                return PaginateResult {
                    ok: false,
                    message: "room not subscribed; call subscribe_room_at first".into(),
                    reached_start: false,
                    reached_end: false,
                };
            };

            if !handle.is_focused {
                return PaginateResult {
                    ok: false,
                    message: "not in focused mode".into(),
                    reached_start: false,
                    reached_end: false,
                };
            }

            Arc::clone(&handle.timeline)
        };
        let stop_rx = self.stop_rx.clone();

        match self.rt.block_on(async move {
            let paginate = tl.paginate_forwards(count);
            if let Some(mut rx) = stop_rx {
                tokio::select! {
                    result = paginate => result.map(Some),
                    _ = async {
                        loop {
                            match rx.changed().await {
                                Ok(()) => { if *rx.borrow() { return; } }
                                Err(_) => return,
                            }
                        }
                    } => Ok(None),
                }
            } else {
                paginate.await.map(Some)
            }
        }) {
            Ok(Some(reached_end)) => PaginateResult {
                ok: true,
                message: String::new(),
                reached_start: false,
                reached_end,
            },
            Ok(None) => PaginateResult {
                ok: false,
                message: "shutdown in progress".into(),
                reached_start: false,
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
    pub fn paginate_forward(&mut self, _room_id: &str, _count: u16) -> PaginateResult {
        PaginateResult {
            ok: false,
            message: "not in focused mode".into(),
            reached_start: false,
            reached_end: false,
        }
    }
}
