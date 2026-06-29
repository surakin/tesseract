#![cfg(feature = "calls")]

//! LiveKit room connection and media track management.
//!
//! This module wraps the `livekit` crate. The API calls here target livekit
//! ~0.7; minor adjustments may be needed once the crate is first compiled.
//! Every intentionally-unverified API surface is marked `// TODO: verify API`.

use std::{
    collections::HashMap,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex as StdMutex,
    },
};

use futures_util::StreamExt as _;
use tokio::task::AbortHandle;
use tracing::{info, warn};

use super::RtcParticipantInfo;
use crate::client::rtc::RtcEventSink;

use livekit::{
    e2ee::{
        key_provider::{KeyDerivationAlgorithm, KeyProvider, KeyProviderOptions},
        E2eeOptions, EncryptionType,
    },
    options::TrackPublishOptions,
    prelude::*,
    webrtc::{
        audio_stream::native::NativeAudioStream,
        video_frame::{BoxVideoFrame, I420Buffer, VideoFrame, VideoRotation},
        video_source::{native::NativeVideoSource, RtcVideoSource},
        video_stream::native::NativeVideoStream,
    },
    RoomEvent, RoomOptions,
};

pub struct LiveKitRoom {
    room: Arc<Room>,
    platform_audio: PlatformAudio,
    video_source: NativeVideoSource,
    audio_publication: LocalTrackPublication,
    video_publication: LocalTrackPublication,
    /// Drop-if-busy flag: prevents queuing more than one pending video frame
    /// callback at a time (avoids flooding the UI thread at 30fps Ã— N callers).
    video_frame_in_flight: Arc<AtomicBool>,
    /// Separate gate for the local self-view loopback.
    local_video_in_flight: Arc<AtomicBool>,
    /// Screen share source and publication; None until start_screen_share().
    screen_source: StdMutex<Option<NativeVideoSource>>,
    screen_publication: StdMutex<Option<LocalTrackPublication>>,
    /// Separate in-flight gate for screen frames â€” must not share with camera.
    screen_frame_in_flight: Arc<AtomicBool>,
    local_identity: String,
    sink: Option<Arc<dyn RtcEventSink>>,
    session_id: u64,
    event_task: AbortHandle,
    key_provider: KeyProvider,
    /// Monotonic start time used to generate RTP timestamps for video frames.
    call_start: std::time::Instant,
    /// Pending peer keys keyed by Matrix user_id â†’ key index â†’ raw bytes.
    /// All received indices are stored so frames encrypted before a rotation
    /// can still be decrypted when the participant first appears in LiveKit.
    pending_peer_keys: Arc<StdMutex<HashMap<String, HashMap<i32, Vec<u8>>>>>,
}

