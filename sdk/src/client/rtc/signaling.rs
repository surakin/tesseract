use anyhow::Context as _;
use matrix_sdk::ruma::events::macros::EventContent;
use serde::{Deserialize, Serialize};

/// Participant info shared with the C++ layer (also re-exported from mod.rs).
#[derive(Clone, Debug, Default)]
pub struct RtcParticipantInfo {
    /// member_id from m.rtc.member — pseudonymous, stable per join instance.
    pub participant_id: String,
    pub user_id: String,
    pub device_id: String,
    pub is_audio_muted: bool,
    pub is_video_muted: bool,
    pub is_screen_sharing: bool,
}

/// Application type for an RTC slot (MSC4196 "m.call").
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct RtcApplication {
    #[serde(rename = "type")]
    pub kind: String,
    /// Element Call compat: empty string for room-scope calls.
    #[serde(default)]
    pub call_id: String,
    /// MSC4196 §application: the `m.call.intent` field (note the dotted name)
    /// lives inside the application object.  matrix-js-sdk validates
    /// `application["m.call.intent"]` and Element Android rejects memberships
    /// where it is absent or invalid, causing our tracks to be ignored.
    /// Valid values: "voice", "video".
    #[serde(
        rename = "m.call.intent",
        default,
        skip_serializing_if = "String::is_empty"
    )]
    pub call_intent: String,
    /// "m.room" for room-level calls.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub scope: String,
}

impl RtcApplication {
    pub fn call(call_intent: &str) -> Self {
        Self {
            kind: "m.call".to_owned(),
            call_id: String::new(),
            call_intent: call_intent.to_owned(),
            scope: "m.room".to_owned(),
        }
    }
}

/// LiveKit focus descriptor, used in `focus_active` / `foci_preferred`.
///
/// Two shapes appear in the wild and all fields are optional so neither fails:
/// - `foci_preferred` entry: `{ type, livekit_service_url, livekit_alias }`
/// - `focus_active` (MSC3401): `{ type, focus_selection: "oldest_membership" }`
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct RtcFocus {
    #[serde(rename = "type", default)]
    pub kind: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub livekit_service_url: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub livekit_alias: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub focus_selection: String,
}

/// Per-participant identity claims inside m.rtc.member.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct RtcMemberDetails {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub claimed_device_id: String,
    #[serde(default)]
    pub claimed_user_id: String,
}

/// Transport descriptor inside m.rtc.member.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum RtcTransport {
    #[serde(rename = "livekit")]
    LiveKit {
        livekit_service_url: String,
        livekit_alias: String,
    },
}

// ---------------------------------------------------------------------------
// MSC3401 types (used by Element Web / matrix-js-sdk)
// ---------------------------------------------------------------------------
// CallMemberEventContent, CallMemberStateKey, LivekitFocus, ActiveLivekitFocus,
// Application, CallScope, Focus, etc. come from ruma::events::call::member.

/// `org.matrix.msc3401.call` state event — announces a call slot.
/// State key = call_id (empty string for the default room call).
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "org.matrix.msc3401.call", kind = State, state_key_type = String)]
pub struct Msc3401CallEventContent {
    #[serde(rename = "m.intent")]
    pub intent: String,
    #[serde(rename = "m.type")]
    pub kind: String,
}

// ---------------------------------------------------------------------------
// MSC4143 / MSC4195 types
// ---------------------------------------------------------------------------

/// `org.matrix.msc4143.rtc.slot` state event — opens/closes a call slot.
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4143.rtc.slot", kind = State, state_key_type = String)]
pub struct RtcSlotEventContent {
    pub application: RtcApplication,
}

/// `org.matrix.msc4143.rtc.member` state event — participant join/leave.
/// State key = member_id (same as sticky_key per MSC4354).
///
/// We send BOTH the MSC4195 fields (`rtc_transports`, `member`) AND the
/// current Element Call fields (`focus_active`, `foci_preferred`, `device_id`)
/// so that both spec revisions can parse our events.
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4143.rtc.member", kind = State, state_key_type = String)]
pub struct RtcMemberEventContent {
    // All fields are optional in deserialization: leave events intentionally
    // send only `disconnect_reason` with no join-state fields present.

