use std::path::PathBuf;

use anyhow::Context as _;
use serde::{Deserialize, Serialize};
use matrix_sdk::{
    authentication::oauth::{ClientId, OAuthSession, UserSession},
    ruma::{
        events::room::message::RoomMessageEventContent,
        OwnedRoomId,
    },
    store::RoomLoadSettings,
    Client,
};
use tokio::runtime::Runtime;
use tokio::sync::watch;

use crate::ffi::{OAuthBegin, OpResult};
use crate::oauth;

#[cfg(not(test))]
use std::collections::HashMap;
#[cfg(not(test))]
use std::sync::{Arc, Mutex};
#[cfg(not(test))]
use cxx::UniquePtr;
#[cfg(not(test))]
use matrix_sdk::SessionChange;
#[cfg(not(test))]
use matrix_sdk_ui::{
    eyeball_im::VectorDiff,
    sync_service::SyncService,
    timeline::{MsgLikeContent, MsgLikeKind, RoomExt, TimelineDetails, TimelineItem, TimelineItemContent, TimelineItemKind},
};
#[cfg(not(test))]
use futures_util::StreamExt;
#[cfg(not(test))]
use crate::ffi::{EventHandlerBridge, TimelineEvent};

// ---------------------------------------------------------------------------

fn ok(msg: impl Into<String>) -> OpResult {
    OpResult { ok: true, message: msg.into() }
}

fn err(msg: impl Into<String>) -> OpResult {
    OpResult { ok: false, message: msg.into() }
}

fn oauth_err(msg: impl Into<String>) -> OAuthBegin {
    OAuthBegin {
        ok:           false,
        message:      msg.into(),
        auth_url:     String::new(),
        redirect_uri: String::new(),
    }
}

