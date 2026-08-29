//! Room-history export: walks a room's timeline back to its creation on an
//! isolated, detached focused timeline — never the live view's shared
//! `Timeline` — and writes the result to a plain-text or HTML document,
//! optionally with images and/or packaged as a `.zip`.
//!
//! ## Why an isolated timeline
//! `TimelineFocus::Event` (already used for MSC3030 jump-to-date in
//! `timeline.rs::subscribe_room_at`) builds a `Timeline` backed by
//! matrix-sdk-ui's `EventFocusedCache`, which — per that function's own doc
//! comment — is never persisted to the store and is entirely separate from
//! the shared, persisted `RoomEventCache` a *live* (`TimelineFocus::Live`)
//! timeline for the same room reads and writes. Never registering this
//! timeline in `self.timelines` (unlike `subscribe_room`/`subscribe_room_at`)
//! keeps it fully private to this export task: a concurrently open live
//! view of the same room shares no cursor, no cache, and no diff stream
//! with it.
//!
//! ## Why not the diff-streaming task
//! The live-view machinery in `timeline.rs` (`spawn_timeline_tasks`,
//! `emit_timeline_batch`) exists to push incremental UI updates and does
//! real per-event work (sender-profile resolution, search indexing) on
//! every diff. An export has no UI to update and no reason to fall behind
//! its own pagination the way that task can (see the doc comment on
//! `paginate_media_view_back_async` for a case where exactly that lag
//! caused a bug). Instead, each window calls `paginate_backwards` directly
//! and reads `Timeline::items()` — a synchronous, always-current snapshot —
//! once at the end of the window.
//!
//! ## Windowing
//! The walk proceeds in windows of `options.window_events` (or a default):
//! build a fresh focused timeline anchored at the current position, call
//! `paginate_backwards` repeatedly until the window fills or `reached_start`,
//! convert+write everything new (oldest-first — `items()` is always
//! chronological), persist a checkpoint, then **drop** the timeline before
//! starting the next, older window. This bounds peak memory to one window's
//! `Timeline` + one window's converted `TimelineEvent`s, regardless of room
//! size, and the window boundary (the oldest event written) doubles as the
//! resume checkpoint — there is no separate cursor concept, which matters
//! because the real matrix-sdk pagination token never crosses the FFI and
//! would not survive a process restart anyway.
//!
//! ## Known limitation (accepted, not engineered around)
//! On resume, the freshly built focused timeline's initial context window
//! (`num_context_events`) can re-surface the exact checkpointed boundary
//! event (guarded against below by seeding the dedup set with it) and,
//! rarely, its immediate newer neighbor. Worst case this duplicates one
//! line at the resume seam — accepted as a cosmetic imperfection rather
//! than adding cross-process dedup state, which would require persisting
//! every written event id rather than just the boundary.
//!
//! ## Event conversion
//! Reuses `timeline_convert::timeline_item_to_ffi` unmodified rather than a
//! parallel "lighter" converter — the extra per-event read-receipt/reaction
//! cost this accepts is outweighed by never having a second place that can
//! drift from how the live view renders the same event kinds.

mod format;
mod images;
mod labels;
mod segments;
pub(super) mod store;
mod zip;

use super::ClientFfi;

#[cfg(not(test))]
use crate::ffi::{RoomExportCheckpointFfi, RoomExportOptionsFfi, RoomExportProgressFfi};

#[cfg(not(test))]
use format::{AttachmentState, EventContext, ExportMeta, ExportSink, HtmlSink, TextSink};
#[cfg(not(test))]
use images::{ImageDescriptor, ImageOutcome};
#[cfg(not(test))]
use labels::Labels;
#[cfg(not(test))]
use segments::SegmentWriter;

#[cfg(not(test))]
use matrix_sdk::{
    ruma::{
        api::{client::room::get_event_by_timestamp::v1::Request as TimestampToEventRequest, Direction},
        MilliSecondsSinceUnixEpoch, OwnedEventId, OwnedRoomId, UInt,
    },
    Client, Room,
};
#[cfg(not(test))]
use matrix_sdk_ui::timeline::{
    RoomExt, TimelineEventFocusThreadMode, TimelineFocus, TimelineItemKind,
};

#[cfg(not(test))]
use std::collections::{HashMap, HashSet};
#[cfg(not(test))]
use std::path::PathBuf;
#[cfg(not(test))]
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(not(test))]
use std::sync::Arc;
#[cfg(not(test))]
use std::time::{SystemTime, UNIX_EPOCH};

/// Default window size when `RoomExportOptionsFfi::window_events == 0`.
#[cfg(not(test))]
const DEFAULT_WINDOW_EVENTS: u32 = 5000;
/// `paginate_backwards` batch size within a window.
#[cfg(not(test))]
const EXPORT_BATCH: u16 = 100;
/// Runaway guard: a server that never reports `reached_start` cannot spin
/// this forever. ~2000 windows × the default 5000 events/window covers a
/// 10M-event room, far beyond anything real.
#[cfg(not(test))]
const MAX_WINDOWS: u32 = 2000;
/// Concurrent image downloads per window (see `images.rs`'s doc comment for
/// why this is a plain semaphore rather than the interactive `PriorityGate`s).
#[cfg(not(test))]
const EXPORT_IMAGE_CONCURRENCY: usize = 2;

#[cfg(not(test))]
pub(crate) struct ExportHandle {
    pub(crate) abort: tokio::task::AbortHandle,
    pub(crate) cancel: Arc<AtomicBool>,
    pub(crate) stop: Arc<AtomicBool>,
}

#[cfg(not(test))]
struct ExportCtx {
    request_id: u64,
    room_id: OwnedRoomId,
    room: Room,
    client: Client,
    options: RoomExportOptionsFfi,
    handler: Option<Arc<Mutex<super::SendHandler>>>,
    app_cache_db: Arc<Mutex<Option<rusqlite::Connection>>>,
    cancel: Arc<AtomicBool>,
    stop: Arc<AtomicBool>,
    rt_handle: tokio::runtime::Handle,
}

#[cfg(not(test))]
use parking_lot::Mutex;

