#![cfg(feature = "calls")]

use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

use base64ct::{Base64, Encoding as _};

use anyhow::Context;
use matrix_sdk::Room;
use parking_lot::Mutex as PLMutex;
use tokio::task::AbortHandle;
use tracing::{info, warn};

use super::{
    e2ee::E2eeManager,
    livekit_room::LiveKitRoom,
    signaling::{
        self, send_msc3401_call_open, send_msc3401_member_join, send_msc3401_member_leave,
        RtcMemberEventContent,
    },
    transport::{fetch_livekit_jwt, fetch_livekit_service_url, livekit_room_alias},
    RtcParticipantInfo,
};

static SESSION_ID_COUNTER: AtomicU64 = AtomicU64::new(1);

fn next_session_id() -> u64 {
    SESSION_ID_COUNTER.fetch_add(1, Ordering::Relaxed)
}

/// Generate a UUID v4 string using OS-level randomness.
fn new_member_id() -> String {
    use rand::RngCore;
    let mut b = [0u8; 16];
    rand::thread_rng().fill_bytes(&mut b);
    b[6] = (b[6] & 0x0f) | 0x40; // version 4
    b[8] = (b[8] & 0x3f) | 0x80; // variant 10xx
    format!(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        b[0], b[1], b[2], b[3],
        b[4], b[5],
        b[6], b[7],
        b[8], b[9],
        b[10], b[11], b[12], b[13], b[14], b[15]
    )
}

/// An active MatrixRTC call session. Drop ends the session (aborts background
/// tasks). For a graceful leave, call `end_call()` before dropping.
pub struct RtcSession {
    pub id: u64,
    pub room_id: String,
    pub slot_id: String,
    pub member_id: String,
    pub device_id: String,
    client: matrix_sdk::Client,
    /// Periodic sticky-event refresh task.
    refresh_task: AbortHandle,
    /// Key-rotation task: rotates and rebroadcasts the frame key whenever a
    /// participant leaves (forward secrecy). None in unencrypted rooms.
    rotate_task: Option<AbortHandle>,
    /// Rebroadcast task: re-sends the current frame key whenever a participant
    /// joins so they can decrypt our media even if they missed the initial
    /// broadcast. None in unencrypted rooms.
    rebroadcast_task: Option<AbortHandle>,
    lk: Arc<LiveKitRoom>,
    e2ee: Arc<PLMutex<E2eeManager>>,
    /// Removes the m.rtc.encryption_key to-device handler when session is dropped.
    _enc_key_guard: matrix_sdk::event_handler::EventHandlerDropGuard,
}

impl RtcSession {
    pub fn mute_audio(&self, muted: bool) {
        self.lk.set_audio_muted(muted);
    }

    pub fn mute_video(&self, muted: bool) {
        self.lk.set_video_muted(muted);
    }

    /// Inject an I420 video frame from Layer 3 (VideoCaptureCallSession).
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
        self.lk
            .push_video_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
    }
}

