use std::path::PathBuf;

use matrix_sdk::{
    ruma::{events::room::message::RoomMessageEventContent, OwnedRoomId},
    Client,
};
use tokio::runtime::Runtime;
use tokio::sync::watch;

use crate::ffi::{BackupProgress, OAuthBegin, OpResult};
use crate::oauth;

mod account;
mod backfill;
mod qr_grant;
mod crypto_reset;
mod profile_fields;
pub(crate) mod gif;
mod image_packs;
mod media;
mod media_gate;
mod media_queue;
mod notifications;
mod pins;
mod recovery;
mod room_list;
pub(crate) mod search;
mod send;
mod session;
#[cfg(not(test))]
mod sync;
mod tags;
mod thread;
mod timeline;
mod timeline_convert;
mod verification;

use session::PersistedSession;

#[cfg(test)]
pub(super) use gif::GifMedia;
#[cfg(test)]
pub(super) use send::{
    build_animated_image_content, build_thread_message_content, derive_mentions,
};

#[cfg(test)]
pub(super) use timeline_convert::{parse_geo_uri, utd_message_for_cause};

#[cfg(test)]
pub(super) use timeline::{visible_index_of, visible_len};

#[cfg(not(test))]
use backfill::{
    apply_backfill_previews, load_backfill_ts_conn, open_app_cache_db, open_search_db,
    BackfillPreview,
};

/// RAII guard that increments `counter` on creation and decrements it on drop,
/// firing `on_inflight_changed` through `handler` both times so the UI dot
/// tracks the exact number of concurrent extra-sync HTTP operations.
#[cfg(not(test))]
pub(crate) struct InFlightGuard {
    counter: Arc<std::sync::atomic::AtomicU32>,
    handler: Option<Arc<Mutex<SendHandler>>>,
    /// Human-readable label for this operation (e.g. the URL being fetched).
    /// Shared with `ClientFfi::in_flight_urls` so the debug tooltip can list
    /// which operations are currently in flight.
    #[cfg(debug_assertions)]
    urls: Arc<Mutex<Vec<String>>>,
    #[cfg(debug_assertions)]
    label: String,
}

#[cfg(not(test))]
impl InFlightGuard {
    pub(crate) fn new(
        counter: &Arc<std::sync::atomic::AtomicU32>,
        handler: &Option<Arc<Mutex<SendHandler>>>,
        #[cfg(debug_assertions)] urls: &Arc<Mutex<Vec<String>>>,
        #[cfg(debug_assertions)] label: String,
    ) -> Self {
        let prev = counter.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let new_count = prev + 1;
        #[cfg(debug_assertions)]
        {
            let mut list = urls.lock();
            list.push(label.clone());
            Self::notify(handler, new_count, &list);
        }
        #[cfg(not(debug_assertions))]
        Self::notify(handler, new_count);
        InFlightGuard {
            counter: counter.clone(),
            handler: handler.clone(),
            #[cfg(debug_assertions)]
            urls: urls.clone(),
            #[cfg(debug_assertions)]
            label,
        }
    }

    /// Release build: notify the UI with just the count.
    #[cfg(not(debug_assertions))]
    fn notify(handler: &Option<Arc<Mutex<SendHandler>>>, count: u32) {
        if let Some(h) = handler {
            let g = h.lock();
            g.on_inflight_changed(count);
        }
    }

    /// Debug build: notify the UI with the count and the newline-joined list of
    /// in-flight operation labels.
    #[cfg(debug_assertions)]
    fn notify(handler: &Option<Arc<Mutex<SendHandler>>>, count: u32, urls: &[String]) {
        if let Some(h) = handler {
            let g = h.lock();
            g.on_inflight_changed_debug(count, &urls.join("\n"));
        }
    }
}

#[cfg(not(test))]
impl Drop for InFlightGuard {
    fn drop(&mut self) {
        #[cfg(debug_assertions)]
        {
            let mut list = self.urls.lock();
            if let Some(pos) = list.iter().position(|l| l == &self.label) {
                list.swap_remove(pos);
            }
            let prev = self.counter.fetch_sub(1, std::sync::atomic::Ordering::Relaxed);
            Self::notify(&self.handler, prev.saturating_sub(1), &list);
        }
        #[cfg(not(debug_assertions))]
        {
            let prev = self.counter.fetch_sub(1, std::sync::atomic::Ordering::Relaxed);
            Self::notify(&self.handler, prev.saturating_sub(1));
        }
    }
}

/// Max concurrent interactive media downloads (avatars + thumbnails). These are
/// small and the user is actively waiting on them, so the lane is wide.
#[cfg(not(test))]
pub(super) const MEDIA_FG_PERMITS: usize = 12;
/// Max concurrent bulk media downloads (full-size source, URL previews, tiles,
/// audio prefetch). With HTTP/2 multiplexing these share one connection, so the
/// old TCP-connection pressure is gone and a wider lane is safe.
#[cfg(not(test))]
pub(super) const MEDIA_BULK_PERMITS: usize = 10;

#[cfg(not(test))]
use crate::ffi::EventHandlerBridge;
#[cfg(not(test))]
use cxx::UniquePtr;
#[cfg(not(test))]
use matrix_sdk::{ruma::UserId, Room};
#[cfg(not(test))]
use matrix_sdk_ui::sync_service::SyncService;
#[cfg(not(test))]
use std::collections::HashMap;
#[cfg(not(test))]
use parking_lot::Mutex;
#[cfg(not(test))]
use std::sync::Arc;

// ---------------------------------------------------------------------------

pub(super) fn ok(msg: impl Into<String>) -> OpResult {
    OpResult {
        ok: true,
        message: msg.into(),
    }
}

pub(super) fn err(msg: impl Into<String>) -> OpResult {
    OpResult {
        ok: false,
        message: msg.into(),
    }
}

/// Early-return an `OpResult` error from a `Result<_, OpResult>` expression.
/// Usage: `let value = try_op!(some_fn_returning_result());`
#[cfg(not(test))]
macro_rules! try_op {
    ($e:expr) => {
        match $e {
            Ok(v) => v,
            Err(e) => return e,
        }
    };
}
#[cfg(not(test))]
pub(super) use try_op;

#[cfg(not(test))]
pub(super) fn parse_room_id(id: &str) -> Result<OwnedRoomId, OpResult> {
    id.parse::<OwnedRoomId>()
        .map_err(|e| err(format!("invalid room id: {e}")))
}

#[cfg(not(test))]
pub(super) fn require_room(
    client: &Client,
    room_id: &str,
) -> Result<(OwnedRoomId, Room), OpResult> {
    let id = parse_room_id(room_id)?;
    let room = client.get_room(&id).ok_or_else(|| err("room not found"))?;
    Ok((id, room))
}

/// Returns true when `kind` is the `M_FORBIDDEN` variant. Used by the
/// presence-polling task to recognise homeservers that refuse to disclose a
/// user's presence (typical for bridge puppet accounts with privacy
/// enabled) and stop polling them. Other error kinds — `NotFound`, `Unknown`,
/// transport errors — are considered retriable and not handled here.
pub(super) fn is_presence_forbidden(
    kind: Option<&matrix_sdk::ruma::api::error::ErrorKind>,
) -> bool {
    use matrix_sdk::ruma::api::error::ErrorKind;
    matches!(kind, Some(ErrorKind::Forbidden))
}

/// Look up a room member's display name, falling back to their full Matrix ID.
#[cfg(not(test))]
pub(super) async fn member_display_name(
    room: &matrix_sdk::Room,
    uid: &matrix_sdk::ruma::UserId,
) -> String {
    match room.get_member_no_sync(uid).await {
        Ok(Some(m)) => m.display_name().map(str::to_owned).unwrap_or_else(|| uid.to_string()),
        _ => uid.to_string(),
    }
}

/// Look up a room member's display name, falling back to their localpart.
#[cfg(not(test))]
pub(super) async fn member_display_name_local(
    room: &matrix_sdk::Room,
    uid: &matrix_sdk::ruma::UserId,
) -> String {
    match room.get_member_no_sync(uid).await {
        Ok(Some(m)) => m.display_name().map(str::to_owned).unwrap_or_else(|| uid.localpart().to_string()),
        _ => uid.localpart().to_string(),
    }
}

/// Build the spec'd UIAA fallback URL for `stage` and `session`. The
/// homeserver base may or may not end with `/`; the spec path is
/// `_matrix/client/v3/auth/<stage>/fallback/web?session=<session>`.
pub(super) fn build_uia_fallback_url(homeserver: &str, stage: &str, session: &str) -> String {
    let base = homeserver.trim_end_matches('/');
    let stage_enc = urlencoding_encode_segment(stage);
    let session_enc = urlencoding_encode_segment(session);
    format!(
        "{base}/_matrix/client/v3/auth/{stage_enc}/fallback/web?session={session_enc}",
    )
}