    // ── MSC4195 fields ──────────────────────────────────────────────────────
    #[serde(default)]
    pub slot_id: String,
    #[serde(default)]
    pub application: RtcApplication,
    #[serde(default)]
    pub member: RtcMemberDetails,
    #[serde(default)]
    pub rtc_transports: Vec<RtcTransport>,
    #[serde(default)]
    pub sticky_key: String,
    #[serde(default)]
    pub versions: Vec<String>,

    // ── Element Call compat fields (matrix-js-sdk ≥ 34.x) ──────────────────
    // Element looks for `focus_active.type == "livekit"` to show the join UI.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub focus_active: Option<RtcFocus>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub foci_preferred: Vec<RtcFocus>,
    // Top-level device_id that Element uses to identify the participant.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub device_id: String,
}

/// Key material carried in `io.element.call.encryption_keys` to-device events.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RtcKeyContent {
    /// Base64-encoded 32-byte raw key material.
    pub key: String,
    /// Rotation index (0-255, wrapping).
    pub index: u8,
}

/// Sender identity fields in `io.element.call.encryption_keys`.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RtcKeyMember {
    /// Sender's Matrix device ID (used by receiver to target key delivery).
    pub claimed_device_id: String,
    /// Sender's LiveKit participant identity (`@user:server:session_id`).
    pub id: String,
}

/// Session descriptor in `io.element.call.encryption_keys`.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RtcKeySession {
    pub call_id: String,
    pub application: String,
    pub scope: String,
}

/// `io.element.call.encryption_keys` to-device event (Element Call wire format).
///
/// Element Call v0.7+ uses this event type for per-participant E2EE frame-key
/// exchange.  The event content was validated against Element Call 0.20.1.
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "io.element.call.encryption_keys", kind = ToDevice)]
pub struct RtcEncryptionKeyEventContent {
    pub keys: RtcKeyContent,
    pub room_id: String,
    pub member: RtcKeyMember,
    pub session: RtcKeySession,
    #[serde(default)]
    pub sent_ts: u64,
}

// ---------------------------------------------------------------------------
// Helpers for sending state events
// ---------------------------------------------------------------------------

/// MSC3417: true when `room`'s `m.room.create` `creation_content.type` is
/// the call-room type (`org.matrix.msc3417.call`). Read directly from
/// `m.room.create` rather than any mutable per-call state, since MSC3417
/// makes the (immutable) room-create type the authority over a possibly
/// drifted `m.intent` on the `org.matrix.msc3401.call` state event.
pub async fn is_call_room(room: &matrix_sdk::Room) -> bool {
    use matrix_sdk::deserialized_responses::SyncOrStrippedState;
    use matrix_sdk::ruma::events::{room::create::RoomCreateEventContent, SyncStateEvent};
    use matrix_sdk::ruma::room::RoomType;
    room.get_state_event_static::<RoomCreateEventContent>()
        .await
        .ok()
        .flatten()
        .and_then(|raw| raw.deserialize().ok())
        .is_some_and(|ev| {
            let SyncOrStrippedState::Sync(SyncStateEvent::Original(o)) = ev else {
                return false;
            };
            matches!(o.content.room_type, Some(RoomType::Call))
        })
}

/// Open the MSC3401 call slot event. State key = call_id (empty = default room call).
/// Idempotent — safe to re-send. `m.intent` follows the room's MSC3417
/// creation type so it never drifts out of sync with it: `"m.call"` for a
/// dedicated call room, `"m.room"` otherwise.
pub async fn send_msc3401_call_open(room: &matrix_sdk::Room, call_id: &str) -> anyhow::Result<()> {
    let intent = if is_call_room(room).await { "m.call" } else { "m.room" };
    let content = Msc3401CallEventContent {
        intent: intent.to_owned(),
        kind: "m.video".to_owned(),
    };
    room.send_state_event_for_key(call_id, content).await?;
    Ok(())
}

