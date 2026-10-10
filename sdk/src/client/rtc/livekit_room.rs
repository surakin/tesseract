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
        peer_connection_factory::native::PeerConnectionFactoryExt,
        video_frame::{BoxVideoFrame, I420Buffer, VideoFrame, VideoRotation},
        video_source::{native::NativeVideoSource, RtcVideoSource},
        video_stream::native::NativeVideoStream,
    },
    RoomEvent, RoomOptions,
};

/// E2EE state shared by every LiveKit connection of one call: LiveKit's
/// frame-key store and the peer keys received so far ((Matrix user id,
/// device id) -> key index -> raw key), which are applied to a participant
/// when it appears on any SFU we are connected to. Keys are per device: one
/// account in the call from two devices sends two different keys for the
/// same index, and each must only reach that device's participant.
#[derive(Clone)]
pub struct SharedKeys {
    pub key_provider: KeyProvider,
    pending: Arc<StdMutex<HashMap<(String, String), HashMap<i32, Vec<u8>>>>>,
}

impl SharedKeys {
    pub fn new() -> Self {
        // Match Element Call's key provider: HKDF, ratchet window 10, keyring
        // 256 (MSC4195 / Element's MatrixKeyProvider). PBKDF2 would derive a
        // different key and frames could not be decrypted.
        let key_provider = KeyProvider::new(KeyProviderOptions {
            key_derivation_algorithm: KeyDerivationAlgorithm::HKDF,
            ratchet_window_size: 10,
            failure_tolerance: 10,
            key_ring_size: 256,
            ..KeyProviderOptions::default()
        });
        Self {
            key_provider,
            pending: Arc::new(StdMutex::new(HashMap::new())),
        }
    }

    /// Remember a peer key (all indices are kept so frames encrypted before a
    /// rotation can still be decrypted when the participant first appears).
    /// `sender_device_id` may be empty when the event didn't name one; the
    /// key then applies to every device of the sender (see `key_owner_matches`).
    pub fn store(&self, sender_user_id: &str, sender_device_id: &str, index: i32, raw_key: Vec<u8>) {
        self.pending
            .lock()
            .unwrap()
            .entry((sender_user_id.to_owned(), sender_device_id.to_owned()))
            .or_default()
            .insert(index, raw_key);
    }

    /// Apply every stored key to the participants currently in `room`.
    fn apply_all_to(&self, room: &Room) {
        let pending = self.pending.lock().unwrap();
        for (identity, _) in room.remote_participants() {
            apply_pending_for(&self.key_provider, &pending, &identity);
        }
    }

    /// Apply one key to the matching participants currently in `room`.
    fn apply_one_to(
        &self,
        room: &Room,
        sender_user_id: &str,
        sender_device_id: &str,
        index: i32,
        raw_key: &[u8],
    ) -> usize {
        let mut applied = 0usize;
        for (identity, _) in room.remote_participants() {
            let id_str = identity.as_str();
            if key_owner_matches(id_str, sender_user_id, sender_device_id) {
                info!(
                    "e2ee: set_key for participant {id_str} \
                     (from {sender_user_id}/{sender_device_id} index={index})"
                );
                self.key_provider.set_key(&identity, index, raw_key.to_vec());
                applied += 1;
            }
        }
        applied
    }
}

/// Whether a key sent by `(user_id, device_id)` belongs to the LiveKit
/// participant `identity` (`{user_id}:{device}`, or a bare `{user_id}` from
/// older clients). Without a device the key can't be told apart, so it goes
/// to every device of the user; the user part is parsed with the port-aware
/// `parse_lk_identity`, so `@b:x.org` never matches `@b:x.org:8448:PHONE`.
fn key_owner_matches(identity: &str, user_id: &str, device_id: &str) -> bool {
    if identity == user_id {
        return true;
    }
    if device_id.is_empty() {
        return matches!(parse_lk_identity(identity), Some((user, Some(_))) if user == user_id);
    }
    match identity.strip_prefix(user_id).and_then(|rest| rest.strip_prefix(':')) {
        Some(id_device) => id_device == device_id,
        None => false,
    }
}

/// Set every stored key that belongs to `identity` (`{user_id}:{device}`).
fn apply_pending_for(
    key_provider: &KeyProvider,
    pending: &HashMap<(String, String), HashMap<i32, Vec<u8>>>,
    identity: &ParticipantIdentity,
) {
    let id_str = identity.as_str();
    for ((user_id, device_id), keys_by_index) in pending {
        if key_owner_matches(id_str, user_id, device_id) {
            for (idx, raw_key) in keys_by_index {
                key_provider.set_key(identity, *idx, raw_key.clone());
            }
        }
    }
}

/// Which SFU each live call member publishes on (`(user_id, device_id)` ->
/// JWT service URL), kept current by the session's member reconciler.
#[derive(Clone, Default)]
pub struct MemberTransports(Arc<parking_lot::Mutex<HashMap<(String, String), String>>>);

