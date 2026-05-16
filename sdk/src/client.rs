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

use crate::ffi::{BackupProgress, OAuthBegin, OpResult, PaginateResult};
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
    timeline::{MsgLikeContent, MsgLikeKind, RoomExt, TimelineDetails, TimelineEventItemId, TimelineEventFocusThreadMode, TimelineFocus, TimelineItem, TimelineItemContent, TimelineItemKind, VirtualTimelineItem},
};
#[cfg(not(test))]
use futures_util::StreamExt;
#[cfg(not(test))]
use crate::ffi::{EventHandlerBridge, ReactionGroup, ReadReceipt, TimelineEvent, VerificationEmoji};
#[cfg(not(test))]
use matrix_sdk::{Room, ruma::UserId};
#[cfg(not(test))]
use matrix_sdk::ruma::{
    events::AnySyncTimelineEvent,
    push::{HttpPusherData, PushConditionRoomCtx, PushFormat, Ruleset},
    serde::Raw,
    UInt,
};
#[cfg(not(test))]
use matrix_sdk::ruma::api::client::push::{PusherIds, PusherInit, PusherKind};

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

// `BackupProgress.state` encoding — kept in sync with the docs on the cxx
// shared struct in `bridge.rs` and the `BackupState` enum in
// `client/include/tesseract/types.h`.
const BACKUP_STATE_UNKNOWN:     u8 = 0;
const BACKUP_STATE_DISABLED:    u8 = 1;
const BACKUP_STATE_ENABLED:     u8 = 2;
const BACKUP_STATE_DOWNLOADING: u8 = 3;
const BACKUP_STATE_CREATING:    u8 = 4;

#[cfg(not(test))]
fn backup_state_code(s: matrix_sdk::encryption::backups::BackupState) -> u8 {
    use matrix_sdk::encryption::backups::BackupState as B;
    match s {
        B::Unknown                   => BACKUP_STATE_UNKNOWN,
        B::Disabling                 => BACKUP_STATE_DISABLED,
        B::Enabled                   => BACKUP_STATE_ENABLED,
        B::Downloading | B::Enabling | B::Resuming => BACKUP_STATE_DOWNLOADING,
        B::Creating                  => BACKUP_STATE_CREATING,
    }
}

// `on_room_list_state` payload encoding — kept in sync with the
// `RoomListState` enum in `client/include/tesseract/types.h`. Mirrors
// `matrix_sdk_ui::room_list_service::State`.
pub(crate) const ROOM_LIST_STATE_INIT:       u8 = 0;
pub(crate) const ROOM_LIST_STATE_SETTING_UP: u8 = 1;
pub(crate) const ROOM_LIST_STATE_RECOVERING: u8 = 2;
pub(crate) const ROOM_LIST_STATE_RUNNING:    u8 = 3;
pub(crate) const ROOM_LIST_STATE_ERROR:      u8 = 4;
pub(crate) const ROOM_LIST_STATE_TERMINATED: u8 = 5;

fn room_list_state_code(s: &matrix_sdk_ui::room_list_service::State) -> u8 {
    use matrix_sdk_ui::room_list_service::State as S;
    match s {
        S::Init             => ROOM_LIST_STATE_INIT,
        S::SettingUp        => ROOM_LIST_STATE_SETTING_UP,
        S::Recovering       => ROOM_LIST_STATE_RECOVERING,
        S::Running          => ROOM_LIST_STATE_RUNNING,
        S::Error { .. }     => ROOM_LIST_STATE_ERROR,
        S::Terminated { .. } => ROOM_LIST_STATE_TERMINATED,
    }
}

#[cfg(test)]
fn backup_progress_default() -> BackupProgress {
    BackupProgress { state: BACKUP_STATE_UNKNOWN, imported_keys: 0, total_keys: 0 }
}

/// Default per-platform location for the matrix-sdk SQLite store. Used as the
/// initial value of `ClientFfi::data_dir`; callers can override it via
/// `ClientFfi::set_data_dir` (e.g. to scope the store under a specific
/// account directory in the multi-account layout).
fn default_data_dir() -> PathBuf {
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

use std::sync::atomic::{AtomicU64, Ordering};

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
    /// `true` when this timeline was built via `subscribe_room_at` (focused on
    /// a specific event); `false` for the live timeline built by
    /// `subscribe_room`. `paginate_forward` requires `is_focused == true`.
    is_focused:  bool,
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
    /// Persistent receiver clone used by `paginate_back_with_status` to race
    /// the network call against shutdown. Set in `start_sync`; survives until
    /// the `ClientFfi` is dropped so the cancel arm fires even if the call
    /// starts just before `stop_sync` takes `stop_tx`.
    stop_rx:    Option<watch::Receiver<bool>>,
    oauth_flow: Option<oauth::PendingFlow>,
    #[cfg(not(test))]
    handler:      Option<Arc<Mutex<SendHandler>>>,
    #[cfg(not(test))]
    sync_service: Option<Arc<SyncService>>,
    #[cfg(not(test))]
    timelines:    HashMap<OwnedRoomId, TimelineHandle>,
    /// Background backfill orchestrator handle. Aborting it tears down both
    /// the orchestrator and every per-room silent backfill it spawned (the
    /// children live inside a `JoinSet` owned by the orchestrator future).
    #[cfg(not(test))]
    backfill_task: Option<tokio::task::AbortHandle>,
    /// Abort handles for every long-lived task spawned by `start_sync`
    /// (session-refresh watcher, room/pack watcher, backup watchers, sync
    /// monitor, …). These outlive `self.handler.take()`, so without explicit
    /// aborts they keep calling back through their own `Arc<SendHandler>`
    /// clone into a C++ shell that may already be torn down (use-after-free).
    /// Drained and aborted by `stop_sync`.
    #[cfg(not(test))]
    sync_tasks: Vec<tokio::task::AbortHandle>,
    /// Handles for the `client.add_event_handler` registrations made by
    /// `start_sync` (notification + typing handlers). Removed in `stop_sync`
    /// so they stop firing into a destroyed handler.
    #[cfg(not(test))]
    event_handler_handles: Vec<matrix_sdk::event_handler::EventHandlerHandle>,
    /// Latest known backup state code (see BACKUP_STATE_* constants).
    /// Updated by the backup watcher task and read by `backup_state()`.
    #[cfg(not(test))]
    backup_state_code: Arc<std::sync::atomic::AtomicU8>,
    /// Running counter of room keys imported from the backup since this
    /// process started. Reset to 0 only on logout.
    #[cfg(not(test))]
    imported_keys:    Arc<AtomicU64>,
    /// Cached homeserver media upload limit in bytes. 0 = unknown / unfetched.
    /// Populated lazily on first `media_upload_limit()` call after login.
    media_upload_limit: AtomicU64,
    /// Cached MSC2545 image packs (user pack + every enabled room pack).
    /// Rebuilt by `refresh_image_packs_async` whenever sync delivers a
    /// relevant event; read by the FFI list/* accessors without blocking.
    #[cfg(not(test))]
    image_packs: Arc<Mutex<Vec<crate::image_packs::ImagePack>>>,
    /// Serializes every account-data read-modify-write (`recent_emoji_bump`,
    /// `save_sticker_to_user_pack`, `toggle_favorite_sticker`). Matrix
    /// account-data is last-write-wins with no server-side merge, so two
    /// concurrent GET→modify→PUT cycles would drop one side's change. Held
    /// across the whole cycle so writes apply on top of each other.
    #[cfg(not(test))]
    account_data_lock: Arc<tokio::sync::Mutex<()>>,
    /// Directory holding the matrix-sdk SQLite store for this client. Set via
    /// `set_data_dir` before `oauth_begin` / `restore_session`. Defaults to
    /// `default_data_dir()` for legacy single-account callers.
    data_dir:   PathBuf,
    /// Maps verification `flow_id` → `user_id` for in-flight verification
    /// requests (both incoming and outgoing). Allows `accept_verification`,
    /// `start_sas`, etc. to look up the right VerificationRequest via the
    /// SDK's internal map using (user_id, flow_id).
    #[cfg(not(test))]
    verification_flow_users: Arc<Mutex<HashMap<String, String>>>,
    /// Most-recently-computed SAS emoji per `flow_id`. Populated by the SAS
    /// watcher task when `KeysExchanged` fires; read synchronously by
    /// `get_sas_emojis()`.
    #[cfg(not(test))]
    sas_emoji_cache: Arc<Mutex<HashMap<String, Vec<(String, String)>>>>,
    // Declared last so it drops after all SDK resources; deadpool/SQLite cleanup
    // uses tokio primitives and requires the runtime to still be alive.
    rt:         Runtime,
}

impl Drop for ClientFfi {
    fn drop(&mut self) {
        self.stop_sync();
        #[cfg(not(test))]
        if let Some(h) = self.backfill_task.take() { h.abort(); }
        // Drop SDK objects that call Handle::current() in their Drop impls
        // (SqliteStateStore via matrix_sdk::Client, Timeline) with the runtime
        // handle in TLS.  Without enter() here, dropping pending_login_client_
        // from C++ (Qt event loop, no tokio context) causes a panic_in_cleanup
        // abort — the same hazard oauth_await_callback / restore_session guard
        // against with their own `let _guard = self.rt.enter()` blocks.
        {
            let _guard = self.rt.enter();
            #[cfg(not(test))]
            for (_, th) in self.timelines.drain() {
                for h in th.abort_tasks { h.abort(); }
            }
            // Explicit take: matrix_sdk::Client drops here (runtime in TLS)
            // rather than in the implicit field-drop pass after this fn returns.
            let _ = self.client.take();
        }
        // Remaining fields are all None/empty; rt drops last (declared last).
    }
}

/// Lock a `Mutex` without ever panicking. A poisoned mutex (a thread panicked
/// while holding the guard) is recovered by taking the inner value: panicking
/// here instead — via `.lock().unwrap()` — would unwind a tokio task and, on
/// synchronous FFI entry points, propagate a panic across the C++ boundary
/// (undefined behavior). The protected maps are plain caches whose worst-case
/// post-poison state is a stale entry, so recovery is safe.
#[cfg(not(test))]
fn lock_or_recover<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|p| p.into_inner())
}

/// Send public (`m.read`), private (`m.read.private`), and fully-read
/// (`m.fully_read`) markers for `event_id` in a single request.
///
/// `m.read.private` advances the user's own read position across devices
/// without broadcasting it to other room members (MSC2285). `m.fully_read`
/// sets the account-data marker that matrix-sdk-ui uses to position the
/// `VirtualTimelineItem::ReadMarker` ("New messages" divider) in the timeline.
#[cfg(not(test))]
async fn send_both_receipts(
    room: &matrix_sdk::Room,
    event_id: matrix_sdk::ruma::OwnedEventId,
) -> matrix_sdk::Result<()> {
    use matrix_sdk::room::Receipts;
    room.send_multiple_receipts(
        Receipts::new()
            .fully_read_marker(event_id.clone())
            .public_read_receipt(event_id.clone())
            .private_read_receipt(event_id),
    ).await
}

/// Hard upper bound on a single media download (64 MiB). A malicious or
/// buggy homeserver can serve an arbitrarily large payload for any `mxc://`
/// URI; matrix-sdk buffers the whole body into memory, so without a cap a
/// single fetch can OOM the process. Oversized content is dropped (returns
/// empty) rather than propagated into image decoders.
const MAX_MEDIA_BYTES: usize = 64 * 1024 * 1024;