fn msc3401_state_key(
    user_id: &str,
    membership_id: &str,
) -> anyhow::Result<matrix_sdk::ruma::events::call::member::CallMemberStateKey> {
    let session_part = membership_id
        .strip_prefix(&format!("{user_id}:"))
        .unwrap_or(membership_id);
    let uid: matrix_sdk::ruma::OwnedUserId = user_id.parse().context("invalid user_id")?;
    Ok(matrix_sdk::ruma::events::call::member::CallMemberStateKey::new(
        uid,
        Some(format!("{session_part}_m.call")),
        true,
    ))
}

/// The join time of our own membership as receivers see it: the `created_ts`
/// carried in the event, else the `origin_server_ts` of the initial event.
/// Re-sent refreshes must copy this value or peers see us as re-joining
/// every refresh and never treat us as the oldest member. `None` until the
/// initial event has been echoed back through sync.
pub async fn own_msc3401_created_ts(
    room: &matrix_sdk::Room,
    user_id: &str,
    membership_id: &str,
) -> Option<matrix_sdk::ruma::MilliSecondsSinceUnixEpoch> {
    use matrix_sdk::deserialized_responses::SyncOrStrippedState;
    use matrix_sdk::ruma::events::{call::member::CallMemberEventContent, SyncStateEvent};

    let state_key = msc3401_state_key(user_id, membership_id).ok()?;
    let raw = room
        .get_state_event_static_for_key::<CallMemberEventContent, _>(&state_key)
        .await
        .ok()??;
    match raw.deserialize().ok()? {
        SyncOrStrippedState::Sync(SyncStateEvent::Original(o)) => o
            .content
            .memberships()
            .first()
            .and_then(|m| m.created_ts())
            .or(Some(o.origin_server_ts)),
        _ => None,
    }
}

/// Publish or refresh our MSC3401 call membership.
///
/// `membership_id` is the LiveKit participant identity (`{user_id}:{session_part}`),
/// decoded from the JWT `sub` claim.
///
/// State key format: `_{user_id}_{session_part}_m.call`
/// where `session_part` = membership_id stripped of the `{user_id}:` prefix.
///
/// IMPORTANT: The `device_id` field in the event **content** must be the real
/// Matrix device ID, not the session UUID.  Element Call reads `n.device_id` to
/// find which Matrix device to deliver E2EE frame keys to.  Using the session
/// UUID here means Element Call tries to target a non-existent device ID and
/// the key delivery silently fails.
pub async fn send_msc3401_member_join(
    room: &matrix_sdk::Room,
    call_id: &str,
    matrix_device_id: &str,
    membership_id: &str,
    service_url: &str,
    livekit_alias: &str,
    user_id: &str,
    audio_only: bool,
    created_ts: Option<matrix_sdk::ruma::MilliSecondsSinceUnixEpoch>,
) -> anyhow::Result<()> {
    use matrix_sdk::ruma::{
        api::client::state::send_state_event,
        events::{AnyStateEventContent, StateEventType},
    };

    // The state key uses the session part (UUID) so that each LiveKit session
    // gets a unique state event even if multiple sessions share the same device.
    let state_key = msc3401_state_key(user_id, membership_id)?;
    let body = msc3401_member_body(
        call_id,
        matrix_device_id,
        membership_id,
        service_url,
        livekit_alias,
        audio_only,
        created_ts,
    )?;

    let raw_body = serde_json::value::to_raw_value(&body)
        .map(|v| matrix_sdk::ruma::serde::Raw::<AnyStateEventContent>::from_json(v))
        .map_err(|e| anyhow::anyhow!("re-serialize with membershipID: {e}"))?;

    let request = send_state_event::v3::Request::new_raw(
        room.room_id().to_owned(),
        StateEventType::from("org.matrix.msc3401.call.member"),
        state_key.as_ref().to_owned(),
        raw_body,
    );
    room.client().send(request).await?;
    Ok(())
}

