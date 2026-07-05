// MatrixRTC FFI methods on ClientFfi.
//
// All methods are always compiled so the cxx bridge declaration is satisfied in
// both calls-enabled and calls-disabled builds.  The bodies use `#[cfg(feature
// = "calls")]` to either call the real implementation or return an error.
// Variables and imports used only in the calls-enabled path generate "unused"
// warnings in non-calls builds; suppress them here rather than sprinkling
// underscore prefixes throughout.
#![allow(unused_imports, unused_variables)]

use super::{err, ok, ClientFfi};
use crate::ffi::OpResult;

impl ClientFfi {
    /// Start a MatrixRTC call in `room_id` / `slot_id`.
    pub fn rtc_start_call(&mut self, room_id: &str, slot_id: &str, audio_only: bool) -> OpResult {
        #[cfg(not(feature = "calls"))]
        return err("calls feature not enabled in this build");

        #[cfg(feature = "calls")]
        {
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
            let result = std::thread::Builder::new()
                .stack_size(16 * 1024 * 1024)
                .spawn(move || {
                    handle.block_on(crate::client::rtc::session::start_call(
                        &client, &http, &room_id, &slot_id, audio_only,
                    ))
                })
                .expect("failed to spawn call thread")
                .join()
                .expect("call thread panicked");
            match result {
                Ok(session) => {
                    self.active_rtc_call = Some(Box::new(session));
                    ok("call started")
                }
                Err(e) => err(e.to_string()),
            }
        }
    }

    /// Gracefully leave the active call.
    pub fn rtc_end_call(&mut self) {
        #[cfg(feature = "calls")]
        if let Some(session) = self.active_rtc_call.take() {
            self.rt
                .block_on(crate::client::rtc::session::end_call(&*session));
        }
    }

    /// Mute or unmute the local audio track.
    pub fn rtc_set_audio_muted(&mut self, muted: bool) {
        #[cfg(feature = "calls")]
        {
            // mute() calls livekit_runtime::spawn → tokio::task::spawn, which
            // panics if there is no tokio runtime on the current thread. Enter
            // the runtime context so the spawn lands on our executor.
            let _guard = self.rt.enter();
            if let Some(session) = &self.active_rtc_call {
                session.mute_audio(muted);
            }
        }
    }

    /// Mute or unmute the local video track.
    pub fn rtc_set_video_muted(&mut self, muted: bool) {
        #[cfg(feature = "calls")]
        {
            let _guard = self.rt.enter();
            if let Some(session) = &self.active_rtc_call {
                session.mute_video(muted);
            }
        }
    }

    /// Inject a raw I420 video frame into the live session.
    #[allow(clippy::too_many_arguments)]
    pub fn rtc_push_video_frame_i420(
        &mut self,
        y: &[u8],
        u: &[u8],
        v: &[u8],
        width: u32,
        height: u32,
        stride_y: u32,
        stride_u: u32,
        stride_v: u32,
    ) {
        #[cfg(feature = "calls")]
        if let Some(session) = &self.active_rtc_call {
            session.push_video_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
        }
    }

    /// Start publishing a local screen share track. Blocks until the LiveKit
    /// SDP round-trip completes so the caller can immediately begin pushing
    /// frames without racing against a None screen_source.
    pub fn rtc_start_screen_share(&mut self) -> crate::ffi::OpResult {
        #[cfg(not(feature = "calls"))]
        return err("calls feature not enabled in this build");

        #[cfg(feature = "calls")]
        {
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
    }

    /// Stop the local screen share track. No-op when no call is active.
    pub fn rtc_stop_screen_share(&mut self) {
        #[cfg(feature = "calls")]
        {
            let _guard = self.rt.enter();
            if let Some(session) = &self.active_rtc_call {
                session.stop_screen_share();
            }
        }
    }

    /// Inject a raw I420 screen frame into the live session. No-op when no call active.
    #[allow(clippy::too_many_arguments)]
    pub fn rtc_push_screen_frame_i420(
        &mut self,
        y: &[u8],
        u: &[u8],
        v: &[u8],
        width: u32,
        height: u32,
        stride_y: u32,
        stride_u: u32,
        stride_v: u32,
    ) {
        #[cfg(feature = "calls")]
        if let Some(session) = &self.active_rtc_call {
            session.push_screen_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
        }
    }
}
