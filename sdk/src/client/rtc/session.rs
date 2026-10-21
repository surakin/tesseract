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
    e2ee::{broadcast_key_to_members, ParkedKey, ParkedKeys},
    key_rotation::{KeyRotator, RotationAction},
    livekit_room::{LiveKitRoom, MemberTransports, SharedKeys},
    members::{desired_sfus, transport_map, LkFocus},
    mrtc_membership::{
        can_send_member_event, can_send_slot, leave_reason_now, new_member_id as new_mrtc_member_id,
        open_slot as open_mrtc_slot, send_invite, should_open_slot, should_write_member_events,
        slot_matches_room, withdraw_invite, JoinParams, LeaveCode, MrtcMembership, REFRESH_EVERY,
    },
    slots::{hashed_identities, read_all_live_members, read_slot},
    sfu_set::{SfuContext, SfuSet},
    signaling::{
        self, send_msc3401_call_open, send_msc3401_member_join, send_msc3401_member_leave,
        DelayedRestart,
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
    e2ee: Arc<PLMutex<KeyRotator>>,
    /// Our sticky `m.rtc.member` membership, when this call also writes one.
    mrtc: Option<Arc<MrtcMembership>>,
    /// Resends the member event before its sticky duration runs out.
    mrtc_refresh: Option<AbortHandle>,
    /// We sent a MatrixRTC invite that still needs withdrawing when we leave an empty call.
    sent_invite: bool,
    /// Removes the encryption-key to-device handler when the session is dropped.
    _enc_key_guards: (
        matrix_sdk::event_handler::EventHandlerDropGuard,
        matrix_sdk::event_handler::EventHandlerDropGuard,
    ),
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

    let uid = client
        .user_id()
        .ok_or_else(|| anyhow::anyhow!("not logged in"))?;
    let own_device = client
        .device_id()
        .ok_or_else(|| anyhow::anyhow!("device id not available"))?
        .to_owned();
    let device_id = own_device.to_string();
    let user_id = uid.to_string();

    // Our own homeserver's SFU: the one we publish on, whatever the other
    // members use (multi-SFU).
    let local_service_url = fetch_livekit_service_url(client, http).await?;
    let lk_alias = livekit_room_alias(room_id, slot_id);
    info!("rtc: publishing on local SFU {local_service_url}");

    let initial_members = read_all_live_members(&room, uid, &own_device).await;
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
        sfu_url: None,
        server_name: None,
    };
    member_transports.replace(transport_map(
        &initial_members,
        Some((join_ts, &local_focus)),
    ));
    member_transports.identities().replace(hashed_identities(&initial_members));

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
    // Unknown is not "unencrypted": the state has not loaded yet (a freshly
    // joined room, or a store that lost the event). Sending media in the clear on
    // that guess would fail open, so the call is refused until the state is known.
    let use_e2ee = match room.encryption_state() {
        matrix_sdk_base::EncryptionState::Encrypted => true,
        matrix_sdk_base::EncryptionState::NotEncrypted => false,
        matrix_sdk_base::EncryptionState::Unknown => {
            anyhow::bail!("this room's encryption state is not known yet; try the call again")
        }
    };
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
    let shared = SharedKeys::with_identities(member_transports.identities());
    let sfus_holder: Arc<PLMutex<Option<Arc<SfuSet>>>> = Arc::new(PLMutex::new(None));
    // Keys from senders whose membership event we have not seen yet wait here
    // (MSC4143) until `reconcile_loop` sees them join.
    let parked: Arc<PLMutex<ParkedKeys>> = Arc::new(PLMutex::new(ParkedKeys::new()));
    let key_ctx = KeyCtx {
        shared: shared.clone(),
        holder: Arc::clone(&sfus_holder),
        room: room.clone(),
        parked: Arc::clone(&parked),
        own_user: uid.to_owned(),
        own_device: own_device.clone(),
    };
    use crate::client::rtc::signaling::{MediaKeyStableEventContent, MediaKeyUnstableEventContent};
    let ctx_a = key_ctx.clone();
    let unstable_handle = client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::ToDeviceEvent<MediaKeyUnstableEventContent>,
              info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>| {
            handle_key_event(ctx_a.clone(), ev.sender, ev.content.inner, info)
        },
    );
    let ctx_b = key_ctx;
    let stable_handle = client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::ToDeviceEvent<MediaKeyStableEventContent>,
              info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>| {
            handle_key_event(ctx_b.clone(), ev.sender, ev.content.inner, info)
        },
    );
    let enc_key_guards = (
        client.event_handler_drop_guard(unstable_handle),
        client.event_handler_drop_guard(stable_handle),
    );

    // ── Pre-flight Matrix signaling ───────────────────────────────────────
    if let Some(ref pid) = preflight_identity {
        info!("rtc: pre-flight signaling with identity={pid}");
        if signaling::should_open_call_slot(
            signaling::call_slot_exists(&room, "").await,
            !initial_members.is_empty(),
        ) {
            signaling::start_msc3401_call_slot(&room, "").await?;
        }
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

    // MSC4143: open the room's m.call slot for clients that read it. Best
    // effort: the legacy events above are what this call runs on.
    let slot_usable = {
        let existing = read_slot(&room).await;
        let mut usable = existing.as_ref().is_some_and(|s| slot_matches_room(s, use_e2ee));
        if initial_members.is_empty()
            && should_open_slot(existing.as_ref(), can_send_slot(&room).await)
        {
            match open_mrtc_slot(&room).await {
                Ok(()) => usable = true,
                Err(e) => warn!("rtc: could not open the m.rtc.slot (non-fatal): {e}"),
            }
        }
        usable
    };

    // MatrixRTC membership next to the legacy one, like Element Call's
    // compatibility mode: a sticky m.rtc.member while the slot is usable.
    let mrtc: Option<Arc<MrtcMembership>> = if should_write_member_events(
        client.supports_sticky_events().await.unwrap_or(false),
        slot_usable,
        can_send_member_event(&room).await,
    ) {
        let membership = Arc::new(MrtcMembership::new(
            room.clone(),
            JoinParams {
                member_id: new_mrtc_member_id(),
                user_id: user_id.clone(),
                device_id: device_id.clone(),
                sfu_url: lk_transport.server_url.clone(),
                service_url: local_service_url.clone(),
                audio_only,
            },
        ));
        match membership.join().await {
            Ok(()) => Some(membership),
            Err(e) => {
                warn!("rtc: MatrixRTC join failed (non-fatal): {e}");
                None
            }
        }
    } else {
        None
    };

    // MSC4075: ring the room only if nobody else is in the call yet, and
    // never in a dedicated call room (joining there isn't calling anyone).
    if signaling::should_ring_room(
        !initial_members.is_empty(),
        signaling::is_call_room(&room).await,
    ) {
        if let Err(e) =
            signaling::send_rtc_notification(&room, call_intent, &device_id, &user_id).await
        {
            warn!("rtc: send_rtc_notification failed (non-fatal): {e}");
        }
    }
    // The MatrixRTC invite (sticky) rings a DM for clients that read it.
    let mut sent_invite = false;
    if mrtc.is_some()
        && signaling::should_ring_room(
            !initial_members.is_empty(),
            signaling::is_call_room(&room).await,
        )
        && room.is_direct().await.unwrap_or(false)
    {
        match send_invite(&room, super::sticky::now_ms()).await {
            Ok(_) => sent_invite = true,
            Err(e) => warn!("rtc: MatrixRTC invite failed (non-fatal): {e}"),
        }
    }

    // ── Connect to our SFU ────────────────────────────────────────────────
    let sink = super::sink_for_client(&client);
    let lk = match LiveKitRoom::connect(
        &lk_transport.server_url,
        &lk_transport.jwt,
        session_id,
        sink.clone(),
        use_e2ee,
        shared.clone(),
        &local_service_url,
        member_transports.clone(),
    )
    .await
    {
        Ok(lk) => Arc::new(lk),
        Err(e) => {
            abandon_announcements(
                &room,
                mrtc.as_deref(),
                sent_invite,
                LeaveCode::TransportError,
                "could not connect to the SFU",
            )
            .await;
            return Err(e);
        }
    };
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
        let resent: anyhow::Result<()> = async {
            if signaling::should_open_call_slot(
                signaling::call_slot_exists(&room, "").await,
                !initial_members.is_empty(),
            ) {
                signaling::start_msc3401_call_slot(&room, "").await?;
            }
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
            .await
        }
        .await;
        if let Err(e) = resent {
            abandon_announcements(
                &room,
                mrtc.as_deref(),
                sent_invite,
                LeaveCode::TransportError,
                "could not update the call membership",
            )
            .await;
            return Err(e);
        }
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
    let e2ee = Arc::new(PLMutex::new(KeyRotator::new(
        initial_members.iter().map(|m| m.key()).collect(),
        Instant::now(),
    )));
    if use_e2ee {
        let (idx, key_b64, raw) = e2ee.lock().initial();
        // Set our own key first so the FrameCryptor can start encrypting
        // before we broadcast (avoids sending the key before frames are ready).
        sfus.primary.set_own_frame_key(&raw, idx as i32);
        let recipients: Vec<(String, String)> =
            initial_members.iter().map(|m| m.key()).collect();
        broadcast_key_to_members(
            client,
            &recipients,
            room_id,
            &lk_identity,
            &key_b64,
            idx,
            mrtc.as_ref().map(|m| m.member_id()),
        )
        .await;
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
    let member_handle = room.add_event_handler(move |_ev: SyncStateEvent<signaling::CallMemberRawEventContent>| {
        let tx = member_tx_h.clone();
        async move {
            let _ = tx.try_send(());
        }
    });
    let member_guard = client.event_handler_drop_guard(member_handle);
    // MatrixRTC members arrive as sticky events: wake the reconcile loop on those too.
    {
        let mut changes = room.sticky_events().subscribe();
        let tx = member_tx.clone();
        tokio::spawn(async move {
            loop {
                tokio::select! {
                    changed = changes.recv() => {
                        match changed {
                            Ok(_) | Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                                let _ = tx.try_send(());
                            }
                            Err(_) => break,
                        }
                    }
                    _ = tx.closed() => break,
                }
            }
        });
    }
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
        parked,
        mrtc.clone(),
    ))
    .abort_handle();

    let mrtc_refresh = mrtc.as_ref().map(|m| {
        let m = Arc::clone(m);
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(REFRESH_EVERY);
            tick.tick().await;
            loop {
                tick.tick().await;
                if m.has_left() {
                    break;
                }
                if let Err(e) = m.join().await {
                    warn!("rtc: MatrixRTC membership refresh failed: {e}");
                }
            }
        })
        .abort_handle()
    });

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
        mrtc,
        mrtc_refresh,
        sent_invite,
        _enc_key_guards: enc_key_guards,
        _member_guard: member_guard,
    })
}

