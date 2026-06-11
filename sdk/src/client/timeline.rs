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
use matrix_sdk::{ruma::OwnedRoomId, ruma::UInt, ruma::UserId, Client, Room};
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
use parking_lot::Mutex;
use std::sync::Arc;

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
pub(super) fn filter_for_channel(
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
    // Event id per slot, parallel to `visible`. Empty string for slots whose
    // converted event has no event id (e.g. virtual day-dividers / read
    // markers, local echoes whose id hasn't been assigned yet) or for slots
    // that are not visible. Used by `refresh_receipts` to verify shadow
    // alignment with matrix-sdk-ui's current `items()` before re-emitting.
    visible_ids: &mut Vec<String>,
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
                    visible_ids.push(ev.event_id.clone());
                    {
                        let g = handler.lock();
                        emit_inserted(&g, channel, room_id, idx, &ev);
                    }
                } else {
                    visible.push(false);
                    visible_ids.push(String::new());
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
                visible_ids.push(ev.event_id.clone());
                {
                    let g = handler.lock();
                    emit_inserted(&g, channel, room_id, idx, &ev);
                }
            } else {
                visible.push(false);
                visible_ids.push(String::new());
            }
        }
        VectorDiff::PushFront { value } => {
            let ev = filter_for_channel(
                timeline_item_to_ffi(&value, room_id, room, me).await,
                channel,
            );
            if let Some(ev) = ev {
                visible.insert(0, true);
                visible_ids.insert(0, ev.event_id.clone());
                {
                    let g = handler.lock();
                    emit_inserted(&g, channel, room_id, 0, &ev);
                }
            } else {
                visible.insert(0, false);
                visible_ids.insert(0, String::new());
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
                visible_ids.insert(index, ev.event_id.clone());
                {
                    let g = handler.lock();
                    emit_inserted(&g, channel, room_id, v_idx, &ev);
                }
            } else {
                visible.insert(index, false);
                visible_ids.insert(index, String::new());
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
                    if let Some(slot) = visible_ids.get_mut(index) {
                        *slot = ev.event_id.clone();
                    }
                    {
                        let g = handler.lock();
                        emit_updated(&g, channel, room_id, v_idx, &ev);
                    }
                }
                (false, Some(ev)) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Some(slot) = visible.get_mut(index) {
                        *slot = true;
                    }
                    if let Some(slot) = visible_ids.get_mut(index) {
                        *slot = ev.event_id.clone();
                    }
                    {
                        let g = handler.lock();
                        emit_inserted(&g, channel, room_id, v_idx, &ev);
                    }
                }
                (true, None) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Some(slot) = visible.get_mut(index) {
                        *slot = false;
                    }
                    if let Some(slot) = visible_ids.get_mut(index) {
                        *slot = String::new();
                    }
                    {
                        let g = handler.lock();
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
                if index < visible_ids.len() {
                    visible_ids.remove(index);
                }
                {
                    let g = handler.lock();
                    emit_removed(&g, channel, room_id, v_idx);
                }
            } else if index < visible.len() {
                visible.remove(index);
                if index < visible_ids.len() {
                    visible_ids.remove(index);
                }
            }
        }
        VectorDiff::Truncate { length } => {
            // Drop highest indices first so each visible-index we report
            // is still valid when the C++ side applies it.
            while visible.len() > length {
                let raw = visible.len() - 1;
                let was = visible[raw];
                visible.pop();
                visible_ids.pop();
                if was {
                    let v_idx = visible_len(visible);
                    {
                        let g = handler.lock();
                        emit_removed(&g, channel, room_id, v_idx);
                    }
                }
            }
        }
        VectorDiff::PopBack => {
            if let Some(was) = visible.pop() {
                visible_ids.pop();
                if was {
                    let v_idx = visible_len(visible);
                    {
                        let g = handler.lock();
                        emit_removed(&g, channel, room_id, v_idx);
                    }
                }
            }
        }
        VectorDiff::PopFront => {
            if !visible.is_empty() {
                let was = visible.remove(0);
                if !visible_ids.is_empty() {
                    visible_ids.remove(0);
                }
                if was {
                    {
                        let g = handler.lock();
                        emit_removed(&g, channel, room_id, 0);
                    }
                }
            }
        }
        VectorDiff::Clear => {
            visible.clear();
            visible_ids.clear();
            {
                let g = handler.lock();
                let empty: Vec<TimelineEvent> = Vec::new();
                emit_reset(&g, channel, room_id, &empty);
            }
        }
        VectorDiff::Reset { values } => {
            // Atomic snapshot replace. Build the new visibility mirror +
            // snapshot in one pass before the single `emit_reset`
            // call so the UI rebuilds in one shot.
            visible.clear();
            visible_ids.clear();
            visible.reserve(values.len());
            visible_ids.reserve(values.len());
            let mut snapshot: Vec<TimelineEvent> = Vec::new();
            for item in &values {
                let ev = filter_for_channel(
                    timeline_item_to_ffi(item, room_id, room, me).await,
                    channel,
                );
                if let Some(ev) = ev {
                    visible.push(true);
                    visible_ids.push(ev.event_id.clone());
                    snapshot.push(ev);
                } else {
                    visible.push(false);
                    visible_ids.push(String::new());
                }
            }
            {
                let g = handler.lock();
                emit_reset(&g, channel, room_id, &snapshot);
            }
        }
    }
}