/// Minimal percent-encoder for a single path segment / query value. Encodes
/// every byte that isn't unreserved per RFC 3986. Used only for the UIAA
/// fallback URL; pulling in a full URL crate just for this would be overkill.
fn urlencoding_encode_segment(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    for b in input.bytes() {
        let unreserved = matches!(b,
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~');
        if unreserved {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{:02X}", b));
        }
    }
    out
}

pub(super) fn oauth_err(msg: impl Into<String>) -> OAuthBegin {
    OAuthBegin {
        ok: false,
        message: msg.into(),
        auth_url: String::new(),
        redirect_uri: String::new(),
    }
}

// `BackupProgress.state` encoding — kept in sync with the docs on the cxx
// shared struct in `bridge.rs` and the `BackupState` enum in
// `client/include/tesseract/types.h`.
const BACKUP_STATE_UNKNOWN: u8 = 0;
const BACKUP_STATE_DISABLED: u8 = 1;
const BACKUP_STATE_ENABLED: u8 = 2;
const BACKUP_STATE_DOWNLOADING: u8 = 3;
const BACKUP_STATE_CREATING: u8 = 4;

#[cfg(not(test))]
pub(super) fn backup_state_code(s: matrix_sdk::encryption::backups::BackupState) -> u8 {
    use matrix_sdk::encryption::backups::BackupState as B;
    match s {
        B::Unknown => BACKUP_STATE_UNKNOWN,
        B::Disabling => BACKUP_STATE_DISABLED,
        B::Enabled => BACKUP_STATE_ENABLED,
        B::Downloading | B::Enabling | B::Resuming => BACKUP_STATE_DOWNLOADING,
        B::Creating => BACKUP_STATE_CREATING,
    }
}

// `on_room_list_state` payload encoding — kept in sync with the
// `RoomListState` enum in `client/include/tesseract/types.h`. Mirrors
// `matrix_sdk_ui::room_list_service::State`.
pub(crate) const ROOM_LIST_STATE_INIT: u8 = 0;
pub(crate) const ROOM_LIST_STATE_SETTING_UP: u8 = 1;
pub(crate) const ROOM_LIST_STATE_RECOVERING: u8 = 2;
pub(crate) const ROOM_LIST_STATE_RUNNING: u8 = 3;
pub(crate) const ROOM_LIST_STATE_ERROR: u8 = 4;
pub(crate) const ROOM_LIST_STATE_TERMINATED: u8 = 5;

pub(super) fn room_list_state_code(s: &matrix_sdk_ui::room_list_service::State) -> u8 {
    use matrix_sdk_ui::room_list_service::State as S;
    match s {
        S::Init => ROOM_LIST_STATE_INIT,
        S::SettingUp => ROOM_LIST_STATE_SETTING_UP,
        S::Recovering => ROOM_LIST_STATE_RECOVERING,
        S::Running => ROOM_LIST_STATE_RUNNING,
        S::Error { .. } => ROOM_LIST_STATE_ERROR,
        S::Terminated { .. } => ROOM_LIST_STATE_TERMINATED,
    }
}

#[cfg(test)]
pub(super) fn backup_progress_default() -> BackupProgress {
    BackupProgress {
        state: BACKUP_STATE_UNKNOWN,
        imported_keys: 0,
        total_keys: 0,
    }
}

/// Default per-platform location for the matrix-sdk SQLite store. Used as the
/// initial value of `ClientFfi::data_dir`; callers can override it via
/// `ClientFfi::set_data_dir` (e.g. to scope the store under a specific
/// account directory in the multi-account layout).
fn default_data_dir() -> PathBuf {
    let base = dirs_like_home().unwrap_or_else(std::env::temp_dir);
    let dir = base.join("tesseract").join("matrix-store");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

fn dirs_like_home() -> Option<PathBuf> {
    #[cfg(target_os = "windows")]
    {
        std::env::var_os("APPDATA").map(PathBuf::from)
    }
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

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

#[cfg(not(test))]
pub(super) struct SendHandler(pub(super) UniquePtr<EventHandlerBridge>);
#[cfg(not(test))]
unsafe impl Send for SendHandler {}
#[cfg(not(test))]
impl std::ops::Deref for SendHandler {
    type Target = UniquePtr<EventHandlerBridge>;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

#[cfg(not(test))]
pub(super) struct TimelineHandle {
    pub(super) timeline: Arc<matrix_sdk_ui::Timeline>,
    pub(super) abort_tasks: Vec<tokio::task::AbortHandle>,
    /// `true` when this timeline was built via `subscribe_room_at` (focused on
    /// a specific event); `false` for the live timeline built by
    /// `subscribe_room`. `paginate_forward` requires `is_focused == true`.
    pub(super) is_focused: bool,
    /// Set to `true` before aborting the tasks so any in-flight emission that
    /// slips past the cooperative-cancellation window is silently dropped.
    pub(super) cancelled: Arc<AtomicBool>,
}

#[cfg(not(test))]
pub(super) struct ThreadListHandle {
    pub(super) service: std::sync::Arc<matrix_sdk_ui::timeline::ThreadListService>,
    pub(super) abort: tokio::task::AbortHandle,
}

pub struct ClientFfi {
    pub(super) client: Option<Client>,
    pub(super) stop_tx: Option<watch::Sender<bool>>,
    /// Persistent receiver clone used by `paginate_back_with_status` to race
    /// the network call against shutdown. Set in `start_sync`; survives until
    /// the `ClientFfi` is dropped so the cancel arm fires even if the call
    /// starts just before `stop_sync` takes `stop_tx`.
    pub(super) stop_rx: Option<watch::Receiver<bool>>,
    pub(super) oauth_flow: Option<oauth::PendingFlow>,
    pub(super) qr_grant: Option<qr_grant::QrGrantHandle>,
    #[cfg(not(test))]
    pub(super) handler: Option<Arc<Mutex<SendHandler>>>,
    #[cfg(not(test))]
    pub(super) sync_service: Option<Arc<SyncService>>,
    #[cfg(not(test))]
    pub(super) timelines: parking_lot::RwLock<HashMap<OwnedRoomId, TimelineHandle>>,
    /// Active thread-focused timelines keyed by (room_id, thread_root_event_id).
    /// Each entry holds the same `TimelineHandle` structure used by `timelines`.
    /// Behind a `RwLock` (like `timelines`) so the thread FFI methods can take
    /// `&self` and run concurrently under the C++ shared lock — see the locking
    /// note in `client/src/client.cpp`.
    #[cfg(not(test))]
    pub(super) thread_timelines: parking_lot::RwLock<HashMap<
        (OwnedRoomId, matrix_sdk::ruma::OwnedEventId),
        TimelineHandle,
    >>,
    /// Active thread-list subscriptions keyed by room_id. Each entry holds a
    /// `ThreadListService` and an abort handle for the items-watcher task.
    /// `RwLock`-wrapped for the same `&self` reason as `thread_timelines`.
    #[cfg(not(test))]
    pub(super) thread_lists: parking_lot::RwLock<HashMap<OwnedRoomId, ThreadListHandle>>,
    /// Background backfill orchestrator handle. Aborting it tears down both
    /// the orchestrator and every per-room silent backfill it spawned (the
    /// children live inside a `JoinSet` owned by the orchestrator future).
    #[cfg(not(test))]
    pub(super) backfill_task: Option<tokio::task::AbortHandle>,
    /// One-shot unread-prefetch orchestrator handle. Kept separate from
    /// `backfill_task` so the inactive-grouping backfill and the unread
    /// prefetch never abort one another (they share neither handle nor
    /// idempotency guard). Aborting tears down the orchestrator and every
    /// per-room silent prefetch it spawned (children live in a `JoinSet`
    /// owned by the orchestrator future).
    #[cfg(not(test))]
    pub(super) prefetch_task: Option<tokio::task::AbortHandle>,
    /// Newest unread-prefetch room set requested while `prefetch_task` was still
    /// running. The running task drains this when its current batch finishes, so
    /// messages that arrive mid-prefetch get warmed without waiting for the next
    /// fingerprint change. Coalesced — only the latest set is kept.
    #[cfg(not(test))]
    pub(super) pending_prefetch:
        Arc<parking_lot::Mutex<Option<Vec<OwnedRoomId>>>>,
    /// Shared concurrency limiter for all silent room warm-up pagination — the
    /// inactive-grouping backfill and the unread prefetch both acquire from it,
    /// so their *combined* in-flight `backfill_room_silent` count is bounded
    /// (rather than each having its own lane that sum to a larger total).
    #[cfg(not(test))]
    pub(super) warm_semaphore: Arc<tokio::sync::Semaphore>,
    /// In-progress cross-signing reset handle (from `reset_cross_signing`),
    /// held between `begin_reset_crypto_identity` and the background poll /
    /// `cancel_reset_crypto_identity`. `None` when no reset is pending.
    #[cfg(not(test))]
    pub(super) crypto_reset_handle:
        Option<Arc<matrix_sdk::encryption::CrossSigningResetHandle>>,
    /// Count of in-flight extra HTTP operations (media downloads, /messages
    /// back-pagination). Does not include the sync long-poll (always 1 when
    /// Running). Wrapped in Arc so it can be cloned into spawned tasks.
    #[cfg(not(test))]
    pub(super) in_flight: Arc<std::sync::atomic::AtomicU32>,
    /// Active in-flight operation labels for debug builds. Shared with every
    /// `InFlightGuard` so the tooltip shows which operations are running.
    #[cfg(not(test))]
    #[cfg(debug_assertions)]
    pub(super) in_flight_urls: Arc<Mutex<Vec<String>>>,
    /// Room-list previews derived from back-pagination, keyed by room ID.
    /// `room.latest_event()` is only updated by the live sync loop; for rooms
    /// whose latest event arrived only through back-pagination the sync-backed
    /// field stays `None`. This cache is applied after every `build_room_infos`
    /// call so previews persist even when a notable update overwrites the list.
    #[cfg(not(test))]
    pub(super) backfill_previews: Arc<Mutex<HashMap<String, BackfillPreview>>>,
    /// Per-account SQLite database for backfill state (`backfill_ts` table).
    /// Open for the lifetime of the sync session; `None` before first
    /// `start_sync` or after account switch.
    #[cfg(not(test))]
    pub(super) app_cache_db: Arc<Mutex<Option<rusqlite::Connection>>>,
    /// Per-account SQLite database for the full-text search index
    /// (`message_index`, `message_fts`, `search_state`). Opened alongside
    /// `app_cache_db` in `start_sync`; `None` before first login or after
    /// account switch.
    #[cfg(not(test))]
    pub(super) search_db: Arc<Mutex<Option<rusqlite::Connection>>>,
    /// Opt-in full-text search indexing gate. When false (the default), the
    /// timeline/pagination/backfill paths skip writing decrypted bodies to the
    /// `message_index` FTS5 table in `app_cache.db`. Flipped by
    /// `set_search_indexing_enabled` from the Settings toggle. `Arc` so it can
    /// be cloned into spawned indexing tasks.
    #[cfg(not(test))]
    pub(super) search_indexing_enabled: Arc<std::sync::atomic::AtomicBool>,
    /// Abort handles for every long-lived task spawned by `start_sync`
    /// (session-refresh watcher, room/pack watcher, backup watchers, sync
    /// monitor, …). These outlive `self.handler.take()`, so without explicit
    /// aborts they keep calling back through their own `Arc<SendHandler>`
    /// clone into a C++ shell that may already be torn down (use-after-free).
    /// Drained and aborted by `stop_sync`.
    #[cfg(not(test))]
    pub(super) sync_tasks: Vec<tokio::task::AbortHandle>,
    /// Handles for the `client.add_event_handler` registrations made by
    /// `start_sync` (notification + typing handlers). Removed in `stop_sync`
    /// so they stop firing into a destroyed handler.
    #[cfg(not(test))]
    pub(super) event_handler_handles: Vec<matrix_sdk::event_handler::EventHandlerHandle>,
    /// Latest known backup state code (see BACKUP_STATE_* constants).
    /// Updated by the backup watcher task and read by `backup_state()`.
    #[cfg(not(test))]
    pub(super) backup_state_code: Arc<std::sync::atomic::AtomicU8>,
    /// Running counter of room keys imported from the backup since this
    /// process started. Reset to 0 only on logout.
    #[cfg(not(test))]
    pub(super) imported_keys: Arc<AtomicU64>,
    /// Cached homeserver media upload limit in bytes. 0 = unknown / unfetched.
    /// Populated lazily on first `media_upload_limit()` call after login.
    pub(super) media_upload_limit: AtomicU64,
    /// Cached MSC2545 image packs (user pack + every enabled room pack).
    /// Rebuilt by `refresh_image_packs_async` whenever sync delivers a
    /// relevant event; read by the FFI list/* accessors without blocking.
    #[cfg(not(test))]
    pub(super) image_packs: Arc<Mutex<Vec<crate::image_packs::ImagePack>>>,
    /// Set to `true` immediately after a successful `set_account_data_raw` for
    /// the user pack. Cleared by the sync watcher the first time `rebuild_image_packs`
    /// returns a user pack (confirming the server echo arrived in the state store).
    /// Guards the preserve logic: only hold the cached user pack across a stale
    /// rebuild while our own write is still in flight — not after an external
    /// deletion of the pack.
    #[cfg(not(test))]
    pub(super) user_pack_write_pending: Arc<std::sync::atomic::AtomicBool>,
    /// Set to `true` by the `AnyGlobalAccountDataEvent` handler whenever an
    /// image-pack account-data event arrives (user pack or emote-rooms list).
    /// Cleared atomically by the sync watcher after each `rebuild_image_packs`
    /// call.  Allows the notable-update loop to skip the O(all-rooms) SQLite
    /// sweep on pure read-receipt / recency-stamp bursts while still catching
    /// remote pack edits that arrive with the catch-all `NONE` reason.
    /// Initialized to `true` so the first sync loop always rebuilds the pack
    /// cache after login.
    #[cfg(not(test))]
    pub(super) packs_dirty: Arc<std::sync::atomic::AtomicBool>,
    /// Serializes every account-data read-modify-write (`recent_emoji_bump`,
    /// `save_sticker_to_user_pack`, `toggle_favorite_sticker`). Matrix
    /// account-data is last-write-wins with no server-side merge, so two
    /// concurrent GET→modify→PUT cycles would drop one side's change. Held
    /// across the whole cycle so writes apply on top of each other.
    #[cfg(not(test))]
    pub(super) account_data_lock: Arc<tokio::sync::Mutex<()>>,
    /// Directory holding the matrix-sdk SQLite store for this client. Set via
    /// `set_data_dir` before `oauth_begin` / `restore_session`. Defaults to
    /// `default_data_dir()` for legacy single-account callers.
    pub(super) data_dir: PathBuf,
    /// Shared HTTP client for generic URL fetches (OSM tile images, etc.).
    /// Created once so TLS root certificates are loaded only on startup and
    /// connection pools are reused across calls.
    pub(super) http_client: reqwest::Client,
    /// When `false`, the background presence polling loop skips every tick.
    /// Controlled by `set_presence_polling_enabled`; stored as an `AtomicBool`
    /// so it can be updated from the UI thread while the polling task runs on
    /// a worker thread without requiring a lock or a full stop/restart.
    pub(super) presence_polling_enabled: std::sync::Arc<std::sync::atomic::AtomicBool>,
    /// Last set of room IDs pushed to `RoomListService::subscribe_to_rooms`.
    /// Used by `sync_room_subscriptions` to skip the re-push (and the SQL
    /// fan-out it triggers inside matrix-sdk) when the subscribed set is
    /// unchanged — e.g. re-selecting the same room, or toggling a thread that
    /// belongs to an already-subscribed room.
    #[cfg(not(test))]
    pub(super) last_sync_room_subscriptions:
        Arc<parking_lot::Mutex<std::collections::HashSet<OwnedRoomId>>>,
    /// Cached set of DM counterpart user IDs (as Matrix user-id strings).
    /// Repopulated after every `build_room_infos` sweep — `RoomInfo` already
    /// carries `dm_counterpart_user_id` — so the presence polling loop does
    /// not have to walk every joined room (with the per-room
    /// `dm_other_user` member-list lookup that goes with it) on every tick.
    #[cfg(not(test))]
    pub(super) dm_counterparts:
        Arc<parking_lot::RwLock<std::collections::HashSet<String>>>,
    /// Users that returned 403 Forbidden on the presence endpoint —
    /// typically bridge puppet accounts with presence privacy enabled.
    /// They never recover mid-session so we record them and skip future
    /// polls. Shared between the long-running 60s polling loop and
    /// one-off `poll_presence_now` kicks (e.g. on window re-focus).
    /// Lives for the lifetime of this ClientFfi; a re-login starts fresh.
    #[cfg(not(test))]
    pub(super) forbidden_presence:
        Arc<Mutex<std::collections::HashSet<matrix_sdk::ruma::OwnedUserId>>>,
    /// Maps verification `flow_id` → `user_id` for in-flight verification
    /// requests (both incoming and outgoing). Allows `accept_verification`,
    /// `start_sas`, etc. to look up the VerificationRequest via the
    /// SDK's internal map using (user_id, flow_id).
    #[cfg(not(test))]
    pub(super) verification_flow_users: Arc<Mutex<HashMap<String, String>>>,
    /// Most-recently-computed SAS emoji per `flow_id`. Populated by the SAS
    /// watcher task when `KeysExchanged` fires; read synchronously by
    /// `get_sas_emojis()`.
    #[cfg(not(test))]
    pub(super) sas_emoji_cache: Arc<Mutex<HashMap<String, Vec<(String, String)>>>>,
    /// Abort handles for every verification/SAS watcher task. These spawns
    /// outlive their initiating method, hold cloned `Arc<SendHandler>`
    /// references, and would otherwise call back into a destroyed C++
    /// handler on shutdown (the same UAF class as `sync_tasks`). Drained
    /// and aborted by `stop_sync`. Shared via `Arc` because nested watchers
    /// (`watch_verification_request` spawns a `watch_sas` on its own when
    /// the flow transitions) need to register from inside the running
    /// future where `&self` is unavailable.
    #[cfg(not(test))]
    pub(super) verification_tasks: Arc<Mutex<Vec<tokio::task::AbortHandle>>>,
    /// Priority gate for interactive media downloads (avatars + thumbnails).
    /// More slots than the bulk lane so the small, visible fetches the user is
    /// waiting on are never crowded out by slow full-size downloads. Unlike a
    /// plain semaphore, a parked waiter can be re-prioritized (`prioritize_media`)
    /// so a fetch for a just-scrolled-to row jumps ahead of the off-screen
    /// backlog. Async slots, not OS threads — idle slots cost nothing.
    #[cfg(not(test))]
    pub(super) media_gate_fg: Arc<media_gate::PriorityGate>,
    /// Priority gate for bulk media downloads (full-size source, URL previews,
    /// map tiles, audio prefetch). Few slots so a stalled large download can
    /// occupy at most a handful.
    #[cfg(not(test))]
    pub(super) media_gate_bulk: Arc<media_gate::PriorityGate>,
    /// Abort handles for in-flight `fetch_media_async` / `get_url_preview_async`
    /// tasks, keyed by `group_id` (a hash of the originating room id; 0 =
    /// ungrouped). `cancel_media_group` drains and aborts a group's tasks on
    /// room switch. Each task removes its own `(request_id, handle)` on
    /// completion. Interior-mutable so the `&self` media methods can register
    /// without `&self`. Also drained wholesale in `stop_sync`.
    #[cfg(not(test))]
    pub(super) media_tasks:
        Arc<Mutex<HashMap<u64, Vec<(u64, tokio::task::AbortHandle)>>>>,
    /// Set of `"kind:source"` cache keys for media that matrix-sdk's internal
    /// SQLite store already holds. `fetch_media_async` uses this to skip the
    /// priority gate for locally-cached media: a SQLite hit takes < 1 ms, so a
    /// 10 ms probe always resolves before any real network round-trip. Cleared
    /// on `start_sync` (account switch) so stale keys from a different session
    /// never bypass a legitimate download.
    #[cfg(not(test))]
    pub(super) sdk_media_fetched: Arc<Mutex<std::collections::HashSet<String>>>,
    /// Endpoint prefix for MSC4133 profile field writes, populated by
    /// `get_server_info` when the server advertises
    /// `unstable_features["uk.tcpip.msc4133"] == true`.
    /// `None` = server does not support MSC4133 (writes disabled).
    pub(super) profile_fields_prefix: std::sync::Arc<std::sync::RwLock<Option<String>>>,
    // Declared last so it drops after all SDK resources; deadpool/SQLite cleanup
    // uses tokio primitives and requires the runtime to still be alive.
    pub(super) rt: Runtime,
}

impl Drop for ClientFfi {
    fn drop(&mut self) {
        self.stop_sync();
        #[cfg(not(test))]
        if let Some(h) = self.backfill_task.take() {
            h.abort();
        }
        #[cfg(not(test))]
        if let Some(h) = self.prefetch_task.take() {
            h.abort();
        }
        // Drop SDK objects that call Handle::current() in their Drop impls
        // (SqliteStateStore via matrix_sdk::Client, Timeline) with the runtime
        // handle in TLS.  Without enter() here, dropping pending_login_client_
        // from C++ (Qt event loop, no tokio context) causes a panic_in_cleanup
        // abort — the same hazard oauth_await_callback / restore_session guard
        // against with their own `let _guard = self.rt.enter()` blocks.
        {
            let _guard = self.rt.enter();
            #[cfg(not(test))]
            for (_, th) in self.timelines.write().drain() {
                th.cancelled.store(true, Ordering::Release);
                for h in th.abort_tasks {
                    h.abort();
                }
            }
            #[cfg(not(test))]
            for (_, th) in self.thread_timelines.write().drain() {
                th.cancelled.store(true, Ordering::Release);
                for h in th.abort_tasks {
                    h.abort();
                }
            }
            #[cfg(not(test))]
            for (_, h) in self.thread_lists.write().drain() {
                h.abort.abort();
            }
            // Explicit take: matrix_sdk::Client drops here (runtime in TLS)
            // rather than in the implicit field-drop pass after this fn returns.
            let _ = self.client.take();
        }
        // Remaining fields are all None/empty; rt drops last (declared last).
    }
}

/// Lock a `Mutex` without ever panicking. `parking_lot` mutexes do not poison,
/// so `.lock()` returns the guard directly even if a prior holder panicked.
/// This matters on synchronous FFI entry points, where a panic unwinding across
/// the C++ boundary is undefined behavior.
#[cfg(not(test))]
pub(super) fn lock_or_recover<T>(m: &Mutex<T>) -> parking_lot::MutexGuard<'_, T> {
    m.lock()
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
    )
    .await
}

/// Encode raw 16-bit mono PCM samples (48 kHz) into an Ogg/Opus byte stream.
/// `_waveform` and `_duration_ms` are stored by the caller for the MSC1767
/// waveform content but are not embedded into the Opus stream itself (Opus
/// has no waveform sidecar; the data goes into the Matrix event content).
///
/// Returns `Err` when `samples` is empty or the Opus encoder fails.
pub(crate) fn encode_voice_ogg(
    samples: &[i16],
    _waveform: &[u16],
    _duration_ms: u64,
) -> Result<Vec<u8>, String> {
    use audiopus::coder::Encoder;
    use audiopus::{Application, Channels, SampleRate};
    use ogg::PacketWriter;

    if samples.is_empty() {
        return Err("empty PCM".to_owned());
    }

    let enc = Encoder::new(SampleRate::Hz48000, Channels::Mono, Application::Voip)
        .map_err(|e| e.to_string())?;

    const FRAME: usize = 960;
    let mut opus_packets: Vec<Vec<u8>> = Vec::new();
    let mut out_buf = vec![0u8; 4000];

    let mut pos = 0usize;
    while pos + FRAME <= samples.len() {
        let n = enc
            .encode(&samples[pos..pos + FRAME], &mut out_buf)
            .map_err(|e| e.to_string())?;
        opus_packets.push(out_buf[..n].to_vec());
        pos += FRAME;
    }

    let mut ogg_bytes: Vec<u8> = Vec::new();
    let mut pw = PacketWriter::new(&mut ogg_bytes);

    // OpusHead identification header.
    let mut opus_head = Vec::with_capacity(19);
    opus_head.extend_from_slice(b"OpusHead");
    opus_head.push(1);                                    // version
    opus_head.push(1);                                    // channel count (mono)
    opus_head.extend_from_slice(&312u16.to_le_bytes());   // pre-skip
    opus_head.extend_from_slice(&48000u32.to_le_bytes()); // input sample rate
    opus_head.extend_from_slice(&0i16.to_le_bytes());     // output gain
    opus_head.push(0);                                    // channel mapping family
    // RFC 7845 §3: each header MUST begin on its own page.
    pw.write_packet(
        opus_head,
        0x1234_5678,
        ogg::PacketWriteEndInfo::EndPage,
        0,
    )
    .map_err(|e| e.to_string())?;

    // OpusTags comment header.
    let vendor = b"tesseract";
    let mut tags = Vec::new();
    tags.extend_from_slice(b"OpusTags");
    tags.extend_from_slice(&(vendor.len() as u32).to_le_bytes());
    tags.extend_from_slice(vendor);
    tags.extend_from_slice(&0u32.to_le_bytes()); // zero user comments
    pw.write_packet(
        tags,
        0x1234_5678,
        ogg::PacketWriteEndInfo::EndPage,
        0,
    )
    .map_err(|e| e.to_string())?;

    // Audio pages.
    let mut granule: u64 = 0;
    let total = opus_packets.len();
    for (i, pkt) in opus_packets.into_iter().enumerate() {
        granule += FRAME as u64;
        let end_info = if i + 1 == total {
            ogg::PacketWriteEndInfo::EndStream
        } else {
            ogg::PacketWriteEndInfo::NormalPacket
        };
        pw.write_packet(pkt, 0x1234_5678, end_info, granule)
            .map_err(|e| e.to_string())?;
    }

    Ok(ogg_bytes)
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
            client: None,
            stop_tx: None,
            stop_rx: None,
            oauth_flow: None,
            qr_grant: None,
            #[cfg(not(test))]
            handler: None,
            #[cfg(not(test))]
            sync_service: None,
            #[cfg(not(test))]
            timelines: parking_lot::RwLock::new(HashMap::new()),
            #[cfg(not(test))]
            thread_timelines: parking_lot::RwLock::new(HashMap::new()),
            #[cfg(not(test))]
            thread_lists: parking_lot::RwLock::new(HashMap::new()),
            #[cfg(not(test))]
            backfill_task: None,
            #[cfg(not(test))]
            prefetch_task: None,
            #[cfg(not(test))]
            pending_prefetch: Arc::new(parking_lot::Mutex::new(None)),
            #[cfg(not(test))]
            warm_semaphore: Arc::new(tokio::sync::Semaphore::new(3)),
            #[cfg(not(test))]
            crypto_reset_handle: None,
            #[cfg(not(test))]
            in_flight: Arc::new(std::sync::atomic::AtomicU32::new(0)),
            #[cfg(not(test))]
            #[cfg(debug_assertions)]
            in_flight_urls: Arc::new(Mutex::new(Vec::new())),
            #[cfg(not(test))]
            backfill_previews: Arc::new(Mutex::new(HashMap::new())),
            #[cfg(not(test))]
            app_cache_db: Arc::new(Mutex::new(None)),
            #[cfg(not(test))]
            search_db: Arc::new(Mutex::new(None)),
            #[cfg(not(test))]
            search_indexing_enabled: Arc::new(std::sync::atomic::AtomicBool::new(false)),
            #[cfg(not(test))]
            sync_tasks: Vec::new(),
            #[cfg(not(test))]
            event_handler_handles: Vec::new(),
            #[cfg(not(test))]
            backup_state_code: Arc::new(std::sync::atomic::AtomicU8::new(BACKUP_STATE_UNKNOWN)),
            #[cfg(not(test))]
            imported_keys: Arc::new(AtomicU64::new(0)),
            media_upload_limit: AtomicU64::new(0),
            #[cfg(not(test))]
            image_packs: Arc::new(Mutex::new(Vec::new())),
            #[cfg(not(test))]
            user_pack_write_pending: Arc::new(std::sync::atomic::AtomicBool::new(false)),
            #[cfg(not(test))]
            packs_dirty: Arc::new(std::sync::atomic::AtomicBool::new(true)),
            #[cfg(not(test))]
            account_data_lock: Arc::new(tokio::sync::Mutex::new(())),
            data_dir: default_data_dir(),
            http_client: reqwest::Client::builder()
                .user_agent("Tesseract/0.1 (Matrix client)")
                .connect_timeout(std::time::Duration::from_secs(10))
                .timeout(std::time::Duration::from_secs(10))
                .http2_adaptive_window(true)
                .build()
                .unwrap_or_else(|_| reqwest::Client::new()),
            presence_polling_enabled: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(true)),
            #[cfg(not(test))]
            last_sync_room_subscriptions: Arc::new(parking_lot::Mutex::new(
                std::collections::HashSet::new(),
            )),
            #[cfg(not(test))]
            dm_counterparts: Arc::new(parking_lot::RwLock::new(
                std::collections::HashSet::new(),
            )),
            #[cfg(not(test))]
            forbidden_presence: Arc::new(Mutex::new(
                std::collections::HashSet::new(),
            )),
            #[cfg(not(test))]
            verification_flow_users: Arc::new(Mutex::new(HashMap::new())),
            #[cfg(not(test))]
            sas_emoji_cache: Arc::new(Mutex::new(HashMap::new())),
            #[cfg(not(test))]
            verification_tasks: Arc::new(Mutex::new(Vec::new())),
            // ceiling = 2× base: lets the queue keep flowing past a few stuck
            // downloads (which stop counting after the stall deadline) while
            // bounding how many hung connections a mass stall can accumulate.
            #[cfg(not(test))]
            media_gate_fg: media_gate::PriorityGate::new(
                MEDIA_FG_PERMITS, MEDIA_FG_PERMITS * 2),
            #[cfg(not(test))]
            media_gate_bulk: media_gate::PriorityGate::new(
                MEDIA_BULK_PERMITS, MEDIA_BULK_PERMITS * 2),
            #[cfg(not(test))]
            media_tasks: Arc::new(Mutex::new(HashMap::new())),
            #[cfg(not(test))]
            sdk_media_fetched: Arc::new(Mutex::new(std::collections::HashSet::new())),
            profile_fields_prefix: std::sync::Arc::new(std::sync::RwLock::new(None)),
            rt: tokio::runtime::Builder::new_multi_thread()
                .enable_all()
                // Timeline construction collects cached events
                // into an `imbl::Vector`; chunk promotion for
                // large `TimelineEvent`s recurses deeply. The
                // 2 MB tokio default is tight, so widen it.
                .thread_stack_size(8 * 1024 * 1024)
                .build()
                .expect("tokio runtime"),
        }
    }

    /// Override the per-instance data directory. Callers should invoke this
    /// immediately after `new()` and before `oauth_begin` / `restore_session`
    /// so the matrix-sdk SQLite store is opened at the right path. Creates
    /// the directory if it does not exist; silently ignores empty input so
    /// FFI callers that pass through an empty string keep the default.
    pub fn set_data_dir(&mut self, path: &str) {
        if path.is_empty() {
            return;
        }
        let p = PathBuf::from(path);
        let _ = std::fs::create_dir_all(&p);
        self.data_dir = p;
    }


    // -----------------------------------------------------------------------
    // Sync loop (Step 2: SyncService + RoomListService)
    //
    // The production impls of `start_sync` and `stop_sync` live in
    // `client::sync`. The test-only stubs below let unit tests instantiate a
    // `ClientFfi` and exercise the trivial no-op paths without dragging in
    // the SyncService / EventHandlerBridge machinery.
    // -----------------------------------------------------------------------

    #[cfg(test)]
    pub fn stop_sync(&mut self) {
        if let Some(tx) = self.stop_tx.take() {
            let _ = tx.send(true);
        }
        // Cancel any in-flight unread prefetch — it's a pure UX warm-up with no
        // persistence obligation, so logout/account-teardown should stop it
        // rather than waste bandwidth fetching into a cache we're discarding.
        #[cfg(not(test))]
        if let Some(h) = self.prefetch_task.take() {
            h.abort();
            *self.pending_prefetch.lock() = None;
        }
    }



    // Sync the full set of currently open rooms to the server's sliding-sync
    // subscription. Must be called after every subscribe_room/unsubscribe_room
    // so the server always sees the union of all open timeline rooms.
    // RoomListService::subscribe_to_rooms calls clear_and_subscribe internally,
    // so we always pass the complete set — never just the delta.
    //
    // Diff-aware: skips the re-push when the set is identical to the last one
    // pushed. matrix-sdk's `subscribe_to_rooms` walks the state store for
    // every subscribed room, which triggers `chunk_large_query_over` calls;
    // re-pushing an unchanged set during routine UI churn (selecting the
    // already-active room, opening a thread in an already-subscribed room)
    // burned CPU on low-power machines for no observable benefit.
    #[cfg(not(test))]
    pub(super) fn sync_room_subscriptions(&self) {
        let Some(svc) = self.sync_service.clone() else {
            return;
        };
        let mut new_set: std::collections::HashSet<OwnedRoomId> =
            self.timelines.read().keys().cloned().collect();
        for (rid, _root) in self.thread_timelines.read().keys() {
            new_set.insert(rid.clone());
        }
        // Compare-and-swap against the last-pushed set. If identical, skip
        // the spawn entirely. The lock window is short (set comparison +
        // assignment) and only contended by sync_room_subscriptions itself.
        {
            let mut guard = self.last_sync_room_subscriptions.lock();
            if *guard == new_set {
                return;
            }
            *guard = new_set.clone();
        }
        let ids: Vec<OwnedRoomId> = new_set.into_iter().collect();
        self.rt.spawn(async move {
            let refs: Vec<&matrix_sdk::ruma::RoomId> =
                ids.iter().map(OwnedRoomId::as_ref).collect();
            svc.room_list_service().subscribe_to_rooms(&refs).await;
        });
    }





    // -----------------------------------------------------------------------
    // Presence polling toggle
    // -----------------------------------------------------------------------

    /// Enable or disable background presence polling. Thread-safe — may be
    /// called from the UI thread while the polling task runs on a worker.
    /// (Production impl lives alongside `start_sync` in `client::sync`; this
    /// test stub mirrors that behaviour without pulling in the watcher task.)
    #[cfg(test)]
    pub fn set_presence_polling_enabled(&self, enabled: bool) {
        self.presence_polling_enabled
            .store(enabled, std::sync::atomic::Ordering::Relaxed);
    }

    /// Test-only no-op stub mirroring the production `poll_presence_now`
    /// (which lives in `client::sync` and needs the watcher task to exist).
    #[cfg(test)]
    pub fn poll_presence_now(&mut self) {}

}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Resolves once the stop watch channel fires `true` or the sender is dropped.
/// When `stop_rx` is `None` (sync not yet started) the future never resolves,
/// so `tokio::select!` callers will just wait for the other branch.
pub(super) async fn stop_fut(stop_rx: Option<watch::Receiver<bool>>) {
    let Some(mut rx) = stop_rx else {
        std::future::pending::<()>().await;
        return;
    };
    loop {
        match rx.changed().await {
            Ok(()) => {
                if *rx.borrow() {
                    return;
                }
            }
            Err(_) => return,
        }
    }
}


