// MatrixRTC FFI methods on ClientFfi.

use super::{err, ok, ClientFfi};
use crate::ffi::RtcStartResult;
use crate::client::rtc::{RtcEventSink, RtcSession};
use parking_lot::Mutex;
use std::future::Future;
use std::sync::Arc;
use std::time::Duration;
use tokio::task::JoinHandle;

/// The teardown of a call that was hung up without waiting for it: the leave
/// events and the SFU disconnect run on the runtime while the UI moves on. At
/// most one is pending; whoever needs the old call fully gone (the next call,
/// a blocking hang-up, the client's drop) waits for it first.
#[derive(Default)]
pub(crate) struct PendingTeardown(Option<JoinHandle<()>>);

impl PendingTeardown {
    /// Run `teardown` on `rt`. A teardown still pending finishes first, so two
    /// of them never overlap.
    pub(crate) fn spawn<F>(&mut self, rt: &tokio::runtime::Handle, teardown: F)
    where
        F: Future<Output = ()> + Send + 'static,
    {
        let previous = self.0.take();
        self.0 = Some(rt.spawn(async move {
            wait_for(previous).await;
            teardown.await;
        }));
    }

    /// Hand the pending teardown to the caller, which awaits it with `wait_for`.
    pub(crate) fn take(&mut self) -> Option<JoinHandle<()>> {
        self.0.take()
    }

    /// Wait up to `limit` for the pending teardown.
    pub(crate) async fn finish(&mut self, limit: Duration) {
        if let Some(handle) = self.0.take() {
            if tokio::time::timeout(limit, handle).await.is_err() {
                tracing::warn!("rtc: the previous call's teardown is still running; not waiting");
            }
        }
    }
}

/// Wait for a teardown taken with `PendingTeardown::take`. A teardown that
/// panicked or was cancelled with its runtime has nothing left to wait for.
pub(crate) async fn wait_for(teardown: Option<JoinHandle<()>>) {
    if let Some(handle) = teardown {
        let _ = handle.await;
    }
}

/// How long a blocking hang-up waits for a call that is still being set up.
const STARTING_WAIT: Duration = Duration::from_secs(15);

/// Prefix of the `on_rtc_session_ended` reason of a call that could not be set
/// up; the rest is the message to show.
pub(crate) const START_FAILED: &str = "start_failed:";

/// What the call state needs from a live call.
pub(crate) trait CallMedia {
    fn mute_audio(&self, muted: bool);
    fn mute_video(&self, muted: bool);
}

impl CallMedia for RtcSession {
    fn mute_audio(&self, muted: bool) {
        RtcSession::mute_audio(self, muted);
    }

    fn mute_video(&self, muted: bool) {
        RtcSession::mute_video(self, muted);
    }
}

/// A call being set up on the runtime.
pub(crate) struct Starting {
    id: u64,
    /// The latest mute requests made before the call was up, applied to it
    /// when it is (the UI asks for them right after the start).
    audio_muted: Option<bool>,
    video_muted: Option<bool>,
    /// Finishes when the setup has returned.
    done: Option<JoinHandle<()>>,
}

/// The call of this client. Shared with the setup, which runs on the runtime
/// and installs the session when it is up.
pub(crate) enum CallState<S> {
    Idle,
    Starting(Starting),
    Active(Box<S>),
}

// Not derived: a derive would require `S: Default`.
#[allow(clippy::derivable_impls)]
impl<S> Default for CallState<S> {
    fn default() -> Self {
        Self::Idle
    }
}

pub(crate) type SharedCall = Arc<Mutex<CallState<RtcSession>>>;

impl<S: CallMedia> CallState<S> {
    /// Enter the setup of call `id`; returns the state it replaces.
    pub(crate) fn begin(&mut self, id: u64) -> Self {
        std::mem::replace(
            self,
            Self::Starting(Starting {
                id,
                audio_muted: None,
                video_muted: None,
                done: None,
            }),
        )
    }

    /// Leave the call: the state becomes idle, and the old one is returned.
    pub(crate) fn take(&mut self) -> Self {
        std::mem::take(self)
    }

    /// The live session and the wait for a setup still running.
    pub(crate) fn retire(self) -> (Option<Box<S>>, Option<JoinHandle<()>>) {
        match self {
            Self::Idle => (None, None),
            Self::Starting(s) => (None, s.done),
            Self::Active(session) => (Some(session), None),
        }
    }

