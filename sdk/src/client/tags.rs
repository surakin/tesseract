//! Per-room tag toggles (`m.favourite` / `m.lowpriority`).
//!
//! Thin FFI wrappers over matrix-sdk's `Room::set_is_favourite` /
//! `Room::set_is_low_priority`. Those helpers already enforce mutual
//! exclusivity server-side: setting favourite removes low-priority and vice
//! versa. Fire-and-forget; errors are logged by the SDK. Blocks — worker thread.

use super::ClientFfi;

#[cfg(not(test))]
impl ClientFfi {
    pub fn set_room_favourite(&self, room_id: &str, value: bool) {
        use matrix_sdk::ruma::RoomId;
        let Some(client) = self.client.clone() else { return; };
        let room_id = room_id.to_owned();
        let _guard = super::InFlightGuard::new(&self.in_flight, &self.handler);
        self.rt.block_on(async move {
            let Ok(rid) = RoomId::parse(&room_id) else { return; };
            let Some(room) = client.get_room(&rid) else { return; };
            let _ = room.set_is_favourite(value, None).await;
        });
    }

    pub fn set_room_low_priority(&self, room_id: &str, value: bool) {
        use matrix_sdk::ruma::RoomId;
        let Some(client) = self.client.clone() else { return; };
        let room_id = room_id.to_owned();
        let _guard = super::InFlightGuard::new(&self.in_flight, &self.handler);
        self.rt.block_on(async move {
            let Ok(rid) = RoomId::parse(&room_id) else { return; };
            let Some(room) = client.get_room(&rid) else { return; };
            let _ = room.set_is_low_priority(value, None).await;
        });
    }
}

#[cfg(test)]
impl ClientFfi {
    pub fn set_room_favourite(&self, _room_id: &str, _value: bool) {}
    pub fn set_room_low_priority(&self, _room_id: &str, _value: bool) {}
}