impl Drop for RtcSession {
    fn drop(&mut self) {
        self.refresh_task.abort();
        if let Some(ref h) = self.rotate_task {
            h.abort();
        }
        if let Some(ref h) = self.rebroadcast_task {
            h.abort();
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Start a call in `room_id` under `slot_id` (typically `"call#default"`).
///
/// Performs: transport discovery → JWT acquisition → m.rtc.member join event →
/// LiveKit connection → E2EE key broadcast → sticky-refresh task.
pub async fn start_call(
    client: &matrix_sdk::Client,
    http: &reqwest::Client,
    room_id: &str,
    slot_id: &str,
) -> anyhow::Result<RtcSession> {
    // "call#default" and "" are Tesseract/C++ labels for the default room call.
    // Element uses "m.call#ROOM" as the canonical slot_id sent to the JWT service.
    // Normalize to match, so both parties get a JWT for the same LiveKit room.
    let slot_id = match slot_id {
        "" | "call#default" => "m.call#ROOM",
        other => other,
    };

    let session_id = next_session_id();
    let member_id = new_member_id();

    let room_oid: matrix_sdk::ruma::OwnedRoomId =
        room_id.parse().context("invalid room_id")?;
    let room = client
        .get_room(&room_oid)
        .ok_or_else(|| anyhow::anyhow!("room not found: {room_id}"))?;

    // Transport discovery
    let hs_url = client.homeserver().to_string();
    let access_token = client
        .access_token()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?;
    let uid = client
        .user_id()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?;
    let server_name = uid.server_name().as_str();
    let service_url =
        fetch_livekit_service_url(http, &hs_url, &access_token, server_name).await?;

    let lk_alias = livekit_room_alias(room_id, slot_id);

    let user_id = uid.to_string();
    let device_id = client
        .device_id()
        .ok_or_else(|| anyhow::anyhow!("device id not available"))?
        .to_string();

    // OpenID token → JWT
    let openid = get_openid_token(client).await?;
    let lk_transport = fetch_livekit_jwt(
        http,
        &service_url,
        room_id,
        slot_id,
        &openid.access_token,
        openid.expires_in,
        &openid.matrix_server_name,
        &member_id,
        &device_id,
        &user_id,
    )
    .await?;

    // ── E2EE negotiation ────────────────────────────────────────────────────
    // Only enable frame encryption when the Matrix room is encrypted.
    // In unencrypted rooms, peers don't send m.rtc.encryption_key events and
    // have no key to decrypt our frames, so encrypting would break the call.
    let use_e2ee = matches!(
        room.encryption_state(),
        matrix_sdk_base::EncryptionState::Encrypted
    );
    info!("rtc: room encrypted={use_e2ee}");

    // ── Pre-flight: learn our LiveKit identity from the JWT ───────────────
    // The JWT service assigns the LiveKit participant identity in the `sub`
    // claim. Decode it without verifying the signature so we can send Matrix
    // signaling BEFORE connecting to LiveKit.
    //
    // Sending m.rtc.member first ensures that when we appear in the LiveKit
    // room the receiving client (e.g. Element Android) already has our state
    // event and can immediately associate our tracks with our Matrix identity.
    // Without this, Element subscribes our tracks as an "unknown" participant
    // and never re-attaches them after the state event later arrives.
    let preflight_identity =
        super::transport::decode_jwt_sub(&lk_transport.jwt);

    // ── E2EE key handler ─────────────────────────────────────────────────
    // Register BEFORE signaling so we buffer any key events the peer sends
    // immediately on receiving our m.rtc.member. lk_holder starts as None
    // (pre-connect); it is populated after LiveKit connects and buffered keys
    // are drained at that point.
    let early_keys: Arc<PLMutex<Vec<(String, i32, Vec<u8>)>>> =
        Arc::new(PLMutex::new(Vec::new()));
    let lk_holder: Arc<PLMutex<Option<Arc<LiveKitRoom>>>> =
        Arc::new(PLMutex::new(None));

    let early_keys_h = Arc::clone(&early_keys);
    let lk_holder_h = Arc::clone(&lk_holder);
    let enc_key_handle = client.add_event_handler({
        move |ev: matrix_sdk::ruma::events::ToDeviceEvent<
            crate::client::rtc::signaling::RtcEncryptionKeyEventContent,
        >| {
            let early = Arc::clone(&early_keys_h);
            let holder = Arc::clone(&lk_holder_h);
            async move {
                let sender = ev.sender.to_string();
                let index = ev.content.keys.index as i32;
                let key_b64_len = ev.content.keys.key.len();
                match Base64::decode_vec(&ev.content.keys.key) {
                    Ok(raw_key) if raw_key.is_empty() => {
                        warn!("e2ee: peer key from {sender} index={index} decoded to empty bytes — dropping");
                    }
                    Ok(raw_key) => {
                        let lk_opt = holder.lock().clone();
                        if let Some(lk) = lk_opt {
                            lk.queue_peer_key(&sender, index, raw_key);
                        } else {
                            early.lock().push((sender, index, raw_key));
                        }
                    }
                    Err(e) => warn!(
                        "e2ee: bad base64 in peer key from {sender} index={index} \
                         key_b64_len={key_b64_len}: {e}"
                    ),
                }
            }
        }
    });
    let enc_key_guard = client.event_handler_drop_guard(enc_key_handle);

    // ── Pre-flight Matrix signaling ───────────────────────────────────────
    // Send join events using the JWT-decoded identity BEFORE connecting to
    // LiveKit. If JWT decode fails (should not happen with a well-formed token)
    // we skip and re-send after connecting with the actual identity.
    if let Some(ref pid) = preflight_identity {
        info!("rtc: pre-flight signaling with identity={pid}");
        send_msc3401_call_open(&room, "").await?;
        send_msc3401_member_join(
            &room, "", &device_id, pid, &lk_transport.service_url, &lk_alias, &user_id,
        )
        .await?;
    }

    // MSC4075: send ring notification only if we are the first participant.
    // Our own pre-flight send_msc3401_member_join() hasn't been echoed back
    // through sync yet, so the local state cache only reflects other users.
    {
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::SyncStateEvent;

        let existing = room
            .get_state_events_static::<RtcMemberEventContent>()
            .await
            .unwrap_or_default();

        let has_active_members = existing.iter().any(|raw| {
            let Ok(ev) = raw.deserialize() else { return false };
            let content = match ev {
                SyncOrStrippedState::Sync(SyncStateEvent::Original(o)) => o.content,
                _ => return false,
            };
            // Leave events carry only disconnect_reason; join events have
            // application/focus_active/slot_id populated. Mirrors the check
            // in register_invitation_handler.
            !content.application.kind.is_empty()
                || content.focus_active.is_some()
                || !content.slot_id.is_empty()
        });

        if !has_active_members {
            if let Err(e) =
                signaling::send_rtc_notification(&room, "video", &device_id, &user_id).await
            {
                warn!("rtc: send_rtc_notification failed (non-fatal): {e}");
            }
        }
    }

    // ── Connect to LiveKit ────────────────────────────────────────────────
    // Create the rotation channel before connecting so the event loop can fire
    // rotation triggers as soon as participants start disconnecting.
    let (rotate_tx, rotate_rx) = if use_e2ee {
        let (tx, rx) = tokio::sync::mpsc::channel::<()>(4);
        (Some(tx), Some(rx))
    } else {
        (None, None)
    };
    let (rebroadcast_tx, rebroadcast_rx) = if use_e2ee {
        let (tx, rx) = tokio::sync::mpsc::channel::<String>(16);
        (Some(tx), Some(rx))
    } else {
        (None, None)
    };

    let sink = super::global_sink();
    let lk = Arc::new(
        LiveKitRoom::connect(
            &lk_transport.server_url,
            &lk_transport.jwt,
            session_id,
            sink,
            use_e2ee,
            rotate_tx,
            rebroadcast_tx,
        )
        .await?,
    );
    let lk_identity = lk.local_identity().to_owned();
    info!("rtc: lk connected, identity={lk_identity} (preflight={preflight_identity:?})");

    // ── Activate key holder and drain buffered keys ───────────────────────
    *lk_holder.lock() = Some(Arc::clone(&lk));
    for (sender, index, raw_key) in early_keys.lock().drain(..) {
        lk.queue_peer_key(&sender, index, raw_key);
    }

    // ── Correct signaling if the actual identity differs from preflight ───
    // Normally they match. A mismatch would indicate a JWT service change or
    // decode error; re-send the correct events so Element can track us.
    if preflight_identity.as_deref() != Some(lk_identity.as_str()) {
        warn!(
            "rtc: identity mismatch — preflight={preflight_identity:?} actual={lk_identity}; \
             re-sending signaling"
        );
        send_msc3401_call_open(&room, "").await?;
        send_msc3401_member_join(
            &room,
            "",
            &device_id,
            &lk_identity,
            &lk_transport.service_url,
            &lk_alias,
            &user_id,
        )
        .await?;
    }

    // Initialise E2EE only for encrypted rooms.
    let e2ee = Arc::new(PLMutex::new(E2eeManager::new()));
    if use_e2ee {
        let mgr = e2ee.lock();
        // Set our own frame key first so the FrameCryptor can start encrypting
        // before we broadcast (avoids sending the key before frames are ready).
        lk.set_own_frame_key(mgr.own_raw_key(), 0);
        mgr.broadcast_own_key(client, &room, &lk_identity)
            .await
            .unwrap_or_else(|e| warn!("e2ee broadcast failed: {e}"));
    }

    // Spawn the key-rotation task. Fires whenever the LiveKit event loop sends
    // a () on rotate_rx (i.e. on every ParticipantDisconnected). Absent for
    // unencrypted rooms (rotate_rx is None).
    let rotate_task = rotate_rx.map(|mut rx| {
        let e2ee_r = Arc::clone(&e2ee);
        let lk_r = Arc::clone(&lk);
        let client_r = client.clone();
        let room_r = room.clone();
        let identity_r = lk_identity.clone();
        tokio::spawn(async move {
            while rx.recv().await.is_some() {
                // Rotate and snapshot — both synchronous, guard drops before
                // the await.  parking_lot::MutexGuard is !Send so we must not
                // hold it across an async boundary.
                let (new_idx, raw, key_b64): (u8, super::e2ee::KeyMaterial, String) = {
                    let mut mgr = e2ee_r.lock();
                    let (idx, b64) = mgr.rotate();
                    (idx, *mgr.own_raw_key(), b64)
                };
                lk_r.set_own_frame_key(&raw, new_idx as i32);
                super::e2ee::broadcast_key(
                    &client_r,
                    &room_r,
                    &identity_r,
                    &key_b64,
                    new_idx,
                )
                .await
                .unwrap_or_else(|e| warn!("e2ee rebroadcast after participant leave: {e}"));
            }
        })
        .abort_handle()
    });

    // Spawn the rebroadcast task. Fires whenever a participant connects
    // (ParticipantConnected or pre-existing at Connected), carrying their
    // Matrix user ID extracted from the LiveKit identity.  Uses
    // send_frame_key_to_user which bypasses room.members() and performs a
    // fresh /keys/query when their devices aren't in the local cache.
    let rebroadcast_task = rebroadcast_rx.map(|mut rx| {
        let e2ee_r    = Arc::clone(&e2ee);
        let client_r  = client.clone();
        let room_id_r = room.room_id().to_string();
        let identity_r = lk_identity.clone();
        tokio::spawn(async move {
            while let Some(matrix_user_id) = rx.recv().await {
                let (idx, key_b64): (u8, String) = {
                    let mgr = e2ee_r.lock();
                    (mgr.own_index(), mgr.own_key_b64())
                };
                super::e2ee::send_frame_key_to_user(
                    &client_r,
                    &matrix_user_id,
                    &room_id_r,
                    &identity_r,
                    &key_b64,
                    idx,
                )
                .await
                .unwrap_or_else(|e| warn!("e2ee rebroadcast to {matrix_user_id}: {e}"));
            }
        })
        .abort_handle()
    });

    // Sticky-refresh background task (uses lk_identity as member_id to match
    // what we sent in the initial m.rtc.member and m.call.member events above).
    let refresh_task = spawn_refresh_task(
        room.clone(),
        lk_identity.clone(),
        service_url.clone(),
        lk_alias.clone(),
        device_id.clone(),
        user_id.clone(),
    );

    info!("rtc: session {session_id} started in {room_id}/{slot_id}");
    Ok(RtcSession {
        id: session_id,
        room_id: room_id.to_owned(),
        slot_id: slot_id.to_owned(),
        member_id: lk_identity,
        device_id,
        client: client.clone(),
        refresh_task,
        rotate_task,
        rebroadcast_task,
        lk,
        e2ee,
        _enc_key_guard: enc_key_guard,
    })
}

/// Gracefully leave the call: send disconnect events, tear down LiveKit, abort tasks.
pub async fn end_call(session: &RtcSession) {
    session.refresh_task.abort();
    let room_oid: matrix_sdk::ruma::OwnedRoomId =
        session.room_id.parse().unwrap_or_else(|_| unreachable!());
    if let Some(room) = session.client.get_room(&room_oid) {
        let user_id = session.client.user_id().map(|u| u.to_string()).unwrap_or_default();
        // MSC3401 leave — state key must match the join (uses session member_id, not Matrix device_id)
        let _ = send_msc3401_member_leave(&room, &user_id, &session.member_id).await;
    }
    session.lk.disconnect().await;
}

// ---------------------------------------------------------------------------
// Invitation watcher (registered once at sync start)
// ---------------------------------------------------------------------------

/// Register a global m.rtc.member handler that fires `on_invitation` when
/// another user opens a call slot. Called from sync.rs::start_sync.
pub fn register_invitation_handler(client: &matrix_sdk::Client) {
    use matrix_sdk::ruma::events::SyncStateEvent;

    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: SyncStateEvent<RtcMemberEventContent>, room: Room| {
            let client = client_clone.clone();
            async move {
                let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
                let own_device = client.device_id().map(|d| d.to_string()).unwrap_or_default();
                let sender = ev.sender().to_string();
                let room_id = room.room_id().to_string();

                // Ignore only events from our own device (state key = member_id we sent).
                let content = match &ev {
                    SyncStateEvent::Original(o) => &o.content,
                    SyncStateEvent::Redacted(_) => {
                        info!("rtc: redacted m.rtc.member from {sender} in {room_id}");
                        return;
                    }
                };

                // MSC4143 state key = member_id; our member_id is set per-session so we
                // can't compare it here. Fall back to ignoring our own user+device combo
                // via the device_id field in the content.
                if sender == own_user && content.device_id == own_device {
                    return;
                }

                // Leave events only carry `disconnect_reason` — all join-state
                // fields are empty. Don't surface these as new invitations.
                let is_join = !content.application.kind.is_empty()
                    || content.focus_active.is_some()
                    || !content.slot_id.is_empty();

                if !is_join {
                    return;
                }

                // slot_id is present in our events; Element Call omits it and
                // uses the room-scope default slot implicitly.
                let slot_id = if content.slot_id.is_empty() {
                    "call#default".to_owned()
                } else {
                    content.slot_id.clone()
                };
                if let Some(sink) = super::global_sink() {
                    sink.on_invitation(&room_id, &slot_id, &sender, "", 0, "");
                }
            }
        },
    );
}

/// Register a handler for MSC3401 `org.matrix.msc3401.call.member` events.
/// Element Web uses this format. Called alongside `register_invitation_handler`.
pub fn register_msc3401_invitation_handler(client: &matrix_sdk::Client) {
    use matrix_sdk::ruma::events::{
        SyncStateEvent,
        call::member::{Application, CallMemberEventContent},
    };

    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: SyncStateEvent<CallMemberEventContent>, room: Room| {
            let client = client_clone.clone();
            async move {
                let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
                let own_device = client.device_id().map(|d| d.to_string()).unwrap_or_default();
                let sender = ev.sender().to_string();
                let room_id = room.room_id().to_string();

                // Ignore only events from our own device (not from ourselves on Element/another device).
                let own_state_key = format!("_{}_{}_m.call", own_user, own_device);
                let (content, event_state_key) = match &ev {
                    SyncStateEvent::Original(o) => (&o.content, o.state_key.as_ref()),
                    SyncStateEvent::Redacted(_) => return,
                };

                if event_state_key == own_state_key {
                    return;
                }

                // Empty variant = leave; LegacyContent / SessionContent = join.
                if matches!(content, CallMemberEventContent::Empty(_)) {
                    return;
                }

                let call_id = content
                    .memberships()
                    .into_iter()
                    .find_map(|m| {
                        if let Application::Call(app) = m.application() {
                            Some(app.call_id.clone())
                        } else {
                            None
                        }
                    })
                    .unwrap_or_default();

                let slot_id = call_id;
                if let Some(sink) = super::global_sink() {
                    sink.on_invitation(&room_id, &slot_id, &sender, "", 0, "");
                }
            }
        },
    );
}

/// Register a handler for stable `m.rtc.notification` and unstable
/// `org.matrix.msc4075.rtc.notification` events (MSC4075).
/// Fires `on_invitation` when a ring notification arrives from another user.
pub fn register_rtc_notification_handler(client: &matrix_sdk::Client) {
    use matrix_sdk::ruma::events::{
        SyncMessageLikeEvent,
        rtc::notification::{CallIntent, NotificationType, RtcNotificationEventContent},
    };

    // Stable prefix: m.rtc.notification
    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: SyncMessageLikeEvent<RtcNotificationEventContent>, room: Room| {
            let client = client_clone.clone();
            async move {
                let orig = match &ev {
                    SyncMessageLikeEvent::Original(o) => o,
                    SyncMessageLikeEvent::Redacted(_) => return,
                };
                let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
                if orig.sender.to_string() == own_user {
                    return;
                }
                if orig.content.notification_type != NotificationType::Ring {
                    return;
                }

                let expiry = orig.content.expiration_ts(orig.origin_server_ts, None);
                let now_ms = {
                    use std::time::{SystemTime, UNIX_EPOCH};
                    SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as u64
                };
                let remaining_ms = u64::from(expiry.get()).saturating_sub(now_ms).min(120_000);
                if remaining_ms == 0 {
                    return;
                }

                let call_intent = match &orig.content.call_intent {
                    Some(CallIntent::Audio) => "audio",
                    Some(CallIntent::Video) => "video",
                    _ => "",
                };
                let notification_event_id = orig.event_id.to_string();
                let sender = orig.sender.to_string();
                let room_id = room.room_id().to_string();

                // TODO MSC4075: send m.call.ring.ack once caller device_id is available
                // (the stable notification event does not carry caller device_id)

                if let Some(sink) = super::global_sink() {
                    sink.on_invitation(
                        &room_id,
                        "call#default",
                        &sender,
                        call_intent,
                        remaining_ms,
                        &notification_event_id,
                    );
                }
            }
        },
    );

