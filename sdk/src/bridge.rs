pub use super::client::ClientFfi;

pub fn client_create() -> Box<ClientFfi> {
    Box::new(ClientFfi::new())
}

#[cxx::bridge(namespace = "tesseract_ffi")]
pub mod ffi {
    // -------------------------------------------------------------------------
    // Shared plain-data types
    // -------------------------------------------------------------------------

    /// Lightweight room descriptor returned by list_rooms().
    struct RoomInfo {
        id:                String,
        name:              String,
        topic:             String,
        unread_count:      u64,
        is_direct:         bool,
        /// mxc:// URI of the room avatar, empty string if none.
        avatar_url:        String,
        /// Body text of the most recent message (best-effort, may be empty).
        last_message_body: String,
        /// Unix timestamp in milliseconds of the most recent activity, or 0.
        last_activity_ts:  u64,
        /// True when this room's type is "m.space".
        is_space:          bool,
    }

    /// One aggregated reaction key on a `TimelineEvent`.
    /// `count` always equals `senders.len()`.
    struct ReactionGroup {
        /// Unicode emoji string for normal reactions, or `:shortcode:` for
        /// MSC 4027 custom-image reactions.
        key:           String,
        count:         u64,
        /// True when the current user is among the senders for this key.
        reacted_by_me: bool,
        /// JSON-serialised `MediaSource` (compatible with
        /// `fetch_source_bytes`) when this is an MSC 4027 custom-image
        /// reaction. Empty string for plain Unicode reactions.
        source_json:   String,
        /// Display label for each sender, in iteration order from the SDK.
        /// Each entry is the member's display name when resolvable from the
        /// room state, otherwise the bare Matrix ID.
        senders:       Vec<String>,
    }

    /// A single timeline event (message).
    /// Discriminated union: inspect `msg_type` to determine which fields are valid.
    /// For `m.image`   → source_json, width, height are populated.
    ///                   image_filename is non-empty only when the sender supplied
    ///                   an explicit MSC2530 filename; in that case `body` is a caption.
    /// For `m.file`    → file_json, file_name, file_size are populated.
    /// For `m.sticker` → source_json, width, height are populated (same as m.image).
    struct TimelineEvent {
        event_id:          String,
        room_id:           String,
        sender:            String,
        sender_name:       String,
        sender_avatar_url: String,
        body:              String,
        /// Unix timestamp in milliseconds.
        timestamp:         u64,
        /// "m.text" | "m.image" | "m.file" | "m.sticker" | "m.redacted" | …
        /// "m.redacted" → body is empty; render as a tombstone placeholder.
        msg_type:          String,
        /// mxc:// URI of the image (valid when msg_type is "m.image" or "m.sticker").
        source_json:       String,
        width:             u64,
        height:            u64,
        /// JSON serialisation of a `MediaSource` for the file attachment.
        /// Non-empty when msg_type is "m.file".
        file_json:         String,
        file_name:         String,
        file_size:         u64,
        /// Non-empty when msg_type is "m.image" and the sender provided an explicit
        /// MSC2530 `filename` field (distinct from `body`).  When set, `body` is a
        /// user-written caption and should be displayed below the image.
        image_filename:    String,
        /// Aggregated reactions, grouped by key. May be empty.
        reactions:         Vec<ReactionGroup>,
    }

    /// Outcome of an asynchronous SDK operation.
    struct OpResult {
        ok:      bool,
        message: String,
    }

    /// First-phase result of an OAuth flow.
    struct OAuthBegin {
        ok:           bool,
        message:      String,
        auth_url:     String,
        redirect_uri: String,
    }

    /// Snapshot of the server-side key-backup state plus a running counter of
    /// imported room keys for this device. Carried by `on_backup_progress`
    /// callbacks and returned by `backup_state()`.
    ///
    /// `state` is a `u8` because cxx does not support C++-side enums in shared
    /// structs without extra boilerplate; the C++ wrapper translates it to a
    /// typed `tesseract::BackupState` enum.
    ///   0 = Unknown
    ///   1 = Disabled    (no backup on the server)
    ///   2 = Enabled     (steady-state: backup is up to date for this device)
    ///   3 = Downloading (still importing keys from the backup)
    ///   4 = Creating    (uploading initial backup)
    struct BackupProgress {
        state:         u8,
        /// Room keys imported into the local store since recover() started,
        /// or 0 when no recover is in progress.
        imported_keys: u64,
        /// Best-effort total of room keys present on the server-side backup,
        /// or 0 when unknown.
        total_keys:    u64,
    }