// Re-emit `on_message_updated` for every visible event that carries a
// non-empty read-receipt list, using the streaming task's authoritative
// `visible` / `visible_ids` shadow to translate matrix-sdk-ui slot indices
// into the C++ row positions that the UI is currently rendering.
//
// Runs inline in the streaming task (not a separate spawn), so it cannot
// race a diff being applied to the C++ row vector — `tokio::select!` only
// runs one branch at a time. The shadow can still lag matrix-sdk-ui's
// internal vector when `tl.items().await` returns a snapshot newer than
// the most recently consumed `VectorDiff` (matrix-sdk-ui keeps producing
// diffs while we work); we detect any such drift by comparing event ids
// position-by-position and bail out on the first mismatch. The pending
// diffs in `stream` will deliver the necessary updates as they get
// processed, including the receipt details we skipped here.
#[cfg(not(test))]
async fn refresh_receipts(
    tl: &Arc<matrix_sdk_ui::Timeline>,
    visible: &[bool],
    visible_ids: &[String],
    handler: &Arc<Mutex<SendHandler>>,
    room_id: &str,
    room: &Room,
    me: Option<&UserId>,
    channel: &TimelineChannel,
    cancelled: &AtomicBool,
) {
    let items = tl.items().await;

    for (slot_idx, item) in items.iter().enumerate() {
        if cancelled.load(Ordering::Acquire) { return; }
        // matrix-sdk-ui's snapshot has more slots than our shadow — the
        // streaming task is behind on inserts. Let it catch up.
        if slot_idx >= visible.len() {
            return;
        }

        let ev = filter_for_channel(
            timeline_item_to_ffi(item, room_id, room, me).await,
            channel,
        );
        let was_visible_in_shadow = visible[slot_idx];
        let is_visible_now = ev.is_some();
        if was_visible_in_shadow != is_visible_now {
            // Visibility transition pending in the diff stream; bail out and
            // let the streaming task apply it.
            return;
        }
        let Some(ev) = ev else { continue; };

        // Shadow vs. items() event-id mismatch at the same slot means
        // matrix-sdk-ui has shifted items via Insert/Remove that the
        // streaming task hasn't yet processed. Stop emitting — any further
        // emit would land at the wrong C++ row.
        let shadow_id = &visible_ids[slot_idx];
        if !shadow_id.is_empty() && shadow_id != &ev.event_id {
            return;
        }

        if !ev.read_receipts.is_empty() {
            let v_idx = visible_index_of(visible, slot_idx);
            {
                let g = handler.lock();
                emit_updated(&g, channel, room_id, v_idx, &ev);
            }
        }
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
        let cancelled_stream = cancelled;

        // One-shot signal from the fetch_members task to the streaming task
        // saying "member sync is done, run the receipt-refresh pass". Routing
        // the refresh through the streaming task instead of running it on a
        // separate task is what prevents the duplicate-in-later-row bug:
        // `tokio::select!` only runs one branch at a time, so the refresh
        // computes its emit indices against the same `visible`/`visible_ids`
        // shadow that the diff loop maintains — they can't drift apart.
        let (members_done_tx, members_done_rx) =
            tokio::sync::mpsc::channel::<()>(1);

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
                let mut visible_ids: Vec<String> = Vec::with_capacity(initial_items.len());
                let mut snapshot: Vec<TimelineEvent> = Vec::new();
                for item in initial_items.iter() {
                    let ev = filter_for_channel(
                        timeline_item_to_ffi(item, &rid, &room_clone, me.as_deref()).await,
                        &ch,
                    );
                    if let Some(ev) = ev {
                        visible.push(true);
                        visible_ids.push(ev.event_id.clone());
                        snapshot.push(ev);
                    } else {
                        visible.push(false);
                        visible_ids.push(String::new());
                    }
                }
                if !cancelled_stream.load(Ordering::Acquire) {
                    {
                        let guard = h.lock();
                        emit_reset(&guard, &ch, &rid, &snapshot);
                    }
                }
                drop(snapshot);

                // Holds the receipt-refresh receiver until it has fired once;
                // we then take it to None so the select! branch resolves to
                // `pending()` forever, avoiding a busy-loop on the closed
                // channel.
                let mut members_done_rx: Option<tokio::sync::mpsc::Receiver<()>> =
                    Some(members_done_rx);

                loop {
                    if cancelled_stream.load(Ordering::Acquire) { break; }
                    tokio::select! {
                        // `biased`: drain diffs first so the shadow stays in
                        // step with C++ before any refresh can run.
                        biased;
                        maybe_diffs = stream.next() => {
                            let Some(diffs) = maybe_diffs else { break; };
                            for diff in diffs {
                                handle_timeline_diff(
                                    diff,
                                    &mut visible,
                                    &mut visible_ids,
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
                        sig = async {
                            match members_done_rx.as_mut() {
                                Some(rx) => rx.recv().await,
                                None => std::future::pending::<Option<()>>().await,
                            }
                        } => {
                            // Receipt refresh is one-shot — drop the receiver
                            // so subsequent select! iterations stay parked on
                            // the `pending()` arm.
                            members_done_rx = None;
                            if sig.is_some() {
                                refresh_receipts(
                                    &tl,
                                    &visible,
                                    &visible_ids,
                                    &h,
                                    &rid,
                                    &room_clone,
                                    me.as_deref(),
                                    &ch,
                                    &cancelled_stream,
                                )
                                .await;
                            }
                        }
                    }
                }
            })
            .abort_handle();

        // Backfill sender profiles. `matrix-sdk-ui`'s Timeline does not sync
        // member info on its own — `EventTimelineItem::sender_profile()`
        // stays at `TimelineDetails::Pending` for any user whose membership
        // state wasn't included in the initial sync's room delta, so their
        // messages render with an empty name + avatar. `fetch_members()`
        // runs `sync_members()` then patches every affected timeline item
        // in place, which the streaming task above picks up as
        // `VectorDiff::Set` and re-emits to C++. That covers items whose
        // *sender* profile was pending; items whose sender was already
        // resolved but whose read-receipt list referenced users with
        // missing profiles are handled by the receipt-refresh pass that
        // runs inside the streaming task after this signal.
        //
        // We spawn `fetch_members` separately so the initial timeline isn't
        // blocked behind a multi-second member sync on big rooms.
        let tl_for_members = Arc::clone(timeline);
        let fetch_abort = rt
            .spawn(async move {
                tl_for_members.fetch_members().await;
                let _ = members_done_tx.send(()).await;
            })
            .abort_handle();

        (abort, fetch_abort)
    }

    #[cfg(not(test))]
    pub fn subscribe_room(&self, room_id: &str) -> OpResult {
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

        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };

        // Reuse a still-live, non-focused timeline for this room instead of the
        // expensive rebuild. `Timeline::init_focus` (inside `build()`) is the
        // costly part — it collects the cached events into an `imbl::Vector`.
        // Keeping the built `Timeline` and only restarting its streaming task
        // re-emits the current items as a fresh reset to the (already-cleared by
        // begin_switch_loading) UI, then resumes diffs — so returning to a
        // recently-viewed room is instant. Focused (jump-to-event) or already
        // cancelled handles fall through to a clean rebuild below.
        {
            let mut guard = self.timelines.write();
            if let Some(existing) = guard.get_mut(&room_id) {
                if !existing.is_focused
                    && !existing.cancelled.load(Ordering::Acquire)
                {
                    // Cancel the OLD tasks' in-flight emissions before aborting:
                    // tokio abort is cooperative, so without this an old streaming
                    // task can emit one more VectorDiff (stale, pre-reset indices)
                    // that races the new task's reset and corrupts the rows. The
                    // respawned tasks get a fresh cancellation flag.
                    existing.cancelled.store(true, Ordering::Release);
                    for h in existing.abort_tasks.drain(..) {
                        h.abort();
                    }
                    let new_cancelled = Arc::new(AtomicBool::new(false));
                    let timeline = Arc::clone(&existing.timeline);
                    let (abort, fetch_abort) = Self::spawn_timeline_tasks(
                        &timeline,
                        &room,
                        room_id.to_string(),
                        &handler,
                        &client,
                        &self.rt,
                        TimelineChannel::Room,
                        Arc::clone(&new_cancelled),
                    );
                    existing.abort_tasks = vec![abort, fetch_abort];
                    existing.cancelled  = new_cancelled;
                    drop(guard);
                    self.sync_room_subscriptions();
                    let _ = self.subscribe_room_threads(room_id.as_str());
                    return ok("");
                }
            }
        }

        // Drop any previous (focused / cancelled) subscription for this room
        // before building a fresh live timeline.
        {
            let mut guard = self.timelines.write();
            if let Some(prev) = guard.remove(&room_id) {
                prev.cancelled.store(true, Ordering::Release);
                for h in prev.abort_tasks {
                    h.abort();
                }
            }
        }

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

        // No synchronous empty reset here: the UI clears the previous room's
        // rows itself the instant the user clicks (begin_switch_loading) and
        // holds a clean loading view, so emitting an empty reset would only add
        // a redundant blank frame. The single populated reset arrives from the
        // streaming task below (it emits before pumping any diff, preserving
        // ordering).

        let cancelled = Arc::new(AtomicBool::new(false));
        let (abort, fetch_abort) =
            Self::spawn_timeline_tasks(&timeline, &room, room_id_str, &handler, &client, &self.rt, TimelineChannel::Room, Arc::clone(&cancelled));

        self.timelines.write().insert(
            room_id.clone(),
            TimelineHandle {
                timeline,
                abort_tasks: vec![abort, fetch_abort],
                is_focused: false,
                cancelled,
            },
        );

        self.sync_room_subscriptions();
        // Auto-attach the ThreadListService: it drives the per-row "N
        // replies" chip via `apply_thread_chips` inside the watcher task,
        // and it's the same data source the threads panel consumes. Calling
        // it here (idempotent — replaces any prior subscription) means the
        // chip is live even when the user never opens the panel.
        let _ = self.subscribe_room_threads(room_id.as_str());
        ok("")
    }

    #[cfg(not(test))]
    pub fn unsubscribe_room(&self, room_id: &str) {
        if let Ok(id) = room_id.parse::<OwnedRoomId>() {
            if let Some(h) = self.timelines.write().remove(&id) {
                h.cancelled.store(true, Ordering::Release);
                for abort in h.abort_tasks {
                    abort.abort();
                }
            }
        }
        // Tear down the auto-attached ThreadListService watcher so it
        // stops poking the room timeline (which we just unsubscribed).
        self.unsubscribe_room_threads(room_id);
        self.sync_room_subscriptions();
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
            let guard = self.timelines.read();
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
    // Async (non-blocking) pagination
    // -----------------------------------------------------------------------

    /// Non-blocking paginate-back. Spawns the network call as a tokio task and
    /// delivers the result via `on_paginate_result(request_id, ok,
    /// reached_start, false, message)`. Frees the calling C++ thread
    /// immediately — no worker thread is pinned during the HTTP round-trip.
    #[cfg(not(test))]
    pub fn paginate_back_async(&self, request_id: u64, room_id: &str, count: u16) {
        let handler = self.handler.clone();
        let deliver = move |ok: bool, reached_start: bool, msg: &str| {
            if let Some(h) = &handler {
                {
                    let g = h.lock();
                    g.on_paginate_result(request_id, ok, reached_start, false, msg);
                }
            }
        };

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                deliver(false, false, &format!("invalid room id: {e}"));
                return;
            }
        };

        let tl = {
            let guard = self.timelines.read();
            let Some(handle) = guard.get(&room_id) else {
                deliver(false, false, "room not subscribed; call subscribe_room first");
                return;
            };
            Arc::clone(&handle.timeline)
        };
        let stop_rx = self.stop_rx.clone();
        let handler = self.handler.clone();

        self.rt.spawn(async move {
            let deliver = move |ok: bool, reached_start: bool, msg: &str| {
                if let Some(h) = &handler {
                    {
                        let g = h.lock();
                        g.on_paginate_result(request_id, ok, reached_start, false, msg);
                    }
                }
            };

            let result = async move {
                let paginate = tl.paginate_backwards(count);
                if let Some(mut rx) = stop_rx {
                    tokio::select! {
                        r = paginate => r.map(Some),
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
            }
            .await;

            match result {
                Ok(Some(reached_start)) => deliver(true, reached_start, ""),
                Ok(None) => deliver(false, false, "shutdown in progress"),
                Err(e) => deliver(false, false, &e.to_string()),
            }
        });
    }

    #[cfg(test)]
    pub fn paginate_back_async(&self, _request_id: u64, _room_id: &str, _count: u16) {}

    /// Non-blocking paginate-forward. Spawns the network call as a tokio task
    /// and delivers the result via `on_paginate_result(request_id, ok, false,
    /// reached_end, message)`. Frees the calling C++ thread immediately.
    #[cfg(not(test))]
    pub fn paginate_forward_async(&self, request_id: u64, room_id: &str, count: u16) {
        let handler = self.handler.clone();
        let deliver = move |ok: bool, reached_end: bool, msg: &str| {
            if let Some(h) = &handler {
                {
                    let g = h.lock();
                    g.on_paginate_result(request_id, ok, false, reached_end, msg);
                }
            }
        };

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                deliver(false, false, &format!("invalid room id: {e}"));
                return;
            }
        };

        let tl = {
            let guard = self.timelines.read();
            let Some(handle) = guard.get(&room_id) else {
                deliver(false, false, "room not subscribed; call subscribe_room_at first");
                return;
            };
            if !handle.is_focused {
                deliver(false, false, "not in focused mode");
                return;
            }
            Arc::clone(&handle.timeline)
        };
        let stop_rx = self.stop_rx.clone();
        let handler = self.handler.clone();

        self.rt.spawn(async move {
            let deliver = move |ok: bool, reached_end: bool, msg: &str| {
                if let Some(h) = &handler {
                    {
                        let g = h.lock();
                        g.on_paginate_result(request_id, ok, false, reached_end, msg);
                    }
                }
            };

            let result = async move {
                let paginate = tl.paginate_forwards(count);
                if let Some(mut rx) = stop_rx {
                    tokio::select! {
                        r = paginate => r.map(Some),
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
            }
            .await;

            match result {
                Ok(Some(reached_end)) => deliver(true, reached_end, ""),
                Ok(None) => deliver(false, false, "shutdown in progress"),
                Err(e) => deliver(false, false, &e.to_string()),
            }
        });
    }

    #[cfg(test)]
    pub fn paginate_forward_async(&self, _request_id: u64, _room_id: &str, _count: u16) {}

    // -----------------------------------------------------------------------
    // MSC3030 Jump to Date
    // -----------------------------------------------------------------------

    /// MSC3030: resolve a Unix millisecond timestamp to the nearest event ID
    /// in `room_id`. `dir` is `"f"` (forward) or `"b"` (backward).
    /// On success, `OpResult.message` holds the event ID string.
    #[cfg(not(test))]
    pub fn timestamp_to_event(&self, room_id: &str, ts_ms: u64, dir: &str) -> OpResult {
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
    pub fn timestamp_to_event(&self, _room_id: &str, _ts_ms: u64, _dir: &str) -> OpResult {
        err("not logged in")
    }

    /// MSC3030: subscribe to `room_id`'s timeline focused on `focus_event_id`.
    /// Tears down any previous subscription for this room, then builds a
    /// `TimelineFocus::Event` timeline. Fires `on_timeline_reset` + individual
    /// event callbacks identically to `subscribe_room`. Sets `is_focused = true`
    /// so that `paginate_forward` can gate itself.
    #[cfg(not(test))]
    pub fn subscribe_room_at(&self, room_id: &str, focus_event_id: &str) -> OpResult {
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
            let mut guard = self.timelines.write();
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
        {
            let guard = handler.lock();
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_timeline_reset(&room_id_str, &empty);
        }

        let cancelled = Arc::new(AtomicBool::new(false));
        let (abort, fetch_abort) =
            Self::spawn_timeline_tasks(&timeline, &room, room_id_str, &handler, &client, &self.rt, TimelineChannel::Room, Arc::clone(&cancelled));

        self.timelines.write().insert(
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
    pub fn subscribe_room_at(&self, _room_id: &str, _focus_event_id: &str) -> OpResult {
        err("not logged in")
    }

}