/// The content of our `org.matrix.msc3401.call.member` join event.
fn msc3401_member_body(
    call_id: &str,
    matrix_device_id: &str,
    membership_id: &str,
    service_url: &str,
    livekit_alias: &str,
    audio_only: bool,
    created_ts: Option<matrix_sdk::ruma::MilliSecondsSinceUnixEpoch>,
) -> anyhow::Result<serde_json::Value> {
    use matrix_sdk::ruma::{
        events::{
            call::member::{
                ActiveFocus, ActiveLivekitFocus, Application, CallApplicationContent,
                CallMemberEventContent, CallScope, Focus, LivekitFocus,
            },
            rtc::notification::CallIntent,
        },
        MilliSecondsSinceUnixEpoch,
    };

    let mut app_content = CallApplicationContent::new(call_id.to_owned(), CallScope::Room);
    app_content.call_intent = Some(if audio_only {
        CallIntent::Audio
    } else {
        CallIntent::Video
    });

    let content = CallMemberEventContent::new(
        Application::Call(app_content),
        matrix_device_id.into(), // Real Matrix device ID so EC can target key delivery
        ActiveFocus::Livekit(ActiveLivekitFocus::new()),
        vec![Focus::Livekit(LivekitFocus::new(
            livekit_alias.to_owned(),
            service_url.to_owned(),
        ))],
        created_ts,
        // The membership is valid for 4h after joining; extend it on every
        // refresh so a long call does not expire while created_ts stays fixed.
        created_ts.map(|c| {
            let elapsed_ms = u64::from(MilliSecondsSinceUnixEpoch::now().0)
                .saturating_sub(u64::from(c.0));
            std::time::Duration::from_millis(elapsed_ms) + std::time::Duration::from_secs(4 * 3600)
        }),
    );

    // ruma 0.34's SessionMembershipData doesn't model `membershipID` (it exists
    // only in LegacyMembershipData).  Element Android uses this field to map the
    // LiveKit participant identity back to the Matrix membership; without it our
    // tracks are subscribed at the SFU level but never rendered.
    // Serialize via ruma then inject the field before sending.
    let mut body: serde_json::Value =
        serde_json::to_value(&content).context("serialize CallMemberEventContent")?;
    body["membershipID"] = serde_json::Value::String(membership_id.to_owned());
    // multi_sfu (Element Call's mode): we publish on our own homeserver's SFU
    // (foci_preferred[0]) and subscribe to every other member's SFU. ruma can
    // only build `oldest_membership` here, so override the value.
    body["focus_active"]["focus_selection"] =
        serde_json::Value::String(super::members::SELECTION_MULTI_SFU.to_owned());
    Ok(body)
}

#[cfg(test)]
mod member_body_tests {
    use super::*;
    use matrix_sdk::ruma::{MilliSecondsSinceUnixEpoch, UInt};

    #[test]
    fn join_advertises_multi_sfu_and_our_local_sfu_first() {
        let body = msc3401_member_body("", "DEV", "@a:x.org:DEV", "https://jwt.x.org", "!r:x.org", false, None)
            .unwrap();
        assert_eq!(body["focus_active"]["type"], "livekit");
        assert_eq!(body["focus_active"]["focus_selection"], "multi_sfu");
        assert_eq!(body["foci_preferred"][0]["livekit_service_url"], "https://jwt.x.org");
        assert_eq!(body["foci_preferred"][0]["livekit_alias"], "!r:x.org");
        assert_eq!(body["device_id"], "DEV");
        assert_eq!(body["membershipID"], "@a:x.org:DEV");
        assert!(body.get("created_ts").is_none(), "initial join carries no created_ts");
    }

    #[test]
    fn refresh_keeps_created_ts_and_extends_expires() {
        let created = MilliSecondsSinceUnixEpoch(
            MilliSecondsSinceUnixEpoch::now().0 - UInt::from(60_000u32),
        );
        let body = msc3401_member_body("", "DEV", "@a:x.org:DEV", "https://jwt.x.org", "!r", true, Some(created))
            .unwrap();
        assert_eq!(body["created_ts"], serde_json::to_value(created).unwrap());
        // 4h plus the ~60 s already elapsed
        let expires = body["expires"].as_u64().unwrap();
        assert!(expires >= 4 * 3600 * 1000 + 59_000 && expires < 4 * 3600 * 1000 + 70_000, "{expires}");
    }
}