    // -------------------------------------------------------------------------
    // C++ types that Rust calls back into
    // -------------------------------------------------------------------------
    unsafe extern "C++" {
        include!("tesseract/event_handler_bridge.h");

        type EventHandlerBridge;

        fn on_message_event(self: &EventHandlerBridge, event: &TimelineEvent);
        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str, soft_logout: bool);
        fn on_session_refreshed(self: &EventHandlerBridge, session_json: &str);
        /// Fired when a room's timeline is reset (room selected / subscribed).
        /// The UI should clear its message view for this room_id.
        fn on_timeline_reset(self: &EventHandlerBridge, room_id: &str);
        /// Fired when the key-backup state changes or when imported-key
        /// counters advance during a recover() call.
        fn on_backup_progress(self: &EventHandlerBridge, progress: &BackupProgress);
    }

    // -------------------------------------------------------------------------
    // Rust-owned opaque type exposed to C++
    // -------------------------------------------------------------------------
    extern "Rust" {
        type ClientFfi;

        fn client_create() -> Box<ClientFfi>;

        // ----- OAuth -----

        fn oauth_begin(self: &mut ClientFfi, homeserver: &str) -> OAuthBegin;
        fn oauth_await_callback(self: &mut ClientFfi) -> OpResult;
        fn oauth_cancel(self: &mut ClientFfi);

        // ----- Session -----

        fn restore_session(self: &mut ClientFfi, session_json: &str) -> OpResult;
        fn export_session(self: &ClientFfi) -> String;

        // ----- Sync -----

        fn start_sync(self: &mut ClientFfi, handler: UniquePtr<EventHandlerBridge>);
        fn stop_sync(self: &mut ClientFfi);

        // ----- Room list -----

        fn list_rooms(self: &ClientFfi) -> Vec<RoomInfo>;

        // ----- Timeline subscription (Step 2) -----

        /// Subscribe to a room's timeline. Emits on_timeline_reset then
        /// on_message_event for each cached item, and on_message_event for
        /// every subsequent live update. Call paginate_back to load history.
        fn subscribe_room(self: &mut ClientFfi, room_id: &str) -> OpResult;

        /// Unsubscribe from a room's timeline and cancel its background task.
        fn unsubscribe_room(self: &mut ClientFfi, room_id: &str);

        /// Paginate backwards in a subscribed room's timeline. New items
        /// arrive via on_message_event callbacks in oldest-first order.
        fn paginate_back(self: &mut ClientFfi, room_id: &str, count: u16) -> OpResult;

        /// Kick off a background pass that paginates every joined room not
        /// currently subscribed, up to ~50 events each, with bounded
        /// concurrency. Idempotent — safe to call from every room-open
        /// path. Silent: emits no `on_message_event` / `on_timeline_reset`
        /// callbacks for the rooms it visits. The persistent SDK event
        /// cache is what gets warmed.
        fn start_background_backfill(self: &mut ClientFfi) -> OpResult;

        /// Cancel an in-progress background backfill. No-op if none is
        /// running. Also called automatically from `stop_sync` and `Drop`.
        fn stop_background_backfill(self: &mut ClientFfi);

        // ----- Messaging -----

        fn send_message(self: &mut ClientFfi, room_id: &str, body: &str) -> OpResult;

        /// Toggle the current user's `key` reaction on `event_id` in
        /// `room_id`. Adds the reaction when the user has not yet reacted
        /// with this key; redacts it when they have. Wraps matrix-sdk-ui's
        /// `Timeline::toggle_reaction`. Requires that `room_id` is
        /// currently subscribed via `subscribe_room`.
        fn send_reaction(self: &mut ClientFfi,
                         room_id: &str,
                         event_id: &str,
                         key: &str) -> OpResult;

        /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
        /// Wraps matrix-sdk-ui's `Timeline::redact`. Requires that the room
        /// is currently subscribed via `subscribe_room`. Server-side
        /// permission errors surface as `OpResult { ok: false, message: ... }`.
        fn redact_event(self: &mut ClientFfi,
                        room_id: &str,
                        event_id: &str,
                        reason: &str) -> OpResult;

        // ----- Identity -----

        /// Returns the current user's Matrix ID (e.g. @alice:example.org), or
        /// an empty string if not logged in.
        fn user_id(self: &ClientFfi) -> String;

        /// Returns the current user's display name, or an empty string when
        /// none is set / not logged in / the network fetch fails. Cached by
        /// matrix-sdk after the first call.
        fn current_user_display_name(self: &ClientFfi) -> String;

        /// Returns the mxc:// URI of the current user's avatar, or an empty
        /// string when none is set / not logged in / the network fetch fails.
        /// Cached by matrix-sdk after the first call.
        fn current_user_avatar_url(self: &ClientFfi) -> String;

        // ----- Media -----

        /// Download the avatar image for a room and return the raw bytes
        /// (typically JPEG or PNG). Returns an empty Vec when no avatar is set
        /// or the download fails. The SDK media cache is consulted first so
        /// subsequent calls are instant.
        fn fetch_avatar_bytes(self: &mut ClientFfi, room_id: &str) -> Vec<u8>;

        /// Download arbitrary mxc:// media and return the raw bytes. Returns
        /// an empty Vec when the URL is invalid, media is unavailable, or the
        /// client is not logged in. The SDK media cache is consulted first so
        /// repeat calls for the same URL are instant.
        fn fetch_media_bytes(self: &mut ClientFfi, mxc_url: &str) -> Vec<u8>;

        /// Download media from a JSON-serialised `MediaSource` (plain mxc:// or
        /// encrypted `EncryptedFile`). Returns raw bytes on success or an
        /// empty Vec on failure (invalid JSON, decrypt error, network, etc.).
        fn fetch_source_bytes(self: &mut ClientFfi, source_json: &str) -> Vec<u8>;

        // ----- Spaces -----

        /// Returns the room IDs of all direct children declared by a space
        /// (via `m.space.child` state events). Only returns IDs of rooms the
        /// client is a member of (i.e. present in the room list).
        fn space_children(self: &ClientFfi, space_id: &str) -> Vec<String>;

        // ----- Recovery / key backup (Step 6) -----

        /// Returns true when this device is missing the cross-signing /
        /// backup secrets that already exist on the server. The UI should
        /// surface a "Verify this device" banner when this is true.
        fn needs_recovery(self: &ClientFfi) -> bool;

        /// Unlock the server-side secret storage with a recovery key or
        /// passphrase, import the cross-signing private keys + backup
        /// decryption key into this device, and start downloading historical
        /// room keys. Returns once the SDK reports a steady-state backup, or
        /// with an error if the key is wrong / no secret storage exists.
        fn recover(self: &mut ClientFfi, key_or_passphrase: &str) -> OpResult;

        /// Current snapshot of the backup state and import counters.
        fn backup_state(self: &ClientFfi) -> BackupProgress;

        // ----- Session teardown -----

        fn logout(self: &mut ClientFfi) -> OpResult;
    }
}