#[cfg(not(test))]
#[allow(clippy::too_many_arguments)]
fn emit_progress(ctx: &ExportCtx, events_written: u64, bytes_written: u64, oldest_ts_ms: u64,
                  newest_ts_ms: u64, room_created_ts_ms: u64, images: (u64, u64, u64),
                  reached_start: bool, finalizing: bool, assembly: (u64, u64)) {
    let Some(h) = &ctx.handler else { return };
    let progress = RoomExportProgressFfi {
        request_id: ctx.request_id,
        room_id: ctx.room_id.to_string(),
        events_written,
        bytes_written,
        oldest_ts_ms,
        newest_ts_ms,
        room_created_ts_ms,
        stop_at_ts_ms: ctx.options.stop_at_ts_ms,
        images_downloaded: images.0,
        images_skipped: images.1,
        images_failed: images.2,
        reached_start,
        finalizing,
        assembly_done: assembly.0,
        assembly_total: assembly.1,
    };
    let g = h.lock();
    g.on_room_export_progress(&progress);
}

#[cfg(not(test))]
fn emit_complete(
    handler: &Option<Arc<Mutex<super::SendHandler>>>,
    request_id: u64,
    ok: bool,
    cancelled: bool,
    reached_start: bool,
    out_path: &str,
    events_written: u64,
    bytes_written: u64,
    message: &str,
) {
    let Some(h) = handler else { return };
    let g = h.lock();
    g.on_room_export_complete(
        request_id,
        ok,
        cancelled,
        reached_start,
        out_path,
        events_written,
        bytes_written,
        message,
    );
}

/// cxx-bridge shared structs don't implement `Default` (unlike the
/// pure-Rust `#[cfg(test)]` stubs, which derive it) — this is the explicit
/// "no checkpoint" value `room_export_checkpoint` returns instead.
#[cfg(not(test))]
fn empty_checkpoint() -> RoomExportCheckpointFfi {
    RoomExportCheckpointFfi {
        exists: false,
        room_id: String::new(),
        out_path: String::new(),
        format: String::new(),
        oldest_event_id: String::new(),
        oldest_ts_ms: 0,
        events_written: 0,
        updated_at_secs: 0,
    }
}

#[cfg(not(test))]
fn sanitize_room_dir_name(room_id: &str) -> String {
    room_id
        .chars()
        .map(|c| match c {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' | '\0' => '_',
            c if c.is_control() => '_',
            c => c,
        })
        .collect()
}

#[cfg(not(test))]
impl ClientFfi {
    /// Starts a full-history export of `room_id`. Non-blocking: spawns a
    /// tokio task that walks an isolated focused timeline window by window,
    /// formats and writes the output itself, and reports via
    /// `on_room_export_progress` (throttled to roughly once per window) and
    /// exactly one `on_room_export_complete`. Refuses (delivering
    /// `ok=false` immediately) if any export — for this room or another —
    /// is already in flight; only one export runs app-wide at a time.
    pub fn start_room_export_async(&self, request_id: u64, room_id: &str, options: RoomExportOptionsFfi) {
        let handler = self.handler.clone();

        if options.format != "txt" && options.format != "html" {
            emit_complete(&handler, request_id, false, false, false, "", 0, 0,
                &format!("unsupported format {:?}", options.format));
            return;
        }
        let Some(client) = self.client.clone() else {
            emit_complete(&handler, request_id, false, false, false, "", 0, 0, "not logged in");
            return;
        };
        let room_id_owned: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => {
                emit_complete(&handler, request_id, false, false, false, "", 0, 0,
                    &format!("invalid room id: {e}"));
                return;
            }
        };
        let Some(room) = client.get_room(&room_id_owned) else {
            emit_complete(&handler, request_id, false, false, false, "", 0, 0, "room not found");
            return;
        };

        {
            let mut tasks = self.export_tasks.lock();
            tasks.retain(|_, h| !h.abort.is_finished());
            if !tasks.is_empty() {
                emit_complete(&handler, request_id, false, false, false, "", 0, 0,
                    "another export is already in progress");
                return;
            }
        }

        let cancel = Arc::new(AtomicBool::new(false));
        let stop = Arc::new(AtomicBool::new(false));
        let ctx = ExportCtx {
            request_id,
            room_id: room_id_owned.clone(),
            room,
            client,
            options,
            handler,
            app_cache_db: Arc::clone(&self.app_cache_db),
            cancel: Arc::clone(&cancel),
            stop: Arc::clone(&stop),
            rt_handle: self.rt.handle().clone(),
        };

        let join = self.rt.spawn(async move {
            run_export(ctx).await;
        });

        self.export_tasks.lock().insert(
            request_id,
            ExportHandle { abort: join.abort_handle(), cancel, stop },
        );
    }

    /// Cooperatively cancels an in-flight export: the running task notices
    /// the flag at its next check point, flushes what it has, persists a
    /// checkpoint, and — unlike `cancel_paginate_back` — DOES fire
    /// `on_room_export_complete(ok=true, cancelled=true, ...)` once it has
    /// torn down, since a cancelled export leaves real artifacts (a partial
    /// file, a checkpoint) worth reporting. No-op if `request_id` isn't a
    /// currently-running export.
    pub fn cancel_room_export(&self, request_id: u64) {
        if let Some(h) = self.export_tasks.lock().get(&request_id) {
            h.cancel.store(true, Ordering::Release);
        }
    }

    /// Cooperatively stops an in-flight export at its next safe point,
    /// finishing normally (full assembly/zip) with whatever was gathered so
    /// far — unlike `cancel_room_export`, which discards the in-flight
    /// document. No-op if `request_id` isn't a currently-running export.
    pub fn stop_room_export(&self, request_id: u64) {
        if let Some(h) = self.export_tasks.lock().get(&request_id) {
            h.stop.store(true, Ordering::Release);
        }
    }

    /// Last persisted checkpoint for `room_id`, or `exists: false` when
    /// none. Synchronous local SQLite read (same cost class as
    /// `load_media_backoff`) — call off the UI thread.
    pub fn room_export_checkpoint(&self, room_id: &str) -> RoomExportCheckpointFfi {
        let guard = self.app_cache_db.lock();
        let Some(conn) = guard.as_ref() else {
            return empty_checkpoint();
        };
        match store::query_checkpoint(conn, room_id) {
            Some(cp) => RoomExportCheckpointFfi {
                exists: true,
                room_id: cp.room_id,
                out_path: cp.out_path,
                format: cp.format,
                oldest_event_id: cp.oldest_event_id,
                oldest_ts_ms: cp.oldest_ts_ms,
                events_written: cp.events_written,
                updated_at_secs: cp.updated_at_secs,
            },
            None => empty_checkpoint(),
        }
    }

    /// Forgets a room's export checkpoint (user declined resume, or started
    /// a fresh export over an old one).
    pub fn clear_room_export_checkpoint(&self, room_id: &str) {
        let guard = self.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            store::delete_checkpoint(conn, room_id);
        }
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn start_room_export_async(&self, _request_id: u64, _room_id: &str, _options: crate::ffi::RoomExportOptionsFfi) {}
    pub fn cancel_room_export(&self, _request_id: u64) {}
    pub fn stop_room_export(&self, _request_id: u64) {}
    pub fn room_export_checkpoint(&self, _room_id: &str) -> crate::ffi::RoomExportCheckpointFfi {
        crate::ffi::RoomExportCheckpointFfi::default()
    }
    pub fn clear_room_export_checkpoint(&self, _room_id: &str) {}
}

