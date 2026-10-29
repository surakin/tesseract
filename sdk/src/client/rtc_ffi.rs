// MatrixRTC FFI methods on ClientFfi.

use super::{err, ok, ClientFfi};
use crate::ffi::OpResult;

impl ClientFfi {
    /// Start a MatrixRTC call in `room_id` / `slot_id`.
    pub fn rtc_start_call(&mut self, room_id: &str, slot_id: &str, audio_only: bool) -> OpResult {
        // Install the rustls crypto provider once per process. livekit's TLS
        // stack uses rustls but cannot auto-select a provider when both
        // aws-lc-rs and ring are present in the dep tree; we pin aws-lc-rs to
        // match the rest of the codebase. Returns Err if already installed.
        let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let http = self.http_client.clone();
        let room_id = room_id.to_owned();
        let slot_id = slot_id.to_owned();
        // block_on() drives the future on the *calling* thread (the Win32
        // message pump, ~1 MB stack), which overflows inside the
        // tungstenite WebSocket handshake state machine.  Run it instead
        // on a dedicated OS thread with a 16 MB stack; Handle::block_on
        // has no Send bound, so the non-Send MutexGuard in session.rs is
        // fine.  The handle keeps the same runtime for I/O and spawns.
        let handle = self.rt.handle().clone();
        // A call still active is ended before the new one starts, not when the
        // new session is stored: both announce the same legacy member (its state
        // key follows our device), so a late leave of the old one would erase
        // the new one's. The new call starts after the old one has left.
        let previous = self.active_rtc_call.take();
        // A failed spawn or a panic in the setup is a failed call, not a crash of
        // the whole client.
        let spawned = std::thread::Builder::new()
            .stack_size(16 * 1024 * 1024)
            .spawn(move || {
                handle.block_on(async move {
                    if let Some(old) = previous {
                        crate::client::rtc::session::end_call(&old).await;
                        drop(old);
                    }
                    crate::client::rtc::session::start_call(
                        &client, &http, &room_id, &slot_id, audio_only,
                    )
                    .await
                })
            });
        let worker = match spawned {
            Ok(worker) => worker,
            Err(e) => return err(format!("could not start the call thread: {e}")),
        };
        let result = match worker.join() {
            Ok(result) => result,
            Err(_) => return err("starting the call failed unexpectedly"),
        };
        match result {
            Ok(session) => {
                self.active_rtc_call = Some(Box::new(session));
                ok("call started")
            }
            Err(e) => err(e.to_string()),
        }
    }

    /// Gracefully leave the active call.
    pub fn rtc_end_call(&mut self) {
        if let Some(session) = self.active_rtc_call.take() {
            self.rt
                .block_on(crate::client::rtc::session::end_call(&*session));
        }
    }

    /// Returns true iff the current user may send every state event needed
    /// to join a MatrixRTC call in this room — `org.matrix.msc3401.call.member`
    /// (Element's "Join Element Call calls"). Reads cached m.room.power_levels
    /// — no network round-trip. Returns false on any uncertainty (not logged
    /// in, room unknown, PL unreadable).
    pub fn can_join_call_in_room(&self, room_id: &str) -> bool {
        self.can_send_call_state(room_id, &["org.matrix.msc3401.call.member"])
    }

    /// Returns true iff the current user may start a call in this room: join
    /// permission plus `org.matrix.msc3401.call` (Element's "Start Element
    /// Call calls"). Same cached read and fail-closed behaviour as
    /// `can_join_call_in_room`.
    pub fn can_start_call_in_room(&self, room_id: &str) -> bool {
        self.can_send_call_state(
            room_id,
            &["org.matrix.msc3401.call", "org.matrix.msc3401.call.member"],
        )
    }

    fn can_send_call_state(&self, room_id: &str, event_types: &[&str]) -> bool {
        use matrix_sdk::ruma::events::StateEventType;
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
            Ok(pl) => event_types
                .iter()
                .all(|t| pl.user_can_send_state(user_id, StateEventType::from(*t))),
            Err(_) => false,
        }
    }

    /// Mute or unmute the local audio track.
    pub fn rtc_set_audio_muted(&self, muted: bool) {
        // mute() calls tokio::task::spawn, which
        // panics if there is no tokio runtime on the current thread. Enter
        // the runtime context so the spawn lands on our executor.
        let _guard = self.rt.enter();
        if let Some(session) = &self.active_rtc_call {
            session.mute_audio(muted);
        }
    }

    /// Mute or unmute the local video track.
    pub fn rtc_set_video_muted(&self, muted: bool) {
        let _guard = self.rt.enter();
        if let Some(session) = &self.active_rtc_call {
            session.mute_video(muted);
        }
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
        if let Some(session) = &self.active_rtc_call {
            session.push_video_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
        }
    }

    /// Start publishing a local screen share track. Blocks until the LiveKit
    /// SDP round-trip completes so the caller can immediately begin pushing
    /// frames without racing against a None screen_source.
    pub fn rtc_start_screen_share(&mut self) -> crate::ffi::OpResult {
        let handle = self.rt.handle().clone();
        if let Some(session) = &self.active_rtc_call {
            match session.start_screen_share(handle) {
                Ok(()) => ok("screen share started"),
                Err(e) => err(e.to_string()),
            }
        } else {
            err("no active call")
        }
    }

    /// Stop the local screen share track. No-op when no call is active.
    pub fn rtc_stop_screen_share(&mut self) {
        let _guard = self.rt.enter();
        if let Some(session) = &self.active_rtc_call {
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
        if let Some(session) = &self.active_rtc_call {
            session.push_screen_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
        }
    }

    /// Decline an incoming MatrixRTC invite (`m.rtc.decline` referencing the
    /// invite event). Fire and forget; failures are logged.
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
        let invite_event_id = invite_event_id.to_owned();
        self.rt.spawn(async move {
            let Some(room) = client.get_room(&room_id) else {
                return;
            };
            if let Err(e) = crate::client::rtc::invites::send_decline(&room, &invite_event_id).await {
                tracing::warn!("rtc: declining {invite_event_id} failed: {e}");
            }
        });
    }
}