fn data_dir() -> PathBuf {
    let base = dirs_like_home().unwrap_or_else(std::env::temp_dir);
    let dir  = base.join("tesseract").join("matrix-store");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

fn dirs_like_home() -> Option<PathBuf> {
    #[cfg(target_os = "windows")]
    { std::env::var_os("APPDATA").map(PathBuf::from) }
    #[cfg(target_os = "macos")]
    {
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .map(|h| h.join("Library").join("Application Support"))
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
    }
}

// ---------------------------------------------------------------------------

#[cfg(not(test))]
struct SendHandler(UniquePtr<EventHandlerBridge>);
#[cfg(not(test))]
unsafe impl Send for SendHandler {}
#[cfg(not(test))]
impl std::ops::Deref for SendHandler {
    type Target = UniquePtr<EventHandlerBridge>;
    fn deref(&self) -> &Self::Target { &self.0 }
}

#[cfg(not(test))]
struct TimelineHandle {
    timeline:    Arc<matrix_sdk_ui::Timeline>,
    abort_tasks: Vec<tokio::task::AbortHandle>,
}

/// Serialisable wrapper for a full OAuth session (client_id + user session).
#[derive(Serialize, Deserialize)]
struct PersistedSession {
    client_id: ClientId,
    #[serde(flatten)]
    user: UserSession,
}

pub struct ClientFfi {
    client:     Option<Client>,
    stop_tx:    Option<watch::Sender<bool>>,
    oauth_flow: Option<oauth::PendingFlow>,
    #[cfg(not(test))]
    handler:      Option<Arc<Mutex<SendHandler>>>,
    #[cfg(not(test))]
    sync_service: Option<Arc<SyncService>>,
    #[cfg(not(test))]
    timelines:    HashMap<OwnedRoomId, TimelineHandle>,
    // Declared last so it drops after all SDK resources; deadpool/SQLite cleanup
    // uses tokio primitives and requires the runtime to still be alive.
    rt:         Runtime,
}

impl Drop for ClientFfi {
    fn drop(&mut self) {
        self.stop_sync();
        #[cfg(not(test))]
        for (_, th) in self.timelines.drain() {
            for h in th.abort_tasks { h.abort(); }
        }
        // Field drops proceed: client → … → rt (runtime drops last)
    }
}

impl ClientFfi {
    pub fn new() -> Self {
        let _ = tracing_subscriber::fmt()
            .with_env_filter(
                tracing_subscriber::EnvFilter::from_default_env()
                    .add_directive("matrix_sdk=info".parse().unwrap()),
            )
            .try_init();

        Self {
            client:     None,
            stop_tx:    None,
            oauth_flow: None,
            #[cfg(not(test))]
            handler:      None,
            #[cfg(not(test))]
            sync_service: None,
            #[cfg(not(test))]
            timelines:    HashMap::new(),
            rt:         Runtime::new().expect("tokio runtime"),
        }
    }

    // -----------------------------------------------------------------------
    // OAuth login
    // -----------------------------------------------------------------------

    pub fn oauth_begin(&mut self, homeserver: &str) -> OAuthBegin {
        if let Some(prev) = self.oauth_flow.take() {
            oauth::cancel(&prev);
        }

        let hs   = homeserver.to_owned();
        let path = data_dir();

        let _ = std::fs::remove_dir_all(&path);

        match self.rt.block_on(oauth::begin(&hs, &path)) {
            Ok(begin) => {
                let auth_url     = begin.auth_url;
                let redirect_uri = begin.redirect_uri;
                self.oauth_flow  = Some(begin.flow);
                OAuthBegin { ok: true, message: String::new(), auth_url, redirect_uri }
            }
            Err(e) => oauth_err(e.to_string()),
        }
    }

    pub fn oauth_await_callback(&mut self) -> OpResult {
        let Some(flow) = self.oauth_flow.take() else {
            return err("no oauth flow in progress; call oauth_begin first");
        };

        match self.rt.block_on(oauth::await_callback(flow)) {
            Ok(client) => { self.client = Some(client); ok("") }
            Err(e)     => err(format!("{e:#}")),
        }
    }

    pub fn oauth_cancel(&mut self) {
        if let Some(flow) = self.oauth_flow.take() {
            oauth::cancel(&flow);
        }
    }

    // -----------------------------------------------------------------------
    // Session persistence
    // -----------------------------------------------------------------------

    pub fn restore_session(&mut self, session_json: &str) -> OpResult {
        let persisted: PersistedSession = match serde_json::from_str(session_json) {
            Ok(s)  => s,
            Err(e) => return err(format!("parse session JSON: {e}")),
        };

        let homeserver = persisted.user.meta.user_id.server_name().to_string();
        let path       = data_dir();

        let result = self.rt.block_on(async move {
            let client = Client::builder()
                .server_name_or_homeserver_url(homeserver)
                .sqlite_store(&path, None)
                .build()
                .await
                .context("build client")?;

            let session = OAuthSession { client_id: persisted.client_id, user: persisted.user };
            client
                .oauth()
                .restore_session(session, RoomLoadSettings::default())
                .await
                .context("restore oauth session")?;

            anyhow::Ok(client)
        });

        match result {
            Ok(c)  => { self.client = Some(c); ok("") }
            Err(e) => err(e.to_string()),
        }
    }

    pub fn export_session(&self) -> String {
        let Some(client) = &self.client else { return String::new() };
        let Some(session) = client.oauth().full_session() else { return String::new() };
        let persisted = PersistedSession { client_id: session.client_id, user: session.user };
        serde_json::to_string(&persisted).unwrap_or_default()
    }

    // -----------------------------------------------------------------------
    // Sync loop (Step 2: SyncService + RoomListService)
    // -----------------------------------------------------------------------

    #[cfg(not(test))]
    pub fn start_sync(&mut self, handler: UniquePtr<EventHandlerBridge>) {
        let Some(client) = self.client.clone() else { return };

        let (stop_tx, stop_rx) = watch::channel(false);
        let stop_tx_auth = stop_tx.clone();
        self.stop_tx = Some(stop_tx);

        let handler = Arc::new(Mutex::new(SendHandler(handler)));
        self.handler = Some(Arc::clone(&handler));

        // Subscribe the event cache before SyncService starts so every sync
        // response is persisted to SQLite from the very first update.
        // subscribe() is synchronous but internally calls tokio::task::spawn,
        // so it must be called within the runtime context.
        let _rt_guard = self.rt.enter();
        if let Err(e) = client.event_cache().subscribe() {
            tracing::error!("event cache subscribe failed: {e}");
            if let Ok(guard) = handler.lock() {
                guard.on_error("event_cache_init", &e.to_string(), false);
            }
            return;
        }
        drop(_rt_guard);

        // Session refresh watcher.
        {
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            self.rt.spawn(async move {
                let mut changes = client_clone.subscribe_to_session_changes();
                loop {
                    match changes.recv().await {
                        Ok(SessionChange::TokensRefreshed) => {
                            let Some(full) = client_clone.oauth().full_session() else { continue };
                            let persisted = PersistedSession {
                                client_id: full.client_id,
                                user:      full.user,
                            };
                            let Ok(json) = serde_json::to_string(&persisted) else { continue };
                            if let Ok(guard) = h.lock() {
                                guard.on_session_refreshed(&json);
                            }
                        }
                        Ok(SessionChange::UnknownToken { soft_logout }) => {
                            // Stop SyncService before it can reach State::Error and
                            // wipe the SQLite data directory while a fresh login is
                            // already in progress.
                            let _ = stop_tx_auth.send(true);
                            if let Ok(guard) = h.lock() {
                                guard.on_error(
                                    "sync_auth_error",
                                    "Session token is no longer valid; please log in again.",
                                    soft_logout,
                                );
                            }
                            break;
                        }
                        Err(_) => break,
                    }
                }
            });
        }

        // Build SyncService.
        let sync_service = match self.rt.block_on(
            SyncService::builder(client.clone()).build()
        ) {
            Ok(s) => Arc::new(s),
            Err(e) => {
                if let Ok(guard) = handler.lock() {
                    guard.on_error("sync_init", &e.to_string(), false);
                }
                return;
            }
        };
        self.sync_service = Some(Arc::clone(&sync_service));

        // Room info watcher: re-emits the full room list on every notable room update.
        {
            let h                 = Arc::clone(&handler);
            let client_clone      = client.clone();
            let mut notable_rx    = client.room_info_notable_update_receiver();
            let mut stop_rx_rooms = stop_rx.clone();

            self.rt.spawn(async move {
                loop {
                    tokio::select! {
                        _ = stop_rx_rooms.changed() => {
                            if *stop_rx_rooms.borrow() { break; }
                        }
                        result = notable_rx.recv() => {
                            use tokio::sync::broadcast::error::RecvError;
                            match result {
                                Ok(_)                      => {}
                                Err(RecvError::Lagged(_))  => {}
                                Err(RecvError::Closed)     => break,
                            }
                            let rooms = build_room_infos(&client_clone).await;
                            if let Ok(guard) = h.lock() {
                                guard.on_rooms_updated(&rooms);
                            }
                        }
                    }
                }
            });
        }

        // Start SyncService and monitor state.
        let svc_clone      = Arc::clone(&sync_service);
        let h_state        = Arc::clone(&handler);
        let mut stop_rx_sv = stop_rx.clone();

        self.rt.spawn(async move {
            svc_clone.start().await;

            use matrix_sdk_ui::sync_service::State as SyncServiceState;
            let mut state_stream = svc_clone.state();

            loop {
                tokio::select! {
                    _ = stop_rx_sv.changed() => {
                        if *stop_rx_sv.borrow() {
                            let _ = svc_clone.stop().await;
                            break;
                        }
                    }
                    Some(state) = state_stream.next() => {
                        if matches!(state, SyncServiceState::Error(_)) {
                            // Clear the local SQLite store — the most common
                            // cause of State::Error is a stale to_device.since
                            // token accumulated across SDK upgrades or server
                            // migrations.  Deleting the store lets the next
                            // restore_session() start from scratch.
                            let _ = std::fs::remove_dir_all(data_dir());
                            if let Ok(guard) = h_state.lock() {
                                guard.on_error(
                                    "sync_reconnect",
                                    "Sync state corrupted; reconnecting automatically…",
                                    false,
                                );
                            }
                            let _ = svc_clone.stop().await;
                            break;
                        }
                    }
                }
            }
        });
    }

    pub fn stop_sync(&mut self) {
        if let Some(tx) = self.stop_tx.take() {
            let _ = tx.send(true);
        }
        #[cfg(not(test))]
        if let Some(svc) = self.sync_service.take() {
            self.rt.block_on(async move { let _ = svc.stop().await; });
        }
    }

    // -----------------------------------------------------------------------
    // Timeline subscription (Step 2)
    // -----------------------------------------------------------------------

    #[cfg(not(test))]
    pub fn subscribe_room(&mut self, room_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let Some(handler) = self.handler.clone() else { return err("sync not started") };

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };

        // Drop any previous subscription for this room.
        if let Some(prev) = self.timelines.remove(&room_id) {
            for h in prev.abort_tasks { h.abort(); }
        }

        let Some(room) = client.get_room(&room_id) else { return err("room not found") };

        let timeline = match self.rt.block_on(room.timeline()) {
            Ok(t)  => Arc::new(t),
            Err(e) => return err(format!("build timeline: {e}")),
        };

        let room_id_str = room_id.to_string();

        // Notify the UI to clear the message view for this room.
        if let Ok(guard) = handler.lock() {
            guard.on_timeline_reset(&room_id_str);
        }

        // Spawn a task that streams timeline items to the UI.
        let tl    = Arc::clone(&timeline);
        let h     = Arc::clone(&handler);
        let rid   = room_id_str.clone();

        let abort = self.rt.spawn(async move {
            let (initial_items, mut stream) = tl.subscribe().await;

            for item in initial_items.iter() {
                if let Some(ev) = timeline_item_to_ffi(item, &rid) {
                    if let Ok(guard) = h.lock() {
                        guard.on_message_event(&ev);
                    }
                }
            }

            while let Some(diffs) = stream.next().await {
                for diff in diffs {
                    handle_timeline_diff(diff, &h, &rid);
                }
            }
        }).abort_handle();

        self.timelines.insert(room_id, TimelineHandle {
            timeline,
            abort_tasks: vec![abort],
        });

        ok("")
    }

    #[cfg(not(test))]
    pub fn unsubscribe_room(&mut self, room_id: &str) {
        if let Ok(id) = room_id.parse::<OwnedRoomId>() {
            if let Some(h) = self.timelines.remove(&id) {
                for abort in h.abort_tasks { abort.abort(); }
            }
        }
    }

    #[cfg(not(test))]
    pub fn paginate_back(&mut self, room_id: &str, count: u16) -> OpResult {
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };

        let Some(handle) = self.timelines.get(&room_id) else {
            return err("room not subscribed; call subscribe_room first");
        };

        let tl = Arc::clone(&handle.timeline);
        match self.rt.block_on(tl.paginate_backwards(count)) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    // -----------------------------------------------------------------------
    // Messaging
    // -----------------------------------------------------------------------

    pub fn send_message(&mut self, room_id: &str, body: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let content = RoomMessageEventContent::text_plain(body);
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    pub fn user_id(&self) -> String {
        self.client
            .as_ref()
            .and_then(|c| c.user_id())
            .map(|id| id.to_string())
            .unwrap_or_default()
    }

    pub fn list_rooms(&self) -> Vec<crate::ffi::RoomInfo> {
        let Some(client) = self.client.clone() else { return Vec::new() };
        self.rt.block_on(build_room_infos(&client))
    }

    pub fn space_children(&self, space_id: &str) -> Vec<String> {
        let Some(client) = self.client.as_ref() else { return vec![]; };
        let Ok(room_id) = OwnedRoomId::try_from(space_id) else { return vec![]; };
        let Some(space_room) = client.get_room(&room_id) else { return vec![]; };
        let client = client.clone();

        self.rt.block_on(async move {
            use matrix_sdk::deserialized_responses::SyncOrStrippedState;
            use matrix_sdk::ruma::events::SyncStateEvent;
            use matrix_sdk::ruma::events::space::child::SpaceChildEventContent;

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
                        Some(e.state_key.to_owned())
                    }
                    Ok(SyncOrStrippedState::Sync(SyncStateEvent::Redacted(_))) => None,
                    Ok(SyncOrStrippedState::Stripped(e)) => Some(e.state_key.to_owned()),
                    Err(_) => None,
                })
                .filter(|child_id| client.get_room(child_id).is_some())
                .map(|id| id.to_string())
                .collect()
        })
    }

    pub fn fetch_avatar_bytes(&mut self, room_id: &str) -> Vec<u8> {
        use matrix_sdk::media::MediaFormat;
        let Some(client) = self.client.clone() else { return Vec::new() };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return Vec::new(),
        };
        let Some(room) = client.get_room(&room_id) else { return Vec::new() };
        self.rt
            .block_on(room.avatar(MediaFormat::File))
            .ok()
            .flatten()
            .unwrap_or_default()
    }

    pub fn fetch_media_bytes(&mut self, mxc_url: &str) -> Vec<u8> {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;
        let Some(client) = self.client.clone() else { return Vec::new() };
        let uri = OwnedMxcUri::from(mxc_url);
        if !uri.is_valid() { return Vec::new(); }
        let request = MediaRequestParameters {
            source: MediaSource::Plain(uri.into()),
            format: MediaFormat::File,
        };
        self.rt
            .block_on(client.media().get_media_content(&request, true))
            .unwrap_or_default()
    }

