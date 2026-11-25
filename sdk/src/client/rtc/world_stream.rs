//! The 3D call world's pose stream: which incoming packets and tracks are accepted, the
//! latest-wins sender, and the data-track-or-packets transport choice. See
//! `docs/3D-DATA-STREAMS.md`.

use std::{
    future::Future,
    pin::Pin,
    sync::{Arc, Mutex},
};

use livekit::{
    data_track::{DataTrackFrame, LocalDataTrack, PushFrameErrorReason},
    DataPacket, Room,
};
use tokio::sync::Notify;
use tokio::task::AbortHandle;
use tracing::{debug, info};

use super::WORLD_CAPABILITY;

/// Largest payload sent or accepted, in bytes.
pub const MAX_WORLD_PAYLOAD: usize = 1024;

/// Whether a received data packet is a world frame we take.
///
/// `call_e2ee` is the connection's encryption setting; in an encrypted call the sender's
/// sticky `is_encrypted()` status must hold (a plaintext packet turns it false for good).
pub fn accept_packet(
    topic: Option<&str>,
    payload_len: usize,
    call_e2ee: bool,
    sender_encrypted: bool,
    sender_capable: bool,
) -> bool {
    topic == Some(WORLD_CAPABILITY)
        && payload_len <= MAX_WORLD_PAYLOAD
        && sender_capable
        && (!call_e2ee || sender_encrypted)
}

/// Whether a remote data track is a world track we subscribe to, judged once when it is
/// published. In an encrypted call (`call_e2ee`) it must say it is end-to-end encrypted. The
/// publisher's membership may not have arrived yet, so capability is judged per frame
/// instead ([`accept_track_frame`]).
pub fn accept_track(name: &str, uses_e2ee: bool, call_e2ee: bool) -> bool {
    name == WORLD_CAPABILITY && (!call_e2ee || uses_e2ee)
}

/// Whether one frame read from a world data track is taken.
pub fn accept_track_frame(payload_len: usize, publisher_capable: bool) -> bool {
    publisher_capable && payload_len <= MAX_WORLD_PAYLOAD
}

/// Which transport our own frames go out on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransportKind {
    Packets,
    Track,
}

/// What the owner of a [`WorldTransportChoice`] must do next.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Action {
    /// Switch sending to this transport; `None` leaves it as it is.
    pub transport: Option<TransportKind>,
    /// Start a data-track publish attempt carrying this generation.
    pub attempt: Option<u64>,
    /// Log which transport was chosen.
    pub announce: bool,
}

/// The data-track-or-packets decision for one connection, as a pure state machine. Each
/// publish attempt carries a generation, so a result that arrives after a reconnect (or a
/// second time) is ignored.
#[derive(Debug, Default)]
pub struct WorldTransportChoice {
    generation: u64,
    pending: bool,
}

impl WorldTransportChoice {
    fn begin(&mut self) -> Action {
        self.generation += 1;
        self.pending = true;
        Action {
            transport: Some(TransportKind::Packets),
            attempt: Some(self.generation),
            announce: false,
        }
    }

    /// The call connected: send on packets at once and try to publish a track.
    pub fn on_connected(&mut self) -> Action {
        self.begin()
    }

    /// The attempt `generation` finished. Only the current, unresolved attempt counts; on
    /// failure packets stay for the rest of the connection.
    pub fn on_publish_result(&mut self, generation: u64, ok: bool) -> Action {
        if !self.pending || generation != self.generation {
            return Action::default();
        }
        self.pending = false;
        Action {
            transport: Some(if ok {
                TransportKind::Track
            } else {
                TransportKind::Packets
            }),
            attempt: None,
            announce: true,
        }
    }

    /// The connection is closing: no later result counts.
    pub fn on_closed(&mut self) {
        self.generation += 1;
        self.pending = false;
    }

    /// The connection was re-established, maybe on another SFU: drop the track, send on
    /// packets, and try again.
    pub fn on_reconnected(&mut self) -> Action {
        self.begin()
    }
}

/// Result of pushing one frame on a data track.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PushOutcome {
    Sent,
    /// The track's queue is full: the frame is dropped.
    QueueFull,
    /// The track is gone: fall back to packets.
    Unpublished,
}

/// A published data track as the transport uses it; a fake in tests.
pub trait TrackSink: Send {
    fn push(&self, payload: &[u8]) -> PushOutcome;
    fn unpublish(&self);
}

impl TrackSink for LocalDataTrack {
    fn push(&self, payload: &[u8]) -> PushOutcome {
        match self.try_push(DataTrackFrame::new(payload.to_vec())) {
            Ok(()) => PushOutcome::Sent,
            Err(e) => match e.reason() {
                PushFrameErrorReason::QueueFull => PushOutcome::QueueFull,
                PushFrameErrorReason::TrackUnpublished => PushOutcome::Unpublished,
            },
        }
    }

