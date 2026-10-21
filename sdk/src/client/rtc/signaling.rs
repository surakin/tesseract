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

/// `org.matrix.msc3401.call.member` as received, kept as raw JSON. ruma's typed
/// `CallMemberEventContent` refuses events that omit fields it requires (for
/// example `livekit_alias`), and a handler typed on it is then never called;
/// this one always matches. Read with
/// [`super::members::members_from_event_json`].
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "org.matrix.msc3401.call.member", kind = State, state_key_type = String)]
pub struct CallMemberRawEventContent {
    #[serde(default, flatten)]
    pub fields: serde_json::Map<String, serde_json::Value>,
}

/// `io.element.call.encryption_keys` as received: the legacy and the MSC4143
/// shapes both parse (see [`super::model::MediaKeyContent`]). Receive-only;
/// we send [`RtcEncryptionKeyEventContent`]'s shape.
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "io.element.call.encryption_keys", kind = ToDevice)]
pub struct MediaKeyUnstableEventContent {
    #[serde(flatten)]
    pub inner: super::model::MediaKeyContent,
}

/// The stable name of the same event, `m.rtc.encryption_key` (MSC4143).
#[derive(Clone, Debug, Serialize, Deserialize, EventContent)]
#[ruma_event(type = "m.rtc.encryption_key", kind = ToDevice)]
pub struct MediaKeyStableEventContent {
    #[serde(flatten)]
    pub inner: super::model::MediaKeyContent,
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
    let intent = if is_call_room(room).await {
        "m.call"
    } else {
        "m.room"
    };
    let content = Msc3401CallEventContent {
        intent: intent.to_owned(),
        kind: "m.video".to_owned(),
    };
    room.send_state_event_for_key(call_id, content).await?;
    Ok(())
}

/// True when the room already carries the `org.matrix.msc3401.call` slot
/// event for `call_id`.
pub async fn call_slot_exists(room: &matrix_sdk::Room, call_id: &str) -> bool {
    use matrix_sdk::ruma::events::StateEventType;
    room.get_state_event(StateEventType::from("org.matrix.msc3401.call"), call_id)
        .await
        .ok()
        .flatten()
        .is_some_and(|_| true)
}

/// Whether joining must publish the `org.matrix.msc3401.call` slot event.
/// That event needs the "Start Element Call calls" permission (moderator by
/// default), unlike the member event, so a joiner of a call someone else
/// started must not send it.
pub fn should_open_call_slot(slot_exists: bool, others_in_call: bool) -> bool {
    !slot_exists && !others_in_call
}

/// Whether joining should ring the room (MSC4075). Only the first joiner
/// rings, and never in an MSC3417 call room: there, joining is dropping in,
/// not calling the other members.
pub fn should_ring_room(others_in_call: bool, is_call_room: bool) -> bool {
    !others_in_call && !is_call_room
}

/// Like [`send_msc3401_call_open`] but turns a `M_FORBIDDEN` into a
/// message saying what is missing.
pub async fn start_msc3401_call_slot(room: &matrix_sdk::Room, call_id: &str) -> anyhow::Result<()> {
    match send_msc3401_call_open(room, call_id).await {
        Err(e)
            if e.downcast_ref::<matrix_sdk::Error>().is_some_and(|me| {
                matches!(
                    me.client_api_error_kind(),
                    Some(matrix_sdk::ruma::api::error::ErrorKind::Forbidden)
                )
            }) =>
        {
            Err(anyhow::anyhow!(
                "you don't have permission to start calls in this room ({e})"
            ))
        }
        other => other,
    }
}

