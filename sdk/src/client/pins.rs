//! Matrix pinned events: m.room.pinned_events state event read/write +
//! the power-level check used by the UI to hide the Pin button when the
//! current user can't pin. Mirrors the set_room_topic pattern in
//! room_list.rs (read-modify-write of a state event).

use super::{err, ok, ClientFfi};
use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{require_room, try_op};

#[cfg(not(test))]
impl ClientFfi {
    /// Append `event_id` to m.room.pinned_events if not already present, then
    /// write the state event back. Server enforces permission.
    pub fn pin_event(&self, room_id: &str, event_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        use matrix_sdk::ruma::events::SyncStateEvent;
        use matrix_sdk::ruma::OwnedEventId;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));

        let Ok(target) = event_id.parse::<OwnedEventId>() else {
            return err("invalid event_id");
        };

        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "pins/send".to_string(),
        );

        // Read existing m.room.pinned_events (may be absent → start empty).
        let existing: Vec<OwnedEventId> = match self
            .rt
            .block_on(room.get_state_event_static::<RoomPinnedEventsEventContent>())
        {
            Ok(Some(raw)) => match raw.deserialize() {
                Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(o))) => o.content.pinned,
                // Redacted or stripped variants: treat as no existing pins.
                Ok(_) => Vec::new(),
                Err(_) => Vec::new(),
            },
            Ok(None) => Vec::new(),
            Err(e) => return err(format!("read pinned: {e}")),
        };

        if existing.iter().any(|id| id == &target) {
            return ok(""); // already pinned — idempotent
        }

        let mut next = existing;
        next.push(target);
        let content = RoomPinnedEventsEventContent::new(next);

        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    /// Remove `event_id` from m.room.pinned_events if present, then write
    /// back. No-op (returns ok) if not pinned.
    pub fn unpin_event(&self, room_id: &str, event_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        use matrix_sdk::ruma::events::SyncStateEvent;
        use matrix_sdk::ruma::OwnedEventId;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));

        let Ok(target) = event_id.parse::<OwnedEventId>() else {
            return err("invalid event_id");
        };

        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "pins/unpin".to_string(),
        );

        let existing: Vec<OwnedEventId> = match self
            .rt
            .block_on(room.get_state_event_static::<RoomPinnedEventsEventContent>())
        {
            Ok(Some(raw)) => match raw.deserialize() {
                Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(o))) => o.content.pinned,
                // Redacted/stripped: nothing to unpin from.
                Ok(_) => return ok(""),
                Err(_) => return ok(""),
            },
            Ok(None) => return ok(""),
            Err(e) => return err(format!("read pinned: {e}")),
        };

        if !existing.iter().any(|id| id == &target) {
            return ok(""); // not pinned — idempotent
        }

        let next: Vec<OwnedEventId> = existing.into_iter().filter(|id| id != &target).collect();
        let content = RoomPinnedEventsEventContent::new(next);

        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    /// Returns true iff the current user has permission to send
    /// m.room.pinned_events state events in this room. Reads cached
    /// m.room.power_levels — no network round-trip. Returns false on any
    /// uncertainty (not logged in, room unknown, PL unreadable).
    pub fn can_pin_in_room(&self, room_id: &str) -> bool {
        use matrix_sdk::ruma::events::StateEventType;
        use matrix_sdk::ruma::OwnedRoomId;

        let Some(client) = self.client.as_ref() else {
            return false;
        };
        let Ok(room_id_parsed) = room_id.parse::<OwnedRoomId>() else {
            return false;
        };
        let Some(room) = client.get_room(&room_id_parsed) else {
            return false;
        };
        let Some(user_id) = client.user_id() else {
            return false;
        };

        match self.rt.block_on(room.power_levels()) {
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomPinnedEvents),
            Err(_) => false,
        }
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn pin_event(&self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }
    pub fn unpin_event(&self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }
    pub fn can_pin_in_room(&self, _room_id: &str) -> bool {
        false
    }
}

// ---------------------------------------------------------------------------
// Tests (content-shape only — state-event sends require a live homeserver).
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
    use matrix_sdk::ruma::OwnedEventId;

    #[test]
    fn empty_pinned_content_serializes() {
        let c = RoomPinnedEventsEventContent::new(Vec::new());
        let v = serde_json::to_value(&c).unwrap();
        assert!(v.get("pinned").is_some());
        assert_eq!(v["pinned"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn pinned_content_round_trips_two_ids() {
        let ids: Vec<OwnedEventId> = vec![
            "$abc:server".parse().unwrap(),
            "$def:server".parse().unwrap(),
        ];
        let c = RoomPinnedEventsEventContent::new(ids.clone());
        let v = serde_json::to_value(&c).unwrap();
        let arr = v["pinned"].as_array().unwrap();
        assert_eq!(arr.len(), 2);
        assert_eq!(arr[0].as_str().unwrap(), "$abc:server");
        assert_eq!(arr[1].as_str().unwrap(), "$def:server");
    }
}