impl LiveKitRoom {
    /// Connect, publish local audio + video tracks, start the event loop.
    ///
    /// `rotate_tx`: when `Some`, the event loop fires a `()` message on every
    /// `ParticipantDisconnected` event so the caller can rotate the frame key.
    /// `rebroadcast_tx`: when `Some`, the event loop sends the participant's
    /// Matrix user ID on every `ParticipantConnected` and for each participant
    /// already present at `Connected`, so the caller can re-broadcast the
    /// current key directly to that user bypassing the room member list cache.
    pub async fn connect(
        server_url: &str,
        jwt: &str,
        session_id: u64,
        sink: Option<Arc<dyn RtcEventSink>>,
        use_e2ee: bool,
        rotate_tx: Option<tokio::sync::mpsc::Sender<()>>,
        rebroadcast_tx: Option<tokio::sync::mpsc::Sender<String>>,
    ) -> anyhow::Result<Self> {
        // Match Element Call's key provider configuration exactly so both sides
        // derive the same AES-128-GCM frame key from the exchanged key material:
        //   - HKDF (not PBKDF2) with SHA-256, salt="LKFrameEncryptionKey"
        //   - ratchetWindowSize=8, failureTolerance=10, keyringSize=16
        // Using PBKDF2 produces a different derived key and frames can't decrypt.
        let key_provider = KeyProvider::new(KeyProviderOptions {
            key_derivation_algorithm: KeyDerivationAlgorithm::HKDF,
            ratchet_window_size: 8,
            failure_tolerance: 10,
            key_ring_size: 16,
            ..KeyProviderOptions::default()
        });
        // RoomOptions is #[non_exhaustive] in the external crate â€” must use
        // Default::default() then field-assign rather than struct literal.
        let mut room_options = RoomOptions::default();
        if use_e2ee {
            room_options.encryption = Some(E2eeOptions {
                encryption_type: EncryptionType::Gcm,
                key_provider: key_provider.clone(),
            });
        }
        let (room, events) = Room::connect(server_url, jwt, room_options).await?;
        let room = Arc::new(room);

        // Local audio track via platform hardware ADM.
        // PlatformAudio gives libwebrtc's AEC3 access to the speaker-output
        // reference, enabling echo cancellation. AudioProcessingOptions::default()
        // enables AEC, NS, and AGC. Capture is handled entirely by the platform
        // ADM â€” no manual PCM injection needed.
        let platform_audio =
            PlatformAudio::new().map_err(|e| anyhow::anyhow!("PlatformAudio init failed: {e}"))?;
        platform_audio
            .configure_audio_processing(AudioProcessingOptions::default())
            .map_err(|e| anyhow::anyhow!("PlatformAudio configure failed: {e}"))?;
        let local_audio = LocalAudioTrack::create_audio_track("mic", platform_audio.rtc_source());
        // dtx=false: always send audio packets even during silence.
        // red=false: send plain Opus instead of RED; some SFUs fail to forward
        //   RED audio to subscribers when the subscriber's codec is Opus-only.
        // source=Microphone: required by SFUs that use source to classify tracks
        //   before deciding whether to forward to subscribers.
        let audio_opts_pub = TrackPublishOptions {
            dtx: false,
            red: false,
            simulcast: false,
            source: TrackSource::Microphone,
            ..Default::default()
        };
        let audio_publication = room
            .local_participant()
            .publish_track(LocalTrack::Audio(local_audio), audio_opts_pub)
            .await?;

        // Local video track (camera frames injected by Layer 3)
        let video_source = NativeVideoSource::new(
            livekit::webrtc::video_source::VideoResolution {
                width: 640,
                height: 480,
            },
            false,
        );
        let local_video = LocalVideoTrack::create_video_track(
            "camera",
            RtcVideoSource::Native(video_source.clone()),
        );
        // simulcast=false: single VP8 layer.
        // source=Camera: signals to the SFU what kind of track this is.
        let video_opts_pub = TrackPublishOptions {
            simulcast: false,
            source: TrackSource::Camera,
            ..Default::default()
        };
        let video_publication = room
            .local_participant()
            .publish_track(LocalTrack::Video(local_video), video_opts_pub)
            .await?;

        // Emit local participant immediately so the call overlay populates even
        // when no remote participants have joined yet.
        if let Some(ref s) = sink {
            let local = room.local_participant();
            let local_identity = local.identity().as_str().to_owned();
            let (local_user_id, local_device_id) = split_identity(&local_identity);
            s.on_participant_joined(
                session_id,
                RtcParticipantInfo {
                    participant_id: local_identity,
                    user_id: local_user_id,
                    device_id: local_device_id,
                    is_audio_muted: false,
                    is_video_muted: false,
                    is_screen_sharing: false,
                },
            );
        }

        let local_identity = room.local_participant().identity().as_str().to_owned();
        let video_frame_in_flight = Arc::new(AtomicBool::new(false));
        let local_video_in_flight = Arc::new(AtomicBool::new(false));
        let screen_frame_in_flight = Arc::new(AtomicBool::new(false));
        let pending_peer_keys: Arc<StdMutex<HashMap<String, HashMap<i32, Vec<u8>>>>> =
            Arc::new(StdMutex::new(HashMap::new()));
        let event_task = spawn_event_task(
            Arc::clone(&room),
            events,
            session_id,
            Arc::clone(&video_frame_in_flight),
            Arc::clone(&screen_frame_in_flight),
            sink.clone(),
            key_provider.clone(),
            Arc::clone(&pending_peer_keys),
            rotate_tx,
            rebroadcast_tx,
        );

        info!("rtc: livekit connected (session {session_id}), local identity={local_identity}");
        Ok(Self {
            room,
            platform_audio,
            video_source,
            audio_publication,
            video_publication,
            video_frame_in_flight,
            local_video_in_flight,
            screen_source: StdMutex::new(None),
            screen_publication: StdMutex::new(None),
            screen_frame_in_flight,
            local_identity,
            sink,
            session_id,
            event_task,
            key_provider,
            call_start: std::time::Instant::now(),
            pending_peer_keys,
        })
    }

