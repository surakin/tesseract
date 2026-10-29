use std::{
    collections::HashSet,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
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
    e2ee::{recheck_device_id, ClientKeySender, KeyDelivery, ParkedKey, ParkedKeys, RateLimiter, Recheck},
    key_rotation::{ActivationGate, KeyRotator, RotationAction, Sessions},
    livekit_room::{LiveKitRoom, MemberTransports, PrimaryEnded, SharedKeys},
    members::{desired_sfus, transport_map, JoinCheck, LkFocus},
    mrtc_membership::{
        can_send_member_event, can_send_slot, encryption_flipped_mid_call, leave_reason_now,
        new_member_id as new_mrtc_member_id, open_slot as open_mrtc_slot, plan_slot, send_invite,
        should_write_member_events, withdraw_invite, JoinParams, LeaveCode, MrtcMembership, SlotPlan,
    },
    slots::{
        hashed_identities, read_all_live_members, read_all_live_members_checked, read_slot, read_slot_state_keyed,
        stale_own_members,
    },
    identity::{classify_identity, IdentityShape},
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
/// The member list of a room that was never fully loaded is fetched when a call
/// starts; the call does not wait longer than this for it.
const INITIAL_MEMBERS_TIMEOUT: Duration = Duration::from_secs(8);
/// While a new key waits to be put to use, sends that end are looked at at most
/// this often.
const GATE_RECHECK_COALESCE: Duration = Duration::from_millis(250);

fn next_session_id() -> u64 {
    SESSION_ID_COUNTER.fetch_add(1, Ordering::Relaxed)
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
    e2ee: Arc<PLMutex<KeyRotator>>,
    /// The memberships and ring this call announced, the background tasks that
    /// keep them alive, and the one way to take them back.
    announced: Arc<Announced>,
    /// The runtime the call runs on, for the leave that a drop sends.
    runtime: tokio::runtime::Handle,
    /// Removes the encryption-key to-device handler when the session is dropped.
    _enc_key_guards: (
        matrix_sdk::event_handler::EventHandlerDropGuard,
        matrix_sdk::event_handler::EventHandlerDropGuard,
    ),
    /// Removes the call-member state handler when the session is dropped.
    _member_guard: matrix_sdk::event_handler::EventHandlerDropGuard,
    /// Removes the room-membership handler (a kick, ban or leave wakes the
    /// reconcile loop) when the session is dropped.
    _room_member_guard: matrix_sdk::event_handler::EventHandlerDropGuard,
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
    /// Stops every background task and, when `end_call` did not already,
    /// withdraws the memberships on the runtime in a detached task (a session
    /// replaced or dropped without a hang-up must not keep announcing us). The
    /// withdrawal is a no-op for whatever a delivered `end_call` took back and
    /// finishes what a timed-out one left.
    fn drop(&mut self) {
        self.announced.shutdown();
        // Spawning on a runtime that has shut down drops the task; it never panics.
        let announced = Arc::clone(&self.announced);
        self.runtime.spawn(async move {
            announced.withdraw(LeaveCode::Leave, None).await;
        });
    }
}

/// How the call's signalling reaches the room. The room in production; a
/// recorder in tests.
#[async_trait::async_trait]
trait Signals: Send + Sync {
    /// Send the legacy `call.member` leave for the LiveKit identity `member_id`.
    async fn legacy_leave(&self, member_id: &str) -> anyhow::Result<()>;
    /// Cancel a pending MSC4140 delayed event (best effort).
    async fn cancel_delayed(&self, delay_id: &str);
    /// Whether anyone else has joined the call.
    async fn call_answered(&self) -> bool;
    /// Send the withdrawal of our MatrixRTC invite.
    async fn withdraw_invite(&self) -> anyhow::Result<()>;
}

struct RoomSignals {
    room: Room,
    user_id: String,
}

#[async_trait::async_trait]
impl Signals for RoomSignals {
    async fn legacy_leave(&self, member_id: &str) -> anyhow::Result<()> {
        send_msc3401_member_leave(&self.room, &self.user_id, member_id).await
    }

    async fn cancel_delayed(&self, delay_id: &str) {
        signaling::cancel_delayed_event(&self.room.client(), delay_id).await;
    }

    async fn call_answered(&self) -> bool {
        let client = self.room.client();
        match (client.user_id(), client.device_id()) {
            (Some(u), Some(d)) => !read_all_live_members(&self.room, u, d).await.is_empty(),
            // Unknown: leave the ring alone rather than cancel one someone answered.
            _ => true,
        }
    }

    async fn withdraw_invite(&self) -> anyhow::Result<()> {
        withdraw_invite(&self.room).await
    }
}

/// The memberships and ring a call announced, the background tasks that keep
/// them alive, and how to take it all back. Each part is withdrawn once it is
/// delivered, however many of `end_call`, a dropped session, an SFU disconnect,
/// the reconcile loop and a failed setup ask: a request that was cancelled or
/// failed is tried again by the next one.
struct Announced {
    signals: Arc<dyn Signals>,
    /// LiveKit identities we sent a legacy `call.member` join for (the state key follows it).
    legacy_ids: PLMutex<Vec<String>>,
    /// The pending MSC4140 delayed leave of the legacy member.
    delay_id: Arc<PLMutex<Option<String>>>,
    /// Our sticky `m.rtc.member` membership, when this call also writes one.
    /// Kept even when its join failed: the send may still have gone through.
    mrtc: std::sync::OnceLock<Arc<MrtcMembership>>,
    /// Whether the legacy leave has been delivered; held while it is sent and
    /// while the heartbeat sends a join, so the two cannot interleave.
    legacy_left: tokio::sync::Mutex<bool>,
    /// We sent a MatrixRTC invite; whether it is settled (withdrawn, or the
    /// call was answered) is the value, held while it is sent.
    invite_sent: AtomicBool,
    invite_settled: tokio::sync::Mutex<bool>,
    /// The call is being torn down: nothing may announce us any more.
    closed: AtomicBool,
    /// The heartbeat and the sticky refresh: the tasks that send joins.
    tasks: PLMutex<Vec<AbortHandle>>,
    /// The reconcile loop, kept apart because it tears the call down itself
    /// when the room turns encrypted and must not abort itself.
    reconcile: PLMutex<Option<AbortHandle>>,
}

impl Announced {
    fn new(signals: Arc<dyn Signals>) -> Self {
        Self {
            signals,
            legacy_ids: PLMutex::new(Vec::new()),
            delay_id: Arc::new(PLMutex::new(None)),
            mrtc: std::sync::OnceLock::new(),
            legacy_left: tokio::sync::Mutex::new(false),
            invite_sent: AtomicBool::new(false),
            invite_settled: tokio::sync::Mutex::new(false),
            closed: AtomicBool::new(false),
            tasks: PLMutex::new(Vec::new()),
            reconcile: PLMutex::new(None),
        }
    }

    /// Record a legacy join we are about to send, so a withdrawal covers it
    /// even if the send times out after the homeserver processed it.
    fn joined_legacy(&self, id: &str) {
        let mut ids = self.legacy_ids.lock();
        if !ids.iter().any(|i| i == id) {
            ids.push(id.to_owned());
        }
    }

    /// Record the sticky membership, so a withdrawal covers it.
    fn set_membership(&self, membership: Arc<MrtcMembership>) {
        let _ = self.mrtc.set(membership);
    }

    /// Record that we sent a MatrixRTC invite, so a withdrawal covers it.
    fn invited(&self) {
        self.invite_sent.store(true, Ordering::SeqCst);
    }

    fn is_closed(&self) -> bool {
        self.closed.load(Ordering::SeqCst)
    }

    /// Hand a task that announces us over to be stopped by [`Self::close`].
    /// A task registered after the call closed is stopped at once.
    fn register_task(&self, task: AbortHandle) {
        let mut tasks = self.tasks.lock();
        if self.is_closed() {
            task.abort();
        } else {
            tasks.push(task);
        }
    }

    fn register_reconcile(&self, task: AbortHandle) {
        let mut slot = self.reconcile.lock();
        if self.is_closed() {
            task.abort();
        } else {
            *slot = Some(task);
        }
    }

    /// Stop everything that sends a join and refuse further ones. The reconcile
    /// loop is left running: it may be the caller (see [`Self::shutdown`]).
    fn close(&self) {
        let mut tasks = self.tasks.lock();
        self.closed.store(true, Ordering::SeqCst);
        for task in tasks.drain(..) {
            task.abort();
        }
    }

    /// [`Self::close`] plus the reconcile loop: the first step of every
    /// teardown that is not driven by that loop.
    fn shutdown(&self) {
        self.close();
        if let Some(task) = self.reconcile.lock().take() {
            task.abort();
        }
    }

    /// The legacy-membership lock for sending a join, or `None` once the call
    /// is closed or the legacy leave is delivered. Held across the send, so a
    /// join cannot land after the leave.
    async fn open_for_join(&self) -> Option<tokio::sync::MutexGuard<'_, bool>> {
        let left = self.legacy_left.lock().await;
        (!self.is_closed() && !*left).then_some(left)
    }

    /// Send the leave for a legacy identity we no longer use (a wrong guess at
    /// our LiveKit identity) and stop tracking it once it is out.
    async fn leave_legacy_identity(&self, id: &str) {
        if send_legacy_leave(&*self.signals, id).await {
            self.legacy_ids.lock().retain(|i| i != id);
        }
    }

    /// Take back the legacy and the sticky membership and the ring. The delayed
    /// leave is the fallback while the legacy leave has not got out, so it is
    /// only cancelled once that has.
    async fn withdraw(&self, code: LeaveCode, why: Option<&str>) {
        {
            let mut left = self.legacy_left.lock().await;
            if !*left {
                let ids = self.legacy_ids.lock().clone();
                let mut all_sent = true;
                for id in &ids {
                    all_sent &= send_legacy_leave(&*self.signals, id).await;
                }
                if all_sent {
                    *left = true;
                    let pending = self.delay_id.lock().take();
                    if let Some(id) = pending {
                        self.signals.cancel_delayed(&id).await;
                    }
                }
            }
        }
        if let Some(m) = self.mrtc.get() {
            m.leave(code, why).await;
        }
        if self.invite_sent.load(Ordering::SeqCst) {
            let mut settled = self.invite_settled.lock().await;
            if !*settled {
                // A call somebody joined needs no cancelled ring.
                *settled = self.signals.call_answered().await
                    || self.signals.withdraw_invite().await.is_ok();
            }
        }
    }
}