pub fn fetch_source_bytes(&mut self, mxc_url: &str) -> Vec<u8> {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else { return Vec::new() };
        if mxc_url.is_empty() { return Vec::new(); }

        let uri = OwnedMxcUri::from(mxc_url);
        if !uri.is_valid() { return Vec::new(); }

        let request = MediaRequestParameters {
            source: MediaSource::Plain(uri.into()),
            format: MediaFormat::File,
        };
self.rt
            .block_on(client.media().get_media_content(&request, true))
            .unwrap_or_default()
    }

    // -----------------------------------------------------------------------
    // Logout
    // -----------------------------------------------------------------------

    pub fn logout(&mut self) -> OpResult {
        self.stop_sync();

        if let Some(flow) = self.oauth_flow.take() {
            oauth::cancel(&flow);
        }

        #[cfg(not(test))]
        { self.timelines.clear(); }

        let Some(client) = self.client.take() else {
            let _ = std::fs::remove_dir_all(data_dir());
            return ok("");
        };

        let revoke = self.rt.block_on(async move {
            client.oauth().logout().await
        });

        let _ = std::fs::remove_dir_all(data_dir());

        match revoke {
            Ok(_)  => ok(""),
            Err(e) => err(format!("oauth logout failed (local store cleared): {e}")),
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

async fn build_room_infos(client: &Client) -> Vec<crate::ffi::RoomInfo> {
    let mut result = Vec::new();
    for room in client.joined_rooms() {
        if room.is_tombstoned() {
            continue;
        }
        let name = room
            .display_name()
            .await
            .map(|n| n.to_string())
            .unwrap_or_else(|_| room.room_id().to_string());
        let is_space = room.is_space();
        result.push(crate::ffi::RoomInfo {
            id:                room.room_id().to_string(),
            name,
            topic:             room.topic().unwrap_or_default(),
            unread_count:      room.unread_notification_counts().notification_count,
            is_direct:         room.is_direct().await.unwrap_or(false),
            avatar_url:        room.avatar_url()
                                   .map(|u| u.to_string())
                                   .unwrap_or_default(),
            last_message_body: String::new(),
            last_activity_ts:  0,
            is_space,
        });
    }
    result
}

#[cfg(not(test))]
fn timeline_item_to_ffi(
    item: &Arc<TimelineItem>,
    room_id: &str,
) -> Option<TimelineEvent> {
    use matrix_sdk::ruma::events::room::message::MessageType;

    let event_item = match item.kind() {
        TimelineItemKind::Event(e) => e,
        _ => return None,
    };

    // Sticker events are MsgLikeKind::Sticker, not MsgLikeKind::Message.
    // Handle them before falling through to the message-only path.
    if let TimelineItemContent::MsgLike(MsgLikeContent { kind: MsgLikeKind::Sticker(s), .. }) =
        event_item.content()
    {
        let c = s.content();
        let src = match &c.source {
            matrix_sdk::ruma::events::sticker::StickerMediaSource::Plain(uri) => uri.to_string(),
            _ => String::new(),
        };
        let w = c.info.width.map(u64::from).unwrap_or(0);
        let h = c.info.height.map(u64::from).unwrap_or(0);
        let (sender_name, sender_avatar_url) =
            if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                (
                    p.display_name.clone().unwrap_or_default(),
                    p.avatar_url.as_ref().map(|u| u.to_string()).unwrap_or_default(),
                )
            } else {
                (String::new(), String::new())
            };
        return Some(TimelineEvent {
            event_id:          event_item.event_id().map(|id| id.to_string()).unwrap_or_default(),
            room_id:           room_id.to_owned(),
            sender:            event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            body:              c.body.clone(),
            timestamp:         event_item.timestamp().get().into(),
            msg_type:          "m.sticker".to_owned(),
            source_json:       src,
            width:             w,
            height:            h,
            file_json:         String::new(),
            file_name:         String::new(),
            file_size:         0u64,
            image_filename:    String::new(),
        });
    }

    let msg_content = match event_item.content() {
        TimelineItemContent::MsgLike(MsgLikeContent { kind: MsgLikeKind::Message(msg), .. }) => msg,
        _ => return None,
    };

    let (body, msg_type, source_json, width, height, file_json, file_name, file_size, image_filename) =
        match msg_content.msgtype() {
            MessageType::Text(t) => (
                t.body.clone(),
                "m.text".to_owned(),
                String::new(),
                0u64,
                0u64,
                String::new(),
                String::new(),
                0u64,
                String::new(),
            ),
            MessageType::Image(i) => {
                let source_str = match &i.source {
                    matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => uri.to_string(),
                    matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) => String::new(),
                };
                let (w, h) = i
                    .info
                    .as_ref()
                    .map(|info| (
                        info.width.map(|v| u64::from(v)).unwrap_or(0u64),
                        info.height.map(|v| u64::from(v)).unwrap_or(0u64),
                    ))
                    .unwrap_or((0u64, 0u64));
                // MSC2530: filename field signals that body is a user caption.
                let img_filename = i.filename.clone().unwrap_or_default();
                (i.body.clone(), "m.image".to_owned(), source_str, w, h,
                 String::new(), String::new(), 0u64, img_filename)
            }
            MessageType::File(f) => {
                let file_str = match &f.source {
                    matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => uri.to_string(),
                    matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) => String::new(),
                };
                let name = f.filename.clone().unwrap_or_else(|| f.body.clone());
                let size = f
                    .info
                    .as_ref()
                    .and_then(|info| info.size)
                    .map(|v| u64::from(v))
                    .unwrap_or(0u64);
                (f.body.clone(), "m.file".to_owned(), String::new(), 0u64, 0u64,
                 file_str, name, size, String::new())
            }
            _ => return None,
        };

    let (sender_name, sender_avatar_url) =
        if let TimelineDetails::Ready(p) = event_item.sender_profile() {
            (
                p.display_name.clone().unwrap_or_default(),
                p.avatar_url.as_ref().map(|u| u.to_string()).unwrap_or_default(),
            )
        } else {
            (String::new(), String::new())
        };

    Some(TimelineEvent {
        event_id:          event_item.event_id()
            .map(|id| id.to_string())
            .unwrap_or_default(),
        room_id:           room_id.to_owned(),
        sender:            event_item.sender().to_string(),
        sender_name,
        sender_avatar_url,
        body,
        timestamp:         event_item.timestamp().get().into(),
        msg_type,
        source_json,
        width,
        height,
        file_json,
        file_name,
        file_size,
        image_filename,
    })
}