fn msc3401_state_key(
    user_id: &str,
    membership_id: &str,
) -> anyhow::Result<matrix_sdk::ruma::events::call::member::CallMemberStateKey> {
    let session_part = membership_id
        .strip_prefix(&format!("{user_id}:"))
        .unwrap_or(membership_id);
    let uid: matrix_sdk::ruma::OwnedUserId = user_id.parse().context("invalid user_id")?;
    Ok(
        matrix_sdk::ruma::events::call::member::CallMemberStateKey::new(
            uid,
            Some(format!("{session_part}_m.call")),
            true,
        ),
    )
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
            let elapsed_ms =
                u64::from(MilliSecondsSinceUnixEpoch::now().0).saturating_sub(u64::from(c.0));
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
        let body = msc3401_member_body(
            "",
            "DEV",
            "@a:x.org:DEV",
            "https://jwt.x.org",
            "!r:x.org",
            false,
            None,
        )
        .unwrap();
        assert_eq!(body["focus_active"]["type"], "livekit");
        assert_eq!(body["focus_active"]["focus_selection"], "multi_sfu");
        assert_eq!(
            body["foci_preferred"][0]["livekit_service_url"],
            "https://jwt.x.org"
        );
        assert_eq!(body["foci_preferred"][0]["livekit_alias"], "!r:x.org");
        assert_eq!(body["device_id"], "DEV");
        assert_eq!(body["membershipID"], "@a:x.org:DEV");
        assert!(
            body.get("created_ts").is_none(),
            "initial join carries no created_ts"
        );
    }

    #[test]
    fn refresh_keeps_created_ts_and_extends_expires() {
        let created =
            MilliSecondsSinceUnixEpoch(MilliSecondsSinceUnixEpoch::now().0 - UInt::from(60_000u32));
        let body = msc3401_member_body(
            "",
            "DEV",
            "@a:x.org:DEV",
            "https://jwt.x.org",
            "!r",
            true,
            Some(created),
        )
        .unwrap();
        assert_eq!(body["created_ts"], serde_json::to_value(created).unwrap());
        // 4h plus the ~60 s already elapsed
        let expires = body["expires"].as_u64().unwrap();
        assert!(
            expires >= 4 * 3600 * 1000 + 59_000 && expires < 4 * 3600 * 1000 + 70_000,
            "{expires}"
        );
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
        events::{call::member::CallMemberEventContent, AnyStateEventContent, StateEventType},
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

/// Body of the MSC4075 notification that announces a new call.
///
/// Only a direct message rings the peer, with an `@room` mention. A group room
/// gets a plain `notification` with no mention, so a large room is not rung
/// (and mentioned) for every member each time someone starts a call.
pub fn rtc_notification_body(
    is_direct: bool,
    now_ms: u64,
    call_intent: &str,
    device_id: &str,
) -> serde_json::Value {
    let (notification_type, mentions) = if is_direct {
        ("ring", serde_json::json!({ "room": true }))
    } else {
        ("notification", serde_json::json!({}))
    };
    let mut body = serde_json::json!({
        "notification_type": notification_type,
        "sender_ts": now_ms,
        "lifetime": 30_000u64,
        "device_id": device_id,
        "m.mentions": mentions,
    });
    if !call_intent.is_empty() {
        body["m.call.intent"] = serde_json::Value::String(call_intent.to_owned());
    }
    body
}

/// Send the MSC4075 notification that announces a call we are starting.
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
    let is_direct = room.is_direct().await.unwrap_or(false);
    let body = rtc_notification_body(is_direct, now_ms, call_intent, device_id);

    // Use room.send_raw() so the event is automatically encrypted when the
    // room is encrypted. The previous room.client().send(raw_http_request)
    // bypass sent the content in cleartext regardless of room encryption state.
    room.send_raw("org.matrix.msc4075.rtc.notification", body)
        .await
        .map_err(|e| anyhow::anyhow!("send rtc notification: {e}"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn rtc_notification_rings_only_a_direct_message() {
        let dm = super::rtc_notification_body(true, 1, "audio", "DEV");
        assert_eq!(dm["notification_type"], "ring");
        assert_eq!(dm["m.mentions"]["room"], true);
        assert_eq!(dm["m.call.intent"], "audio");

        let group = super::rtc_notification_body(false, 1, "", "DEV");
        assert_eq!(group["notification_type"], "notification");
        assert!(group["m.mentions"]
            .as_object()
            .is_some_and(|m| m.is_empty()));
        assert!(group.get("m.call.intent").is_none());
    }

    use super::*;
    use serde_json::json;

    #[test]
    fn call_slot_opened_only_when_starting_a_call() {
        assert!(should_open_call_slot(false, false));
        assert!(!should_open_call_slot(true, false));
        assert!(!should_open_call_slot(false, true));
        assert!(!should_open_call_slot(true, true));
    }

    #[test]
    fn ring_only_first_joiner_and_never_in_call_rooms() {
        assert!(should_ring_room(false, false));
        assert!(!should_ring_room(true, false));
        assert!(!should_ring_room(false, true));
        assert!(!should_ring_room(true, true));
    }

    #[test]
    fn state_key_strips_user_prefix_from_membership_id() {
        let k = msc3401_state_key("@a:x.org", "@a:x.org:SESSION").unwrap();
        assert_eq!(k.as_ref(), "_@a:x.org_SESSION_m.call");
    }

    #[test]
    fn state_key_keeps_membership_id_without_matching_prefix() {
        let k = msc3401_state_key("@a:x.org", "@b:x.org:SESSION").unwrap();
        assert_eq!(k.as_ref(), "_@a:x.org_@b:x.org:SESSION_m.call");
        let k = msc3401_state_key("@a:x.org", "plain").unwrap();
        assert_eq!(k.as_ref(), "_@a:x.org_plain_m.call");
    }

    #[test]
    fn state_key_rejects_invalid_user_id() {
        assert!(msc3401_state_key("not a user id", "x").is_err());
        assert!(msc3401_state_key("", "x").is_err());
    }

    #[test]
    fn rtc_application_call_serializes_all_fields() {
        let v = serde_json::to_value(RtcApplication::call("video")).unwrap();
        assert_eq!(
            v,
            json!({"type": "m.call", "call_id": "", "m.call.intent": "video", "scope": "m.room"})
        );
    }

    #[test]
    fn rtc_application_omits_empty_intent_and_scope() {
        let app = RtcApplication {
            kind: "m.call".into(),
            ..Default::default()
        };
        assert_eq!(
            serde_json::to_value(&app).unwrap(),
            json!({"type": "m.call", "call_id": ""})
        );
    }

    #[test]
    fn rtc_application_deserializes_with_missing_optionals() {
        let app: RtcApplication = serde_json::from_value(json!({"type": "m.call"})).unwrap();
        assert_eq!(app.kind, "m.call");
        assert!(app.call_id.is_empty() && app.call_intent.is_empty() && app.scope.is_empty());
        let app: RtcApplication =
            serde_json::from_value(json!({"type": "m.call", "m.call.intent": "voice"})).unwrap();
        assert_eq!(app.call_intent, "voice");
    }

    #[test]
    fn rtc_focus_serializes_only_populated_fields() {
        let f = RtcFocus {
            kind: "livekit".into(),
            livekit_service_url: "https://lk".into(),
            ..Default::default()
        };
        assert_eq!(
            serde_json::to_value(&f).unwrap(),
            json!({"type": "livekit", "livekit_service_url": "https://lk"})
        );
    }

    #[test]
    fn rtc_focus_parses_both_wild_shapes() {
        let active: RtcFocus = serde_json::from_value(
            json!({"type": "livekit", "focus_selection": "oldest_membership"}),
        )
        .unwrap();
        assert_eq!(active.focus_selection, "oldest_membership");
        assert!(active.livekit_service_url.is_empty());
        let preferred: RtcFocus = serde_json::from_value(
            json!({"type": "livekit", "livekit_service_url": "u", "livekit_alias": "a", "extra": 1}),
        )
        .unwrap();
        assert_eq!(
            (
                preferred.livekit_service_url.as_str(),
                preferred.livekit_alias.as_str()
            ),
            ("u", "a")
        );
        // Entirely empty object is tolerated.
        let empty: RtcFocus = serde_json::from_value(json!({})).unwrap();
        assert!(empty.kind.is_empty());
    }

    #[test]
    fn encryption_key_event_parses_element_call_payload() {
        let c: RtcEncryptionKeyEventContent = serde_json::from_value(json!({
            "keys": {"key": "AAAA", "index": 7},
            "room_id": "!r:x",
            "member": {"claimed_device_id": "DEV", "id": "@a:x:DEV"},
            "session": {"call_id": "", "application": "m.call", "scope": "m.room"},
            "sent_ts": 123
        }))
        .unwrap();
        assert_eq!(c.keys.index, 7);
        assert_eq!(c.member.claimed_device_id, "DEV");
        assert_eq!(c.session.application, "m.call");
        assert_eq!(c.sent_ts, 123);
    }

    #[test]
    fn encryption_key_event_defaults_sent_ts_and_rejects_bad_index() {
        let base = |index: serde_json::Value| {
            json!({
                "keys": {"key": "AAAA", "index": index},
                "room_id": "!r:x",
                "member": {"claimed_device_id": "D", "id": "i"},
                "session": {"call_id": "", "application": "m.call", "scope": "m.room"},
            })
        };
        let c: RtcEncryptionKeyEventContent = serde_json::from_value(base(json!(0))).unwrap();
        assert_eq!(c.sent_ts, 0);
        // index is a u8: 256 and negatives do not fit.
        assert!(serde_json::from_value::<RtcEncryptionKeyEventContent>(base(json!(256))).is_err());
        assert!(serde_json::from_value::<RtcEncryptionKeyEventContent>(base(json!(-1))).is_err());
    }

    #[test]
    fn msc4075_notification_intent_is_optional() {
        let c: Msc4075RtcNotificationEventContent = serde_json::from_value(json!({
            "notification_type": "ring", "sender_ts": 5, "lifetime": 30000
        }))
        .unwrap();
        assert!(c.call_intent.is_none());
        assert!(serde_json::to_value(&c)
            .unwrap()
            .get("m.call.intent")
            .is_none());
        let c: Msc4075RtcNotificationEventContent = serde_json::from_value(json!({
            "notification_type": "ring", "sender_ts": 5, "lifetime": 1, "m.call.intent": "audio"
        }))
        .unwrap();
        assert_eq!(c.call_intent.as_deref(), Some("audio"));
    }

    #[test]
    fn member_body_intent_follows_audio_only() {
        let video = msc3401_member_body("", "D", "m", "u", "a", false, None).unwrap();
        let audio = msc3401_member_body("", "D", "m", "u", "a", true, None).unwrap();
        assert_eq!(video["m.call.intent"], "video");
        assert_eq!(audio["m.call.intent"], "audio");
    }

    #[test]
    fn member_body_carries_call_id_and_room_scope() {
        let b = msc3401_member_body("call-1", "D", "m", "u", "a", false, None).unwrap();
        assert_eq!(b["application"], "m.call");
        assert_eq!(b["call_id"], "call-1");
        assert_eq!(b["scope"], "m.room");
    }
}
