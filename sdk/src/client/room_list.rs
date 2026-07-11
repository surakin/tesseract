//! Room list / invites / room management (join, leave, members, topic,
//! room summary, space children).
//!
//! Split out of `client.rs` in the modularization refactor; behavior unchanged.

use super::{err, ok, ClientFfi};

use crate::ffi::OpResult;

#[cfg(all(not(test), debug_assertions))]
use std::sync::Arc;

#[cfg(not(test))]
use super::{build_invite_infos, build_room_infos, require_room, stop_fut, try_op};

#[cfg(not(test))]
use matrix_sdk::ruma::OwnedRoomId;

/// Serialisation target for `get_room_summary` / `get_space_child_summaries_batch`.
/// Matches the JSON shape consumed by `parse_room_summary_json` in client.cpp.
#[cfg(not(test))]
#[derive(serde::Serialize)]
struct RoomSummaryJson {
    room_id: String,
    canonical_alias: String,
    name: String,
    topic: String,
    avatar_url: String,
    num_joined_members: u64,
    join_rule: String,
    world_readable: bool,
    is_space: bool,
    membership: String,
}

#[cfg(not(test))]
impl From<matrix_sdk::room_preview::RoomPreview> for RoomSummaryJson {
    fn from(p: matrix_sdk::room_preview::RoomPreview) -> Self {
        use matrix_sdk::ruma::room::RoomType;
        use matrix_sdk_base::RoomState;

        RoomSummaryJson {
            room_id: p.room_id.to_string(),
            canonical_alias: p.canonical_alias.map(|a| a.to_string()).unwrap_or_default(),
            name: p.name.unwrap_or_default(),
            topic: p.topic.unwrap_or_default(),
            avatar_url: p.avatar_url.map(|u| u.to_string()).unwrap_or_default(),
            num_joined_members: p.num_joined_members,
            join_rule: p
                .join_rule
                .as_ref()
                .map(|j| j.as_str().to_owned())
                .unwrap_or_else(|| "public".to_owned()),
            world_readable: p.is_world_readable.unwrap_or(false),
            is_space: matches!(p.room_type, Some(RoomType::Space)),
            membership: match p.state {
                Some(RoomState::Joined) => "join".to_owned(),
                Some(RoomState::Left) => "leave".to_owned(),
                Some(RoomState::Invited) => "invite".to_owned(),
                Some(RoomState::Knocked) => "knock".to_owned(),
                Some(RoomState::Banned) => "ban".to_owned(),
                None => String::new(),
            },
        }
    }
}

type SpaceSummaryTasks =
    std::sync::Arc<parking_lot::Mutex<std::collections::HashMap<String, Vec<(u64, tokio::task::AbortHandle)>>>>;

/// Register a `get_space_child_summary_async` task's abort handle under
/// `space_id` so `abort_space_summary_group` can abort it later. Prunes
/// already-finished handles first to keep the per-space `Vec` from growing
/// unbounded across a long browsing session. Not `ClientFfi`-specific, so it's
/// unit-testable without a real client (see `tests` module below).
fn register_space_summary_task(
    map: &SpaceSummaryTasks,
    space_id: String,
    request_id: u64,
    handle: tokio::task::AbortHandle,
) {
    let mut m = map.lock();
    let v = m.entry(space_id).or_default();
    v.retain(|(_, h)| !h.is_finished());
    v.push((request_id, handle));
}