    // Unstable prefix: org.matrix.msc4075.rtc.notification (Element Web compat)
    let client_clone2 = client.clone();
    client.add_event_handler(
        move |ev: SyncMessageLikeEvent<
            crate::client::rtc::signaling::Msc4075RtcNotificationEventContent,
        >,
              room: Room| {
            let client = client_clone2.clone();
            async move {
                let orig = match &ev {
                    SyncMessageLikeEvent::Original(o) => o,
                    SyncMessageLikeEvent::Redacted(_) => return,
                };
                let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
                if orig.sender.to_string() == own_user {
                    return;
                }
                if orig.content.notification_type != "ring" {
                    return;
                }

                let now_ms = {
                    use std::time::{SystemTime, UNIX_EPOCH};
                    SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as u64
                };
                let expiry_ms = orig.content.sender_ts.saturating_add(orig.content.lifetime);
                let remaining_ms = expiry_ms.saturating_sub(now_ms).min(120_000);
                if remaining_ms == 0 {
                    return;
                }

                let call_intent = orig.content.call_intent.as_deref().unwrap_or("");
                let notification_event_id = orig.event_id.to_string();
                let sender = orig.sender.to_string();
                let room_id = room.room_id().to_string();

                if let Some(sink) = super::global_sink() {
                    sink.on_invitation(
                        &room_id,
                        "call#default",
                        &sender,
                        call_intent,
                        remaining_ms,
                        &notification_event_id,
                    );
                }
            }
        },
    );
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

struct OpenIdToken {
    access_token: String,
    expires_in: u64,
    matrix_server_name: String,
}

async fn get_openid_token(client: &matrix_sdk::Client) -> anyhow::Result<OpenIdToken> {
    use matrix_sdk::ruma::api::client::account::request_openid_token;

    let user_id = client
        .user_id()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?
        .to_owned();
    let resp = client
        .send(request_openid_token::v3::Request::new(user_id))
        .await
        .context("request_openid_token")?;
    Ok(OpenIdToken {
        access_token: resp.access_token.to_string(),
        expires_in: resp.expires_in.as_secs(),
        matrix_server_name: resp.matrix_server_name.to_string(),
    })
}

/// Periodic task: re-sends both MSC3401 and MSC4143 membership events every
/// 25 seconds to keep sticky events alive (typical expiry window is 30 s).
fn spawn_refresh_task(
    room: Room,
    member_id: String,
    service_url: String,
    lk_alias: String,
    device_id: String,
    user_id: String,
) -> AbortHandle {
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(25));
        interval.tick().await; // skip the immediate 0-delay first tick
        loop {
            interval.tick().await;
            let _ = send_msc3401_member_join(
                &room, "", &device_id, &member_id, &service_url, &lk_alias, &user_id,
            )
            .await;
        }
    })
    .abort_handle()
}
