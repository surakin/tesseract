use std::{
    collections::HashSet,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};

use base64ct::{Base64, Encoding as _};

use anyhow::Context;
use matrix_sdk::Room;
use parking_lot::Mutex as PLMutex;
use tokio::task::AbortHandle;
use tracing::{info, warn};

use super::{
    e2ee::{broadcast_key_to_members, E2eeManager},
    livekit_room::{LiveKitRoom, MemberTransports, SharedKeys},
    members::{desired_sfus, read_live_members, transport_map, LkFocus},
    sfu_set::{SfuContext, SfuSet},
    signaling::{
        self, send_msc3401_call_open, send_msc3401_member_join, send_msc3401_member_leave,
        DelayedRestart, RtcMemberEventContent,
    },
    transport::{fetch_livekit_jwt, fetch_livekit_service_url, livekit_room_alias},
    RtcParticipantInfo,
};

static SESSION_ID_COUNTER: AtomicU64 = AtomicU64::new(1);

/// Dead-man's switch: the homeserver sends our leave this long after our last
/// heartbeat (matches matrix-js-sdk's default), so a crash or a lost
/// connection doesn't leave a ghost participant behind.
const DELAYED_LEAVE_DELAY: Duration = Duration::from_secs(8);
const DELAYED_LEAVE_HEARTBEAT: Duration = Duration::from_secs(5);
/// Membership events are valid for 4 hours after joining; resend well before.
const MEMBERSHIP_REFRESH: Duration = Duration::from_secs(3600);
/// Safety net in case a member-change sync event is missed.
const RECONCILE_INTERVAL: Duration = Duration::from_secs(30);

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
    /// Our publishing connection plus the subscribe-only connections to the
    /// SFUs other members publish on.
    sfus: Arc<SfuSet>,
    /// Delayed-leave heartbeat and hourly membership refresh.
    membership_task: AbortHandle,
    /// Follows call-member changes: connects/drops SFUs and keeps the E2EE
    /// frame keys in step with the member set.
    reconcile_task: AbortHandle,
    /// The pending MSC4140 delayed leave, cancelled on a graceful hang-up.
    delay_id: Arc<PLMutex<Option<String>>>,
    e2ee: Arc<PLMutex<E2eeManager>>,
    /// Removes the encryption-key to-device handler when the session is dropped.
    _enc_key_guard: matrix_sdk::event_handler::EventHandlerDropGuard,
    /// Removes the call-member state handler when the session is dropped.
    _member_guard: matrix_sdk::event_handler::EventHandlerDropGuard,
}

impl RtcSession {
    pub fn mute_audio(&self, muted: bool) {
        self.sfus.primary.set_audio_muted(muted);
    }

    pub fn mute_video(&self, muted: bool) {
        self.sfus.primary.set_video_muted(muted);
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
        self.sfus
            .primary
            .push_video_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
    }

    /// Publish the screen share track synchronously.
    /// Spawns a dedicated thread (same pattern as rtc_start_call) so the
    /// LiveKit SDP round-trip doesn't overflow the caller's stack.
    pub fn start_screen_share(&self, handle: tokio::runtime::Handle) -> anyhow::Result<()> {
        let lk = Arc::clone(&self.sfus.primary);
        std::thread::Builder::new()
            .stack_size(16 * 1024 * 1024)
            .spawn(move || handle.block_on(lk.start_screen_share()))
            .map_err(|e| anyhow::anyhow!("failed to spawn screen share thread: {e}"))?
            .join()
            .unwrap_or_else(|_| Err(anyhow::anyhow!("screen share thread panicked")))
    }

    /// Stop the screen share track.
    pub fn stop_screen_share(&self) {
        self.sfus.primary.stop_screen_share();
    }

    /// Inject a raw I420 screen frame.
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
        self.sfus
            .primary
            .push_screen_frame_i420(y, u, v, width, height, stride_y, stride_u, stride_v);
    }
}