/// Resolves the room's newest event id "as of now", and its timestamp, via
/// the same MSC3030 `/timestamp_to_event` call `timestamp_to_event` uses —
/// inlined (rather than calling that method) because it internally does
/// `self.rt.block_on`, which panics when called from a task already running
/// on that same runtime, exactly the situation this export task is in.
#[cfg(not(test))]
async fn resolve_newest_event(client: &Client, room_id: &OwnedRoomId) -> Result<(OwnedEventId, u64), String> {
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    let ts = UInt::try_from(now_ms)
        .map(MilliSecondsSinceUnixEpoch)
        .map_err(|_| "current timestamp out of range".to_string())?;
    let req = TimestampToEventRequest::new(room_id.clone(), ts, Direction::Backward);
    client
        .send(req)
        .await
        .map(|resp| (resp.event_id, resp.origin_server_ts.get().into()))
        .map_err(|e| e.to_string())
}

/// Best-effort resolution of a room's creation timestamp, from the local
/// state store's `m.room.create` event — never a network call. Unlike
/// `Room::create_content()` (which reads the minimal, persisted-summary
/// `RoomCreateWithCreatorEventContent` and drops `origin_server_ts`
/// entirely), this reads the raw state event to keep the timestamp. Mirrors
/// the `get_state_event_static` idiom already used to read `m.room.topic`'s
/// HTML block (`client/mod.rs`'s topic resolution). Returns `0` on any
/// failure (not yet synced, deserialize error, stripped state) — the
/// progress dialog already treats `room_created_ts_ms == 0` as "fall back
/// to indeterminate," so this must never fail the export itself.
#[cfg(not(test))]
async fn resolve_room_created_ts_ms(room: &Room) -> u64 {
    use matrix_sdk::deserialized_responses::SyncOrStrippedState;
    use matrix_sdk::ruma::events::{room::create::RoomCreateEventContent, SyncStateEvent};

    room.get_state_event_static::<RoomCreateEventContent>()
        .await
        .ok()
        .flatten()
        .and_then(|raw| raw.deserialize().ok())
        .and_then(|ev| {
            let SyncOrStrippedState::Sync(SyncStateEvent::Original(o)) = ev else {
                return None;
            };
            Some(o.origin_server_ts.get().into())
        })
        .unwrap_or(0)
}

/// Builds an isolated `TimelineFocus::Event` timeline anchored at
/// `focus_event_id`, on the blocking pool — mirrors `subscribe_room_at`'s
/// reasoning (the focused build's CPU-bound `imbl` collection would
/// otherwise run on one of this runtime's only 2 worker threads, next to
/// every other room's live-sync/diff-stream work). Never inserted into
/// `self.timelines`: this is precisely what keeps it isolated from any
/// live view of the same room.
#[cfg(not(test))]
async fn build_focused_timeline(
    room: &Room,
    focus_event_id: OwnedEventId,
    rt_handle: &tokio::runtime::Handle,
) -> Result<Arc<matrix_sdk_ui::Timeline>, String> {
    let room = room.clone();
    let inner_handle = rt_handle.clone();
    let result = rt_handle
        .spawn_blocking(move || {
            inner_handle.block_on(async move {
                room.timeline_builder()
                    .with_focus(TimelineFocus::Event {
                        target: focus_event_id,
                        num_context_events: 1,
                        thread_mode: TimelineEventFocusThreadMode::Automatic { hide_threaded_events: true },
                    })
                    .build()
                    .await
            })
        })
        .await;
    match result {
        Ok(Ok(t)) => Ok(Arc::new(t)),
        Ok(Err(e)) => Err(format!("build focused timeline: {e}")),
        Err(e) => Err(format!("build focused timeline task: {e}")),
    }
}

#[cfg(not(test))]
enum WindowOutcome {
    Continue { next_focus: OwnedEventId },
    ReachedStart,
    ReachedStop,
    /// The user clicked Stop: distinct from `ReachedStop` (a data-driven
    /// time-range cutoff) even though both take the same "finish and
    /// assemble what's gathered" path in `run_export`.
    Stopped,
    Cancelled,
    Failed(String),
}

/// Bridges one window's per-image `ImageOutcome`s (images.rs) into the
/// per-event lookup the write loop below needs to build a
/// `format::AttachmentState` for each event — owns its own `String` rather
/// than borrowing, since it outlives the `outcomes` vec it's built from.
#[cfg(not(test))]
enum MediaOutcomeForEvent {
    Saved(String),
    TooLarge,
}

/// Drops events older than `stop_at_ts_ms` from the front of an
/// oldest-first event vec (mutating in place), returning whether the
/// cutoff was crossed anywhere in this batch. `stop_at_ts_ms == 0` means
/// "no cutoff" — never crosses, vec left untouched. Not `#[cfg(not(test))]`
/// like the rest of this module, since it's pure `Vec` manipulation with no
/// timeline/client dependency — unlike `run_window`, safe (and worth) unit
/// testing directly.
fn apply_stop_at_ts_ms(events: &mut Vec<crate::ffi::TimelineEvent>, stop_at_ts_ms: u64) -> bool {
    if stop_at_ts_ms == 0 {
        return false;
    }
    let crossed = events.iter().any(|ev| ev.timestamp != 0 && ev.timestamp < stop_at_ts_ms);
    if crossed {
        // Find the first real (non-virtual) event at or after the cutoff
        // and drop everything before it — the too-old prefix, plus any
        // virtual items (date separators etc.) that fell within it.
        // `unwrap_or(len)` drops the whole batch when every real event in
        // it is too old.
        let keep_from = events
            .iter()
            .position(|ev| ev.timestamp != 0 && ev.timestamp >= stop_at_ts_ms)
            .unwrap_or(events.len());
        events.drain(0..keep_from);
    }
    crossed
}