/// Rebuild the aggregated image-pack cache. Reads prefer the unstable
/// `im.ponies.*` names (first entry in `EMOTE_ROOMS_TYPES` / `ROOM_PACK_TYPES`)
/// since most homeservers and rooms only carry those. Returns an empty Vec
/// when not logged in.
#[cfg(not(test))]
pub(super) async fn rebuild_image_packs(
    client: &Client,
    http_cache: &mut std::collections::HashMap<(OwnedRoomId, String), Option<serde_json::Value>>,
) -> Vec<crate::image_packs::ImagePack> {
    use matrix_sdk::ruma::events::{GlobalAccountDataEventType, StateEventType};
    use serde_json::Value;

    let mut packs: Vec<crate::image_packs::ImagePack> = Vec::new();

    // -- User pack (account_data) --
    // MSC2545 defines no personal pack, so there is only the de-facto
    // `im.ponies.user_emotes` — no stable name to prefer.
    {
        let et = GlobalAccountDataEventType::from(crate::image_packs::TYPE_USER_PACK);
        if let Ok(Some(raw)) = client.account().account_data_raw(et).await {
            if let Ok(content) = serde_json::from_str::<Value>(raw.json().get()) {
                if let Some(mut pack) = crate::image_packs::parse_pack_content(
                    "user".to_owned(),
                    crate::image_packs::PackSource::User,
                    &content,
                ) {
                    if pack.display_name.is_empty() {
                        pack.display_name = "Saved Stickers".to_owned();
                    }
                    packs.push(pack);
                }
            }
        }
    }

    // -- Globally enabled room packs (account_data) --
    let mut room_refs: Vec<(String, String)> = Vec::new();
    for ev_type_str in crate::image_packs::EMOTE_ROOMS_TYPES {
        let et = GlobalAccountDataEventType::from(ev_type_str);
        let Ok(Some(raw)) = client.account().account_data_raw(et).await else {
            continue;
        };
        let Ok(content) = serde_json::from_str::<Value>(raw.json().get()) else {
            continue;
        };
        room_refs = crate::image_packs::iter_emote_rooms(&content);
        break;
    }

    use matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState;
    for (room_id_str, state_key) in room_refs {
        let Ok(room_id) = room_id_str.parse::<OwnedRoomId>() else {
            continue;
        };

        // Helper: extract content Value from a state event envelope.
        let extract_content = |raw_state: &RawAnySyncOrStrippedState| -> Option<Value> {
            match raw_state {
                RawAnySyncOrStrippedState::Sync(raw) => raw.get_field("content").ok().flatten(),
                RawAnySyncOrStrippedState::Stripped(raw) => raw.get_field("content").ok().flatten(),
            }
        };

        let mut found = false;

        // Fast path: local SSS cache.
        if let Some(room) = client.get_room(&room_id) {
            for ev_type_str in crate::image_packs::ROOM_PACK_TYPES {
                let et = StateEventType::from(ev_type_str);
                // `RawAnySyncOrStrippedState` is an untagged enum wrapping a
                // `Raw<AnySyncStateEvent>` or `Raw<AnyStrippedStateEvent>`. Both
                // variants carry `{type, content, state_key,...}`.
                let Ok(Some(raw_state)) = room.get_state_event(et, &state_key).await else {
                    continue;
                };
                let Some(content) = extract_content(&raw_state) else {
                    continue;
                };
                let source = crate::image_packs::PackSource::Room {
                    room_id: room_id_str.clone(),
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
        // the local cache on first subscription.  We fetch once per
        // (room_id, state_key) per session and cache the result — subsequent
        // notable-update rebuilds reuse it without hitting the server again.
        // Guard: only attempt when the user is currently joined — a stale
        // subscription entry for a room the user has left produces a 403.
        let is_joined = client
            .get_room(&room_id)
            .map(|r| r.state() == matrix_sdk::RoomState::Joined)
            .unwrap_or(false);
        if !found && is_joined {
            let cache_key = (room_id.clone(), state_key.clone());
            let cached = http_cache.get(&cache_key).cloned();
            let content_opt: Option<Value> = match cached {
                Some(v) => v, // already fetched this session (may be None = not found)
                None => {
                    use matrix_sdk::ruma::api::client::state::get_state_event_for_key;
                    let mut fetched: Option<Value> = None;
                    for ev_type_str in crate::image_packs::ROOM_PACK_TYPES {
                        let et = StateEventType::from(ev_type_str);
                        let req = get_state_event_for_key::v3::Request::new(
                            room_id.clone(),
                            et,
                            state_key.clone(),
                        );
                        if let Ok(response) = client.send(req).await {
                            if let Ok(content) = serde_json::from_str::<Value>(
                                response.event_or_content.get(),
                            ) {
                                fetched = Some(content);
                                break;
                            }
                        }
                    }
                    http_cache.insert(cache_key, fetched.clone());
                    fetched
                }
            };
            if let Some(content) = content_opt {
                let source = crate::image_packs::PackSource::Room {
                    room_id: room_id_str.clone(),
                    state_key: state_key.clone(),
                };
                let id = crate::image_packs::pack_id_for(&source);
                if let Some(pack) = crate::image_packs::parse_pack_content(id, source, &content) {
                    packs.push(pack);
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
        for ev_type_str in crate::image_packs::ROOM_PACK_TYPES {
            let et = StateEventType::from(ev_type_str);
            let Ok(events) = room.get_state_events(et).await else {
                continue;
            };
            for raw_state in &events {
                let (state_key, content_opt): (String, Option<Value>) = match raw_state {
                    RawAnySyncOrStrippedState::Sync(raw) => (
                        raw.get_field("state_key")
                            .ok()
                            .flatten()
                            .unwrap_or_default(),
                        raw.get_field("content").ok().flatten(),
                    ),
                    RawAnySyncOrStrippedState::Stripped(raw) => (
                        raw.get_field("state_key")
                            .ok()
                            .flatten()
                            .unwrap_or_default(),
                        raw.get_field("content").ok().flatten(),
                    ),
                };
                let source = crate::image_packs::PackSource::Room {
                    room_id: room_id_str.clone(),
                    state_key: state_key.clone(),
                };
                let id = crate::image_packs::pack_id_for(&source);
                if !added_ids.insert(id.clone()) {
                    continue;
                }
                let Some(content) = content_opt else { continue };
                if let Some(mut pack) = crate::image_packs::parse_pack_content(id, source, &content)
                {
                    if pack.display_name.is_empty() {
                        pack.display_name = room
                            .display_name()
                            .await
                            .map(|n| n.to_string())
                            .unwrap_or_default();
                    }
                    packs.push(pack);
                }
            }
            if !events.is_empty() {
                break;
            }
        }
    }

    packs
}

/// Preview of a room's latest event for the room-list sidebar.
/// `kind`: "text" | "image" | "video" | "file" | "audio" | "sticker" | ""
/// (empty = nothing to preview). `text` is the first plain line (text-like
/// kinds only). `sticker_url` is the mxc URI for "sticker".
/// `thumbnail_url` is the mxc URI used for the room-list thumbnail chip:
/// populated for "image" (the image's own mxc) and "sticker" (same as sticker_url).
#[derive(Debug, Default, PartialEq)]
pub(super) struct LatestPreview {
    pub(super) kind: String,
    pub(super) text: String,
    pub(super) sticker_url: String,
    pub(super) thumbnail_url: String,
}

/// First non-empty line of `s`, trimmed. Splits on the first '\n'.
pub(super) fn first_line(s: &str) -> String {
    s.lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .unwrap_or("")
        .to_owned()
}

/// Strip a Matrix `formatted_body` (HTML subset) to its first plain-text
/// line. Block/break tags become newlines; all other tags are dropped;
/// the handful of entities Matrix uses are decoded. Char-safe (UTF-8).
/// No external deps.
pub(super) fn html_first_line(html: &str) -> String {
    let mut out = String::with_capacity(html.len());
    let mut in_tag = false;
    let mut tag = String::new();
    for c in html.chars() {
        if in_tag {
            if c == '>' {
                in_tag = false;
                let lower = tag.to_ascii_lowercase();
                let name: String = lower
                    .trim_start_matches('/')
                    .chars()
                    .take_while(|c| c.is_ascii_alphanumeric())
                    .collect();
                let breaks = matches!(
                    name.as_str(),
                    "br" | "p"
                        | "div"
                        | "li"
                        | "tr"
                        | "blockquote"
                        | "h1"
                        | "h2"
                        | "h3"
                        | "h4"
                        | "h5"
                        | "h6"
                );
                if breaks {
                    out.push('\n');
                }
                tag.clear();
            } else {
                tag.push(c);
            }
        } else if c == '<' {
            in_tag = true;
        } else {
            out.push(c);
        }
    }
    let decoded = out
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        .replace("&nbsp;", " ")
        .replace("&amp;", "&");
    first_line(&decoded)
}

fn latest_event_preview(value: &matrix_sdk::latest_events::LatestEventValue) -> LatestPreview {
    use matrix_sdk::latest_events::LatestEventValue;
    use matrix_sdk::ruma::events::{
        room::message::MessageType, room::MediaSource, sticker::StickerMediaSource,
        AnySyncMessageLikeEvent, AnySyncTimelineEvent,
    };

    let text_kind = |body: &str, formatted: Option<&str>| -> LatestPreview {
        let line = match formatted {
            Some(html) if !html.trim().is_empty() => html_first_line(html),
            _ => String::new(),
        };
        let line = if line.is_empty() {
            first_line(body)
        } else {
            line
        };
        if line.is_empty() {
            LatestPreview::default()
        } else {
            LatestPreview {
                kind: "text".to_owned(),
                text: line,
                sticker_url: String::new(),
                thumbnail_url: String::new(),
            }
        }
    };
    let media_kind = |k: &str| LatestPreview {
        kind: k.to_owned(),
        text: String::new(),
        sticker_url: String::new(),
        thumbnail_url: String::new(),
    };

    match value {
        LatestEventValue::Remote(timeline_event) => {
            let Some(event) = timeline_event.raw().deserialize().ok() else {
                return LatestPreview::default();
            };
            match event {
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(ev)) => {
                    let Some(orig) = ev.as_original() else {
                        return LatestPreview::default();
                    };
                    match &orig.content.msgtype {
                        MessageType::Text(t) => {
                            text_kind(&t.body, t.formatted.as_ref().map(|f| f.body.as_str()))
                        }
                        MessageType::Notice(n) => {
                            text_kind(&n.body, n.formatted.as_ref().map(|f| f.body.as_str()))
                        }
                        MessageType::Emote(e) => {
                            text_kind(&e.body, e.formatted.as_ref().map(|f| f.body.as_str()))
                        }
                        MessageType::Image(img) => {
                            // For encrypted images, serialise the full
                            // MediaSource so fetch_source_bytes can re-parse
                            // the EncryptedFile and decrypt — matches the
                            // timeline path. Extracting just `f.url` would
                            // ship the ciphertext URL with no key and the
                            // C++ side would loop refetching undecryptable
                            // bytes.
                            let url = match &img.source {
                                MediaSource::Plain(uri) => uri.to_string(),
                                MediaSource::Encrypted(_) => {
                                    serde_json::to_string(&img.source)
                                        .unwrap_or_default()
                                }
                            };
                            LatestPreview {
                                kind: "image".to_owned(),
                                text: String::new(),
                                sticker_url: String::new(),
                                thumbnail_url: url,
                            }
                        }
                        MessageType::Video(_) => {
                            let is_gif = serde_json::from_str::<serde_json::Value>(
                                timeline_event.raw().json().get(),
                            )
                            .ok()
                            .and_then(|v| {
                                v.pointer("/content/info/fi.mau.gif")
                                    .and_then(|v| v.as_bool())
                            })
                            .unwrap_or(false);
                            media_kind(if is_gif { "gif" } else { "video" })
                        }
                        MessageType::File(_) => media_kind("file"),
                        MessageType::Audio(_) => media_kind("audio"),
                        _ => LatestPreview::default(),
                    }
                }
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(ev)) => {
                    let Some(orig) = ev.as_original() else {
                        return LatestPreview::default();
                    };
                    // For encrypted stickers, serialise the full MediaSource
                    // so fetch_source_bytes can re-parse the EncryptedFile
                    // and decrypt — matches the timeline path's source_json.
                    // Extracting just `f.url` shipped the encrypted-ciphertext
                    // URL with no key, causing the C++ side to refetch in a
                    // loop because the bytes never decoded.
                    let url = match &orig.content.source {
                        StickerMediaSource::Plain(uri) => uri.to_string(),
                        StickerMediaSource::Encrypted(f) => {
                            let ms = MediaSource::Encrypted(f.clone());
                            serde_json::to_string(&ms).unwrap_or_default()
                        }
                        _ => String::new(),
                    };
                    LatestPreview {
                        kind: "sticker".to_owned(),
                        text: String::new(),
                        sticker_url: url.clone(),
                        thumbnail_url: url,
                    }
                }
                _ => LatestPreview::default(),
            }
        }
        LatestEventValue::LocalIsSending(local) | LatestEventValue::LocalCannotBeSent(local) => {
            extract_local_preview(&local.content)
        }
        LatestEventValue::LocalHasBeenSent { value: local, .. } => {
            extract_local_preview(&local.content)
        }
        LatestEventValue::None | LatestEventValue::RemoteInvite { .. } => LatestPreview::default(),
    }
}

fn latest_event_sender(
    value: &matrix_sdk::latest_events::LatestEventValue,
) -> Option<matrix_sdk::ruma::OwnedUserId> {
    use matrix_sdk::latest_events::LatestEventValue;
    use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};

    match value {
        LatestEventValue::Remote(timeline_event) => {
            let event = timeline_event.raw().deserialize().ok()?;
            match event {
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(ev)) => {
                    ev.as_original().map(|e| e.sender.clone())
                }
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(ev)) => {
                    ev.as_original().map(|e| e.sender.clone())
                }
                _ => None,
            }
        }
        _ => None,
    }
}

fn extract_local_preview(content: &matrix_sdk::store::SerializableEventContent) -> LatestPreview {
    use matrix_sdk::ruma::events::{room::message::MessageType, AnyMessageLikeEventContent};
    let Some(c) = content.deserialize().ok() else {
        return LatestPreview::default();
    };
    let msgtype = match c {
        AnyMessageLikeEventContent::RoomMessage(c) => c.msgtype,
        _ => return LatestPreview::default(),
    };
    let text_kind = |body: &str, formatted: Option<&str>| -> LatestPreview {
        let line = match formatted {
            Some(html) if !html.trim().is_empty() => html_first_line(html),
            _ => String::new(),
        };
        let line = if line.is_empty() {
            first_line(body)
        } else {
            line
        };
        if line.is_empty() {
            LatestPreview::default()
        } else {
            LatestPreview {
                kind: "text".to_owned(),
                text: line,
                sticker_url: String::new(),
                thumbnail_url: String::new(),
            }
        }
    };
    let media_kind = |k: &str| LatestPreview {
        kind: k.to_owned(),
        text: String::new(),
        sticker_url: String::new(),
        thumbnail_url: String::new(),
    };
    match msgtype {
        MessageType::Text(t) => text_kind(&t.body, t.formatted.as_ref().map(|f| f.body.as_str())),
        MessageType::Notice(n) => text_kind(&n.body, n.formatted.as_ref().map(|f| f.body.as_str())),
        MessageType::Emote(e) => text_kind(&e.body, e.formatted.as_ref().map(|f| f.body.as_str())),
        MessageType::Image(_) => media_kind("image"),
        MessageType::Video(_) => {
            let is_gif = serde_json::to_value(content)
                .ok()
                .and_then(|v| {
                    v.pointer("/event/info/fi.mau.gif")
                        .and_then(|v| v.as_bool())
                })
                .unwrap_or(false);
            media_kind(if is_gif { "gif" } else { "video" })
        }
        MessageType::File(_) => media_kind("file"),
        MessageType::Audio(_) => media_kind("audio"),
        _ => LatestPreview::default(),
    }
}

/// Pick the "other user" of a DM room, skipping the current user and any
/// functional members (bridge bots, per MSC4171). Returns None when no real
/// counterpart can be identified.
///
/// Performance: relies on matrix-sdk's in-memory `service_members()` and
/// `active_members_count()` accessors to avoid the expensive
/// `room.members(JOIN).await` for any room that obviously isn't a 1:1.
#[cfg(test)]
pub(super) async fn dm_other_user(
    _room: &matrix_sdk::Room,
    _me: &matrix_sdk::ruma::UserId,
) -> Option<crate::ffi::RoomMember> {
    None
}

#[cfg(not(test))]
pub(super) async fn dm_other_user(room: &Room, me: &UserId) -> Option<crate::ffi::RoomMember> {
    let functional = room.service_members().unwrap_or_default();

    let to_bridge = |m: &matrix_sdk::room::RoomMember| -> crate::ffi::RoomMember {
        let uid = m.user_id();
        let display_name = m
            .display_name()
            .map(str::to_owned)
            .unwrap_or_else(|| uid.localpart().to_string());
        let avatar_url = m.avatar_url().map(|u| u.to_string()).unwrap_or_default();
        crate::ffi::RoomMember {
            user_id: uid.to_string(),
            display_name,
            avatar_url,
        }
    };

    // 1) Prefer m.direct: the puppet user is here, the bridge bot is not.
    //    This path is cheap (in-memory) and works for properly-marked DMs.
    for target in room.direct_targets() {
        let Some(uid) = target.as_user_id() else {
            continue; // 3PID / email / phone target — skip.
        };
        if uid == me || functional.contains(uid) {
            continue;
        }
        if let Ok(Some(m)) = room.get_member_no_sync(uid).await {
            return Some(to_bridge(&m));
        }
    }

    // 2) Fallback for rooms that aren't tagged in m.direct: look at the
    //    member list, but only after a cheap precheck that there are
    //    exactly 2 real members (= me + counterpart). Skips the expensive
    //    members() fetch on group rooms.
    let active = room.active_members_count();
    let service = room.active_service_members_count().unwrap_or(0);
    if active.saturating_sub(service) != 2 {
        return None;
    }
    if let Ok(members) = room.members(matrix_sdk::RoomMemberships::JOIN).await {
        let mut real = members
            .iter()
            .filter(|m| m.user_id() != me && !functional.contains(m.user_id()));
        if let Some(first) = real.next() {
            if real.next().is_none() {
                return Some(to_bridge(first));
            }
        }
    }
    None
}

/// Resolve a single pinned event id against the local event cache (cheap;
/// no network round-trip) and produce the `PinnedEvent` snapshot rendered by
/// the banner UI. Falls back to `(unavailable)` when the id can't be
/// resolved from cache — click-to-jump still works for events visible in
/// loaded history.
#[cfg(not(test))]
async fn resolve_pinned_event(
    room: &Room,
    event_id: &matrix_sdk::ruma::OwnedEventId,
) -> crate::ffi::PinnedEvent {
    use matrix_sdk::ruma::events::room::message::MessageType;
    use matrix_sdk::ruma::events::{AnySyncMessageLikeEvent, AnySyncTimelineEvent};

    let mut out = crate::ffi::PinnedEvent {
        event_id: event_id.to_string(),
        sender_name: String::new(),
        body_preview: String::new(),
        timestamp: 0,
    };

    // Try cache → disk → network. `load_or_fetch_event` is the only
    // matrix-sdk API that loads from the SQLite store and falls back to
    // /event/{id} when neither cache nor store has the event. The earlier
    // `find_event`-only path was cache-only, which produced "(unavailable)"
    // for every pin not in the live timeline window — i.e. almost all of
    // them. Once fetched, the event lands in the cache, so subsequent
    // build_room_infos ticks for the same pin are zero-cost.
    let ev = match room.load_or_fetch_event(event_id, None).await {
        Ok(ev) => ev,
        Err(_) => {
            out.body_preview = "(unavailable)".to_owned();
            return out;
        }
    };

    // Body / kind preview — mirrors `msglike_snippet` in timeline_convert.rs.
    let Ok(any) = ev.raw().deserialize() else {
        out.body_preview = "(unavailable)".to_owned();
        return out;
    };

    let (sender, ts_ms, body) = match any {
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(m)) => {
            let Some(orig) = m.as_original() else {
                out.body_preview = "(deleted)".to_owned();
                return out;
            };
            let body = match &orig.content.msgtype {
                MessageType::Text(t) => first_line(&t.body),
                MessageType::Notice(n) => first_line(&n.body),
                MessageType::Emote(e) => first_line(&e.body),
                MessageType::Image(_) => "(image)".to_owned(),
                MessageType::File(_) => "(file)".to_owned(),
                MessageType::Audio(a) => {
                    if a.voice.is_some() {
                        "(voice)".to_owned()
                    } else {
                        "(audio)".to_owned()
                    }
                }
                MessageType::Video(_) => "(video)".to_owned(),
                _ => "(message)".to_owned(),
            };
            (
                Some(orig.sender.clone()),
                u64::from(orig.origin_server_ts.0),
                body,
            )
        }
        AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(s)) => {
            let Some(orig) = s.as_original() else {
                out.body_preview = "(deleted)".to_owned();
                return out;
            };
            (
                Some(orig.sender.clone()),
                u64::from(orig.origin_server_ts.0),
                "(sticker)".to_owned(),
            )
        }
        _ => {
            out.body_preview = "(message)".to_owned();
            return out;
        }
    };

    out.timestamp = ts_ms;
    out.body_preview = body;
    if let Some(uid) = sender {
        out.sender_name = member_display_name_local(room, &uid).await;
    }
    out
}