/// Send the legacy member leave, once more if the first send fails.
async fn send_legacy_leave(signals: &dyn Signals, member_id: &str) -> bool {
    for attempt in 0..2 {
        match signals.legacy_leave(member_id).await {
            Ok(()) => return true,
            Err(e) => warn!("rtc: legacy call-member leave failed (attempt {}): {e}", attempt + 1),
        }
        if attempt == 0 {
            tokio::time::sleep(Duration::from_millis(500)).await;
        }
    }
    false
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
        events::{call::member::CallMemberEventContent, room::member::SyncRoomMemberEvent, SyncStateEvent},
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
    // One id for this join: the /get_token request, the sticky `m.rtc.member`
    // and the key events all name it, so that the LiveKit identity the JWT
    // service derives from it (MSC4195: a hash of our user and this id) is the
    // one our peers compute from our member event.
    let member_id = new_mrtc_member_id();
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

    // A big room that was never fully loaded would fetch its member list here:
    // the call does not wait for it past the timeout, and goes on with what the
    // local store knows (the reconcile loop corrects it with the full list).
    let initial_members = match tokio::time::timeout(
        INITIAL_MEMBERS_TIMEOUT,
        read_all_live_members(&room, uid, &own_device),
    )
    .await
    {
        Ok(members) => members,
        Err(_) => {
            warn!("rtc: the room's member list is slow to load; starting with the members already known");
            // These members are the first media key's recipients, so a sender the
            // store does not know is left out until the reconcile loop's full read
            // confirms it.
            read_all_live_members_checked(&room, uid, &own_device, JoinCheck::KnownJoined).await
        }
    };
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
    let mut local_focus = LkFocus {
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
    // Now that the SFU's WebSocket URL is known, a member that names only that
    // URL as its transport can be recognised as being on our SFU.
    local_focus.sfu_url = Some(lk_transport.server_url.clone());
    member_transports.replace(transport_map(&initial_members, Some((join_ts, &local_focus))));

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
    // Wakes the reconcile loop: a member change, or a parked key to judge again.
    let (member_tx, member_rx) = tokio::sync::mpsc::channel::<()>(4);
    let key_ctx = KeyCtx::new(
        shared.clone(),
        Arc::clone(&sfus_holder),
        room.clone(),
        Arc::clone(&parked),
        uid.to_owned(),
        own_device.clone(),
        member_tx.clone(),
    );
    use crate::client::rtc::signaling::{MediaKeyStableEventContent, MediaKeyUnstableEventContent};
    let ctx_a = key_ctx.clone();
    let unstable_handle = client.add_event_handler(
        move |ev: matrix_sdk::ruma::events::ToDeviceEvent<MediaKeyUnstableEventContent>,
              info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>| {
            handle_key_event(ctx_a.clone(), ev.sender, ev.content.inner, info)
        },
    );
    let ctx_b = key_ctx.clone();
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
    // From the first join on, a failed setup takes back everything announced so far.
    let announced = Arc::new(Announced::new(Arc::new(RoomSignals {
        room: room.clone(),
        user_id: user_id.clone(),
    })));
    if let Some(ref pid) = preflight_identity {
        info!("rtc: pre-flight signaling with identity={pid}");
        if signaling::should_open_call_slot(
            signaling::call_slot_exists(&room, "").await,
            !initial_members.is_empty(),
        ) {
            signaling::start_msc3401_call_slot(&room, "").await?;
        }
        // Recorded before the send: a join that timed out may still have landed.
        announced.joined_legacy(pid);
        if let Err(e) = send_msc3401_member_join(
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
        .await
        {
            announced
                .withdraw(LeaveCode::TransportError, Some("could not announce the call"))
                .await;
            return Err(e);
        }
    }

    // MSC4143: open the room's m.call slot for clients that read it, or fix one
    // that does not fit the room (see `plan_slot`). Best effort: the legacy
    // events above are what this call runs on.
    let slot_usable = {
        let (state, slot_key) = read_slot_state_keyed(&room).await;
        match plan_slot(&state, use_e2ee, can_send_slot(&room).await, !initial_members.is_empty()) {
            SlotPlan::Use => true,
            SlotPlan::Write => match open_mrtc_slot(&room, slot_key).await {
                Ok(()) => {
                    info!("rtc: wrote the m.rtc.slot ({state:?} before)");
                    true
                }
                Err(e) => {
                    warn!("rtc: could not write the m.rtc.slot (non-fatal): {e}");
                    false
                }
            },
            SlotPlan::Skip => {
                info!("rtc: leaving the room's m.rtc.slot alone ({state:?}); this call is legacy-only");
                false
            }
        }
    };

    // MatrixRTC membership next to the legacy one, like Element Call's
    // compatibility mode: a sticky m.rtc.member while the slot is usable.
    // `mrtc` is the membership that joined; `announced` also keeps one whose
    // join failed, since the send may still have gone through.
    let mrtc: Option<Arc<MrtcMembership>> = if should_write_member_events(
        client.supports_sticky_events().await.unwrap_or(false),
        slot_usable,
        can_send_member_event(&room).await,
    ) {
        let membership = Arc::new(MrtcMembership::new(
            room.clone(),
            JoinParams {
                member_id: member_id.clone(),
                user_id: user_id.clone(),
                device_id: device_id.clone(),
                sfu_url: lk_transport.server_url.clone(),
                service_url: local_service_url.clone(),
                audio_only,
            },
        ));
        announced.set_membership(Arc::clone(&membership));
        match membership.join().await {
            Ok(()) => {
                // A session of ours that ended without leaving (a restart) is
                // still joined for minutes under another member id: withdraw it
                // now that we are in, never before, so a failed join leaves it alone.
                let stale = stale_own_members(
                    &super::sticky::live_entries(&room),
                    &user_id,
                    &device_id,
                    membership.member_id(),
                    super::sticky::now_ms(),
                );
                membership.withdraw_stale(&stale).await;
                Some(membership)
            }
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
    if mrtc.is_some()
        && signaling::should_ring_room(
            !initial_members.is_empty(),
            signaling::is_call_room(&room).await,
        )
        && room.is_direct().await.unwrap_or(false)
    {
        // Recorded before the send: an invite that timed out may still have landed.
        announced.invited();
        if let Err(e) = send_invite(&room, super::sticky::now_ms()).await {
            warn!("rtc: MatrixRTC invite failed (non-fatal): {e}");
        }
    }

    // ── Connect to our SFU ────────────────────────────────────────────────
    let sink = super::sink_for_client(&client);
    // A lost connection to our SFU ends the call for good (the UI is told by
    // the connection itself): take back what it announced and stop the tasks
    // that would keep announcing it. A hang-up closes the call first, so this
    // does nothing then.
    let on_primary_ended: PrimaryEnded = {
        let announced = Arc::clone(&announced);
        let holder = Arc::clone(&sfus_holder);
        Arc::new(move || {
            if announced.is_closed() {
                return;
            }
            warn!("rtc: lost the connection to our SFU; taking back the call's announcements");
            announced.shutdown();
            let announced = Arc::clone(&announced);
            let holder = Arc::clone(&holder);
            tokio::spawn(async move {
                let set = holder.lock().clone();
                if let Some(set) = set {
                    set.disconnect_all().await;
                }
                announced
                    .withdraw(LeaveCode::TransportError, Some("lost the connection to the SFU"))
                    .await;
            });
        })
    };
    let lk = match LiveKitRoom::connect(
        &lk_transport.server_url,
        &lk_transport.jwt,
        session_id,
        sink.clone(),
        use_e2ee,
        shared.clone(),
        &local_service_url,
        member_transports.clone(),
        on_primary_ended,
    )
    .await
    {
        Ok(lk) => Arc::new(lk),
        Err(e) => {
            announced
                .withdraw(LeaveCode::TransportError, Some("could not connect to the SFU"))
                .await;
            return Err(e);
        }
    };
    info!("rtc: livekit connect done at {:?}", t_start.elapsed());
    let lk_identity = lk.local_identity().to_owned();
    info!("rtc: lk connected, identity={lk_identity} (preflight={preflight_identity:?})");
    // Peers find our tracks by the identity they derive from our member event.
    match classify_identity(&lk_identity, &user_id, &device_id, &member_id) {
        IdentityShape::Hashed => info!("rtc: LiveKit identity is the MSC4195 hash of our member id"),
        IdentityShape::Legacy => {
            info!("rtc: LiveKit identity is the legacy user:device form (older JWT service)");
        }
        IdentityShape::Unexpected => warn!(
            "rtc: LiveKit identity {lk_identity} is neither the hash of our member id ({member_id}) nor \
             user:device; MatrixRTC peers may not match our tracks to our member event"
        ),
    }

    // ── Correct signaling if the actual identity differs from preflight ───
    // Normally they match. A mismatch would indicate a JWT service change or
    // decode error; re-send the correct events so Element can track us.
    if preflight_identity.as_deref() != Some(lk_identity.as_str()) {
        warn!(
            "rtc: identity mismatch — preflight={preflight_identity:?} actual={lk_identity}; \
             re-sending signaling"
        );
        // The re-sent join may land even when the send reports a failure.
        announced.joined_legacy(&lk_identity);
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
            announced
                .withdraw(LeaveCode::TransportError, Some("could not update the call membership"))
                .await;
            return Err(e);
        }
        // The membership under the wrong identity would otherwise stay as a ghost.
        if let Some(wrong) = preflight_identity.as_deref() {
            announced.leave_legacy_identity(wrong).await;
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
        initial_members.iter().map(|m| (m.key(), m.session_tag())).collect(),
        Instant::now(),
    )));
    // Keys go out from a worker of their own, never from the call's setup or
    // from the reconcile loop.
    let delivery = KeyDelivery::new();
    if use_e2ee {
        announced.register_task(delivery.spawn(Arc::new(ClientKeySender::new(
            client.clone(),
            room_id.to_owned(),
            lk_identity.clone(),
            mrtc.as_ref().map(|m| m.member_id().to_owned()),
        ))));
        let (idx, key_b64, raw) = e2ee.lock().initial();
        // Set our own key first so the FrameCryptor can start encrypting
        // before we broadcast (avoids sending the key before frames are ready).
        sfus.primary.set_own_frame_key(&raw, idx as i32);
        let recipients: Vec<(String, String)> =
            initial_members.iter().map(|m| m.key()).collect();
        delivery.enqueue(&recipients, idx, &key_b64);
    }

    // ── Background tasks ──────────────────────────────────────────────────
    announced.register_task(spawn_membership_task(
        room.clone(),
        lk_identity.clone(),
        local_service_url.clone(),
        lk_alias.clone(),
        device_id.clone(),
        user_id.clone(),
        audio_only,
        Arc::clone(&announced),
    ));

    let member_tx_h = member_tx.clone();
    let member_handle = room.add_event_handler(move |_ev: SyncStateEvent<signaling::CallMemberRawEventContent>| {
        let tx = member_tx_h.clone();
        async move {
            let _ = tx.try_send(());
        }
    });
    let member_guard = client.event_handler_drop_guard(member_handle);
    // A kick, ban or leave is an m.room.member change, not a call.member one: wake
    // the loop on those too, so the user stops receiving keys and the leave rotation
    // happens at once instead of at the next 30 s pass. A profile change does not.
    let member_tx_r = member_tx.clone();
    let room_member_handle = room.add_event_handler(move |ev: SyncRoomMemberEvent| {
        let tx = member_tx_r.clone();
        async move {
            let prev = match &ev {
                SyncRoomMemberEvent::Original(o) => o.prev_content().map(|c| c.membership.clone()),
                SyncRoomMemberEvent::Redacted(_) => None,
            };
            if membership_event_matters(ev.membership(), prev.as_ref()) {
                let _ = tx.try_send(());
            }
        }
    });
    let room_member_guard = client.event_handler_drop_guard(room_member_handle);
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
        delivery,
        use_e2ee,
        initial_members.iter().map(|m| m.key()).collect(),
        member_rx,
        key_ctx,
        mrtc.clone(),
        Arc::clone(&announced),
        session_id,
    ))
    .abort_handle();
    announced.register_reconcile(reconcile_task);

    if let Some(m) = &mrtc {
        let m = Arc::clone(m);
        announced.register_task(tokio::spawn(async move { m.run_refresh().await }).abort_handle());
    }

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
        e2ee,
        announced,
        runtime: tokio::runtime::Handle::current(),
        _enc_key_guards: enc_key_guards,
        _member_guard: member_guard,
        _room_member_guard: room_member_guard,
    })
}