/// Leave the MSC3401 call. Sends ruma's Empty content to the per-session state key.
/// `membership_id` must be the same LiveKit identity string (`{user_id}:{member_id}`)
/// used in `send_msc3401_member_join` so the state key matches.
pub async fn send_msc3401_member_leave(
    room: &matrix_sdk::Room,
    user_id: &str,
    membership_id: &str,
) -> anyhow::Result<()> {
    use matrix_sdk::ruma::events::call::member::CallMemberEventContent;

    let state_key = msc3401_state_key(user_id, membership_id)?;
    let content = CallMemberEventContent::new_empty(None);
    room.send_state_event_for_key(&state_key, content).await?;
    Ok(())
}

/// Outcome of restarting a delayed event.
pub enum DelayedRestart {
    Ok,
    /// The homeserver no longer knows the delayed event (already sent, or
    /// cancelled by a newer state event for the same key); schedule a new one.
    Gone,
    /// The homeserver does not support delayed events.
    Unsupported,
}

/// Schedule the MSC3401 leave as an MSC4140 delayed event (dead-man's switch):
/// if we crash or lose connectivity, the homeserver sends the leave for us.
/// Returns the `delay_id`, or `None` when delayed events are unavailable.
pub async fn schedule_delayed_msc3401_leave(
    room: &matrix_sdk::Room,
    user_id: &str,
    membership_id: &str,
    delay: std::time::Duration,
) -> Option<String> {
    use matrix_sdk::ruma::{
        api::client::delayed_events::{delayed_state_event, DelayParameters},
        events::{
            call::member::CallMemberEventContent, AnyStateEventContent, StateEventType,
        },
    };

    let state_key = msc3401_state_key(user_id, membership_id).ok()?;
    let body = serde_json::to_value(CallMemberEventContent::new_empty(None)).ok()?;
    let raw = matrix_sdk::ruma::serde::Raw::<AnyStateEventContent>::from_json(
        serde_json::value::to_raw_value(&body).ok()?,
    );
    let request = delayed_state_event::unstable::Request::new_raw(
        room.room_id().to_owned(),
        state_key.as_ref().to_owned(),
        StateEventType::from("org.matrix.msc3401.call.member"),
        DelayParameters::Timeout { timeout: delay },
        raw,
    );
    match room.client().send(request).await {
        Ok(resp) => Some(resp.delay_id),
        Err(e) => {
            tracing::info!("rtc: delayed leave not scheduled ({e}); continuing without it");
            None
        }
    }
}

/// Send an MSC4140 update action for a delayed event. Tries the current
/// endpoint (`.../{delay_id}/{action}`) and falls back to the older one
/// (action in the body) when the homeserver doesn't know the former.
async fn update_delayed_event(
    client: &matrix_sdk::Client,
    delay_id: &str,
    action: matrix_sdk::ruma::api::client::delayed_events::update_delayed_event::UpdateAction,
) -> Result<(), matrix_sdk::Error> {
    use matrix_sdk::ruma::api::{
        client::delayed_events::update_delayed_event::{unstable_v1, unstable_v2},
        error::ErrorKind,
    };

    let v2 = unstable_v2::Request::new(delay_id.to_owned(), action.clone());
    match client.send(v2).await {
        Ok(_) => Ok(()),
        Err(e) if matches!(e.client_api_error_kind(), Some(ErrorKind::Unrecognized)) => {
            let v1 = unstable_v1::Request::new(delay_id.to_owned(), action);
            client.send(v1).await.map(|_| ()).map_err(Into::into)
        }
        Err(e) => Err(e.into()),
    }
}