/// Build a single `RoomInfo` snapshot from a `Room`. Returns `None` for
/// tombstoned rooms (filtered out of the UI list). Called both during the
/// initial `joined_rooms()` walk in `build_room_infos` and per-room from the
/// incremental room-info-update watcher in `sync.rs` so we don't pay the
/// O(N) SQLite fan-out cost when only one room changed.
#[cfg(not(test))]
pub(super) async fn build_room_info(
    client: &Client,
    room: &Room,
) -> Option<crate::ffi::RoomInfo> {
    if room.is_tombstoned() {
        return None;
    }
    let name = room
        .display_name()
        .await
        .map(|n| n.to_string())
        .unwrap_or_else(|_| room.room_id().to_string());
    let is_space = room.is_space();
    // Prefer the client-side computed counts over the server-reported
    // `unread_notification_counts()` because the latter is empty for
    // encrypted rooms (the server can't apply push rules to ciphertext),
    // which on E2EE-heavy accounts means every room reports 0 unreads.
    // See matrix-sdk-base 0.17 docs on `Room::num_unread_notifications`.
    let notification_count = room.num_unread_notifications();
    let highlight_count    = room.num_unread_mentions();
    // Total unread (regardless of push rules) drives the room-list quiet-unread
    // dot; muted rooms are excluded from it. Same client-side read-receipt
    // source as the counts above (reliable for encrypted rooms).
    let unread_count = room.num_unread_messages();
    let muted = matches!(
        room.cached_user_defined_notification_mode(),
        Some(matrix_sdk::notification_settings::RoomNotificationMode::Mute));
    let last_activity_ts = room
        .latest_event_timestamp()
        .map(|t| u64::from(t.0))
        .unwrap_or(0);
    let tags = room.tags().await.ok().flatten();
    let is_favorite = tags
        .as_ref()
        .map(|t| t.contains_key(&matrix_sdk::ruma::events::tag::TagName::Favorite))
        .unwrap_or(false);
    let is_low_priority = tags
        .as_ref()
        .map(|t| t.contains_key(&matrix_sdk::ruma::events::tag::TagName::LowPriority))
        .unwrap_or(false);
    // MSC3765: extract HTML body from the m.topic content block when present.
    let topic_html = {
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::{room::topic::RoomTopicEventContent, SyncStateEvent};
        room.get_state_event_static::<RoomTopicEventContent>()
            .await
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

    // Deref to base Room to avoid matrix-sdk-ui RoomExt shadowing latest_event()
    // with an async version (same trick as mark_room_as_read, line 2148).
    let lev = std::ops::Deref::deref(room).latest_event();
    let LatestPreview {
        kind: last_message_kind,
        text: last_message_body,
        sticker_url: last_message_sticker_url,
        thumbnail_url: last_message_thumbnail_url,
    } = latest_event_preview(&lev);
    let last_message_sender_name = if last_message_kind.is_empty() {
        String::new()
    } else {
        match latest_event_sender(&lev) {
            Some(sender) if client.user_id().is_some_and(|me| me != &*sender) => {
                member_display_name_local(room, &sender).await
            }
            _ => String::new(),
        }
    };
    let is_encrypted = room.encryption_state().is_encrypted();
    let history_visibility = {
        use matrix_sdk::ruma::events::room::history_visibility::HistoryVisibility;
        match room.history_visibility_or_default() {
            HistoryVisibility::WorldReadable => "world_readable".to_string(),
            HistoryVisibility::Shared => "shared".to_string(),
            HistoryVisibility::Invited => "invited".to_string(),
            HistoryVisibility::Joined => "joined".to_string(),
            _ => "shared".to_string(),
        }
    };
    let is_direct = room.is_direct().await.unwrap_or(false);
    let avatar_url = room.avatar_url().map(|u| u.to_string()).unwrap_or_default();
    // Fallback avatar when the room has no avatar of its own: the other
    // participant's mxc. Bridge bots (functional members) are excluded
    // so bridged 1:1s show the puppet's avatar, not the bot's.
    //
    // We deliberately do NOT gate on `is_direct` — many rooms users
    // treat as DMs aren't actually marked in m.direct account data, but
    // dm_other_user already protects against false positives by only
    // returning Some when there is exactly one real counterpart.
    // dm_other_user is called for all non-space rooms (cheap: in-memory path
    // for m.direct rooms; early-out on member-count for group rooms).
    // dm_avatar_url is only used as a fallback when the room has no avatar.
    // dm_counterpart_user_id is needed for presence lookups regardless of avatar.
    let dm_other = if !is_space {
        match client.user_id() {
            Some(me) => dm_other_user(room, me).await,
            None => None,
        }
    } else {
        None
    };
    let dm_avatar_url = if avatar_url.is_empty() {
        dm_other.as_ref().map(|m| m.avatar_url.clone()).unwrap_or_default()
    } else {
        String::new()
    };
    let dm_counterpart_user_id = dm_other.map(|m| m.user_id).unwrap_or_default();

    // m.room.pinned_events: resolve each id against the local event cache so
    // the banner UI can render sender + body snippet without a separate
    // fetch round-trip. Sorted newest-first for the banner.
    // Resolved concurrently: each load_or_fetch_event may be a network call
    // on first access; join_all lets them overlap instead of serialising.
    let pinned_events: Vec<crate::ffi::PinnedEvent> = {
        use matrix_sdk::deserialized_responses::SyncOrStrippedState;
        use matrix_sdk::ruma::events::room::pinned_events::RoomPinnedEventsEventContent;
        use matrix_sdk::ruma::events::SyncStateEvent;
        let pinned_ids: Vec<matrix_sdk::ruma::OwnedEventId> = match room
            .get_state_event_static::<RoomPinnedEventsEventContent>()
            .await
        {
            Ok(Some(raw)) => match raw.deserialize() {
                Ok(SyncOrStrippedState::Sync(SyncStateEvent::Original(o))) => o.content.pinned,
                _ => Vec::new(),
            },
            _ => Vec::new(),
        };
        let mut resolved: Vec<crate::ffi::PinnedEvent> =
            futures_util::future::join_all(
                pinned_ids.iter().map(|id| resolve_pinned_event(room, id)),
            )
            .await;
        resolved.sort_by(|a, b| b.timestamp.cmp(&a.timestamp));
        resolved
    };

    Some(crate::ffi::RoomInfo {
        id: room.room_id().to_string(),
        name,
        topic: room.topic().unwrap_or_default(),
        topic_html,
        notification_count,
        highlight_count,
        unread_count,
        muted,
        is_direct,
        avatar_url,
        dm_avatar_url,
        dm_counterpart_user_id,
        last_message_body,
        last_message_sender_name,
        last_message_kind,
        last_message_sticker_url,
        last_message_thumbnail_url,
        last_activity_ts,
        is_space,
        is_favorite,
        is_low_priority,
        is_encrypted,
        history_visibility,
        pinned_events,
        canonical_alias: room
            .canonical_alias()
            .map(|a| a.to_string())
            .unwrap_or_default(),
    })
}

/// Sort a room-list snapshot in the order the UI expects: unread rooms first,
/// then by newest `last_activity_ts`. Per-shell space partitioning runs after
/// and preserves the order within each (non-space / space) bucket.
#[cfg(not(test))]
pub(super) fn sort_room_infos(rooms: &mut Vec<crate::ffi::RoomInfo>) {
    rooms.sort_by(|a, b| {
        let a_unread = a.notification_count > 0 || a.highlight_count > 0;
        let b_unread = b.notification_count > 0 || b.highlight_count > 0;
        b_unread
            .cmp(&a_unread)
            .then_with(|| b.last_activity_ts.cmp(&a.last_activity_ts))
            .then_with(|| a.id.cmp(&b.id))
    });
}

/// Change-detection fingerprint for the room-list snapshot. The sync watcher
/// re-emits the room list to the UI only when this fingerprint differs from the
/// previous notable update. It must therefore encode every field the UI orders
/// or *sections* by; a field left out here is silently dropped from live
/// updates until an unrelated change happens to perturb the fingerprint.
///
/// Not `#[cfg(not(test))]`: it is pure (operates on `RoomInfo` only) so it can
/// be unit-tested without a live client.
pub(super) fn room_list_fingerprint(
    rooms: &[crate::ffi::RoomInfo],
) -> Vec<(bool, bool, bool, bool, u64, String)> {
    let mut tmp: Vec<&crate::ffi::RoomInfo> = rooms.iter().collect();
    tmp.sort_by(|a, b| {
        let au = a.notification_count > 0 || a.highlight_count > 0;
        let bu = b.notification_count > 0 || b.highlight_count > 0;
        bu.cmp(&au)
            .then_with(|| b.last_activity_ts.cmp(&a.last_activity_ts))
            .then_with(|| a.id.cmp(&b.id))
    });
    tmp.iter()
        .map(|r| {
            let unread = r.notification_count > 0 || r.highlight_count > 0;
            // Quiet unread (room-list dot): unread messages with no notification,
            // not muted. Tracked separately from `unread` so that read-clearing a
            // quiet room (unread → 0 with no new event, so last_activity_ts is
            // unchanged) still changes the fingerprint and refreshes the dot.
            let quiet_unread = !unread && r.unread_count > 0 && !r.muted;
            // Include the favourite / low-priority tags: they change a room's
            // room-list section without affecting recency/unread ordering, so
            // omitting them here suppresses the live update for tag toggles.
            (
                unread,
                quiet_unread,
                r.is_favorite,
                r.is_low_priority,
                r.last_activity_ts,
                r.id.clone(),
            )
        })
        .collect()
}

#[cfg(not(test))]
pub(super) async fn build_room_infos(client: &Client) -> Vec<crate::ffi::RoomInfo> {
    let mut result = Vec::new();
    for room in client.joined_rooms() {
        if let Some(info) = build_room_info(client, &room).await {
            result.push(info);
        }
    }
    sort_room_infos(&mut result);
    result
}

/// Build a snapshot of all pending room invitations from the local SDK cache.
/// For each invited room, the sender of the local user's m.room.member
/// (Invited) stripped-state event is the inviter. `invited_at_ts` is 0
/// because stripped state events do not carry origin_server_ts unless the
/// homeserver implements MSC4319 (not in current ruma feature set).
#[cfg(not(test))]
pub(super) async fn build_invite_infos(client: &Client) -> Vec<crate::ffi::InviteInfo> {
    let mut result = Vec::new();
    for room in client.invited_rooms() {
        let room_id = room.room_id().to_string();
        let room_name = room
            .display_name()
            .await
            .map(|n| n.to_string())
            .unwrap_or_else(|_| room_id.clone());
        let room_avatar_url = room.avatar_url().map(|u| u.to_string()).unwrap_or_default();
        let room_topic = room.topic().unwrap_or_default();
        let is_direct = room.is_direct().await.unwrap_or(false);

        let (inviter_user_id, inviter_display_name, inviter_avatar_url, invited_at_ts) =
            match room.invite_details().await {
                Ok(details) => {
                    let uid = details.inviter_id.to_string();
                    // invited_at_ts: stripped-state events omit origin_server_ts;
                    // use the invitee event's ts when available (Sync path), else 0.
                    let ts = details
                        .invitee
                        .event()
                        .origin_server_ts()
                        .map(|t| u64::from(t.0))
                        .unwrap_or(0);
                    match details.inviter {
                        Some(m) => {
                            let dn = m
                                .display_name()
                                .map(str::to_owned)
                                .unwrap_or_else(|| details.inviter_id.localpart().to_string());
                            let av = m
                                .avatar_url()
                                .map(|u| u.to_string())
                                .unwrap_or_default();
                            (uid, dn, av, ts)
                        }
                        None => (uid, details.inviter_id.localpart().to_string(), String::new(), ts),
                    }
                }
                Err(_) => (String::new(), String::new(), String::new(), 0),
            };

        result.push(crate::ffi::InviteInfo {
            room_id,
            room_name,
            room_avatar_url,
            room_topic,
            is_direct,
            inviter_user_id,
            inviter_display_name,
            inviter_avatar_url,
            invited_at_ts,
        });
    }
    result
}

// ---------------------------------------------------------------------------
// SAS verification helpers
// ---------------------------------------------------------------------------

/// Watch a `VerificationRequest`'s state stream. Fires `on_verification_request`
/// (incoming=false) when the request is accepted (Ready state), then spawns a
/// SAS watcher when the flow transitions to `Transitioned { SasV1 }`.
/// Also surfaces top-level Done / Cancelled transitions before SAS starts.
// (Verification watchers moved to client/verification.rs)

// ---------------------------------------------------------------------------
// Server pushers (Step 12)
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    // Build a minimal RoomInfo for fingerprint tests.
    fn room(id: &str) -> crate::ffi::RoomInfo {
        crate::ffi::RoomInfo {
            id: id.to_owned(),
            ..Default::default()
        }
    }

    #[test]
    fn fingerprint_changes_when_favourite_toggles() {
        // A favourite toggle moves the room to a different room-list section, so
        // the snapshot fingerprint MUST change — otherwise the sync watcher
        // suppresses the emit and the UI doesn't update until restart.
        let mut r = room("!a:example.org");
        let before = room_list_fingerprint(std::slice::from_ref(&r));
        r.is_favorite = true;
        let after = room_list_fingerprint(std::slice::from_ref(&r));
        assert_ne!(before, after);
    }

    #[test]
    fn fingerprint_changes_when_low_priority_toggles() {
        let mut r = room("!a:example.org");
        let before = room_list_fingerprint(std::slice::from_ref(&r));
        r.is_low_priority = true;
        let after = room_list_fingerprint(std::slice::from_ref(&r));
        assert_ne!(before, after);
    }

    #[test]
    fn fingerprint_stable_when_nothing_relevant_changes() {
        let r = room("!a:example.org");
        assert_eq!(
            room_list_fingerprint(std::slice::from_ref(&r)),
            room_list_fingerprint(std::slice::from_ref(&r))
        );
    }

    #[test]
    fn fingerprint_changes_when_quiet_unread_toggles() {
        // A non-notifying message (unread_count 0 → 1 with notification_count
        // still 0) must change the fingerprint so the room-list dot appears /
        // clears live — last_activity_ts can be unchanged on a read-clear.
        let mut r = room("!a:example.org");
        let before = room_list_fingerprint(std::slice::from_ref(&r));
        r.unread_count = 1;
        let after = room_list_fingerprint(std::slice::from_ref(&r));
        assert_ne!(before, after);
    }

    #[test]
    fn fingerprint_ignores_quiet_unread_when_muted() {
        // A muted room shows no dot, so its unread count must not perturb the
        // fingerprint (otherwise busy muted rooms would spam UI refreshes).
        let mut r = room("!a:example.org");
        r.muted = true;
        let before = room_list_fingerprint(std::slice::from_ref(&r));
        r.unread_count = 5;
        let after = room_list_fingerprint(std::slice::from_ref(&r));
        assert_eq!(before, after);
    }

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

    // --- derive_mentions -------------------------------------------------

    #[test]
    fn derive_mentions_none_for_plain_html() {
        let (m, html) = derive_mentions("<p>hello <strong>world</strong></p>");
        assert!(m.is_none());
        assert_eq!(html, "<p>hello <strong>world</strong></p>");
    }

    #[test]
    fn derive_mentions_single_user_link() {
        let src = r#"hi <a href="https://matrix.to/#/@alice:example.org">Alice</a>!"#;
        let (m, html) = derive_mentions(src);
        let m = m.expect("mentions");
        assert!(!m.room);
        assert_eq!(m.user_ids.len(), 1);
        assert!(m
            .user_ids
            .iter()
            .any(|u| u.as_str() == "@alice:example.org"));
        // User anchors are preserved verbatim.
        assert_eq!(html, src);
    }

    #[test]
    fn derive_mentions_multiple_users() {
        let src = concat!(
            r#"<a href="https://matrix.to/#/@alice:example.org">Alice</a> "#,
            r#"<a href="https://matrix.to/#/@bob:example.org">Bob</a>"#
        );
        let (m, _) = derive_mentions(src);
        let m = m.expect("mentions");
        assert_eq!(m.user_ids.len(), 2);
    }

    #[test]
    fn derive_mentions_room_sentinel_rewritten() {
        let src = r#"<a href="https://matrix.to/#/@room">@room</a> heads up"#;
        let (m, html) = derive_mentions(src);
        let m = m.expect("mentions");
        assert!(m.room);
        assert!(m.user_ids.is_empty());
        // The sentinel anchor is replaced with its plain inner text.
        assert_eq!(html, "@room heads up");
    }

    #[test]
    fn derive_mentions_mixed_user_and_room() {
        let src = concat!(
            r#"<a href="https://matrix.to/#/@room">@room</a> and "#,
            r#"<a href="https://matrix.to/#/@alice:example.org">Alice</a>"#
        );
        let (m, html) = derive_mentions(src);
        let m = m.expect("mentions");
        assert!(m.room);
        assert_eq!(m.user_ids.len(), 1);
        assert_eq!(
            html,
            r#"@room and <a href="https://matrix.to/#/@alice:example.org">Alice</a>"#
        );
    }

    #[test]
    fn derive_mentions_ignores_plain_links() {
        let src = r#"see <a href="https://example.com/foo">this</a>"#;
        let (m, html) = derive_mentions(src);
        assert!(m.is_none());
        assert_eq!(html, src);
    }

    #[test]
    fn derive_mentions_strips_via_params() {
        let src =
            r#"<a href="https://matrix.to/#/@alice:example.org?via=example.org">A</a>"#;
        let (m, _) = derive_mentions(src);
        let m = m.expect("mentions");
        assert!(m
            .user_ids
            .iter()
            .any(|u| u.as_str() == "@alice:example.org"));
    }

    #[test]
    fn derive_mentions_malformed_anchor_no_panic() {
        let (m, html) = derive_mentions(r#"<a href="https://matrix.to/#/@x:y">oops"#);
        // No closing tag: nothing is derived, input is preserved.
        assert!(m.is_none());
        assert!(html.contains("oops"));
    }

    #[test]
    fn derive_mentions_does_not_match_abbr_tag() {
        let src = "<abbr>nope</abbr>";
        let (m, html) = derive_mentions(src);
        assert!(m.is_none());
        assert_eq!(html, src);
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
    fn list_invites_is_empty_when_not_logged_in() {
        let c = ClientFfi::new();
        assert!(c.list_invites().is_empty());
    }

    #[test]
    fn accept_invite_async_does_not_panic_when_not_logged_in() {
        let c = ClientFfi::new();
        c.accept_invite_async(0, "!room:example.com");
    }

    #[test]
    fn decline_invite_async_does_not_panic_when_not_logged_in() {
        let c = ClientFfi::new();
        c.decline_invite_async("!room:example.com");
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
    fn export_room_keys_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.export_room_keys("/tmp/keys.txt", "pass");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn import_room_keys_fails_when_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.import_room_keys("/tmp/keys.txt", "pass");
        assert!(!r.ok);
        assert_eq!(r.message, "not logged in");
    }

    #[test]
    fn set_presence_polling_enabled_roundtrips() {
        let mut c = ClientFfi::new();
        assert!(c.presence_polling_enabled.load(std::sync::atomic::Ordering::Relaxed));
        c.set_presence_polling_enabled(false);
        assert!(!c.presence_polling_enabled.load(std::sync::atomic::Ordering::Relaxed));
        c.set_presence_polling_enabled(true);
        assert!(c.presence_polling_enabled.load(std::sync::atomic::Ordering::Relaxed));
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
        assert_eq!(room_list_state_code(&S::Init), ROOM_LIST_STATE_INIT);
        assert_eq!(
            room_list_state_code(&S::SettingUp),
            ROOM_LIST_STATE_SETTING_UP
        );
        assert_eq!(
            room_list_state_code(&S::Recovering),
            ROOM_LIST_STATE_RECOVERING
        );
        assert_eq!(room_list_state_code(&S::Running), ROOM_LIST_STATE_RUNNING);
        assert_eq!(
            room_list_state_code(&S::Error {
                from: Box::new(S::Running)
            }),
            ROOM_LIST_STATE_ERROR,
        );
        assert_eq!(
            room_list_state_code(&S::Terminated {
                from: Box::new(S::Running)
            }),
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
        let max = block
            .waveform
            .iter()
            .map(|a| u64::from(a.get()))
            .max()
            .unwrap_or(0);
        assert_eq!(max, 1024);
        assert_eq!(
            content
                .info
                .as_deref()
                .and_then(|i| i.mimetype.clone())
                .as_deref(),
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
        assert!(
            content.voice.is_none(),
            "plain audio must not carry the voice marker"
        );
        assert!(
            content.audio.is_none(),
            "plain audio carries no MSC1767 details"
        );
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
    fn thread_relation_message_shape() {
        let root: matrix_sdk::ruma::OwnedEventId = "$root:server".parse().unwrap();
        let content = build_thread_message_content("body", "", root, None);
        let val = serde_json::to_value(&content).unwrap();
        assert_eq!(val["msgtype"], "m.text");
        assert_eq!(val["body"], "body");
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
        // No explicit in-thread reply target: fall back to the root and flag it.
        assert_eq!(val["m.relates_to"]["m.in_reply_to"]["event_id"], "$root:server");
        assert_eq!(val["m.relates_to"]["is_falling_back"], true);
    }

    #[test]
    fn thread_relation_reply_shape() {
        let root: matrix_sdk::ruma::OwnedEventId = "$root:server".parse().unwrap();
        let reply: matrix_sdk::ruma::OwnedEventId = "$reply:server".parse().unwrap();
        let content = build_thread_message_content("body", "", root, Some(reply));
        let val = serde_json::to_value(&content).unwrap();
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
        assert_eq!(
            val["m.relates_to"]["m.in_reply_to"]["event_id"],
            "$reply:server"
        );
        // A real in-thread reply is not a fallback (ruma serializes the
        // default `false` as omission).
        assert!(val["m.relates_to"]["is_falling_back"].is_null());
    }

    #[test]
    fn send_thread_message_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_thread_message("!room:server", "$root:server", "hi", "");
        assert!(!r.ok);
    }

    #[test]
    fn send_thread_reply_not_logged_in() {
        let mut c = ClientFfi::new();
        let r =
            c.send_thread_reply("!room:server", "$root:server", "$reply:server", "hi", "");
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
        let r = c.register_pusher(
            "key",
            "im.gnomos.tesseract",
            "Tesseract",
            "My Device",
            "https://push.example.com/up",
            "en",
        );
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

    #[test]
    fn send_audio_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_audio("!room:server", &[], "audio/ogg", "voice.ogg", "", 0, "", "");
        assert!(!r.ok);
        assert!(r.message.contains("not logged in"), "got: {}", r.message);
    }

    #[test]
    fn send_video_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.send_video(
            "!room:server", &[], "video/mp4", "clip.mp4", "",
            0, 0, &[], 0, 0, 0, "", "",
        );
        assert!(!r.ok);
        assert!(r.message.contains("not logged in"), "got: {}", r.message);
    }

    #[test]
    fn animated_image_content_sets_flags() {
        let val = build_animated_image_content(
            GifMedia::Plain("mxc://server/abc123".into()),
            "anim.gif",
            "", // no caption
            "image/gif",
            320,
            240,
            2048,
            "", // no reply
            "", // no thread
        );
        assert_eq!(val["msgtype"], "m.image");
        assert_eq!(val["body"], "anim.gif");
        assert_eq!(val["url"], "mxc://server/abc123");
        assert_eq!(val["org.matrix.msc4230.is_animated"], true);
        assert_eq!(val["info"]["fi.mau.gif"], true);
        assert_eq!(val["info"]["mimetype"], "image/gif");
        assert!(
            val.get("filename").is_none(),
            "no MSC2530 filename when no caption"
        );
        assert!(val.get("m.relates_to").is_none());
    }

    #[test]
    fn animated_image_content_encrypted_uses_file_block() {
        let enc = serde_json::json!({
            "url": "mxc://server/enc",
            "key": { "kty": "oct" },
            "iv": "iv",
            "hashes": { "sha256": "h" },
            "v": "v2",
        });
        let val = build_animated_image_content(
            GifMedia::Encrypted(enc.clone()),
            "anim.webp",
            "",
            "image/webp",
            320,
            240,
            2048,
            "",
            "",
        );
        // Encrypted rooms carry a `file` block, never a plaintext `url`.
        assert_eq!(val["file"], enc);
        assert!(val.get("url").is_none(), "no plaintext url when encrypted");
        assert_eq!(val["org.matrix.msc4230.is_animated"], true);
        assert_eq!(val["info"]["fi.mau.gif"], true);
        assert_eq!(val["info"]["mimetype"], "image/webp");
    }

    #[test]
    fn animated_image_content_with_caption_and_reply() {
        let val = build_animated_image_content(
            GifMedia::Plain("mxc://server/xyz".into()),
            "cat.gif",
            "Look at this",
            "image/gif",
            0,
            0,
            512,
            "$reply:server",
            "", // no thread
        );
        assert_eq!(val["body"], "Look at this");
        assert_eq!(val["filename"], "cat.gif");
        assert_eq!(val["m.relates_to"]["m.in_reply_to"]["event_id"], "$reply:server");
    }

    #[test]
    fn animated_image_content_threaded() {
        let val = build_animated_image_content(
            GifMedia::Plain("mxc://server/abc".into()), "a.gif", "", "image/gif", 1, 1, 10, "", "$root:server",
        );
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
        assert_eq!(val["m.relates_to"]["m.in_reply_to"]["event_id"], "$root:server");
        assert_eq!(val["m.relates_to"]["is_falling_back"], true);
    }

    #[test]
    fn animated_image_content_threaded_reply() {
        let val = build_animated_image_content(
            GifMedia::Plain("mxc://server/abc".into()), "a.gif", "", "image/gif", 1, 1, 10, "$reply:server",
            "$root:server",
        );
        assert_eq!(val["m.relates_to"]["rel_type"], "m.thread");
        assert_eq!(val["m.relates_to"]["event_id"], "$root:server");
        // Explicit in-thread reply target: point at it, not the root, and no fallback flag.
        assert_eq!(val["m.relates_to"]["m.in_reply_to"]["event_id"], "$reply:server");
        assert!(val["m.relates_to"]["is_falling_back"].is_null());
    }

    #[test]
    fn send_voice_rejects_empty_pcm() {
        let result = encode_voice_ogg(&[], &[], 0);
        assert!(result.is_err(), "empty PCM must be rejected");
    }

    #[test]
    fn send_voice_encodes_valid_ogg() {
        let sample_count = 48000usize;
        let samples: Vec<i16> = (0..sample_count)
            .map(|i| {
                let t = i as f64 / 48000.0;
                (f64::sin(2.0 * std::f64::consts::PI * 440.0 * t) * 16000.0) as i16
            })
            .collect();

        let waveform: Vec<u16> = vec![500; 10];
        let result = encode_voice_ogg(&samples, &waveform, 1000);
        assert!(result.is_ok(), "encoding must succeed: {:?}", result.err());
        let ogg = result.unwrap();
        assert!(ogg.starts_with(b"OggS"), "output must start with OGG magic");
        assert!(ogg.len() > 100, "output must be non-trivial");
    }

    #[test]
    fn send_voice_waveform_clamped_to_256() {
        let samples: Vec<i16> = vec![1000i16; 960];
        let big_waveform: Vec<u16> = vec![500; 512];
        let result = encode_voice_ogg(&samples, &big_waveform, 20);
        assert!(result.is_ok());
    }

    #[test]
    fn timeline_event_has_thread_fields() {
        let ev = crate::ffi::TimelineEvent {
            thread_root_id: "$root:server".to_owned(),
            is_thread_root: true,
            thread_reply_count: 3,
            thread_latest_sender_name: "Alice".to_owned(),
            thread_latest_body: "hi".to_owned(),
            thread_latest_ts: 42,
            ..Default::default()
        };
        assert!(ev.is_thread_root);
        assert_eq!(ev.thread_reply_count, 3);
        assert_eq!(ev.thread_root_id, "$root:server");
        assert_eq!(ev.thread_latest_ts, 42);
    }
}

#[cfg(test)]
mod tests_latest_event_body {
    use super::{latest_event_preview, LatestPreview};
    use matrix_sdk::latest_events::{
        LatestEventValue, LocalLatestEventValue, RemoteLatestEventValue,
    };
    use matrix_sdk::ruma::events::{
        room::message::RoomMessageEventContent, AnyMessageLikeEventContent,
    };
    use matrix_sdk::ruma::{events::AnySyncTimelineEvent, serde::Raw, MilliSecondsSinceUnixEpoch};
    use matrix_sdk::store::SerializableEventContent;

    fn remote(json: serde_json::Value) -> LatestEventValue {
        let raw = Raw::<AnySyncTimelineEvent>::from_json_string(json.to_string()).unwrap();
        LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(raw))
    }
    fn text(t: &str) -> LatestPreview {
        LatestPreview {
            kind: "text".into(),
            text: t.into(),
            sticker_url: String::new(),
            thumbnail_url: String::new(),
        }
    }
    fn media(k: &str) -> LatestPreview {
        LatestPreview {
            kind: k.into(),
            text: String::new(),
            sticker_url: String::new(),
            thumbnail_url: String::new(),
        }
    }

    #[test]
    fn none_returns_default() {
        assert_eq!(
            latest_event_preview(&LatestEventValue::None),
            LatestPreview::default()
        );
    }

    #[test]
    fn remote_invite_returns_default() {
        let v = LatestEventValue::RemoteInvite {
            event_id: None,
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            inviter: None,
        };
        assert_eq!(latest_event_preview(&v), LatestPreview::default());
    }

    #[test]
    fn remote_text_plain() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": { "msgtype": "m.text", "body": "hello world" }
        }));
        assert_eq!(latest_event_preview(&v), text("hello world"));
    }

    #[test]
    fn remote_text_prefers_formatted_first_line() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.text",
                "body": "plain line one\nplain line two",
                "format": "org.matrix.custom.html",
                "formatted_body": "<p><b>bold</b> first</p><p>second para</p>"
            }
        }));
        assert_eq!(latest_event_preview(&v), text("bold first"));
    }

    #[test]
    fn remote_text_html_entities_decoded() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.text", "body": "x",
                "format": "org.matrix.custom.html",
                "formatted_body": "a &amp; b &lt;c&gt;<br>next"
            }
        }));
        assert_eq!(latest_event_preview(&v), text("a & b <c>"));
    }

    #[test]
    fn remote_text_formatted_utf8_preserved() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.text", "body": "x",
                "format": "org.matrix.custom.html",
                "formatted_body": "<p>h\u{00e9}llo \u{1f31f} w\u{00f6}rld</p><p>2</p>"
            }
        }));
        assert_eq!(latest_event_preview(&v), text("héllo 🌟 wörld"));
    }

    #[test]
    fn remote_notice_and_emote_are_text() {
        for mt in ["m.notice", "m.emote"] {
            let v = remote(serde_json::json!({
                "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
                "sender": "@a:e.com", "origin_server_ts": 1,
                "content": { "msgtype": mt, "body": "notice/emote body" }
            }));
            assert_eq!(latest_event_preview(&v), text("notice/emote body"));
        }
    }

    #[test]
    fn remote_image_kind_carries_thumbnail_url() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": { "msgtype": "m.image", "body": "f.png", "url": "mxc://e.com/x" }
        }));
        assert_eq!(
            latest_event_preview(&v),
            LatestPreview {
                kind: "image".into(),
                text: String::new(),
                sticker_url: String::new(),
                thumbnail_url: "mxc://e.com/x".into(),
            }
        );
    }

    #[test]
    fn remote_video_file_audio_kinds() {
        for (mt, kind) in [("m.video", "video"), ("m.file", "file"), ("m.audio", "audio")] {
            let v = remote(serde_json::json!({
                "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
                "sender": "@a:e.com", "origin_server_ts": 1,
                "content": { "msgtype": mt, "body": "f.bin", "url": "mxc://e.com/x" }
            }));
            assert_eq!(latest_event_preview(&v), media(kind));
        }
    }

    #[test]
    fn remote_video_with_fi_mau_gif_returns_gif() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.video", "body": "tenor.mp4",
                "url": "mxc://e.com/x",
                "info": { "mimetype": "video/mp4", "fi.mau.gif": true }
            }
        }));
        assert_eq!(latest_event_preview(&v), media("gif"));
    }

    #[test]
    fn remote_video_without_fi_mau_gif_stays_video() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.video", "body": "clip.mp4",
                "url": "mxc://e.com/x",
                "info": { "mimetype": "video/mp4" }
            }
        }));
        assert_eq!(latest_event_preview(&v), media("video"));
    }

    #[test]
    fn remote_sticker_kind_and_url() {
        let v = remote(serde_json::json!({
            "type": "m.sticker", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "body": "wave",
                "url": "mxc://e.com/stick1",
                "info": { "w": 128, "h": 128, "mimetype": "image/png" }
            }
        }));
        assert_eq!(
            latest_event_preview(&v),
            LatestPreview {
                kind: "sticker".into(),
                text: String::new(),
                sticker_url: "mxc://e.com/stick1".into(),
                thumbnail_url: "mxc://e.com/stick1".into(),
            }
        );
    }

    #[test]
    fn remote_state_event_returns_default() {
        let v = remote(serde_json::json!({
            "type": "m.room.member", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "state_key": "@a:e.com", "origin_server_ts": 1,
            "content": { "membership": "join" }
        }));
        assert_eq!(latest_event_preview(&v), LatestPreview::default());
    }

    #[test]
    fn remote_empty_body_returns_default() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": { "msgtype": "m.text", "body": "   " }
        }));
        assert_eq!(latest_event_preview(&v), LatestPreview::default());
    }

    #[test]
    fn local_is_sending_text() {
        let content = SerializableEventContent::new(&AnyMessageLikeEventContent::RoomMessage(
            RoomMessageEventContent::text_plain("sending\u{2026}"),
        ))
        .unwrap();
        let v = LatestEventValue::LocalIsSending(LocalLatestEventValue {
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            content,
        });
        assert_eq!(latest_event_preview(&v), text("sending\u{2026}"));
    }

    #[test]
    fn local_cannot_be_sent_text() {
        let content = SerializableEventContent::new(&AnyMessageLikeEventContent::RoomMessage(
            RoomMessageEventContent::text_plain("stuck message"),
        ))
        .unwrap();
        let v = LatestEventValue::LocalCannotBeSent(LocalLatestEventValue {
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            content,
        });
        assert_eq!(latest_event_preview(&v), text("stuck message"));
    }

    #[test]
    fn local_has_been_sent_text() {
        use matrix_sdk::ruma::owned_event_id;
        let content = SerializableEventContent::new(&AnyMessageLikeEventContent::RoomMessage(
            RoomMessageEventContent::text_plain("sent!"),
        ))
        .unwrap();
        let v = LatestEventValue::LocalHasBeenSent {
            event_id: owned_event_id!("$ev_sent"),
            value: LocalLatestEventValue {
                timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
                content,
            },
        };
        assert_eq!(latest_event_preview(&v), text("sent!"));
    }
}