    fn unpublish(&self) {
        LocalDataTrack::unpublish(self);
    }
}

/// Publishes the data track; boxed so tests can inject a fake.
type PublishTrackFn = Box<
    dyn Fn() -> Pin<Box<dyn Future<Output = Result<Box<dyn TrackSink>, String>> + Send>>
        + Send
        + Sync,
>;

struct TransportState {
    choice: WorldTransportChoice,
    track: Option<Box<dyn TrackSink>>,
    attempt: Option<AbortHandle>,
}

/// Our outgoing world frames on the primary connection: data packets at once, a data track
/// once one is published.
pub struct WorldTransport {
    packets: PublishFn,
    publish_track: PublishTrackFn,
    state: Mutex<TransportState>,
}

impl WorldTransport {
    pub fn new(room: Arc<Room>) -> Arc<Self> {
        let packet_room = Arc::clone(&room);
        Self::with_parts(
            Box::new(move |payload| {
                let room = Arc::clone(&packet_room);
                Box::pin(async move {
                    let packet = DataPacket {
                        payload,
                        topic: Some(WORLD_CAPABILITY.into()),
                        reliable: false,
                        ..Default::default()
                    };
                    if let Err(e) = room.local_participant().publish_data(packet).await {
                        debug!("rtc: world frame send failed: {e}");
                    }
                })
            }),
            Box::new(move || {
                let room = Arc::clone(&room);
                Box::pin(async move {
                    room.local_participant()
                        .publish_data_track(WORLD_CAPABILITY)
                        .await
                        .map(|t| Box::new(t) as Box<dyn TrackSink>)
                        .map_err(|e| e.to_string())
                })
            }),
        )
    }

    fn with_parts(packets: PublishFn, publish_track: PublishTrackFn) -> Arc<Self> {
        Arc::new(Self {
            packets,
            publish_track,
            state: Mutex::new(TransportState {
                choice: WorldTransportChoice::default(),
                track: None,
                attempt: None,
            }),
        })
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, TransportState> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// The call connected: packets now, a data-track attempt in the background.
    pub fn start(self: &Arc<Self>) {
        let mut st = self.lock();
        let action = st.choice.on_connected();
        self.begin_attempt(&mut st, action);
    }

    /// The primary connection reconnected: start over from packets.
    pub fn on_reconnected(self: &Arc<Self>) {
        let mut st = self.lock();
        let action = st.choice.on_reconnected();
        self.begin_attempt(&mut st, action);
    }

    fn begin_attempt(self: &Arc<Self>, st: &mut TransportState, action: Action) {
        if let Some(old) = st.attempt.take() {
            old.abort();
        }
        if let Some(track) = st.track.take() {
            track.unpublish();
        }
        let Some(generation) = action.attempt else {
            return;
        };
        let this = Arc::clone(self);
        st.attempt = Some(
            tokio::spawn(async move {
                let result = (this.publish_track)().await;
                this.finish_attempt(generation, result);
            })
            .abort_handle(),
        );
    }

    fn finish_attempt(&self, generation: u64, result: Result<Box<dyn TrackSink>, String>) {
        let mut st = self.lock();
        let action = st.choice.on_publish_result(generation, result.is_ok());
        let Some(kind) = action.transport else {
            // Stale or closed: a track that arrives late must not be left published.
            if let Ok(track) = result {
                track.unpublish();
            }
            return;
        };
        st.attempt = None;
        match (kind, result) {
            (TransportKind::Track, Ok(track)) => {
                st.track = Some(track);
                info!("rtc: world stream on a data track");
            }
            (_, Err(e)) => info!("rtc: world stream on data packets (no data track: {e})"),
            (_, Ok(_)) => {}
        }
    }

    /// Sends one frame on the current transport. A full track queue drops the frame; a
    /// track that has gone away falls back to packets.
    pub async fn send(&self, payload: Vec<u8>) {
        {
            let mut st = self.lock();
            if let Some(track) = &st.track {
                match track.push(&payload) {
                    PushOutcome::Sent | PushOutcome::QueueFull => return,
                    PushOutcome::Unpublished => {
                        st.track = None;
                        info!("rtc: world data track is gone, sending data packets");
                    }
                }
            }
        }
        (self.packets)(payload).await;
    }

    /// The connection is closing: end the attempt, ignore its result and unpublish the track.
    pub fn stop(&self) {
        let mut st = self.lock();
        st.choice.on_closed();
        if let Some(a) = st.attempt.take() {
            a.abort();
        }
        if let Some(t) = st.track.take() {
            t.unpublish();
        }
    }
}

/// Sends one payload; boxed so tests can inject a fake.
pub type PublishFn = Box<dyn Fn(Vec<u8>) -> Pin<Box<dyn Future<Output = ()> + Send>> + Send + Sync>;

struct Slot {
    latest: Mutex<Option<Vec<u8>>>,
    wake: Notify,
}

/// A latest-payload slot drained by one task: an offer replaces any payload not sent yet, so a
/// slow network never builds a queue of stale poses.
pub struct WorldSender {
    slot: Arc<Slot>,
    task: AbortHandle,
}

impl WorldSender {
    /// Spawns the sending task on the current runtime.
    pub fn spawn(publish: PublishFn) -> Self {
        let slot = Arc::new(Slot {
            latest: Mutex::new(None),
            wake: Notify::new(),
        });
        let s = Arc::clone(&slot);
        let task = tokio::spawn(async move {
            loop {
                let next = s.latest.lock().unwrap_or_else(|e| e.into_inner()).take();
                match next {
                    Some(payload) => publish(payload).await,
                    None => s.wake.notified().await,
                }
            }
        })
        .abort_handle();
        Self { slot, task }
    }