/// Abort and drop every task registered under `space_id`, if any.
fn abort_space_summary_group(map: &SpaceSummaryTasks, space_id: &str) {
    let mut m = map.lock();
    if let Some(v) = m.remove(space_id) {
        for (_, h) in v {
            h.abort();
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
            self.rt
                .block_on(build_room_infos(&client, &self.app_cache_db))
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
                            if e.content.via.is_empty() {
                                None
                            } else {
                                Some(e.state_key.to_owned())
                            }
                        }
                        Ok(SyncOrStrippedState::Sync(SyncStateEvent::Redacted(_))) => None,
                        Ok(SyncOrStrippedState::Stripped(e)) => {
                            if e.content.via.as_ref().map_or(true, |v| v.is_empty()) {
                                None
                            } else {
                                Some(e.state_key.to_owned())
                            }
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
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/get_summary".to_string(),
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

        let Some(client) = self.client.clone() else {
            return String::new();
        };
        let Ok(rid) = OwnedRoomId::try_from(room_id) else {
            return String::new();
        };
        let Ok(space_room_id) = OwnedRoomId::try_from(space_id) else {
            return String::new();
        };
        let room_id_or_alias: OwnedRoomOrAliasId = rid.clone().into();
        let stop_rx = self.stop_rx.clone();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            format!("room_list/space_child/{}", room_id),
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
                                Ok(SyncOrStrippedState::Stripped(e)) if e.state_key == rid => {
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

    /// Async counterpart of `get_space_child_summary`. Spawns the fetch on the
    /// tokio runtime and fires `on_space_child_summary_ready(request_id, json)`
    /// on completion (empty string on failure or timeout). Does not pin a thread.
    #[cfg(not(test))]
    pub fn get_space_child_summary_async(&self, request_id: u64, space_id: &str, room_id: &str) {
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::space::child::SpaceChildEventContent;
        use matrix_sdk::ruma::events::SyncStateEvent;
        use matrix_sdk::ruma::{OwnedRoomId, OwnedRoomOrAliasId, OwnedServerName};

        let deliver = |json: String| {
            if let Some(ref h) = self.handler {
                let g = h.lock();
                g.on_space_child_summary_ready(request_id, &json);
            }
        };

        let Some(client) = self.client.clone() else {
            deliver(String::new());
            return;
        };
        let Ok(rid) = OwnedRoomId::try_from(room_id) else {
            deliver(String::new());
            return;
        };
        let Ok(space_room_id) = OwnedRoomId::try_from(space_id) else {
            deliver(String::new());
            return;
        };
        let room_id_or_alias: OwnedRoomOrAliasId = rid.clone().into();

        let handler = self.handler.clone();
        let stop_rx = self.stop_rx.clone();
        let app_cache_db = self.app_cache_db.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        let room_id_owned = room_id.to_owned();
        let space_id_owned = space_id.to_owned();

        let handle = self.rt.spawn(async move {
            let json = {
                let _guard = super::InFlightGuard::new(
                    &in_flight,
                    &handler,
                    #[cfg(debug_assertions)]
                    &in_flight_urls,
                    #[cfg(debug_assertions)]
                    format!("room_list/space_child/{}", room_id_owned),
                );

                const PREVIEW_TIMEOUT_SECS: u64 = 30;

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
                                    Ok(SyncOrStrippedState::Stripped(e)) if e.state_key == rid => {
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
            }; // _guard drops here; inflight count decrements before callback

            if !json.is_empty() {
                use std::time::{SystemTime, UNIX_EPOCH};
                let now = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map(|d| d.as_secs() as i64)
                    .unwrap_or(0);
                if let Some(ref conn) = *app_cache_db.lock() {
                    super::backfill::store_room_summary_conn(conn, &room_id_owned, &json, now);
                }
            }

            if let Some(h) = handler {
                let g = h.lock();
                g.on_space_child_summary_ready(request_id, &json);
            }
        });

        register_space_summary_task(
            &self.space_summary_tasks,
            space_id_owned,
            request_id,
            handle.abort_handle(),
        );
    }

    /// Abort every still-running `get_space_child_summary_async` fetch
    /// registered under `space_id`. Called when the user navigates away from a
    /// space so its still-pending unjoined-room preview fetches stop hitting
    /// the homeserver once their results can no longer be used.
    #[cfg(not(test))]
    pub fn cancel_space_summaries(&self, space_id: &str) {
        abort_space_summary_group(&self.space_summary_tasks, space_id);
    }

    #[cfg(test)]
    pub fn cancel_space_summaries(&self, _space_id: &str) {}

    #[cfg(test)]
    pub fn get_room_summary(&self, _room_id_or_alias: &str) -> String {
        String::new()
    }

    #[cfg(test)]
    pub fn get_space_child_summary(&self, _space_id: &str, _room_id: &str) -> String {
        String::new()
    }

    #[cfg(test)]
    pub fn get_space_child_summary_async(&self, _request_id: u64, _space_id: &str, _room_id: &str) {
    }

    #[cfg(not(test))]
    pub fn get_cached_room_summary(&self, room_id: &str) -> String {
        let guard = self.app_cache_db.lock();
        let Some(ref conn) = *guard else {
            return String::new();
        };
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
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/join".to_string(),
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
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/leave".to_string(),
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
        let Some(client) = self.client.clone() else {
            return;
        };
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
            Err(e) => {
                deliver(false, "", &format!("invalid room id: {e}"));
                return;
            }
        };

        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/accept_invite".to_string(),
            );
            let Some(room) = client.get_room(&room_id_parsed) else {
                deliver(false, "", "room not found");
                return;
            };
            let was_direct = room.is_direct().await.unwrap_or(false);
            match room.join().await {
                Ok(_) => {
                    if was_direct {
                        let _ = room.set_is_direct(true).await;
                    }
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
        let Some(client) = self.client.clone() else {
            return;
        };
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
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/decline_invite".to_string(),
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
        let Some(client) = self.client.clone() else {
            return;
        };
        let room_id_parsed: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        let Ok(uid) = matrix_sdk::ruma::UserId::parse(inviter_user_id) else {
            return;
        };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/block_invite".to_string(),
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

        let Some(client) = self.client.clone() else {
            return;
        };
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
            Err(_) => {
                deliver(false, "", "invalid room id or alias");
                return;
            }
        };

        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/join".to_string(),
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
        let Some(client) = self.client.clone() else {
            return;
        };
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
            Err(e) => {
                deliver(false, &format!("invalid room id: {e}"));
                return;
            }
        };

        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/leave".to_string(),
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
        let Some(client) = self.client.clone() else {
            return;
        };
        let room_id_parsed: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        let Ok(uid) = UserId::parse(user_id) else {
            return;
        };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/invite_user".to_string(),
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
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/get_members".to_string(),
        );
        match self
            .rt
            .block_on(room.members(matrix_sdk::RoomMemberships::JOIN))
        {
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
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_topic".to_string(),
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

    /// Enable encryption for a room by sending m.room.encryption. No-op if
    /// already encrypted. There is no counterpart to disable encryption —
    /// no such operation exists anywhere in matrix-sdk. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_encryption(&self, room_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_encryption".to_string(),
        );
        match self.rt.block_on(room.enable_encryption()) {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_encryption(&self, _room_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Send an m.room.join_rules state event. `join_rule` must be one of
    /// "public"/"invite"/"knock" — any other value (including
    /// "restricted"/"knock_restricted", which carry an allow-list this
    /// client doesn't manage) is rejected as defense-in-depth against a
    /// stale staged value, since the UI never offers them as editable
    /// choices. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_join_rule(&self, room_id: &str, join_rule: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::join_rules::{JoinRule, RoomJoinRulesEventContent};

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let rule = match join_rule {
            "public" => JoinRule::Public,
            "invite" => JoinRule::Invite,
            "knock" => JoinRule::Knock,
            other => return err(format!("unsupported join rule: {other}")),
        };
        let content = RoomJoinRulesEventContent::new(rule);
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_join_rule".to_string(),
        );
        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_join_rule(&self, _room_id: &str, _join_rule: &str) -> OpResult {
        err("not logged in")
    }

    /// Send an m.room.guest_access state event. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_guest_access(&self, room_id: &str, allow: bool) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::guest_access::{
            GuestAccess, RoomGuestAccessEventContent,
        };

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let content = RoomGuestAccessEventContent::new(if allow {
            GuestAccess::CanJoin
        } else {
            GuestAccess::Forbidden
        });
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_guest_access".to_string(),
        );
        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_guest_access(&self, _room_id: &str, _allow: bool) -> OpResult {
        err("not logged in")
    }

    /// Send an m.room.history_visibility state event. `visibility` must be
    /// one of "world_readable"/"shared"/"invited"/"joined". Blocks — worker
    /// thread.
    #[cfg(not(test))]
    pub fn set_room_history_visibility(&self, room_id: &str, visibility: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::history_visibility::{
            HistoryVisibility, RoomHistoryVisibilityEventContent,
        };

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let hv = match visibility {
            "world_readable" => HistoryVisibility::WorldReadable,
            "shared" => HistoryVisibility::Shared,
            "invited" => HistoryVisibility::Invited,
            "joined" => HistoryVisibility::Joined,
            other => return err(format!("unsupported history visibility: {other}")),
        };
        let content = RoomHistoryVisibilityEventContent::new(hv);
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_history_visibility".to_string(),
        );
        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_history_visibility(&self, _room_id: &str, _visibility: &str) -> OpResult {
        err("not logged in")
    }

    /// Read the room's current power levels, narrowed to the 9 fields the
    /// Permissions room-settings tab edits. Synchronous — Room::power_levels()
    /// is a cached local read with no network round-trip, unlike the fields
    /// the Security & Privacy tab needs an async GET /state fetch for (see
    /// fetch_room_security_state_async's doc comment for that distinction).
    /// Returns Matrix spec defaults on any error. Blocks briefly — worker
    /// thread.
    #[cfg(not(test))]
    pub fn room_power_levels(&self, room_id: &str) -> crate::ffi::RoomPowerLevelsFfi {
        fn defaults() -> crate::ffi::RoomPowerLevelsFfi {
            crate::ffi::RoomPowerLevelsFfi {
                default_role: 0,
                send_messages: 0,
                invite_users: 0,
                change_settings: 50,
                kick_users: 50,
                ban_users: 50,
                remove_messages: 50,
                notify_everyone: 50,
                change_permissions: 50,
            }
        }
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return defaults();
        };
        let Ok((_, room)) = require_room(client, room_id) else {
            return defaults();
        };
        let Ok(pl) = self.rt.block_on(room.power_levels()) else {
            return defaults();
        };
        let change_permissions = pl
            .events
            .get(&matrix_sdk::ruma::events::TimelineEventType::from(
                "m.room.power_levels",
            ))
            .map(|v| i64::from(*v))
            .unwrap_or_else(|| i64::from(pl.state_default));
        crate::ffi::RoomPowerLevelsFfi {
            default_role: i64::from(pl.users_default),
            send_messages: i64::from(pl.events_default),
            invite_users: i64::from(pl.invite),
            change_settings: i64::from(pl.state_default),
            kick_users: i64::from(pl.kick),
            ban_users: i64::from(pl.ban),
            remove_messages: i64::from(pl.redact),
            notify_everyone: i64::from(pl.notifications.room),
            change_permissions,
        }
    }

    #[cfg(test)]
    pub fn room_power_levels(&self, _room_id: &str) -> crate::ffi::RoomPowerLevelsFfi {
        crate::ffi::RoomPowerLevelsFfi::default()
    }

    /// Send an updated m.room.power_levels state event with the 9 fields
    /// from `levels`. Reads the current power levels first (preserving every
    /// field this tab doesn't edit, e.g. per-user overrides and other
    /// per-event-type overrides), mutates only the 9 fields the Permissions
    /// tab exposes, then sends the result. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_power_levels(
        &self,
        room_id: &str,
        levels: crate::ffi::RoomPowerLevelsFfi,
    ) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::power_levels::RoomPowerLevelsEventContent;
        use matrix_sdk::ruma::events::TimelineEventType;
        use matrix_sdk::ruma::Int;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let mut pl = match self.rt.block_on(room.power_levels()) {
            Ok(p) => p,
            Err(e) => return err(e.to_string()),
        };

        fn to_int(val: i64) -> Result<Int, OpResult> {
            Int::new(val).ok_or_else(|| err(format!("power level {val} out of range")))
        }
        pl.users_default = try_op!(to_int(levels.default_role));
        pl.events_default = try_op!(to_int(levels.send_messages));
        pl.invite = try_op!(to_int(levels.invite_users));
        pl.state_default = try_op!(to_int(levels.change_settings));
        pl.kick = try_op!(to_int(levels.kick_users));
        pl.ban = try_op!(to_int(levels.ban_users));
        pl.redact = try_op!(to_int(levels.remove_messages));
        pl.notifications.room = try_op!(to_int(levels.notify_everyone));
        pl.events.insert(
            TimelineEventType::from("m.room.power_levels"),
            try_op!(to_int(levels.change_permissions)),
        );

        let content = match RoomPowerLevelsEventContent::try_from(pl) {
            Ok(c) => c,
            Err(e) => return err(e.to_string()),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_power_levels".to_string(),
        );
        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_power_levels(
        &self,
        _room_id: &str,
        _levels: crate::ffi::RoomPowerLevelsFfi,
    ) -> OpResult {
        err("not logged in")
    }

    /// The current user's own effective power level in this room, via
    /// ruma's `RoomPowerLevels::for_user` — NOT hand-rolled from the
    /// `users`/`users_default` fields, because room versions 12+ give room
    /// creators an "infinite" power level that never appears in the
    /// `users` map at all (`explicitly_privilege_room_creators`); reading
    /// `users`/`users_default` directly would misreport a creator as
    /// having only `users_default`'s (often 0) power. `has_explicit_override`
    /// is true when the level is fixed regardless of a staged
    /// `users_default` change — i.e. a privileged creator (Infinite) or an
    /// explicit `users` entry — false when it simply mirrors the current
    /// `users_default`. `level` uses i64::MAX for Infinite, which sorts
    /// above any valid Matrix power level (ruma's `Int` is a JS-safe
    /// integer, far below i64::MAX). Synchronous — Room::power_levels() is
    /// a cached local read with no network round-trip. Returns level 0 /
    /// no override on any error. Blocks briefly — worker thread.
    #[cfg(not(test))]
    pub fn room_own_power_level(&self, room_id: &str) -> crate::ffi::RoomOwnPowerLevelFfi {
        use matrix_sdk::ruma::events::room::power_levels::UserPowerLevel;

        fn defaults() -> crate::ffi::RoomOwnPowerLevelFfi {
            crate::ffi::RoomOwnPowerLevelFfi {
                level: 0,
                has_explicit_override: false,
            }
        }
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return defaults();
        };
        let Ok((_, room)) = require_room(client, room_id) else {
            return defaults();
        };
        let Some(user_id) = client.user_id() else {
            return defaults();
        };
        let Ok(pl) = self.rt.block_on(room.power_levels()) else {
            return defaults();
        };
        let (level, is_infinite) = match pl.for_user(user_id) {
            UserPowerLevel::Infinite => (i64::MAX, true),
            UserPowerLevel::Int(v) => (i64::from(v), false),
            // #[non_exhaustive] enum — treat any future variant as the
            // safest fallback: fixed and maximally privileged, same as
            // Infinite, rather than silently reading as an unprivileged 0.
            _ => (i64::MAX, true),
        };
        let has_explicit_override = is_infinite || pl.users.contains_key(user_id);
        crate::ffi::RoomOwnPowerLevelFfi {
            level,
            has_explicit_override,
        }
    }

    #[cfg(test)]
    pub fn room_own_power_level(&self, _room_id: &str) -> crate::ffi::RoomOwnPowerLevelFfi {
        crate::ffi::RoomOwnPowerLevelFfi::default()
    }

    /// Fetch the room's current m.room.encryption/m.room.join_rules/
    /// m.room.guest_access/m.room.history_visibility state directly from the
    /// homeserver via GET /rooms/{id}/state, bypassing the local sync cache
    /// entirely. Needed because guest_access is never delivered via sliding
    /// sync (absent from matrix-sdk-ui's hardcoded required_state list, with
    /// no app-level hook to add it — confirmed by inspecting the vendored
    /// crate source) and because join_rules/encryption/history_visibility
    /// are subject to a separate update-notification filter that can leave
    /// the locally cached values stale after a state-event write. Mirrors
    /// the same on-demand-fetch precedent already used for bridge detection
    /// (start_bridge_status_check, backfill.rs) for exactly this class of
    /// problem — state that sync doesn't reliably keep fresh.
    ///
    /// Async — does not block. Calls back via on_room_security_state_ready
    /// with a RoomSecurityStateFfi — fields default to the MSC-safe defaults
    /// (mirroring build_room_info's own fallback shapes) if the
    /// corresponding event is missing or the fetch fails outright.
    #[cfg(not(test))]
    pub fn fetch_room_security_state_async(&self, request_id: u64, room_id: &str) {
        fn defaults() -> crate::ffi::RoomSecurityStateFfi {
            crate::ffi::RoomSecurityStateFfi {
                is_encrypted: false,
                join_rule: String::new(),
                guest_access: false,
                history_visibility: "shared".to_string(),
            }
        }
        let Some(client) = self.client.clone() else {
            if let Some(ref h) = self.handler {
                h.lock().on_room_security_state_ready(request_id, &defaults());
            }
            return;
        };
        let Ok(rid) = matrix_sdk::ruma::RoomId::parse(room_id) else {
            if let Some(ref h) = self.handler {
                h.lock().on_room_security_state_ready(request_id, &defaults());
            }
            return;
        };
        let handler = self.handler.clone();
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = self.in_flight_urls.clone();
        self.rt.spawn(async move {
            use matrix_sdk::ruma::api::client::state::get_state_events::v3 as state_api;

            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler,
                #[cfg(debug_assertions)]
                &in_flight_urls,
                #[cfg(debug_assertions)]
                "room_list/fetch_room_security_state".to_string(),
            );

            let mut is_encrypted = false;
            let mut join_rule = String::new();
            let mut guest_access = false;
            let mut history_visibility = "shared".to_string();

            if let Ok(resp) = client.send(state_api::Request::new(rid)).await {
                for raw in &resp.room_state {
                    let Ok(Some(ty)) = raw.get_field::<String>("type") else {
                        continue;
                    };
                    match ty.as_str() {
                        // GET /state only lists an event type if it is the
                        // room's CURRENT state — presence alone means encryption
                        // is on (there is no valid "off" content for this type).
                        "m.room.encryption" => is_encrypted = true,
                        "m.room.join_rules" => {
                            if let Ok(Some(content)) =
                                raw.get_field::<serde_json::Value>("content")
                            {
                                join_rule = content
                                    .get("join_rule")
                                    .and_then(|v| v.as_str())
                                    .unwrap_or_default()
                                    .to_string();
                            }
                        }
                        "m.room.guest_access" => {
                            if let Ok(Some(content)) =
                                raw.get_field::<serde_json::Value>("content")
                            {
                                guest_access = content.get("guest_access").and_then(|v| v.as_str())
                                    == Some("can_join");
                            }
                        }
                        "m.room.history_visibility" => {
                            if let Ok(Some(content)) =
                                raw.get_field::<serde_json::Value>("content")
                            {
                                history_visibility = content
                                    .get("history_visibility")
                                    .and_then(|v| v.as_str())
                                    .unwrap_or("shared")
                                    .to_string();
                            }
                        }
                        _ => {}
                    }
                }
            }

            let state = crate::ffi::RoomSecurityStateFfi {
                is_encrypted,
                join_rule,
                guest_access,
                history_visibility,
            };

            if let Some(h) = handler {
                h.lock().on_room_security_state_ready(request_id, &state);
            }
        });
    }

    #[cfg(test)]
    pub fn fetch_room_security_state_async(&self, _request_id: u64, _room_id: &str) {}

    /// Set the current user's display name in a specific room (m.room.member
    /// state event). Preserves all other existing member event fields. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_user_room_display_name(&self, room_id: &str, name: &str) -> OpResult {
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
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_display_name".to_string(),
        );

        let mut content = match self.rt.block_on(room.get_member(user_id)) {
            Ok(Some(m)) => match m.event().as_sync() {
                Some(e) => e.as_original().map(|o| o.content.clone()),
                None => None,
            }
            .unwrap_or_else(|| RoomMemberEventContent::new(MembershipState::Join)),
            _ => RoomMemberEventContent::new(MembershipState::Join),
        };

        content.displayname = if name.is_empty() {
            None
        } else {
            Some(name.to_owned())
        };

        match self
            .rt
            .block_on(room.send_state_event_for_key(user_id, content))
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_user_room_display_name(&self, _room_id: &str, _name: &str) -> OpResult {
        err("not logged in")
    }

    /// Set the current user's avatar in a specific room (m.room.member state
    /// event). Preserves all other existing member event fields. Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_user_room_avatar(&self, room_id: &str, mxc_uri: &str) -> OpResult {
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
            Ok(u) => u,
            Err(_) => return err("invalid mxc URI"),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_avatar".to_string(),
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

        match self
            .rt
            .block_on(room.send_state_event_for_key(user_id, content))
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_user_room_avatar(&self, _room_id: &str, _mxc_uri: &str) -> OpResult {
        err("not logged in")
    }

    /// Send an m.room.name state event to set the room's own display name
    /// (visible to all members) — distinct from set_user_room_display_name,
    /// which only sets the current user's per-room member override. Blocks —
    /// worker thread.
    #[cfg(not(test))]
    pub fn set_room_display_name(&self, room_id: &str, name: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::events::room::name::RoomNameEventContent;

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let content = RoomNameEventContent::new(name.to_owned());
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_name".to_string(),
        );
        match self.rt.block_on(room.send_state_event(content)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_display_name(&self, _room_id: &str, _name: &str) -> OpResult {
        err("not logged in")
    }

    /// Set or clear the room's m.room.avatar state event (visible to all
    /// members) — distinct from set_user_room_avatar, which only sets the
    /// current user's per-room member override. Pass an empty mxc_uri to
    /// clear the room avatar (sends an empty `{}` content object rather than
    /// `{"url":null}` — RoomAvatarEventContent::url has no
    /// skip_serializing_if, so a literal `None` would serialize an explicit
    /// null, which some homeservers may reject). Blocks — worker thread.
    #[cfg(not(test))]
    pub fn set_room_avatar(&self, room_id: &str, mxc_uri: &str) -> OpResult {
        let _enter = self.rt.enter();
        use matrix_sdk::ruma::{events::room::avatar::RoomAvatarEventContent, OwnedMxcUri};

        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)]
            &self.in_flight_urls,
            #[cfg(debug_assertions)]
            "room_list/set_room_avatar".to_string(),
        );

        let result = if mxc_uri.is_empty() {
            self.rt
                .block_on(room.send_state_event_raw(
                    "m.room.avatar",
                    "",
                    serde_json::json!({}),
                ))
                .map(|_| ())
        } else {
            let mxc: OwnedMxcUri = match mxc_uri.try_into() {
                Ok(u) => u,
                Err(_) => return err("invalid mxc URI"),
            };
            // RoomAvatarEventContent is #[non_exhaustive] outside its
            // defining crate — build via new() then set the field, rather
            // than a struct literal (which would not compile here).
            let mut content = RoomAvatarEventContent::new();
            content.url = Some(mxc);
            self.rt.block_on(room.send_state_event(content)).map(|_| ())
        };

        match result {
            Ok(()) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn set_room_avatar(&self, _room_id: &str, _mxc_uri: &str) -> OpResult {
        err("not logged in")
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.name in this room. Cached read — no network
    /// round-trip. False on any uncertainty. Mirrors can_pin_in_room
    /// (pins.rs).
    #[cfg(not(test))]
    pub fn can_set_room_name(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomName),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_name(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.topic in this room. Cached read — no network
    /// round-trip. False on any uncertainty. Mirrors can_pin_in_room
    /// (pins.rs).
    #[cfg(not(test))]
    pub fn can_set_room_topic(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomTopic),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_topic(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.avatar in this room. Cached read — no network
    /// round-trip. False on any uncertainty. Mirrors can_pin_in_room
    /// (pins.rs).
    #[cfg(not(test))]
    pub fn can_set_room_avatar(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomAvatar),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_avatar(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.encryption in this room. Cached read — no network
    /// round-trip. False on any uncertainty. Mirrors can_set_room_name.
    #[cfg(not(test))]
    pub fn can_set_room_encryption(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomEncryption),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_encryption(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.join_rules in this room. Cached read — no network
    /// round-trip. False on any uncertainty. Mirrors can_set_room_name.
    #[cfg(not(test))]
    pub fn can_set_room_join_rules(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomJoinRules),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_join_rules(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.guest_access in this room. Cached read — no network
    /// round-trip. False on any uncertainty. Mirrors can_set_room_name.
    #[cfg(not(test))]
    pub fn can_set_room_guest_access(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomGuestAccess),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_guest_access(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.history_visibility in this room. Cached read — no
    /// network round-trip. False on any uncertainty. Mirrors can_set_room_name.
    #[cfg(not(test))]
    pub fn can_set_room_history_visibility(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomHistoryVisibility),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_history_visibility(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending m.room.power_levels in this room — the single all-or-nothing
    /// gate for the whole Permissions tab (Matrix has no finer granularity
    /// than this; the server itself rejects attempts to grant a level above
    /// the sender's own). Cached read — no network round-trip. False on any
    /// uncertainty. Mirrors can_set_room_name.
    #[cfg(not(test))]
    pub fn can_set_room_power_levels(&self, room_id: &str) -> bool {
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
            Ok(pl) => pl.user_can_send_state(user_id, StateEventType::RoomPowerLevels),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_power_levels(&self, _room_id: &str) -> bool {
        false
    }

    /// True iff the current user's power level meets the requirement for
    /// sending either the stable or unstable MSC2545 room image-pack state
    /// event type in this room (see image_packs::ROOM_PACK_TYPES — a room's
    /// packs may live under either type, so permission to send *either* is
    /// enough to edit them). Cached read — no network round-trip. False on
    /// any uncertainty. Mirrors can_set_room_name.
    #[cfg(not(test))]
    pub fn can_set_room_image_packs(&self, room_id: &str) -> bool {
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
            Ok(pl) => crate::image_packs::ROOM_PACK_TYPES
                .iter()
                .any(|t| pl.user_can_send_state(user_id, StateEventType::from(*t))),
            Err(_) => false,
        }
    }

    #[cfg(test)]
    pub fn can_set_room_image_packs(&self, _room_id: &str) -> bool {
        false
    }
}

// ---------------------------------------------------------------------------
// Tests (content-shape only — state-event sends require a live homeserver).
// Mirrors the style of pins.rs's `mod tests`.
// ---------------------------------------------------------------------------

#[cfg(test)]
mod room_settings_tests {
    use matrix_sdk::ruma::events::room::{
        avatar::RoomAvatarEventContent, name::RoomNameEventContent,
    };

    #[test]
    fn room_name_content_serializes_name() {
        let c = RoomNameEventContent::new("My Room".to_owned());
        let v = serde_json::to_value(&c).unwrap();
        assert_eq!(v["name"], "My Room");
    }

    #[test]
    fn clear_avatar_content_has_no_url_key() {
        // Mirrors what set_room_avatar sends for an empty mxc_uri: a bare
        // `{}` rather than `{"url": null}`.
        let v = serde_json::json!({});
        assert!(v.get("url").is_none());
    }

    #[test]
    fn set_avatar_content_serializes_url() {
        let mxc: matrix_sdk::ruma::OwnedMxcUri = "mxc://example.org/abc".into();
        let mut c = RoomAvatarEventContent::new();
        c.url = Some(mxc);
        let v = serde_json::to_value(&c).unwrap();
        assert_eq!(v["url"], "mxc://example.org/abc");
        // info was never set — must be omitted, not null, per its
        // skip_serializing_if.
        assert!(v.get("info").is_none());
    }
}

#[cfg(test)]
mod space_summary_task_tests {
    use super::{abort_space_summary_group, register_space_summary_task, SpaceSummaryTasks};

    fn new_map() -> SpaceSummaryTasks {
        std::sync::Arc::new(parking_lot::Mutex::new(std::collections::HashMap::new()))
    }

    #[tokio::test]
    async fn abort_group_aborts_only_that_spaces_tasks() {
        let map = new_map();
        let rt = tokio::runtime::Handle::current();

        let a1 = rt.spawn(std::future::pending::<()>());
        let a2 = rt.spawn(std::future::pending::<()>());
        let b1 = rt.spawn(std::future::pending::<()>());
        register_space_summary_task(&map, "space-a".to_string(), 1, a1.abort_handle());
        register_space_summary_task(&map, "space-a".to_string(), 2, a2.abort_handle());
        register_space_summary_task(&map, "space-b".to_string(), 3, b1.abort_handle());

        abort_space_summary_group(&map, "space-a");

        assert!(a1.await.unwrap_err().is_cancelled());
        assert!(a2.await.unwrap_err().is_cancelled());
        assert!(map.lock().get("space-a").is_none());

        // space-b's task is untouched.
        assert!(map.lock().get("space-b").is_some());
        b1.abort();
    }

    #[tokio::test]
    async fn register_prunes_already_finished_handles() {
        let map = new_map();
        let rt = tokio::runtime::Handle::current();

        let finished = rt.spawn(async {});
        let finished_abort = finished.abort_handle();
        finished.await.unwrap();
        register_space_summary_task(&map, "space-a".to_string(), 1, finished_abort);
        assert_eq!(map.lock().get("space-a").unwrap().len(), 1);

        // `finished`'s entry is already done, so registering a second task
        // under the same space should prune it rather than accumulate it.
        let live = rt.spawn(std::future::pending::<()>());
        register_space_summary_task(&map, "space-a".to_string(), 2, live.abort_handle());
        {
            let m = map.lock();
            let v = m.get("space-a").unwrap();
            assert_eq!(v.len(), 1);
            assert_eq!(v[0].0, 2);
        }

        abort_space_summary_group(&map, "space-a");
        assert!(live.await.unwrap_err().is_cancelled());
    }
}