#[cfg(test)]
mod location_tests {
    use super::parse_geo_uri;

    #[test]
    fn parse_geo_uri_standard() {
        let r = parse_geo_uri("geo:51.5,-0.1");
        assert_eq!(r, Some((51.5, -0.1)));
    }

    #[test]
    fn parse_geo_uri_with_altitude() {
        let r = parse_geo_uri("geo:51.5,-0.1,0");
        assert_eq!(r, Some((51.5, -0.1)));
    }

    #[test]
    fn parse_geo_uri_with_uncertainty() {
        let r = parse_geo_uri("geo:51.5,-0.1;u=35");
        assert_eq!(r, Some((51.5, -0.1)));
    }

    #[test]
    fn parse_geo_uri_malformed() {
        assert_eq!(parse_geo_uri("not-a-geo-uri"), None);
        assert_eq!(parse_geo_uri("geo:"), None);
        assert_eq!(parse_geo_uri("geo:abc,def"), None);
    }

    #[test]
    fn image_info_thumbnail_source_round_trips_plain_uri() {
        use matrix_sdk::ruma::events::room::message::ImageMessageEventContent;
        let json = serde_json::json!({
            "body": "photo.jpg",
            "msgtype": "m.image",
            "url": "mxc://example.org/full",
            "info": {
                "w": 1920, "h": 1080, "mimetype": "image/jpeg",
                "thumbnail_url": "mxc://example.org/thumb",
                "thumbnail_info": { "w": 320, "h": 200, "mimetype": "image/jpeg" }
            }
        });
        let content: ImageMessageEventContent =
            serde_json::from_value(json).expect("deserialises");
        let thumb_src = content
            .info
            .as_ref()
            .and_then(|info| info.thumbnail_source.as_ref());
        assert!(thumb_src.is_some(), "thumbnail_source must be populated from thumbnail_url");
        match thumb_src.unwrap() {
            matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => {
                assert_eq!(uri.to_string(), "mxc://example.org/thumb");
            }
            _ => panic!("expected Plain MediaSource"),
        }
    }