    pub fn set_audio_muted(&self, muted: bool) {
        if muted {
            self.audio_publication.mute();
        } else {
            self.audio_publication.unmute();
        }
    }

    pub fn set_video_muted(&self, muted: bool) {
        if muted {
            self.video_publication.mute();
        } else {
            self.video_publication.unmute();
        }
    }

    /// Inject a raw I420 frame from VideoCaptureCallSession (Layer 3).
    pub fn push_video_frame_i420(
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
        let mut buf = I420Buffer::new(width, height);
        {
            let (dy, du, dv) = buf.data_mut();
            // I420Buffer is tightly packed; copy row-by-row to handle source
            // padding (stride > packed width), which macOS/Win32 cameras produce.
            copy_plane(y, dy, width as usize, height as usize, stride_y as usize);
            let h_uv = ((height as usize) + 1) / 2;
            let w_uv = ((width as usize) + 1) / 2;
            copy_plane(u, du, w_uv, h_uv, stride_u as usize);
            copy_plane(v, dv, w_uv, h_uv, stride_v as usize);
        }
        let timestamp_us = self.call_start.elapsed().as_micros() as i64;
        let frame = VideoFrame {
            rotation: VideoRotation::VideoRotation0,
            timestamp_us,
            frame_metadata: None,
            buffer: buf,
        };
        self.video_source.capture_frame(&frame);

        // Self-view loopback: deliver a decoded RGBA copy to the call overlay so
        // the local participant cell shows the camera feed without a round-trip
        // through the SFU.
        if let Some(ref s) = self.sink {
            if self
                .local_video_in_flight
                .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
            {
                let rgba = i420_planes_to_rgba(y, u, v, width, height, stride_y, stride_u);
                s.on_video_frame(self.session_id, &self.local_identity, width, height, rgba);
                self.local_video_in_flight.store(false, Ordering::Release);
            }
        }
    }

    /// Publish a new screen share track. Creates the NativeVideoSource and
    /// LocalTrackPublication with TrackSource::Screenshare and stores them.
    pub async fn start_screen_share(&self) -> anyhow::Result<()> {
        let screen_source = NativeVideoSource::new(
            livekit::webrtc::video_source::VideoResolution {
                width: 1920,
                height: 1080,
            },
            false,
        );
        let screen_track = LocalVideoTrack::create_video_track(
            "screenshare",
            RtcVideoSource::Native(screen_source.clone()),
        );
        let opts = TrackPublishOptions {
            simulcast: false,
            source: TrackSource::Screenshare,
            ..Default::default()
        };
        let pub_ = self
            .room
            .local_participant()
            .publish_track(LocalTrack::Video(screen_track), opts)
            .await?;
        *self.screen_source.lock().unwrap() = Some(screen_source);
        *self.screen_publication.lock().unwrap() = Some(pub_);
        Ok(())
    }

