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
        push::{HttpPusherData, PushConditionRoomCtx, PushFormat, Ruleset},
        serde::Raw,
        OwnedRoomId, UInt,
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
#[cfg(not(test))]
pub(super) async fn evaluate_push_rules(
    client: &Client,
    room: &Room,
    source_json: &str,
) -> (bool, bool) {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    use serde_json::Value;

    // Read push rules from the local account-data cache (no network).
    let push_rules_et = GlobalAccountDataEventType::from("m.push_rules");
    let Some(raw) = client
        .account()
        .account_data_raw(push_rules_et)
        .await
        .ok()
        .flatten()
    else {
        return (false, false);
    };
    let Ok(content) = serde_json::from_str::<Value>(raw.json().get()) else {
        return (false, false);
    };
    let global = content.get("global").cloned().unwrap_or_default();
    let Ok(ruleset) = serde_json::from_value::<Ruleset>(global) else {
        return (false, false);
    };

    // Build push-condition context (no power-level data available without an
    // extra network round-trip — this skips sender_notification_permission
    // conditions but correctly handles member_count, contains_user_name, and
    // event_match rules such as mentions and keywords).
    let Some(uid) = client.user_id().map(|u| u.to_owned()) else {
        return (false, false);
    };
    let display_name = client
        .account()
        .get_display_name()
        .await
        .ok()
        .flatten()
        .unwrap_or_default();
    let member_count = room.joined_members_count();
    let ctx = PushConditionRoomCtx::new(
        room.room_id().to_owned(),
        UInt::try_from(member_count).unwrap_or(UInt::MAX),
        uid,
        display_name,
    );

    // Wrap source_json (synthetic event envelope) as Raw<AnySyncTimelineEvent>.
    let Ok(raw_value) = serde_json::from_str::<Box<serde_json::value::RawValue>>(source_json)
    else {
        return (false, false);
    };
    let raw_event = Raw::<AnySyncTimelineEvent>::from_json(raw_value);

    let actions = ruleset.get_actions(&raw_event, &ctx).await;
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
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::push::{Action, PushCondition, Ruleset};
        use serde_json::Value;

        let Some(client) = self.client.clone() else {
            return "default".to_owned();
        };
        let room_id = room_id.to_owned();

        self.rt.block_on(async move {
            let et = GlobalAccountDataEventType::from("m.push_rules");
            let Some(raw) = client
                .account()
                .account_data_raw(et)
                .await
                .ok()
                .flatten()
            else {
                return "default".to_owned();
            };
            let Ok(content) = serde_json::from_str::<Value>(raw.json().get()) else {
                return "default".to_owned();
            };
            let global = content.get("global").cloned().unwrap_or_default();
            let Ok(ruleset) = serde_json::from_value::<Ruleset>(global) else {
                return "default".to_owned();
            };

            // Check override rules for a room_id EventMatch condition with empty
            // actions — that is the "Off" (suppress all, including mentions) mode.
            for rule in &ruleset.override_ {
                if !rule.enabled {
                    continue;
                }
                let has_room_match = rule.conditions.iter().any(|c| {
                    if let PushCondition::EventMatch(data) = c {
                        data.key == "room_id" && data.pattern == room_id
                    } else {
                        false
                    }
                });
                if has_room_match && rule.actions.is_empty() {
                    return "off".to_owned();
                }
            }

            // Check room rules for this room_id.
            for rule in &ruleset.room {
                if rule.rule_id.as_str() != room_id {
                    continue;
                }
                if !rule.enabled {
                    continue;
                }
                if rule.actions.iter().any(|a| matches!(a, Action::Notify)) {
                    return "all".to_owned();
                }
                return "mentions".to_owned();
            }

            "default".to_owned()
        })
    }

    pub fn set_room_notification_mode(&mut self, room_id: &str, mode: &str) {
        use matrix_sdk::ruma::api::client::push::{
            delete_pushrule::v3::Request as DeletePushRule,
            set_pushrule::v3::Request as SetPushRule,
            RuleKind,
        };
        use matrix_sdk::ruma::push::{
            Action, EventMatchConditionData, NewConditionalPushRule, NewPushRule,
            NewSimplePushRule, PushCondition,
        };
        use matrix_sdk::ruma::OwnedRoomId;

        let Some(client) = self.client.clone() else {
            return;
        };
        let mode    = mode.to_owned();
        let room_id = room_id.to_owned();

        self.rt.block_on(async move {
            match mode.as_str() {
                "default" => {
                    let _ = client
                        .send(DeletePushRule::new(RuleKind::Override, room_id.clone()))
                        .await;
                    let _ = client
                        .send(DeletePushRule::new(RuleKind::Room, room_id))
                        .await;
                }
                "all" => {
                    let _ = client
                        .send(DeletePushRule::new(RuleKind::Override, room_id.clone()))
                        .await;
                    let Ok(rid) = room_id.parse::<OwnedRoomId>() else { return; };
                    let rule =
                        NewPushRule::Room(NewSimplePushRule::new(rid, vec![Action::Notify]));
                    let _ = client.send(SetPushRule::new(rule)).await;
                }
                "mentions" => {
                    let _ = client
                        .send(DeletePushRule::new(RuleKind::Override, room_id.clone()))
                        .await;
                    let Ok(rid) = room_id.parse::<OwnedRoomId>() else { return; };
                    let rule = NewPushRule::Room(NewSimplePushRule::new(rid, vec![]));
                    let _ = client.send(SetPushRule::new(rule)).await;
                }
                "off" => {
                    let _ = client
                        .send(DeletePushRule::new(RuleKind::Room, room_id.clone()))
                        .await;
                    let cond = PushCondition::EventMatch(EventMatchConditionData::new(
                        "room_id".to_owned(),
                        room_id.clone(),
                    ));
                    let rule = NewPushRule::Override(NewConditionalPushRule::new(
                        room_id,
                        vec![cond],
                        vec![],
                    ));
                    let _ = client.send(SetPushRule::new(rule)).await;
                }
                _ => {}
            }
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