/// Push a scheduled delayed event further into the future.
pub async fn restart_delayed_event(client: &matrix_sdk::Client, delay_id: &str) -> DelayedRestart {
    use matrix_sdk::ruma::api::{
        client::delayed_events::update_delayed_event::UpdateAction, error::ErrorKind,
    };

    match update_delayed_event(client, delay_id, UpdateAction::Restart).await {
        Ok(()) => DelayedRestart::Ok,
        Err(e) => match e.client_api_error_kind() {
            Some(ErrorKind::NotFound) => DelayedRestart::Gone,
            Some(ErrorKind::Unrecognized) => DelayedRestart::Unsupported,
            _ => {
                tracing::warn!("rtc: restarting delayed leave failed: {e}");
                DelayedRestart::Ok
            }
        },
    }
}

/// Cancel a scheduled delayed event so it is never sent (best effort).
pub async fn cancel_delayed_event(client: &matrix_sdk::Client, delay_id: &str) {
    use matrix_sdk::ruma::api::client::delayed_events::update_delayed_event::UpdateAction;
    let _ = update_delayed_event(client, delay_id, UpdateAction::Cancel).await;
}

/// Open or refresh the call slot. State key = slot_id.
/// Per MSC4143 §slot: the presence of this event tells other clients that
/// the slot is active. Idempotent — safe to re-send.
pub async fn send_rtc_slot(
    room: &matrix_sdk::Room,
    slot_id: &str,
    call_intent: &str,
) -> anyhow::Result<()> {
    let content = RtcSlotEventContent {
        application: RtcApplication::call(call_intent),
    };
    room.send_state_event_for_key(slot_id, content).await?;
    Ok(())
}

/// Send an m.rtc.member join event. State key = member_id (= sticky_key).
pub async fn send_rtc_member_join(
    room: &matrix_sdk::Room,
    slot_id: &str,
    member_id: &str,
    service_url: &str,
    livekit_alias: &str,
    device_id: &str,
    user_id: &str,
    call_intent: &str,
) -> anyhow::Result<()> {
    let focus = RtcFocus {
        kind: "livekit".to_owned(),
        livekit_service_url: service_url.to_owned(),
        ..Default::default()
    };
    let content = RtcMemberEventContent {
        // MSC4195 fields
        slot_id: slot_id.to_owned(),
        application: RtcApplication::call(call_intent),
        member: RtcMemberDetails {
            id: member_id.to_owned(),
            claimed_device_id: device_id.to_owned(),
            claimed_user_id: user_id.to_owned(),
        },
        rtc_transports: vec![RtcTransport::LiveKit {
            livekit_service_url: service_url.to_owned(),
            livekit_alias: livekit_alias.to_owned(),
        }],
        sticky_key: member_id.to_owned(),
        versions: vec!["v0".to_owned()],
        // Element Call compat fields
        // Include livekit_alias so receiving clients can identify which room
        // we are in (matching their own livekit_alias).
        focus_active: Some(RtcFocus {
            kind: focus.kind.clone(),
            livekit_service_url: focus.livekit_service_url.clone(),
            livekit_alias: livekit_alias.to_owned(),
            ..Default::default()
        }),
        foci_preferred: vec![RtcFocus {
            kind: focus.kind.clone(),
            livekit_service_url: focus.livekit_service_url.clone(),
            livekit_alias: livekit_alias.to_owned(),
            ..Default::default()
        }],
        device_id: device_id.to_owned(),
    };
    // matrix-sdk 0.18: send_state_event_for_key(&state_key, content)
    room.send_state_event_for_key(member_id, content).await?;
    Ok(())
}

/// Send an m.rtc.member disconnect event (minimal content that does not satisfy
/// the connected-state schema, per MSC4143 §leave).
pub async fn send_rtc_member_leave(
    room: &matrix_sdk::Room,
    member_id: &str,
    reason: &str,
) -> anyhow::Result<()> {
    use matrix_sdk::ruma::{
        api::client::state::send_state_event,
        events::{AnyStateEventContent, StateEventType},
    };

    let body = serde_json::json!({
        "disconnect_reason": {
            "class": "user",
            "reason": reason,
        }
    });
    let raw_body = serde_json::value::to_raw_value(&body)
        .map(|v| matrix_sdk::ruma::serde::Raw::<AnyStateEventContent>::from_json(v))
        .map_err(|e| anyhow::anyhow!("serialize leave body: {e}"))?;
    let event_type = StateEventType::from("org.matrix.msc4143.rtc.member");
    let room_id = room.room_id().to_owned();
    let request =
        send_state_event::v3::Request::new_raw(room_id, event_type, member_id.to_owned(), raw_body);
    room.client().send(request).await?;
    Ok(())
}