    /// Leave a setup still running (the client is going away).
    pub(crate) fn take_starting(&mut self) -> Option<JoinHandle<()>> {
        match self {
            Self::Starting(_) => self.take().retire().1,
            _ => None,
        }
    }

    /// Record the wait for setup `id`; ignored when that setup is no longer
    /// the current one.
    pub(crate) fn watch_start(&mut self, id: u64, done: JoinHandle<()>) {
        if let Self::Starting(s) = self {
            if s.id == id {
                s.done = Some(done);
            }
        }
    }

    /// The setup `id` came up. Only the setup still current gets to be the
    /// call; a session whose call was hung up or replaced meanwhile is handed
    /// back for the caller to take down.
    pub(crate) fn install(&mut self, id: u64, session: S) -> Result<(), S> {
        match self {
            Self::Starting(s) if s.id == id => {
                if let Some(muted) = s.audio_muted {
                    session.mute_audio(muted);
                }
                if let Some(muted) = s.video_muted {
                    session.mute_video(muted);
                }
                *self = Self::Active(Box::new(session));
                Ok(())
            }
            _ => Err(session),
        }
    }

    /// The setup `id` failed. True when it was still the current one, so the
    /// UI has to be told; a hung-up or replaced setup failing is not news.
    pub(crate) fn fail(&mut self, id: u64) -> bool {
        match self {
            Self::Starting(s) if s.id == id => {
                *self = Self::Idle;
                true
            }
            _ => false,
        }
    }

    pub(crate) fn session(&self) -> Option<&S> {
        match self {
            Self::Active(session) => Some(session),
            _ => None,
        }
    }

    pub(crate) fn set_audio_muted(&mut self, muted: bool) {
        match self {
            Self::Active(session) => session.mute_audio(muted),
            Self::Starting(s) => s.audio_muted = Some(muted),
            Self::Idle => {}
        }
    }

    pub(crate) fn set_video_muted(&mut self, muted: bool) {
        match self {
            Self::Active(session) => session.mute_video(muted),
            Self::Starting(s) => s.video_muted = Some(muted),
            Self::Idle => {}
        }
    }
}

/// The setup of a call: whatever is still going down is gone first (a pending
/// hang-up, then the call that was active), then `start` runs. Two calls of one
/// account announce the same legacy member, so a late leave of the old one
/// would erase the new one's.
async fn start_after_teardown<S, T, End, EndFut, Start, StartFut>(
    pending: Option<JoinHandle<()>>,
    old: Option<Box<S>>,
    end: End,
    start: Start,
) -> T
where
    End: FnOnce(Box<S>) -> EndFut,
    EndFut: Future<Output = ()>,
    Start: FnOnce() -> StartFut,
    StartFut: Future<Output = T>,
{
    wait_for(pending).await;
    if let Some(old) = old {
        end(old).await;
    }
    start().await
}

/// A setup failed: if it is still the current call, it is over and the UI is
/// told why.
fn report_start_failure(state: &SharedCall, sink: Option<&dyn RtcEventSink>, id: u64, why: &str) {
    if !state.lock().fail(id) {
        return;
    }
    tracing::warn!("rtc: starting call {id} failed: {why}");
    if let Some(sink) = sink {
        sink.on_session_ended(id, &format!("{START_FAILED}{why}"));
    }
}

fn start_refused(message: impl Into<String>) -> RtcStartResult {
    RtcStartResult {
        ok: false,
        message: message.into(),
        session_id: 0,
    }
}