    /// Replaces any unsent payload with `payload`.
    pub fn offer(&self, payload: Vec<u8>) {
        *self.slot.latest.lock().unwrap_or_else(|e| e.into_inner()) = Some(payload);
        self.slot.wake.notify_one();
    }
}

impl WorldSender {
    /// Ends the sending task now (the connection is closing).
    pub fn stop(&self) {
        self.task.abort();
    }
}

impl Drop for WorldSender {
    fn drop(&mut self) {
        self.task.abort();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const T: Option<&str> = Some(WORLD_CAPABILITY);

    #[test]
    fn accept_packet_truth_table() {
        assert!(accept_packet(T, 32, false, false, true));
        assert!(accept_packet(T, MAX_WORLD_PAYLOAD, true, true, true));
        assert!(!accept_packet(None, 32, false, true, true));
        assert!(!accept_packet(Some("other"), 32, false, true, true));
        assert!(!accept_packet(T, MAX_WORLD_PAYLOAD + 1, false, true, true));
        assert!(!accept_packet(T, 32, true, false, true));
        assert!(!accept_packet(T, 32, false, true, false));
    }

    #[test]
    fn accept_track_truth_table() {
        // Unencrypted call: either encryption flag is taken.
        assert!(accept_track(WORLD_CAPABILITY, false, false));
        assert!(accept_track(WORLD_CAPABILITY, true, false));
        // Encrypted call: only an encrypted track.
        assert!(accept_track(WORLD_CAPABILITY, true, true));
        assert!(!accept_track(WORLD_CAPABILITY, false, true));
        assert!(!accept_track("other", true, false));
    }

    #[test]
    fn accept_track_frame_needs_capability_and_size() {
        assert!(accept_track_frame(32, true));
        assert!(accept_track_frame(MAX_WORLD_PAYLOAD, true));
        assert!(!accept_track_frame(MAX_WORLD_PAYLOAD + 1, true));
        assert!(!accept_track_frame(32, false));
    }

    #[test]
    fn choice_closed_ignores_later_results() {
        let mut c = WorldTransportChoice::default();
        let g = c.on_connected().attempt.unwrap();
        c.on_closed();
        assert_eq!(c.on_publish_result(g, true), Action::default());
    }

    #[derive(Default)]
    struct FakeTrack {
        outcome: Mutex<Option<PushOutcome>>,
        pushes: std::sync::atomic::AtomicUsize,
        unpublished: std::sync::atomic::AtomicBool,
    }
    struct FakeHandle(Arc<FakeTrack>);
    impl TrackSink for FakeHandle {
        fn push(&self, _: &[u8]) -> PushOutcome {
            self.0
                .pushes
                .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            self.0.outcome.lock().unwrap().unwrap()
        }
        fn unpublish(&self) {
            self.0
                .unpublished
                .store(true, std::sync::atomic::Ordering::SeqCst);
        }
    }

    /// A transport whose publish attempt never finishes (tests call `finish_attempt`) and
    /// whose packets are counted.
    fn transport() -> (Arc<WorldTransport>, Arc<std::sync::atomic::AtomicUsize>) {
        let packets = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let p = Arc::clone(&packets);
        let t = WorldTransport::with_parts(
            Box::new(move |_| {
                let p = Arc::clone(&p);
                Box::pin(async move {
                    p.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                })
            }),
            Box::new(|| Box::pin(std::future::pending())),
        );
        (t, packets)
    }

    fn fake(outcome: PushOutcome) -> (Arc<FakeTrack>, Box<dyn TrackSink>) {
        let f = Arc::new(FakeTrack::default());
        *f.outcome.lock().unwrap() = Some(outcome);
        (Arc::clone(&f), Box::new(FakeHandle(f)))
    }

    #[tokio::test]
    async fn send_on_a_full_track_queue_drops_the_frame() {
        use std::sync::atomic::Ordering::SeqCst;
        let (t, packets) = transport();
        t.start();
        let (f, h) = fake(PushOutcome::QueueFull);
        t.finish_attempt(1, Ok(h));
        t.send(vec![1]).await;
        assert_eq!(f.pushes.load(SeqCst), 1);
        assert_eq!(packets.load(SeqCst), 0);
        // Still on the track afterwards.
        t.send(vec![2]).await;
        assert_eq!(f.pushes.load(SeqCst), 2);
    }

    #[tokio::test]
    async fn send_on_an_unpublished_track_falls_back_to_packets() {
        use std::sync::atomic::Ordering::SeqCst;
        let (t, packets) = transport();
        t.start();
        let (f, h) = fake(PushOutcome::Unpublished);
        t.finish_attempt(1, Ok(h));
        t.send(vec![1]).await;
        t.send(vec![2]).await;
        // The track is tried once, then packets carry both frames.
        assert_eq!(f.pushes.load(SeqCst), 1);
        assert_eq!(packets.load(SeqCst), 2);
    }

    #[tokio::test]
    async fn finish_attempt_after_stop_is_ignored() {
        use std::sync::atomic::Ordering::SeqCst;
        let (t, packets) = transport();
        t.start();
        t.stop();
        let (f, h) = fake(PushOutcome::Sent);
        t.finish_attempt(1, Ok(h));
        assert!(f.unpublished.load(SeqCst));
        t.send(vec![1]).await;
        assert_eq!(f.pushes.load(SeqCst), 0);
        assert_eq!(packets.load(SeqCst), 1);
    }

    #[test]
    fn choice_connect_starts_packets_and_one_attempt() {
        let mut c = WorldTransportChoice::default();
        let a = c.on_connected();
        assert_eq!(a.transport, Some(TransportKind::Packets));
        assert!(a.attempt.is_some());
        assert!(!a.announce);
    }

    #[test]
    fn choice_success_switches_to_track() {
        let mut c = WorldTransportChoice::default();
        let gen = c.on_connected().attempt.unwrap();
        let a = c.on_publish_result(gen, true);
        assert_eq!(a.transport, Some(TransportKind::Track));
        assert!(a.announce);
        assert!(a.attempt.is_none());
    }

    #[test]
    fn choice_failure_keeps_packets_with_no_second_attempt() {
        let mut c = WorldTransportChoice::default();
        let gen = c.on_connected().attempt.unwrap();
        let a = c.on_publish_result(gen, false);
        assert_eq!(a.transport, Some(TransportKind::Packets));
        assert!(a.announce);
        assert!(a.attempt.is_none());
        // A repeated result for the same attempt is ignored.
        assert_eq!(c.on_publish_result(gen, true), Action::default());
    }

    #[test]
    fn choice_reconnect_returns_to_packets_with_new_attempt() {
        let mut c = WorldTransportChoice::default();
        let g1 = c.on_connected().attempt.unwrap();
        assert_eq!(
            c.on_publish_result(g1, true).transport,
            Some(TransportKind::Track)
        );
        let a = c.on_reconnected();
        assert_eq!(a.transport, Some(TransportKind::Packets));
        let g2 = a.attempt.unwrap();
        assert_ne!(g1, g2);
    }

    #[test]
    fn choice_stale_success_after_reconnect_is_ignored() {
        let mut c = WorldTransportChoice::default();
        let g1 = c.on_connected().attempt.unwrap();
        let g2 = c.on_reconnected().attempt.unwrap();
        assert_eq!(c.on_publish_result(g1, true), Action::default());
        assert_eq!(c.on_publish_result(g1, false), Action::default());
        assert_eq!(
            c.on_publish_result(g2, true).transport,
            Some(TransportKind::Track)
        );
    }

    #[tokio::test]
    async fn sender_delivers_first_then_only_latest() {
        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let gate = Arc::new(Notify::new());
        let (sent2, gate2) = (Arc::clone(&sent), Arc::clone(&gate));
        let sender = WorldSender::spawn(Box::new(move |p| {
            let (sent, gate) = (Arc::clone(&sent2), Arc::clone(&gate2));
            Box::pin(async move {
                let first = sent.lock().unwrap().is_empty();
                sent.lock().unwrap().push(p);
                if first {
                    gate.notified().await;
                }
            })
        }));
        sender.offer(vec![1]);
        for _ in 0..50 {
            if !sent.lock().unwrap().is_empty() {
                break;
            }
            tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        }
        sender.offer(vec![2]);
        sender.offer(vec![3]);
        gate.notify_one();
        for _ in 0..50 {
            if sent.lock().unwrap().len() >= 2 {
                break;
            }
            tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        assert_eq!(*sent.lock().unwrap(), vec![vec![1], vec![3]]);
    }
}