#[cfg(not(test))]
fn handle_timeline_diff(
    diff: VectorDiff<Arc<TimelineItem>>,
    handler: &Arc<Mutex<SendHandler>>,
    room_id: &str,
) {
    let (reset, items): (bool, Vec<Arc<TimelineItem>>) = match diff {
        VectorDiff::Append    { values }    => (false, values.into_iter().collect()),
        VectorDiff::PushBack  { value }     => (false, vec![value]),
        VectorDiff::PushFront { value }     => (false, vec![value]),
        VectorDiff::Insert    { value, .. } => (false, vec![value]),
        VectorDiff::Set       { value, .. } => (false, vec![value]),
        // Reset rebuilds the full timeline; clear the UI first to avoid
        // appending to the already-displayed items.
        VectorDiff::Reset     { values }    => (true,  values.into_iter().collect()),
        _ => return,
    };

    if reset {
        if let Ok(guard) = handler.lock() {
            guard.on_timeline_reset(room_id);
        }
    }

    for item in &items {
        if let Some(ev) = timeline_item_to_ffi(item, room_id) {
            if let Ok(guard) = handler.lock() {
                guard.on_message_event(&ev);
            }
        }
    }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn client_ffi_new_starts_empty() {
        let c = ClientFfi::new();
        assert!(c.client.is_none());
        assert!(c.stop_tx.is_none());
        assert!(c.oauth_flow.is_none());
    }

    #[test]
    fn op_result_ok_sets_fields() {
        let r = ok("success");
        assert!(r.ok);
        assert_eq!(r.message, "success");
    }

    #[test]
    fn op_result_err_sets_fields() {
        let r = err("failure");
        assert!(!r.ok);
        assert_eq!(r.message, "failure");
    }

    #[test]
    fn oauth_err_has_no_urls() {
        let r = oauth_err("bad server");
        assert!(!r.ok);
        assert_eq!(r.message, "bad server");
        assert!(r.auth_url.is_empty());
        assert!(r.redirect_uri.is_empty());
    }

    #[test]
    fn export_session_is_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.export_session().is_empty());
    }

    #[test]
    fn list_rooms_is_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.list_rooms().is_empty());
    }

    #[test]
    fn send_message_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_message("!room:example.com", "hello");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn restore_session_fails_on_invalid_json() {
        let mut c = ClientFfi::new();
        let r = c.restore_session("not json {{{");
        assert!(!r.ok);
        assert!(r.message.contains("parse session JSON"));
    }

    #[test]
    fn oauth_cancel_is_noop_without_flow() {
        let mut c = ClientFfi::new();
        c.oauth_cancel();
    }

    #[test]
    fn stop_sync_is_noop_without_start() {
        let mut c = ClientFfi::new();
        c.stop_sync();
    }

    #[test]
    #[cfg(all(unix, not(target_os = "macos")))]
    fn data_dir_ends_with_expected_suffix() {
        let d = data_dir();
        let s = d.to_string_lossy();
        assert!(
            s.ends_with("tesseract/matrix-store"),
            "unexpected data_dir: {s}"
        );
    }
}