#[cfg(test)]
mod apply_stop_at_ts_ms_tests {
    use super::apply_stop_at_ts_ms;
    use crate::ffi::TimelineEvent;

    fn ev(timestamp: u64) -> TimelineEvent {
        TimelineEvent { timestamp, ..Default::default() }
    }

    #[test]
    fn no_cutoff_leaves_vec_untouched() {
        let mut events = vec![ev(100), ev(200)];
        let crossed = apply_stop_at_ts_ms(&mut events, 0);
        assert!(!crossed);
        assert_eq!(events.len(), 2);
    }

    #[test]
    fn every_event_within_range_is_kept() {
        let mut events = vec![ev(500), ev(600), ev(700)];
        let crossed = apply_stop_at_ts_ms(&mut events, 400);
        assert!(!crossed);
        assert_eq!(events.len(), 3);
    }

    #[test]
    fn drops_the_too_old_prefix_and_keeps_the_in_range_suffix() {
        // Oldest-first: 100/200 predate the 400 cutoff, 500/600 don't.
        let mut events = vec![ev(100), ev(200), ev(500), ev(600)];
        let crossed = apply_stop_at_ts_ms(&mut events, 400);
        assert!(crossed);
        assert_eq!(events.iter().map(|e| e.timestamp).collect::<Vec<_>>(), vec![500, 600]);
    }

    #[test]
    fn every_event_older_than_cutoff_empties_the_vec() {
        let mut events = vec![ev(100), ev(200), ev(300)];
        let crossed = apply_stop_at_ts_ms(&mut events, 400);
        assert!(crossed);
        assert!(events.is_empty());
    }

    #[test]
    fn virtual_items_timestamp_zero_never_count_as_old_or_as_the_boundary() {
        // A virtual item (timestamp 0) sitting right at the boundary must
        // not be mistaken for the first in-range event — the boundary is
        // the next real event after it.
        let mut events = vec![ev(100), ev(0), ev(500)];
        let crossed = apply_stop_at_ts_ms(&mut events, 400);
        assert!(crossed);
        assert_eq!(events.iter().map(|e| e.timestamp).collect::<Vec<_>>(), vec![500]);
    }

    #[test]
    fn boundary_timestamp_exactly_at_cutoff_is_kept() {
        let mut events = vec![ev(300), ev(400)];
        let crossed = apply_stop_at_ts_ms(&mut events, 400);
        assert!(crossed);
        assert_eq!(events.iter().map(|e| e.timestamp).collect::<Vec<_>>(), vec![400]);
    }
}