    #[test]
    fn image_info_without_thumbnail_has_none_thumbnail_source() {
        use matrix_sdk::ruma::events::room::message::ImageMessageEventContent;
        let json = serde_json::json!({
            "body": "photo.jpg",
            "msgtype": "m.image",
            "url": "mxc://example.org/full",
            "info": { "w": 800, "h": 600, "mimetype": "image/png" }
        });
        let content: ImageMessageEventContent =
            serde_json::from_value(json).expect("deserialises");
        let thumb_src = content
            .info
            .as_ref()
            .and_then(|info| info.thumbnail_source.as_ref());
        assert!(thumb_src.is_none(), "no thumbnail → None");
    }
}

#[cfg(test)]
mod server_info_tests {
    use super::*;

    #[test]
    fn get_server_info_no_client_returns_empty() {
        let ffi = ClientFfi::new();
        assert!(ffi.get_server_info().is_empty());
    }
}

#[cfg(test)]
mod uia_fallback_url_tests {
    use super::{build_uia_fallback_url, urlencoding_encode_segment};

    #[test]
    fn builds_spec_compliant_url() {
        let url = build_uia_fallback_url(
            "https://matrix-client.matrix.org",
            "m.login.sso",
            "abc123",
        );
        assert_eq!(
            url,
            "https://matrix-client.matrix.org/_matrix/client/v3/auth/m.login.sso/fallback/web?session=abc123",
        );
    }

