//! Push rules / pushers / per-room notification mode.
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::{err, ok, ClientFfi};

use crate::ffi::OpResult;

#[cfg(not(test))]
use matrix_sdk::{
    ruma::{
        api::client::push::{PusherIds, PusherInit, PusherKind},
        events::AnySyncTimelineEvent,
        push::{HttpPusherData, PushFormat},
        serde::Raw,
        OwnedRoomId,
    },
    Client, Room,
};

// ---------------------------------------------------------------------------
// Push-rule evaluation helpers (used by notification handlers in sync.rs)
// ---------------------------------------------------------------------------

/// Builds a minimal raw Matrix event JSON envelope suitable for passing to
/// `Ruleset::get_actions`. Uses `serde_json::to_string` for string fields so
/// control characters (\n, \r, \t, …) are escaped correctly.
#[cfg(not(test))]
pub(super) fn build_push_rule_json(
    room_id: &str,
    event_id: &str,
    sender: &str,
    body: &str,
    msg_type: &str,
    timestamp: u64,
) -> String {
    let msg_type = if msg_type.is_empty() {
        "m.text"
    } else {
        msg_type
    };
    let event_id = if event_id.is_empty() {
        "$unknown"
    } else {
        event_id
    };
    // Every interpolated string is serialized through serde_json so a server
    // that returns an event_id / msg_type containing `"` or `\` cannot break
    // the envelope (or inject extra JSON keys into the push-rule evaluation).
    serde_json::json!({
        "type": "m.room.message",
        "event_id": event_id,
        "sender": sender,
        "room_id": room_id,
        "origin_server_ts": timestamp,
        "content": { "msgtype": msg_type, "body": body },
    })
    .to_string()
}

/// Evaluates Matrix push rules for `source_json` (a synthetic raw event
/// envelope) and returns `(should_notify, is_mention)`.
/// Returns `(false, false)` on any error — callers must not rely on errors being
/// propagated.
///
/// Delegates to `Room::push_context()` so we inherit matrix-sdk's full
/// `PushConditionRoomCtx` build (including power levels and thread
/// subscriptions). The previous hand-rolled implementation explicitly skipped
/// power-level data, which broke `sender_notification_permission` conditions.
#[cfg(not(test))]
pub(super) async fn evaluate_push_rules(
    _client: &Client,
    room: &Room,
    source_json: &str,
) -> (bool, bool) {
    let Ok(raw_value) = serde_json::from_str::<Box<serde_json::value::RawValue>>(source_json)
    else {
        return (false, false);
    };
    let raw_event = Raw::<AnySyncTimelineEvent>::from_json(raw_value);

    let Ok(Some(ctx)) = room.push_context().await else {
        return (false, false);
    };
    let actions = ctx.for_event(&raw_event).await;
    let should_notify = actions.iter().any(|a| a.should_notify());
    let is_mention = should_notify && actions.iter().any(|a| a.is_highlight());
    (should_notify, is_mention)
}

// ---------------------------------------------------------------------------
// FFI impls
// ---------------------------------------------------------------------------

#[cfg(not(test))]
impl ClientFfi {
    // Hint that a push notification arrived for a room. Subscribes the server
    // to that room (plus all already-open rooms) so the next sliding-sync
    // cycle delivers fresh state before the regular sync loop catches up.
    // The hint is ephemeral: the next subscribe_room/unsubscribe_room call
    // will re-sync the subscription set from self.timelines.
    pub fn hint_push_room(&mut self, room_id: &str) -> OpResult {
        let Some(svc) = self.sync_service.clone() else {
            return err("sync not started");
        };
        let push_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let mut ids: Vec<OwnedRoomId> = self.timelines.read().unwrap().keys().cloned().collect();
        if !ids.contains(&push_id) {
            ids.push(push_id);
        }
        self.rt.spawn(async move {
            let refs: Vec<&matrix_sdk::ruma::RoomId> =
                ids.iter().map(OwnedRoomId::as_ref).collect();
            svc.room_list_service().subscribe_to_rooms(&refs).await;
        });
        ok("")
    }
}

