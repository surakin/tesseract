//! Room list / invites / room management (join, leave, members, topic,
//! room summary, space children).
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::{err, ok, ClientFfi};

use crate::ffi::OpResult;

#[cfg(all(not(test), debug_assertions))]
use std::sync::Arc;

#[cfg(not(test))]
use super::{
    build_invite_infos, build_room_infos, require_room, stop_fut, try_op,
};

#[cfg(not(test))]
use matrix_sdk::ruma::OwnedRoomId;

/// Serialisation target for `get_room_summary` / `get_space_child_summaries_batch`.
/// Matches the JSON shape consumed by `parse_room_summary_json` in client.cpp.
#[cfg(not(test))]
#[derive(serde::Serialize)]
struct RoomSummaryJson {
    room_id:            String,
    canonical_alias:    String,
    name:               String,
    topic:              String,
    avatar_url:         String,
    num_joined_members: u64,
    join_rule:          String,
    world_readable:     bool,
    is_space:           bool,
    membership:         String,
}

#[cfg(not(test))]
impl From<matrix_sdk::room_preview::RoomPreview> for RoomSummaryJson {
    fn from(p: matrix_sdk::room_preview::RoomPreview) -> Self {
        use matrix_sdk::ruma::room::RoomType;
        use matrix_sdk_base::RoomState;

        RoomSummaryJson {
            room_id:            p.room_id.to_string(),
            canonical_alias:    p.canonical_alias.map(|a| a.to_string()).unwrap_or_default(),
            name:               p.name.unwrap_or_default(),
            topic:              p.topic.unwrap_or_default(),
            avatar_url:         p.avatar_url.map(|u| u.to_string()).unwrap_or_default(),
            num_joined_members: p.num_joined_members,
            join_rule:          p.join_rule
                                    .as_ref()
                                    .map(|j| j.as_str().to_owned())
                                    .unwrap_or_else(|| "public".to_owned()),
            world_readable:     p.is_world_readable.unwrap_or(false),
            is_space:           matches!(p.room_type, Some(RoomType::Space)),
            membership:         match p.state {
                Some(RoomState::Joined)  => "join".to_owned(),
                Some(RoomState::Left)    => "leave".to_owned(),
                Some(RoomState::Invited) => "invite".to_owned(),
                Some(RoomState::Knocked) => "knock".to_owned(),
                Some(RoomState::Banned)  => "ban".to_owned(),
                None                     => String::new(),
            },
        }
    }
}

impl ClientFfi {
    pub fn list_rooms(&self) -> Vec<crate::ffi::RoomInfo> {
        #[cfg(not(test))]
        {
            let Some(client) = self.client.clone() else {
                return Vec::new();
            };
            self.rt.block_on(build_room_infos(&client))
        }
        #[cfg(test)]
        {
            Vec::new()
        }
    }

    /// Snapshot of all pending room invitations. Reads the local SDK cache —
    /// no network roundtrip. Blocks — call from a worker thread.
    #[cfg(not(test))]
    pub fn list_invites(&self) -> Vec<crate::ffi::InviteInfo> {
        let Some(client) = self.client.clone() else {
            return Vec::new();
        };
        self.rt.block_on(build_invite_infos(&client))
    }

    #[cfg(test)]
    pub fn list_invites(&self) -> Vec<crate::ffi::InviteInfo> {
        Vec::new()
    }