    #[test]
    fn trims_trailing_slash_on_homeserver() {
        let url = build_uia_fallback_url(
            "https://matrix.example.org/",
            "m.login.password",
            "s",
        );
        assert!(url.starts_with("https://matrix.example.org/_matrix/client/v3/auth/"));
        assert!(!url.contains("//_matrix"));
    }

    #[test]
    fn percent_encodes_session_special_chars() {
        let url = build_uia_fallback_url(
            "https://h",
            "m.login.password",
            "with space&plus+slash/",
        );
        assert!(url.ends_with("session=with%20space%26plus%2Bslash%2F"));
    }

    #[test]
    fn encode_segment_keeps_unreserved() {
        assert_eq!(urlencoding_encode_segment("AZaz09-_.~"), "AZaz09-_.~");
    }
}

#[cfg(test)]
mod set_presence_tests {
    use super::ClientFfi;

    #[test]
    fn valid_state_bytes_pass_validation() {
        // No client → "not logged in", but the state byte is accepted.
        let mut ffi = ClientFfi::new();
        for byte in [1u8, 2, 3] {
            let r = ffi.set_presence(byte);
            assert!(!r.ok, "no client → must fail");
            assert_eq!(r.message, "not logged in",
                       "byte {byte} should pass validation");
        }
    }