/// Take back what a call announced when its setup fails before it is up:
/// leave the MatrixRTC slot and withdraw the ring, so peers do not see a
/// phantom participant or a call that never connected.
async fn abandon_announcements(
    room: &Room,
    mrtc: Option<&MrtcMembership>,
    sent_invite: bool,
    code: LeaveCode,
    why: &str,
) {
    if let Some(m) = mrtc {
        m.leave(code, Some(why)).await;
    }
    if sent_invite {
        let _ = withdraw_invite(room).await;
    }
}

/// Gracefully leave the call: send disconnect events, tear down LiveKit, abort tasks.
pub async fn end_call(session: &RtcSession) {
    let t0 = Instant::now();
    session.membership_task.abort();
    session.reconcile_task.abort();
    if let Some(refresh) = &session.mrtc_refresh {
        refresh.abort();
    }
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
    if let Some(m) = &session.mrtc {
        m.leave(LeaveCode::Leave, None).await;
        if session.sent_invite {
            if let Some(room) = session.client.get_room(&room_oid) {
                // Withdraw our ring when nobody else ever joined. Best effort:
                // whatever happens here, the teardown below still runs.
                if let (Some(u), Some(d)) = (session.client.user_id(), session.client.device_id()) {
                    if read_all_live_members(&room, u, d).await.is_empty() {
                        let _ = withdraw_invite(&room).await;
                    }
                }
            }
        }
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
    // A key is trusted only when the device it came from is known and is the
    // one the event claims: an unknown sending device has nothing to check.
    let device = match sender_device {
        None => return Err("sent unencrypted".to_owned()),
        Some(None) => return Err("sender device is unknown".to_owned()),
        Some(Some(device)) => device,
    };
    if content.room_id != expected_room {
        return Err(format!("for another room ({})", content.room_id));
    }
    if content.session.application != "m.call" {
        return Err(format!("for application {:?}", content.session.application));
    }
    if content.member.claimed_device_id.is_empty() {
        return Err("claims no device".to_owned());
    }
    if device != content.member.claimed_device_id {
        return Err(format!(
            "claims device {} but was sent from {device}",
            content.member.claimed_device_id
        ));
    }
    Ok(())
}

/// MSC4153 on the receiving side: with `exclude_insecure` on (the same switch
/// that limits whom we send keys to), a key must come from a device its owner
/// cross-signed. `UnverifiedIdentity` is fine: the device is signed, we just
/// haven't verified the person.
fn check_device_trusted(
    state: &matrix_sdk::deserialized_responses::VerificationState,
    exclude_insecure: bool,
) -> Result<(), String> {
    use matrix_sdk::deserialized_responses::{VerificationLevel, VerificationState};
    if !exclude_insecure {
        return Ok(());
    }
    // An allow-list: anything not known to be a cross-signed device is out,
    // including a previously verified identity that has changed.
    match state {
        VerificationState::Verified
        | VerificationState::Unverified(VerificationLevel::UnverifiedIdentity) => Ok(()),
        _ => Err("sending device is not cross-signed by its owner".to_owned()),
    }
}

/// Whether `(user, device)` is one of the live call members `(user_id, device_id)`.
fn is_live_member(members: &[(String, String)], user: &str, device: &str) -> bool {
    members.iter().any(|(u, d)| u == user && d == device)
}

/// A frame key that passed the content checks.
#[derive(Debug)]
struct AcceptedKey {
    /// The device the key came from (authenticated by decryption).
    device: String,
    index: u8,
    key_b64: String,
}

/// The event-content checks for either key-event shape. With a `member_id`
/// (MSC4143) a visible `m.rtc.member` of that id must be from the same user
/// and device; an id we have not seen yet passes, and the caller parks the key
/// until the member shows up. Without one (Element's legacy shape) the
/// claimed-device rules of [`check_key_event_fields`] apply.
fn evaluate_key_event(
    expected_room: &str,
    c: &crate::client::rtc::model::MediaKeyContent,
    sender: &str,
    sender_device: Option<Option<&str>>,
    members: &[crate::client::rtc::members::CallMember],
) -> Result<AcceptedKey, String> {
    use crate::client::rtc::members::MemberSource;
    use crate::client::rtc::signaling::RtcEncryptionKeyEventContent;

    let device = match sender_device {
        None => return Err("sent unencrypted".to_owned()),
        Some(None) => return Err("sender device is unknown".to_owned()),
        Some(Some(d)) => d,
    };
    if c.room_id != expected_room {
        return Err(format!("for another room ({})", c.room_id));
    }
    let (index, key_b64) = c.media_key_parts().ok_or_else(|| "carries no key".to_owned())?;
    match &c.member_id {
        Some(id) => {
            // Member ids are visible to everyone, so another user may have
            // published an event with the same id: only the sender's own
            // entry counts, and an id only others hold is a mismatch.
            let owners: Vec<&crate::client::rtc::members::CallMember> = members
                .iter()
                .filter(|m| m.source == MemberSource::MatrixRtc && m.member_id == *id)
                .collect();
            if !owners.is_empty() {
                let mine: Vec<_> = owners.iter().filter(|m| m.user_id == sender).collect();
                if mine.is_empty() {
                    return Err(format!("member {id} belongs to another user"));
                }
                if !mine.iter().any(|m| m.device_id == device) {
                    return Err(format!("member {id} joined from another device"));
                }
            }
        }
        None => {
            let (Some(keys), Some(member), Some(session)) = (&c.keys, &c.member, &c.session) else {
                return Err("legacy key event is incomplete".to_owned());
            };
            let legacy = RtcEncryptionKeyEventContent {
                keys: keys.clone(),
                room_id: c.room_id.clone(),
                member: member.clone(),
                session: session.clone(),
                sent_ts: 0,
            };
            check_key_event_fields(expected_room, &legacy, Some(Some(device)))?;
        }
    }
    Ok(AcceptedKey { device: device.to_owned(), index, key_b64 })
}

/// Whether a frame-key to-device event may be used for this call: the content
/// checks, the device-trust check, and the sender must be joined to the room.
/// Anything else could inject keys for other calls or from outsiders.
async fn accept_key_event(
    room: &Room,
    sender: &matrix_sdk::ruma::UserId,
    content: &crate::client::rtc::model::MediaKeyContent,
    info: Option<&matrix_sdk::deserialized_responses::EncryptionInfo>,
    members: &[crate::client::rtc::members::CallMember],
) -> Result<AcceptedKey, String> {
    use matrix_sdk::ruma::events::room::member::MembershipState;

    let accepted = evaluate_key_event(
        room.room_id().as_str(),
        content,
        sender.as_str(),
        info.map(|i| i.sender_device.as_deref().map(|d| d.as_str())),
        members,
    )?;
    if let Some(i) = info {
        check_device_trusted(
            &i.verification_state,
            crate::client::identity::exclude_insecure_devices(),
        )?;
    }
    match room.get_member(sender).await {
        Ok(Some(m)) if *m.membership() == MembershipState::Join => Ok(accepted),
        Ok(_) => Err("sender is not joined to the room".to_owned()),
        Err(e) => Err(format!("could not look up the sender's membership: {e}")),
    }
}

/// Everything a key-event handler needs; cloned per event.
#[derive(Clone)]
struct KeyCtx {
    shared: SharedKeys,
    holder: Arc<PLMutex<Option<Arc<SfuSet>>>>,
    room: Room,
    parked: Arc<PLMutex<ParkedKeys>>,
    own_user: matrix_sdk::ruma::OwnedUserId,
    own_device: matrix_sdk::ruma::OwnedDeviceId,
}

async fn handle_key_event(
    ctx: KeyCtx,
    sender_id: matrix_sdk::ruma::OwnedUserId,
    content: crate::client::rtc::model::MediaKeyContent,
    info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>,
) {
    let sender = sender_id.to_string();
    let members = read_all_live_members(&ctx.room, &ctx.own_user, &ctx.own_device).await;
    let accepted = match accept_key_event(&ctx.room, &sender_id, &content, info.as_ref(), &members).await {
        Ok(a) => a,
        Err(why) => {
            warn!("e2ee: ignoring key event from {sender}: {why}");
            return;
        }
    };
    let index = accepted.index as i32;
    let key_b64_len = accepted.key_b64.len();
    let raw_key = match Base64::decode_vec(&accepted.key_b64) {
        Ok(k) if k.is_empty() => {
            warn!("e2ee: peer key from {sender} index={index} decoded to empty bytes — dropping");
            return;
        }
        Ok(k) => k,
        Err(e) => {
            warn!("e2ee: bad base64 in peer key from {sender} index={index} key_b64_len={key_b64_len}: {e}");
            return;
        }
    };
    let device = accepted.device.as_str();
    // Same reason as in the reconcile loop: resolve hashed identities first.
    ctx.shared.identities().replace(hashed_identities(&members));
    let keys: Vec<(String, String)> = members.iter().map(|m| m.key()).collect();
    if !is_live_member(&keys, &sender, device) {
        info!("e2ee: parking key from {sender}/{device}: no live membership yet");
        ctx.parked.lock().park(
            ParkedKey { user_id: sender, device_id: device.to_owned(), index, raw_key },
            Instant::now(),
        );
        return;
    }
    let set = ctx.holder.lock().clone();
    match set {
        Some(set) => set.apply_key(&sender, device, index, raw_key),
        None => ctx.shared.store(&sender, device, index, raw_key),
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
    e2ee: Arc<PLMutex<KeyRotator>>,
    use_e2ee: bool,
    room_id: String,
    lk_identity: String,
    mut previous: HashSet<(String, String)>,
    mut changes: tokio::sync::mpsc::Receiver<()>,
    parked: Arc<PLMutex<ParkedKeys>>,
    mrtc: Option<Arc<MrtcMembership>>,
) {
    let mut interval = tokio::time::interval(RECONCILE_INTERVAL);
    interval.tick().await;
    let mut first = true;
    loop {
        if !first {
            let key_deadline = e2ee
                .lock()
                .next_deadline()
                .map(tokio::time::Instant::from_std)
                .unwrap_or_else(|| tokio::time::Instant::now() + RECONCILE_INTERVAL);
            tokio::select! {
                _ = changes.recv() => {}
                _ = interval.tick() => {}
                _ = tokio::time::sleep_until(key_deadline), if use_e2ee => {}
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
        if let Some(m) = mrtc.as_ref().filter(|m| !m.has_left()) {
            let slot = read_slot(&room).await;
            if let Some((code, why)) =
                leave_reason_now(slot.as_ref(), m.encrypted_at_join(), room.encryption_state().is_encrypted())
            {
                m.leave(code, Some(why)).await;
            }
        }
        let members = read_all_live_members(&room, &own_user, &own_device).await;
        let current: HashSet<(String, String)> = members.iter().map(|m| m.key()).collect();
        // Hashed LiveKit identities must resolve before any key is applied, or a
        // key for a member who is already connected matches nobody.
        let hashed = hashed_identities(&members);
        let have_hashed = !hashed.is_empty();
        member_transports.identities().replace(hashed);
        if have_hashed {
            sfus.apply_all_keys();
        }
        let released = parked
            .lock()
            .take_for(&current.iter().cloned().collect::<Vec<_>>(), Instant::now());
        for k in released {
            info!("e2ee: applying parked key from {}/{}", k.user_id, k.device_id);
            sfus.apply_key(&k.user_id, &k.device_id, k.index, k.raw_key);
        }
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

        if use_e2ee {
            let now = Instant::now();
            let actions = {
                let mut rotator = e2ee.lock();
                let mut a = rotator.on_members_changed(&current, now);
                a.extend(rotator.poll(now));
                a
            };
            for action in actions {
                match action {
                    RotationAction::Send { index, key_b64, to } => {
                        broadcast_key_to_members(
                            &client,
                            &to,
                            &room_id,
                            &lk_identity,
                            &key_b64,
                            index,
                            mrtc.as_ref().map(|m| m.member_id()),
                        )
                        .await;
                    }
                    RotationAction::Activate { index, raw_key } => {
                        sfus.primary.set_own_frame_key(&raw_key, index as i32);
                    }
                }
            }
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
// Invitation watchers (registered once at sync start)
// ---------------------------------------------------------------------------

/// Register a handler for MSC3401 `org.matrix.msc3401.call.member` events.
/// Element Web uses this format.
pub fn register_msc3401_invitation_handler(client: &matrix_sdk::Client) -> matrix_sdk::event_handler::EventHandlerHandle {
    use matrix_sdk::ruma::events::SyncStateEvent;

    let client_clone = client.clone();
    client.add_event_handler(
        move |ev: SyncStateEvent<signaling::CallMemberRawEventContent>, room: Room| {
            let client = client_clone.clone();
            async move {
                let (Some(own_user), Some(own_device)) =
                    (client.user_id().map(|u| u.to_owned()), client.device_id().map(|d| d.to_owned()))
                else {
                    return;
                };
                let room_id = room.room_id().to_string();
                let SyncStateEvent::Original(o) = &ev else { return };

                // A leave (empty content), a membership whose lifetime has passed
                // and our own device are not live invitations. The expiry also
                // covers stale timeline replay redelivered when a room is
                // (re-)opened. Read from the raw JSON: ruma's typed model drops
                // the events of newer clients.
                let content = serde_json::Value::Object(o.content.fields.clone());
                let live = super::members::members_from_event_json(
                    &content,
                    o.state_key.as_ref(),
                    &o.sender,
                    o.origin_server_ts,
                    Some((&own_user, &own_device)),
                    super::sticky::now_ms(),
                );
                if live.is_empty() {
                    return;
                }
                let slot_id = super::members::call_id_of(&content);
                if let Some(sink) = super::sink_for_client(&client) {
                    sink.on_invitation(&room_id, &slot_id, o.sender.as_str(), "", 0, "");
                }
            }
        },
    )
}

/// Register a handler for stable `m.rtc.notification` and unstable
/// `org.matrix.msc4075.rtc.notification` events (MSC4075).
/// Fires `on_invitation` when a ring notification arrives from another user.
pub fn register_rtc_notification_handler(
    client: &matrix_sdk::Client,
) -> Vec<matrix_sdk::event_handler::EventHandlerHandle> {
    use matrix_sdk::ruma::events::{
        rtc::notification::{CallIntent, NotificationType, RtcNotificationEventContent},
        SyncMessageLikeEvent,
    };

    // Stable prefix: m.rtc.notification
    let client_clone = client.clone();
    let stable = client.add_event_handler(
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
                if signaling::is_call_room(&room).await {
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

                if let Some(sink) = super::sink_for_client(&client) {
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
    let unstable = client.add_event_handler(
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
                if signaling::is_call_room(&room).await {
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

                if let Some(sink) = super::sink_for_client(&client) {
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
    vec![stable, unstable]
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
            Application::Call(CallApplicationContent::new(
                "123456".to_owned(),
                CallScope::Room,
            )),
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
    }

    #[test]
    fn rejects_unknown_sender_device() {
        let c = content("!r:x.org", "m.call", "DEV");
        assert_eq!(
            check_key_event_fields("!r:x.org", &c, Some(None)).unwrap_err(),
            "sender device is unknown"
        );
    }

    #[test]
    fn rejects_empty_claimed_device() {
        let c = content("!r:x.org", "m.call", "");
        assert!(check_key_event_fields("!r:x.org", &c, Some(Some("DEV"))).is_err());
    }

    #[test]
    fn rejects_unencrypted() {
        let c = content("!r:x.org", "m.call", "DEV");
        assert!(check_key_event_fields("!r:x.org", &c, None).is_err());
    }

    #[test]
    fn rejects_other_room_or_application() {
        assert!(check_key_event_fields(
            "!r:x.org",
            &content("!other:x.org", "m.call", "DEV"),
            Some(Some("DEV"))
        )
        .is_err());
        assert!(check_key_event_fields(
            "!r:x.org",
            &content("!r:x.org", "m.whiteboard", "DEV"),
            Some(Some("DEV"))
        )
        .is_err());
    }

    #[test]
    fn rejects_device_mismatch() {
        let c = content("!r:x.org", "m.call", "CLAIMED");
        assert!(check_key_event_fields("!r:x.org", &c, Some(Some("ACTUAL"))).is_err());
    }

    #[test]
    fn error_messages_name_the_cause() {
        let c = content("!other:x.org", "m.call", "DEV");
        assert_eq!(
            check_key_event_fields("!r:x.org", &c, None).unwrap_err(),
            "sent unencrypted"
        );
        assert!(check_key_event_fields("!r:x.org", &c, Some(Some("DEV")))
            .unwrap_err()
            .contains("!other:x.org"));
        let c = content("!r:x.org", "m.whiteboard", "DEV");
        assert!(check_key_event_fields("!r:x.org", &c, Some(Some("DEV")))
            .unwrap_err()
            .contains("m.whiteboard"));
        let c = content("!r:x.org", "m.call", "CLAIMED");
        let e = check_key_event_fields("!r:x.org", &c, Some(Some("ACTUAL"))).unwrap_err();
        assert!(e.contains("CLAIMED") && e.contains("ACTUAL"), "{e}");
    }

    fn media(v: serde_json::Value) -> crate::client::rtc::model::MediaKeyContent {
        serde_json::from_value(v).unwrap()
    }

    fn mrtc_member(user: &str, device: &str, member_id: &str) -> crate::client::rtc::members::CallMember {
        use crate::client::rtc::members::{CallMember, MemberSource};
        CallMember {
            user_id: user.into(), device_id: device.into(), state_key: member_id.into(),
            joined_at: matrix_sdk::ruma::MilliSecondsSinceUnixEpoch::now(),
            focus_selection: "multi_sfu".into(), foci_preferred: vec![], member_id: member_id.into(),
            source: MemberSource::MatrixRtc, intent: None, capabilities: None,
        }
    }

    fn stable(room: &str, member_id: &str) -> crate::client::rtc::model::MediaKeyContent {
        media(serde_json::json!({
            "room_id": room, "member_id": member_id,
            "media_key": { "index": 4, "key": "AAAA" }
        }))
    }

    #[test]
    fn stable_shape_from_the_member_s_own_device_is_accepted() {
        let members = vec![mrtc_member("@a:x.org", "DEV", "mid1")];
        let ok = evaluate_key_event("!r:x.org", &stable("!r:x.org", "mid1"), "@a:x.org", Some(Some("DEV")), &members).unwrap();
        assert_eq!((ok.device.as_str(), ok.index, ok.key_b64.as_str()), ("DEV", 4, "AAAA"));
    }

    #[test]
    fn a_squatted_member_id_does_not_block_the_real_owner() {
        // Mallory published a member event with the victim's (visible) member id.
        let members = vec![mrtc_member("@mallory:x.org", "M", "mid1"), mrtc_member("@a:x.org", "DEV", "mid1")];
        let ok = evaluate_key_event("!r:x.org", &stable("!r:x.org", "mid1"), "@a:x.org", Some(Some("DEV")), &members).unwrap();
        assert_eq!(ok.device, "DEV");
        // ...and Mallory's own key under that id still only works from Mallory's device.
        let e = evaluate_key_event("!r:x.org", &stable("!r:x.org", "mid1"), "@mallory:x.org", Some(Some("OTHER")), &members).unwrap_err();
        assert!(e.contains("another device"), "{e}");
    }

    #[test]
    fn stable_shape_for_another_users_member_id_is_rejected() {
        let members = vec![mrtc_member("@a:x.org", "DEV", "mid1")];
        let e = evaluate_key_event("!r:x.org", &stable("!r:x.org", "mid1"), "@mallory:x.org", Some(Some("DEV")), &members).unwrap_err();
        assert!(e.contains("another user"), "{e}");
    }

    #[test]
    fn stable_shape_from_a_different_device_than_the_member_event_is_rejected() {
        let members = vec![mrtc_member("@a:x.org", "DEV", "mid1")];
        let e = evaluate_key_event("!r:x.org", &stable("!r:x.org", "mid1"), "@a:x.org", Some(Some("OTHER")), &members).unwrap_err();
        assert!(e.contains("another device"), "{e}");
    }

    #[test]
    fn stable_shape_with_an_unseen_member_id_is_left_to_parking() {
        let ok = evaluate_key_event("!r:x.org", &stable("!r:x.org", "unseen"), "@a:x.org", Some(Some("DEV")), &[]).unwrap();
        assert_eq!(ok.device, "DEV");
    }

    #[test]
    fn stable_shape_still_needs_encryption_and_the_right_room() {
        let c = stable("!r:x.org", "m");
        assert_eq!(evaluate_key_event("!r:x.org", &c, "@a:x.org", None, &[]).unwrap_err(), "sent unencrypted");
        assert!(evaluate_key_event("!r:x.org", &c, "@a:x.org", Some(None), &[]).is_err());
        assert!(evaluate_key_event("!other:x.org", &c, "@a:x.org", Some(Some("DEV")), &[]).is_err());
    }

    #[test]
    fn legacy_shape_keeps_the_claimed_device_rules() {
        let c = media(serde_json::json!({
            "room_id": "!r:x.org",
            "keys": { "key": "BBBB", "index": 7 },
            "member": { "claimed_device_id": "DEV", "id": "@a:x.org:DEV" },
            "session": { "call_id": "", "application": "m.call", "scope": "m.room" }
        }));
        let ok = evaluate_key_event("!r:x.org", &c, "@a:x.org", Some(Some("DEV")), &[]).unwrap();
        assert_eq!((ok.index, ok.key_b64.as_str()), (7, "BBBB"));
        assert!(evaluate_key_event("!r:x.org", &c, "@a:x.org", Some(Some("ACTUAL")), &[]).is_err());
    }

    #[test]
    fn event_without_any_key_is_rejected() {
        let c = media(serde_json::json!({ "room_id": "!r:x.org", "member_id": "m" }));
        assert!(evaluate_key_event("!r:x.org", &c, "@a:x.org", Some(Some("DEV")), &[]).is_err());
    }

    #[test]
    fn strict_mode_rejects_devices_their_owner_did_not_sign() {
        use matrix_sdk::deserialized_responses::{VerificationLevel, VerificationState};
        let unsigned = VerificationState::Unverified(VerificationLevel::UnsignedDevice);
        assert_eq!(
            check_device_trusted(&unsigned, true).unwrap_err(),
            "sending device is not cross-signed by its owner"
        );
        let mismatched = VerificationState::Unverified(VerificationLevel::MismatchedSender);
        assert!(check_device_trusted(&mismatched, true).is_err());
    }

    #[test]
    fn strict_mode_rejects_a_changed_identity_that_was_verified() {
        use matrix_sdk::deserialized_responses::{VerificationLevel, VerificationState};
        let violation = VerificationState::Unverified(VerificationLevel::VerificationViolation);
        assert!(check_device_trusted(&violation, true).is_err());
        assert!(check_device_trusted(&violation, false).is_ok());
    }

    #[test]
    fn strict_mode_accepts_cross_signed_devices_of_unverified_users() {
        use matrix_sdk::deserialized_responses::{VerificationLevel, VerificationState};
        assert!(check_device_trusted(&VerificationState::Verified, true).is_ok());
        let unverified_user = VerificationState::Unverified(VerificationLevel::UnverifiedIdentity);
        assert!(check_device_trusted(&unverified_user, true).is_ok());
    }

    #[test]
    fn lax_mode_accepts_any_device() {
        use matrix_sdk::deserialized_responses::{VerificationLevel, VerificationState};
        let unsigned = VerificationState::Unverified(VerificationLevel::UnsignedDevice);
        assert!(check_device_trusted(&unsigned, false).is_ok());
    }

    #[test]
    fn is_live_member_matches_user_and_device() {
        let members = vec![("@a:x.org".to_owned(), "DEV".to_owned())];
        assert!(is_live_member(&members, "@a:x.org", "DEV"));
        assert!(!is_live_member(&members, "@a:x.org", "OTHER"));
        assert!(!is_live_member(&members, "@b:x.org", "DEV"));
    }

    #[test]
    fn unencrypted_is_rejected_before_other_checks() {
        // Wrong room and wrong application, but the unencrypted verdict wins.
        let c = content("!other:x.org", "m.whiteboard", "DEV");
        assert_eq!(
            check_key_event_fields("!r:x.org", &c, None).unwrap_err(),
            "sent unencrypted"
        );
    }
}

#[cfg(test)]
mod id_tests {
    use super::*;

    #[test]
    fn member_id_is_a_v4_uuid() {
        for _ in 0..50 {
            let id = new_member_id();
            let parts: Vec<&str> = id.split('-').collect();
            assert_eq!(
                parts.iter().map(|p| p.len()).collect::<Vec<_>>(),
                vec![8, 4, 4, 4, 12],
                "{id}"
            );
            assert!(
                id.chars().all(|c| c == '-' || c.is_ascii_hexdigit()),
                "{id}"
            );
            assert!(id.chars().all(|c| !c.is_ascii_uppercase()), "{id}");
            assert!(parts[2].starts_with('4'), "version nibble: {id}");
            assert!(
                matches!(parts[3].chars().next(), Some('8' | '9' | 'a' | 'b')),
                "variant nibble: {id}"
            );
        }
    }

    #[test]
    fn member_ids_are_unique() {
        let ids: std::collections::HashSet<String> = (0..200).map(|_| new_member_id()).collect();
        assert_eq!(ids.len(), 200);
    }

    #[test]
    fn session_ids_strictly_increase() {
        let a = next_session_id();
        let b = next_session_id();
        let c = next_session_id();
        assert!(a < b && b < c);
    }

    #[test]
    fn heartbeat_beats_the_delayed_leave_and_refresh_beats_expiry() {
        // The dead-man's switch must be re-armed before it fires...
        assert!(DELAYED_LEAVE_HEARTBEAT < DELAYED_LEAVE_DELAY);
        // ...and the membership must be resent well inside its 4 h validity.
        assert!(MEMBERSHIP_REFRESH < Duration::from_secs(4 * 3600));
    }
}