impl MemberTransports {
    pub fn replace(&self, map: HashMap<(String, String), String>) {
        *self.0.lock() = map;
    }

    /// Whether a LiveKit participant seen on the SFU `sfu_url` is a real call
    /// participant. Element also joins our SFU with a subscribe-only connection
    /// (hashed identity, or the publisher's own identity) that publishes
    /// nothing; showing it would add a phantom tile or clobber the real one.
    /// So an identity must look like `@user:server:DEVICE`, and a known member
    /// must actually publish on this SFU.
    fn allows(&self, sfu_url: &str, identity: &str) -> bool {
        let (user, device) = split_identity(identity);
        if device.is_empty() {
            return false;
        }
        match self.0.lock().get(&(user, device)) {
            Some(published_on) => published_on == sfu_url,
            None => true,
        }
    }
}

/// Forwards call events to the UI, dropping those of participants that are not
/// real publishers on this SFU (see [`MemberTransports::allows`]).
struct FilteredSink {
    inner: Arc<dyn RtcEventSink>,
    sfu_url: String,
    members: MemberTransports,
    /// Our own identity on this connection is always let through.
    local_identity: String,
}

impl FilteredSink {
    fn wrap(
        inner: Option<Arc<dyn RtcEventSink>>,
        sfu_url: &str,
        members: &MemberTransports,
        local_identity: String,
    ) -> Option<Arc<dyn RtcEventSink>> {
        inner.map(|inner| {
            Arc::new(Self {
                inner,
                sfu_url: sfu_url.to_owned(),
                members: members.clone(),
                local_identity,
            }) as Arc<dyn RtcEventSink>
        })
    }

    fn ok(&self, participant_id: &str) -> bool {
        participant_id == self.local_identity || self.members.allows(&self.sfu_url, participant_id)
    }
}

impl RtcEventSink for FilteredSink {
    fn on_invitation(
        &self,
        room_id: &str,
        slot_id: &str,
        caller_user_id: &str,
        call_intent: &str,
        lifetime_ms: u64,
        notification_event_id: &str,
    ) {
        self.inner.on_invitation(
            room_id,
            slot_id,
            caller_user_id,
            call_intent,
            lifetime_ms,
            notification_event_id,
        );
    }
    fn on_participant_joined(&self, session_id: u64, info: RtcParticipantInfo) {
        if self.ok(&info.participant_id) {
            self.inner.on_participant_joined(session_id, info);
        }
    }
    fn on_participant_left(&self, session_id: u64, participant_id: &str) {
        if self.ok(participant_id) {
            self.inner.on_participant_left(session_id, participant_id);
        }
    }
    fn on_participant_updated(&self, session_id: u64, info: RtcParticipantInfo) {
        if self.ok(&info.participant_id) {
            self.inner.on_participant_updated(session_id, info);
        }
    }
    fn on_session_ended(&self, session_id: u64, reason: &str) {
        self.inner.on_session_ended(session_id, reason);
    }
    fn on_video_frame(&self, session_id: u64, participant_id: &str, width: u32, height: u32, rgba: Vec<u8>) {
        if self.ok(participant_id) {
            self.inner.on_video_frame(session_id, participant_id, width, height, rgba);
        }
    }
    fn on_screen_frame(&self, session_id: u64, participant_id: &str, width: u32, height: u32, rgba: Vec<u8>) {
        if self.ok(participant_id) {
            self.inner.on_screen_frame(session_id, participant_id, width, height, rgba);
        }
    }
    fn on_audio_frame(
        &self,
        session_id: u64,
        participant_id: &str,
        samples: &[i16],
        sample_rate: u32,
        num_channels: u32,
    ) {
        if self.ok(participant_id) {
            self.inner
                .on_audio_frame(session_id, participant_id, samples, sample_rate, num_channels);
        }
    }
}

pub struct LiveKitRoom {
    room: Arc<Room>,
    platform_audio: PlatformAudio,
    video_source: NativeVideoSource,
    audio_publication: LocalTrackPublication,
    video_publication: LocalTrackPublication,
    /// The user asked for video but no real camera frame has arrived since.
    /// The camera track is published muted and only unmuted by the next
    /// `push_video_frame_i420`: until a real frame lands, `NativeVideoSource`
    /// feeds its own keepalive frames (an all-zero I420 buffer — green, not
    /// black) and remote clients would render those instead of an avatar.
    /// A mutex rather than an atomic so a concurrent mute can't be undone by
    /// a frame thread that already decided to unmute.
    video_unmute_pending: StdMutex<bool>,
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
    /// Separate gate for the local screen-share self-view loopback.
    local_screen_in_flight: Arc<AtomicBool>,
    local_identity: String,
    sink: Option<Arc<dyn RtcEventSink>>,
    session_id: u64,
    event_task: AbortHandle,
    shared: SharedKeys,
    /// Monotonic start time used to generate RTP timestamps for video frames.
    call_start: std::time::Instant,
}