    /// Mute and drop the screen share track.
    pub fn stop_screen_share(&self) {
        if let Some(ref pub_) = *self.screen_publication.lock().unwrap() {
            pub_.mute();
        }
        *self.screen_source.lock().unwrap() = None;
        *self.screen_publication.lock().unwrap() = None;
    }

    /// Inject a raw I420 screen frame. No-op when no screen share is active.
    pub fn push_screen_frame_i420(
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
        let src_guard = self.screen_source.lock().unwrap();
        let Some(ref src) = *src_guard else { return };
        if self
            .screen_frame_in_flight
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return;
        }
        let mut buf = I420Buffer::new(width, height);
        {
            let (dy, du, dv) = buf.data_mut();
            copy_plane(y, dy, width as usize, height as usize, stride_y as usize);
            let h_uv = ((height as usize) + 1) / 2;
            let w_uv = ((width as usize) + 1) / 2;
            copy_plane(u, du, w_uv, h_uv, stride_u as usize);
            copy_plane(v, dv, w_uv, h_uv, stride_v as usize);
        }
        let timestamp_us = self.call_start.elapsed().as_micros() as i64;
        let frame = VideoFrame {
            rotation: VideoRotation::VideoRotation0,
            timestamp_us,
            frame_metadata: None,
            buffer: buf,
        };
        src.capture_frame(&frame);
        self.screen_frame_in_flight.store(false, Ordering::Release);
    }

    /// The LiveKit participant identity assigned by the JWT service.
    /// Must be used as `member_id` in m.rtc.member and m.rtc.encryption_key
    /// events so other clients can correlate our key with our tracks.
    pub fn local_identity(&self) -> &str {
        &self.local_identity
    }

    /// Set our own 32-byte raw key material in the KeyProvider so LiveKit
    /// encrypts our outgoing tracks with AES-GCM.
    pub fn set_own_frame_key(&self, raw_key: &[u8], index: i32) {
        info!(
            "e2ee: set own frame key index={index} identity={}",
            self.local_identity
        );
        let identity: ParticipantIdentity = self.local_identity.clone().into();
        self.key_provider
            .set_key(&identity, index, raw_key.to_vec());
    }

    /// Store a peer's raw key material so LiveKit can decrypt their incoming
    /// tracks. Applies immediately to any connected participants whose LiveKit
    /// identity starts with `{sender_user_id}:` (the format the JWT service
    /// uses), and queues the key for participants who connect later.
    pub fn queue_peer_key(&self, sender_user_id: &str, index: i32, raw_key: Vec<u8>) {
        // Store this index; preserve all other indices so frames encrypted
        // before a rotation can still be decrypted when the participant appears.
        self.pending_peer_keys
            .lock()
            .unwrap()
            .entry(sender_user_id.to_owned())
            .or_default()
            .insert(index, raw_key.clone());

        // Apply to any participant already in the room.
        let prefix = format!("{}:", sender_user_id);
        let mut applied = 0usize;
        for (identity, _) in self.room.remote_participants() {
            let id_str = identity.as_str();
            if id_str.starts_with(&prefix) || id_str == sender_user_id {
                info!(
                    "e2ee: set_key for participant {id_str} (from {sender_user_id} index={index})"
                );
                self.key_provider.set_key(&identity, index, raw_key.clone());
                applied += 1;
            }
        }
        if applied == 0 {
            info!(
                "e2ee: queued key for {sender_user_id} index={index} (participant not yet connected)"
            );
        }
    }

    pub async fn disconnect(&self) {
        let _ = self.room.close().await;
        self.event_task.abort();
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Copy one I420 plane from a (possibly padded) source into a tightly-packed
/// destination. `src` has `stride` bytes per row; `dst` has exactly
/// `width * rows` bytes (no padding). Panics only if `dst` is smaller than
/// `width * rows` â€” that invariant is guaranteed by `I420Buffer::new`.
fn copy_plane(src: &[u8], dst: &mut [u8], width: usize, rows: usize, stride: usize) {
    if stride == width {
        // Fast path: already tightly packed.
        let len = width * rows;
        dst[..len].copy_from_slice(&src[..len]);
    } else {
        for row in 0..rows {
            let src_off = row * stride;
            let dst_off = row * width;
            dst[dst_off..dst_off + width].copy_from_slice(&src[src_off..src_off + width]);
        }
    }
}

// ---------------------------------------------------------------------------
// Background event task
// ---------------------------------------------------------------------------

fn spawn_event_task(
    room: Arc<Room>,
    mut events: tokio::sync::mpsc::UnboundedReceiver<RoomEvent>,
    session_id: u64,
    video_in_flight: Arc<AtomicBool>,
    screen_in_flight: Arc<AtomicBool>,
    sink: Option<Arc<dyn RtcEventSink>>,
    key_provider: KeyProvider,
    pending_peer_keys: Arc<StdMutex<HashMap<String, HashMap<i32, Vec<u8>>>>>,
    rotate_tx: Option<tokio::sync::mpsc::Sender<()>>,
    rebroadcast_tx: Option<tokio::sync::mpsc::Sender<String>>,
) -> AbortHandle {
    tokio::spawn(async move {
        while let Some(event) = events.recv().await {
            match event {
                RoomEvent::Connected {
                    participants_with_tracks,
                } => {
                    // Fires once at join time and contains ALL participants who were
                    // already in the room before us. livekit-rs does NOT emit
                    // ParticipantConnected for these pre-existing participants.
                    for (participant, _pubs) in &participants_with_tracks {
                        if let Some(ref s) = sink {
                            s.on_participant_joined(session_id, participant_info(participant));
                        }
                        // Apply any pending E2EE keys for this participant (all indices).
                        let id = participant.identity();
                        {
                            let pending = pending_peer_keys.lock().unwrap();
                            for (user_id_prefix, keys_by_index) in pending.iter() {
                                let prefix = format!("{}:", user_id_prefix);
                                let id_str = id.as_str();
                                if id_str.starts_with(&prefix) || id_str == user_id_prefix.as_str()
                                {
                                    for (idx, raw_key) in keys_by_index {
                                        key_provider.set_key(&id, *idx, raw_key.clone());
                                    }
                                }
                            }
                        }
                        // Re-broadcast our key to each pre-existing participant
                        // using their Matrix user ID (extracted from the LiveKit
                        // identity) so the broadcast reaches them even when they
                        // are not yet in the local room-member cache.
                        if let Some(ref tx) = rebroadcast_tx {
                            if let Some(uid) = matrix_user_id_from_lk_identity(id.as_str()) {
                                let _ = tx.try_send(uid.to_owned());
                            }
                        }
                    }
                    // TrackSubscribed events for these participants' tracks will
                    // follow asynchronously once WebRTC subscription negotiation
                    // completes â€” handled by the TrackSubscribed arm below.
                }
                RoomEvent::ConnectionStateChanged(state) => {
                    info!("rtc: connection state â†’ {state:?}");
                }
                RoomEvent::LocalTrackPublished { track, .. } => {
                    info!("rtc: local track published: {:?}", track.kind());
                }
                RoomEvent::LocalTrackSubscribed { .. } => {}
                RoomEvent::TrackSubscriptionFailed {
                    participant, error, ..
                } => {
                    warn!(
                        "rtc: track subscription failed for {}: {error}",
                        participant.identity()
                    );
                }
                RoomEvent::ParticipantEncryptionStatusChanged { .. } => {}
                RoomEvent::ParticipantConnected(p) => {
                    info!("rtc: participant connected: {}", p.identity());
                    if let Some(ref s) = sink {
                        s.on_participant_joined(session_id, participant_info(&p));
                    }
                    // Apply any pending peer keys for this participant (all indices).
                    let id = p.identity();
                    let pending = pending_peer_keys.lock().unwrap();
                    for (user_id_prefix, keys_by_index) in pending.iter() {
                        let prefix = format!("{}:", user_id_prefix);
                        let id_str = id.as_str();
                        if id_str.starts_with(&prefix) || id_str == user_id_prefix.as_str() {
                            for (idx, raw_key) in keys_by_index {
                                key_provider.set_key(&id, *idx, raw_key.clone());
                            }
                        }
                    }
                    // Re-broadcast our own key to this specific participant using
                    // their Matrix user ID (extracted from the LiveKit identity).
                    // This bypasses room.members() which may not include them yet.
                    if let Some(ref tx) = rebroadcast_tx {
                        let id = p.identity();
                        if let Some(uid) = matrix_user_id_from_lk_identity(id.as_str()) {
                            let _ = tx.try_send(uid.to_owned());
                        }
                    }
                }
                RoomEvent::ParticipantDisconnected(p) => {
                    info!("rtc: participant disconnected: {}", p.identity());
                    if let Some(ref s) = sink {
                        s.on_participant_left(session_id, p.identity().as_str());
                    }
                    // Trigger a key rotation so the departed participant loses
                    // the ability to decrypt future media (forward secrecy).
                    if let Some(ref tx) = rotate_tx {
                        let _ = tx.try_send(());
                    }
                }
                RoomEvent::TrackSubscribed {
                    track, participant, ..
                } => {
                    info!(
                        "rtc: TrackSubscribed from {} kind={:?}",
                        participant.identity(),
                        track.kind()
                    );
                    // Re-emit participant state now that publications are populated.
                    // At ParticipantConnected time the publication list is empty
                    // (0 pubs), so is_video_muted=true and the tile shows an avatar
                    // even though frames are arriving. Refreshing here fixes that.
                    if let Some(ref s) = sink {
                        s.on_participant_updated(session_id, participant_info(&participant));
                    }
                    let pid = participant.identity().as_str().to_owned();
                    match track {
                        RemoteTrack::Video(video_track) => {
                            // Determine if this is a screen share track by inspecting
                            // the publication source before the async stream starts.
                            let is_screen = {
                                let pubs = participant.track_publications();
                                pubs.values().any(|p| {
                                    p.source() == TrackSource::Screenshare
                                        && p.kind() == TrackKind::Video
                                        && !p.is_muted()
                                })
                            };
                            let sink2 = sink.clone();
                            let vif = if is_screen {
                                Arc::clone(&screen_in_flight)
                            } else {
                                Arc::clone(&video_in_flight)
                            };
                            let sid = session_id;
                            let pid2 = pid.clone();
                            let rtc = video_track.rtc_track();
                            let is_scr = is_screen;
                            tokio::spawn(async move {
                                let mut stream = NativeVideoStream::new(rtc);
                                while let Some(frame) = stream.next().await {
                                    if vif
                                        .compare_exchange(
                                            false,
                                            true,
                                            Ordering::Acquire,
                                            Ordering::Relaxed,
                                        )
                                        .is_err()
                                    {
                                        continue;
                                    }
                                    if let Some(ref s) = sink2 {
                                        if let Some(rgba) = i420_to_rgba(&frame) {
                                            let buf = frame.buffer.as_ref();
                                            let w = buf.width();
                                            let h = buf.height();
                                            if is_scr {
                                                s.on_screen_frame(sid, &pid2, w, h, rgba);
                                            } else {
                                                s.on_video_frame(sid, &pid2, w, h, rgba);
                                            }
                                        }
                                    }
                                    vif.store(false, Ordering::Release);
                                }
                            });
                        }
                        RemoteTrack::Audio(audio_track) => {
                            let sink2 = sink.clone();
                            let sid = session_id;
                            let rtc = audio_track.rtc_track();
                            tokio::spawn(async move {
                                // Request 48kHz mono to match our capture format.
                                let mut stream = NativeAudioStream::new(rtc, 48_000, 1);
                                while let Some(frame) = stream.next().await {
                                    if let Some(ref s) = sink2 {
                                        s.on_audio_frame(
                                            sid,
                                            &pid,
                                            &frame.data,
                                            frame.sample_rate,
                                            frame.num_channels,
                                        );
                                    }
                                }
                            });
                        }
                    }
                }
                RoomEvent::TrackMuted { participant, .. } => {
                    if let Some(ref s) = sink {
                        let identity = participant.identity();
                        if let Some(rp) = room.remote_participants().get(&identity) {
                            s.on_participant_updated(session_id, participant_info(&rp));
                        } else if identity == room.local_participant().identity() {
                            s.on_participant_updated(
                                session_id,
                                local_participant_info(&room.local_participant()),
                            );
                        }
                    }
                }
                RoomEvent::TrackUnmuted { participant, .. } => {
                    if let Some(ref s) = sink {
                        let identity = participant.identity();
                        if let Some(rp) = room.remote_participants().get(&identity) {
                            s.on_participant_updated(session_id, participant_info(&rp));
                        } else if identity == room.local_participant().identity() {
                            s.on_participant_updated(
                                session_id,
                                local_participant_info(&room.local_participant()),
                            );
                        }
                    }
                }
                RoomEvent::Reconnecting => {
                    warn!("rtc: livekit reconnectingâ€¦");
                }
                RoomEvent::Reconnected => {
                    info!("rtc: livekit reconnected");
                }
                RoomEvent::Disconnected { reason } => {
                    info!("rtc: room disconnected: {reason:?}");
                    if let Some(ref s) = sink {
                        s.on_session_ended(session_id, &format!("{reason:?}"));
                    }
                    break;
                }
                other => {
                    tracing::debug!("rtc: unhandled event: {other:?}");
                }
            }
        }
    })
    .abort_handle()
}

fn participant_info(p: &RemoteParticipant) -> RtcParticipantInfo {
    let pubs = p.track_publications();
    let is_audio_muted = !pubs
        .values()
        .any(|pub_| pub_.kind() == TrackKind::Audio && !pub_.is_muted());
    let is_video_muted = !pubs
        .values()
        .any(|pub_| pub_.kind() == TrackKind::Video && !pub_.is_muted());
    let is_screen_sharing = pubs
        .values()
        .any(|pub_| pub_.source() == TrackSource::Screenshare && !pub_.is_muted());
    let identity = p.identity().as_str().to_owned();
    // Identity format from the JWT service: "{user_id}:{device_id}" where
    // user_id is a Matrix ID (@localpart:server).  Split at the second ':'
    // to recover the user_id â€” the first ':' is inside the Matrix ID itself.
    let (user_id, device_id) = split_identity(&identity);
    RtcParticipantInfo {
        participant_id: identity,
        user_id,
        device_id,
        is_audio_muted,
        is_video_muted,
        is_screen_sharing,
    }
}

fn local_participant_info(p: &LocalParticipant) -> RtcParticipantInfo {
    let pubs = p.track_publications();
    let is_audio_muted = !pubs
        .values()
        .any(|pub_| pub_.kind() == TrackKind::Audio && !pub_.is_muted());
    let is_video_muted = !pubs
        .values()
        .any(|pub_| pub_.kind() == TrackKind::Video && !pub_.is_muted());
    let is_screen_sharing = pubs
        .values()
        .any(|pub_| pub_.source() == TrackSource::Screenshare && !pub_.is_muted());
    let identity = p.identity().as_str().to_owned();
    let (user_id, device_id) = split_identity(&identity);
    RtcParticipantInfo {
        participant_id: identity,
        user_id,
        device_id,
        is_audio_muted,
        is_video_muted,
        is_screen_sharing,
    }
}

/// Extract the Matrix user ID prefix from a LiveKit participant identity.
///
/// LiveKit identities have the form `@localpart:server:device_id`.
/// Returns `Some("@localpart:server")` on success, `None` if the format is
/// unexpected (e.g. the identity is not a Matrix user ID).
fn matrix_user_id_from_lk_identity(identity: &str) -> Option<&str> {
    if !identity.starts_with('@') {
        return None;
    }
    let first_colon = identity.find(':')?;
    let after_first = &identity[first_colon + 1..];
    let second_colon = after_first.find(':')?;
    Some(&identity[..first_colon + 1 + second_colon])
}

/// Split a LiveKit identity string of the form `@localpart:server:device_id`
/// into `(user_id, device_id)`.  Returns the full string and an empty device_id
/// if the expected structure is not present.
fn split_identity(identity: &str) -> (String, String) {
    // Matrix IDs start with '@' and contain exactly one ':' for the server part.
    // The JWT service appends a second ':' + device_id suffix.
    if identity.starts_with('@') {
        if let Some(server_colon) = identity.find(':') {
            let after_server = &identity[server_colon + 1..];
            if let Some(device_colon) = after_server.find(':') {
                let uid_end = server_colon + 1 + device_colon;
                return (
                    identity[..uid_end].to_owned(),
                    identity[uid_end + 1..].to_owned(),
                );
            }
        }
    }
    (identity.to_owned(), String::new())
}

/// Software I420 â†’ RGBA conversion from raw planes (BT.601 full-range).
/// Used for the local self-view loopback where we have the planes directly.
/// BT.601 full-range YUV â†’ RGBA using integer fixed-point (1/1024 units).
/// Avoids per-pixel f32 casts and floating-point multiplies.
#[inline(always)]
fn yuv_to_rgba_pixel(y: u8, u: u8, v: u8) -> [u8; 4] {
    let y = y as i32;
    let u = u as i32 - 128;
    let v = v as i32 - 128;
    let r = (y + ((1436 * v) >> 10)).clamp(0, 255) as u8;
    let g = (y - ((352 * u + 731 * v) >> 10)).clamp(0, 255) as u8;
    let b = (y + ((1815 * u) >> 10)).clamp(0, 255) as u8;
    [r, g, b, 255]
}

fn i420_planes_to_rgba(
    y_plane: &[u8],
    u_plane: &[u8],
    v_plane: &[u8],
    width: u32,
    height: u32,
    stride_y: u32,
    stride_uv: u32,
) -> Vec<u8> {
    let w = width as usize;
    let h = height as usize;
    let sy = stride_y as usize;
    let suv = stride_uv as usize;
    let mut rgba = vec![0u8; w * h * 4];
    for row in 0..h {
        for col in 0..w {
            let px = yuv_to_rgba_pixel(
                y_plane[row * sy + col],
                u_plane[(row / 2) * suv + (col / 2)],
                v_plane[(row / 2) * suv + (col / 2)],
            );
            let idx = (row * w + col) * 4;
            rgba[idx..idx + 4].copy_from_slice(&px);
        }
    }
    rgba
}

/// Software I420 â†’ RGBA conversion (BT.601 full-range).
/// Returns None if the received frame buffer isn't I420 type.
fn i420_to_rgba(frame: &BoxVideoFrame) -> Option<Vec<u8>> {
    let buf = frame.buffer.as_ref();
    let w = buf.width() as usize;
    let h = buf.height() as usize;
    let i420 = buf.as_i420()?;
    let (y_data, u_data, v_data) = i420.data();

    if y_data.len() < w * h {
        return None;
    }

    let mut rgba = vec![0u8; w * h * 4];
    for row in 0..h {
        for col in 0..w {
            let px = yuv_to_rgba_pixel(
                y_data[row * w + col],
                u_data[(row / 2) * (w / 2) + (col / 2)],
                v_data[(row / 2) * (w / 2) + (col / 2)],
            );
            let idx = (row * w + col) * 4;
            rgba[idx..idx + 4].copy_from_slice(&px);
        }
    }
    Some(rgba)
}