/// Processes one window: builds its isolated timeline, paginates until the
/// window fills (or the room's start, or the cancel flag), converts and
/// writes every new event (oldest-first, as `items()` always is), runs the
/// image pass if requested, persists a checkpoint, then returns which
/// direction to continue in. The timeline is dropped when this returns —
/// that drop is the memory bound the whole windowed design exists for.
#[cfg(not(test))]
#[allow(clippy::too_many_arguments)]
async fn run_window(
    ctx: &ExportCtx,
    focus_event_id: OwnedEventId,
    seed_seen: Option<&str>,
    seen: &mut HashSet<String>,
    window_events: u32,
    sink: &dyn ExportSink,
    labels: &Labels,
    segment_writer: &mut SegmentWriter,
    media_dir: Option<&std::path::Path>,
    running_totals: &mut RunningTotals,
    room_created_ts_ms: u64,
    avatar_cache: &mut HashMap<String, Option<String>>,
    avatar_names_used: &mut HashSet<String>,
) -> WindowOutcome {
    if let Some(id) = seed_seen {
        seen.insert(id.to_string());
    }

    let timeline = match build_focused_timeline(&ctx.room, focus_event_id, &ctx.rt_handle).await {
        Ok(t) => t,
        Err(e) => return WindowOutcome::Failed(e),
    };

    let window_start_len = timeline.items().await.len();
    let mut reached_start = false;
    loop {
        if ctx.cancel.load(Ordering::Relaxed) {
            return WindowOutcome::Cancelled;
        }
        if ctx.stop.load(Ordering::Relaxed) {
            break;
        }
        match timeline.paginate_backwards(EXPORT_BATCH).await {
            Ok(true) => {
                reached_start = true;
                break;
            }
            Ok(false) => {}
            Err(e) => {
                // One retry, then give up on this window — a network drop
                // mid-round is the common case this guards against; the
                // checkpoint from the previous window means a re-run picks
                // up close to here regardless.
                tracing::warn!("history export: paginate round failed: {e}, retrying once");
                if ctx.cancel.load(Ordering::Relaxed) {
                    return WindowOutcome::Cancelled;
                }
                match timeline.paginate_backwards(EXPORT_BATCH).await {
                    Ok(true) => {
                        reached_start = true;
                        break;
                    }
                    Ok(false) => {}
                    Err(e2) => return WindowOutcome::Failed(e2.to_string()),
                }
            }
        }
        let items_snapshot = timeline.items().await;
        let len_now = items_snapshot.len();

        // Cheap approximate progress tick — no per-event conversion, no
        // async work, just a synchronous scan of the already-fetched
        // snapshot. Without this, a room whose whole history fits in one
        // window (up to window_events, default 5000) would report nothing
        // at all until the window — and possibly the entire export — is
        // already done, since the real, exact emit_progress() call only
        // fires once the window's events are converted and written. Each
        // round already implies a real network round-trip, so this is
        // naturally throttled without extra bookkeeping.
        //
        // Deliberately not attempting an approximate *count* here at all
        // (see the two dead-end attempts this replaced, both confirmed
        // wrong by direct instrumentation): `Timeline::items().len()`
        // reflects transient, not-yet-reconciled internal state while
        // pagination is actively running — observed climbing to 2x+ the
        // real, final per-window total, even after excluding virtual
        // items. It settles to the true count only once the loop below
        // exits and a fresh `items()` read is taken. The oldest loaded
        // timestamp doesn't have this problem — the oldest *real* event
        // seen so far stays valid regardless of whatever transient/
        // duplicate state briefly inflates the surrounding item count —
        // so it's the only thing this tick reports; the UI shows the date
        // without a message count until this window's authoritative one
        // is ready.
        let approx_oldest_from_this_round: Option<u64> = items_snapshot
            .iter()
            .find_map(|item| match item.kind() {
                TimelineItemKind::Event(e) => Some(e.timestamp().get().into()),
                TimelineItemKind::Virtual(_) => None,
            });
        let approx_oldest_ts_ms = approx_oldest_from_this_round.unwrap_or(running_totals.oldest_ts_ms);
        emit_progress(
            ctx,
            running_totals.events_written,
            running_totals.bytes_written,
            approx_oldest_ts_ms,
            running_totals.newest_ts_ms,
            room_created_ts_ms,
            (running_totals.images_downloaded, running_totals.images_skipped, running_totals.images_failed),
            false,
            false,
            (0, 0),
        );

        if len_now.saturating_sub(window_start_len) >= window_events as usize {
            break;
        }

        // Stop paginating as soon as the approximate oldest-so-far crosses
        // the requested cutoff, rather than always fetching a full window
        // — the exact truncation below still runs on whatever this
        // over-fetches by (pagination returns in EXPORT_BATCH-sized
        // rounds, so the overshoot is at most one round's worth).
        if ctx.options.stop_at_ts_ms != 0 {
            if let Some(t) = approx_oldest_from_this_round {
                if t < ctx.options.stop_at_ts_ms {
                    break;
                }
            }
        }
    }

    let items = timeline.items().await;
    let me = ctx.client.user_id().map(|u| u.to_owned());
    let room_id_str = ctx.room_id.to_string();

    let mut window_events_vec: Vec<crate::ffi::TimelineEvent> = Vec::new();
    for item in items.iter() {
        let real_id = match item.kind() {
            TimelineItemKind::Event(e) => e.event_id().map(|id| id.to_string()),
            TimelineItemKind::Virtual(_) => None,
        };
        if let Some(id) = &real_id {
            if seen.contains(id) {
                continue;
            }
        }
        let Some(ev) = super::timeline_convert::timeline_item_to_ffi(
            item,
            &room_id_str,
            &ctx.room,
            me.as_deref(),
        )
        .await
        else {
            continue;
        };
        if let Some(id) = real_id {
            seen.insert(id);
        }
        window_events_vec.push(ev);
    }

    // Optional stop-at-timestamp: once reached, everything *before* this
    // point in the (oldest-first) vec is excluded — those events are older
    // than the requested range — and the walk halts here.
    let hit_stop = apply_stop_at_ts_ms(&mut window_events_vec, ctx.options.stop_at_ts_ms);

    // Image pass: collect descriptors from this window only, so memory
    // stays bounded to one window regardless of room size.
    let mut media_outcomes: std::collections::HashMap<String, MediaOutcomeForEvent> = std::collections::HashMap::new();
    let mut images_downloaded = 0u64;
    let mut images_skipped = 0u64;
    let mut images_failed = 0u64;
    if let Some(dir) = media_dir {
        let mut descriptors = Vec::new();
        for ev in &window_events_vec {
            if !matches!(ev.msg_type.as_str(), "m.image" | "m.sticker") {
                continue;
            }
            let source = if !ev.source_encrypted_json.is_empty() {
                ev.source_encrypted_json.clone()
            } else {
                ev.source_url.clone()
            };
            if source.is_empty() {
                continue;
            }
            // MSC2530 explicit filename (rare) > file_name (m.file/m.audio;
            // never set for m.image) > body — for a plain m.image event
            // with no MSC2530 filename, `body` itself IS the filename
            // (e.g. "IMG_1234.jpg"), per timeline_convert.rs's handling of
            // MessageType::Image. Falling back straight to "file" here
            // (skipping body) was the bug: image_filename is empty for the
            // vast majority of images, so every export used the sanitized
            // event id as the name instead of the real filename.
            let original_name = if !ev.image_filename.is_empty() {
                ev.image_filename.clone()
            } else if !ev.file_name.is_empty() {
                ev.file_name.clone()
            } else {
                ev.body.clone()
            };
            descriptors.push(ImageDescriptor { event_id: ev.event_id.clone(), source, original_name });
        }
        if !descriptors.is_empty() {
            let outcomes = images::download_images(
                &ctx.client,
                dir,
                descriptors,
                EXPORT_IMAGE_CONCURRENCY,
                &ctx.cancel,
                &mut HashSet::new(),
            )
            .await;
            for (desc, outcome) in outcomes {
                match outcome {
                    ImageOutcome::Saved { rel_path } => {
                        images_downloaded += 1;
                        media_outcomes.insert(desc.event_id, MediaOutcomeForEvent::Saved(format!("media/{rel_path}")));
                    }
                    // A deliberate policy skip, not a technical failure —
                    // same progress bucket as a cancelled download.
                    ImageOutcome::TooLarge => {
                        images_skipped += 1;
                        media_outcomes.insert(desc.event_id, MediaOutcomeForEvent::TooLarge);
                    }
                    ImageOutcome::Skipped => images_skipped += 1,
                    ImageOutcome::Failed => images_failed += 1,
                }
            }
        }

        // Avatar pass: one fetch per not-yet-cached sender in this window
        // (cache persists across the whole export in `avatar_cache`, so
        // the same sender's photo is never re-downloaded window after
        // window). HTML-only, same "include_images" gate as the message
        // image pass above (both share the same `media_dir` presence
        // check).
        let mut avatar_descriptors = Vec::new();
        let mut seen_senders_this_window: HashSet<String> = HashSet::new();
        for ev in &window_events_vec {
            if ev.msg_type.starts_with("virtual.") || ev.msg_type == "m.room.member" {
                continue;
            }
            if avatar_cache.contains_key(&ev.sender) || !seen_senders_this_window.insert(ev.sender.clone()) {
                continue;
            }
            if ev.sender_avatar_url.is_empty() {
                avatar_cache.insert(ev.sender.clone(), None);
                continue;
            }
            avatar_descriptors.push(ImageDescriptor {
                event_id: ev.sender.clone(),
                source: ev.sender_avatar_url.clone(),
                original_name: if !ev.sender_name.is_empty() { ev.sender_name.clone() } else { ev.sender.clone() },
            });
        }
        if !avatar_descriptors.is_empty() {
            let outcomes = images::download_images(
                &ctx.client,
                dir,
                avatar_descriptors,
                EXPORT_IMAGE_CONCURRENCY,
                &ctx.cancel,
                avatar_names_used,
            )
            .await;
            for (desc, outcome) in outcomes {
                let path = match outcome {
                    ImageOutcome::Saved { rel_path } => Some(format!("media/{rel_path}")),
                    ImageOutcome::TooLarge | ImageOutcome::Skipped | ImageOutcome::Failed => None,
                };
                avatar_cache.insert(desc.event_id, path);
            }
        }
    }

    if let Err(e) = segment_writer.begin_segment() {
        return WindowOutcome::Failed(e.to_string());
    }

    let mut oldest_written_id: Option<String> = None;
    let mut oldest_written_ts = running_totals.oldest_ts_ms;
    let mut prev: Option<&crate::ffi::TimelineEvent> = None;
    for ev in &window_events_vec {
        let attachment = match media_outcomes.get(&ev.event_id) {
            Some(MediaOutcomeForEvent::Saved(path)) => AttachmentState::Saved(path.as_str()),
            Some(MediaOutcomeForEvent::TooLarge) => AttachmentState::TooLarge,
            None => AttachmentState::None,
        };
        let avatar_path = avatar_cache.get(&ev.sender).and_then(|p| p.as_deref());
        let event_ctx = EventContext { prev, avatar_path };
        let line = sink.event(ev, &event_ctx, attachment, labels);
        prev = Some(ev);
        if let Err(e) = segment_writer.write(&line) {
            return WindowOutcome::Failed(e.to_string());
        }
        // Virtual items (date dividers, read markers, the timeline-start
        // marker — matrix-sdk-ui's `VirtualTimelineItem`, converted
        // unconditionally by `timeline_item_to_ffi`) render as an empty
        // line via `sink.event`'s `msg_type.starts_with("virtual.")`
        // check, but were still being counted here as if they were real
        // messages — inflating `events_written` well past the room's
        // actual message count (a room with sparse history over a long
        // span can have nearly as many date dividers as messages).
        let is_virtual = ev.msg_type.starts_with("virtual.");
        if !is_virtual {
            running_totals.events_written += 1;
            running_totals.bytes_written += line.len() as u64;
            if running_totals.newest_ts_ms == 0 {
                running_totals.newest_ts_ms = ev.timestamp;
            }
        }
        // `window_events_vec` is oldest-first, so the *first* real event
        // seen here is the oldest one in this window — only that one
        // should ever set these, not every event (which would leave the
        // window's newest event behind instead).
        if oldest_written_id.is_none() && !is_virtual && !ev.event_id.is_empty() {
            oldest_written_id = Some(ev.event_id.clone());
            oldest_written_ts = ev.timestamp;
        }
    }
    running_totals.oldest_ts_ms = oldest_written_ts;
    running_totals.images_downloaded += images_downloaded;
    running_totals.images_skipped += images_skipped;
    running_totals.images_failed += images_failed;

    if let Err(e) = segment_writer.finish_segment() {
        return WindowOutcome::Failed(e.to_string());
    }

    if let Some(oldest_id) = &oldest_written_id {
        let now_secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
        let cp = store::Checkpoint {
            room_id: ctx.room_id.to_string(),
            out_path: ctx.options.out_path.clone(),
            format: ctx.options.format.clone(),
            oldest_event_id: oldest_id.clone(),
            oldest_ts_ms: running_totals.oldest_ts_ms,
            events_written: running_totals.events_written,
            segments_written: segment_writer.segments_written(),
            updated_at_secs: now_secs,
            stop_at_ts_ms: ctx.options.stop_at_ts_ms,
        };
        let guard = ctx.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            store::upsert_checkpoint(conn, &cp);
        }
    }

    emit_progress(
        ctx,
        running_totals.events_written,
        running_totals.bytes_written,
        running_totals.oldest_ts_ms,
        running_totals.newest_ts_ms,
        room_created_ts_ms,
        (running_totals.images_downloaded, running_totals.images_skipped, running_totals.images_failed),
        reached_start,
        false,
        (0, 0),
    );

    drop(timeline);

    if ctx.cancel.load(Ordering::Relaxed) {
        WindowOutcome::Cancelled
    } else if reached_start {
        WindowOutcome::ReachedStart
    } else if hit_stop {
        WindowOutcome::ReachedStop
    } else if ctx.stop.load(Ordering::Relaxed) {
        WindowOutcome::Stopped
    } else if let Some(next) = oldest_written_id {
        match next.parse::<OwnedEventId>() {
            Ok(id) => WindowOutcome::Continue { next_focus: id },
            Err(e) => WindowOutcome::Failed(format!("invalid event id from timeline: {e}")),
        }
    } else {
        // No new events were written and we haven't reached the start —
        // avoid spinning forever on a window that made no progress.
        WindowOutcome::Failed("export made no progress in this window".to_string())
    }
}