    #[test]
    fn unknown_state_byte_is_rejected() {
        let mut ffi = ClientFfi::new();
        for byte in [0u8, 4, 5, 255] {
            let r = ffi.set_presence(byte);
            assert!(!r.ok);
            assert!(
                r.message.starts_with("invalid presence state"),
                "byte {byte} message was {:?}", r.message
            );
        }
    }
}

#[cfg(test)]
mod thread_timeline_tests {
    use super::ClientFfi;

    #[test]
    fn subscribe_thread_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.subscribe_thread("!room:server", "$root:server");
        assert!(!r.ok);
    }

    #[test]
    fn paginate_thread_back_not_subscribed() {
        let mut c = ClientFfi::new();
        let r = c.paginate_thread_back("!room:server", "$root:server", 20);
        assert!(!r.ok);
    }
}

#[cfg(test)]
mod utd_message_tests {
    use super::utd_message_for_cause;
    use matrix_sdk_base::crypto::types::events::UtdCause;

    #[test]
    fn every_known_cause_has_a_padlock_message() {
        // Every UtdCause variant should map to a non-empty single-line
        // string that starts with the padlock glyph so the row is
        // visually recognisable as a crypto-failure.
        for cause in [
            UtdCause::Unknown,
            UtdCause::SentBeforeWeJoined,
            UtdCause::VerificationViolation,
            UtdCause::UnsignedDevice,
            UtdCause::UnknownDevice,
            UtdCause::HistoricalMessageAndBackupIsDisabled,
            UtdCause::WithheldForUnverifiedOrInsecureDevice,
            UtdCause::WithheldBySender,
            UtdCause::HistoricalMessageAndDeviceIsUnverified,
        ] {
            let m = utd_message_for_cause(cause);
            assert!(!m.is_empty(), "cause {cause:?} had empty message");
            assert!(
                m.starts_with('🔒'),
                "cause {cause:?} message {m:?} should start with padlock"
            );
            assert!(
                !m.contains('\n'),
                "cause {cause:?} message {m:?} must be single line"
            );
        }
    }
}

#[cfg(test)]
mod presence_polling_tests {
    use super::is_presence_forbidden;
    use matrix_sdk::ruma::api::error::ErrorKind;

    #[test]
    fn forbidden_kind_is_detected() {
        assert!(is_presence_forbidden(Some(&ErrorKind::Forbidden)));
    }

    #[test]
    fn other_kinds_are_not_forbidden() {
        for k in [
            ErrorKind::NotFound,
            ErrorKind::Unknown,
            ErrorKind::Unrecognized,
        ] {
            assert!(!is_presence_forbidden(Some(&k)));
        }
    }

    #[test]
    fn none_is_not_forbidden() {
        // Network errors / non-matrix-API errors surface as None — must
        // not be treated as a permanent stop.
        assert!(!is_presence_forbidden(None));
    }
}

#[cfg(test)]
mod thread_list_tests {
    use super::ClientFfi;

    #[test]
    fn subscribe_room_threads_not_logged_in() {
        let mut c = ClientFfi::new();
        let r = c.subscribe_room_threads("!room:server");
        assert!(!r.ok);
    }

    #[test]
    fn list_room_threads_empty_when_not_subscribed() {
        let c = ClientFfi::new();
        assert!(c.list_room_threads("!room:server").is_empty());
    }

    #[test]
    fn thread_info_default_shape() {
        let ti = crate::ffi::ThreadInfo {
            root_event_id: "$root:server".to_owned(),
            num_replies: 4,
            ..Default::default()
        };
        assert_eq!(ti.root_event_id, "$root:server");
        assert_eq!(ti.num_replies, 4);
        assert!(ti.latest_event_id.is_empty());
    }
}