/// Whether an `m.room.member` event can change who is a call member: anything
/// but a join that stays a join (a profile change).
fn membership_event_matters(
    now: &matrix_sdk::ruma::events::room::member::MembershipState,
    before: Option<&matrix_sdk::ruma::events::room::member::MembershipState>,
) -> bool {
    use matrix_sdk::ruma::events::room::member::MembershipState::Join;
    !(*now == Join && before == Some(&Join))
}

/// How long `end_call` waits for the memberships to be taken back before it
/// stops blocking the caller. The session's drop finishes what is left.
const END_CALL_WITHDRAW_TIMEOUT: Duration = Duration::from_secs(5);

/// Gracefully leave the call: send disconnect events, tear down LiveKit, abort tasks.
/// The caller drops the session right after, which finishes any withdrawal
/// that did not get out within [`END_CALL_WITHDRAW_TIMEOUT`] (offline).
pub async fn end_call(session: &RtcSession) {
    let t0 = Instant::now();
    // Every task that could announce us again stops before the leaves go out.
    // The leave below is delivered even when this aborts the reconcile task in
    // the middle of its own: whichever did not get out is sent here.
    session.announced.shutdown();
    // Legacy leave (state key must match the join: the LiveKit identity, not
    // the Matrix device id), its delayed fallback, the sticky leave and the ring.
    let withdrawn = tokio::time::timeout(
        END_CALL_WITHDRAW_TIMEOUT,
        session.announced.withdraw(LeaveCode::Leave, None),
    )
    .await;
    if withdrawn.is_err() {
        warn!("rtc: taking back the call's announcements is taking long; finishing in the background");
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

/// Why a frame-key event was not accepted.
#[derive(Debug, PartialEq, Eq)]
enum KeyRejection {
    /// It never will be.
    Final(String),
    /// The sending device is not known yet (or not far enough along to be
    /// judged): worth judging again once it is.
    Retry(String),
}

/// Whether a key failed because its sending device is just not known (yet),
/// as opposed to being known and wrong. `sender_device` is as in
/// [`check_key_event_fields`]; `state` is the event's verification state.
fn device_not_known_yet(
    sender_device: Option<Option<&str>>,
    state: Option<&matrix_sdk::deserialized_responses::VerificationState>,
) -> bool {
    use matrix_sdk::deserialized_responses::{DeviceLinkProblem, VerificationLevel, VerificationState};
    matches!(sender_device, Some(None))
        || matches!(
            state,
            Some(VerificationState::Unverified(VerificationLevel::None(DeviceLinkProblem::MissingDevice)))
        )
}

/// The verdict on a frame-key event from what the event and the room say, with
/// no I/O: the content checks and the device-trust check, and whether a
/// failure can still clear up. `info` is the sending device
/// and the event's verification state, `None` when it arrived unencrypted.
fn judge_key_event(
    expected_room: &str,
    c: &crate::client::rtc::model::MediaKeyContent,
    sender: &str,
    info: Option<(Option<&str>, &matrix_sdk::deserialized_responses::VerificationState)>,
    members: &[crate::client::rtc::members::CallMember],
    exclude_insecure: bool,
) -> Result<AcceptedKey, KeyRejection> {
    let not_yet = device_not_known_yet(info.map(|(d, _)| d), info.map(|(_, s)| s));
    let reject = |why: String| if not_yet { KeyRejection::Retry(why) } else { KeyRejection::Final(why) };
    let accepted =
        evaluate_key_event(expected_room, c, sender, info.map(|(d, _)| d), members).map_err(reject)?;
    if let Some((_, state)) = info {
        check_device_trusted(state, exclude_insecure).map_err(reject)?;
    }
    Ok(accepted)
}

/// [`judge_key_event`] for a decrypted to-device event.
fn accept_key_event(
    room: &Room,
    sender: &matrix_sdk::ruma::UserId,
    content: &crate::client::rtc::model::MediaKeyContent,
    info: Option<&matrix_sdk::deserialized_responses::EncryptionInfo>,
    members: &[crate::client::rtc::members::CallMember],
) -> Result<AcceptedKey, KeyRejection> {
    judge_key_event(
        room.room_id().as_str(),
        content,
        sender.as_str(),
        info.map(|i| (i.sender_device.as_deref().map(|d| d.as_str()), &i.verification_state)),
        members,
        crate::client::identity::exclude_insecure_devices(),
    )
}

/// The longest base64 key we look at: a 32-byte key is 44 characters.
const MAX_KEY_B64_LEN: usize = 256;
/// The longest `member_id` we look at (the longest real one is a UUID).
const MAX_MEMBER_ID_LEN: usize = 512;

/// The checks a key event must pass before anything is read from the room:
/// encrypted, for this room, and of a sane size. Anyone who can message us
/// can send events, so nothing here may cost more than a comparison.
fn cheap_key_checks(
    expected_room: &str,
    content: &crate::client::rtc::model::MediaKeyContent,
    encrypted: bool,
) -> Result<(), String> {
    if !encrypted {
        return Err("sent unencrypted".to_owned());
    }
    if content.room_id != expected_room {
        return Err(format!("for another room ({})", content.room_id));
    }
    let (_, key_b64) = content.media_key_parts().ok_or_else(|| "carries no key".to_owned())?;
    if key_b64.len() > MAX_KEY_B64_LEN {
        return Err(format!("key is {} characters long", key_b64.len()));
    }
    if content.member_id.as_ref().is_some_and(|id| id.len() > MAX_MEMBER_ID_LEN) {
        return Err("member id is too long".to_owned());
    }
    Ok(())
}

/// Whether the sender of a key is joined to the room.
#[derive(Debug, PartialEq, Eq)]
enum SenderJoin {
    Joined,
    NotJoined,
    /// The lookup failed: nothing is known about the sender.
    Unknown(String),
}

/// The verdict on a member lookup: only a definite answer rules a sender out.
fn classify_sender<E: std::fmt::Display>(
    found: Result<Option<matrix_sdk::ruma::events::room::member::MembershipState>, E>,
) -> SenderJoin {
    use matrix_sdk::ruma::events::room::member::MembershipState;
    match found {
        Ok(Some(MembershipState::Join)) => SenderJoin::Joined,
        Ok(_) => SenderJoin::NotJoined,
        Err(e) => SenderJoin::Unknown(format!("could not look up the sender's membership: {e}")),
    }
}

/// The sender must be joined to the room: anything else could inject keys
/// from outsiders.
async fn sender_is_joined(room: &Room, sender: &matrix_sdk::ruma::UserId) -> SenderJoin {
    classify_sender(room.get_member(sender).await.map(|m| m.map(|m| m.membership().clone())))
}

/// The verification state of a device we now know, the way the crypto layer
/// states it for a decrypted event.
fn verification_state_for_device(
    verified: bool,
    cross_signed_by_owner: bool,
    identity_has_violation: bool,
) -> matrix_sdk::deserialized_responses::VerificationState {
    use matrix_sdk::deserialized_responses::{VerificationLevel, VerificationState};
    if verified {
        VerificationState::Verified
    } else if cross_signed_by_owner {
        VerificationState::Unverified(if identity_has_violation {
            VerificationLevel::VerificationViolation
        } else {
            VerificationLevel::UnverifiedIdentity
        })
    } else {
        VerificationState::Unverified(VerificationLevel::UnsignedDevice)
    }
}

/// `info` as it would read now: the sending device, found by the Olm key the
/// event came from, and its trust as the crypto store has it. `None` while
/// the device is still not known.
async fn refreshed_encryption_info(
    client: &matrix_sdk::Client,
    info: &matrix_sdk::deserialized_responses::EncryptionInfo,
) -> Option<matrix_sdk::deserialized_responses::EncryptionInfo> {
    use matrix_sdk::deserialized_responses::AlgorithmInfo;
    let AlgorithmInfo::OlmV1Curve25519AesSha2 { curve25519_public_key_base64: curve } = &info.algorithm_info
    else {
        return None;
    };
    let devices = client.encryption().get_user_devices(&info.sender).await.ok()?;
    let device = devices
        .devices()
        .find(|d| d.curve25519_key().is_some_and(|k| k.to_base64() == *curve))?;
    let violation = client
        .encryption()
        .get_user_identity(&info.sender)
        .await
        .ok()
        .flatten()
        .is_some_and(|i| i.has_verification_violation());
    Some(matrix_sdk::deserialized_responses::EncryptionInfo {
        sender_device: Some(device.device_id().to_owned()),
        verification_state: verification_state_for_device(
            device.is_verified(),
            device.is_cross_signed_by_owner(),
            violation,
        ),
        ..info.clone()
    })
}

/// Most one log line per sender and this long, for events we ignore or park.
const IGNORED_KEY_WARN_INTERVAL: Duration = Duration::from_secs(10);
/// A sender whose unknown device we ask the homeserver about at most this often.
const DEVICE_QUERY_INTERVAL: Duration = Duration::from_secs(30);
/// Senders remembered by the two limiters above.
const LIMITER_SENDERS: usize = 256;

/// Everything a key-event handler needs; cloned per event.
#[derive(Clone)]
struct KeyCtx {
    shared: SharedKeys,
    holder: Arc<PLMutex<Option<Arc<SfuSet>>>>,
    room: Room,
    parked: Arc<PLMutex<ParkedKeys>>,
    own_user: matrix_sdk::ruma::OwnedUserId,
    own_device: matrix_sdk::ruma::OwnedDeviceId,
    /// Wakes the reconcile loop, which judges parked keys again.
    wake: tokio::sync::mpsc::Sender<()>,
    ignored_warns: Arc<PLMutex<RateLimiter>>,
    parked_notes: Arc<PLMutex<RateLimiter>>,
    device_queries: Arc<PLMutex<RateLimiter>>,
}

impl KeyCtx {
    fn new(
        shared: SharedKeys,
        holder: Arc<PLMutex<Option<Arc<SfuSet>>>>,
        room: Room,
        parked: Arc<PLMutex<ParkedKeys>>,
        own_user: matrix_sdk::ruma::OwnedUserId,
        own_device: matrix_sdk::ruma::OwnedDeviceId,
        wake: tokio::sync::mpsc::Sender<()>,
    ) -> Self {
        Self {
            shared,
            holder,
            room,
            parked,
            own_user,
            own_device,
            wake,
            ignored_warns: Arc::new(PLMutex::new(RateLimiter::new(IGNORED_KEY_WARN_INTERVAL, LIMITER_SENDERS))),
            parked_notes: Arc::new(PLMutex::new(RateLimiter::new(IGNORED_KEY_WARN_INTERVAL, LIMITER_SENDERS))),
            device_queries: Arc::new(PLMutex::new(RateLimiter::new(DEVICE_QUERY_INTERVAL, LIMITER_SENDERS))),
        }
    }

    /// Say why a key event was ignored, once per sender per interval.
    fn warn_ignored(&self, sender: &str, why: &str) {
        if self.ignored_warns.lock().allow(sender, Instant::now()) {
            warn!("e2ee: ignoring key event from {sender}: {why}");
        }
    }

    /// Whether to log that a key from `sender` was parked: once per sender per
    /// interval, since a member can send these as fast as it likes.
    fn note_parked(&self, sender: &str) -> bool {
        self.parked_notes.lock().allow(sender, Instant::now())
    }

    /// Ask the homeserver for the devices of `sender` (once in a while), then
    /// have the parked keys judged again.
    fn query_devices_of(&self, sender: &matrix_sdk::ruma::UserId) {
        if !self.device_queries.lock().allow(sender.as_str(), Instant::now()) {
            return;
        }
        let client = self.room.client();
        let sender = sender.to_owned();
        let wake = self.wake.clone();
        tokio::spawn(async move {
            if let Err(e) = client.encryption().request_user_identity(&sender).await {
                warn!("e2ee: could not query the devices of {sender}: {e}");
            }
            let _ = wake.try_send(());
        });
    }

    /// A key that passed the checks: use it for its member, or park it until
    /// that member's membership shows up.
    fn apply_or_park(
        &self,
        sender: &str,
        accepted: AcceptedKey,
        members: &[crate::client::rtc::members::CallMember],
    ) {
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
        self.shared.identities().replace(hashed_identities(members));
        let keys: Vec<(String, String)> = members.iter().map(|m| m.key()).collect();
        if !is_live_member(&keys, sender, device) {
            if self.note_parked(sender) {
                info!("e2ee: parking key from {sender}/{device}: no live membership yet");
            }
            self.parked.lock().park(
                ParkedKey {
                    user_id: sender.to_owned(),
                    device_id: device.to_owned(),
                    index,
                    raw_key,
                    recheck: None,
                },
                Instant::now(),
            );
            return;
        }
        let set = self.holder.lock().clone();
        match set {
            Some(set) => set.apply_key(sender, device, index, raw_key),
            None => self.shared.store(sender, device, index, raw_key),
        }
    }

    /// Keep a key whose sending device is not known yet, to judge it again at
    /// the next reconcile pass, and ask the homeserver for the sender's devices.
    fn park_for_recheck(
        &self,
        sender: &matrix_sdk::ruma::UserId,
        content: crate::client::rtc::model::MediaKeyContent,
        info: matrix_sdk::deserialized_responses::EncryptionInfo,
        at: Instant,
    ) {
        use matrix_sdk::deserialized_responses::AlgorithmInfo;
        let AlgorithmInfo::OlmV1Curve25519AesSha2 { curve25519_public_key_base64: curve } = &info.algorithm_info
        else {
            self.warn_ignored(sender.as_str(), "sender device is unknown and cannot be looked up");
            return;
        };
        if self.note_parked(sender.as_str()) {
            info!("e2ee: parking key from {sender}: sending device not known yet");
        }
        let key = ParkedKey {
            user_id: sender.to_string(),
            device_id: recheck_device_id(curve),
            index: 0,
            raw_key: Vec::new(),
            recheck: Some(Recheck { content, info }),
        };
        self.parked.lock().park(key, at);
    }
}

async fn handle_key_event(
    ctx: KeyCtx,
    sender_id: matrix_sdk::ruma::OwnedUserId,
    content: crate::client::rtc::model::MediaKeyContent,
    info: Option<matrix_sdk::deserialized_responses::EncryptionInfo>,
) {
    let sender = sender_id.to_string();
    // Cheapest first: anyone may send us this event, so nothing is read from
    // the room until the event has earned it.
    if let Err(why) = cheap_key_checks(ctx.room.room_id().as_str(), &content, info.is_some()) {
        ctx.warn_ignored(&sender, &why);
        return;
    }
    match sender_is_joined(&ctx.room, &sender_id).await {
        SenderJoin::Joined => {}
        SenderJoin::NotJoined => {
            ctx.warn_ignored(&sender, "sender is not joined to the room");
            return;
        }
        SenderJoin::Unknown(why) => {
            ctx.warn_ignored(&sender, &why);
            return;
        }
    }
    let members = read_all_live_members(&ctx.room, &ctx.own_user, &ctx.own_device).await;
    match accept_key_event(&ctx.room, &sender_id, &content, info.as_ref(), &members) {
        Ok(accepted) => ctx.apply_or_park(&sender, accepted, &members),
        Err(KeyRejection::Retry(why)) => {
            if let Some(info) = info {
                tracing::debug!("e2ee: key from {sender} cannot be judged yet: {why}");
                ctx.park_for_recheck(&sender_id, content, info, Instant::now());
                ctx.query_devices_of(&sender_id);
            }
        }
        Err(KeyRejection::Final(why)) => ctx.warn_ignored(&sender, &why),
    }
}

/// Judge the keys parked for want of a known sending device again: the ones
/// whose device is known now are used (or parked for their membership), the
/// ones that turn out to be untrustworthy are dropped, and the rest wait on
/// until their time is up.
async fn recheck_parked_keys(ctx: &KeyCtx, members: &[crate::client::rtc::members::CallMember]) {
    let waiting = ctx.parked.lock().take_rechecks(Instant::now());
    if waiting.is_empty() {
        return;
    }
    let client = ctx.room.client();
    for (parked_at, key) in waiting {
        let Some(recheck) = key.recheck else { continue };
        let Ok(sender_id) = matrix_sdk::ruma::UserId::parse(&key.user_id) else { continue };
        let Some(info) = refreshed_encryption_info(&client, &recheck.info).await else {
            ctx.parked.lock().park(
                ParkedKey { recheck: Some(recheck), ..key },
                parked_at,
            );
            continue;
        };
        match sender_is_joined(&ctx.room, &sender_id).await {
            SenderJoin::Joined => {}
            SenderJoin::NotJoined => {
                ctx.warn_ignored(&key.user_id, "sender is not joined to the room");
                continue;
            }
            SenderJoin::Unknown(why) => {
                // A failed lookup says nothing: judge it again at the next pass.
                tracing::debug!("e2ee: {why}; keeping the key from {} to judge again", key.user_id);
                ctx.parked.lock().park(ParkedKey { recheck: Some(recheck), ..key }, parked_at);
                continue;
            }
        }
        match accept_key_event(&ctx.room, &sender_id, &recheck.content, Some(&info), members) {
            Ok(accepted) => {
                tracing::debug!("e2ee: judged the key from {} now that its device is known", key.user_id);
                ctx.apply_or_park(&key.user_id, accepted, members);
            }
            Err(KeyRejection::Retry(_)) => {
                ctx.parked.lock().park(ParkedKey { recheck: Some(recheck), ..key }, parked_at);
            }
            Err(KeyRejection::Final(why)) => ctx.warn_ignored(&key.user_id, &why),
        }
    }
}

// ---------------------------------------------------------------------------
// Background tasks
// ---------------------------------------------------------------------------

/// Keeps our membership alive: heartbeats the MSC4140 delayed leave every few
/// seconds and resends the membership event hourly (with a fresh `expires` and
/// the original `created_ts`, so peers see a stable join age). Sends nothing
/// once `announced` is closed.
fn spawn_membership_task(
    room: Room,
    member_id: String,
    service_url: String,
    lk_alias: String,
    device_id: String,
    user_id: String,
    audio_only: bool,
    announced: Arc<Announced>,
) -> AbortHandle {
    tokio::spawn(async move {
        let client = room.client();
        let delay_id = &announced.delay_id;
        let mut delayed_ok = true;
        if announced.is_closed() {
            return;
        }
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
            if announced.is_closed() {
                return;
            }
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
                // Held until the resend and its delayed leave are out, so a
                // teardown's leave cannot be overtaken by them.
                let Some(_open) = announced.open_for_join().await else {
                    return;
                };
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

/// Hand the rotator's decisions on: keys to the delivery worker, activations to the gate.
fn apply_key_actions(actions: Vec<RotationAction>, delivery: &KeyDelivery, gate: &mut ActivationGate, now: Instant) {
    for action in actions {
        match action {
            RotationAction::Send { index, key_b64, to } => delivery.enqueue(&to, index, &key_b64),
            RotationAction::Activate { index, raw_key } => gate.hold(index, raw_key, now),
        }
    }
}

/// Put the held key to use if its delivery has settled (or the wait is over),
/// and tell the rotator, which then counts it as the key in use.
fn release_gate(
    gate: &mut ActivationGate,
    delivery: &KeyDelivery,
    e2ee: &Arc<PLMutex<KeyRotator>>,
    sfus: &Arc<SfuSet>,
) {
    let now = Instant::now();
    let Some((index, raw_key)) = gate.release(|i| delivery.settled(i), now) else { return };
    sfus.primary.set_own_frame_key(&raw_key, index as i32);
    let (more, live) = {
        let mut rotator = e2ee.lock();
        let more = rotator.activated(index, now);
        (more, rotator.live_indices())
    };
    apply_key_actions(more, delivery, gate, now);
    delivery.retain_indices(&live);
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
    delivery: KeyDelivery,
    use_e2ee: bool,
    mut previous: HashSet<(String, String)>,
    mut changes: tokio::sync::mpsc::Receiver<()>,
    key_ctx: KeyCtx,
    mrtc: Option<Arc<MrtcMembership>>,
    announced: Arc<Announced>,
    session_id: u64,
) {
    let mut interval = tokio::time::interval(RECONCILE_INTERVAL);
    interval.tick().await;
    let mut first = true;
    // A new key that waits for its first delivery round before it is used.
    let mut gate = ActivationGate::default();
    loop {
        if !first {
            let key_deadline = e2ee
                .lock()
                .next_deadline()
                .map(tokio::time::Instant::from_std)
                .unwrap_or_else(|| tokio::time::Instant::now() + RECONCILE_INTERVAL);
            let gate_deadline = gate.deadline().map(tokio::time::Instant::from_std);
            let mut only_the_gate = false;
            tokio::select! {
                _ = changes.recv() => {}
                _ = interval.tick() => {}
                _ = tokio::time::sleep_until(key_deadline), if use_e2ee => {}
                _ = async {
                    delivery.progressed().await;
                    // Sends end one after the other: look once for the burst.
                    tokio::time::sleep(GATE_RECHECK_COALESCE).await;
                }, if gate_deadline.is_some() => { only_the_gate = true; }
                _ = tokio::time::sleep_until(gate_deadline.unwrap_or(key_deadline)), if gate_deadline.is_some() => {}
            }
            // A send ended while a key waits to be used: that is all there is to look at.
            if only_the_gate {
                release_gate(&mut gate, &delivery, &e2ee, &sfus);
                continue;
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
        // Media keeps flowing unencrypted once the room turns encrypted; the
        // call cannot be repaired from here, so end it and say why.
        if encryption_flipped_mid_call(use_e2ee, room.encryption_state().is_encrypted()) {
            warn!("rtc: the room became encrypted during a call without frame encryption; ending the call");
            // Nothing may announce us again. This loop is left running: it is
            // the caller and returns below.
            announced.close();
            // The UI leaves the call and the media stops first, so neither waits
            // for the network; the leaves below may take a while offline. The
            // reason goes out before the disconnect, whose own end event would say less.
            if let Some(sink) = super::sink_for_client(&client) {
                sink.on_session_ended(session_id, "encryption_error");
            }
            sfus.disconnect_all().await;
            announced
                .withdraw(LeaveCode::SlotClosed, Some("the room became encrypted"))
                .await;
            return;
        }
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
        let released = key_ctx
            .parked
            .lock()
            .take_for(&current.iter().cloned().collect::<Vec<_>>(), Instant::now());
        for k in released {
            info!("e2ee: applying parked key from {}/{}", k.user_id, k.device_id);
            sfus.apply_key(&k.user_id, &k.device_id, k.index, k.raw_key);
        }
        // Keys whose sending device was not known when they arrived.
        recheck_parked_keys(&key_ctx, &members).await;
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
            let sessions: Sessions = members.iter().map(|m| (m.key(), m.session_tag())).collect();
            delivery.retain_recipients(&current);
            let actions = {
                let mut rotator = e2ee.lock();
                let mut a = rotator.on_members_changed(&sessions, now);
                a.extend(rotator.poll(now));
                a
            };
            // Sending is the delivery worker's job: nothing here waits on the
            // network. A new key is only put to use once its delivery has
            // settled (see `ActivationGate`).
            apply_key_actions(actions, &delivery, &mut gate, now);
            delivery.retain_indices(&e2ee.lock().live_indices());
            release_gate(&mut gate, &delivery, &e2ee, &sfus);
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

                // The same event may also be a sticky invite the sticky-map
                // watcher announces under its real slot: announce it only once.
                if !super::invites::claim_legacy_ring(&room, &notification_event_id) {
                    return;
                }
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

                // The same event may also be a sticky invite the sticky-map
                // watcher announces under its real slot: announce it only once.
                if !super::invites::claim_legacy_ring(&room, &notification_event_id) {
                    return;
                }
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

    // ── Judging a key whose sending device may not be known yet ────────────────

    use matrix_sdk::deserialized_responses::{DeviceLinkProblem, VerificationLevel, VerificationState};

    fn missing() -> VerificationState {
        VerificationState::Unverified(VerificationLevel::None(DeviceLinkProblem::MissingDevice))
    }

    fn judge(
        device: Option<&str>,
        state: &VerificationState,
        strict: bool,
    ) -> Result<AcceptedKey, KeyRejection> {
        let c = stable("!r:x.org", "unseen");
        judge_key_event("!r:x.org", &c, "@a:x.org", Some((device, state)), &[], strict)
    }

    #[test]
    fn a_key_from_a_device_not_known_yet_is_worth_judging_again() {
        // Strict or not, the verdict has to wait for the device.
        for strict in [true, false] {
            assert!(matches!(judge(None, &missing(), strict), Err(KeyRejection::Retry(_))), "strict={strict}");
        }
        assert!(device_not_known_yet(Some(None), None));
        assert!(device_not_known_yet(Some(Some("DEV")), Some(&missing())));
        assert!(!device_not_known_yet(Some(Some("DEV")), Some(&VerificationState::Verified)));
        assert!(!device_not_known_yet(None, None), "unencrypted is final");
    }

    #[test]
    fn a_known_device_that_is_not_trusted_is_rejected_outright() {
        let unsigned = VerificationState::Unverified(VerificationLevel::UnsignedDevice);
        assert!(matches!(judge(Some("DEV"), &unsigned, true), Err(KeyRejection::Final(_))));
        let violation = VerificationState::Unverified(VerificationLevel::VerificationViolation);
        assert!(matches!(judge(Some("DEV"), &violation, true), Err(KeyRejection::Final(_))));
        let mismatched = VerificationState::Unverified(VerificationLevel::MismatchedSender);
        assert!(matches!(judge(Some("DEV"), &mismatched, true), Err(KeyRejection::Final(_))));
    }

    #[test]
    fn an_insecure_source_is_final() {
        let insecure = VerificationState::Unverified(VerificationLevel::None(DeviceLinkProblem::InsecureSource));
        // Not a device we are waiting to learn about: this will not get better.
        assert!(!device_not_known_yet(Some(Some("DEV")), Some(&insecure)));
        assert!(matches!(judge(Some("DEV"), &insecure, true), Err(KeyRejection::Final(_))));
    }

    #[test]
    fn a_known_trusted_device_is_accepted_and_lax_mode_ignores_trust() {
        assert!(judge(Some("DEV"), &VerificationState::Verified, true).is_ok());
        let unsigned = VerificationState::Unverified(VerificationLevel::UnsignedDevice);
        assert!(judge(Some("DEV"), &unsigned, false).is_ok());
    }

    #[test]
    fn an_unencrypted_key_is_final() {
        let c = stable("!r:x.org", "m");
        assert_eq!(
            judge_key_event("!r:x.org", &c, "@a:x.org", None, &[], true).unwrap_err(),
            KeyRejection::Final("sent unencrypted".to_owned())
        );
    }

    #[test]
    fn a_wrong_member_is_final_for_a_known_device() {
        let members = vec![mrtc_member("@a:x.org", "DEV", "mid1")];
        let c = stable("!r:x.org", "mid1");
        let r = judge_key_event(
            "!r:x.org",
            &c,
            "@a:x.org",
            Some((Some("OTHER"), &VerificationState::Verified)),
            &members,
            true,
        );
        assert!(matches!(r, Err(KeyRejection::Final(_))));
    }

    #[test]
    fn a_device_found_later_is_judged_by_how_the_crypto_layer_states_it() {
        use VerificationLevel::*;
        assert_eq!(verification_state_for_device(true, false, false), VerificationState::Verified);
        assert_eq!(verification_state_for_device(true, true, true), VerificationState::Verified);
        assert_eq!(
            verification_state_for_device(false, true, false),
            VerificationState::Unverified(UnverifiedIdentity)
        );
        assert_eq!(
            verification_state_for_device(false, true, true),
            VerificationState::Unverified(VerificationViolation)
        );
        assert_eq!(
            verification_state_for_device(false, false, false),
            VerificationState::Unverified(UnsignedDevice)
        );
        // ...so strict mode takes the first two kinds and refuses the rest.
        assert!(check_device_trusted(&verification_state_for_device(false, true, false), true).is_ok());
        assert!(check_device_trusted(&verification_state_for_device(false, true, true), true).is_err());
        assert!(check_device_trusted(&verification_state_for_device(false, false, false), true).is_err());
    }

    // ── Checks before the room is read ─────────────────────────────────────────

    #[test]
    fn cheap_checks_turn_away_bad_events_without_the_room() {
        let ok = stable("!r:x.org", "m");
        assert!(cheap_key_checks("!r:x.org", &ok, true).is_ok());
        assert_eq!(cheap_key_checks("!r:x.org", &ok, false).unwrap_err(), "sent unencrypted");
        assert!(cheap_key_checks("!other:x.org", &ok, true).unwrap_err().contains("another room"));
        let none = media(serde_json::json!({ "room_id": "!r:x.org", "member_id": "m" }));
        assert_eq!(cheap_key_checks("!r:x.org", &none, true).unwrap_err(), "carries no key");
    }

    #[test]
    fn cheap_checks_cap_the_size_of_what_we_look_at() {
        let long_key = media(serde_json::json!({
            "room_id": "!r:x.org", "member_id": "m",
            "media_key": { "index": 1, "key": "A".repeat(MAX_KEY_B64_LEN + 1) }
        }));
        assert!(cheap_key_checks("!r:x.org", &long_key, true).unwrap_err().contains("characters"));
        let long_id = media(serde_json::json!({
            "room_id": "!r:x.org", "member_id": "m".repeat(MAX_MEMBER_ID_LEN + 1),
            "media_key": { "index": 1, "key": "AAAA" }
        }));
        assert!(cheap_key_checks("!r:x.org", &long_id, true).is_err());
        let edge = media(serde_json::json!({
            "room_id": "!r:x.org", "member_id": "m".repeat(MAX_MEMBER_ID_LEN),
            "media_key": { "index": 1, "key": "A".repeat(MAX_KEY_B64_LEN) }
        }));
        assert!(cheap_key_checks("!r:x.org", &edge, true).is_ok());
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
mod sender_membership_tests {
    use super::*;
    use matrix_sdk::ruma::events::room::member::MembershipState;

    #[test]
    fn only_a_definite_answer_rules_a_sender_out() {
        assert_eq!(classify_sender::<String>(Ok(Some(MembershipState::Join))), SenderJoin::Joined);
        for gone in [MembershipState::Leave, MembershipState::Ban, MembershipState::Invite] {
            assert_eq!(classify_sender::<String>(Ok(Some(gone))), SenderJoin::NotJoined);
        }
        assert_eq!(classify_sender::<String>(Ok(None)), SenderJoin::NotJoined);
        // A failed request is not "not joined": the parked key must survive it.
        assert!(matches!(classify_sender(Err("timed out")), SenderJoin::Unknown(_)));
    }

    #[test]
    fn a_profile_change_does_not_wake_the_reconcile_loop_but_a_kick_does() {
        use MembershipState::{Ban, Invite, Join, Leave};
        assert!(!membership_event_matters(&Join, Some(&Join)), "a new display name");
        assert!(membership_event_matters(&Leave, Some(&Join)), "a leave or a kick");
        assert!(membership_event_matters(&Ban, Some(&Join)));
        assert!(membership_event_matters(&Join, Some(&Invite)), "a join");
        assert!(membership_event_matters(&Join, None), "no previous state to compare with");
    }
}

#[cfg(test)]
mod id_tests {
    use super::*;

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

#[cfg(test)]
mod announced_tests {
    use super::*;
    use crate::client::rtc::mrtc_membership::StickySink;
    use serde_json::Value;
    use std::sync::{atomic::AtomicUsize, Mutex};

    type Log = Arc<Mutex<Vec<String>>>;

    /// Records every send in one shared log, so the order across the legacy,
    /// sticky and invite parts can be asserted.
    struct FakeSignals {
        log: Log,
        legacy_failures: AtomicUsize,
        invite_failures: AtomicUsize,
        answered: AtomicBool,
        /// The next legacy leave never finishes (a request stuck in flight).
        hang_first_legacy: AtomicBool,
    }

    /// Count one failure off `n`; whether there was one left.
    fn take_one(n: &AtomicUsize) -> bool {
        let left = n.load(Ordering::SeqCst);
        n.store(left.saturating_sub(1), Ordering::SeqCst);
        left > 0
    }

    #[async_trait::async_trait]
    impl Signals for FakeSignals {
        async fn legacy_leave(&self, member_id: &str) -> anyhow::Result<()> {
            let fail = take_one(&self.legacy_failures);
            let hang = self.hang_first_legacy.swap(false, Ordering::SeqCst);
            if hang {
                self.log.lock().unwrap().push(format!("legacy-leave {member_id} (hung)"));
                std::future::pending::<()>().await;
            }
            self.log.lock().unwrap().push(format!("legacy-leave {member_id}{}", if fail { " (failed)" } else { "" }));
            if fail {
                anyhow::bail!("offline");
            }
            Ok(())
        }

        async fn cancel_delayed(&self, delay_id: &str) {
            self.log.lock().unwrap().push(format!("cancel-delayed {delay_id}"));
        }

        async fn call_answered(&self) -> bool {
            self.answered.load(Ordering::SeqCst)
        }

        async fn withdraw_invite(&self) -> anyhow::Result<()> {
            let fail = take_one(&self.invite_failures);
            self.log.lock().unwrap().push(format!("withdraw-invite{}", if fail { " (failed)" } else { "" }));
            if fail {
                anyhow::bail!("offline");
            }
            Ok(())
        }
    }

    struct LogSticky(Log);

    #[async_trait::async_trait]
    impl StickySink for LogSticky {
        async fn send(&self, _t: &str, content: Value, _d: Duration) -> anyhow::Result<()> {
            let kind = if content["member"]["membership"] == "leave" { "sticky-leave" } else { "sticky-join" };
            self.0.lock().unwrap().push(kind.to_owned());
            Ok(())
        }
    }

    fn fake(legacy_failures: usize, invite_failures: usize) -> (Announced, Log, Arc<FakeSignals>) {
        let log: Log = Arc::default();
        let signals = Arc::new(FakeSignals {
            log: log.clone(),
            legacy_failures: AtomicUsize::new(legacy_failures),
            invite_failures: AtomicUsize::new(invite_failures),
            answered: AtomicBool::new(false),
            hang_first_legacy: AtomicBool::new(false),
        });
        (Announced::new(signals.clone()), log, signals)
    }

    fn with_sticky(a: &Announced, log: &Log) {
        a.set_membership(Arc::new(MrtcMembership::with_sink(
            Arc::new(LogSticky(log.clone())),
            JoinParams {
                member_id: "m1".into(),
                user_id: "@a:x".into(),
                device_id: "D".into(),
                sfu_url: "wss://s".into(),
                service_url: "https://j".into(),
                audio_only: false,
            },
            true,
        )));
    }

    fn entries(log: &Log) -> Vec<String> {
        log.lock().unwrap().clone()
    }

    #[tokio::test(start_paused = true)]
    async fn withdraw_takes_back_legacy_then_delayed_then_sticky_then_invite() {
        let (a, log, _) = fake(0, 0);
        with_sticky(&a, &log);
        a.joined_legacy("id1");
        a.joined_legacy("id2");
        a.joined_legacy("id1"); // recorded once
        *a.delay_id.lock() = Some("delay9".into());
        a.invited();
        a.withdraw(LeaveCode::Leave, None).await;
        assert_eq!(
            entries(&log),
            ["legacy-leave id1", "legacy-leave id2", "cancel-delayed delay9", "sticky-leave", "withdraw-invite"]
        );
        assert!(a.delay_id.lock().is_none());
    }

    #[tokio::test(start_paused = true)]
    async fn a_delivered_withdrawal_is_not_repeated() {
        let (a, log, _) = fake(0, 0);
        with_sticky(&a, &log);
        a.joined_legacy("id1");
        a.invited();
        a.withdraw(LeaveCode::Leave, None).await;
        let first = entries(&log);
        a.withdraw(LeaveCode::Leave, None).await;
        a.withdraw(LeaveCode::SlotClosed, Some("again")).await;
        assert_eq!(entries(&log), first, "end_call then drop sends nothing twice");
    }

    #[tokio::test(start_paused = true)]
    async fn a_failed_legacy_leave_is_retried_once_then_again_by_the_next_withdrawal() {
        let (a, log, _) = fake(3, 0);
        a.joined_legacy("id1");
        *a.delay_id.lock() = Some("delay9".into());
        a.withdraw(LeaveCode::Leave, None).await;
        assert_eq!(
            entries(&log),
            ["legacy-leave id1 (failed)", "legacy-leave id1 (failed)"],
            "one retry, and the delayed leave stays as the fallback"
        );
        assert_eq!(a.delay_id.lock().as_deref(), Some("delay9"));
        // The next request (the session's drop) retries and, with one failure
        // left, succeeds on its own retry.
        a.withdraw(LeaveCode::Leave, None).await;
        assert_eq!(
            entries(&log)[2..],
            ["legacy-leave id1 (failed)", "legacy-leave id1", "cancel-delayed delay9"]
        );
        a.withdraw(LeaveCode::Leave, None).await;
        assert_eq!(entries(&log).len(), 5, "delivered: nothing more");
    }

    #[tokio::test(start_paused = true)]
    async fn a_withdrawal_cut_short_by_end_calls_timeout_is_finished_by_the_drop() {
        let (a, log, signals) = fake(0, 0);
        with_sticky(&a, &log);
        a.joined_legacy("id1");
        a.invited();
        signals.hang_first_legacy.store(true, Ordering::SeqCst);
        let cut = tokio::time::timeout(END_CALL_WITHDRAW_TIMEOUT, a.withdraw(LeaveCode::Leave, None)).await;
        assert!(cut.is_err(), "end_call stops waiting");
        assert_eq!(entries(&log), ["legacy-leave id1 (hung)"]);
        // The session's drop asks again and completes every part.
        a.withdraw(LeaveCode::Leave, None).await;
        assert_eq!(entries(&log)[1..], ["legacy-leave id1", "sticky-leave", "withdraw-invite"]);
    }

    #[tokio::test(start_paused = true)]
    async fn the_invite_is_withdrawn_only_for_a_call_nobody_joined() {
        let (a, log, signals) = fake(0, 0);
        a.invited();
        signals.answered.store(true, Ordering::SeqCst);
        a.withdraw(LeaveCode::Leave, None).await;
        assert!(entries(&log).is_empty(), "an answered call keeps its ring record");

        let (a, log, _) = fake(0, 0);
        a.withdraw(LeaveCode::Leave, None).await;
        assert!(entries(&log).is_empty(), "no invite was sent, none is withdrawn");
    }

    #[tokio::test(start_paused = true)]
    async fn a_failed_invite_withdrawal_is_attempted_again() {
        let (a, log, _) = fake(0, 1);
        a.invited();
        a.withdraw(LeaveCode::Leave, None).await;
        a.withdraw(LeaveCode::Leave, None).await;
        a.withdraw(LeaveCode::Leave, None).await;
        assert_eq!(entries(&log), ["withdraw-invite (failed)", "withdraw-invite"]);
    }

    #[tokio::test(start_paused = true)]
    async fn a_sticky_leave_is_sent_even_when_its_join_never_confirmed() {
        // The membership is recorded before its join; a withdrawal covers it.
        let (a, log, _) = fake(0, 0);
        with_sticky(&a, &log);
        a.withdraw(LeaveCode::TransportError, Some("lost")).await;
        assert_eq!(entries(&log), ["sticky-leave"]);
    }

    #[tokio::test(start_paused = true)]
    async fn a_wrong_identity_stays_tracked_until_its_leave_is_out() {
        let (a, log, _) = fake(2, 0);
        a.joined_legacy("wrong");
        a.joined_legacy("right");
        a.leave_legacy_identity("wrong").await;
        assert_eq!(a.legacy_ids.lock().clone(), ["wrong", "right"], "both sends failed");
        a.leave_legacy_identity("wrong").await;
        assert_eq!(a.legacy_ids.lock().clone(), ["right"]);
        assert_eq!(entries(&log).len(), 3);
    }

    #[tokio::test]
    async fn nothing_joins_once_the_call_is_closed() {
        let (a, _, _) = fake(0, 0);
        assert!(a.open_for_join().await.is_some());
        a.close();
        assert!(a.is_closed());
        assert!(a.open_for_join().await.is_none(), "the heartbeat must not send a join");
    }

    #[tokio::test(start_paused = true)]
    async fn nothing_joins_after_the_legacy_leave_is_out() {
        let (a, _, _) = fake(0, 0);
        a.joined_legacy("id1");
        a.withdraw(LeaveCode::Leave, None).await;
        assert!(a.open_for_join().await.is_none());
    }

    #[tokio::test]
    async fn closing_stops_the_announcing_tasks_and_a_late_task_at_once() {
        let (a, _, _) = fake(0, 0);
        let heartbeat = tokio::spawn(std::future::pending::<()>());
        let reconcile = tokio::spawn(std::future::pending::<()>());
        a.register_task(heartbeat.abort_handle());
        a.register_reconcile(reconcile.abort_handle());
        a.close();
        assert!(heartbeat.await.unwrap_err().is_cancelled());
        // The reconcile loop is the one task a teardown it drives must not abort.
        tokio::task::yield_now().await;
        assert!(!reconcile.is_finished());
        a.shutdown();
        assert!(reconcile.await.unwrap_err().is_cancelled());

        let late = tokio::spawn(std::future::pending::<()>());
        a.register_task(late.abort_handle());
        assert!(late.await.unwrap_err().is_cancelled(), "registered after the close");
        let late_reconcile = tokio::spawn(std::future::pending::<()>());
        a.register_reconcile(late_reconcile.abort_handle());
        assert!(late_reconcile.await.unwrap_err().is_cancelled());
    }
}