impl ClientFfi {
    /// Start a MatrixRTC call in `room_id` / `slot_id`. Returns at once with the
    /// session id every event of the call will carry: the old call's teardown,
    /// the connection and the announcements run on the runtime, so neither the
    /// UI thread nor the FFI lock waits for them. A call that cannot be set up
    /// ends with a session-ended event (see [`START_FAILED`]).
    pub fn rtc_start_call(&mut self, room_id: &str, slot_id: &str, audio_only: bool) -> RtcStartResult {
        // Install the rustls crypto provider once per process. livekit's TLS
        // stack uses rustls but cannot auto-select a provider when both
        // aws-lc-rs and ring are present in the dep tree; we pin aws-lc-rs to
        // match the rest of the codebase. Returns Err if already installed.
        let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();

        let Some(client) = self.client.clone() else {
            return start_refused("not logged in");
        };
        let http = self.http_client.clone();
        let room_id = room_id.to_owned();
        let slot_id = slot_id.to_owned();
        let handle = self.rt.handle().clone();
        let session_id = crate::client::rtc::session::next_session_id();
        // A call still active is ended before the new one starts, not when the
        // new session is stored (see `start_after_teardown`). A setup still
        // running is cancelled: it takes its own session down when it comes up.
        let (previous, stale_setup) = self.rtc_call.lock().begin(session_id).retire();
        if stale_setup.is_some() {
            self.rtc_teardown
                .spawn(&handle, async move { wait_for(stale_setup).await });
        }
        // A hang-up that did not wait for its teardown (`rtc_end_call`) is still
        // running: the same rule.
        let pending = self.rtc_teardown.take();
        let state = Arc::clone(&self.rtc_call);
        let sink = crate::client::rtc::sink_for_client(&client);
        let (finished_tx, finished_rx) = tokio::sync::oneshot::channel::<()>();
        let worker_handle = handle.clone();
        // block_on() drives the future on the *calling* thread (the Win32
        // message pump, ~1 MB stack), which overflows inside the
        // tungstenite WebSocket handshake state machine.  Run it instead
        // on a dedicated OS thread with a 16 MB stack; Handle::block_on
        // has no Send bound, so the non-Send MutexGuard in session.rs is
        // fine.  The handle keeps the same runtime for I/O and spawns.
        let spawned = std::thread::Builder::new()
            .stack_size(16 * 1024 * 1024)
            .spawn(move || {
                // Dropped when the thread ends, however it ends.
                let _finished = finished_tx;
                let ran = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    worker_handle.block_on(async {
                    let outcome = start_after_teardown(
                        pending,
                        previous,
                        |old| async move {
                            crate::client::rtc::session::end_call(&old).await;
                            drop(old);
                        },
                        || {
                            crate::client::rtc::session::start_call(
                                &client, &http, &room_id, &slot_id, audio_only, session_id,
                            )
                        },
                    )
                    .await;
                    match outcome {
                        Ok(session) => {
                            let rejected = state.lock().install(session_id, session);
                            if let Err(session) = rejected {
                                // Hung up (or replaced) while it connected.
                                crate::client::rtc::session::end_call(&session).await;
                                drop(session);
                            }
                        }
                        Err(e) => report_start_failure(&state, sink.as_deref(), session_id, &e.to_string()),
                    }
                    })
                }));
                if ran.is_err() {
                    // A panic in the setup is a failed call, not a crash of the client.
                    report_start_failure(&state, sink.as_deref(), session_id, "starting the call failed unexpectedly");
                }
            });
        if let Err(e) = spawned {
            let why = format!("could not start the call thread: {e}");
            self.rtc_call.lock().fail(session_id);
            return start_refused(why);
        }
        let done = handle.spawn(async move {
            let _ = finished_rx.await;
        });
        self.rtc_call.lock().watch_start(session_id, done);
        RtcStartResult {
            ok: true,
            message: "call starting".to_owned(),
            session_id,
        }
    }

    /// Gracefully leave the active call without waiting for it: the call is
    /// taken now (it is over for every other `rtc_*` method) and its leave
    /// events and SFU disconnect finish on the runtime. A call still being set
    /// up is cancelled and takes itself down once it is up. The next
    /// `rtc_start_call` waits for either.
    pub fn rtc_end_call(&mut self) {
        let (session, setup) = self.rtc_call.lock().take().retire();
        let handle = self.rt.handle().clone();
        if let Some(session) = session {
            self.rtc_teardown.spawn(&handle, async move {
                crate::client::rtc::session::end_call(&session).await;
                drop(session);
            });
        } else if setup.is_some() {
            self.rtc_teardown.spawn(&handle, async move { wait_for(setup).await });
        }
    }

    /// Gracefully leave the active call and return once the leave has gone out
    /// and the SFU connections are closed (bounded, a few seconds). For callers
    /// that tear the client or the runtime down right after.
    pub fn rtc_end_call_wait(&mut self) {
        let pending = self.rtc_teardown.take();
        let (session, setup) = self.rtc_call.lock().take().retire();
        self.rt.block_on(async move {
            wait_for(pending).await;
            if setup.is_some() && tokio::time::timeout(STARTING_WAIT, wait_for(setup)).await.is_err() {
                tracing::warn!("rtc: a call being set up is not done yet; not waiting");
            }
            if let Some(session) = session {
                crate::client::rtc::session::end_call(&session).await;
                drop(session);
            }
        });
    }

    /// Returns true iff the current user may join a MatrixRTC call in this room:
    /// the legacy `org.matrix.msc3401.call.member` state event (the sticky
    /// MatrixRTC events degrade to a legacy-only call). Reads cached
    /// m.room.power_levels — no network round-trip. Returns false on any
    /// uncertainty (not logged in, room unknown, PL unreadable).
    pub fn can_join_call_in_room(&self, room_id: &str) -> bool {
        self.call_permission(room_id, crate::client::rtc::mrtc_membership::may_join_call)
    }

    /// Returns true iff the current user may start a call in this room: join
    /// permission plus the legacy `org.matrix.msc3401.call` event. Same cached
    /// read and fail-closed behaviour as `can_join_call_in_room`.
    pub fn can_start_call_in_room(&self, room_id: &str) -> bool {
        self.call_permission(room_id, crate::client::rtc::mrtc_membership::may_start_call)
    }

    fn call_permission(
        &self,
        room_id: &str,
        check: fn(&matrix_sdk::ruma::events::room::power_levels::RoomPowerLevels, &matrix_sdk::ruma::UserId) -> bool,
    ) -> bool {
        use matrix_sdk::ruma::OwnedRoomId;

        let Some(client) = self.client.as_ref() else {
            return false;
        };
        let Ok(room_id_parsed) = room_id.parse::<OwnedRoomId>() else {
            return false;
        };
        let Some(room) = client.get_room(&room_id_parsed) else {
            return false;
        };
        let Some(user_id) = client.user_id() else {
            return false;
        };

        match self.rt.block_on(room.power_levels()) {
            Ok(pl) => check(&pl, user_id),
            Err(_) => false,
        }
    }

    /// Mute or unmute the local audio track.
    pub fn rtc_set_audio_muted(&self, muted: bool) {
        // mute() calls tokio::task::spawn, which
        // panics if there is no tokio runtime on the current thread. Enter
        // the runtime context so the spawn lands on our executor.
        let _guard = self.rt.enter();
        self.rtc_call.lock().set_audio_muted(muted);
    }

    /// Mute or unmute the local video track.
    pub fn rtc_set_video_muted(&self, muted: bool) {
        let _guard = self.rt.enter();
        self.rtc_call.lock().set_video_muted(muted);
    }

    /// Inject a raw I420 video frame into the live session.
    #[allow(clippy::too_many_arguments)]
    pub fn rtc_push_video_frame_i420(
        &self,
        y: &[u8],
        u: &[u8],
        v: &[u8],
        width: u32,
        height: u32,
        stride_y: u32,
        stride_u: u32,
        stride_v: u32,
    ) {
        // The first frame after an unmute request performs the deferred
        // unmute, which spawns onto the runtime (see rtc_set_audio_muted);
        // frames arrive on capture threads with no runtime context.
        let _guard = self.rt.enter();
        if let Some(session) = self.rtc_call.lock().session() {
            session.push_video_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
        }
    }

    /// Start publishing a local screen share track. Blocks until the LiveKit
    /// SDP round-trip completes so the caller can immediately begin pushing
    /// frames without racing against a None screen_source.
    pub fn rtc_start_screen_share(&mut self) -> crate::ffi::OpResult {
        let handle = self.rt.handle().clone();
        let call = self.rtc_call.lock();
        match &*call {
            CallState::Active(session) => match session.start_screen_share(handle) {
                Ok(()) => ok("screen share started"),
                Err(e) => err(e.to_string()),
            },
            CallState::Starting(_) => err("the call is still connecting"),
            CallState::Idle => err("no active call"),
        }
    }

    /// Stop the local screen share track. No-op when no call is active.
    pub fn rtc_stop_screen_share(&mut self) {
        let _guard = self.rt.enter();
        if let Some(session) = self.rtc_call.lock().session() {
            session.stop_screen_share();
        }
    }

    /// Inject a raw I420 screen frame into the live session. No-op when no call active.
    #[allow(clippy::too_many_arguments)]
    pub fn rtc_push_screen_frame_i420(
        &self,
        y: &[u8],
        u: &[u8],
        v: &[u8],
        width: u32,
        height: u32,
        stride_y: u32,
        stride_u: u32,
        stride_v: u32,
    ) {
        if let Some(session) = self.rtc_call.lock().session() {
            session.push_screen_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
        }
    }

    /// Decline an incoming MatrixRTC invite (`m.rtc.decline` referencing the
    /// invite event). Fire and forget: queued behind earlier declines, sent in
    /// order, each retried once; failures are logged (rate-limited).
    pub fn rtc_decline_invite_async(&self, room_id: &str, invite_event_id: &str) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let Ok(room_id) = room_id.parse::<matrix_sdk::ruma::OwnedRoomId>() else {
            return;
        };
        if invite_event_id.is_empty() {
            return;
        }
        let _enter = self.rt.enter();
        crate::client::rtc::invites::queue_decline(client, room_id, invite_event_id.to_owned());
    }
}