#[cfg(not(test))]
impl ClientFfi {
    pub fn get_room_notification_mode(&self, room_id: &str) -> String {
        use matrix_sdk::notification_settings::RoomNotificationMode;
        use matrix_sdk::ruma::RoomId;

        let Some(client) = self.client.clone() else {
            return "default".to_owned();
        };
        let room_id = room_id.to_owned();

        self.rt.block_on(async move {
            let Ok(rid) = RoomId::parse(&room_id) else {
                return "default".to_owned();
            };
            let settings = client.notification_settings().await;
            // `None` means no user-defined rule for this room → it follows the
            // account/server default, which we surface as "default".
            match settings.get_user_defined_room_notification_mode(&rid).await {
                Some(RoomNotificationMode::AllMessages) => "all".to_owned(),
                Some(RoomNotificationMode::MentionsAndKeywordsOnly) => "mentions".to_owned(),
                Some(RoomNotificationMode::Mute) => "off".to_owned(),
                None => "default".to_owned(),
            }
        })
    }

    pub fn set_room_notification_mode(&mut self, room_id: &str, mode: &str) {
        use matrix_sdk::notification_settings::RoomNotificationMode;
        use matrix_sdk::ruma::RoomId;

        let Some(client) = self.client.clone() else {
            return;
        };
        let mode    = mode.to_owned();
        let room_id = room_id.to_owned();

        self.rt.block_on(async move {
            let Ok(rid) = RoomId::parse(&room_id) else { return; };
            let settings = client.notification_settings().await;

            let target = match mode.as_str() {
                "all" => RoomNotificationMode::AllMessages,
                "mentions" => RoomNotificationMode::MentionsAndKeywordsOnly,
                "off" => RoomNotificationMode::Mute,
                // "default" (and any unrecognised value) clears the user-defined
                // rules so the room falls back to the account/server default.
                _ => {
                    let _ = settings.delete_user_defined_room_rules(&rid).await;
                    return;
                }
            };
            let _ = settings.set_room_notification_mode(&rid, target).await;
        });
    }
}

#[cfg(not(test))]
impl ClientFfi {
    pub fn register_pusher(
        &mut self,
        pushkey: &str,
        app_id: &str,
        app_display_name: &str,
        device_display_name: &str,
        endpoint_url: &str,
        lang: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let mut http_data = HttpPusherData::new(endpoint_url.to_owned());
        http_data.format = Some(PushFormat::EventIdOnly);
        let pusher = PusherInit {
            ids: PusherIds::new(pushkey.to_owned(), app_id.to_owned()),
            app_display_name: app_display_name.to_owned(),
            kind: PusherKind::Http(http_data),
            lang: lang.to_owned(),
            device_display_name: device_display_name.to_owned(),
            profile_tag: None,
        };
        match self.rt.block_on(client.pusher().set(pusher.into())) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    pub fn remove_pusher(&mut self, pushkey: &str, app_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let ids = PusherIds::new(pushkey.to_owned(), app_id.to_owned());
        match self.rt.block_on(client.pusher().delete(ids)) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn register_pusher(
        &mut self,
        _pushkey: &str,
        _app_id: &str,
        _app_display_name: &str,
        _device_display_name: &str,
        _endpoint_url: &str,
        _lang: &str,
    ) -> OpResult {
        err("not logged in")
    }

    pub fn remove_pusher(&mut self, _pushkey: &str, _app_id: &str) -> OpResult {
        err("not logged in")
    }

    pub fn hint_push_room(&mut self, _room_id: &str) -> OpResult {
        err("sync not started")
    }

    pub fn get_room_notification_mode(&self, _room_id: &str) -> String {
        "default".to_owned()
    }

    pub fn set_room_notification_mode(&mut self, _room_id: &str, _mode: &str) {}
}
