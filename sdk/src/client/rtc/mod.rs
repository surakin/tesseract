//! MatrixRTC calls: signaling, transport, session lifecycle, and LiveKit media.
// Layer 1 types and methods will be wired up in Layer 3 (media capture) and
// Layer 4 (C++ client).  Suppress dead-code warnings until then.
#![allow(dead_code, unused_imports)]

pub mod e2ee;
pub mod livekit_room;
pub mod session;
pub mod signaling;
pub mod transport;

pub use session::{start_call, RtcSession};
pub use signaling::RtcParticipantInfo;

use std::sync::{Arc, OnceLock};

/// Sink for call events, implemented by the FFI bridge (Layer 2).
/// All methods are invoked on tokio worker threads; the implementation
/// must not touch the UI directly — it must post to the UI thread.
pub trait RtcEventSink: Send + Sync + 'static {
    fn on_invitation(
        &self,
        room_id: &str,
        slot_id: &str,
        caller_user_id: &str,
        call_intent: &str,           // "audio" | "video" | "" (empty = unknown)
        lifetime_ms: u64,            // remaining ms before ring expires; 0 = no explicit timeout
        notification_event_id: &str, // "$..." or "" when triggered by member-state event
    );
    fn on_participant_joined(&self, session_id: u64, info: RtcParticipantInfo);
    fn on_participant_left(&self, session_id: u64, participant_id: &str);
    fn on_participant_updated(&self, session_id: u64, info: RtcParticipantInfo);
    fn on_session_ended(&self, session_id: u64, reason: &str);
    fn on_video_frame(
        &self,
        session_id: u64,
        participant_id: &str,
        width: u32,
        height: u32,
        rgba: Vec<u8>,
    );
    /// Decoded RGBA screen share frame from a remote participant (~15fps).
    fn on_screen_frame(
        &self,
        session_id: u64,
        participant_id: &str,
        width: u32,
        height: u32,
        rgba: Vec<u8>,
    );
    /// S16LE 48kHz mono PCM from a remote participant's audio track.
    fn on_audio_frame(
        &self,
        session_id: u64,
        participant_id: &str,
        samples: &[i16],
        sample_rate: u32,
        num_channels: u32,
    );
}

static GLOBAL_SINK: OnceLock<Arc<dyn RtcEventSink>> = OnceLock::new();

/// Called once at startup by Layer 2 (before any sync starts) to register the
/// C++ callback bridge.
pub fn register_sink(sink: Arc<dyn RtcEventSink>) {
    let _ = GLOBAL_SINK.set(sink);
}

pub(crate) fn global_sink() -> Option<Arc<dyn RtcEventSink>> {
    GLOBAL_SINK.get().cloned()
}