impl Drop for RtcSession {
    fn drop(&mut self) {
        self.membership_task.abort();
        self.reconcile_task.abort();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Start a call in `room_id` under `slot_id` (typically `"call#default"`).
///
/// Performs: discovery of our homeserver's SFU → JWT → membership join event
/// (`multi_sfu`) → LiveKit connection on our SFU → E2EE key broadcast, then
/// spawns the background tasks that keep the call in step with the room's
/// other members (subscribing to their SFUs, key distribution, heartbeat).
pub async fn start_call(
    client: &matrix_sdk::Client,
    http: &reqwest::Client,
    room_id: &str,
    slot_id: &str,
    audio_only: bool,
) -> anyhow::Result<RtcSession> {
    use matrix_sdk::ruma::{
        events::{call::member::CallMemberEventContent, SyncStateEvent},
        MilliSecondsSinceUnixEpoch,
    };

    let call_intent = if audio_only { "audio" } else { "video" };
    // "call#default" and "" are Tesseract/C++ labels for the default room call.
    // Element uses "m.call#ROOM" as the canonical slot_id sent to the JWT service.
    // Normalize to match, so both parties get a JWT for the same LiveKit room.
    let slot_id = match slot_id {
        "" | "call#default" => "m.call#ROOM",
        other => other,
    };

    let t_start = Instant::now();
    let session_id = next_session_id();
    let member_id = new_member_id();
    let join_ts = MilliSecondsSinceUnixEpoch::now();

    let room_oid: matrix_sdk::ruma::OwnedRoomId = room_id.parse().context("invalid room_id")?;
    let room = client
        .get_room(&room_oid)
        .ok_or_else(|| anyhow::anyhow!("room not found: {room_id}"))?;

    let hs_url = client.homeserver().to_string();
    let access_token = client
        .access_token()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?;
    let uid = client
        .user_id()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?;
    let own_device = client
        .device_id()
        .ok_or_else(|| anyhow::anyhow!("device id not available"))?
        .to_owned();
    let device_id = own_device.to_string();
    let user_id = uid.to_string();
    let server_name = uid.server_name().as_str();

    // Our own homeserver's SFU: the one we publish on, whatever the other
    // members use (multi-SFU).
    let local_service_url =
        fetch_livekit_service_url(http, &hs_url, &access_token, server_name).await?;
    let lk_alias = livekit_room_alias(room_id, slot_id);
    info!("rtc: publishing on local SFU {local_service_url}");

    let initial_members = read_live_members(&room, uid, &own_device).await;
    info!(
        "rtc: {} other call member(s) already joined: {:?}",
        initial_members.len(),
        initial_members
            .iter()
            .map(|m| (&m.user_id, &m.focus_selection))
            .collect::<Vec<_>>()
    );

    // Which SFU each member publishes on; lets the LiveKit layer ignore
    // participants that are not real publishers on the SFU they appear on.
    let member_transports = MemberTransports::default();
    let local_focus = LkFocus {
        service_url: local_service_url.clone(),
        alias: lk_alias.clone(),
    };
    member_transports.replace(transport_map(
        &initial_members,
        Some((join_ts, &local_focus)),
    ));

    // OpenID token → JWT
    let openid = get_openid_token(client).await?;
    let lk_transport = fetch_livekit_jwt(
        http,
        &local_service_url,
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
    info!("rtc: focus discovery done at {:?}", t_start.elapsed());

    // ── E2EE negotiation ────────────────────────────────────────────────────
    // Only enable frame encryption when the Matrix room is encrypted.
    // In unencrypted rooms, peers don't send encryption-key events and
    // have no key to decrypt our frames, so encrypting would break the call.
    let use_e2ee = matches!(
        room.encryption_state(),
        matrix_sdk_base::EncryptionState::Encrypted
    );
    info!("rtc: room encrypted={use_e2ee}");

    // The JWT service assigns the LiveKit participant identity in the `sub`
    // claim. Decode it without verifying the signature so we can send Matrix
    // signaling BEFORE connecting to LiveKit: the receiving client (e.g. Element
    // Android) then already has our state event when we appear in the SFU room
    // and can associate our tracks with our Matrix identity.
    let preflight_identity = super::transport::decode_jwt_sub(&lk_transport.jwt);

    // ── E2EE key handler ─────────────────────────────────────────────────
    // Register BEFORE signaling so we keep any key events the peer sends
    // immediately on receiving our membership. Keys land in `shared` and are
    // applied to participants as they appear on any SFU.
    let shared = SharedKeys::new();
    let sfus_holder: Arc<PLMutex<Option<Arc<SfuSet>>>> = Arc::new(PLMutex::new(None));
    let shared_h = shared.clone();
    let holder_h = Arc::clone(&sfus_holder);
    let room_h = room.clone();
    let enc_key_handle = client.add_event_handler({
        move |ev: matrix_sdk::ruma::events::ToDeviceEvent<
            crate::client::rtc::signaling::RtcEncryptionKeyEventContent,
        >,
              info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>| {
            let shared = shared_h.clone();
            let holder = Arc::clone(&holder_h);
            let room = room_h.clone();
            async move {
                let sender = ev.sender.to_string();
                if let Err(why) = accept_key_event(&room, &ev, info.as_ref()).await {
                    warn!("e2ee: ignoring key event from {sender}: {why}");
                    return;
                }
                let index = ev.content.keys.index as i32;
                let key_b64_len = ev.content.keys.key.len();
                match Base64::decode_vec(&ev.content.keys.key) {
                    Ok(raw_key) if raw_key.is_empty() => {
                        warn!("e2ee: peer key from {sender} index={index} decoded to empty bytes — dropping");
                    }
                    Ok(raw_key) => {
                        let set = holder.lock().clone();
                        match set {
                            Some(set) => set.apply_key(&sender, index, raw_key),
                            None => shared.store(&sender, index, raw_key),
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
    if let Some(ref pid) = preflight_identity {
        info!("rtc: pre-flight signaling with identity={pid}");
        send_msc3401_call_open(&room, "").await?;
        send_msc3401_member_join(
            &room,
            "",
            &device_id,
            pid,
            &local_service_url,
            &lk_alias,
            &user_id,
            audio_only,
            None,
        )
        .await?;
    }

    // MSC4075: ring the room only if nobody else is in the call yet.
    if initial_members.is_empty() {
        if let Err(e) =
            signaling::send_rtc_notification(&room, call_intent, &device_id, &user_id).await
        {
            warn!("rtc: send_rtc_notification failed (non-fatal): {e}");
        }
    }

    // ── Connect to our SFU ────────────────────────────────────────────────
    let sink = super::global_sink();
    let lk = Arc::new(
        LiveKitRoom::connect(
            &lk_transport.server_url,
            &lk_transport.jwt,
            session_id,
            sink.clone(),
            use_e2ee,
            shared.clone(),
            &local_service_url,
            member_transports.clone(),
        )
        .await?,
    );
    info!("rtc: livekit connect done at {:?}", t_start.elapsed());
    let lk_identity = lk.local_identity().to_owned();
    info!("rtc: lk connected, identity={lk_identity} (preflight={preflight_identity:?})");

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
            &local_service_url,
            &lk_alias,
            &user_id,
            audio_only,
            None,
        )
        .await?;
    }

    let sfus = Arc::new(SfuSet::new(
        SfuContext {
            client: client.clone(),
            http: http.clone(),
            room_id: room_id.to_owned(),
            slot_id: slot_id.to_owned(),
            member_id: member_id.clone(),
            device_id: device_id.clone(),
            user_id: user_id.clone(),
            session_id,
            sink,
            use_e2ee,
            members: member_transports.clone(),
        },
        lk,
        shared,
    ));
    // Keys that arrived while connecting only sit in the shared store.
    *sfus_holder.lock() = Some(Arc::clone(&sfus));
    sfus.apply_all_keys();

    // ── Our frame key ─────────────────────────────────────────────────────
    let e2ee = Arc::new(PLMutex::new(E2eeManager::new()));
    if use_e2ee {
        let (idx, key_b64, raw) = {
            let mgr = e2ee.lock();
            (mgr.own_index(), mgr.own_key_b64(), *mgr.own_raw_key())
        };
        // Set our own key first so the FrameCryptor can start encrypting
        // before we broadcast (avoids sending the key before frames are ready).
        sfus.primary.set_own_frame_key(&raw, idx as i32);
        let recipients: Vec<(String, String)> =
            initial_members.iter().map(|m| m.key()).collect();
        broadcast_key_to_members(client, &recipients, room_id, &lk_identity, &key_b64, idx).await;
    }

    // ── Background tasks ──────────────────────────────────────────────────
    let delay_id = Arc::new(PLMutex::new(None));
    let membership_task = spawn_membership_task(
        room.clone(),
        lk_identity.clone(),
        local_service_url.clone(),
        lk_alias.clone(),
        device_id.clone(),
        user_id.clone(),
        audio_only,
        Arc::clone(&delay_id),
    );

    let (member_tx, member_rx) = tokio::sync::mpsc::channel::<()>(4);
    let member_tx_h = member_tx.clone();
    let member_handle = room.add_event_handler(move |_ev: SyncStateEvent<CallMemberEventContent>| {
        let tx = member_tx_h.clone();
        async move {
            let _ = tx.try_send(());
        }
    });
    let member_guard = client.event_handler_drop_guard(member_handle);
    let reconcile_task = tokio::spawn(reconcile_loop(
        room.clone(),
        client.clone(),
        uid.to_owned(),
        own_device,
        local_focus,
        member_transports,
        join_ts,
        Arc::clone(&sfus),
        Arc::clone(&e2ee),
        use_e2ee,
        room_id.to_owned(),
        lk_identity.clone(),
        initial_members.iter().map(|m| m.key()).collect(),
        member_rx,
    ))
    .abort_handle();

    info!(
        "rtc: session {session_id} started in {room_id}/{slot_id} after {:?}",
        t_start.elapsed()
    );
    Ok(RtcSession {
        id: session_id,
        room_id: room_id.to_owned(),
        slot_id: slot_id.to_owned(),
        member_id: lk_identity,
        device_id,
        client: client.clone(),
        sfus,
        membership_task,
        reconcile_task,
        delay_id,
        e2ee,
        _enc_key_guard: enc_key_guard,
        _member_guard: member_guard,
    })
}

/// Gracefully leave the call: send disconnect events, tear down LiveKit, abort tasks.
pub async fn end_call(session: &RtcSession) {
    let t0 = Instant::now();
    session.membership_task.abort();
    session.reconcile_task.abort();
    let pending_leave = session.delay_id.lock().take();
    if let Some(id) = pending_leave {
        signaling::cancel_delayed_event(&session.client, &id).await;
    }
    let room_oid: matrix_sdk::ruma::OwnedRoomId =
        session.room_id.parse().unwrap_or_else(|_| unreachable!());
    if let Some(room) = session.client.get_room(&room_oid) {
        let user_id = session
            .client
            .user_id()
            .map(|u| u.to_string())
            .unwrap_or_default();
        // MSC3401 leave — state key must match the join (uses session member_id, not Matrix device_id)
        let _ = send_msc3401_member_leave(&room, &user_id, &session.member_id).await;
    }
    info!("rtc: leave event sent after {:?}", t0.elapsed());
    session.sfus.disconnect_all().await;
    info!("rtc: livekit disconnected after {:?}", t0.elapsed());
}

/// The event-content checks for a frame-key to-device event (MSC4143).
/// `sender_device` is `None` when the event arrived unencrypted, otherwise the
/// authenticated sending device, if known.
fn check_key_event_fields(
    expected_room: &str,
    content: &crate::client::rtc::signaling::RtcEncryptionKeyEventContent,
    sender_device: Option<Option<&str>>,
) -> Result<(), String> {
    let Some(device) = sender_device else {
        return Err("sent unencrypted".to_owned());
    };
    if content.room_id != expected_room {
        return Err(format!("for another room ({})", content.room_id));
    }
    if content.session.application != "m.call" {
        return Err(format!("for application {:?}", content.session.application));
    }
    if let Some(device) = device {
        if device != content.member.claimed_device_id {
            return Err(format!(
                "claims device {} but was sent from {device}",
                content.member.claimed_device_id
            ));
        }
    }
    Ok(())
}

/// Whether a frame-key to-device event may be used for this call: the content
/// checks above, plus the sender must be joined to the room. Anything else
/// could inject keys for other calls or from outsiders.
async fn accept_key_event(
    room: &Room,
    ev: &matrix_sdk::ruma::events::ToDeviceEvent<
        crate::client::rtc::signaling::RtcEncryptionKeyEventContent,
    >,
    info: Option<&matrix_sdk::deserialized_responses::EncryptionInfo>,
) -> Result<(), String> {
    use matrix_sdk::ruma::events::room::member::MembershipState;

    check_key_event_fields(
        room.room_id().as_str(),
        &ev.content,
        info.map(|i| i.sender_device.as_deref().map(|d| d.as_str())),
    )?;
    match room.get_member(&ev.sender).await {
        Ok(Some(m)) if *m.membership() == MembershipState::Join => Ok(()),
        Ok(_) => Err("sender is not joined to the room".to_owned()),
        Err(e) => Err(format!("could not look up the sender's membership: {e}")),
    }
}

// ---------------------------------------------------------------------------
// Background tasks
// ---------------------------------------------------------------------------

/// Keeps our membership alive: heartbeats the MSC4140 delayed leave every few
/// seconds and resends the membership event hourly (with a fresh `expires` and
/// the original `created_ts`, so peers see a stable join age).
fn spawn_membership_task(
    room: Room,
    member_id: String,
    service_url: String,
    lk_alias: String,
    device_id: String,
    user_id: String,
    audio_only: bool,
    delay_id: Arc<PLMutex<Option<String>>>,
) -> AbortHandle {
    tokio::spawn(async move {
        let client = room.client();
        let mut delayed_ok = true;
        let first = signaling::schedule_delayed_msc3401_leave(
            &room,
            &user_id,
            &member_id,
            DELAYED_LEAVE_DELAY,
        )
        .await;
        delayed_ok &= first.is_some();
        *delay_id.lock() = first;

        let mut created_ts = None;
        let mut last_refresh = Instant::now();
        let mut interval = tokio::time::interval(DELAYED_LEAVE_HEARTBEAT);
        interval.tick().await; // skip the immediate 0-delay first tick
        loop {
            interval.tick().await;
            let _job = crate::client::activity::begin(
                "rtc-membership-refresh",
                "Calls",
                crate::client::activity::JobKind::Periodic,
            );

            if delayed_ok {
                let current = delay_id.lock().clone();
                match current {
                    Some(id) => match signaling::restart_delayed_event(&client, &id).await {
                        DelayedRestart::Ok => {}
                        DelayedRestart::Gone => {
                            let n = signaling::schedule_delayed_msc3401_leave(
                                &room,
                                &user_id,
                                &member_id,
                                DELAYED_LEAVE_DELAY,
                            )
                            .await;
                            delayed_ok = n.is_some();
                            *delay_id.lock() = n;
                        }
                        DelayedRestart::Unsupported => {
                            delayed_ok = false;
                            *delay_id.lock() = None;
                        }
                    },
                    None => delayed_ok = false,
                }
            }

            if last_refresh.elapsed() >= MEMBERSHIP_REFRESH {
                if created_ts.is_none() {
                    created_ts =
                        signaling::own_msc3401_created_ts(&room, &user_id, &member_id).await;
                }
                // Without our original join time a resend would reset our age.
                let Some(ts) = created_ts else { continue };
                let sent = send_msc3401_member_join(
                    &room,
                    "",
                    &device_id,
                    &member_id,
                    &service_url,
                    &lk_alias,
                    &user_id,
                    audio_only,
                    Some(ts),
                )
                .await;
                if let Err(e) = sent {
                    warn!("rtc: membership refresh failed: {e}");
                    continue;
                }
                last_refresh = Instant::now();
                // A new state event for the same key cancels the delayed leave.
                if delayed_ok {
                    let n = signaling::schedule_delayed_msc3401_leave(
                        &room,
                        &user_id,
                        &member_id,
                        DELAYED_LEAVE_DELAY,
                    )
                    .await;
                    delayed_ok = n.is_some();
                    *delay_id.lock() = n;
                }
            }
        }
    })
    .abort_handle()
}

/// Follows the room's call members: connects to the SFUs they publish on,
/// drops SFUs nobody uses any more, and keeps the E2EE key distribution in
/// step (send our key to new members, rotate when someone leaves).
#[allow(clippy::too_many_arguments)]
async fn reconcile_loop(
    room: Room,
    client: matrix_sdk::Client,
    own_user: matrix_sdk::ruma::OwnedUserId,
    own_device: matrix_sdk::ruma::OwnedDeviceId,
    local_focus: LkFocus,
    member_transports: MemberTransports,
    join_ts: matrix_sdk::ruma::MilliSecondsSinceUnixEpoch,
    sfus: Arc<SfuSet>,
    e2ee: Arc<PLMutex<E2eeManager>>,
    use_e2ee: bool,
    room_id: String,
    lk_identity: String,
    mut previous: HashSet<(String, String)>,
    mut changes: tokio::sync::mpsc::Receiver<()>,
) {
    let mut interval = tokio::time::interval(RECONCILE_INTERVAL);
    interval.tick().await;
    let mut first = true;
    loop {
        if !first {
            tokio::select! {
                _ = changes.recv() => {}
                _ = interval.tick() => {}
            }
            // Coalesce a burst of member events into one pass.
            while changes.try_recv().is_ok() {}
        }
        first = false;

        let _job = crate::client::activity::begin(
            "rtc-member-reconcile",
            "Calls",
            crate::client::activity::JobKind::Periodic,
        );
        let members = read_live_members(&room, &own_user, &own_device).await;
        let current: HashSet<(String, String)> = members.iter().map(|m| m.key()).collect();
        let joined: Vec<(String, String)> = current.difference(&previous).cloned().collect();
        let someone_left = previous.difference(&current).next().is_some();
        if !joined.is_empty() || someone_left {
            info!(
                "rtc: call members changed: +{} -{} (now {})",
                joined.len(),
                previous.difference(&current).count(),
                current.len()
            );
        }

        if use_e2ee && someone_left {
            // Forward secrecy: leavers must not decrypt future media.
            let (idx, key_b64, raw) = {
                let mut mgr = e2ee.lock();
                let (idx, key_b64) = mgr.rotate();
                (idx, key_b64, *mgr.own_raw_key())
            };
            sfus.primary.set_own_frame_key(&raw, idx as i32);
            let everyone: Vec<(String, String)> = current.iter().cloned().collect();
            broadcast_key_to_members(&client, &everyone, &room_id, &lk_identity, &key_b64, idx)
                .await;
        } else if use_e2ee && !joined.is_empty() {
            let (idx, key_b64) = {
                let mgr = e2ee.lock();
                (mgr.own_index(), mgr.own_key_b64())
            };
            broadcast_key_to_members(&client, &joined, &room_id, &lk_identity, &key_b64, idx)
                .await;
        }

        member_transports.replace(transport_map(&members, Some((join_ts, &local_focus))));
        let desired = desired_sfus(
            &members,
            Some((join_ts, &local_focus)),
            &local_focus.service_url,
        );
        sfus.reconcile(&desired).await;
        previous = current;
    }
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
                let own_device = client
                    .device_id()
                    .map(|d| d.to_string())
                    .unwrap_or_default();
                let sender = ev.sender().to_string();
                let room_id = room.room_id().to_string();

                // Ignore only events from our own device (state key = member_id we sent).
                let (content, event_state_key) = match &ev {
                    SyncStateEvent::Original(o) => (&o.content, &o.state_key),
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

                // Best-effort staleness filter: RtcMemberEventContent (MSC4143) has no
                // expires/created_ts TTL to check (unlike MSC3401's CallMemberEventContent
                // in register_msc3401_invitation_handler above), so we can't detect a
                // membership that's expired without ever being cleanly left. What we CAN
                // detect is a historical timeline replay of an already-ended call: if the
                // room's current state for this state_key has since moved on to a leave,
                // this delivered event is stale and must not raise a fresh banner. This
                // does not cover the crash / never-left case, where current state still
                // shows a join too.
                if let Ok(Some(raw_current)) = room
                    .get_state_event_static_for_key::<RtcMemberEventContent, _>(event_state_key)
                    .await
                {
                    use matrix_sdk::deserialized_responses::SyncOrStrippedState;
                    if let Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(cur))) =
                        raw_current.deserialize()
                    {
                        let cur_is_join = !cur.content.application.kind.is_empty()
                            || cur.content.focus_active.is_some()
                            || !cur.content.slot_id.is_empty();
                        if !cur_is_join {
                            return;
                        }
                    }
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
        call::member::{Application, CallMemberEventContent},
        SyncStateEvent,
    };

    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: SyncStateEvent<CallMemberEventContent>, room: Room| {
            let client = client_clone.clone();
            async move {
                let own_user = client.user_id().map(|u| u.to_string()).unwrap_or_default();
                let own_device = client
                    .device_id()
                    .map(|d| d.to_string())
                    .unwrap_or_default();
                let sender = ev.sender().to_string();
                let room_id = room.room_id().to_string();

                // Ignore only events from our own device (not from ourselves on Element/another device).
                let own_state_key = format!("_{}_{}_m.call", own_user, own_device);
                let (content, event_state_key, origin_server_ts) = match &ev {
                    SyncStateEvent::Original(o) => {
                        (&o.content, o.state_key.as_ref(), o.origin_server_ts)
                    }
                    SyncStateEvent::Redacted(_) => return,
                };

                if event_state_key == own_state_key {
                    return;
                }

                // Empty variant = leave; a join whose TTL (created_ts/origin_server_ts +
                // expires, default 4h) has passed is equally not a live invitation. This
                // also covers stale timeline replay redelivered via
                // ClientFfi::sync_room_subscriptions when a room is (re-)opened.
                if content.active_memberships(Some(origin_server_ts)).is_empty() {
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
        rtc::notification::{CallIntent, NotificationType, RtcNotificationEventContent},
        SyncMessageLikeEvent,
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

pub(super) struct OpenIdToken {
    pub access_token: String,
    pub expires_in: u64,
    pub matrix_server_name: String,
}

pub(super) async fn get_openid_token(client: &matrix_sdk::Client) -> anyhow::Result<OpenIdToken> {
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

#[cfg(test)]
mod invitation_staleness_tests {
    use std::time::Duration;

    use matrix_sdk::ruma::{
        events::call::member::{
            ActiveFocus, ActiveLivekitFocus, Application, CallApplicationContent,
            CallMemberEventContent, CallScope, Focus, LivekitFocus,
        },
        owned_device_id, MilliSecondsSinceUnixEpoch,
    };

    fn call_member_content(created_ts: MilliSecondsSinceUnixEpoch) -> CallMemberEventContent {
        CallMemberEventContent::new(
            Application::Call(CallApplicationContent::new("123456".to_owned(), CallScope::Room)),
            owned_device_id!("ABCDE"),
            ActiveFocus::Livekit(ActiveLivekitFocus::new()),
            vec![Focus::Livekit(LivekitFocus::new(
                "1".to_owned(),
                "https://livekit.example.org".to_owned(),
            ))],
            Some(created_ts),
            Some(Duration::from_secs(60)), // 1 minute TTL
        )
    }

    // Mirrors the exact check register_msc3401_invitation_handler now performs
    // (and that has_active_call in client/mod.rs already relied on): a join
    // membership whose created_ts + expires has passed must not be treated as
    // a live invitation, even though it's not the `Empty` (explicit-leave) variant.
    #[test]
    fn expired_membership_is_not_active() {
        let ten_minutes_ago = MilliSecondsSinceUnixEpoch::now()
            .to_system_time()
            .and_then(|t| t.checked_sub(Duration::from_secs(600)))
            .map(MilliSecondsSinceUnixEpoch::from_system_time)
            .flatten()
            .expect("valid past timestamp");

        let content = call_member_content(ten_minutes_ago);
        assert!(content.active_memberships(None).is_empty());
    }

    #[test]
    fn fresh_membership_is_active() {
        let content = call_member_content(MilliSecondsSinceUnixEpoch::now());
        assert!(!content.active_memberships(None).is_empty());
    }
}

#[cfg(test)]
mod key_event_tests {
    use super::*;
    use crate::client::rtc::signaling::RtcEncryptionKeyEventContent;

    fn content(room: &str, application: &str, device: &str) -> RtcEncryptionKeyEventContent {
        serde_json::from_value(serde_json::json!({
            "keys": { "key": "AAAA", "index": 0 },
            "room_id": room,
            "member": { "claimed_device_id": device, "id": "@a:x.org:DEV" },
            "session": { "call_id": "", "application": application, "scope": "m.room" },
        }))
        .unwrap()
    }

    #[test]
    fn accepts_matching_encrypted_event() {
        let c = content("!r:x.org", "m.call", "DEV");
        assert!(check_key_event_fields("!r:x.org", &c, Some(Some("DEV"))).is_ok());
        // sending device unknown to the crypto layer: nothing to compare against
        assert!(check_key_event_fields("!r:x.org", &c, Some(None)).is_ok());
    }

    #[test]
    fn rejects_unencrypted() {
        let c = content("!r:x.org", "m.call", "DEV");
        assert!(check_key_event_fields("!r:x.org", &c, None).is_err());
    }

    #[test]
    fn rejects_other_room_or_application() {
        assert!(check_key_event_fields("!r:x.org", &content("!other:x.org", "m.call", "DEV"), Some(Some("DEV"))).is_err());
        assert!(check_key_event_fields("!r:x.org", &content("!r:x.org", "m.whiteboard", "DEV"), Some(Some("DEV"))).is_err());
    }

    #[test]
    fn rejects_device_mismatch() {
        let c = content("!r:x.org", "m.call", "CLAIMED");
        assert!(check_key_event_fields("!r:x.org", &c, Some(Some("ACTUAL"))).is_err());
    }
}