fn cap_media_bytes(bytes: Vec<u8>) -> Vec<u8> {
    if bytes.len() > MAX_MEDIA_BYTES {
        tracing::warn!(
            "media download {} bytes exceeds {} byte cap; discarding",
            bytes.len(), MAX_MEDIA_BYTES,
        );
        return Vec::new();
    }
    bytes
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
            stop_rx:    None,
            oauth_flow: None,
            #[cfg(not(test))]
            handler:      None,
            #[cfg(not(test))]
            sync_service: None,
            #[cfg(not(test))]
            timelines:    HashMap::new(),
            #[cfg(not(test))]
            backfill_task: None,
            #[cfg(not(test))]
            sync_tasks: Vec::new(),
            #[cfg(not(test))]
            event_handler_handles: Vec::new(),
            #[cfg(not(test))]
            backup_state_code: Arc::new(std::sync::atomic::AtomicU8::new(BACKUP_STATE_UNKNOWN)),
            #[cfg(not(test))]
            imported_keys:    Arc::new(AtomicU64::new(0)),
            media_upload_limit: AtomicU64::new(0),
            #[cfg(not(test))]
            image_packs: Arc::new(Mutex::new(Vec::new())),
            #[cfg(not(test))]
            account_data_lock: Arc::new(tokio::sync::Mutex::new(())),
            data_dir:   default_data_dir(),
            #[cfg(not(test))]
            verification_flow_users: Arc::new(Mutex::new(HashMap::new())),
            #[cfg(not(test))]
            sas_emoji_cache: Arc::new(Mutex::new(HashMap::new())),
            rt:         Runtime::new().expect("tokio runtime"),
        }
    }

    /// Override the per-instance data directory. Callers should invoke this
    /// immediately after `new()` and before `oauth_begin` / `restore_session`
    /// so the matrix-sdk SQLite store is opened at the right path. Creates
    /// the directory if it does not exist; silently ignores empty input so
    /// FFI callers that pass through an empty string keep the default.
    pub fn set_data_dir(&mut self, path: &str) {
        if path.is_empty() { return; }
        let p = PathBuf::from(path);
        let _ = std::fs::create_dir_all(&p);
        self.data_dir = p;
    }

    // -----------------------------------------------------------------------
    // OAuth login
    // -----------------------------------------------------------------------

    pub fn oauth_begin(&mut self, homeserver: &str) -> OAuthBegin {
        if let Some(prev) = self.oauth_flow.take() {
            oauth::cancel(&prev);
        }

        let hs   = homeserver.to_owned();
        let path = self.data_dir.clone();

        // Only start from a clean store when there is no active session. If a
        // session was already restored (e.g. the user hit "Sign in again"
        // after a timeout, or a second flow), wiping here would silently
        // destroy the live SQLite cache for the restored account. The store
        // is cleared explicitly on logout instead.
        if self.client.is_none() {
            let _ = std::fs::remove_dir_all(&path);
        }
        let _ = std::fs::create_dir_all(&path);

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
            Ok(client) => {
                // Enter the runtime so any prior Client we're overwriting drops
                // with a tokio context in TLS — matrix-sdk's SqliteStateStore /
                // deadpool tear-down calls Handle::current() in its Drop impl.
                let _guard = self.rt.enter();
                self.client = Some(client);
                ok("")
            }
            Err(e) => err(format!("{e:#}")),
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
        let path       = self.data_dir.clone();
        let _ = std::fs::create_dir_all(&path);

        let result = self.rt.block_on(async move {
            let client = Client::builder()
                .server_name_or_homeserver_url(homeserver)
                .sqlite_store(&path, None)
                .handle_refresh_tokens()
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
            Ok(c) => {
                // Enter the runtime so the previous Client (still in self.client
                // after an auth-error → re-restore path) drops with a tokio
                // context in TLS — matrix-sdk's SqliteStateStore / deadpool
                // tear-down calls Handle::current() in its Drop impl.
                let _guard = self.rt.enter();
                self.client = Some(c);
                ok("")
            }
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

    /// Spawn a long-lived sync task and record its abort handle so
    /// `stop_sync` can cancel it before the C++ handler is destroyed.
    #[cfg(not(test))]
    fn spawn_tracked<F>(&mut self, fut: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        let h = self.rt.spawn(fut).abort_handle();
        self.sync_tasks.push(h);
    }

    #[cfg(not(test))]
    pub fn start_sync(&mut self, handler: UniquePtr<EventHandlerBridge>) {
        let Some(client) = self.client.clone() else { return };

        let (stop_tx, stop_rx) = watch::channel(false);
        let stop_tx_auth = stop_tx.clone();
        self.stop_rx = Some(stop_rx.clone());
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
            self.spawn_tracked(async move {
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
                        Ok(SessionChange::UnknownToken(data)) => {
                            // Stop SyncService before it can reach State::Error and
                            // wipe the SQLite data directory while a fresh login is
                            // already in progress.
                            let _ = stop_tx_auth.send(true);
                            if let Ok(guard) = h.lock() {
                                guard.on_error(
                                    "sync_auth_error",
                                    "Session token is no longer valid; please log in again.",
                                    data.soft_logout,
                                );
                            }
                            break;
                        }
                        Err(_) => break,
                    }
                }
            });
        }

        // Global notification handler — fires for every room on every sync
        // response, without requiring a per-room subscribe_room call.
        {
            use matrix_sdk::ruma::events::room::message::MessageType;
            use matrix_sdk::ruma::events::OriginalSyncMessageLikeEvent;
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            self.event_handler_handles.push(client.add_event_handler(
                move |ev: OriginalSyncMessageLikeEvent<RoomMessageEventContent>,
                      room: Room| {
                    let h            = Arc::clone(&h);
                    let client_clone = client_clone.clone();
                    async move {
                        let (body, msg_type_str) = match &ev.content.msgtype {
                            MessageType::Text(t)  => (t.body.trim().to_owned(), "m.text"),
                            MessageType::Image(i) => (i.body.trim().to_owned(), "m.image"),
                            MessageType::File(f)  => (f.body.trim().to_owned(), "m.file"),
                            MessageType::Audio(a) => (a.body.trim().to_owned(), "m.audio"),
                            MessageType::Video(v) => (v.body.trim().to_owned(), "m.video"),
                            _ => return,
                        };
                        if body.is_empty() { return; }
                        let me = client_clone.user_id().map(|u| u.to_owned());
                        let sender = ev.sender.as_str().to_owned();
                        if me.as_deref().map(|u| u.as_str()) == Some(&sender) { return; }
                        let room_id  = room.room_id().as_str().to_owned();
                        let event_id = ev.event_id.as_str();
                        let ts: u64  = ev.origin_server_ts.get().into();
                        let synthetic = build_push_rule_json(
                            &room_id, event_id, &sender, &body, msg_type_str, ts);
                        let (should_notify, is_mention) =
                            evaluate_push_rules(&client_clone, &room, &synthetic).await;
                        if should_notify {
                            let room_name = room.display_name().await
                                .map(|n| n.to_string())
                                .unwrap_or_else(|_| room_id.clone());
                            // Resolve the sender's display name for the
                            // notification (push-rule eval above still uses
                            // the raw MXID). Same cached-member pattern as
                            // the typing handler; falls back to localpart.
                            let sender_name = match
                                room.get_member_no_sync(&ev.sender).await
                            {
                                Ok(Some(m)) => m.display_name()
                                    .map(str::to_owned)
                                    .unwrap_or_else(
                                        || ev.sender.localpart().to_string()),
                                _ => ev.sender.localpart().to_string(),
                            };
                            let avatar = room.avatar(matrix_sdk::media::MediaFormat::File)
                                .await.ok().flatten().unwrap_or_default();
                            if let Ok(g) = h.lock() {
                                g.on_notification(&room_id, &room_name,
                                                  &sender_name, &body, is_mention, &avatar);
                            }
                        }
                    }
                },
            ));
        }

        // Global typing-notification handler. Fires for every room whenever
        // the set of typing users changes. Self is filtered out so the UI
        // never shows the local user in the typing bar.
        {
            use matrix_sdk::ruma::events::typing::SyncTypingEvent;
            let h    = Arc::clone(&handler);
            let me   = client.user_id().map(|u| u.to_owned());
            self.event_handler_handles.push(client.add_event_handler(
                move |ev: SyncTypingEvent, room: Room| {
                    let h  = Arc::clone(&h);
                    let me = me.clone();
                    async move {
                        let rid  = room.room_id().to_string();
                        let mut uids: Vec<String> = Vec::new();
                        for u in ev.content.user_ids.iter() {
                            if me.as_deref() == Some(u.as_ref()) { continue; }
                            // Prefer the cached room-member display name so the
                            // typing strip shows a readable name rather than an
                            // opaque localpart (e.g. "@78fa3bcde:hs"). Falls
                            // back to the localpart when no member profile is
                            // cached (no extra network round-trip).
                            let name = match room.get_member_no_sync(u).await {
                                Ok(Some(m)) => m.display_name()
                                    .map(str::to_owned)
                                    .unwrap_or_else(|| u.localpart().to_string()),
                                _ => u.localpart().to_string(),
                            };
                            uids.push(name);
                        }
                        if let Ok(g) = h.lock() {
                            g.on_typing_changed(&rid, &uids);
                        }
                    }
                },
            ));
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
            let packs_cache       = Arc::clone(&self.image_packs);

            self.spawn_tracked(async move {
                // Initial snapshot. `room_info_notable_update_receiver`
                // only fires on *notable* room transitions (display-name,
                // member changes, encryption state, …). After
                // `restore_session()` the matrix-sdk has already
                // repopulated `joined_rooms()` from the SQLite cache, but
                // no notable update is emitted until the server actually
                // sends one — so on a quiet restored session the UI
                // would sit forever on an empty room list. Push the
                // cached set once before entering the recv loop. On a
                // fresh login `joined_rooms()` is empty here and we
                // emit an empty list, which is then overwritten by the
                // first sync's notable update.
                let rooms = build_room_infos(&client_clone).await;
                if let Ok(guard) = h.lock() {
                    guard.on_rooms_updated(&rooms);
                }
                // Initial prefs snapshot — fired BEFORE on_rooms_updated so
                // the UI has pendingRestoreRoom_ set when the room list
                // arrives and can navigate immediately on first paint.
                let mut prev_prefs = read_prefs_json(&client_clone).await;
                if let Ok(guard) = h.lock() {
                    guard.on_account_prefs_updated(&prev_prefs);
                }

                // Initial image-pack snapshot, same reasoning as for the
                // room list: piggy-back on the same wakeup channel so
                // both lists arrive together after every notable
                // sync delta.
                let pks = rebuild_image_packs(&client_clone).await;
                if let Ok(mut g) = packs_cache.lock() { *g = pks; }
                if let Ok(guard) = h.lock() { guard.on_image_packs_updated(); }

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
                            // Refresh image packs on the same tick.
                            // Account-data and state-event changes that
                            // matter for image packs flow through the
                            // same sync deltas that produce notable
                            // room updates; piggy-backing keeps us off
                            // a polling timer and out of the event
                            // handler machinery.
                            let pks = rebuild_image_packs(&client_clone).await;
                            if let Ok(mut g) = packs_cache.lock() {
                                // Rebuild reads the SDK state store, which doesn't
                                // reflect set_account_data_raw until the server echo
                                // arrives in a sync response. If we just saved a
                                // sticker, the cache has the new user pack but the
                                // state store is still stale. room_info_notable_update
                                // doesn't fire for account_data events, so this may
                                // be the only rebuild before the echo — preserve the
                                // cached user pack so it doesn't vanish from the picker.
                                let has_user = pks.iter().any(|p| {
                                    p.source == crate::image_packs::PackSource::User
                                });
                                if !has_user {
                                    if let Some(cached) = g.iter()
                                        .find(|p| p.source == crate::image_packs::PackSource::User)
                                        .cloned()
                                    {
                                        let mut merged = pks;
                                        merged.insert(0, cached);
                                        *g = merged;
                                    } else {
                                        *g = pks;
                                    }
                                } else {
                                    *g = pks;
                                }
                            }
                            if let Ok(guard) = h.lock() {
                                guard.on_image_packs_updated();
                            }
                            // Check for prefs changes on the same tick.
                            // `save_prefs` does a fire-and-forget PUT which
                            // echoes back as an account-data event in the
                            // next sync, triggering a notable update.
                            let cur_prefs = read_prefs_json(&client_clone).await;
                            if cur_prefs != prev_prefs {
                                if let Ok(guard) = h.lock() {
                                    guard.on_account_prefs_updated(&cur_prefs);
                                }
                                prev_prefs = cur_prefs;
                            }
                        }
                    }
                }
            });
        }

        // Recovery state watcher (Step 6).
        //
        // `client.encryption().recovery().state()` starts as `Unknown` and is
        // only populated once the relevant account-data events arrive during
        // the first sync cycle. Without this watcher, a UI that calls
        // `needs_recovery()` right after `start_sync()` always sees `false`.
        //
        // Every state transition triggers an extra `on_backup_progress` so the
        // UI gets a chance to re-evaluate `needs_recovery()`. Reusing that
        // callback (instead of adding a dedicated one) keeps the FFI small —
        // the UI was already re-checking via this slot.
        {
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            let state_code   = Arc::clone(&self.backup_state_code);
            let imported     = Arc::clone(&self.imported_keys);
            let mut stop_rx  = stop_rx.clone();

            self.spawn_tracked(async move {
                use futures_util::StreamExt;
                let mut rec_stream = client_clone.encryption().recovery().state_stream();
                loop {
                    tokio::select! {
                        _ = stop_rx.changed() => {
                            if *stop_rx.borrow() { break; }
                        }
                        Some(_state) = rec_stream.next() => {
                            // Re-emit a snapshot; the UI re-queries needs_recovery().
                            if let Ok(guard) = h.lock() {
                                guard.on_backup_progress(&BackupProgress {
                                    state:         state_code.load(Ordering::Relaxed),
                                    imported_keys: imported.load(Ordering::Relaxed),
                                    total_keys:    0,
                                });
                            }
                        }
                        else => break,
                    }
                }
            });
        }

        // Backup-state watcher (Step 6).
        //
        // Subscribes to `Backups::state_stream()` for high-level transitions
        // (Unknown → Enabling → Downloading → Enabled, etc.) and emits an
        // `on_backup_progress` callback on every change.
        //
        // `total_keys` is left at 0 because matrix-sdk does not expose a cheap
        // "how many keys does the backup contain" query.
        {
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            let state_code   = Arc::clone(&self.backup_state_code);
            let imported     = Arc::clone(&self.imported_keys);
            let mut stop_rx  = stop_rx.clone();

            self.spawn_tracked(async move {
                use futures_util::StreamExt;
                let mut state_stream = client_clone.encryption().backups().state_stream();

                // Emit an initial snapshot so a UI that opens before the
                // first state change still has a starting value.
                {
                    let s = backup_state_code(client_clone.encryption().backups().state());
                    state_code.store(s, Ordering::Relaxed);
                    if let Ok(guard) = h.lock() {
                        guard.on_backup_progress(&BackupProgress {
                            state:         s,
                            imported_keys: imported.load(Ordering::Relaxed),
                            total_keys:    0,
                        });
                    }
                }

                loop {
                    tokio::select! {
                        _ = stop_rx.changed() => {
                            if *stop_rx.borrow() { break; }
                        }
                        Some(Ok(state)) = state_stream.next() => {
                            let s = backup_state_code(state);
                            state_code.store(s, Ordering::Relaxed);
                            if let Ok(guard) = h.lock() {
                                guard.on_backup_progress(&BackupProgress {
                                    state:         s,
                                    imported_keys: imported.load(Ordering::Relaxed),
                                    total_keys:    0,
                                });
                            }
                        }
                        else => break,
                    }
                }
            });
        }

        // Imported-room-keys watcher (Step 6).
        //
        // `Encryption::room_keys_received_stream()` only becomes available once
        // the OlmMachine is initialised — which can lag a beat behind login. If
        // we were to call it once at start_sync time it might return `None`,
        // and we'd silently miss every batch (this is exactly what made the
        // recovery dialog show "0 keys imported").
        //
        // So we poll with a short backoff until the stream becomes available,
        // then forward each batch's `.len()` into the shared imported_keys
        // counter and re-emit an `on_backup_progress` so the UI updates live.
        {
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            let state_code   = Arc::clone(&self.backup_state_code);
            let imported     = Arc::clone(&self.imported_keys);
            let mut stop_rx  = stop_rx.clone();

            self.spawn_tracked(async move {
                use futures_util::StreamExt;

                let keys_stream = loop {
                    if *stop_rx.borrow() { return; }
                    if let Some(s) = client_clone
                        .encryption()
                        .room_keys_received_stream()
                        .await
                    {
                        break s;
                    }
                    tokio::select! {
                        _ = stop_rx.changed() => {
                            if *stop_rx.borrow() { return; }
                        }
                        _ = tokio::time::sleep(std::time::Duration::from_millis(500)) => {}
                    }
                };
                let mut keys_stream = Box::pin(keys_stream);

                loop {
                    tokio::select! {
                        _ = stop_rx.changed() => {
                            if *stop_rx.borrow() { break; }
                        }
                        Some(batch) = keys_stream.next() => {
                            if let Ok(keys) = batch {
                                let n = imported.fetch_add(keys.len() as u64, Ordering::Relaxed)
                                    + keys.len() as u64;
                                if let Ok(guard) = h.lock() {
                                    guard.on_backup_progress(&BackupProgress {
                                        state:         state_code.load(Ordering::Relaxed),
                                        imported_keys: n,
                                        total_keys:    0,
                                    });
                                }
                            }
                        }
                        else => break,
                    }
                }
            });
        }

        // RoomListService state watcher.
        //
        // Surfaces the high-level sliding-sync phases (Init → SettingUp →
        // Running, plus Recovering on reconnect) so the UI can show a
        // "Syncing rooms…" status while the joined-room set is still being
        // hydrated. The SyncService itself only exposes Idle/Running/Error;
        // the room-list service is where the actually-interesting transitions
        // live.
        //
        // Emits an initial snapshot before the recv loop so a UI that opens
        // before the first transition still has a starting value, matching
        // the backup-state watcher above.
        {
            let h            = Arc::clone(&handler);
            let svc_clone    = Arc::clone(&sync_service);
            let mut stop_rx  = stop_rx.clone();

            self.spawn_tracked(async move {
                let rls          = svc_clone.room_list_service();
                let mut state_rx = rls.state();

                // Initial snapshot.
                {
                    let s = room_list_state_code(&state_rx.next_now());
                    if let Ok(guard) = h.lock() {
                        guard.on_room_list_state(s);
                    }
                }

                loop {
                    tokio::select! {
                        _ = stop_rx.changed() => {
                            if *stop_rx.borrow() { break; }
                        }
                        Some(state) = state_rx.next() => {
                            let s = room_list_state_code(&state);
                            if let Ok(guard) = h.lock() {
                                guard.on_room_list_state(s);
                            }
                        }
                        else => break,
                    }
                }
            });
        }

        // Verification state watcher.
        //
        // Subscribes to `client.encryption().verification_state()` so the UI
        // is notified whenever the cross-signing verified status of the current
        // account changes (Unknown → Unverified → Verified). Fires an initial
        // snapshot on startup so the UI always has a starting value.
        {
            let h            = Arc::clone(&handler);
            let client_clone = client.clone();
            let mut stop_rx  = stop_rx.clone();

            self.spawn_tracked(async move {
                use matrix_sdk::encryption::VerificationState;
                let mut state_rx = client_clone.encryption().verification_state();

                // Initial snapshot.
                {
                    let s = matches!(state_rx.next_now(), VerificationState::Verified);
                    if let Ok(guard) = h.lock() {
                        guard.on_verification_state_changed(s);
                    }
                }

                loop {
                    tokio::select! {
                        _ = stop_rx.changed() => {
                            if *stop_rx.borrow() { break; }
                        }
                        Some(state) = state_rx.next() => {
                            let verified = matches!(state, VerificationState::Verified);
                            if let Ok(guard) = h.lock() {
                                guard.on_verification_state_changed(verified);
                            }
                        }
                        else => break,
                    }
                }
            });
        }

        // Incoming verification request handler.
        //
        // Registers a to-device event handler for `m.key.verification.request`
        // so the UI is notified when another device initiates a SAS flow with
        // this one. After the SDK processes the event internally, we retrieve
        // the `VerificationRequest` object and store the flow_id → user_id
        // mapping so subsequent API calls can do the lookup.
        {
            use matrix_sdk::ruma::events::{
                key::verification::request::ToDeviceKeyVerificationRequestEventContent,
                ToDeviceEvent,
            };
            let h          = Arc::clone(&handler);
            let flow_users = Arc::clone(&self.verification_flow_users);
            let emoji_cache = Arc::clone(&self.sas_emoji_cache);

            self.event_handler_handles.push(client.add_event_handler(
                move |ev: ToDeviceEvent<ToDeviceKeyVerificationRequestEventContent>,
                      client: Client| {
                    let h           = Arc::clone(&h);
                    let flow_users  = Arc::clone(&flow_users);
                    let emoji_cache = Arc::clone(&emoji_cache);
                    async move {
                        let flow_id   = ev.content.transaction_id.to_string();
                        let user_id   = ev.sender.as_str().to_owned();
                        let device_id = ev.content.from_device.as_str().to_owned();

                        // The OlmMachine processes the to-device event and
                        // adds the request to its internal map asynchronously.
                        // A single fixed sleep silently drops the request on
                        // slow hardware / under sync load, so poll with a
                        // bounded backoff (≈ 50+100+200+400+800+1600 ≈ 3.15s
                        // total) instead.
                        let mut req = None;
                        let mut delay_ms = 50u64;
                        for _ in 0..6 {
                            tokio::time::sleep(
                                std::time::Duration::from_millis(delay_ms)).await;
                            req = client
                                .encryption()
                                .get_verification_request(&ev.sender, &flow_id)
                                .await;
                            if req.is_some() { break; }
                            delay_ms *= 2;
                        }
                        if let Some(req) = req {
                            lock_or_recover(&flow_users)
                                .insert(flow_id.clone(), user_id.clone());
                            if let Ok(guard) = h.lock() {
                                guard.on_verification_request(
                                    &flow_id, &user_id, &device_id, true);
                            }
                            // Spawn a watcher so we can surface request-level
                            // transitions (Done / Cancelled) that occur before
                            // start_sas is called.
                            let h2           = Arc::clone(&h);
                            let flow_users2  = Arc::clone(&flow_users);
                            let emoji_cache2 = Arc::clone(&emoji_cache);
                            let flow_id2     = flow_id.clone();
                            tokio::spawn(watch_verification_request(
                                req, flow_id2, h2, flow_users2, emoji_cache2,
                            ));
                        } else {
                            tracing::warn!(
                                "verification request {flow_id} from {user_id} \
                                 not visible after retries; dropped",
                            );
                        }
                    }
                },
            ));
        }

        // Start SyncService and monitor state.
        let svc_clone      = Arc::clone(&sync_service);
        let h_state        = Arc::clone(&handler);
        let mut stop_rx_sv = stop_rx.clone();

        self.spawn_tracked(async move {
            svc_clone.start().await;

            use matrix_sdk_ui::sync_service::State as SyncServiceState;
            let mut state_stream = svc_clone.state();
            let mut consecutive_errors: u32 = 0;

            loop {
                tokio::select! {
                    _ = stop_rx_sv.changed() => {
                        // Authoritative SyncService::stop() is owned by
                        // stop_sync(); just exit the monitor here so the
                        // service is not stopped twice concurrently.
                        if *stop_rx_sv.borrow() { break; }
                    }
                    Some(state) = state_stream.next() => {
                        match state {
                            SyncServiceState::Running => {
                                consecutive_errors = 0;
                            }
                            SyncServiceState::Error(_) => {
                                let _ = svc_clone.stop().await;
                                consecutive_errors += 1;
                                // Keep retrying indefinitely — a transient
                                // outage (tunnel, sleep, server restart) must
                                // not permanently kill sync for the session.
                                // Re-notify the UI every 5 failures so a long
                                // outage isn't silent without spamming.
                                if consecutive_errors % 5 == 0 {
                                    if let Ok(guard) = h_state.lock() {
                                        guard.on_error(
                                            "sync_reconnect",
                                            "Sync is failing repeatedly; retrying…",
                                            false,
                                        );
                                    }
                                }
                                // Exponential backoff capped at 40s.
                                let delay = std::time::Duration::from_secs(
                                    5 * (1u64 << consecutive_errors.saturating_sub(1).min(3)));
                                tokio::select! {
                                    _ = tokio::time::sleep(delay) => {}
                                    _ = stop_rx_sv.changed() => {}
                                }
                                if *stop_rx_sv.borrow() { break; }
                                svc_clone.start().await;
                                state_stream = svc_clone.state();
                            }
                            _ => {}
                        }
                    }
                }
            }
        });
    }

    pub fn stop_sync(&mut self) {
        // Take the handler here so that the Drop impl calling stop_sync a
        // second time (after ~MainWindow has already run) is a no-op.  If the
        // handler is already gone we skip the session flush on the second call
        // rather than calling back into a partially-destroyed C++ object.
        #[cfg(not(test))]
        let handler = self.handler.take();
        // Flush the latest OAuth session to disk before tearing down the
        // runtime.  The session-watcher task (spawned in start_sync) saves
        // new tokens whenever TokensRefreshed fires, but its JoinHandle is
        // discarded so it may be cancelled mid-flight when the runtime drops.
        // Saving here, while the C++ EventHandler is still alive, ensures the
        // most recent refresh token is always persisted on clean shutdown.
        #[cfg(not(test))]
        if let (Some(client), Some(handler)) = (&self.client, &handler) {
            if let Some(full) = client.oauth().full_session() {
                let persisted = PersistedSession { client_id: full.client_id, user: full.user };
                if let Ok(json) = serde_json::to_string(&persisted) {
                    if let Ok(guard) = handler.lock() {
                        guard.on_session_refreshed(&json);
                    }
                }
            }
        }

        if let Some(tx) = self.stop_tx.take() {
            let _ = tx.send(true);
        }
        // Remove the global event-handler registrations so the notification /
        // typing / verification handlers stop firing into a handler that is
        // about to be dropped.
        #[cfg(not(test))]
        if let Some(client) = &self.client {
            for eh in self.event_handler_handles.drain(..) {
                client.remove_event_handler(eh);
            }
        }
        #[cfg(not(test))]
        if let Some(h) = self.backfill_task.take() {
            h.abort();
        }
        // Hard-abort every long-lived sync task. The stop signal above lets
        // the ones that select on it exit cleanly; aborting also cancels the
        // session-refresh watcher (which has no stop-channel arm) and closes
        // the use-after-free window where a task could still call back into
        // the C++ handler after self.handler was taken.
        #[cfg(not(test))]
        for h in self.sync_tasks.drain(..) {
            h.abort();
        }
        #[cfg(not(test))]
        if let Some(svc) = self.sync_service.take() {
            self.rt.block_on(async move { let _ = svc.stop().await; });
        }
    }

    // -----------------------------------------------------------------------
    // Timeline subscription (Step 2)
    // -----------------------------------------------------------------------

    /// Spawn the two tasks that are common to both `subscribe_room` and
    /// `subscribe_room_at`: the streaming task that pumps timeline diffs to
    /// the UI, and the `fetch_members` backfill task. Returns their
    /// `AbortHandle`s so the caller can store them in `TimelineHandle`.
    ///
    /// `timeline` and `room` must already be fully constructed; `room_id_str`
    /// is the string form of the room ID used by handler callbacks.
    #[cfg(not(test))]
    fn spawn_timeline_tasks(
        timeline:    &Arc<matrix_sdk_ui::Timeline>,
        room:        &matrix_sdk::Room,
        room_id_str: String,
        handler:     &Arc<Mutex<SendHandler>>,
        client:      &Client,
        rt:          &tokio::runtime::Runtime,
    ) -> (tokio::task::AbortHandle, tokio::task::AbortHandle) {
        let tl         = Arc::clone(timeline);
        let h          = Arc::clone(handler);
        let rid        = room_id_str;
        let room_clone = room.clone();
        let me         = client.user_id().map(|u| u.to_owned());
        let client_ref = client.clone();

        let abort = rt.spawn(async move {
            let (initial_items, mut stream) = tl.subscribe().await;

            // Build the visibility mirror + initial snapshot in one pass.
            // The mirror is `true` for every matrix-sdk-ui timeline slot
            // whose `timeline_item_to_ffi` yields Some — this covers both
            // real message events and virtual items (day-dividers,
            // read-markers, timeline-start). State events and membership
            // changes remain `false` so they are silently filtered.
            let mut visible: Vec<bool> = Vec::with_capacity(initial_items.len());
            let mut snapshot: Vec<TimelineEvent> = Vec::new();
            for item in initial_items.iter() {
                let ev = timeline_item_to_ffi(
                    item, &rid, &room_clone, me.as_deref()).await;
                visible.push(ev.is_some());
                if let Some(ev) = ev { snapshot.push(ev); }
            }
            if let Ok(guard) = h.lock() {
                guard.on_timeline_reset(&rid, &snapshot);
            }
            drop(snapshot);

            while let Some(diffs) = stream.next().await {
                for diff in diffs {
                    handle_timeline_diff(
                        diff, &mut visible, &h, &rid, &room_clone, me.as_deref(),
                        &client_ref).await;
                }
            }
        }).abort_handle();

        // Backfill sender profiles. `matrix-sdk-ui`'s Timeline does not
        // sync member info on its own — `EventTimelineItem::sender_profile()`
        // stays at `TimelineDetails::Pending` for any user whose
        // membership state wasn't included in the initial sync's room
        // delta, so their messages render with an empty name + avatar.
        // `fetch_members()` runs `sync_members()` then patches every
        // affected timeline item in place, which the streaming task
        // above picks up as `VectorDiff::Set` and re-emits to C++. We
        // spawn it separately so the initial items aren't blocked
        // behind a multi-second member sync on big rooms.
        let tl_for_members = Arc::clone(timeline);
        let fetch_abort = rt.spawn(async move {
            tl_for_members.fetch_members().await;
        }).abort_handle();

        (abort, fetch_abort)
    }

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

        // Synchronously clear the UI for this room. The follow-up snapshot
        // reset (with the initial cached items) arrives from the spawned
        // task below. Both go through the UI's post-to-UI queue so they
        // serialize in order and no live diffs can land between them —
        // diffs only flow once the task starts pumping `stream`.
        if let Ok(guard) = handler.lock() {
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_timeline_reset(&room_id_str, &empty);
        }

        let (abort, fetch_abort) = Self::spawn_timeline_tasks(
            &timeline, &room, room_id_str, &handler, &client, &self.rt,
        );

        self.timelines.insert(room_id, TimelineHandle {
            timeline,
            abort_tasks: vec![abort, fetch_abort],
            is_focused:  false,
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
        let result = self.paginate_back_with_status(room_id, count);
        OpResult { ok: result.ok, message: result.message }
    }

    #[cfg(not(test))]
    pub fn paginate_back_with_status(
        &mut self,
        room_id: &str,
        count: u16,
    ) -> PaginateResult {
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return PaginateResult {
                ok: false,
                message: format!("invalid room id: {e}"),
                reached_start: false,
                reached_end:   false,
            },
        };

        let Some(handle) = self.timelines.get(&room_id) else {
            return PaginateResult {
                ok: false,
                message: "room not subscribed; call subscribe_room first".into(),
                reached_start: false,
                reached_end:   false,
            };
        };

        let tl      = Arc::clone(&handle.timeline);
        let stop_rx = self.stop_rx.clone();

        // Race the network round-trip against the shutdown signal so that
        // `stop_sync()` unblocks any worker threads waiting in this call.
        match self.rt.block_on(async move {
            let paginate = tl.paginate_backwards(count);
            if let Some(mut rx) = stop_rx {
                tokio::select! {
                    result = paginate => result.map(Some),
                    _ = async {
                        loop {
                            match rx.changed().await {
                                Ok(()) => { if *rx.borrow() { return; } }
                                Err(_) => return, // sender dropped = shutdown
                            }
                        }
                    } => Ok(None),
                }
            } else {
                paginate.await.map(Some)
            }
        }) {
            Ok(Some(reached_start)) => PaginateResult {
                ok: true,
                message: String::new(),
                reached_start,
                reached_end: false,
            },
            Ok(None) => PaginateResult {
                ok: false,
                message: "shutdown in progress".into(),
                reached_start: false,
                reached_end:   false,
            },
            Err(e) => PaginateResult {
                ok: false,
                message: e.to_string(),
                reached_start: false,
                reached_end:   false,
            },
        }
    }

    // -----------------------------------------------------------------------
    // MSC3030 Jump to Date
    // -----------------------------------------------------------------------

    /// MSC3030: resolve a Unix millisecond timestamp to the nearest event ID
    /// in `room_id`. `dir` is `"f"` (forward) or `"b"` (backward).
    /// On success, `OpResult.message` holds the event ID string.
    #[cfg(not(test))]
    pub fn timestamp_to_event(
        &mut self,
        room_id: &str,
        ts_ms:   u64,
        dir:     &str,
    ) -> OpResult {
        use matrix_sdk::ruma::{
            MilliSecondsSinceUnixEpoch,
            api::{Direction, client::room::get_event_by_timestamp::v1::Request},
        };

        let Some(client) = self.client.clone() else { return err("not logged in") };

        let room_id: matrix_sdk::ruma::OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };

        let direction = match dir {
            "f" => Direction::Forward,
            "b" => Direction::Backward,
            _   => return err(format!("invalid dir {:?}: expected \"f\" or \"b\"", dir)),
        };

        let ts = match UInt::try_from(ts_ms) {
            Ok(u)  => MilliSecondsSinceUnixEpoch(u),
            Err(_) => return err(format!("timestamp {ts_ms} is out of range for MilliSecondsSinceUnixEpoch")),
        };

        let req = Request::new(room_id, ts, direction);

        // No shutdown-race here: timestamp lookups are typically fast (single HTTP
        // round-trip) and not in a loop, so blocking the thread is acceptable.
        match self.rt.block_on(async move { client.send(req).await }) {
            Ok(resp) => ok(resp.event_id.to_string()),
            Err(e)   => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn timestamp_to_event(&mut self, _room_id: &str, _ts_ms: u64, _dir: &str) -> OpResult {
        err("not logged in")
    }

    /// MSC3030: subscribe to `room_id`'s timeline focused on `focus_event_id`.
    /// Tears down any previous subscription for this room, then builds a
    /// `TimelineFocus::Event` timeline. Fires `on_timeline_reset` + individual
    /// event callbacks identically to `subscribe_room`. Sets `is_focused = true`
    /// so that `paginate_forward` can gate itself.
    #[cfg(not(test))]
    pub fn subscribe_room_at(
        &mut self,
        room_id:        &str,
        focus_event_id: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let Some(handler) = self.handler.clone() else { return err("sync not started") };

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };

        let target: matrix_sdk::ruma::OwnedEventId = match focus_event_id.try_into() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        // Drop any previous subscription for this room.
        if let Some(prev) = self.timelines.remove(&room_id) {
            for h in prev.abort_tasks { h.abort(); }
        }

        let Some(room) = client.get_room(&room_id) else { return err("room not found") };

        let focus = TimelineFocus::Event {
            target,
            num_context_events: 50,
            thread_mode: TimelineEventFocusThreadMode::Automatic {
                hide_threaded_events: false,
            },
        };

        let timeline = match self.rt.block_on(
            room.timeline_builder().with_focus(focus).build()
        ) {
            Ok(t)  => Arc::new(t),
            Err(e) => return err(format!("build focused timeline: {e}")),
        };

        let room_id_str = room_id.to_string();

        // Synchronously clear the UI for this room (same as subscribe_room).
        if let Ok(guard) = handler.lock() {
            let empty: Vec<TimelineEvent> = Vec::new();
            guard.on_timeline_reset(&room_id_str, &empty);
        }

        let (abort, fetch_abort) = Self::spawn_timeline_tasks(
            &timeline, &room, room_id_str, &handler, &client, &self.rt,
        );

        self.timelines.insert(room_id, TimelineHandle {
            timeline,
            abort_tasks: vec![abort, fetch_abort],
            is_focused:  true,
        });

        ok("")
    }

    #[cfg(test)]
    pub fn subscribe_room_at(&mut self, _room_id: &str, _focus_event_id: &str) -> OpResult {
        err("not logged in")
    }

    /// MSC3030: paginate forward in a focused timeline. Only valid after
    /// `subscribe_room_at`; returns an error for live timelines.
    /// `reached_end = true` when the timeline has reached the live end.
    #[cfg(not(test))]
    pub fn paginate_forward(&mut self, room_id: &str, count: u16) -> PaginateResult {
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return PaginateResult {
                ok: false,
                message: format!("invalid room id: {e}"),
                reached_start: false,
                reached_end:   false,
            },
        };

        let Some(handle) = self.timelines.get(&room_id) else {
            return PaginateResult {
                ok: false,
                message: "room not subscribed; call subscribe_room_at first".into(),
                reached_start: false,
                reached_end:   false,
            };
        };

        if !handle.is_focused {
            return PaginateResult {
                ok: false,
                message: "not in focused mode".into(),
                reached_start: false,
                reached_end:   false,
            };
        }

        let tl      = Arc::clone(&handle.timeline);
        let stop_rx = self.stop_rx.clone();

        match self.rt.block_on(async move {
            let paginate = tl.paginate_forwards(count);
            if let Some(mut rx) = stop_rx {
                tokio::select! {
                    result = paginate => result.map(Some),
                    _ = async {
                        loop {
                            match rx.changed().await {
                                Ok(()) => { if *rx.borrow() { return; } }
                                Err(_) => return,
                            }
                        }
                    } => Ok(None),
                }
            } else {
                paginate.await.map(Some)
            }
        }) {
            Ok(Some(reached_end)) => PaginateResult {
                ok: true,
                message: String::new(),
                reached_start: false,
                reached_end,
            },
            Ok(None) => PaginateResult {
                ok: false,
                message: "shutdown in progress".into(),
                reached_start: false,
                reached_end:   false,
            },
            Err(e) => PaginateResult {
                ok: false,
                message: e.to_string(),
                reached_start: false,
                reached_end:   false,
            },
        }
    }

    #[cfg(test)]
    pub fn paginate_forward(&mut self, _room_id: &str, _count: u16) -> PaginateResult {
        PaginateResult {
            ok: false,
            message: "not in focused mode".into(),
            reached_start: false,
            reached_end:   false,
        }
    }

    // -----------------------------------------------------------------------
    // Background backfill of non-active rooms
    // -----------------------------------------------------------------------
    //
    // Warms the SDK's sqlite event cache for every joined room the user has
    // not opened yet, so the second time they click any room its history is
    // already present locally. The active room (whichever is currently in
    // `self.timelines`) is skipped — its foreground subscribe + paginate
    // path always finishes first because UIs only call this after their
    // own paginate_back returns.
    //
    // Bounded concurrency (3 in flight) keeps the homeserver happy on
    // accounts with hundreds of rooms. Per-room timelines are dropped after
    // the loop completes; only the persistent event-cache rows remain.

    #[cfg(not(test))]
    pub fn start_background_backfill(
        &mut self,
        room_ids: &cxx::CxxVector<cxx::CxxString>,
    ) -> OpResult {
        // Idempotent: if a previous orchestrator is still running, leave it
        // alone. Finished/aborted handles can be replaced.
        if let Some(h) = self.backfill_task.as_ref() {
            if !h.is_finished() {
                return ok("");
            }
        }
        self.backfill_task = None;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };

        // Snapshot the work set up-front so the orchestrator owns no
        // borrows of `self`. Skip rooms that already have a foreground
        // Timeline (the user-active one).
        let skip: std::collections::HashSet<OwnedRoomId> =
            self.timelines.keys().cloned().collect();

        let mut to_backfill: Vec<OwnedRoomId> = Vec::new();
        for id_cxx in room_ids {
            let Ok(id_str) = id_cxx.to_str() else { continue };
            let Ok(id) = OwnedRoomId::try_from(id_str) else { continue };
            if skip.contains(&id) { continue; }
            if let Some(room) = client.get_room(&id) {
                if !room.is_tombstoned() {
                    to_backfill.push(id);
                }
            }
        }

        if to_backfill.is_empty() {
            return ok("");
        }

        let abort = self.rt.spawn(async move {
            let semaphore = Arc::new(tokio::sync::Semaphore::new(3));
            let mut joinset = tokio::task::JoinSet::new();

            for rid in to_backfill {
                let client = client.clone();
                let sem    = semaphore.clone();
                joinset.spawn(async move {
                    let _permit = match sem.acquire_owned().await {
                        Ok(p)  => p,
                        Err(_) => return,
                    };
                    let _ = backfill_room_silent(&client, &rid, 50).await;
                });
            }

            while joinset.join_next().await.is_some() {}
        }).abort_handle();

        self.backfill_task = Some(abort);
        ok("")
    }

    #[cfg(not(test))]
    pub fn stop_background_backfill(&mut self) {
        if let Some(h) = self.backfill_task.take() {
            h.abort();
        }
    }

    // -----------------------------------------------------------------------
    // Messaging
    // -----------------------------------------------------------------------

    pub fn send_message(&mut self, room_id: &str, body: &str, formatted_body: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let content = if formatted_body.is_empty() {
            RoomMessageEventContent::text_plain(body)
        } else {
            RoomMessageEventContent::text_html(body, formatted_body)
        };
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    /// Send a typing notice to `room_id`. Fire-and-forget; errors are swallowed.
    #[cfg(not(test))]
    pub fn send_typing_notice(&mut self, room_id: &str, typing: bool) {
        let Some(client) = self.client.clone() else { return };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        self.rt.spawn(async move {
            let Some(room) = client.get_room(&room_id) else { return };
            let _ = room.typing_notice(typing).await;
        });
    }

    #[cfg(test)]
    pub fn send_typing_notice(&mut self, _room_id: &str, _typing: bool) {}

    /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds the
    /// `m.in_reply_to` relation and sends via `room.send()`. Does not require
    /// `subscribe_room`. Does not add the plain-text fallback body (Tesseract
    /// renders its own quote block).
    #[cfg(not(test))]
    pub fn send_reply(&mut self, room_id: &str, event_id: &str, body: &str, formatted_body: &str) -> OpResult {
        use matrix_sdk::ruma::events::room::message::Relation;
        use matrix_sdk::ruma::events::relation::{InReplyTo, Reply};

        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let mut content = if formatted_body.is_empty() {
            RoomMessageEventContent::text_plain(body)
        } else {
            RoomMessageEventContent::text_html(body, formatted_body)
        };
        content.relates_to = Some(Relation::Reply(
            Reply::new(InReplyTo::new(event_id)),
        ));
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_reply(&mut self, _room_id: &str, _event_id: &str, _body: &str, _formatted_body: &str) -> OpResult {
        err("not logged in")
    }

    /// Trigger an async fetch of the replied-to event's details for all
    /// timeline items in `room_id` that reference `event_id` via
    /// `m.in_reply_to`. When the data arrives, the SDK re-emits every
    /// affected item as an `on_message_updated` callback so the UI can
    /// paint the quote block with the resolved sender name and body snippet.
    /// Requires `subscribe_room`. The call spawns a tokio task and returns
    /// immediately — it never blocks the UI thread.
    #[cfg(not(test))]
    pub fn fetch_reply_details(&mut self, room_id: &str, event_id: &str) -> OpResult {
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let Some(handle) = self.timelines.get(&room_id) else {
            return err("room not subscribed");
        };
        let tl = Arc::clone(&handle.timeline);
        self.rt.spawn(async move {
            let _ = tl.fetch_details_for_event(&event_id).await;
        });
        ok("")
    }

    #[cfg(test)]
    pub fn fetch_reply_details(&mut self, _: &str, _: &str) -> OpResult {
        err("not logged in")
    }

    /// Upload `bytes` (already encoded as `mime_type`) and send an `m.image`
    /// event. Caption/filename handling follows MSC2530 — see the FFI doc
    /// comment in `bridge.rs`. Returns `OpResult` with `ok=false` for
    /// invalid IDs, unknown rooms, bad mime strings, upload failures, or
    /// send failures. `width`/`height` of 0 are passed through unset.
    #[cfg(not(test))]
    pub fn send_image(
        &mut self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        width: u32,
        height: u32,
        reply_event_id: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseImageInfo};
        use matrix_sdk::ruma::UInt;
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::room::reply::{EnforceThread, Reply};
        use matrix_sdk::ruma::events::room::message::AddMentions;

        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };

        let info = BaseImageInfo {
            width:       if width  != 0 { UInt::new(width  as u64) } else { None },
            height:      if height != 0 { UInt::new(height as u64) } else { None },
            size:        UInt::new(bytes.len() as u64),
            blurhash:    None,
            is_animated: None,
        };
        let mut config = AttachmentConfig::new().info(AttachmentInfo::Image(info));
        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        if !reply_event_id.is_empty() {
            let reply_id: matrix_sdk::ruma::OwnedEventId = match reply_event_id.parse() {
                Ok(id) => id,
                Err(e) => return err(format!("invalid reply event id: {e}")),
            };
            config = config.reply(Some(Reply { event_id: reply_id, enforce_thread: EnforceThread::Unthreaded, add_mentions: AddMentions::No }));
        }

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self.rt.block_on(async move {
            room.send_attachment(filename, &mime, data, config).await
        }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_image(
        &mut self,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _width: u32,
        _height: u32,
        _reply_event_id: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Upload `bytes` as-is and send an `m.file` event. See the FFI doc
    /// comment in `bridge.rs` for caption/filename framing.
    /// matrix-sdk's `send_attachment` infers `m.file` from the absence of an
    /// image/video/audio `AttachmentInfo`; encryption is handled transparently
    /// for E2EE rooms.
    #[cfg(not(test))]
    pub fn send_file(
        &mut self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        reply_event_id: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::AttachmentConfig;
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::room::reply::{EnforceThread, Reply};
        use matrix_sdk::ruma::events::room::message::AddMentions;

        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };

        let mut config = AttachmentConfig::new();
        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        if !reply_event_id.is_empty() {
            let reply_id: matrix_sdk::ruma::OwnedEventId = match reply_event_id.parse() {
                Ok(id) => id,
                Err(e) => return err(format!("invalid reply event id: {e}")),
            };
            config = config.reply(Some(Reply { event_id: reply_id, enforce_thread: EnforceThread::Unthreaded, add_mentions: AddMentions::No }));
        }

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self.rt.block_on(async move {
            room.send_attachment(filename, &mime, data, config).await
        }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_file(
        &mut self,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _reply_event_id: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Returns the cached homeserver upload limit, lazily fetching it on the
    /// first call after login. The query (`/_matrix/media/v3/config`) is
    /// blocking but happens at most once per session.
    /// Returns 0 when unknown, the server doesn't advertise a limit, or the
    /// client is not logged in.
    #[cfg(not(test))]
    pub fn media_upload_limit(&mut self) -> u64 {
        let cached = self.media_upload_limit.load(Ordering::Relaxed);
        if cached != 0 { return cached; }

        let Some(client) = self.client.clone() else { return 0 };
        let limit = self.rt.block_on(async move {
            client.load_or_fetch_max_upload_size().await
                .map(u64::from)
                .unwrap_or(0)
        });
        if limit != 0 {
            self.media_upload_limit.store(limit, Ordering::Relaxed);
        }
        limit
    }

    #[cfg(test)]
    pub fn media_upload_limit(&mut self) -> u64 { 0 }

    /// Toggle the current user's `key` reaction on `event_id` in `room_id`.
    /// First call adds the reaction; second redacts it. Requires that
    /// `room_id` is currently subscribed via `subscribe_room` — we look up
    /// its `Timeline` handle to invoke `toggle_reaction`.
    #[cfg(not(test))]
    pub fn send_reaction(&mut self, room_id: &str, event_id: &str, key: &str) -> OpResult {
        if self.client.is_none() { return err("not logged in"); }
        if key.is_empty() { return err("reaction key is empty"); }

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        let Some(handle) = self.timelines.get(&room_id) else {
            return err("room not subscribed; call subscribe_room first");
        };
        let tl = Arc::clone(&handle.timeline);
        let item_id = TimelineEventItemId::EventId(event_id);
        let key = key.to_owned();

        match self.rt.block_on(async move {
            tl.toggle_reaction(&item_id, &key).await
        }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_reaction(&mut self, _room_id: &str, _event_id: &str, _key: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_reaction_custom(
        &mut self,
        room_id: &str,
        event_id: &str,
        key: &str,
        shortcode: &str,
    ) -> OpResult {
        if self.client.is_none() { return err("not logged in"); }
        if key.is_empty() { return err("reaction key is empty"); }

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id = event_id.to_owned();
        let key = key.to_owned();
        let shortcode = shortcode.to_owned();

        let Some(client) = self.client.clone() else { return err("not logged in") };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };

        match self.rt.block_on(async move {
            let mut content = serde_json::json!({
                "m.relates_to": {
                    "rel_type": "m.annotation",
                    "event_id": event_id,
                    "key": key,
                }
            });
            if !shortcode.is_empty() {
                content["com.beeper.reaction.shortcode"] =
                    serde_json::Value::String(shortcode);
            }
            room.send_raw("m.reaction", content).await
        }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_reaction_custom(
        &mut self,
        _room_id: &str,
        _event_id: &str,
        _key: &str,
        _shortcode: &str,
    ) -> OpResult { err("not logged in") }

    #[cfg(not(test))]
    pub fn send_read_receipt(&mut self, room_id: &str, event_id: &str) -> OpResult {
        let Some(client) = self.client.as_ref() else { return err("not logged in"); };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let room = match client.get_room(&room_id) {
            Some(r) => r,
            None    => return err("room not found"),
        };
        match self.rt.block_on(send_both_receipts(&room, event_id)) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_read_receipt(&mut self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Send public `m.read` and private `m.read.private` receipts for the
    /// latest cached event in `room_id`. Clears the unread count without
    /// requiring the room to be subscribed via `subscribe_room`.
    #[cfg(not(test))]
    pub fn mark_room_as_read(&mut self, room_id: &str) -> OpResult {
        let Some(client) = self.client.as_ref() else { return err("not logged in"); };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let room = match client.get_room(&room_id) {
            Some(r) => r,
            None    => return err("room not found"),
        };
        // Deref to the base room type to get the synchronous `latest_event()`
        // with `event_id()`. The `RoomExt` trait (from matrix-sdk-ui, in scope
        // for timeline features) would otherwise shadow it with an async version
        // that doesn't carry the event ID.
        let event_id = match std::ops::Deref::deref(&room).latest_event().event_id() {
            Some(id) => id.to_owned(),
            None     => return ok(""),
        };
        match self.rt.block_on(send_both_receipts(&room, event_id)) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn mark_room_as_read(&mut self, _room_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
    /// Wraps matrix-sdk-ui's `Timeline::redact`. The room must currently
    /// be subscribed via `subscribe_room`. Server-side permission errors
    /// (e.g. trying to redact someone else's message without power) surface
    /// as `OpResult { ok: false, message: ... }`.
    #[cfg(not(test))]
    pub fn redact_event(&mut self, room_id: &str, event_id: &str, reason: &str) -> OpResult {
        if self.client.is_none() { return err("not logged in"); }

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        let Some(handle) = self.timelines.get(&room_id) else {
            return err("room not subscribed; call subscribe_room first");
        };
        let tl = Arc::clone(&handle.timeline);
        let item_id = TimelineEventItemId::EventId(event_id);
        let reason_opt = if reason.is_empty() { None } else { Some(reason.to_owned()) };

        match self.rt.block_on(async move {
            tl.redact(&item_id, reason_opt.as_deref()).await
        }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn redact_event(&mut self, _room_id: &str, _event_id: &str, _reason: &str) -> OpResult {
        err("not logged in")
    }

    /// Edit `event_id` in `room_id` replacing its body with `new_body`.
    /// Uses `Room::make_edit_event` (builds the `m.replace` Replacement
    /// relation) then sends via `RoomSendQueue`. Only own `m.text` events
    /// can be edited; the SDK returns an error for non-own or non-text
    /// events. Does not require `subscribe_room`.
    #[cfg(not(test))]
    pub fn send_edit(&mut self, room_id: &str, event_id: &str, new_body: &str, formatted_body: &str) -> OpResult {
        use matrix_sdk::room::edit::EditedContent;

        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };
        let new_content = if formatted_body.is_empty() {
            RoomMessageEventContent::text_plain(new_body)
        } else {
            RoomMessageEventContent::text_html(new_body, formatted_body)
        };
        match self.rt.block_on(async move {
            let edit_event = room
                .make_edit_event(&event_id, EditedContent::RoomMessage(new_content.into()))
                .await
                .map_err(|e| e.to_string())?;
            room.send_queue().send(edit_event).await.map_err(|e| e.to_string())
        }) {
            Ok(_)  => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn send_edit(&mut self, _room_id: &str, _event_id: &str, _new_body: &str, _formatted_body: &str) -> OpResult {
        err("not logged in")
    }

    /// Top-N glyphs from the user's MSC4356 `recent_emoji` account-data,
    /// ordered by `total` desc. Reads with the precedence stable
    /// (`m.recent_emoji`) → unstable
    /// (`io.github.johennes.msc4356.recent_emoji`) → legacy
    /// (`io.element.recent_emoji`) so existing Element users see their
    /// historical history on the first MSC4356 run without losing it. Reads
    /// the local sync cache only — no network roundtrip. Returns an empty
    /// vec when not logged in, when no blob has ever been written, or on
    /// any deserialization error: a broken blob never stalls the picker.
    #[cfg(not(test))]
    pub fn recent_emoji_top(&mut self, n: u32) -> Vec<String> {
        let Some(client) = self.client.clone() else { return Vec::new(); };
        let entries = self.rt.block_on(async move {
            read_recent_emoji_entries(&client).await
        });
        crate::recent_emoji::top_by_count(&entries, n as usize)
    }

    #[cfg(test)]
    pub fn recent_emoji_top(&mut self, _n: u32) -> Vec<String> { Vec::new() }

    /// Record one use of `glyph` in the user's account-data. Fire-and-forget
    /// against the homeserver: the GET-modify-PUT round-trips would
    /// otherwise stall every emoji click, and the picker's "most used"
    /// ranking tolerates the occasional dropped bump on rapid input
    /// (matrix's last-write-wins account-data semantics merge cleanly on
    /// the next sync). Dual-writes the canonical `m.recent_emoji` and the
    /// unstable `io.github.johennes.msc4356.recent_emoji` so other MSC4356
    /// clients pick the data up regardless of which side has reached
    /// stable yet. The legacy `io.element.recent_emoji` blob is read on
    /// fallback but never written, leaving Element / other clients to
    /// manage their own copy.
    #[cfg(not(test))]
    pub fn recent_emoji_bump(&mut self, glyph: &str) {
        if glyph.is_empty() { return; }
        let Some(client) = self.client.clone() else { return; };
        let glyph = glyph.to_owned();
        let ad_lock = Arc::clone(&self.account_data_lock);
        self.rt.spawn(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            use matrix_sdk::ruma::serde::Raw;

            // Serialize the whole GET→modify→PUT against other account-data
            // writers so rapid bumps build on each other instead of racing.
            let _ad_guard = ad_lock.lock().await;
            let entries = read_recent_emoji_entries(&client).await;
            let bumped  = crate::recent_emoji::bump(entries, &glyph);
            let content = crate::recent_emoji::serialize_msc4356(&bumped);
            let raw = match Raw::new(&content) {
                Ok(r)  => r.cast_unchecked(),
                Err(_) => return,
            };
            // Dual-write to stable + unstable types. Errors are swallowed
            // by the fire-and-forget contract.
            for ty in [
                crate::recent_emoji::TYPE_STABLE,
                crate::recent_emoji::TYPE_UNSTABLE,
            ] {
                let ev_type = GlobalAccountDataEventType::from(ty);
                let _ = client.account()
                    .set_account_data_raw(ev_type, raw.clone())
                    .await;
            }
        });
    }

    #[cfg(test)]
    pub fn recent_emoji_bump(&mut self, _glyph: &str) {}

    // ----- Application prefs (im.gnomos.tesseract global account-data) -----

    #[cfg(not(test))]
    pub fn load_prefs(&mut self) -> String {
        let Some(client) = self.client.clone() else { return "{}".to_owned(); };
        self.rt.block_on(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            let et = GlobalAccountDataEventType::from("im.gnomos.tesseract");
            client.account()
                .account_data_raw(et).await
                .ok().flatten()
                .map(|r| r.json().get().to_owned())
                .unwrap_or_else(|| "{}".to_owned())
        })
    }

    #[cfg(test)]
    pub fn load_prefs(&mut self) -> String { "{}".to_owned() }

    #[cfg(not(test))]
    pub fn save_prefs(&mut self, json: &str) {
        let Some(client) = self.client.clone() else { return; };
        let json = json.to_owned();
        self.rt.spawn(async move {
            use matrix_sdk::ruma::events::GlobalAccountDataEventType;
            use matrix_sdk::ruma::serde::Raw;
            let Ok(raw_value) = serde_json::from_str::<serde_json::Value>(&json) else { return; };
            let Ok(raw) = Raw::new(&raw_value) else { return; };
            let et = GlobalAccountDataEventType::from("im.gnomos.tesseract");
            let _ = client.account()
                .set_account_data_raw(et, raw.cast_unchecked())
                .await;
        });
    }

    #[cfg(test)]
    pub fn save_prefs(&mut self, _json: &str) {}

    pub fn user_id(&self) -> String {
        self.client
            .as_ref()
            .and_then(|c| c.user_id())
            .map(|id| id.to_string())
            .unwrap_or_default()
    }

    pub fn current_user_display_name(&self) -> String {
        let Some(client) = self.client.clone() else { return String::new() };
        self.rt.block_on(async move {
            client
                .account()
                .get_display_name()
                .await
                .ok()
                .flatten()
                .unwrap_or_default()
        })
    }

    pub fn current_user_avatar_url(&self) -> String {
        let Some(client) = self.client.clone() else { return String::new() };
        self.rt.block_on(async move {
            client
                .account()
                .get_avatar_url()
                .await
                .ok()
                .flatten()
                .map(|u| u.to_string())
                .unwrap_or_default()
        })
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
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            tokio::select! {
                result = room.avatar(MediaFormat::File) =>
                    result.ok().flatten().unwrap_or_default(),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
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
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    /// Download media from either a plain `mxc://` URI or a JSON-serialised
    /// `MediaSource` carrying an `EncryptedFile`. The two shapes are detected
    /// by the leading `mxc:` prefix: plain URIs go through `MediaSource::Plain`
    /// and JSON payloads are deserialised as a full `MediaSource` so the SDK
    /// can decrypt encrypted attachments (MSC2545 stickers, encrypted images,
    /// etc.). Returns an empty Vec on any failure.
    pub fn fetch_source_bytes(&mut self, source: &str) -> Vec<u8> {
        use matrix_sdk::media::{MediaFormat, MediaRequestParameters};
        use matrix_sdk::ruma::events::room::MediaSource;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else { return Vec::new() };
        if source.is_empty() { return Vec::new(); }

        let media_source = if source.starts_with("mxc://") {
            let uri = OwnedMxcUri::from(source);
            if !uri.is_valid() { return Vec::new(); }
            MediaSource::Plain(uri.into())
        } else {
            match serde_json::from_str::<MediaSource>(source) {
                Ok(s)  => s,
                Err(_) => return Vec::new(),
            }
        };

        let request = MediaRequestParameters {
            source: media_source,
            format: MediaFormat::File,
        };
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            let media = client.media();
            tokio::select! {
                result = media.get_media_content(&request, true) =>
                    cap_media_bytes(result.unwrap_or_default()),
                _ = stop_fut(stop_rx) => Vec::new(),
            }
        })
    }

    // -----------------------------------------------------------------------
    // URL preview (homeserver og:* metadata fetch)
    // -----------------------------------------------------------------------

    #[cfg(not(test))]
    #[allow(deprecated)]
    pub fn get_url_preview(&mut self, url: &str) -> String {
        use ruma::api::client::media::get_media_preview::v3::Request;

        let Some(client) = self.client.clone() else { return String::new() };
        if url.is_empty() { return String::new(); }

        let url_str = url.to_owned();
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            let req = Request::new(url_str);
            tokio::select! {
                result = async { client.send(req).await } => {
                    match result {
                        Ok(resp) => resp.data
                            .map(|v| v.get().to_owned())
                            .unwrap_or_default(),
                        Err(_) => String::new(),
                    }
                }
                _ = stop_fut(stop_rx) => String::new(),
            }
        })
    }

    #[cfg(test)]
    pub fn get_url_preview(&mut self, _url: &str) -> String {
        String::new()
    }

    // -----------------------------------------------------------------------
    // MSC3266 room summary / join
    // -----------------------------------------------------------------------

    #[cfg(not(test))]
    pub fn get_room_summary(&mut self, room_id_or_alias: &str) -> String {
        use matrix_sdk::ruma::api::client::room::get_summary::v1::Request;
        use matrix_sdk::ruma::OwnedRoomOrAliasId;
        use matrix_sdk::ruma::room::{JoinRuleSummary, RoomType};

        let Some(client) = self.client.clone() else { return String::new() };
        if room_id_or_alias.is_empty() { return String::new(); }

        let id: OwnedRoomOrAliasId = match room_id_or_alias.try_into() {
            Ok(id) => id,
            Err(_) => return String::new(),
        };
        let stop_rx = self.stop_rx.clone();
        self.rt.block_on(async move {
            let req = Request::new(id, vec![]);
            tokio::select! {
                result = client.send(req) => {
                    match result {
                        Ok(resp) => {
                            let s = &resp.summary;
                            let join_rule = match &s.join_rule {
                                JoinRuleSummary::Public        => "public",
                                JoinRuleSummary::Invite        => "invite",
                                JoinRuleSummary::Knock         => "knock",
                                JoinRuleSummary::KnockRestricted(_) => "knock_restricted",
                                JoinRuleSummary::Restricted(_) => "restricted",
                                JoinRuleSummary::Private       => "private",
                                _                              => "unknown",
                            };
                            let encryption = s.encryption.as_ref()
                                .map(|e| e.as_str())
                                .unwrap_or("");
                            let is_space = matches!(s.room_type, Some(RoomType::Space));
                            let membership = resp.membership.as_ref()
                                .map(|m| m.as_str())
                                .unwrap_or("");
                            serde_json::json!({
                                "room_id":            s.room_id.as_str(),
                                "canonical_alias":    s.canonical_alias.as_ref().map(|a| a.as_str()).unwrap_or(""),
                                "name":               s.name.as_deref().unwrap_or(""),
                                "topic":              s.topic.as_deref().unwrap_or(""),
                                "avatar_url":         s.avatar_url.as_ref().map(|u| u.as_str()).unwrap_or(""),
                                "num_joined_members": u64::from(s.num_joined_members),
                                "join_rule":          join_rule,
                                "world_readable":     s.world_readable,
                                "guest_can_join":     s.guest_can_join,
                                "encryption":         encryption,
                                "is_space":           is_space,
                                "membership":         membership,
                            }).to_string()
                        }
                        Err(_) => String::new(),
                    }
                }
                _ = stop_fut(stop_rx) => String::new(),
            }
        })
    }

    #[cfg(test)]
    pub fn get_room_summary(&mut self, _room_id_or_alias: &str) -> String {
        String::new()
    }

    #[cfg(not(test))]
    pub fn join_room(&mut self, room_id_or_alias: &str) -> String {
        use matrix_sdk::ruma::OwnedRoomOrAliasId;

        let Some(client) = self.client.clone() else { return String::new() };
        if room_id_or_alias.is_empty() { return String::new(); }

        let id: OwnedRoomOrAliasId = match room_id_or_alias.try_into() {
            Ok(id) => id,
            Err(_) => return String::new(),
        };
        let stop_rx = self.stop_rx.clone();
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
    pub fn join_room(&mut self, _room_id_or_alias: &str) -> String {
        String::new()
    }

    // -----------------------------------------------------------------------
    // Homeserver discovery
    // -----------------------------------------------------------------------

    // Fetch .well-known/matrix/client and return m.homeserver.base_url, or
    // None if the server returned non-2xx or the key is absent.
    #[cfg(not(test))]
    async fn fetch_well_known(http: &reqwest::Client, server: &str) -> Option<String> {
        let url  = format!("https://{}/.well-known/matrix/client", server);
        let resp = http.get(&url).send().await.ok()?;
        if !resp.status().is_success() { return None; }
        let body: serde_json::Value = resp.json().await.ok()?;
        let base = body["m.homeserver"]["base_url"].as_str()?.trim_end_matches('/').to_owned();
        Some(base)
    }

    // Confirm the candidate base URL actually speaks Matrix by hitting
    // /_matrix/client/versions and expecting any 2xx response.
    #[cfg(not(test))]
    async fn validate_homeserver(http: &reqwest::Client, base_url: &str) -> bool {
        let url = format!("{}/_matrix/client/versions", base_url);
        http.get(&url).send().await.map(|r| r.status().is_success()).unwrap_or(false)
    }

    /// Discover the homeserver base URL for a server name or Matrix ID.
    /// Returns JSON: `{"base_url":"https://...","error":""}` on success or
    /// `{"base_url":"","error":"..."}` on failure. Uses raw HTTP — no SDK
    /// Client construction required.
    #[cfg(not(test))]
    pub fn discover_homeserver(&mut self, server_name_or_mxid: &str) -> String {
        let input = server_name_or_mxid.trim();

        // Extract server name from a full MXID (@user:server.org → server.org).
        let server = if input.starts_with('@') {
            match input.find(':') {
                Some(i) => &input[i + 1..],
                None    => return r#"{"base_url":"","error":"Invalid Matrix ID — expected @user:server"}"#.to_owned(),
            }
        } else {
            input
        };

        if server.is_empty() {
            return r#"{"base_url":"","error":""}"#.to_owned();
        }

        let server = server.to_owned();
        self.rt.block_on(async move {
            let http = match reqwest::Client::builder()
                .timeout(std::time::Duration::from_secs(5))
                .build()
            {
                Ok(c)  => c,
                Err(e) => {
                    let msg = e.to_string().replace('\\', "\\\\").replace('"', "\\\"").replace('\n', " ");
                    return format!(r#"{{"base_url":"","error":"{msg}"}}"#);
                }
            };

            let base_url = if server.starts_with("https://") || server.starts_with("http://") {
                // Caller passed a full URL — validate it directly.
                let candidate = server.trim_end_matches('/').to_owned();
                if Self::validate_homeserver(&http, &candidate).await { Some(candidate) } else { None }
            } else {
                // Try .well-known first; fall back to https://{server} on failure.
                let candidate = Self::fetch_well_known(&http, &server)
                    .await
                    .unwrap_or_else(|| format!("https://{}", server));
                if Self::validate_homeserver(&http, &candidate).await { Some(candidate) } else { None }
            };

            match base_url {
                Some(url) => format!(r#"{{"base_url":"{url}","error":""}}"#),
                None => {
                    let msg = format!("Could not reach homeserver at {server}");
                    format!(r#"{{"base_url":"","error":"{msg}"}}"#)
                }
            }
        })
    }

    #[cfg(test)]
    pub fn discover_homeserver(&mut self, _: &str) -> String {
        r#"{"base_url":"","error":""}"#.to_owned()
    }

    // -----------------------------------------------------------------------
    // MSC2545 image packs (Step 8)
    // -----------------------------------------------------------------------

    /// Snapshot of every MSC2545 image pack the client currently knows about.
    /// Reads the in-memory `image_packs` cache populated by the sync watcher
    /// and the explicit `refresh_image_packs_async` rebuilds; no network
    /// roundtrip.
    #[cfg(not(test))]
    pub fn list_image_packs(&self) -> Vec<crate::ffi::ImagePackFfi> {
        let Ok(cache) = self.image_packs.lock() else { return Vec::new() };
        cache
            .iter()
            .map(|p| crate::ffi::ImagePackFfi {
                id:               p.id.clone(),
                display_name:     p.display_name.clone(),
                avatar_url:       p.avatar_url.clone(),
                attribution:      p.attribution.clone(),
                usage_mask:       p.usage,
                source_kind:      p.source_kind().to_owned(),
                source_room:      p.source_room().to_owned(),
                source_state_key: p.source_state_key().to_owned(),
            })
            .collect()
    }

    #[cfg(test)]
    pub fn list_image_packs(&self) -> Vec<crate::ffi::ImagePackFfi> { Vec::new() }

    /// Return every entry in `pack_id` whose usage mask intersects
    /// `usage_filter` ("sticker" | "emoticon" | "any" — anything else is
    /// treated as "any"). When `pack_id` doesn't exist, returns empty.
    #[cfg(not(test))]
    pub fn list_pack_images(
        &self,
        pack_id: &str,
        usage_filter: &str,
    ) -> Vec<crate::ffi::ImageEntryFfi> {
        let needed = match usage_filter {
            "sticker"  => crate::image_packs::USAGE_STICKER,
            "emoticon" => crate::image_packs::USAGE_EMOTICON,
            _          => crate::image_packs::USAGE_ANY,
        };
        let Ok(cache) = self.image_packs.lock() else { return Vec::new() };
        let Some(pack) = cache.iter().find(|p| p.id == pack_id) else { return Vec::new() };
        pack.images
            .iter()
            .filter(|e| e.usage & needed != 0)
            .map(|e| image_entry_to_ffi(&pack.id, e))
            .collect()
    }

    #[cfg(test)]
    pub fn list_pack_images(
        &self,
        _pack_id: &str,
        _usage_filter: &str,
    ) -> Vec<crate::ffi::ImageEntryFfi> { Vec::new() }

    /// Flatten every favourite-marked entry across all packs. Sticker-usage
    /// only (Favorites tab is sticker-specific).
    #[cfg(not(test))]
    pub fn list_favorite_stickers(&self) -> Vec<crate::ffi::ImageEntryFfi> {
        let Ok(cache) = self.image_packs.lock() else { return Vec::new() };
        let mut out = Vec::new();
        for pack in cache.iter() {
            for e in &pack.images {
                if e.favorite && (e.usage & crate::image_packs::USAGE_STICKER != 0) {
                    out.push(image_entry_to_ffi(&pack.id, e));
                }
            }
        }
        out
    }

    #[cfg(test)]
    pub fn list_favorite_stickers(&self) -> Vec<crate::ffi::ImageEntryFfi> { Vec::new() }

    /// Send an `m.sticker` event to `room_id`. Wraps
    /// `room.send(StickerEventContent { .. })`. matrix-sdk encrypts in E2EE
    /// rooms transparently; outgoing stickers always carry a plain
    /// `mxc://` source.
    #[cfg(not(test))]
    pub fn send_sticker(
        &mut self,
        room_id: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::sticker::{StickerEventContent, StickerMediaSource};
        use matrix_sdk::ruma::events::room::ImageInfo;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else { return err("not logged in") };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else { return err("room not found") };

        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let uri = OwnedMxcUri::from(image_url);
        if !uri.is_valid() {
            return err("image_url is not a valid mxc:// uri");
        }

        let info: ImageInfo = if info_json.is_empty() || info_json == "{}" {
            ImageInfo::new()
        } else {
            match serde_json::from_str(info_json) {
                Ok(i) => i,
                Err(_) => ImageInfo::new(),
            }
        };

        // ruma's StickerEventContent::new takes a plain mxc URI directly;
        // matrix-sdk handles E2EE rooms transparently when sending. The
        // `StickerMediaSource` enum (Plain / Encrypted) is only meaningful
        // for received events (parsed under the `compat-encrypted-stickers`
        // feature).
        let _ = StickerMediaSource::Plain(uri.clone()); // keep import path used
        let content = StickerEventContent::new(body.to_owned(), info, uri);

        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_sticker(
        &mut self,
        _room_id: &str,
        _body: &str,
        _image_url: &str,
        _info_json: &str,
    ) -> OpResult { err("not logged in") }

    /// Add a sticker to the user's MSC2545 personal pack
    /// (`im.ponies.user_emotes`). Reads the current content (creating an
    /// empty object on first use), inserts the new entry under a
    /// collision-free shortcode derived from `shortcode` or `body`, writes
    /// the result back via `set_account_data_raw`, then triggers a local
    /// cache rebuild.
    #[cfg(not(test))]
    pub fn save_sticker_to_user_pack(
        &mut self,
        shortcode: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else { return err("not logged in") };

        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let uri = matrix_sdk::ruma::OwnedMxcUri::from(image_url);
        if !uri.is_valid() {
            return err("image_url is not a valid mxc:// uri");
        }

        let ev_type =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK_UNSTABLE);

        // Hold the account-data lock across the whole read→modify→write so a
        // concurrent toggle_favorite / save cannot clobber this change.
        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        let client_for_read = client.clone();
        let read_result = self.rt.block_on(async move {
            client_for_read.account().account_data_raw(ev_type.clone()).await
        });

        let current_content: Value = match read_result {
            Ok(Some(raw)) => serde_json::from_str(raw.json().get())
                .unwrap_or_else(|_| Value::Object(serde_json::Map::new())),
            Ok(None) => Value::Object(serde_json::Map::new()),
            Err(e)   => return err(format!("read user pack: {e}")),
        };

        // Compute final shortcode (collision-free) against the existing
        // images map so re-saving the same sticker doesn't shadow a
        // pre-existing entry.
        let existing_images = current_content
            .get("images")
            .and_then(Value::as_object)
            .cloned()
            .unwrap_or_default();

        if crate::image_packs::pack_contains_url(&current_content, image_url) {
            self.update_user_pack_in_cache(&current_content);
            return ok("");
        }

        let base = if shortcode.is_empty() { body } else { shortcode };
        let final_shortcode =
            crate::image_packs::suggest_shortcode(base, &existing_images);

        let new_content = crate::image_packs::upsert_image_into_user_pack(
            current_content,
            &final_shortcode,
            image_url,
            body,
            info_json,
            None,
            "Saved Stickers",
        );

        let raw = match Raw::new(&new_content) {
            Ok(r)  => r.cast_unchecked(),
            Err(e) => return err(format!("serialize user pack: {e}")),
        };

        let client_for_write = client.clone();
        let ev_type_for_write =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK_UNSTABLE);
        let write_result = self.rt.block_on(async move {
            client_for_write
                .account()
                .set_account_data_raw(ev_type_for_write, raw)
                .await
        });

        match write_result {
            Ok(_) => {
                // Directly update the in-memory cache from the new_content we
                // already have — the state store won't reflect the write until
                // the next sync cycle, so rebuild_image_packs would read stale
                // data if we called refresh_image_packs_blocking here.
                self.update_user_pack_in_cache(&new_content);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn save_sticker_to_user_pack(
        &mut self,
        _shortcode: &str,
        _body: &str,
        _image_url: &str,
        _info_json: &str,
    ) -> OpResult { err("not logged in") }

    #[cfg(not(test))]
    pub fn user_pack_has_sticker(&self, image_url: &str) -> bool {
        if image_url.is_empty() { return false; }
        let Ok(cache) = self.image_packs.lock() else { return false };
        cache.iter()
            .filter(|p| matches!(p.source, crate::image_packs::PackSource::User))
            .flat_map(|p| p.images.iter())
            .any(|e| e.url == image_url)
    }

    #[cfg(test)]
    pub fn user_pack_has_sticker(&self, _image_url: &str) -> bool { false }

    #[cfg(not(test))]
    pub fn toggle_favorite_sticker(&mut self, image_url: &str) -> OpResult {
        use matrix_sdk::ruma::events::GlobalAccountDataEventType;
        use matrix_sdk::ruma::serde::Raw;
        use serde_json::Value;

        let Some(client) = self.client.clone() else { return err("not logged in") };
        if image_url.is_empty() { return err("image_url is empty"); }

        let ev_type =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK_UNSTABLE);

        // Hold the account-data lock across the whole read→modify→write so a
        // concurrent save / favorite toggle cannot clobber this change.
        let _ad_guard = {
            let l = Arc::clone(&self.account_data_lock);
            self.rt.block_on(async move { l.lock_owned().await })
        };

        let client_for_read = client.clone();
        let read_result = self.rt.block_on(async move {
            client_for_read.account().account_data_raw(ev_type).await
        });

        let current: Value = match read_result {
            Ok(Some(raw)) => serde_json::from_str(raw.json().get())
                .unwrap_or_else(|_| Value::Object(serde_json::Map::new())),
            Ok(None) => return err("sticker is not saved; add it before favoriting"),
            Err(e)   => return err(format!("read user pack: {e}")),
        };

        let (new_content, _new_state) =
            crate::image_packs::toggle_favorite_in_user_pack(current, image_url);

        let raw = match Raw::new(&new_content) {
            Ok(r)  => r.cast_unchecked(),
            Err(e) => return err(format!("serialize user pack: {e}")),
        };
        let client_for_write = client.clone();
        let ev_type_for_write =
            GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK_UNSTABLE);
        let write_result = self.rt.block_on(async move {
            client_for_write.account().set_account_data_raw(ev_type_for_write, raw).await
        });

        match write_result {
            Ok(_) => {
                self.update_user_pack_in_cache(&new_content);
                ok("")
            }
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn toggle_favorite_sticker(&mut self, _image_url: &str) -> OpResult {
        err("not logged in")
    }

    /// Update only the user pack slot in the in-memory cache from `content`
    /// (already in memory — no state-store round-trip). Fires
    /// `on_image_packs_updated` so the UI refreshes immediately.
    #[cfg(not(test))]
    fn update_user_pack_in_cache(&self, content: &serde_json::Value) {
        let Some(mut pack) = crate::image_packs::parse_pack_content(
            "user".to_owned(),
            crate::image_packs::PackSource::User,
            content,
        ) else { return };
        if pack.display_name.is_empty() {
            pack.display_name = "Saved Stickers".to_owned();
        }
        if let Ok(mut cache) = self.image_packs.lock() {
            if let Some(slot) = cache.iter_mut()
                .find(|p| p.source == crate::image_packs::PackSource::User)
            {
                *slot = pack;
            } else {
                cache.insert(0, pack);
            }
        }
        if let Some(h) = &self.handler {
            if let Ok(g) = h.lock() { g.on_image_packs_updated(); }
        }
    }

    // -----------------------------------------------------------------------
    // Recovery / key backup (Step 6)
    // -----------------------------------------------------------------------

    /// Returns true when this device is missing the cross-signing / backup
    /// secrets that are already present in server-side secret storage. The
    /// UI surfaces a "Verify this device" banner when this is true.
    #[cfg(not(test))]
    pub fn needs_recovery(&self) -> bool {
        let Some(client) = self.client.as_ref() else { return false };
        // `Recovery::state()` is a synchronous observable read; no runtime
        // block_on needed (the previous block_on of an already-ready future
        // added no value and ran on the UI thread).
        use matrix_sdk::encryption::recovery::RecoveryState;
        matches!(client.encryption().recovery().state(), RecoveryState::Incomplete)
    }

    #[cfg(test)]
    pub fn needs_recovery(&self) -> bool { false }

    /// Unlock the server-side secret storage with the supplied recovery key
    /// (or passphrase), importing the cross-signing private keys and the
    /// backup decryption key into this device. The actual backup download
    /// runs asynchronously; observe `on_backup_progress` for progress.
    #[cfg(not(test))]
    pub fn recover(&mut self, key_or_passphrase: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        if key_or_passphrase.trim().is_empty() {
            return err("recovery key or passphrase is empty");
        }
        let key = key_or_passphrase.to_owned();
        match self.rt.block_on(async move {
            client.encryption().recovery().recover(&key).await
        }) {
            Ok(())  => ok(""),
            Err(e)  => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn recover(&mut self, _key_or_passphrase: &str) -> OpResult {
        err("not logged in")
    }

    /// Snapshot of the current backup state plus the running imported-key
    /// counter. Cheap; reads atomic state populated by the watcher task.
    #[cfg(not(test))]
    pub fn backup_state(&self) -> BackupProgress {
        BackupProgress {
            state:         self.backup_state_code.load(Ordering::Relaxed),
            imported_keys: self.imported_keys.load(Ordering::Relaxed),
            total_keys:    0,
        }
    }

    #[cfg(test)]
    pub fn backup_state(&self) -> BackupProgress { backup_progress_default() }

    // -----------------------------------------------------------------------
    // SAS device verification
    // -----------------------------------------------------------------------

    /// Initiate an `m.key.verification.request` to every other device of the
    /// current user. `on_verification_request(incoming=false)` fires when one
    /// device accepts; the UI should then call `start_sas(flow_id)`.
    #[cfg(not(test))]
    pub fn request_self_verification(&mut self) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let Some(handler) = self.handler.clone() else { return err("not syncing") };
        let flow_users   = Arc::clone(&self.verification_flow_users);
        let emoji_cache  = Arc::clone(&self.sas_emoji_cache);

        match self.rt.block_on(async move {
            let user_id = client.user_id()
                .ok_or_else(|| anyhow::anyhow!("not logged in"))?
                .to_owned();
            // Use the user identity (not the own device) so the request is
            // broadcast to all other E2EE sessions — not looped back to this
            // device, which would show an unwanted incoming-request banner.
            let identity = client.encryption()
                .get_user_identity(&user_id)
                .await?
                .ok_or_else(|| anyhow::anyhow!("own identity not found"))?;
            let req = identity.request_verification().await?;

            let flow_id = req.flow_id().to_owned();
            let user_id = req.own_user_id().as_str().to_owned();
            lock_or_recover(&flow_users).insert(flow_id.clone(), user_id);

            tokio::spawn(watch_verification_request(
                req, flow_id, handler, flow_users, emoji_cache,
            ));
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(())  => ok(""),
            Err(e)  => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn request_self_verification(&mut self) -> OpResult { err("not logged in") }

    /// Accept an incoming verification request. Call after receiving
    /// `on_verification_request(incoming=true)`, then call `start_sas`.
    #[cfg(not(test))]
    pub fn accept_verification(&mut self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let user_id = match lock_or_recover(&self.verification_flow_users).get(flow_id).cloned() {
            Some(u) => u,
            None    => return err("no pending verification request for this flow_id"),
        };
        let flow_id = flow_id.to_owned();
        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let req = client.encryption()
                .get_verification_request(uid, &flow_id)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification request not found"))?;
            req.accept().await?;
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(())  => ok(""),
            Err(e)  => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn accept_verification(&mut self, _flow_id: &str) -> OpResult { err("not logged in") }

    /// Start the SAS key exchange. `on_sas_ready` fires when the 7 emoji are
    /// computed. Call after `accept_verification` (incoming) or after
    /// `on_verification_request(incoming=false)` (outgoing).
    #[cfg(not(test))]
    pub fn start_sas(&mut self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let user_id = match lock_or_recover(&self.verification_flow_users).get(flow_id).cloned() {
            Some(u) => u,
            None    => return err("no pending verification request for this flow_id"),
        };
        let flow_id_str  = flow_id.to_owned();
        let Some(handler) = self.handler.clone() else { return err("not syncing") };
        let emoji_cache  = Arc::clone(&self.sas_emoji_cache);

        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            let uid = <&UserId>::try_from(user_id.as_str())?;
            let req = client.encryption()
                .get_verification_request(uid, &flow_id_str)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification request not found"))?;
            let sas = req.start_sas().await?
                .ok_or_else(|| anyhow::anyhow!("SAS not supported"))?;
            tokio::spawn(watch_sas(sas, flow_id_str, handler, emoji_cache));
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(())  => ok(""),
            Err(e)  => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn start_sas(&mut self, _flow_id: &str) -> OpResult { err("not logged in") }

    /// Confirm that the SAS emoji match. Fires `on_verification_done` when
    /// both sides confirm. Call from the "They Match" button handler.
    #[cfg(not(test))]
    pub fn confirm_sas(&mut self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let user_id = match lock_or_recover(&self.verification_flow_users).get(flow_id).cloned() {
            Some(u) => u,
            None    => return err("no active SAS for this flow_id"),
        };
        let flow_id = flow_id.to_owned();
        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            use matrix_sdk::encryption::verification::Verification;
            let uid  = <&UserId>::try_from(user_id.as_str())?;
            let verif = client.encryption()
                .get_verification(uid, &flow_id)
                .await
                .ok_or_else(|| anyhow::anyhow!("verification not found"))?;
            if let Verification::SasV1(sas) = verif {
                sas.confirm().await?;
            }
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(())  => ok(""),
            Err(e)  => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn confirm_sas(&mut self, _flow_id: &str) -> OpResult { err("not logged in") }

    /// Cancel or decline a verification flow (mismatch or user dismiss).
    #[cfg(not(test))]
    pub fn cancel_verification(&mut self, flow_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else { return err("not logged in") };
        let user_id = match lock_or_recover(&self.verification_flow_users).get(flow_id).cloned() {
            Some(u) => u,
            None    => return err("no active verification for this flow_id"),
        };
        let flow_id = flow_id.to_owned();
        match self.rt.block_on(async move {
            use matrix_sdk::ruma::UserId;
            use matrix_sdk::encryption::verification::Verification;
            let uid   = <&UserId>::try_from(user_id.as_str())?;
            // Try the SAS object first; fall back to the request.
            if let Some(Verification::SasV1(sas)) = client.encryption()
                .get_verification(uid, &flow_id).await
            {
                sas.cancel().await?;
            } else if let Some(req) = client.encryption()
                .get_verification_request(uid, &flow_id).await
            {
                req.cancel().await?;
            }
            Ok::<(), anyhow::Error>(())
        }) {
            Ok(())  => ok(""),
            Err(e)  => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn cancel_verification(&mut self, _flow_id: &str) -> OpResult { err("not logged in") }

    /// Return the 7 SAS emoji for `flow_id` after `on_sas_ready` has fired.
    #[cfg(not(test))]
    pub fn get_sas_emojis(&self, flow_id: &str) -> Vec<VerificationEmoji> {
        lock_or_recover(&self.sas_emoji_cache)
            .get(flow_id)
            .map(|pairs| pairs.iter().map(|(sym, desc)| VerificationEmoji {
                symbol:      sym.clone(),
                description: desc.clone(),
            }).collect())
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

        // Abort each timeline's background tasks before dropping the handles,
        // mirroring Drop. Plain `.clear()` drops the AbortHandles without
        // cancelling the spawned futures, which would keep an Arc<Timeline>
        // (and the HTTP client) alive past the token revocation below.
        #[cfg(not(test))]
        {
            let _guard = self.rt.enter();
            for (_, th) in self.timelines.drain() {
                for h in th.abort_tasks { h.abort(); }
            }
        }
        #[cfg(not(test))]
        {
            self.imported_keys.store(0, Ordering::Relaxed);
            self.backup_state_code.store(BACKUP_STATE_UNKNOWN, Ordering::Relaxed);
        }
        self.media_upload_limit.store(0, Ordering::Relaxed);

        let Some(client) = self.client.take() else {
            let _ = std::fs::remove_dir_all(&self.data_dir);
            return ok("");
        };

        let revoke = self.rt.block_on(async move {
            client.oauth().logout().await
        });

        let _ = std::fs::remove_dir_all(&self.data_dir);

        match revoke {
            Ok(_)  => ok(""),
            Err(e) => err(format!("oauth logout failed (local store cleared): {e}")),
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Resolves once the stop watch channel fires `true` or the sender is dropped.
/// When `stop_rx` is `None` (sync not yet started) the future never resolves,
/// so `tokio::select!` callers will just wait for the other branch.
async fn stop_fut(stop_rx: Option<watch::Receiver<bool>>) {
    let Some(mut rx) = stop_rx else {
        std::future::pending::<()>().await;
        return;
    };
    loop {
        match rx.changed().await {
            Ok(()) => { if *rx.borrow() { return; } }
            Err(_) => return,
        }
    }
}

/// Warm the SDK's event-cache for one room without surfacing anything to
/// the UI. Builds a temporary `Timeline`, drops the live diff stream, and
/// paginates backwards in 50-event batches until either the room has at
/// least `target_events` event items locally or matrix-sdk reports that
/// we've reached the start of the timeline.
///
/// The `Timeline` is dropped on return; rows committed to the SDK's sqlite
/// event cache during pagination persist, so the next foreground
/// `subscribe_room` for this room paints from cache without a /messages
/// round-trip.
#[cfg(not(test))]
async fn backfill_room_silent(
    client:        &Client,
    room_id:       &OwnedRoomId,
    target_events: usize,
) -> anyhow::Result<()> {
    let Some(room) = client.get_room(room_id) else {
        return Ok(());
    };

    let timeline = room.timeline().await?;

    // We don't propagate items to the UI, so subscribe + drop the stream.
    // The initial snapshot tells us how much history is already cached.
    let (initial, _stream) = timeline.subscribe().await;
    let mut have = initial.iter()
        .filter(|i| matches!(i.kind(), TimelineItemKind::Event(_)))
        .count();

    while have < target_events {
        match timeline.paginate_backwards(50).await {
            Ok(true)  => break,           // reached the start
            Ok(false) => {}
            Err(_)    => break,           // soft-fail: no point spinning
        }
        have = timeline.items().await.iter()
            .filter(|i| matches!(i.kind(), TimelineItemKind::Event(_)))
            .count();
    }

    Ok(())
}

/// Convert an `ImageEntry` from the cache to the FFI shape, attaching the
/// pack id so the UI can group rows back to packs without re-traversing the
/// cache.
#[cfg(not(test))]
fn image_entry_to_ffi(
    pack_id: &str,
    e:       &crate::image_packs::ImageEntry,
) -> crate::ffi::ImageEntryFfi {
    crate::ffi::ImageEntryFfi {
        pack_id:    pack_id.to_owned(),
        shortcode:  e.shortcode.clone(),
        url:        e.url.clone(),
        body:       e.body.clone(),
        info_json:  e.info_json.clone(),
        usage_mask: e.usage,
        favorite:   e.favorite,
    }
}

/// Rebuild the full `Vec<ImagePack>` from the SDK state store: read the user
/// pack from account_data, read the enabled-rooms list, and for each
/// referenced (room_id, state_key) read the room state event. Tries the
/// unstable type names first (everything in the wild today) and falls back
/// Read the user's MSC4356 recent-emoji blob with stable → unstable →
/// legacy precedence. Returns an empty Vec if no blob exists, the client
/// is in a fresh-login state, or every parse path errors out.
#[cfg(not(test))]
async fn read_recent_emoji_entries(client: &Client) -> Vec<crate::recent_emoji::Entry> {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    use serde_json::Value;

    async fn fetch(client: &Client, ty: &str) -> Option<Value> {
        let et = GlobalAccountDataEventType::from(ty);
        let raw = client.account().account_data_raw(et).await.ok().flatten()?;
        serde_json::from_str::<Value>(raw.json().get()).ok()
    }

    if let Some(v) = fetch(client, crate::recent_emoji::TYPE_STABLE).await {
        let entries = crate::recent_emoji::parse_msc4356(&v);
        if !entries.is_empty() { return entries; }
    }
    if let Some(v) = fetch(client, crate::recent_emoji::TYPE_UNSTABLE).await {
        let entries = crate::recent_emoji::parse_msc4356(&v);
        if !entries.is_empty() { return entries; }
    }
    if let Some(v) = fetch(client, crate::recent_emoji::TYPE_LEGACY).await {
        return crate::recent_emoji::parse_legacy_element(&v);
    }
    Vec::new()
}

/// Read the raw JSON content of the `im.gnomos.tesseract` account-data event
/// from the SDK's local sync cache. Returns `"{}"` when missing or on error.
#[cfg(not(test))]
async fn read_prefs_json(client: &Client) -> String {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    let et = GlobalAccountDataEventType::from("im.gnomos.tesseract");
    client.account()
        .account_data_raw(et).await
        .ok().flatten()
        .map(|r| r.json().get().to_owned())
        .unwrap_or_else(|| "{}".to_owned())
}

/// to the stable names. Returns an empty Vec when not logged in.
#[cfg(not(test))]
async fn rebuild_image_packs(client: &Client) -> Vec<crate::image_packs::ImagePack> {
    use matrix_sdk::ruma::events::{GlobalAccountDataEventType, StateEventType};
    use serde_json::Value;

    let mut packs: Vec<crate::image_packs::ImagePack> = Vec::new();

    // -- User pack (account_data) --
    for ev_type_str in [
        crate::image_packs::TYPE_USER_PACK_UNSTABLE,
        crate::image_packs::TYPE_USER_PACK_STABLE,
    ] {
        let et = GlobalAccountDataEventType::from(ev_type_str);
        let Ok(Some(raw)) = client.account().account_data_raw(et).await else { continue };
        let Ok(content) = serde_json::from_str::<Value>(raw.json().get()) else { continue };
        let Some(mut pack) = crate::image_packs::parse_pack_content(
            "user".to_owned(),
            crate::image_packs::PackSource::User,
            &content,
        ) else { continue };
        if pack.display_name.is_empty() {
            pack.display_name = "Saved Stickers".to_owned();
        }
        packs.push(pack);
        break;
    }

    // -- Globally enabled room packs (account_data) --
    let mut room_refs: Vec<(String, String)> = Vec::new();
    for ev_type_str in [
        crate::image_packs::TYPE_EMOTE_ROOMS_UNSTABLE,
        crate::image_packs::TYPE_EMOTE_ROOMS_STABLE,
    ] {
        let et = GlobalAccountDataEventType::from(ev_type_str);
        let Ok(Some(raw)) = client.account().account_data_raw(et).await else { continue };
        let Ok(content) = serde_json::from_str::<Value>(raw.json().get()) else { continue };
        room_refs = crate::image_packs::iter_emote_rooms(&content);
        break;
    }

    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    for (room_id_str, state_key) in room_refs {
        let Ok(room_id) = room_id_str.parse::<OwnedRoomId>() else { continue };

        // Helper: extract content Value from a state event envelope.
        let extract_content = |raw_state: &RawAnySyncOrStrippedState| -> Option<Value> {
            match raw_state {
                RawAnySyncOrStrippedState::Sync(raw)     => raw.get_field("content").ok().flatten(),
                RawAnySyncOrStrippedState::Stripped(raw) => raw.get_field("content").ok().flatten(),
            }
        };

        let mut found = false;

        // Fast path: local SSS cache.
        if let Some(room) = client.get_room(&room_id) {
            for ev_type_str in [
                crate::image_packs::TYPE_ROOM_PACK_UNSTABLE,
                crate::image_packs::TYPE_ROOM_PACK_STABLE,
            ] {
                let et = StateEventType::from(ev_type_str);
                // `RawAnySyncOrStrippedState` is an untagged enum wrapping a
                // `Raw<AnySyncStateEvent>` or `Raw<AnyStrippedStateEvent>`. Both
                // variants carry `{type, content, state_key,...}`.
                let Ok(Some(raw_state)) = room.get_state_event(et, &state_key).await else { continue };
                let Some(content) = extract_content(&raw_state) else { continue };
                let source = crate::image_packs::PackSource::Room {
                    room_id:   room_id_str.clone(),
                    state_key: state_key.clone(),
                };
                let id = crate::image_packs::pack_id_for(&source);
                if let Some(pack) = crate::image_packs::parse_pack_content(id, source, &content) {
                    packs.push(pack);
                    found = true;
                    break;
                }
            }
        }

        // HTTP fallback: SSS required_state does not include custom event types
        // (im.ponies.room_emotes / m.room.image_pack), so they are absent from
        // the local cache. For explicitly subscribed rooms the number is small,
        // so one HTTP round-trip per missing room is acceptable.
        if !found {
            use matrix_sdk::ruma::api::client::state::get_state_event_for_key;
            for ev_type_str in [
                crate::image_packs::TYPE_ROOM_PACK_UNSTABLE,
                crate::image_packs::TYPE_ROOM_PACK_STABLE,
            ] {
                let et = StateEventType::from(ev_type_str);
                let req = get_state_event_for_key::v3::Request::new(
                    room_id.clone(),
                    et,
                    state_key.clone(),
                );
                let Ok(response) = client.send(req).await else { continue };
                let Ok(content) = serde_json::from_str::<Value>(response.event_or_content.get()) else { continue };
                let source = crate::image_packs::PackSource::Room {
                    room_id:   room_id_str.clone(),
                    state_key: state_key.clone(),
                };
                let id = crate::image_packs::pack_id_for(&source);
                if let Some(pack) = crate::image_packs::parse_pack_content(id, source, &content) {
                    packs.push(pack);
                    break;
                }
            }
        }
    }

    // -- Room packs from ALL joined rooms (implicit membership, beyond the
    //    explicit im.ponies.emote_rooms subscription list above) --
    // This surfaces image packs published by rooms the user is a member of
    // without requiring them to have explicitly subscribed via account data.
    // Dedup against packs already added above.
    let mut added_ids: std::collections::HashSet<String> =
        packs.iter().map(|p| p.id.clone()).collect();

    for room in client.joined_rooms() {
        let room_id_str = room.room_id().to_string();
        for ev_type_str in [
            crate::image_packs::TYPE_ROOM_PACK_UNSTABLE,
            crate::image_packs::TYPE_ROOM_PACK_STABLE,
        ] {
            let et = StateEventType::from(ev_type_str);
            let Ok(events) = room.get_state_events(et).await else { continue };
            for raw_state in &events {
                let (state_key, content_opt): (String, Option<Value>) = match raw_state {
                    RawAnySyncOrStrippedState::Sync(raw) => (
                        raw.get_field("state_key").ok().flatten().unwrap_or_default(),
                        raw.get_field("content").ok().flatten(),
                    ),
                    RawAnySyncOrStrippedState::Stripped(raw) => (
                        raw.get_field("state_key").ok().flatten().unwrap_or_default(),
                        raw.get_field("content").ok().flatten(),
                    ),
                };
                let source = crate::image_packs::PackSource::Room {
                    room_id:   room_id_str.clone(),
                    state_key: state_key.clone(),
                };
                let id = crate::image_packs::pack_id_for(&source);
                if !added_ids.insert(id.clone()) { continue; }
                let Some(content) = content_opt else { continue };
                if let Some(mut pack) =
                    crate::image_packs::parse_pack_content(id, source, &content)
                {
                    if pack.display_name.is_empty() {
                        pack.display_name = room
                            .display_name().await
                            .map(|n| n.to_string())
                            .unwrap_or_default();
                    }
                    packs.push(pack);
                }
            }
            if !events.is_empty() { break; }
        }
    }

    packs
}

fn latest_event_body(value: &matrix_sdk::latest_events::LatestEventValue) -> Option<String> {
    use matrix_sdk::latest_events::LatestEventValue;
    use matrix_sdk::ruma::events::{
        AnySyncMessageLikeEvent, AnySyncTimelineEvent,
        room::message::MessageType,
    };

    match value {
        LatestEventValue::Remote(timeline_event) => {
            let event = timeline_event.raw().deserialize().ok()?;
            let msgtype = match event {
                AnySyncTimelineEvent::MessageLike(
                    AnySyncMessageLikeEvent::RoomMessage(ev)
                ) => ev.as_original()?.content.msgtype.clone(),
                _ => return None,
            };
            let body = match msgtype {
                // Use plain body for previews; formatted_body is HTML and not suited for sidebar display.
                MessageType::Text(t)  => t.body.trim().to_owned(),
                MessageType::Image(i) => i.body.trim().to_owned(),
                MessageType::File(f)  => f.body.trim().to_owned(),
                MessageType::Audio(a) => a.body.trim().to_owned(),
                MessageType::Video(v) => v.body.trim().to_owned(),
                _ => return None,
            };
            if body.is_empty() { None } else { Some(body) }
        }
        LatestEventValue::LocalIsSending(local)
        | LatestEventValue::LocalCannotBeSent(local) => {
            extract_local_body(&local.content)
        }
        LatestEventValue::LocalHasBeenSent { value: local, .. } => {
            extract_local_body(&local.content)
        }
        LatestEventValue::None | LatestEventValue::RemoteInvite { .. } => None,
    }
}

fn extract_local_body(
    content: &matrix_sdk::store::SerializableEventContent,
) -> Option<String> {
    use matrix_sdk::ruma::events::{AnyMessageLikeEventContent, room::message::MessageType};
    let msgtype = match content.deserialize().ok()? {
        AnyMessageLikeEventContent::RoomMessage(c) => c.msgtype,
        _ => return None,
    };
    let body = match msgtype {
        MessageType::Text(t)  => t.body.trim().to_owned(),
        MessageType::Image(i) => i.body.trim().to_owned(),
        MessageType::File(f)  => f.body.trim().to_owned(),
        MessageType::Audio(a) => a.body.trim().to_owned(),
        MessageType::Video(v) => v.body.trim().to_owned(),
        _ => return None,
    };
    if body.is_empty() { None } else { Some(body) }
}

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
        // `num_unread_messages()` is client-computed from read receipts and
        // stays accurate in E2E rooms where the server can't see the body —
        // unlike `unread_notification_counts().notification_count`.
        let unread_count = room.num_unread_messages();
        let last_activity_ts = room
            .latest_event_timestamp()
            .map(|t| u64::from(t.0))
            .unwrap_or(0);
        let is_favorite = room.tags().await
            .ok()
            .flatten()
            .map(|tags| tags.contains_key(
                &matrix_sdk::ruma::events::tag::TagName::Favorite))
            .unwrap_or(false);
        // MSC3765: extract HTML body from the m.topic content block when present.
        let topic_html = {
            use matrix_sdk::ruma::events::{
                room::topic::RoomTopicEventContent,
                SyncStateEvent,
            };
            use matrix_sdk::deserialized_responses::SyncOrStrippedState;
            room.get_state_event_static::<RoomTopicEventContent>().await
                .ok()
                .flatten()
                .and_then(|raw| raw.deserialize().ok())
                .and_then(|ev| {
                    let SyncOrStrippedState::Sync(SyncStateEvent::Original(o)) = ev else {
                        return None;
                    };
                    o.content.topic_block.text.find_html().map(str::to_owned)
                })
                .unwrap_or_default()
        };

        result.push(crate::ffi::RoomInfo {
            id:                room.room_id().to_string(),
            name,
            topic:             room.topic().unwrap_or_default(),
            topic_html,
            unread_count,
            is_direct:         room.is_direct().await.unwrap_or(false),
            avatar_url:        room.avatar_url()
                                   .map(|u| u.to_string())
                                   .unwrap_or_default(),
            last_message_body: String::new(),
            last_activity_ts,
            is_space,
            is_favorite,
        });
    }
    // Activity sort: unread rooms first, then by newest last-event timestamp.
    // Per-shell space partitioning runs after this and preserves the order
    // within each (non-space / space) bucket.
    result.sort_by(|a, b| {
        let a_unread = a.unread_count > 0;
        let b_unread = b.unread_count > 0;
        b_unread
            .cmp(&a_unread)
            .then_with(|| b.last_activity_ts.cmp(&a.last_activity_ts))
    });
    result
}

#[cfg(not(test))]
async fn collect_reactions(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    room: &Room,
    me: Option<&UserId>,
) -> Vec<ReactionGroup> {
    let Some(table) = event_item.content().reactions() else {
        return Vec::new();
    };

    let mut out: Vec<ReactionGroup> = Vec::with_capacity(table.len());
    for (key, by_sender) in table.iter() {
        let count = by_sender.len() as u64;
        let reacted_by_me = me
            .as_ref()
            .map(|uid| by_sender.contains_key(*uid))
            .unwrap_or(false);

        let mut senders: Vec<String> = Vec::with_capacity(by_sender.len());
        for uid in by_sender.keys() {
            // Cheap-ish lookup: hits the SDK's in-memory state store. No
            // network. Falls back to the bare Matrix ID when membership
            // for this user hasn't been hydrated yet.
            let label = match room.get_member_no_sync(uid).await {
                Ok(Some(m)) => m
                    .display_name()
                    .map(str::to_owned)
                    .unwrap_or_else(|| uid.to_string()),
                _ => uid.to_string(),
            };
            senders.push(label);
        }

        // MSC4027: when the reaction key is an mxc:// URI it IS the image URL.
        // No raw-event lookup needed — the key string is sufficient to fetch.
        let source_json = if key.starts_with("mxc://") {
            key.clone()
        } else {
            String::new()
        };
        out.push(ReactionGroup {
            key: key.clone(),
            count,
            reacted_by_me,
            source_json,
            senders,
        });
    }
    out
}

#[cfg(not(test))]
async fn collect_read_receipts(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    room: &Room,
    me: Option<&UserId>,
) -> Vec<ReadReceipt> {
    let table = event_item.read_receipts();
    if table.is_empty() {
        return Vec::new();
    }
    let mut out: Vec<ReadReceipt> = Vec::with_capacity(table.len());
    for uid in table.keys() {
        // Hide the current user's own receipt: they don't need to see their
        // own avatar marching down every message they've read.
        if me.map_or(false, |m| uid.as_str() == m.as_str()) {
            continue;
        }
        // Cheap-ish lookup against the SDK's in-memory state store. Same
        // pattern `collect_reactions` uses for resolving sender labels.
        let (display_name, avatar_url) = match room.get_member_no_sync(uid).await {
            Ok(Some(m)) => (
                m.display_name().map(str::to_owned).unwrap_or_else(|| uid.to_string()),
                m.avatar_url().map(|u| u.to_string()).unwrap_or_default(),
            ),
            _ => (uid.to_string(), String::new()),
        };
        out.push(ReadReceipt {
            user_id: uid.to_string(),
            display_name,
            avatar_url,
        });
    }
    out
}

#[cfg(not(test))]
async fn timeline_item_to_ffi(
    item: &Arc<TimelineItem>,
    room_id: &str,
    room: &Room,
    me: Option<&UserId>,
) -> Option<TimelineEvent> {
    use matrix_sdk::ruma::events::room::message::MessageType;

    let event_item = match item.kind() {
        TimelineItemKind::Event(e) => e,
        TimelineItemKind::Virtual(v) => {
            let (msg_type, timestamp): (&str, u64) = match v {
                VirtualTimelineItem::DateDivider(ts) =>
                    ("virtual.date_divider", ts.get().into()),
                VirtualTimelineItem::ReadMarker =>
                    ("virtual.read_marker", 0),
                VirtualTimelineItem::TimelineStart =>
                    ("virtual.timeline_start", 0),
            };
            return Some(TimelineEvent {
                room_id:              room_id.to_owned(),
                msg_type:             msg_type.to_owned(),
                timestamp,
                event_id:             String::new(),
                sender:               String::new(),
                sender_name:          String::new(),
                sender_avatar_url:    String::new(),
                body:                 String::new(),
                source_json:          String::new(),
                width:                0,
                height:               0,
                file_json:            String::new(),
                file_name:            String::new(),
                file_size:            0,
                image_filename:       String::new(),
                audio_source_json:    String::new(),
                audio_duration_ms:    0,
                audio_waveform:       Vec::new(),
                audio_mime:           String::new(),
                video_thumbnail_json: String::new(),
                video_duration_ms:    0,
                video_mime:           String::new(),
                video_autoplay:       false,
                video_loop:           false,
                video_no_audio:       false,
                video_hide_controls:  false,
                video_gif:            false,
                reactions:            Vec::new(),
                read_receipts:        Vec::new(),
                in_reply_to_id:          String::new(),
                in_reply_to_sender_name: String::new(),
                in_reply_to_body:        String::new(),
                is_edited:               false,
                formatted_body:          String::new(),
                blurhash:                String::new(),
                sticker_info_json:       String::new(),
                image_animated:          false,
            });
        }
    };

    // Redactions: matrix-sdk-ui replaces the original item with
    // MsgLikeKind::Redacted in place. Surface it as a tombstone (msg_type
    // "m.redacted") so the UI can swap the existing row to a placeholder
    // instead of leaving the stale body on screen.
    if let TimelineItemContent::MsgLike(MsgLikeContent { kind: MsgLikeKind::Redacted, .. }) =
        event_item.content()
    {
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
            body:              String::new(),
            timestamp:         event_item.timestamp().get().into(),
            msg_type:          "m.redacted".to_owned(),
            source_json:       String::new(),
            width:             0u64,
            height:            0u64,
            file_json:         String::new(),
            file_name:         String::new(),
            file_size:         0u64,
            image_filename:    String::new(),
            audio_source_json:    String::new(),
            audio_duration_ms:    0u64,
            audio_waveform:       Vec::new(),
            audio_mime:           String::new(),
            video_thumbnail_json: String::new(),
            video_duration_ms:    0u64,
            video_mime:           String::new(),
            video_autoplay:       false,
            video_loop:           false,
            video_no_audio:       false,
            video_hide_controls:  false,
            video_gif:            false,
            reactions:         Vec::new(),
            // Receipts on a tombstone are meaningless — the original event
            // is gone; the redacted placeholder doesn't carry a reading
            // audience worth surfacing.
            read_receipts:        Vec::new(),
            in_reply_to_id:          String::new(),
            in_reply_to_sender_name: String::new(),
            in_reply_to_body:        String::new(),
            is_edited:               false,
            formatted_body:          String::new(),
            blurhash:                String::new(),
            sticker_info_json:       String::new(),
            image_animated:          false,
        });
    }

    // Sticker events are MsgLikeKind::Sticker, not MsgLikeKind::Message.
    // Handle them before falling through to the message-only path.
    if let TimelineItemContent::MsgLike(MsgLikeContent { kind: MsgLikeKind::Sticker(s), .. }) =
        event_item.content()
    {
        let c = s.content();
        // For encrypted sticker sources (MSC2545 + ruma compat-encrypted-stickers
        // feature) we serialise the full MediaSource as JSON so the C++ side can
        // pipe it back through fetch_source_bytes for decryption.
        let src = match &c.source {
            matrix_sdk::ruma::events::sticker::StickerMediaSource::Plain(uri) =>
                uri.to_string(),
            matrix_sdk::ruma::events::sticker::StickerMediaSource::Encrypted(file) => {
                let ms = matrix_sdk::ruma::events::room::MediaSource::Encrypted(file.clone());
                serde_json::to_string(&ms).unwrap_or_default()
            }
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
        let reactions = collect_reactions(event_item, room, me).await;
        let read_receipts = collect_read_receipts(event_item, room, me).await;
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
            audio_source_json:    String::new(),
            audio_duration_ms:    0u64,
            audio_waveform:       Vec::new(),
            audio_mime:           String::new(),
            video_thumbnail_json: String::new(),
            video_duration_ms:    0u64,
            video_mime:           String::new(),
            video_autoplay:       false,
            video_loop:           false,
            video_no_audio:       false,
            video_hide_controls:  false,
            video_gif:            false,
            reactions,
            read_receipts,
            in_reply_to_id:          String::new(),
            in_reply_to_sender_name: String::new(),
            in_reply_to_body:        String::new(),
            is_edited:               false,
            formatted_body:          String::new(),
            blurhash:                c.info.blurhash.as_deref().unwrap_or("").to_owned(),
            sticker_info_json:       serde_json::to_string(&c.info)
                                         .unwrap_or_else(|_| "{}".to_owned()),
            image_animated:          c.info.is_animated.unwrap_or(false),
        });
    }

    let msg_content = match event_item.content() {
        TimelineItemContent::MsgLike(MsgLikeContent { kind: MsgLikeKind::Message(msg), .. }) => msg,
        _ => return None,
    };

    let (body, formatted_body, msg_type, source_json, width, height,
         file_json, file_name, file_size, image_filename,
         audio_source_json, audio_duration_ms, audio_waveform, audio_mime,
         video_thumbnail_json, video_duration_ms, video_mime,
         video_autoplay, video_loop, video_no_audio, video_hide_controls, video_gif) =
        match msg_content.msgtype() {
            MessageType::Text(t) => {
                let fmt = t.formatted.as_ref()
                    .filter(|f| matches!(
                        f.format,
                        matrix_sdk::ruma::events::room::message::MessageFormat::Html
                    ))
                    .map(|f| f.body.clone())
                    .unwrap_or_default();
                (
                    t.body.clone(), fmt, "m.text".to_owned(),
                    String::new(), 0u64, 0u64,
                    String::new(), String::new(), 0u64, String::new(),
                    String::new(), 0u64, Vec::<u16>::new(), String::new(),
                    String::new(), 0u64, String::new(),
                    false, false, false, false, false,
                )
            }
            MessageType::Image(i) => {
                // Plain → plain mxc URI string; encrypted → full MediaSource
                // serialised as JSON so `fetch_source_bytes` can decrypt it.
                // Matches the sticker handler above (no point of difference).
                let source_str = match &i.source {
                    matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => uri.to_string(),
                    matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) =>
                        serde_json::to_string(&i.source).unwrap_or_default(),
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
                (i.body.clone(), String::new(), "m.image".to_owned(), source_str, w, h,
                 String::new(), String::new(), 0u64, img_filename,
                 String::new(), 0u64, Vec::new(), String::new(),
                 String::new(), 0u64, String::new(),
                 false, false, false, false, false)
            }
            MessageType::File(f) => {
                let file_str = match &f.source {
                    matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => uri.to_string(),
                    matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) =>
                        serde_json::to_string(&f.source).unwrap_or_default(),
                };
                let name = f.filename.clone().unwrap_or_else(|| f.body.clone());
                let size = f
                    .info
                    .as_ref()
                    .and_then(|info| info.size)
                    .map(|v| u64::from(v))
                    .unwrap_or(0u64);
                (f.body.clone(), String::new(), "m.file".to_owned(), String::new(), 0u64, 0u64,
                 file_str, name, size, String::new(),
                 String::new(), 0u64, Vec::new(), String::new(),
                 String::new(), 0u64, String::new(),
                 false, false, false, false, false)
            }
            // MSC3245: voice messages are `m.audio` events tagged with
            // `org.matrix.msc3245.voice`; the MSC1767 `audio` block carries
            // duration + waveform. Plain `m.audio` (no voice marker) folds
            // into the file-card path so the UI still surfaces them.
            MessageType::Audio(a) => {
                let source_str = serde_json::to_string(&a.source).unwrap_or_default();
                let info_mime = a.info.as_deref().and_then(|i| i.mimetype.clone()).unwrap_or_default();
                let info_duration_ms = a.info.as_deref()
                    .and_then(|i| i.duration)
                    .map(|d| d.as_millis() as u64)
                    .unwrap_or(0u64);
                if a.voice.is_some() {
                    let (duration_ms, waveform) = match &a.audio {
                        Some(block) => {
                            let dur = block.duration.as_millis() as u64;
                            let wf: Vec<u16> = block.waveform.iter()
                                .map(|amp| u16::try_from(u64::from(amp.get())).unwrap_or(0))
                                .collect();
                            (if dur != 0 { dur } else { info_duration_ms }, wf)
                        }
                        None => (info_duration_ms, Vec::new()),
                    };
                    (a.body.clone(), String::new(), "m.voice".to_owned(), String::new(), 0u64, 0u64,
                     String::new(), String::new(), 0u64, String::new(),
                     source_str, duration_ms, waveform, info_mime,
                     String::new(), 0u64, String::new(),
                     false, false, false, false, false)
                } else {
                    let name = a.filename.clone().unwrap_or_else(|| a.body.clone());
                    let size = a.info.as_deref()
                        .and_then(|i| i.size)
                        .map(u64::from)
                        .unwrap_or(0u64);
                    (a.body.clone(), String::new(), "m.file".to_owned(), String::new(), 0u64, 0u64,
                     source_str, name, size, String::new(),
                     String::new(), 0u64, Vec::new(), String::new(),
                     String::new(), 0u64, String::new(),
                     false, false, false, false, false)
                }
            }
            MessageType::Video(v) => {
                let source_str = match &v.source {
                    matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => uri.to_string(),
                    matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) =>
                        serde_json::to_string(&v.source).unwrap_or_default(),
                };
                let (w, h, dur_ms, mime, thumb_json) = v.info.as_ref().map(|info| {
                    let w    = info.width   .map(u64::from).unwrap_or(0u64);
                    let h    = info.height  .map(u64::from).unwrap_or(0u64);
                    let dur  = info.duration.map(|d| d.as_millis() as u64).unwrap_or(0u64);
                    let mime = info.mimetype.clone().unwrap_or_default();
                    let thumb = info.thumbnail_source.as_ref()
                        .map(|ts| match ts {
                            matrix_sdk::ruma::events::room::MediaSource::Plain(uri) =>
                                uri.to_string(),
                            matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) =>
                                serde_json::to_string(ts).unwrap_or_default(),
                        })
                        .unwrap_or_default();
                    (w, h, dur, mime, thumb)
                }).unwrap_or_default();
                let vid_filename = v.filename.clone().unwrap_or_default();
                // Parse fi.mau.* vendor hints from the raw event JSON.
                let mau = |key: &str| -> bool {
                    let path = format!("/content/info/{}", key);
                    event_item.original_json()
                        .and_then(|raw| {
                            serde_json::from_str::<serde_json::Value>(raw.json().get()).ok()
                        })
                        .and_then(|json| json.pointer(&path)?.as_bool())
                        .unwrap_or(false)
                };
                let video_gif           = mau("fi.mau.gif");
                let video_autoplay      = mau("fi.mau.autoplay")      || video_gif;
                let video_loop          = mau("fi.mau.loop")          || video_gif;
                let video_no_audio      = mau("fi.mau.no_audio")      || video_gif;
                let video_hide_controls = mau("fi.mau.hide_controls") || video_gif;
                (v.body.clone(), String::new(), "m.video".to_owned(), source_str, w, h,
                 String::new(), String::new(), 0u64, vid_filename,
                 String::new(), 0u64, Vec::new(), String::new(),
                 thumb_json, dur_ms, mime,
                 video_autoplay, video_loop, video_no_audio, video_hide_controls, video_gif)
            }
            _ => return None,
        };

    let blurhash = match msg_content.msgtype() {
        MessageType::Image(i) => i.info.as_ref()
            .and_then(|info| info.blurhash.as_deref())
            .unwrap_or("").to_owned(),
        MessageType::Video(v) => v.info.as_ref()
            .and_then(|info| info.blurhash.as_deref())
            .unwrap_or("").to_owned(),
        _ => String::new(),
    };

    let image_animated = match msg_content.msgtype() {
        MessageType::Image(i) => i.info.as_ref()
            .and_then(|info| info.is_animated)
            .unwrap_or(false),
        _ => false,
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

    // m.in_reply_to — extract the event_id and, when the replied-to item is
    // present in the local timeline cache, its sender display name and a brief
    // body snippet for the UI's quote block.
    let (in_reply_to_id, in_reply_to_sender_name, in_reply_to_body) =
        match event_item.content().in_reply_to() {
            None => (String::new(), String::new(), String::new()),
            Some(details) => {
                let id = details.event_id.to_string();
                let (rname, rbody) = match &details.event {
                    TimelineDetails::Ready(replied) => {
                        let name = match &replied.sender_profile {
                            TimelineDetails::Ready(p) =>
                                p.display_name.clone().unwrap_or_default(),
                            _ => String::new(),
                        };
                        let snippet = match &replied.content {
                            TimelineItemContent::MsgLike(MsgLikeContent {
                                kind: MsgLikeKind::Message(m), ..
                            }) => {
                                use matrix_sdk::ruma::events::room::message::MessageType;
                                match m.msgtype() {
                                    MessageType::Text(t) => t.body.clone(),
                                    MessageType::Image(_) => "(image)".to_owned(),
                                    MessageType::File(_)  => "(file)".to_owned(),
                                    MessageType::Audio(_) => "(voice)".to_owned(),
                                    MessageType::Video(_) => "(video)".to_owned(),
                                    _                     => "(message)".to_owned(),
                                }
                            }
                            TimelineItemContent::MsgLike(MsgLikeContent {
                                kind: MsgLikeKind::Sticker(_), ..
                            }) => "(sticker)".to_owned(),
                            TimelineItemContent::MsgLike(MsgLikeContent {
                                kind: MsgLikeKind::Redacted, ..
                            }) => "(deleted)".to_owned(),
                            _ => String::new(),
                        };
                        (name, snippet)
                    }
                    _ => (String::new(), String::new()),
                };
                (id, rname, rbody)
            }
        };

    let reactions = collect_reactions(event_item, room, me).await;
    let read_receipts = collect_read_receipts(event_item, room, me).await;

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
        audio_source_json,
        audio_duration_ms,
        audio_waveform,
        audio_mime,
        video_thumbnail_json,
        video_duration_ms,
        video_mime,
        video_autoplay,
        video_loop,
        video_no_audio,
        video_hide_controls,
        video_gif,
        reactions,
        read_receipts,
        in_reply_to_id,
        in_reply_to_sender_name,
        in_reply_to_body,
        is_edited: msg_content.is_edited(),
        formatted_body,
        blurhash,
        sticker_info_json: String::new(),
        image_animated,
    })
}

// Count visible (event) items strictly before `raw_index` in the
// visibility mirror — this is the visible index that maps to the C++
// row vector for an op at matrix-sdk-ui slot `raw_index`.
fn visible_index_of(visible: &[bool], raw_index: usize) -> u64 {
    // Clamp instead of slicing directly: a buggy / version-mismatched SDK
    // delivering an out-of-range index must not panic on a tokio task thread
    // (which would silently break timeline tracking for the room).
    let end = raw_index.min(visible.len());
    visible[..end].iter().filter(|b| **b).count() as u64
}

fn visible_len(visible: &[bool]) -> u64 {
    visible.iter().filter(|b| **b).count() as u64
}

/// Builds a minimal raw Matrix event JSON envelope suitable for passing to
/// `Ruleset::get_actions`. Uses `serde_json::to_string` for string fields so
/// control characters (\n, \r, \t, …) are escaped correctly.
#[cfg(not(test))]
fn build_push_rule_json(
    room_id:   &str,
    event_id:  &str,
    sender:    &str,
    body:      &str,
    msg_type:  &str,
    timestamp: u64,
) -> String {
    let msg_type  = if msg_type.is_empty()  { "m.text"    } else { msg_type  };
    let event_id  = if event_id.is_empty()  { "$unknown"  } else { event_id  };
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
    }).to_string()
}

/// Evaluates Matrix push rules for `source_json` (a synthetic raw event
/// envelope) and returns `(should_notify, is_mention)`.
/// Returns `(false, false)` on any error — callers must not rely on errors being
/// propagated.
#[cfg(not(test))]
async fn evaluate_push_rules(
    client: &Client,
    room: &Room,
    source_json: &str,
) -> (bool, bool) {
    use matrix_sdk::ruma::events::GlobalAccountDataEventType;
    use serde_json::Value;

    // Read push rules from the local account-data cache (no network).
    let push_rules_et = GlobalAccountDataEventType::from("m.push_rules");
    let Some(raw) = client.account().account_data_raw(push_rules_et).await.ok().flatten()
        else { return (false, false) };
    let Ok(content) = serde_json::from_str::<Value>(raw.json().get())
        else { return (false, false) };
    let global = content.get("global").cloned().unwrap_or_default();
    let Ok(ruleset) = serde_json::from_value::<Ruleset>(global)
        else { return (false, false) };

    // Build push-condition context (no power-level data available without an
    // extra network round-trip — this skips sender_notification_permission
    // conditions but correctly handles member_count, contains_user_name, and
    // event_match rules such as mentions and keywords).
    let Some(uid) = client.user_id().map(|u| u.to_owned()) else { return (false, false) };
    let display_name = client.account().get_display_name().await
        .ok().flatten().unwrap_or_default();
    let member_count = room.joined_members_count();
    let ctx = PushConditionRoomCtx::new(
        room.room_id().to_owned(),
        UInt::try_from(member_count).unwrap_or(UInt::MAX),
        uid,
        display_name,
    );

    // Wrap source_json (synthetic event envelope) as Raw<AnySyncTimelineEvent>.
    let Ok(raw_value) = serde_json::from_str::<Box<serde_json::value::RawValue>>(source_json)
        else { return (false, false) };
    let raw_event = Raw::<AnySyncTimelineEvent>::from_json(raw_value);

    let actions = ruleset.get_actions(&raw_event, &ctx).await;
    let should_notify = actions.iter().any(|a| a.should_notify());
    let is_mention = should_notify && actions.iter().any(|a| a.is_highlight());
    (should_notify, is_mention)
}

#[cfg(not(test))]
async fn handle_timeline_diff(
    diff: VectorDiff<Arc<TimelineItem>>,
    visible: &mut Vec<bool>,
    handler: &Arc<Mutex<SendHandler>>,
    room_id: &str,
    room: &Room,
    me: Option<&UserId>,
    _client: &Client,
) {
    match diff {
        VectorDiff::Append { values } => {
            for item in values {
                let ev = timeline_item_to_ffi(&item, room_id, room, me).await;
                if let Some(ev) = ev {
                    let idx = visible_len(visible);
                    visible.push(true);
                    if let Ok(g) = handler.lock() {
                        g.on_message_inserted(room_id, idx, &ev);
                    }
                } else {
                    visible.push(false);
                }
            }
        }
        VectorDiff::PushBack { value } => {
            let ev = timeline_item_to_ffi(&value, room_id, room, me).await;
            if let Some(ev) = ev {
                let idx = visible_len(visible);
                visible.push(true);
                if let Ok(g) = handler.lock() {
                    g.on_message_inserted(room_id, idx, &ev);
                }
            } else {
                visible.push(false);
            }
        }
        VectorDiff::PushFront { value } => {
            let ev = timeline_item_to_ffi(&value, room_id, room, me).await;
            if let Some(ev) = ev {
                visible.insert(0, true);
                if let Ok(g) = handler.lock() {
                    g.on_message_inserted(room_id, 0, &ev);
                }
            } else {
                visible.insert(0, false);
            }
        }
        VectorDiff::Insert { index, value } => {
            // Vec::insert panics if index > len. matrix-sdk-ui should never
            // emit that, but a bad index on a task thread must not crash
            // timeline tracking — clamp to an append.
            let index = index.min(visible.len());
            let ev = timeline_item_to_ffi(&value, room_id, room, me).await;
            if let Some(ev) = ev {
                let v_idx = visible_index_of(visible, index);
                visible.insert(index, true);
                if let Ok(g) = handler.lock() {
                    g.on_message_inserted(room_id, v_idx, &ev);
                }
            } else {
                visible.insert(index, false);
            }
        }
        VectorDiff::Set { index, value } => {
            // `Set` can change the visibility of the slot in either
            // direction: a virtual item can be replaced by an event item
            // (decryption completes, day-divider repositions), or vice
            // versa. Map the four transitions explicitly.
            if index >= visible.len() {
                // Out-of-range Set means our mirror already disagrees with
                // the SDK's vector. Emitting a phantom insert here would
                // desync the C++ row count further; skip and log instead.
                tracing::error!(
                    "VectorDiff::Set index {} out of range (len {}) for {}",
                    index, visible.len(), room_id,
                );
                return;
            }
            let new_ev = timeline_item_to_ffi(&value, room_id, room, me).await;
            let was_visible = visible.get(index).copied().unwrap_or(false);
            match (was_visible, new_ev) {
                (true, Some(ev)) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Ok(g) = handler.lock() {
                        g.on_message_updated(room_id, v_idx, &ev);
                    }
                }
                (false, Some(ev)) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Some(slot) = visible.get_mut(index) { *slot = true; }
                    if let Ok(g) = handler.lock() {
                        g.on_message_inserted(room_id, v_idx, &ev);
                    }
                }
                (true, None) => {
                    let v_idx = visible_index_of(visible, index);
                    if let Some(slot) = visible.get_mut(index) { *slot = false; }
                    if let Ok(g) = handler.lock() {
                        g.on_message_removed(room_id, v_idx);
                    }
                }
                (false, None) => {}
            }
        }
        VectorDiff::Remove { index } => {
            let was_visible = visible.get(index).copied().unwrap_or(false);
            if was_visible {
                let v_idx = visible_index_of(visible, index);
                visible.remove(index);
                if let Ok(g) = handler.lock() {
                    g.on_message_removed(room_id, v_idx);
                }
            } else if index < visible.len() {
                visible.remove(index);
            }
        }
        VectorDiff::Truncate { length } => {
            // Drop highest indices first so each visible-index we report
            // is still valid when the C++ side applies it.
            while visible.len() > length {
                let raw = visible.len() - 1;
                let was = visible[raw];
                visible.pop();
                if was {
                    let v_idx = visible_len(visible);
                    if let Ok(g) = handler.lock() {
                        g.on_message_removed(room_id, v_idx);
                    }
                }
            }
        }
        VectorDiff::PopBack => {
            if let Some(was) = visible.pop() {
                if was {
                    let v_idx = visible_len(visible);
                    if let Ok(g) = handler.lock() {
                        g.on_message_removed(room_id, v_idx);
                    }
                }
            }
        }
        VectorDiff::PopFront => {
            if !visible.is_empty() {
                let was = visible.remove(0);
                if was {
                    if let Ok(g) = handler.lock() {
                        g.on_message_removed(room_id, 0);
                    }
                }
            }
        }
        VectorDiff::Clear => {
            visible.clear();
            if let Ok(g) = handler.lock() {
                let empty: Vec<TimelineEvent> = Vec::new();
                g.on_timeline_reset(room_id, &empty);
            }
        }
        VectorDiff::Reset { values } => {
            // Atomic snapshot replace. Build the new visibility mirror +
            // snapshot in one pass before the single `on_timeline_reset`
            // call so the UI rebuilds in one shot.
            visible.clear();
            visible.reserve(values.len());
            let mut snapshot: Vec<TimelineEvent> = Vec::new();
            for item in &values {
                let ev = timeline_item_to_ffi(item, room_id, room, me).await;
                visible.push(ev.is_some());
                if let Some(ev) = ev { snapshot.push(ev); }
            }
            if let Ok(g) = handler.lock() {
                g.on_timeline_reset(room_id, &snapshot);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SAS verification helpers
// ---------------------------------------------------------------------------

/// Watch a `VerificationRequest`'s state stream. Fires `on_verification_request`
/// (incoming=false) when the request is accepted (Ready state), then spawns a
/// SAS watcher when the flow transitions to `Transitioned { SasV1 }`.
/// Also surfaces top-level Done / Cancelled transitions before SAS starts.
#[cfg(not(test))]
async fn watch_verification_request(
    req:         matrix_sdk::encryption::verification::VerificationRequest,
    flow_id:     String,
    handler:     Arc<Mutex<SendHandler>>,
    flow_users:  Arc<Mutex<HashMap<String, String>>>,
    emoji_cache: Arc<Mutex<HashMap<String, Vec<(String, String)>>>>,
) {
    use futures_util::StreamExt;
    use matrix_sdk::encryption::verification::{VerificationRequestState, Verification};

    let user_id    = req.other_user_id().as_str().to_owned();
    let we_started = req.we_started();

    let mut changes = req.changes();

    while let Some(state) = changes.next().await {
        match state {
            VerificationRequestState::Ready { ref other_device_data, .. } => {
                // The other side accepted our outgoing request. Signal the UI
                // to transition from Waiting and call start_sas.
                if we_started {
                    let device_id = other_device_data.device_id().as_str().to_owned();
                    if let Ok(guard) = handler.lock() {
                        guard.on_verification_request(&flow_id, &user_id, &device_id, false);
                    }
                }
            }
            VerificationRequestState::Transitioned { verification } => {
                if let Verification::SasV1(sas) = verification {
                    let h2           = Arc::clone(&handler);
                    let flow_id2     = flow_id.clone();
                    let emoji_cache2 = Arc::clone(&emoji_cache);
                    tokio::spawn(watch_sas(sas, flow_id2, h2, emoji_cache2));
                }
                break;
            }
            VerificationRequestState::Done => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_done(&flow_id);
                }
                lock_or_recover(&flow_users).remove(&flow_id);
                break;
            }
            VerificationRequestState::Cancelled(info) => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_cancelled(&flow_id, info.reason());
                }
                lock_or_recover(&flow_users).remove(&flow_id);
                break;
            }
            _ => {}
        }
    }
}

/// Watch a `SasVerification`'s state stream. Fires `on_sas_ready` when the
/// 7 emoji are available, `on_verification_done` on success, and
/// `on_verification_cancelled` on mismatch or cancel.
#[cfg(not(test))]
async fn watch_sas(
    sas:         SasVerification,
    flow_id:     String,
    handler:     Arc<Mutex<SendHandler>>,
    emoji_cache: Arc<Mutex<HashMap<String, Vec<(String, String)>>>>,
) {
    use futures_util::StreamExt;
    use matrix_sdk::encryption::verification::SasState;

    let mut changes = sas.changes();

    while let Some(state) = changes.next().await {
        match state {
            SasState::KeysExchanged { emojis, .. } => {
                if let Some(emojis) = emojis {
                    let pairs: Vec<(String, String)> = emojis.emojis.iter()
                        .map(|e| (e.symbol.to_owned(), e.description.to_owned()))
                        .collect();
                    lock_or_recover(&emoji_cache).insert(flow_id.clone(), pairs.clone());
                    let ve: Vec<VerificationEmoji> = pairs.into_iter()
                        .map(|(sym, desc)| VerificationEmoji {
                            symbol:      sym,
                            description: desc,
                        })
                        .collect();
                    if let Ok(guard) = handler.lock() {
                        guard.on_sas_ready(&flow_id, &ve);
                    }
                }
            }
            SasState::Done { .. } => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_done(&flow_id);
                }
                lock_or_recover(&emoji_cache).remove(&flow_id);
                break;
            }
            SasState::Cancelled(info) => {
                if let Ok(guard) = handler.lock() {
                    guard.on_verification_cancelled(
                        &flow_id,
                        &format!("{}", info.reason()),
                    );
                }
                lock_or_recover(&emoji_cache).remove(&flow_id);
                break;
            }
            _ => {}
        }
    }
}

#[cfg(not(test))]
use matrix_sdk::encryption::verification::SasVerification;

// ---------------------------------------------------------------------------
// Server pushers (Step 12)
// ---------------------------------------------------------------------------

#[cfg(not(test))]
impl ClientFfi {
    pub fn register_pusher(
        &mut self,
        pushkey:             &str,
        app_id:              &str,
        app_display_name:    &str,
        device_display_name: &str,
        endpoint_url:        &str,
        lang:                &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let mut http_data = HttpPusherData::new(endpoint_url.to_owned());
        http_data.format = Some(PushFormat::EventIdOnly);
        let pusher = PusherInit {
            ids:                 PusherIds::new(pushkey.to_owned(), app_id.to_owned()),
            app_display_name:    app_display_name.to_owned(),
            kind:                PusherKind::Http(http_data),
            lang:                lang.to_owned(),
            device_display_name: device_display_name.to_owned(),
            profile_tag:         None,
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
    pub fn register_pusher(&mut self, _pushkey: &str, _app_id: &str,
                            _app_display_name: &str, _device_display_name: &str,
                            _endpoint_url: &str, _lang: &str) -> OpResult {
        err("not logged in")
    }

    pub fn remove_pusher(&mut self, _pushkey: &str, _app_id: &str) -> OpResult {
        err("not logged in")
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
        let r = c.send_message("!room:example.com", "hello", "");
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
    fn needs_recovery_is_false_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(!c.needs_recovery());
    }

    #[test]
    fn recover_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.recover("some-key");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn backup_state_starts_unknown() {
        let c = ClientFfi::new();
        let s = c.backup_state();
        assert_eq!(s.state, BACKUP_STATE_UNKNOWN);
        assert_eq!(s.imported_keys, 0);
        assert_eq!(s.total_keys, 0);
    }

    #[test]
    fn current_user_display_name_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.current_user_display_name().is_empty());
    }

    #[test]
    fn current_user_avatar_url_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.current_user_avatar_url().is_empty());
    }

    #[test]
    #[cfg(all(unix, not(target_os = "macos")))]
    fn default_data_dir_ends_with_expected_suffix() {
        let d = default_data_dir();
        let s = d.to_string_lossy();
        assert!(
            s.ends_with("tesseract/matrix-store"),
            "unexpected default_data_dir: {s}"
        );
    }

    #[test]
    fn set_data_dir_overrides_default_and_ignores_empty() {
        let mut c = ClientFfi::new();
        let default = c.data_dir.clone();

        // Empty string keeps the default — useful for FFI callers that may
        // pass through an empty value.
        c.set_data_dir("");
        assert_eq!(c.data_dir, default);

        let tmp = std::env::temp_dir().join(format!(
            "tesseract-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        c.set_data_dir(tmp.to_str().unwrap());
        assert_eq!(c.data_dir, tmp);
        assert!(tmp.exists(), "set_data_dir should create the directory");
        let _ = std::fs::remove_dir_all(&tmp);
    }

    // -- visibility-mirror translator (the heart of the index-aware FFI) --

    #[test]
    fn visible_index_of_skips_invisible_slots() {
        // raw:     [E, V, E, V, E]
        // visible: [0, _, 1, _, 2]
        let mirror = [true, false, true, false, true];
        assert_eq!(visible_index_of(&mirror, 0), 0);
        assert_eq!(visible_index_of(&mirror, 1), 1);
        assert_eq!(visible_index_of(&mirror, 2), 1);
        assert_eq!(visible_index_of(&mirror, 3), 2);
        assert_eq!(visible_index_of(&mirror, 4), 2);
        assert_eq!(visible_index_of(&mirror, 5), 3);
    }

    #[test]
    fn visible_index_of_empty() {
        let mirror: [bool; 0] = [];
        assert_eq!(visible_index_of(&mirror, 0), 0);
    }

    #[test]
    fn visible_index_of_all_virtual() {
        let mirror = [false, false, false];
        assert_eq!(visible_index_of(&mirror, 0), 0);
        assert_eq!(visible_index_of(&mirror, 1), 0);
        assert_eq!(visible_index_of(&mirror, 3), 0);
    }

    #[test]
    fn visible_index_of_all_events() {
        let mirror = [true, true, true, true];
        assert_eq!(visible_index_of(&mirror, 0), 0);
        assert_eq!(visible_index_of(&mirror, 2), 2);
        assert_eq!(visible_index_of(&mirror, 4), 4);
    }

    #[test]
    fn visible_len_counts_events_only() {
        assert_eq!(visible_len(&[]), 0);
        assert_eq!(visible_len(&[false, false]), 0);
        assert_eq!(visible_len(&[true, false, true, false, true]), 3);
        assert_eq!(visible_len(&[true, true, true]), 3);
    }

    // ---------------------------------------------------------------------
    // RoomListService state mapper — guards the u8 protocol encoding the
    // C++ side decodes into `tesseract::RoomListState`. If matrix-sdk-ui
    // adds a variant, the exhaustive match below stops compiling and we
    // get a clear signal to extend the C++ enum in lock-step.
    // ---------------------------------------------------------------------

    #[test]
    fn room_list_state_code_maps_every_variant() {
        use matrix_sdk_ui::room_list_service::State as S;
        assert_eq!(room_list_state_code(&S::Init),                ROOM_LIST_STATE_INIT);
        assert_eq!(room_list_state_code(&S::SettingUp),           ROOM_LIST_STATE_SETTING_UP);
        assert_eq!(room_list_state_code(&S::Recovering),          ROOM_LIST_STATE_RECOVERING);
        assert_eq!(room_list_state_code(&S::Running),             ROOM_LIST_STATE_RUNNING);
        assert_eq!(
            room_list_state_code(&S::Error { from: Box::new(S::Running) }),
            ROOM_LIST_STATE_ERROR,
        );
        assert_eq!(
            room_list_state_code(&S::Terminated { from: Box::new(S::Running) }),
            ROOM_LIST_STATE_TERMINATED,
        );
    }

    // ---------------------------------------------------------------------
    // MSC3245 — voice message parsing.
    //
    // These tests verify that the `unstable-msc3245-v1-compat` feature is
    // active on the linked ruma so the dispatcher in `timeline_item_to_ffi`
    // can read `audio.voice` / `audio.audio`. Regression guard: if a future
    // dep bump drops the feature, the `voice`/`audio` fields disappear and
    // these tests stop compiling.
    // ---------------------------------------------------------------------

    #[test]
    fn audio_event_with_voice_marker_parses_msc3245_fields() {
        use matrix_sdk::ruma::events::room::message::AudioMessageEventContent;
        let json = serde_json::json!({
            "body": "Voice message",
            "msgtype": "m.audio",
            "url": "mxc://example.org/abc123",
            "info": { "mimetype": "audio/ogg", "size": 12345, "duration": 4200 },
            "org.matrix.msc3245.voice": {},
            "org.matrix.msc1767.audio": {
                "duration": 4200,
                "waveform": [0, 256, 512, 1024, 512, 256, 0]
            }
        });
        let content: AudioMessageEventContent =
            serde_json::from_value(json).expect("AudioMessageEventContent deserialises");
        assert!(content.voice.is_some(), "voice marker must round-trip");
        let block = content.audio.as_ref().expect("audio details block present");
        assert_eq!(block.duration.as_millis() as u64, 4200);
        assert_eq!(block.waveform.len(), 7);
        let max = block.waveform.iter().map(|a| u64::from(a.get())).max().unwrap_or(0);
        assert_eq!(max, 1024);
        assert_eq!(
            content.info.as_deref().and_then(|i| i.mimetype.clone()).as_deref(),
            Some("audio/ogg")
        );
    }

    #[test]
    fn audio_event_without_voice_marker_has_no_voice() {
        use matrix_sdk::ruma::events::room::message::AudioMessageEventContent;
        let json = serde_json::json!({
            "body": "song.mp3",
            "msgtype": "m.audio",
            "url": "mxc://example.org/song",
            "info": { "mimetype": "audio/mpeg", "size": 4_400_000 }
        });
        let content: AudioMessageEventContent =
            serde_json::from_value(json).expect("AudioMessageEventContent deserialises");
        assert!(content.voice.is_none(), "plain audio must not carry the voice marker");
        assert!(content.audio.is_none(), "plain audio carries no MSC1767 details");
    }

    #[test]
    fn unstable_amplitude_saturates_above_max() {
        use matrix_sdk::ruma::events::room::message::UnstableAmplitude;
        // Sender out-of-spec value > 1024 must clamp to 1024 (the spec ceiling).
        let amp = UnstableAmplitude::new(9999);
        assert_eq!(u64::from(amp.get()), 1024);
    }

    #[test]
    fn send_reply_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_reply("!room:example.com", "$event:example.com", "reply body", "");
        assert!(!r.ok);
    }

    #[test]
    fn send_reply_invalid_room_id() {
        let mut c = ClientFfi::new();
        let r = c.send_reply("not-a-room-id", "$event:example.com", "reply body", "");
        assert!(!r.ok);
    }

    #[test]
    fn send_edit_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_edit("!room:example.com", "$event:example.com", "new body", "");
        assert!(!r.ok);
    }

    #[test]
    fn send_edit_invalid_room_id() {
        let mut c = ClientFfi::new();
        let r = c.send_edit("not-a-room-id", "$event:example.com", "new body", "");
        assert!(!r.ok);
    }

    #[test]
    fn load_prefs_returns_empty_object_when_not_logged_in() {
        let mut c = ClientFfi::new();
        assert_eq!(c.load_prefs(), "{}");
    }

    #[test]
    fn save_prefs_is_noop_when_not_logged_in() {
        let mut c = ClientFfi::new();
        c.save_prefs("{\"last_room\":\"!r:example.com\"}");
    }

    #[test]
    fn register_pusher_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.register_pusher("key", "im.gnomos.tesseract",
                                   "Tesseract", "My Device",
                                   "https://push.example.com/up", "en");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn remove_pusher_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.remove_pusher("key", "im.gnomos.tesseract");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }
}

#[cfg(test)]
mod tests_latest_event_body {
    use super::latest_event_body;
    use matrix_sdk::latest_events::{LatestEventValue, LocalLatestEventValue, RemoteLatestEventValue};
    use matrix_sdk::ruma::{MilliSecondsSinceUnixEpoch, serde::Raw, events::AnySyncTimelineEvent};
    use matrix_sdk::store::SerializableEventContent;
    use matrix_sdk::ruma::events::{AnyMessageLikeEventContent, room::message::RoomMessageEventContent};

    #[test]
    fn none_returns_none() {
        assert_eq!(latest_event_body(&LatestEventValue::None), None);
    }

    #[test]
    fn remote_invite_returns_none() {
        let value = LatestEventValue::RemoteInvite {
            event_id: None,
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            inviter: None,
        };
        assert_eq!(latest_event_body(&value), None);
    }

    #[test]
    fn remote_text_message_returns_body() {
        let raw = Raw::<AnySyncTimelineEvent>::from_json_string(
            serde_json::json!({
                "type": "m.room.message",
                "event_id": "$ev0",
                "room_id": "!r0:example.com",
                "sender": "@alice:example.com",
                "origin_server_ts": 1000,
                "content": { "msgtype": "m.text", "body": "hello world" }
            }).to_string()
        ).unwrap();
        let value = LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(raw));
        assert_eq!(latest_event_body(&value), Some("hello world".to_owned()));
    }

    #[test]
    fn remote_image_returns_filename_body() {
        let raw = Raw::<AnySyncTimelineEvent>::from_json_string(
            serde_json::json!({
                "type": "m.room.message",
                "event_id": "$ev1",
                "room_id": "!r0:example.com",
                "sender": "@alice:example.com",
                "origin_server_ts": 1000,
                "content": {
                    "msgtype": "m.image",
                    "body": "photo.jpg",
                    "url": "mxc://example.com/abc"
                }
            }).to_string()
        ).unwrap();
        let value = LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(raw));
        assert_eq!(latest_event_body(&value), Some("photo.jpg".to_owned()));
    }

    #[test]
    fn remote_state_event_returns_none() {
        let raw = Raw::<AnySyncTimelineEvent>::from_json_string(
            serde_json::json!({
                "type": "m.room.member",
                "event_id": "$ev2",
                "room_id": "!r0:example.com",
                "sender": "@alice:example.com",
                "state_key": "@alice:example.com",
                "origin_server_ts": 1000,
                "content": { "membership": "join" }
            }).to_string()
        ).unwrap();
        let value = LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(raw));
        assert_eq!(latest_event_body(&value), None);
    }

    #[test]
    fn remote_empty_body_returns_none() {
        let raw = Raw::<AnySyncTimelineEvent>::from_json_string(
            serde_json::json!({
                "type": "m.room.message",
                "event_id": "$ev3",
                "room_id": "!r0:example.com",
                "sender": "@alice:example.com",
                "origin_server_ts": 1000,
                "content": { "msgtype": "m.text", "body": "   " }
            }).to_string()
        ).unwrap();
        let value = LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(raw));
        assert_eq!(latest_event_body(&value), None);
    }

    #[test]
    fn local_is_sending_text_returns_body() {
        let content = SerializableEventContent::new(
            &AnyMessageLikeEventContent::RoomMessage(
                RoomMessageEventContent::text_plain("sending\u{2026}")
            )
        ).unwrap();
        let value = LatestEventValue::LocalIsSending(LocalLatestEventValue {
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            content,
        });
        assert_eq!(latest_event_body(&value), Some("sending\u{2026}".to_owned()));
    }

    #[test]
    fn local_cannot_be_sent_returns_body() {
        let content = SerializableEventContent::new(
            &AnyMessageLikeEventContent::RoomMessage(
                RoomMessageEventContent::text_plain("stuck message")
            )
        ).unwrap();
        let value = LatestEventValue::LocalCannotBeSent(LocalLatestEventValue {
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            content,
        });
        assert_eq!(latest_event_body(&value), Some("stuck message".to_owned()));
    }

    #[test]
    fn local_has_been_sent_returns_body() {
        use matrix_sdk::ruma::owned_event_id;
        let content = SerializableEventContent::new(
            &AnyMessageLikeEventContent::RoomMessage(
                RoomMessageEventContent::text_plain("sent!")
            )
        ).unwrap();
        let value = LatestEventValue::LocalHasBeenSent {
            event_id: owned_event_id!("$ev_sent"),
            value: LocalLatestEventValue {
                timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
                content,
            },
        };
        assert_eq!(latest_event_body(&value), Some("sent!".to_owned()));
    }
}
