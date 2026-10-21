//! MatrixRTC calls: signaling, transport, session lifecycle, and LiveKit media.
// Layer 1 types and methods will be wired up in Layer 3 (media capture) and
// Layer 4 (C++ client).  Suppress dead-code warnings until then.
#![allow(dead_code, unused_imports)]

pub mod audio_mix;
pub mod e2ee;
pub mod key_rotation;
pub mod members;
pub mod model;
pub mod mrtc_membership;
pub mod identity;
pub mod invites;
pub mod livekit_room;
pub mod session;
pub mod slots;
pub mod sfu_set;
pub mod signaling;
pub mod sticky;
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
    /// Whether media from this participant should be played (see `FilteredSink`).
    fn accepts_participant(&self, _participant_id: &str) -> bool {
        true
    }
}

/// One sink per signed-in account, keyed by its user id. Each account's
/// handler must get its own RTC events. A single process-wide slot kept only
/// the first account's sink, so later accounts (and the same account after
/// a restart) received no call events.
fn sinks() -> &'static std::sync::Mutex<std::collections::HashMap<String, Arc<dyn RtcEventSink>>> {
    static SINKS: OnceLock<
        std::sync::Mutex<std::collections::HashMap<String, Arc<dyn RtcEventSink>>>,
    > = OnceLock::new();
    SINKS.get_or_init(|| std::sync::Mutex::new(std::collections::HashMap::new()))
}

/// Register the C++ callback bridge for `user_id`, replacing any earlier one.
pub fn register_sink(user_id: &str, sink: Arc<dyn RtcEventSink>) {
    sinks()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(user_id.to_owned(), sink);
}

/// Drop `user_id`'s sink on logout, so a signed-out account gets no events.
pub fn unregister_sink(user_id: &str) {
    sinks()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(user_id);
}

/// The sink for the account `client` is signed in as, if one is registered.
pub(crate) fn sink_for_client(client: &matrix_sdk::Client) -> Option<Arc<dyn RtcEventSink>> {
    sink_for_user(&client.user_id()?.to_string())
}

fn sink_for_user(user_id: &str) -> Option<Arc<dyn RtcEventSink>> {
    sinks()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get(user_id)
        .cloned()
}

#[cfg(test)]
mod sink_registry_tests {
    use super::*;

    struct Null;

    impl RtcEventSink for Null {
        fn on_invitation(&self, _: &str, _: &str, _: &str, _: &str, _: u64, _: &str) {}
        fn on_participant_joined(&self, _: u64, _: RtcParticipantInfo) {}
        fn on_participant_left(&self, _: u64, _: &str) {}
        fn on_participant_updated(&self, _: u64, _: RtcParticipantInfo) {}
        fn on_session_ended(&self, _: u64, _: &str) {}
        fn on_video_frame(&self, _: u64, _: &str, _: u32, _: u32, _: Vec<u8>) {}
        fn on_screen_frame(&self, _: u64, _: &str, _: u32, _: u32, _: Vec<u8>) {}
        fn on_audio_frame(&self, _: u64, _: &str, _: &[i16], _: u32, _: u32) {}
    }

    #[test]
    fn each_account_keeps_its_own_sink_until_it_logs_out() {
        let alice: Arc<dyn RtcEventSink> = Arc::new(Null);
        let bob: Arc<dyn RtcEventSink> = Arc::new(Null);
        register_sink("@alice:registry.test", alice.clone());
        register_sink("@bob:registry.test", bob.clone());
        assert!(Arc::ptr_eq(
            &sink_for_user("@alice:registry.test").unwrap(),
            &alice
        ));
        assert!(Arc::ptr_eq(
            &sink_for_user("@bob:registry.test").unwrap(),
            &bob
        ));

        unregister_sink("@alice:registry.test");
        assert!(sink_for_user("@alice:registry.test").is_none());
        assert!(sink_for_user("@bob:registry.test").is_some());
        unregister_sink("@bob:registry.test");
    }
}