#[cfg(not(test))]
#[derive(Default)]
struct RunningTotals {
    events_written: u64,
    bytes_written: u64,
    oldest_ts_ms: u64,
    newest_ts_ms: u64,
    images_downloaded: u64,
    images_skipped: u64,
    images_failed: u64,
}

#[cfg(not(test))]
async fn run_export(mut ctx: ExportCtx) {
    let room_id_str = ctx.room_id.to_string();

    // Best-effort: the isolated timeline's cache is never revisited by the
    // passive redecryptor, so keys must land before the events do (same
    // reasoning as `subscribe_room_at`).
    if let Err(e) = ctx.client.encryption().backups().download_room_keys_for_room(ctx.room.room_id()).await {
        tracing::warn!("history export: failed to download backup room keys: {e}");
    }

    let checkpoint = {
        let guard = ctx.app_cache_db.lock();
        guard.as_ref().and_then(|conn| store::query_checkpoint(conn, &room_id_str))
    };

    // Resolved once, up front, for both the fresh and resumed paths below —
    // a local store read, so cheap enough to always attempt. `0` on failure
    // is not an error case: the progress dialog already falls back to an
    // indeterminate spinner whenever this is `0`.
    let room_created_ts_ms = resolve_room_created_ts_ms(&ctx.room).await;

    let (mut focus_id, mut seen, mut segment_writer, mut totals, mut seed_for_first_window) = 'setup: {
        if let Some(cp) = checkpoint.filter(|cp| cp.format == ctx.options.format && cp.out_path == ctx.options.out_path) {
            let focus: OwnedEventId = match cp.oldest_event_id.parse() {
                Ok(id) => id,
                Err(e) => {
                    emit_complete(&ctx.handler, ctx.request_id, false, false, false, "", 0, 0,
                        &format!("invalid checkpoint event id: {e}"));
                    return;
                }
            };
            // Silently reapply the original run's time-range cutoff rather
            // than trusting whatever the caller's fresh options carry — the
            // UI doesn't re-prompt for a range on resume, so this is the
            // only place the original bound survives.
            ctx.options.stop_at_ts_ms = cp.stop_at_ts_ms;
            let staging = staging_dir(&ctx);
            let writer = match SegmentWriter::resume(&staging.join("segments"), cp.segments_written) {
                Ok(w) => w,
                Err(e) => {
                    emit_complete(&ctx.handler, ctx.request_id, false, false, false, "", 0, 0,
                        &format!("failed to resume export: {e}"));
                    return;
                }
            };
            // Best-effort: the checkpoint doesn't persist `newest_ts_ms`, so
            // re-resolve "current newest" for the progress fraction's fixed
            // reference point. A failure here isn't fatal to the resume —
            // it just leaves the progress bar indeterminate, same as before
            // this fix.
            let newest_ts_ms = resolve_newest_event(&ctx.client, &ctx.room_id)
                .await
                .map(|(_, ts)| ts)
                .unwrap_or(0);
            let totals = RunningTotals {
                events_written: cp.events_written,
                oldest_ts_ms: cp.oldest_ts_ms,
                newest_ts_ms,
                ..Default::default()
            };
            break 'setup (focus, HashSet::new(), writer, totals, Some(cp.oldest_event_id));
        }

        let (focus, newest_ts_ms) = if !ctx.options.resume_from_event_id.is_empty() {
            let focus = match ctx.options.resume_from_event_id.parse::<OwnedEventId>() {
                Ok(id) => id,
                Err(e) => {
                    emit_complete(&ctx.handler, ctx.request_id, false, false, false, "", 0, 0,
                        &format!("invalid resume_from_event_id: {e}"));
                    return;
                }
            };
            // Same best-effort re-resolution as the checkpoint path above.
            let newest_ts_ms = resolve_newest_event(&ctx.client, &ctx.room_id)
                .await
                .map(|(_, ts)| ts)
                .unwrap_or(0);
            (focus, newest_ts_ms)
        } else {
            match resolve_newest_event(&ctx.client, &ctx.room_id).await {
                Ok((id, ts)) => (id, ts),
                Err(e) => {
                    emit_complete(&ctx.handler, ctx.request_id, false, false, false, "", 0, 0,
                        &format!("failed to resolve starting point: {e}"));
                    return;
                }
            }
        };
        let staging = staging_dir(&ctx);
        let writer = match SegmentWriter::create(&staging.join("segments")) {
            Ok(w) => w,
            Err(e) => {
                emit_complete(&ctx.handler, ctx.request_id, false, false, false, "", 0, 0,
                    &format!("failed to start export: {e}"));
                return;
            }
        };
        let seed = if !ctx.options.resume_from_event_id.is_empty() {
            Some(ctx.options.resume_from_event_id.clone())
        } else {
            None
        };
        let totals = RunningTotals { newest_ts_ms, ..Default::default() };
        (focus, HashSet::new(), writer, totals, seed)
    };

    let window_events = if ctx.options.window_events == 0 { DEFAULT_WINDOW_EVENTS } else { ctx.options.window_events };
    let labels = Labels::new(ctx.options.labels.clone());
    let sink: Box<dyn ExportSink> = if ctx.options.format == "html" { Box::new(HtmlSink) } else { Box::new(TextSink) };

    let staging = staging_dir(&ctx);
    let media_dir = staging.join("media");
    let include_media_dir = if ctx.options.include_images && ctx.options.format == "html" {
        Some(media_dir.as_path())
    } else {
        None
    };

    let mut outcome_reached_start = false;
    // A time-range cutoff (`stop_at_ts_ms`) being hit is a *complete*
    // export of exactly what was requested, not a partial one — treated
    // the same as `outcome_reached_start` for checkpoint cleanup below, so
    // a later export of the same room doesn't wrongly present a
    // successful, deliberately-bounded export as "interrupted." Distinct
    // from `WindowOutcome::Stopped` (the user's Stop button), which *is*
    // genuinely partial and keeps its checkpoint on purpose.
    let mut outcome_reached_stop = false;
    let mut outcome_failed: Option<String> = None;
    let mut outcome_cancelled = false;

    // Persist for the whole export (unlike `media_outcomes`, which is
    // per-window): the same sender's avatar must never be re-fetched
    // window after window, and filename collision-avoidance for avatars
    // must hold across windows too (see `images.rs`'s doc comment).
    let mut avatar_cache: HashMap<String, Option<String>> = HashMap::new();
    let mut avatar_names_used: HashSet<String> = HashSet::new();

    for _ in 0..MAX_WINDOWS {
        let result = run_window(
            &ctx,
            focus_id.clone(),
            seed_for_first_window.as_deref(),
            &mut seen,
            window_events,
            sink.as_ref(),
            &labels,
            &mut segment_writer,
            include_media_dir,
            &mut totals,
            room_created_ts_ms,
            &mut avatar_cache,
            &mut avatar_names_used,
        )
        .await;
        seed_for_first_window = None;

        match result {
            WindowOutcome::Continue { next_focus } => focus_id = next_focus,
            WindowOutcome::ReachedStart => {
                outcome_reached_start = true;
                break;
            }
            WindowOutcome::ReachedStop => {
                outcome_reached_stop = true;
                break;
            }
            WindowOutcome::Stopped => break,
            WindowOutcome::Cancelled => {
                outcome_cancelled = true;
                break;
            }
            WindowOutcome::Failed(e) => {
                outcome_failed = Some(e);
                break;
            }
        }
    }

    if let Some(err_msg) = outcome_failed {
        let _ = segment_writer.cleanup();
        emit_complete(&ctx.handler, ctx.request_id, false, false, false, "", totals.events_written, totals.bytes_written, &err_msg);
        return;
    }

    if outcome_cancelled {
        emit_complete(&ctx.handler, ctx.request_id, true, true, false, "", totals.events_written, totals.bytes_written, "");
        return;
    }

    // Assembly (concatenating segments, then zipping if requested) has a
    // known total up front — unlike pagination, whose total event count
    // can never be known in advance — so it gets real, incrementing
    // progress instead of a single opaque "finalizing" tick. Build the
    // zip entry list (if any) before concatenation starts so its length is
    // already known for `assembly_total`.
    let doc_name = format!("history.{}", sink.extension());
    let assembled_path = staging.join(&doc_name);
    let segment_total = segment_writer.segments_written() as u64;
    let mut zip_entries: Vec<zip::ZipEntry> = Vec::new();
    if ctx.options.zip_output {
        zip_entries.push(zip::ZipEntry { source: assembled_path.clone(), archive_path: doc_name.clone() });
        if media_dir.is_dir() {
            if let Ok(rd) = std::fs::read_dir(&media_dir) {
                for entry in rd.flatten() {
                    let path = entry.path();
                    if path.is_file() {
                        let name = entry.file_name().to_string_lossy().to_string();
                        zip_entries.push(zip::ZipEntry { source: path, archive_path: format!("media/{name}") });
                    }
                }
            }
        }
    }
    let assembly_total = segment_total + if ctx.options.zip_output { zip_entries.len() as u64 } else { 0 };

    // One explicit tick announcing the switch to local assembly, before
    // any of it has happened yet — without this the UI would otherwise sit
    // on the last pagination-round tick, looking stalled, for however long
    // it takes the first real assembly tick to arrive.
    emit_progress(
        &ctx,
        totals.events_written,
        totals.bytes_written,
        totals.oldest_ts_ms,
        totals.newest_ts_ms,
        room_created_ts_ms,
        (totals.images_downloaded, totals.images_skipped, totals.images_failed),
        outcome_reached_start,
        true,
        (0, assembly_total),
    );

    // Assemble the final document.
    let meta = ExportMeta {
        room_name: ctx.room.name().unwrap_or_else(|| room_id_str.clone()),
        exported_at_ms: SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0),
    };
    let header = sink.header(&meta, &labels);
    // `complete` here means "the document has everything it was asked
    // for," not "reached the room's true start" — a satisfied time-range
    // cutoff is just as complete as truly reaching the start.
    let footer = sink.footer(&labels, outcome_reached_start || outcome_reached_stop, totals.events_written);
    let concat_result = segment_writer.concatenate_reverse(&assembled_path, &header, &footer, |done| {
        emit_progress(&ctx, totals.events_written, totals.bytes_written, totals.oldest_ts_ms,
            totals.newest_ts_ms, room_created_ts_ms,
            (totals.images_downloaded, totals.images_skipped, totals.images_failed),
            outcome_reached_start, true, (done as u64, assembly_total));
    });
    if let Err(e) = concat_result {
        let _ = segment_writer.cleanup();
        emit_complete(&ctx.handler, ctx.request_id, false, false, outcome_reached_start, "", totals.events_written, totals.bytes_written,
            &format!("failed to assemble export: {e}"));
        return;
    }

    let out_dir = PathBuf::from(&ctx.options.out_path);
    let final_path = if ctx.options.zip_output {
        let zip_path = out_dir.join("history.zip");
        let zip_result = zip::write_zip(&zip_path, &zip_entries, |done| {
            emit_progress(&ctx, totals.events_written, totals.bytes_written, totals.oldest_ts_ms,
                totals.newest_ts_ms, room_created_ts_ms,
                (totals.images_downloaded, totals.images_skipped, totals.images_failed),
                outcome_reached_start, true, (segment_total + done as u64, assembly_total));
        });
        match zip_result {
            Ok(()) => Some(zip_path),
            Err(e) => {
                emit_complete(&ctx.handler, ctx.request_id, false, false, outcome_reached_start, "", totals.events_written, totals.bytes_written,
                    &format!("failed to package zip: {e}"));
                return;
            }
        }
    } else {
        if let Err(e) = std::fs::create_dir_all(&out_dir) {
            emit_complete(&ctx.handler, ctx.request_id, false, false, outcome_reached_start, "", totals.events_written, totals.bytes_written,
                &format!("failed to create output folder: {e}"));
            return;
        }
        let dest_doc = out_dir.join(&doc_name);
        if let Err(e) = std::fs::rename(&assembled_path, &dest_doc) {
            emit_complete(&ctx.handler, ctx.request_id, false, false, outcome_reached_start, "", totals.events_written, totals.bytes_written,
                &format!("failed to write output file: {e}"));
            return;
        }
        if media_dir.is_dir() {
            let dest_media = out_dir.join("media");
            let _ = std::fs::create_dir_all(&dest_media);
            if let Ok(rd) = std::fs::read_dir(&media_dir) {
                for entry in rd.flatten() {
                    let path = entry.path();
                    if path.is_file() {
                        let _ = std::fs::rename(&path, dest_media.join(entry.file_name()));
                    }
                }
            }
        }
        Some(dest_doc)
    };

    let _ = segment_writer.cleanup();
    let _ = std::fs::remove_dir_all(&staging);

    if outcome_reached_start || outcome_reached_stop {
        let guard = ctx.app_cache_db.lock();
        if let Some(conn) = guard.as_ref() {
            store::delete_checkpoint(conn, &room_id_str);
        }
    }

    let out_path_str = final_path.map(|p| p.to_string_lossy().to_string()).unwrap_or_default();
    emit_complete(&ctx.handler, ctx.request_id, true, false, outcome_reached_start, &out_path_str, totals.events_written, totals.bytes_written, "");
}

/// Scratch directory for one room's in-progress export: segment files, the
/// assembled-but-not-yet-placed document, and (when requested) downloaded
/// images. Lives *inside* the user's chosen destination folder (not some
/// hidden app-data directory) specifically so there's visible evidence of
/// progress there while a large export runs, rather than an empty folder
/// until the very end. Named after the room so a second, unrelated export
/// that happens to reuse the same destination folder can't collide with a
/// leftover temp directory. Matches the checkpoint/resume gating, which
/// already requires `out_path` to match exactly — so resuming naturally
/// finds this same location as long as the user picks the same folder
/// again, which the resume UI asks them to do anyway.
#[cfg(not(test))]
fn staging_dir(ctx: &ExportCtx) -> PathBuf {
    PathBuf::from(&ctx.options.out_path)
        .join(format!(".tesseract-export-tmp-{}", sanitize_room_dir_name(&ctx.room_id.to_string())))
}
