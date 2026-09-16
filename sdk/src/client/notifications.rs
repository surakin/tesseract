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
    pub fn hint_push_room(&self, room_id: &str) -> OpResult {
        let Some(svc) = self.sync_service.clone() else {
            return err("sync not started");
        };
        let push_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let mut ids: Vec<OwnedRoomId> = self.timelines.read().keys().cloned().collect();
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

    pub fn set_room_notification_mode(&self, room_id: &str, mode: &str) {
        use matrix_sdk::notification_settings::RoomNotificationMode;
        use matrix_sdk::ruma::RoomId;

        let Some(client) = self.client.clone() else {
            return;
        };
        let mode = mode.to_owned();
        let room_id = room_id.to_owned();

        self.rt.block_on(async move {
            let Ok(rid) = RoomId::parse(&room_id) else {
                return;
            };
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

// ---------------------------------------------------------------------------
// Global mention / @room / general-message / keyword notification controls
// ---------------------------------------------------------------------------
//
// Element-style granular controls, layered on top of matrix-sdk's low-level
// push-rule API (`Client::notification_settings()`). Unlike the per-room
// mode above, these are account-wide (no room_id): they toggle the
// `.m.rule.is_user_mention` / `.m.rule.is_room_mention` override rules
// (MSC3952 intentional mentions — matrix-sdk keeps the deprecated
// `contains_display_name`/`contains_user_name` legacy rules in sync
// automatically when these are toggled, so they don't need separate
// exposure), and the default underride notification mode for the 4
// (encrypted x one-to-one) room categories (the closest matrix concept to
// "notify me for general messages").
//
// Getters fail open (`true`) on error rather than closed: a spurious
// "disabled" reading is worse UX than a rare spurious "enabled" one (e.g.
// during a brief post-logout window). Setters return `false` on failure so
// the UI can revert an optimistic toggle instead of drifting from server
// state.
#[cfg(not(test))]
impl ClientFfi {
    pub fn get_mentions_enabled(&self) -> bool {
        use matrix_sdk::ruma::push::{PredefinedOverrideRuleId, RuleKind};

        let Some(client) = self.client.clone() else {
            return true;
        };
        self.rt.block_on(async move {
            client
                .notification_settings()
                .await
                .is_push_rule_enabled(RuleKind::Override, PredefinedOverrideRuleId::IsUserMention.as_str())
                .await
                .unwrap_or(true)
        })
    }

    pub fn set_mentions_enabled(&self, enabled: bool) -> bool {
        use matrix_sdk::ruma::push::{PredefinedOverrideRuleId, RuleKind};

        let Some(client) = self.client.clone() else {
            return false;
        };
        self.rt.block_on(async move {
            client
                .notification_settings()
                .await
                .set_push_rule_enabled(RuleKind::Override, PredefinedOverrideRuleId::IsUserMention.as_str(), enabled)
                .await
                .is_ok()
        })
    }

    pub fn get_room_mentions_enabled(&self) -> bool {
        use matrix_sdk::ruma::push::{PredefinedOverrideRuleId, RuleKind};

        let Some(client) = self.client.clone() else {
            return true;
        };
        self.rt.block_on(async move {
            client
                .notification_settings()
                .await
                .is_push_rule_enabled(RuleKind::Override, PredefinedOverrideRuleId::IsRoomMention.as_str())
                .await
                .unwrap_or(true)
        })
    }

    pub fn set_room_mentions_enabled(&self, enabled: bool) -> bool {
        use matrix_sdk::ruma::push::{PredefinedOverrideRuleId, RuleKind};

        let Some(client) = self.client.clone() else {
            return false;
        };
        self.rt.block_on(async move {
            client
                .notification_settings()
                .await
                .set_push_rule_enabled(RuleKind::Override, PredefinedOverrideRuleId::IsRoomMention.as_str(), enabled)
                .await
                .is_ok()
        })
    }

    /// Global default: whether rooms with no per-room override notify for
    /// every message (`true`) or mentions/keywords only (`false`). Reads the
    /// unencrypted, non-one-to-one category as representative — the setter
    /// always writes all 4 categories together, so they can't drift apart
    /// when driven exclusively through this FFI surface.
    pub fn get_default_notify_all_messages(&self) -> bool {
        use matrix_sdk::notification_settings::{IsEncrypted, IsOneToOne, RoomNotificationMode};

        let Some(client) = self.client.clone() else {
            return true;
        };
        self.rt.block_on(async move {
            matches!(
                client
                    .notification_settings()
                    .await
                    .get_default_room_notification_mode(IsEncrypted::No, IsOneToOne::No)
                    .await,
                RoomNotificationMode::AllMessages
            )
        })
    }

    /// Sets the global default across all 4 (encrypted x one-to-one) room
    /// categories. Returns `false` if any of the 4 writes failed
    /// (best-effort — matches this file's existing fire-and-forget posture
    /// for `set_room_notification_mode`; a partial failure still leaves
    /// whichever categories succeeded updated).
    pub fn set_default_notify_all_messages(&self, all_messages: bool) -> bool {
        use matrix_sdk::notification_settings::{IsEncrypted, IsOneToOne, RoomNotificationMode};

        let Some(client) = self.client.clone() else {
            return false;
        };
        let mode = if all_messages {
            RoomNotificationMode::AllMessages
        } else {
            RoomNotificationMode::MentionsAndKeywordsOnly
        };
        self.rt.block_on(async move {
            let settings = client.notification_settings().await;
            let combos = [
                (IsEncrypted::No, IsOneToOne::No),
                (IsEncrypted::No, IsOneToOne::Yes),
                (IsEncrypted::Yes, IsOneToOne::No),
                (IsEncrypted::Yes, IsOneToOne::Yes),
            ];
            let mut all_ok = true;
            for (enc, dm) in combos {
                if settings.set_default_room_notification_mode(enc, dm, mode).await.is_err() {
                    all_ok = false;
                }
            }
            all_ok
        })
    }

    /// Whether ANY keyword rule is currently enabled — the master "Notify
    /// on keywords" toggle Element shows above its keyword list. There is
    /// no dedicated push rule for this; it's a derived read over every
    /// non-default (i.e. user-added) Content-kind rule. Defaults to `false`
    /// (nothing to enable) rather than fail-open, unlike the mention/@room
    /// toggles above.
    pub fn get_notify_on_keywords_enabled(&self) -> bool {
        let Some(client) = self.client.clone() else {
            return false;
        };
        self.rt.block_on(async move {
            client.notification_settings().await.contains_keyword_rules().await
        })
    }

    /// Enable/disable every existing keyword rule at once (does not add or
    /// remove any keyword — only flips each one's `enabled` flag). Returns
    /// `false` if any individual rule failed to update; a keyword list with
    /// zero entries trivially succeeds (nothing to toggle).
    pub fn set_notify_on_keywords_enabled(&self, enabled: bool) -> bool {
        use matrix_sdk::ruma::push::RuleKind;

        let Some(client) = self.client.clone() else {
            return false;
        };
        self.rt.block_on(async move {
            let settings = client.notification_settings().await;
            let ruleset = settings.ruleset().await;
            let mut all_ok = true;
            for rule in ruleset.content.iter().filter(|r| !r.default) {
                if settings
                    .set_push_rule_enabled(RuleKind::Content, rule.rule_id.clone(), enabled)
                    .await
                    .is_err()
                {
                    all_ok = false;
                }
            }
            all_ok
        })
    }

    /// Currently enabled notification keywords, in server-reported order.
    pub fn get_notification_keywords(&self) -> Vec<String> {
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        self.rt.block_on(async move {
            client
                .notification_settings()
                .await
                .enabled_keywords()
                .await
                .into_iter()
                .collect()
        })
    }

    /// Adds a keyword-notification rule. Returns `false` on failure (e.g.
    /// server error) — the caller is expected to reject empty/whitespace-only
    /// input before calling, to avoid a wasted round trip.
    pub fn add_notification_keyword(&self, keyword: &str) -> bool {
        let Some(client) = self.client.clone() else {
            return false;
        };
        let keyword = keyword.to_owned();
        self.rt.block_on(async move {
            client.notification_settings().await.add_keyword(keyword).await.is_ok()
        })
    }

    /// Removes a keyword-notification rule. Returns `false` on failure.
    pub fn remove_notification_keyword(&self, keyword: &str) -> bool {
        let Some(client) = self.client.clone() else {
            return false;
        };
        let keyword = keyword.to_owned();
        self.rt.block_on(async move {
            client
                .notification_settings()
                .await
                .remove_keyword(&keyword)
                .await
                .is_ok()
        })
    }
}

#[cfg(not(test))]
impl ClientFfi {
    pub fn register_pusher(
        &self,
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
        match self.rt.block_on(client.pusher().set(pusher.into(), true)) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    pub fn remove_pusher(&self, pushkey: &str, app_id: &str) -> OpResult {
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
        &self,
        _pushkey: &str,
        _app_id: &str,
        _app_display_name: &str,
        _device_display_name: &str,
        _endpoint_url: &str,
        _lang: &str,
    ) -> OpResult {
        err("not logged in")
    }

    pub fn remove_pusher(&self, _pushkey: &str, _app_id: &str) -> OpResult {
        err("not logged in")
    }

    pub fn hint_push_room(&self, _room_id: &str) -> OpResult {
        err("sync not started")
    }

    pub fn get_room_notification_mode(&self, _room_id: &str) -> String {
        "default".to_owned()
    }

    pub fn set_room_notification_mode(&self, _room_id: &str, _mode: &str) {}

    pub fn get_mentions_enabled(&self) -> bool {
        true
    }

    pub fn set_mentions_enabled(&self, _enabled: bool) -> bool {
        false
    }

    pub fn get_room_mentions_enabled(&self) -> bool {
        true
    }

    pub fn set_room_mentions_enabled(&self, _enabled: bool) -> bool {
        false
    }

    pub fn get_default_notify_all_messages(&self) -> bool {
        true
    }

    pub fn set_default_notify_all_messages(&self, _all_messages: bool) -> bool {
        false
    }

    pub fn get_notify_on_keywords_enabled(&self) -> bool {
        false
    }

    pub fn set_notify_on_keywords_enabled(&self, _enabled: bool) -> bool {
        false
    }

    pub fn get_notification_keywords(&self) -> Vec<String> {
        Vec::new()
    }

    pub fn add_notification_keyword(&self, _keyword: &str) -> bool {
        false
    }

    pub fn remove_notification_keyword(&self, _keyword: &str) -> bool {
        false
    }
}

// ---------------------------------------------------------------------------
// Tests (pure logic only — push-rule writes require a live homeserver and
// are covered by matrix-sdk's own extensive notification_settings test
// suite; only Tesseract's thin mapping layer is unit-tested here).
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use matrix_sdk::notification_settings::RoomNotificationMode;

    /// Mirrors the `bool -> RoomNotificationMode` mapping used by
    /// `set_default_notify_all_messages`.
    fn mode_for(all_messages: bool) -> RoomNotificationMode {
        if all_messages {
            RoomNotificationMode::AllMessages
        } else {
            RoomNotificationMode::MentionsAndKeywordsOnly
        }
    }

    #[test]
    fn default_notify_all_messages_true_maps_to_all_messages() {
        assert_eq!(mode_for(true), RoomNotificationMode::AllMessages);
    }

    #[test]
    fn default_notify_all_messages_false_maps_to_mentions_only() {
        assert_eq!(mode_for(false), RoomNotificationMode::MentionsAndKeywordsOnly);
    }

    /// Mirrors the string mapping used by `get_room_notification_mode`/
    /// `set_room_notification_mode`, which the new default-mode functions
    /// build on (regression guard for that existing 4-way mapping).
    #[test]
    fn room_notification_mode_strings_round_trip() {
        let cases = [
            (RoomNotificationMode::AllMessages, "all"),
            (RoomNotificationMode::MentionsAndKeywordsOnly, "mentions"),
            (RoomNotificationMode::Mute, "off"),
        ];
        for (mode, s) in cases {
            let mapped = match mode {
                RoomNotificationMode::AllMessages => "all",
                RoomNotificationMode::MentionsAndKeywordsOnly => "mentions",
                RoomNotificationMode::Mute => "off",
            };
            assert_eq!(mapped, s);
        }
    }
}