#[cfg(test)]
mod teardown_tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .unwrap()
    }

    #[test]
    fn spawn_returns_at_once_and_wait_for_sees_the_teardown_finish() {
        let rt = runtime();
        let done = Arc::new(AtomicBool::new(false));
        let mut pending = PendingTeardown::default();
        let flag = done.clone();
        pending.spawn(rt.handle(), async move {
            tokio::time::sleep(Duration::from_millis(150)).await;
            flag.store(true, Ordering::SeqCst);
        });
        // The caller was not held up.
        assert!(!done.load(Ordering::SeqCst));
        rt.block_on(wait_for(pending.take()));
        assert!(done.load(Ordering::SeqCst));
        // Nothing left to wait for.
        assert!(pending.take().is_none());
    }

    #[test]
    fn a_second_teardown_runs_after_the_first_has_finished() {
        let rt = runtime();
        let order = Arc::new(std::sync::Mutex::new(Vec::new()));
        let mut pending = PendingTeardown::default();
        let first = order.clone();
        pending.spawn(rt.handle(), async move {
            tokio::time::sleep(Duration::from_millis(100)).await;
            first.lock().unwrap().push(1);
        });
        let second = order.clone();
        pending.spawn(rt.handle(), async move {
            second.lock().unwrap().push(2);
        });
        rt.block_on(pending.finish(Duration::from_secs(5)));
        assert_eq!(*order.lock().unwrap(), vec![1, 2]);
    }

    #[test]
    fn finish_gives_up_after_its_limit() {
        let rt = runtime();
        let mut pending = PendingTeardown::default();
        pending.spawn(rt.handle(), async {
            tokio::time::sleep(Duration::from_secs(30)).await;
        });
        let t0 = std::time::Instant::now();
        rt.block_on(pending.finish(Duration::from_millis(50)));
        assert!(t0.elapsed() < Duration::from_secs(5));
    }

    #[test]
    fn a_panicking_teardown_does_not_block_the_next_call() {
        let rt = runtime();
        let mut pending = PendingTeardown::default();
        pending.spawn(rt.handle(), async {
            panic!("teardown failed");
        });
        // The next teardown still runs behind the failed one, and a waiter gets
        // through it. Without the guard in `wait_for` / `spawn` (which ignore
        // the panicked handle's error) the second would never run.
        let ran = Arc::new(AtomicBool::new(false));
        let flag = ran.clone();
        pending.spawn(rt.handle(), async move {
            flag.store(true, Ordering::SeqCst);
        });
        rt.block_on(async {
            tokio::time::timeout(Duration::from_secs(5), wait_for(pending.take()))
                .await
                .expect("a panicked teardown must not hang the waiter");
        });
        assert!(ran.load(Ordering::SeqCst), "the next teardown ran after the panicked one");
    }

    #[test]
    fn a_start_runs_only_after_the_pending_teardown_and_the_old_call_are_gone() {
        let rt = runtime();
        let log = Arc::new(std::sync::Mutex::new(Vec::<&'static str>::new()));
        let mut pending = PendingTeardown::default();
        let l = log.clone();
        pending.spawn(rt.handle(), async move {
            tokio::time::sleep(Duration::from_millis(150)).await;
            l.lock().unwrap().push("teardown");
        });
        let (l_end, l_start) = (log.clone(), log.clone());
        let out = rt.block_on(start_after_teardown(
            pending.take(),
            Some(Box::new(Fake::default())),
            |_old| async move {
                l_end.lock().unwrap().push("end old call");
            },
            || async move {
                l_start.lock().unwrap().push("start");
                7
            },
        ));
        assert_eq!(out, 7);
        assert_eq!(*log.lock().unwrap(), vec!["teardown", "end old call", "start"]);
    }

    #[test]
    fn a_client_waits_for_a_pending_teardown_before_it_goes_away() {
        let mut ffi = ClientFfi::new();
        let done = Arc::new(AtomicBool::new(false));
        let flag = done.clone();
        let handle = ffi.rt.handle().clone();
        ffi.rtc_teardown.spawn(&handle, async move {
            tokio::time::sleep(Duration::from_millis(200)).await;
            flag.store(true, Ordering::SeqCst);
        });
        assert!(!done.load(Ordering::SeqCst));
        drop(ffi);
        assert!(done.load(Ordering::SeqCst), "the drop waited for the leave");
    }

    #[test]
    fn starting_a_call_without_a_login_is_refused_with_no_session() {
        let mut ffi = ClientFfi::new();
        let r = ffi.rtc_start_call("!r:x", "call#default", true);
        assert!(!r.ok);
        assert_eq!(r.session_id, 0);
        assert!(ffi.rtc_call.lock().session().is_none());
    }

    #[derive(Default)]
    struct Fake {
        muted: std::sync::Mutex<Vec<String>>,
    }

    impl CallMedia for Fake {
        fn mute_audio(&self, muted: bool) {
            self.muted.lock().unwrap().push(format!("audio {muted}"));
        }

        fn mute_video(&self, muted: bool) {
            self.muted.lock().unwrap().push(format!("video {muted}"));
        }
    }

    #[test]
    fn a_setup_that_is_still_current_becomes_the_call_with_the_mutes_asked_meanwhile() {
        let mut state = CallState::<Fake>::default();
        state.begin(5);
        state.set_audio_muted(true);
        state.set_video_muted(false);
        state.set_audio_muted(false);
        state.set_audio_muted(true);
        assert!(state.session().is_none(), "not up yet");
        assert!(state.install(5, Fake::default()).is_ok());
        let log = state.session().unwrap().muted.lock().unwrap().clone();
        assert_eq!(log, ["audio true", "video false"], "only the latest request of each kind");
        // Live from now on: forwarded at once.
        state.set_video_muted(true);
        assert_eq!(state.session().unwrap().muted.lock().unwrap().last().unwrap(), "video true");
    }

    #[test]
    fn a_setup_that_was_hung_up_or_replaced_is_handed_back_to_be_taken_down() {
        let mut state = CallState::<Fake>::default();
        state.begin(1);
        // Hung up while connecting.
        let (live, _) = state.take().retire();
        assert!(live.is_none());
        assert!(state.install(1, Fake::default()).is_err());
        assert!(state.session().is_none());
        // Replaced by a newer start.
        state.begin(2);
        state.begin(3);
        assert!(state.install(2, Fake::default()).is_err(), "setup 2 is stale");
        assert!(state.install(3, Fake::default()).is_ok());
        // The call is up: a setup that finishes late cannot displace it.
        assert!(state.install(3, Fake::default()).is_err());
        assert!(state.session().is_some());
    }

    #[test]
    fn only_the_current_setup_failing_is_reported() {
        let mut state = CallState::<Fake>::default();
        state.begin(1);
        state.begin(2);
        assert!(!state.fail(1), "a replaced setup failing is not news");
        assert!(state.fail(2));
        assert!(!state.fail(2), "reported once");
        state.begin(3);
        let _ = state.take();
        assert!(!state.fail(3), "a hung-up setup failing is not news");
    }

    #[test]
    fn a_failed_start_tells_the_ui_with_the_session_id_and_the_reason() {
        #[derive(Default)]
        struct Sink(std::sync::Mutex<Vec<(u64, String)>>);
        impl RtcEventSink for Sink {
            fn on_invitation(&self, _: &str, _: &str, _: &str, _: &str, _: u64, _: &str) {}
            fn on_participant_joined(&self, _: u64, _: crate::client::rtc::RtcParticipantInfo) {}
            fn on_participant_left(&self, _: u64, _: &str) {}
            fn on_participant_updated(&self, _: u64, _: crate::client::rtc::RtcParticipantInfo) {}
            fn on_session_ended(&self, id: u64, reason: &str) {
                self.0.lock().unwrap().push((id, reason.to_owned()));
            }
            fn on_video_frame(&self, _: u64, _: &str, _: u32, _: u32, _: Vec<u8>) {}
            fn on_screen_frame(&self, _: u64, _: &str, _: u32, _: u32, _: Vec<u8>) {}
            fn on_audio_frame(&self, _: u64, _: &str, _: &[i16], _: u32, _: u32) {}
        }
        let state: SharedCall = Default::default();
        state.lock().begin(9);
        let sink = Sink::default();
        report_start_failure(&state, Some(&sink), 9, "room not found");
        report_start_failure(&state, Some(&sink), 9, "again");
        assert_eq!(*sink.0.lock().unwrap(), vec![(9, "start_failed:room not found".to_owned())]);
    }
}