    pub fn space_children_all(&self, space_id: &str) -> Vec<String> {
        #[cfg(not(test))]
        {
            let Some(client) = self.client.as_ref() else {
                return vec![];
            };
            let Ok(room_id) = OwnedRoomId::try_from(space_id) else {
                return vec![];
            };
            let Some(space_room) = client.get_room(&room_id) else {
                return vec![];
            };

            self.rt.block_on(async move {
                use matrix_sdk::deserialized_responses::SyncOrStrippedState;
                use matrix_sdk::ruma::events::space::child::SpaceChildEventContent;
                use matrix_sdk::ruma::events::SyncStateEvent;

                let Ok(events) = space_room
                    .get_state_events_static::<SpaceChildEventContent>()
                    .await
                else {
                    return vec![];
                };

                // Mirror the pattern used by Room::parent_spaces() in matrix-sdk.
                // SpaceChildEventContent has state_key_type = OwnedRoomId, so
                // e.state_key is already typed — no JSON access needed.
                events
                    .into_iter()
                    .filter_map(|ev| match ev.deserialize() {
                        Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(e))) => {
                            if e.content.via.is_empty() { None } else { Some(e.state_key.to_owned()) }
                        }
                        Ok(SyncOrStrippedState::Sync(SyncStateEvent::Redacted(_))) => None,
                        Ok(SyncOrStrippedState::Stripped(e)) => {
                            if e.content.via.as_ref().map_or(true, |v| v.is_empty()) { None } else { Some(e.state_key.to_owned()) }
                        }
                        Err(_) => None,
                    })
                    .map(|id| id.to_string())
                    .collect()
            })
        }
        #[cfg(test)]
        {
            let _ = space_id;
            vec![]
        }
    }

    pub fn space_children(&self, space_id: &str) -> Vec<String> {
        #[cfg(not(test))]
        {
            let Some(client) = self.client.as_ref() else {
                return vec![];
            };
            self.space_children_all(space_id)
                .into_iter()
                .filter(|id| {
                    if let Ok(rid) = OwnedRoomId::try_from(id.as_str()) {
                        client.get_room(&rid).is_some()
                    } else {
                        false
                    }
                })
                .collect()
        }
        #[cfg(test)]
        {
            let _ = space_id;
            vec![]
        }
    }

    #[cfg(not(test))]
    pub fn get_room_summary(&self, room_id_or_alias: &str) -> String {
        use matrix_sdk::ruma::OwnedRoomOrAliasId;

        let Some(client) = self.client.clone() else { return String::new(); };
        if room_id_or_alias.is_empty() { return String::new(); }

        let id: OwnedRoomOrAliasId = match room_id_or_alias.try_into() {
            Ok(id) => id,
            Err(_) => return String::new(),
        };

        let stop_rx = self.stop_rx.clone();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/get_summary".to_string(),
        );
        let json = self.rt.block_on(async move {
            tokio::select! {
                result = client.get_room_preview(&id, vec![]) => {
                    match result {
                        Ok(preview) => serde_json::to_string(&RoomSummaryJson::from(preview))
                            .unwrap_or_default(),
                        Err(_) => String::new(),
                    }
                }
                _ = stop_fut(stop_rx) => String::new(),
            }
        });
        if !json.is_empty() {
            use std::time::{SystemTime, UNIX_EPOCH};
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_secs() as i64)
                .unwrap_or(0);
            if let Some(ref conn) = *self.app_cache_db.lock() {
                super::backfill::store_room_summary_conn(conn, room_id_or_alias, &json, now);
            }
        }
        json
    }

    /// Fetch the MSC3266 room preview for a single unjoined space child.
    /// Returns a JSON-serialised `RoomSummaryJson`, or an empty string on
    /// failure (timeout after 30 s, server error, or stop signal).
    #[cfg(not(test))]
    pub fn get_space_child_summary(&self, space_id: &str, room_id: &str) -> String {
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::space::child::SpaceChildEventContent;
        use matrix_sdk::ruma::events::SyncStateEvent;
        use matrix_sdk::ruma::{OwnedRoomId, OwnedRoomOrAliasId, OwnedServerName};

        let Some(client) = self.client.clone() else { return String::new(); };
        let Ok(rid) = OwnedRoomId::try_from(room_id) else { return String::new(); };
        let Ok(space_room_id) = OwnedRoomId::try_from(space_id) else { return String::new(); };
        let room_id_or_alias: OwnedRoomOrAliasId = rid.clone().into();
        let stop_rx = self.stop_rx.clone();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] format!("room_list/space_child/{}", room_id),
        );

        // Per-request deadline: a homeserver that never replies would otherwise
        // block block_on — and the C++ worker thread it runs on — forever.
        const PREVIEW_TIMEOUT_SECS: u64 = 30;

        let json = self.rt.block_on(async move {
            // Read via-server list from local space state — no network.
            let via: Vec<OwnedServerName> =
                if let Some(space_room) = client.get_room(&space_room_id) {
                    space_room
                        .get_state_events_static::<SpaceChildEventContent>()
                        .await
                        .ok()
                        .and_then(|evs| {
                            evs.into_iter().find_map(|ev| match ev.deserialize() {
                                Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(e)))
                                    if e.state_key == rid =>
                                {
                                    Some(e.content.via)
                                }
                                Ok(SyncOrStrippedState::Stripped(e))
                                    if e.state_key == rid =>
                                {
                                    Some(e.content.via.unwrap_or_default())
                                }
                                _ => None,
                            })
                        })
                        .unwrap_or_default()
                } else {
                    Vec::new()
                };

            let result = tokio::select! {
                timed = tokio::time::timeout(
                    std::time::Duration::from_secs(PREVIEW_TIMEOUT_SECS),
                    client.get_room_preview(&room_id_or_alias, via),
                ) => timed.ok().and_then(|r| r.ok()).map(RoomSummaryJson::from),
                _ = stop_fut(stop_rx) => None,
            };

            match result {
                Some(s) => serde_json::to_string(&s).unwrap_or_default(),
                None => String::new(),
            }
        });
        if !json.is_empty() {
            use std::time::{SystemTime, UNIX_EPOCH};
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_secs() as i64)
                .unwrap_or(0);
            if let Some(ref conn) = *self.app_cache_db.lock() {
                super::backfill::store_room_summary_conn(conn, room_id, &json, now);
            }
        }
        json
    }

    #[cfg(test)]
    pub fn get_room_summary(&self, _room_id_or_alias: &str) -> String {
        String::new()
    }

    #[cfg(test)]
    pub fn get_space_child_summary(
        &self,
        _space_id: &str,
        _room_id: &str,
    ) -> String {
        String::new()
    }

    #[cfg(not(test))]
    pub fn get_cached_room_summary(&self, room_id: &str) -> String {
        let guard = self.app_cache_db.lock();
        let Some(ref conn) = *guard else { return String::new() };
        super::backfill::load_room_summary_conn(conn, room_id)
    }

    #[cfg(test)]
    pub fn get_cached_room_summary(&self, _room_id: &str) -> String {
        String::new()
    }

    #[cfg(not(test))]
    pub fn join_room(&self, room_id_or_alias: &str) -> String {
        use matrix_sdk::ruma::OwnedRoomOrAliasId;

        let Some(client) = self.client.clone() else {
            return String::new();
        };
        if room_id_or_alias.is_empty() {
            return String::new();
        }

        let id: OwnedRoomOrAliasId = match room_id_or_alias.try_into() {
            Ok(id) => id,
            Err(_) => return String::new(),
        };
        let stop_rx = self.stop_rx.clone();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/join".to_string(),
        );
        self.rt.block_on(async move {
            tokio::select! {
                result = client.join_room_by_id_or_alias(&id, &[]) => {
                    match result {
                        Ok(room) => room.room_id().to_string(),
                        Err(_)   => String::new(),
                    }
                }
                _ = stop_fut(stop_rx) => String::new(),
            }
        })
    }

    #[cfg(test)]
    pub fn join_room(&self, _room_id_or_alias: &str) -> String {
        String::new()
    }

    // -----------------------------------------------------------------------
    // Room management
    // -----------------------------------------------------------------------

    /// Leave a room. Blocks the calling thread — call from a worker thread.
    #[cfg(not(test))]
    pub fn leave_room(&self, room_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/leave".to_string(),
        );
        match self.rt.block_on(room.leave()) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn leave_room(&self, _room_id: &str) -> OpResult {
        err("not logged in")
    }

    // ------------------------------------------------------------------
    // Non-blocking async variants — frees the calling C++ thread immediately
    // ------------------------------------------------------------------

    #[cfg(not(test))]
    pub fn accept_invite_async(&self, request_id: u64, room_id: &str) {
        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();
        let room_id_str = room_id.to_owned();

        let deliver = {
            let handler = handler.clone();
            move |ok: bool, joined: &str, msg: &str| {
                if let Some(h) = &handler {
                    {
                        let g = h.lock();
                        g.on_room_action_complete(request_id, ok, joined, msg);
                    }
                }
            }
        };

        let room_id_parsed: OwnedRoomId = match room_id_str.parse() {
            Ok(id) => id,
            Err(e) => { deliver(false, "", &format!("invalid room id: {e}")); return; }
        };

        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "room_list/accept_invite".to_string(),
            );
            let Some(room) = client.get_room(&room_id_parsed) else {
                deliver(false, "", "room not found");
                return;
            };
            let was_direct = room.is_direct().await.unwrap_or(false);
            match room.join().await {
                Ok(_) => {
                    if was_direct { let _ = room.set_is_direct(true).await; }
                    deliver(true, &room_id_str, "");
                }
                Err(e) => deliver(false, "", &e.to_string()),
            }
        });
    }

    #[cfg(test)]
    pub fn accept_invite_async(&self, _request_id: u64, _room_id: &str) {}

    #[cfg(not(test))]
    pub fn decline_invite_async(&self, room_id: &str) {
        let Some(client) = self.client.clone() else { return; };
        let room_id_parsed: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "room_list/decline_invite".to_string(),
            );
            if let Some(room) = client.get_room(&room_id_parsed) {
                let _ = room.leave().await;
            }
        });
    }

    #[cfg(test)]
    pub fn decline_invite_async(&self, _room_id: &str) {}

    #[cfg(not(test))]
    pub fn block_invite_async(&self, room_id: &str, inviter_user_id: &str) {
        let Some(client) = self.client.clone() else { return; };
        let room_id_parsed: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(inviter_user_id) else { return; };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "room_list/block_invite".to_string(),
            );
            if let Some(room) = client.get_room(&room_id_parsed) {
                let _ = room.leave().await;
            }
            let _ = client.account().ignore_user(&uid).await;
        });
    }

    #[cfg(test)]
    pub fn block_invite_async(&self, _room_id: &str, _inviter_user_id: &str) {}

    #[cfg(not(test))]
    pub fn join_room_async(&self, request_id: u64, room_id_or_alias: &str) {
        use matrix_sdk::ruma::OwnedRoomOrAliasId;

        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();
        let id_str = room_id_or_alias.to_owned();
        let stop_rx = self.stop_rx.clone();

        let deliver = move |ok: bool, joined: &str, msg: &str| {
            if let Some(h) = &handler {
                {
                    let g = h.lock();
                    g.on_room_action_complete(request_id, ok, joined, msg);
                }
            }
        };

        let id: OwnedRoomOrAliasId = match id_str.as_str().try_into() {
            Ok(id) => id,
            Err(_) => { deliver(false, "", "invalid room id or alias"); return; }
        };

        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "room_list/join".to_string(),
            );
            let result = tokio::select! {
                r = client.join_room_by_id_or_alias(&id, &[]) => {
                    r.ok().map(|r| r.room_id().to_string())
                }
                _ = stop_fut(stop_rx) => None,
            };
            match result {
                Some(joined_id) => deliver(true, &joined_id, ""),
                None => deliver(false, "", "join failed or cancelled"),
            }
        });
    }

    #[cfg(test)]
    pub fn join_room_async(&self, _request_id: u64, _room_id_or_alias: &str) {}

    #[cfg(not(test))]
    pub fn leave_room_async(&self, request_id: u64, room_id: &str) {
        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();
        let room_id_str = room_id.to_owned();

        let deliver = move |ok: bool, msg: &str| {
            if let Some(h) = &handler {
                {
                    let g = h.lock();
                    g.on_room_action_complete(request_id, ok, "", msg);
                }
            }
        };

        let room_id_parsed: OwnedRoomId = match room_id_str.parse() {
            Ok(id) => id,
            Err(e) => { deliver(false, &format!("invalid room id: {e}")); return; }
        };

        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "room_list/leave".to_string(),
            );
            let Some(room) = client.get_room(&room_id_parsed) else {
                deliver(false, "room not found");
                return;
            };
            match room.leave().await {
                Ok(_) => deliver(true, ""),
                Err(e) => deliver(false, &e.to_string()),
            }
        });
    }

    #[cfg(test)]
    pub fn leave_room_async(&self, _request_id: u64, _room_id: &str) {}

    #[cfg(not(test))]
    pub fn invite_user_async(&self, room_id: &str, user_id: &str) {
        use matrix_sdk::ruma::UserId;
        let Some(client) = self.client.clone() else { return; };
        let room_id_parsed: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        let Ok(uid) = UserId::parse(user_id) else { return; };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "room_list/invite_user".to_string(),
            );
            if let Some(room) = client.get_room(&room_id_parsed) {
                let _ = room.invite_user_by_id(&uid).await;
            }
        });
    }

    #[cfg(test)]
    pub fn invite_user_async(&self, _room_id: &str, _user_id: &str) {}

    /// Fetch the joined member list for a room. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn get_room_members(&self, room_id: &str) -> Vec<crate::ffi::RoomMember> {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return Vec::new();
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return Vec::new(),
        };
        let Some(room) = client.get_room(&room_id) else {
            return Vec::new();
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/get_members".to_string(),
        );
        match self.rt.block_on(room.members(matrix_sdk::RoomMemberships::JOIN)) {
            Ok(members) => members
                .into_iter()
                .map(|m| crate::ffi::RoomMember {
                    user_id: m.user_id().to_string(),
                    display_name: m
                        .display_name()
                        .map(str::to_owned)
                        .unwrap_or_else(|| m.user_id().localpart().to_string()),
                    avatar_url: m.avatar_url().map(|u| u.to_string()).unwrap_or_default(),
                })
                .collect(),
            Err(_) => Vec::new(),
        }
    }

    #[cfg(test)]
    pub fn get_room_members(&self, _room_id: &str) -> Vec<crate::ffi::RoomMember> {
        Vec::new()
    }

    /// Send an m.room.topic state event. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_topic(&self, room_id: &str, topic: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::topic::RoomTopicEventContent;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let content = RoomTopicEventContent::new(topic.to_owned());
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/set_topic".to_string(),
        );
        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_topic(&self, _room_id: &str, _topic: &str) -> OpResult {
        err("not logged in")
    }

    /// Set the current user's display name in a specific room (m.room.member
    /// state event). Preserves all other existing member event fields. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_display_name(&self, room_id: &str, name: &str) -> OpResult {
        use matrix_sdk::ruma::events::room::member::{MembershipState, RoomMemberEventContent};

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let Some(user_id) = client.user_id() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/set_display_name".to_string(),
        );

        let mut content = match self.rt.block_on(room.get_member(user_id)) {
            Ok(Some(m)) => match m.event().as_sync() {
                Some(e) => e.as_original().map(|o| o.content.clone()),
                None => None,
            }
            .unwrap_or_else(|| RoomMemberEventContent::new(MembershipState::Join)),
            _ => RoomMemberEventContent::new(MembershipState::Join),
        };

        content.displayname = if name.is_empty() { None } else { Some(name.to_owned()) };

        match self.rt.block_on(room.send_state_event_for_key(user_id, content)) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_display_name(&self, _room_id: &str, _name: &str) -> OpResult {
        err("not logged in")
    }

    /// Set the current user's avatar in a specific room (m.room.member state
    /// event). Preserves all other existing member event fields. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_avatar(&self, room_id: &str, mxc_uri: &str) -> OpResult {
        use matrix_sdk::ruma::{
            events::room::member::{MembershipState, RoomMemberEventContent},
            OwnedMxcUri,
        };

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let Some(user_id) = client.user_id() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));

        let mxc: OwnedMxcUri = match mxc_uri.try_into() {
            Ok(u)  => u,
            Err(_) => return err("invalid mxc URI"),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "room_list/set_avatar".to_string(),
        );

        let mut content = match self.rt.block_on(room.get_member(user_id)) {
            Ok(Some(m)) => match m.event().as_sync() {
                Some(e) => e.as_original().map(|o| o.content.clone()),
                None => None,
            }
            .unwrap_or_else(|| RoomMemberEventContent::new(MembershipState::Join)),
            _ => RoomMemberEventContent::new(MembershipState::Join),
        };

        content.avatar_url = Some(mxc);

        match self.rt.block_on(room.send_state_event_for_key(user_id, content)) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_avatar(&self, _room_id: &str, _mxc_uri: &str) -> OpResult {
        err("not logged in")
    }
}