impl LiveKitRoom {
    /// Connect, publish local audio + video tracks, start the event loop.
    /// This is the primary connection: it publishes on our own homeserver's
    /// SFU, and its `Disconnected` event ends the call session.
    pub async fn connect(
        server_url: &str,
        jwt: &str,
        session_id: u64,
        sink: Option<Arc<dyn RtcEventSink>>,
        use_e2ee: bool,
        shared: SharedKeys,
        sfu_url: &str,
        members: MemberTransports,
    ) -> anyhow::Result<Self> {
        // RoomOptions is #[non_exhaustive] in the external crate — must use
        // Default::default() then field-assign rather than struct literal.
        let mut room_options = RoomOptions::default();
        if use_e2ee {
            room_options.encryption = Some(E2eeOptions {
                encryption_type: EncryptionType::Gcm,
                key_provider: shared.key_provider.clone(),
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
        // Disable the platform ADM's PLAYOUT side (recording stays enabled above,
        // for the mic track). PlatformAudio::new() turns playout on by default,
        // which on Linux auto-selects a native-PipeWire-backed device (the
        // shipped libwebrtc was built with rtc_use_pipewire=true) purely to give
        // AEC3 a speaker-output reference. That native PipeWire client collides
        // with the separate pipewiresrc-based screen-share pipeline in
        // screen_capture_portal.cpp, silently stalling screen-share negotiation
        // whenever a call is active. Disabling it here routes playout through
        // webrtc-sys's SyntheticAudioDevice instead, which keeps pumping AEC3 a
        // correct render reference (via periodic NeedMorePlayData calls) without
        // opening any real playback device. Remote audio is unaffected: it's
        // already rendered separately by our own NativeAudioStream sink (see
        // RtcEventSink::on_audio_frame below, -> tk::AudioPlayback), so the
        // platform ADM's own playout was never used for anything but this AEC3
        // reference in the first place.
        livekit::rtc_engine::lk_runtime::LkRuntime::instance()
            .pc_factory()
            .set_adm_playout_enabled(false);
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
        // Published muted (the AddTrack request carries track.is_muted()):
        // see `video_unmute_pending`.
        local_video.mute();
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
            s.on_participant_joined(session_id, local_participant_info(&room.local_participant()));
        }

        let local_identity = room.local_participant().identity().as_str().to_owned();
        let video_frame_in_flight = Arc::new(AtomicBool::new(false));
        let local_video_in_flight = Arc::new(AtomicBool::new(false));
        let screen_frame_in_flight = Arc::new(AtomicBool::new(false));
        let local_screen_in_flight = Arc::new(AtomicBool::new(false));
        let event_task = spawn_event_task(
            Arc::clone(&room),
            events,
            session_id,
            Arc::clone(&video_frame_in_flight),
            Arc::clone(&screen_frame_in_flight),
            FilteredSink::wrap(sink.clone(), sfu_url, &members, local_identity.clone()),
            shared.clone(),
            true,
        );

        info!("rtc: livekit connected (session {session_id}), local identity={local_identity}");
        Ok(Self {
            room,
            platform_audio,
            video_source,
            audio_publication,
            video_publication,
            video_unmute_pending: StdMutex::new(false),
            video_frame_in_flight,
            local_video_in_flight,
            screen_source: StdMutex::new(None),
            screen_publication: StdMutex::new(None),
            screen_frame_in_flight,
            local_screen_in_flight,
            local_identity,
            sink,
            session_id,
            event_task,
            shared,
            call_start: std::time::Instant::now(),
        })
    }

    pub fn set_audio_muted(&self, muted: bool) {
        if muted {
            self.audio_publication.mute();
        } else {
            self.audio_publication.unmute();
        }
    }

    /// Muting is immediate; unmuting is deferred to the next real camera
    /// frame (see `video_unmute_pending`), so a camera that is missing or
    /// held by another app never goes live as a placeholder stream.
    pub fn set_video_muted(&self, muted: bool) {
        let mut pending = self.video_unmute_pending.lock().unwrap_or_else(|e| e.into_inner());
        if muted {
            *pending = false;
            self.video_publication.mute();
        } else if self.video_publication.is_muted() {
            *pending = true;
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
        {
            let mut pending = self.video_unmute_pending.lock().unwrap_or_else(|e| e.into_inner());
            if *pending {
                *pending = false;
                self.video_publication.unmute();
            }
        }

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
        // LocalTrackPublished only logs (see spawn_event_task below) — emit the
        // participant update ourselves so the UI creates the local ":screen"
        // tile. Without this, is_screen_sharing never flips true for the local
        // participant and the sharer can never see their own capture.
        if let Some(ref s) = self.sink {
            s.on_participant_updated(
                self.session_id,
                local_participant_info(&self.room.local_participant()),
            );
        }
        Ok(())
    }

    /// Mute immediately (stops local encoding/sending) and unpublish the
    /// screen share track. Muting alone leaves the publication registered on
    /// the server forever with only a "muted" flag flipped, which some
    /// clients don't treat as equivalent to the share having ended — remote
    /// viewers could keep showing a stale ":screen" tile. Unpublishing is the
    /// unambiguous signal (RoomEvent::TrackUnpublished on their side) that the
    /// track is gone.
    pub fn stop_screen_share(&self) {
        let pub_ = self.screen_publication.lock().unwrap().take();
        *self.screen_source.lock().unwrap() = None;
        if let Some(pub_) = pub_ {
            pub_.mute();
            let room = Arc::clone(&self.room);
            let sid = pub_.sid();
            tokio::spawn(async move {
                if let Err(e) = room.local_participant().unpublish_track(&sid).await {
                    warn!("rtc: failed to unpublish screen share track: {e}");
                }
            });
        }
        // Explicit symmetric update — don't rely solely on a TrackMuted/
        // TrackUnpublished event round-trip to clear is_screen_sharing for
        // the local participant's own tile.
        if let Some(ref s) = self.sink {
            s.on_participant_updated(
                self.session_id,
                local_participant_info(&self.room.local_participant()),
            );
        }
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

        // Self-view loopback: LiveKit never echoes a published track back to
        // its publisher, so the sharer needs a direct local copy to confirm
        // capture is working (mirrors the camera loopback above).
        if let Some(ref s) = self.sink {
            if self
                .local_screen_in_flight
                .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
            {
                let rgba = i420_planes_to_rgba(y, u, v, width, height, stride_y, stride_u);
                s.on_screen_frame(self.session_id, &self.local_identity, width, height, rgba);
                self.local_screen_in_flight.store(false, Ordering::Release);
            }
        }
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
        self.shared
            .key_provider
            .set_key(&identity, index, raw_key.to_vec());
    }

    /// Apply a peer's key to the matching participants of this connection.
    pub fn apply_peer_key(
        &self,
        sender_user_id: &str,
        sender_device_id: &str,
        index: i32,
        raw_key: &[u8],
    ) -> usize {
        self.shared
            .apply_one_to(&self.room, sender_user_id, sender_device_id, index, raw_key)
    }

    /// Apply every stored peer key to this connection's current participants.
    pub fn apply_all_peer_keys(&self) {
        self.shared.apply_all_to(&self.room);
    }

    pub async fn disconnect(&self) {
        let _ = self.room.close().await;
        self.event_task.abort();
    }
}

/// A subscribe-only connection to another homeserver's SFU. Members on other
/// SFUs publish there (multi-SFU); we never publish on it.
pub struct RemoteSfuRoom {
    room: Arc<Room>,
    session_id: u64,
    sink: Option<Arc<dyn RtcEventSink>>,
    shared: SharedKeys,
    event_task: AbortHandle,
}

impl RemoteSfuRoom {
    pub async fn connect(
        server_url: &str,
        jwt: &str,
        session_id: u64,
        sink: Option<Arc<dyn RtcEventSink>>,
        use_e2ee: bool,
        shared: SharedKeys,
        sfu_url: &str,
        members: MemberTransports,
    ) -> anyhow::Result<Self> {
        let mut room_options = RoomOptions::default();
        if use_e2ee {
            room_options.encryption = Some(E2eeOptions {
                encryption_type: EncryptionType::Gcm,
                key_provider: shared.key_provider.clone(),
            });
        }
        let (room, events) = Room::connect(server_url, jwt, room_options).await?;
        let room = Arc::new(room);
        let local_identity = room.local_participant().identity().as_str().to_owned();
        let event_task = spawn_event_task(
            Arc::clone(&room),
            events,
            session_id,
            Arc::new(AtomicBool::new(false)),
            Arc::new(AtomicBool::new(false)),
            FilteredSink::wrap(sink.clone(), sfu_url, &members, local_identity),
            shared.clone(),
            false,
        );
        info!("rtc: connected to remote SFU {server_url} (session {session_id})");
        let sink = FilteredSink::wrap(sink, sfu_url, &members, String::new());
        Ok(Self { room, session_id, sink, shared, event_task })
    }

    pub fn apply_peer_key(
        &self,
        sender_user_id: &str,
        sender_device_id: &str,
        index: i32,
        raw_key: &[u8],
    ) -> usize {
        self.shared
            .apply_one_to(&self.room, sender_user_id, sender_device_id, index, raw_key)
    }

    pub fn apply_all_peer_keys(&self) {
        self.shared.apply_all_to(&self.room);
    }

    /// Leave this SFU. Its participants vanish from the call, and LiveKit will
    /// not report that once we are disconnected, so tell the UI ourselves.
    pub async fn disconnect(&self) {
        if let Some(ref s) = self.sink {
            for (identity, _) in self.room.remote_participants() {
                s.on_participant_left(self.session_id, identity.as_str());
            }
        }
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
    shared: SharedKeys,
    is_primary: bool,
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
                        // Apply any stored E2EE keys for this participant.
                        apply_pending_for(
                            &shared.key_provider,
                            &shared.pending.lock().unwrap(),
                            &participant.identity(),
                        );
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
                    apply_pending_for(
                        &shared.key_provider,
                        &shared.pending.lock().unwrap(),
                        &p.identity(),
                    );
                }
                RoomEvent::ParticipantDisconnected(p) => {
                    info!("rtc: participant disconnected: {}", p.identity());
                    if let Some(ref s) = sink {
                        s.on_participant_left(session_id, p.identity().as_str());
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
                            // Which stream this is: ask the track itself, not
                            // "does this participant have a screenshare
                            // publication anywhere" — a participant who is
                            // publishing both camera and screen at once (e.g.
                            // screen share was already running when we joined,
                            // so both TrackSubscribed events land close
                            // together) would otherwise classify both tracks
                            // the same way, sending both streams to one tile
                            // while the other shows only an avatar.
                            let is_screen = video_track.source() == TrackSource::Screenshare;
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
                RoomEvent::TrackUnpublished { participant, .. } => {
                    // Authoritative signal that a remote participant's track
                    // (e.g. their screen share) is gone — refresh their info
                    // so is_screen_sharing recomputes to false and the ":screen"
                    // tile is removed, even if a TrackMuted event was missed.
                    if let Some(ref s) = sink {
                        s.on_participant_updated(session_id, participant_info(&participant));
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
                    // Only the primary (publishing) connection ends the call.
                    // A dropped remote SFU is retried by the SFU reconciler.
                    if is_primary {
                        if let Some(ref s) = sink {
                            s.on_session_ended(session_id, &format!("{reason:?}"));
                        }
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
    // user_id is a Matrix ID (@localpart:server_name, where server_name may
    // carry a port).  `split_identity` finds the device boundary, which is not
    // simply the second ':'.
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

/// Parse a LiveKit identity `{user_id}:{device_id}` (user_id =
/// `@localpart:server_name`) into `(user_id, Some(device_id))`.
///
/// The server name may be `host`, `host:port`, `[ipv6]` or `[ipv6]:port`, so
/// the user id can't be cut at a fixed colon. A `:<digits>:` directly after
/// the server name is a port; a bare `:<digits>` at the end is a device.
/// Returns `None` when the identity isn't `@...` (e.g. a hashed identity);
/// the device is `None` when the identity carries no device part.
fn parse_lk_identity(identity: &str) -> Option<(&str, Option<&str>)> {
    if !identity.starts_with('@') {
        return None;
    }
    let Some(first_colon) = identity.find(':') else {
        return Some((identity, None));
    };
    let server_start = first_colon + 1;
    let server = &identity[server_start..];
    let server_len = if server.starts_with('[') {
        match server.find(']') {
            Some(close) => close + 1,
            None => return Some((identity, None)),
        }
    } else {
        server.find(':').unwrap_or(server.len())
    };
    let server_end = server_start + server_len;
    let Some(after) = identity[server_end..].strip_prefix(':') else {
        return Some((identity, None));
    };
    let digits = after.bytes().take_while(u8::is_ascii_digit).count();
    if digits > 0 && after.as_bytes().get(digits) == Some(&b':') {
        let user_end = server_end + 1 + digits;
        return Some((&identity[..user_end], Some(&identity[user_end + 1..])));
    }
    Some((&identity[..server_end], Some(after)))
}

/// Extract the Matrix user ID prefix from a LiveKit participant identity.
///
/// LiveKit identities have the form `@localpart:server_name:device_id`.
/// Returns `Some("@localpart:server_name")` on success, `None` if the format
/// is unexpected (not a Matrix user ID, or no device part).
fn matrix_user_id_from_lk_identity(identity: &str) -> Option<&str> {
    match parse_lk_identity(identity)? {
        (user, Some(_)) => Some(user),
        _ => None,
    }
}

/// Split a LiveKit identity string of the form `@localpart:server_name:device_id`
/// into `(user_id, device_id)`.  Returns the full string and an empty device_id
/// if the expected structure is not present.
fn split_identity(identity: &str) -> (String, String) {
    match parse_lk_identity(identity) {
        Some((user, Some(device))) => (user.to_owned(), device.to_owned()),
        _ => (identity.to_owned(), String::new()),
    }
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

#[cfg(test)]
mod participant_filter_tests {
    use super::*;

    fn members(entries: &[(&str, &str, &str)]) -> MemberTransports {
        let m = MemberTransports::default();
        m.replace(
            entries
                .iter()
                .map(|(u, d, url)| ((u.to_string(), d.to_string()), url.to_string()))
                .collect(),
        );
        m
    }

    #[test]
    fn hashed_identity_is_not_a_participant() {
        let m = members(&[]);
        assert!(!m.allows("https://sfu.a", "W9SGZNapjtEwbxf1GgfA6thQiWT4zZvHrJHxWIj/lJs"));
    }

    #[test]
    fn member_only_counts_on_the_sfu_it_publishes_on() {
        let m = members(&[("@b:matrix.org", "DEV", "https://sfu.matrix.org")]);
        assert!(m.allows("https://sfu.matrix.org", "@b:matrix.org:DEV"));
        // Element's subscribe-only connection to our SFU carries the same identity.
        assert!(!m.allows("https://sfu.gnomos.org", "@b:matrix.org:DEV"));
    }

    #[test]
    fn unknown_member_with_matrix_identity_is_allowed() {
        assert!(members(&[]).allows("https://sfu.a", "@c:x.org:DEV"));
    }

    #[test]
    fn a_device_key_only_matches_that_device() {
        assert!(key_owner_matches("@b:x.org:PHONE", "@b:x.org", "PHONE"));
        assert!(!key_owner_matches("@b:x.org:LAPTOP", "@b:x.org", "PHONE"));
        assert!(!key_owner_matches("@c:x.org:PHONE", "@b:x.org", "PHONE"));
    }

    #[test]
    fn a_key_without_a_device_matches_every_device_of_the_user() {
        assert!(key_owner_matches("@b:x.org:PHONE", "@b:x.org", ""));
        assert!(key_owner_matches("@b:x.org:LAPTOP", "@b:x.org", ""));
        assert!(key_owner_matches("@b:x.org", "@b:x.org", "PHONE"));
        assert!(!key_owner_matches("@bob:x.org:PHONE", "@b:x.org", ""));
    }

    #[test]
    fn a_server_name_with_a_port_still_matches_its_device() {
        assert!(key_owner_matches("@b:x.org:8448:PHONE", "@b:x.org:8448", "PHONE"));
        assert!(!key_owner_matches("@b:x.org:8448:LAPTOP", "@b:x.org:8448", "PHONE"));
        assert!(key_owner_matches("@b:x.org:8448:PHONE", "@b:x.org:8448", ""));
        // A different user, whose id is this one's without the port.
        assert!(!key_owner_matches("@b:x.org:8448:PHONE", "@b:x.org", "PHONE"));
    }

    #[test]
    fn an_empty_device_key_does_not_leak_to_a_ported_server_user() {
        assert!(!key_owner_matches("@b:x.org:8448:PHONE", "@b:x.org", ""));
        assert!(key_owner_matches("@b:x.org:8448:PHONE", "@b:x.org:8448", ""));
        assert!(key_owner_matches("@b:x.org:8448", "@b:x.org:8448", ""));
        assert!(key_owner_matches("@b:[::1]:8448:PHONE", "@b:[::1]:8448", ""));
        assert!(!key_owner_matches("@b:[::1]:8448:PHONE", "@b:[::1]", ""));
    }
}

#[cfg(test)]
mod helper_tests {
    use super::*;
    use std::sync::Mutex;

    #[test]
    fn split_identity_separates_user_and_device() {
        assert_eq!(
            split_identity("@a:x.org:DEV"),
            ("@a:x.org".to_owned(), "DEV".to_owned())
        );
        // Anything after the second colon belongs to the device part.
        assert_eq!(
            split_identity("@a:x.org:DE:V"),
            ("@a:x.org".to_owned(), "DE:V".to_owned())
        );
    }

    #[test]
    fn split_identity_handles_ports_and_ipv6_server_names() {
        let cases = [
            ("@b:x.org:8448:PHONE", "@b:x.org:8448", "PHONE"),
            ("@b:x.org:8448:DE:V", "@b:x.org:8448", "DE:V"),
            ("@b:[::1]:PHONE", "@b:[::1]", "PHONE"),
            ("@b:[::1]:8448:PHONE", "@b:[::1]:8448", "PHONE"),
            // No further ':' after the digits: the digits are the device.
            ("@b:x.org:8448", "@b:x.org", "8448"),
        ];
        for (id, user, device) in cases {
            assert_eq!(split_identity(id), (user.to_owned(), device.to_owned()), "{id}");
        }
        assert_eq!(matrix_user_id_from_lk_identity("@b:x.org:8448:PHONE"), Some("@b:x.org:8448"));
        assert_eq!(matrix_user_id_from_lk_identity("@b:[::1]:8448:P"), Some("@b:[::1]:8448"));
        assert_eq!(matrix_user_id_from_lk_identity("@b:[::1]:P"), Some("@b:[::1]"));
        assert_eq!(matrix_user_id_from_lk_identity("@b:x.org:8448"), Some("@b:x.org"));
    }

    #[test]
    fn split_identity_without_device_returns_whole_string() {
        for id in ["@a:x.org", "@a", "", "hashedidentity", "no:at:prefix"] {
            assert_eq!(split_identity(id), (id.to_owned(), String::new()), "{id}");
        }
    }

    #[test]
    fn matrix_user_id_from_identity_cuts_at_the_second_colon() {
        assert_eq!(matrix_user_id_from_lk_identity("@a:x.org:DEV"), Some("@a:x.org"));
        assert_eq!(matrix_user_id_from_lk_identity("@a:x.org:DE:V"), Some("@a:x.org"));
    }

    #[test]
    fn matrix_user_id_from_identity_rejects_non_matrix_identities() {
        assert_eq!(matrix_user_id_from_lk_identity("a:x.org:DEV"), None);
        assert_eq!(matrix_user_id_from_lk_identity("@a:x.org"), None);
        assert_eq!(matrix_user_id_from_lk_identity("@a"), None);
        assert_eq!(matrix_user_id_from_lk_identity(""), None);
    }

    #[test]
    fn key_owner_bare_user_identity_matches_regardless_of_device() {
        assert!(key_owner_matches("@a:x.org", "@a:x.org", ""));
        assert!(key_owner_matches("@a:x.org", "@a:x.org", "ANY"));
        assert!(!key_owner_matches("", "@a:x.org", "ANY"));
        // Prefix without the ':' separator is a different user.
        assert!(!key_owner_matches("@a:x.orgDEV", "@a:x.org", ""));
    }

    #[test]
    fn copy_plane_tightly_packed_copies_everything() {
        let src = [1u8, 2, 3, 4, 5, 6];
        let mut dst = [0u8; 6];
        copy_plane(&src, &mut dst, 3, 2, 3);
        assert_eq!(dst, src);
    }

    #[test]
    fn copy_plane_strips_row_padding() {
        // width 2, stride 4: each row has two padding bytes (9) to drop.
        let src = [1u8, 2, 9, 9, 3, 4, 9, 9];
        let mut dst = [0u8; 4];
        copy_plane(&src, &mut dst, 2, 2, 4);
        assert_eq!(dst, [1, 2, 3, 4]);
    }

    #[test]
    fn copy_plane_zero_rows_is_a_noop() {
        let mut dst = [7u8; 2];
        copy_plane(&[], &mut dst, 2, 0, 4);
        assert_eq!(dst, [7, 7]);
    }

    #[test]
    fn yuv_extremes_map_to_white_black_and_grey() {
        assert_eq!(yuv_to_rgba_pixel(255, 128, 128), [255, 255, 255, 255]);
        assert_eq!(yuv_to_rgba_pixel(0, 128, 128), [0, 0, 0, 255]);
        assert_eq!(yuv_to_rgba_pixel(128, 128, 128), [128, 128, 128, 255]);
    }

    #[test]
    fn yuv_chroma_pushes_channels_the_right_way() {
        // High V (Cr) raises red and lowers green; high U (Cb) raises blue.
        let red = yuv_to_rgba_pixel(76, 85, 255);
        assert!(red[0] > 240 && red[1] < 10 && red[2] < 10, "{red:?}");
        let blue = yuv_to_rgba_pixel(29, 255, 107);
        assert!(blue[2] > 240 && blue[0] < 10, "{blue:?}");
        // Out-of-gamut input is clamped rather than wrapping.
        let hot = yuv_to_rgba_pixel(255, 255, 255);
        assert_eq!(hot[0], 255);
        assert_eq!(hot[3], 255);
    }

    #[test]
    fn i420_planes_to_rgba_has_expected_size_and_opaque_alpha() {
        let y = vec![255u8; 4 * 2];
        let u = vec![128u8; 2];
        let v = vec![128u8; 2];
        let out = i420_planes_to_rgba(&y, &u, &v, 4, 2, 4, 2);
        assert_eq!(out.len(), 4 * 2 * 4);
        assert!(out.chunks(4).all(|p| p == [255, 255, 255, 255]));
    }

    #[test]
    fn i420_planes_to_rgba_honours_strides_and_chroma_subsampling() {
        // 2x2 image, y stride 4 (2 padding bytes of 0xEE), uv stride 3.
        let y = [10u8, 200, 0xEE, 0xEE, 30, 100, 0xEE, 0xEE];
        let u = [128u8, 0xEE, 0xEE];
        let v = [128u8, 0xEE, 0xEE];
        let out = i420_planes_to_rgba(&y, &u, &v, 2, 2, 4, 3);
        let grey = |p: usize| out[p * 4];
        assert_eq!([grey(0), grey(1), grey(2), grey(3)], [10, 200, 30, 100]);
        for p in 0..4 {
            assert_eq!(&out[p * 4..p * 4 + 4], [grey(p), grey(p), grey(p), 255]);
        }
    }

    #[test]
    fn shared_keys_store_keeps_one_entry_per_device_and_index() {
        let keys = SharedKeys::new();
        keys.store("@a:x", "PHONE", 0, vec![1]);
        keys.store("@a:x", "PHONE", 1, vec![2]);
        keys.store("@a:x", "LAPTOP", 0, vec![3]);
        keys.store("@a:x", "PHONE", 0, vec![4]); // overwrite
        let p = keys.pending.lock().unwrap();
        assert_eq!(p.len(), 2);
        let phone = &p[&("@a:x".to_owned(), "PHONE".to_owned())];
        assert_eq!(phone.len(), 2);
        assert_eq!(phone[&0], vec![4]);
        assert_eq!(phone[&1], vec![2]);
        assert_eq!(p[&("@a:x".to_owned(), "LAPTOP".to_owned())][&0], vec![3]);
    }

    #[test]
    fn member_transports_replace_swaps_the_whole_map() {
        let m = MemberTransports::default();
        m.replace(HashMap::from([(("@a:x".to_owned(), "D".to_owned()), "sfu1".to_owned())]));
        assert!(m.allows("sfu1", "@a:x:D"));
        assert!(!m.allows("sfu2", "@a:x:D"));
        m.replace(HashMap::new());
        // Unknown now, so allowed anywhere.
        assert!(m.allows("sfu2", "@a:x:D"));
    }

    /// Records which participant ids reached the inner sink, per event kind.
    #[derive(Default)]
    struct Recorder(Mutex<Vec<String>>);

    impl RtcEventSink for Recorder {
        fn on_invitation(&self, room_id: &str, _: &str, _: &str, _: &str, _: u64, _: &str) {
            self.0.lock().unwrap().push(format!("invite:{room_id}"));
        }
        fn on_participant_joined(&self, _: u64, info: RtcParticipantInfo) {
            self.0.lock().unwrap().push(format!("joined:{}", info.participant_id));
        }
        fn on_participant_left(&self, _: u64, id: &str) {
            self.0.lock().unwrap().push(format!("left:{id}"));
        }
        fn on_participant_updated(&self, _: u64, info: RtcParticipantInfo) {
            self.0.lock().unwrap().push(format!("updated:{}", info.participant_id));
        }
        fn on_session_ended(&self, _: u64, reason: &str) {
            self.0.lock().unwrap().push(format!("ended:{reason}"));
        }
        fn on_video_frame(&self, _: u64, id: &str, _: u32, _: u32, _: Vec<u8>) {
            self.0.lock().unwrap().push(format!("video:{id}"));
        }
        fn on_screen_frame(&self, _: u64, id: &str, _: u32, _: u32, _: Vec<u8>) {
            self.0.lock().unwrap().push(format!("screen:{id}"));
        }
        fn on_audio_frame(&self, _: u64, id: &str, _: &[i16], _: u32, _: u32) {
            self.0.lock().unwrap().push(format!("audio:{id}"));
        }
    }

    fn info(id: &str) -> RtcParticipantInfo {
        RtcParticipantInfo {
            participant_id: id.to_owned(),
            ..Default::default()
        }
    }

    #[test]
    fn filtered_sink_wrap_of_none_is_none() {
        let m = MemberTransports::default();
        assert!(FilteredSink::wrap(None, "sfu", &m, "@me:x:D".into()).is_none());
    }

    #[test]
    fn filtered_sink_forwards_real_publishers_and_drops_phantoms() {
        let rec = Arc::new(Recorder::default());
        let members = MemberTransports::default();
        members.replace(HashMap::from([
            (("@b:x".to_owned(), "D".to_owned()), "sfu-here".to_owned()),
            (("@c:x".to_owned(), "D".to_owned()), "sfu-elsewhere".to_owned()),
        ]));
        let sink = FilteredSink::wrap(
            Some(rec.clone() as Arc<dyn RtcEventSink>),
            "sfu-here",
            &members,
            "@me:x:D".into(),
        )
        .unwrap();

        sink.on_participant_joined(1, info("@b:x:D")); // publishes here
        sink.on_participant_joined(1, info("@c:x:D")); // publishes elsewhere
        sink.on_participant_joined(1, info("hashedidentity")); // not a matrix identity
        sink.on_participant_updated(1, info("@c:x:D"));
        sink.on_participant_left(1, "@c:x:D");
        sink.on_video_frame(1, "@c:x:D", 1, 1, vec![]);
        sink.on_screen_frame(1, "@b:x:D", 1, 1, vec![]);
        sink.on_audio_frame(1, "hashedidentity", &[0], 48000, 1);
        sink.on_audio_frame(1, "@b:x:D", &[0], 48000, 1);

        assert_eq!(
            *rec.0.lock().unwrap(),
            vec!["joined:@b:x:D", "screen:@b:x:D", "audio:@b:x:D"]
        );
    }

    #[test]
    fn filtered_sink_always_passes_local_identity_and_session_level_events() {
        let rec = Arc::new(Recorder::default());
        let members = MemberTransports::default();
        // Our own identity is "known" to publish on a different SFU, yet passes.
        members.replace(HashMap::from([(
            ("@me:x".to_owned(), "D".to_owned()),
            "other-sfu".to_owned(),
        )]));
        let sink = FilteredSink::wrap(
            Some(rec.clone() as Arc<dyn RtcEventSink>),
            "this-sfu",
            &members,
            "@me:x:D".into(),
        )
        .unwrap();
        sink.on_participant_joined(2, info("@me:x:D"));
        sink.on_session_ended(2, "bye");
        sink.on_invitation("!r:x", "slot", "@b:x", "audio", 0, "");
        assert_eq!(
            *rec.0.lock().unwrap(),
            vec!["joined:@me:x:D", "ended:bye", "invite:!r:x"]
        );
    }
}