// ---------------------------------------------------------------------------
// MSC4075 — ring notification event types and helpers
// ---------------------------------------------------------------------------

/// Minimal event type for the unstable `org.matrix.msc4075.rtc.notification` prefix.
/// Used only for receiving via add_event_handler; the stable ruma type handles m.rtc.notification.
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "org.matrix.msc4075.rtc.notification", kind = MessageLike)]
pub struct Msc4075RtcNotificationEventContent {
    pub notification_type: String,
    pub sender_ts: u64,
    pub lifetime: u64,
    #[serde(
        rename = "m.call.intent",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub call_intent: Option<String>,
}

/// Send MSC4075 ring notification when we start a call.
/// Uses the unstable event type prefix for Element Web compatibility.
/// Non-fatal: callers should log and continue on error.
pub async fn send_rtc_notification(
    room: &matrix_sdk::Room,
    call_intent: &str,
    device_id: &str,
    _user_id: &str,
) -> anyhow::Result<()> {
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;

    let mut body = serde_json::json!({
        "notification_type": "ring",
        "sender_ts": now_ms,
        "lifetime": 30_000u64,
        "device_id": device_id,
        "m.mentions": { "room": true },
    });
    if !call_intent.is_empty() {
        body["m.call.intent"] = serde_json::Value::String(call_intent.to_owned());
    }

    // Use room.send_raw() so the event is automatically encrypted when the
    // room is encrypted. The previous room.client().send(raw_http_request)
    // bypass sent the content in cleartext regardless of room encryption state.
    room.send_raw("org.matrix.msc4075.rtc.notification", body)
        .await
        .map_err(|e| anyhow::anyhow!("send rtc notification: {e}"))?;
    Ok(())
}

/// Send MSC4075 m.call.ring.ack to-device message to the caller.
/// Called when we display IncomingCallBanner to acknowledge ringing.
pub async fn send_ring_ack(
    client: &matrix_sdk::Client,
    caller_user_id: &str,
    caller_device_id: &str,
    notification_event_id: &str,
) -> anyhow::Result<()> {
    use matrix_sdk::ruma::{
        api::client::to_device::send_event_to_device,
        events::{AnyToDeviceEventContent, ToDeviceEventType},
        to_device::DeviceIdOrAllDevices,
        OwnedDeviceId, OwnedUserId, TransactionId,
    };
    use std::collections::BTreeMap;

    let our_device_id = client
        .device_id()
        .ok_or_else(|| anyhow::anyhow!("no device_id for ring_ack"))?
        .to_string();

    let body = serde_json::json!({
        "device_id": our_device_id,
        "m.relates_to": {
            "rel_type": "m.reference",
            "event_id": notification_event_id
        }
    });
    let raw = serde_json::value::to_raw_value(&body)
        .map(|v| matrix_sdk::ruma::serde::Raw::<AnyToDeviceEventContent>::from_json(v))
        .map_err(|e| anyhow::anyhow!("serialize ring_ack: {e}"))?;

    let uid: OwnedUserId = caller_user_id
        .parse()
        .context("invalid caller_user_id for ring_ack")?;
    let did: OwnedDeviceId = caller_device_id.into();

    let mut per_user: BTreeMap<OwnedUserId, BTreeMap<DeviceIdOrAllDevices, _>> = BTreeMap::new();
    let mut per_device = BTreeMap::new();
    per_device.insert(DeviceIdOrAllDevices::DeviceId(did), raw);
    per_user.insert(uid, per_device);

    let event_type = ToDeviceEventType::from("org.matrix.msc4075.call.ring.ack");
    let request =
        send_event_to_device::v3::Request::new_raw(event_type, TransactionId::new(), per_user);
    client
        .send(request)
        .await
        .map_err(|e| anyhow::anyhow!("send ring_ack: {e}"))?;
    Ok(())
}
