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

    /// One user's most recent read receipt landing on a timeline event.
    /// `display_name` is the room member's resolved name (falls back to the
    /// bare Matrix ID); `avatar_url` is the member's mxc:// URI (empty when
    /// unset). Receipts for the current user are filtered on the Rust side
    /// so the UI never has to render its own avatar on every message.
    struct ReadReceipt {
        user_id:      String,
        display_name: String,
        avatar_url:   String,
    }

    /// A single timeline event (message).
    /// Discriminated union: inspect `msg_type` to determine which fields are valid.
    /// For `m.image`   → source_json, width, height are populated.
    ///                   image_filename is non-empty only when the sender supplied
    ///                   an explicit MSC2530 filename; in that case `body` is a caption.
    /// For `m.file`    → file_json, file_name, file_size are populated.
    /// For `m.sticker` → source_json, width, height are populated (same as m.image).
    /// For `m.voice`   → audio_source_json, audio_duration_ms, audio_waveform,
    ///                   audio_mime are populated (MSC3245 voice messages).
    ///                   Non-voice `m.audio` events are converted to "m.file"
    ///                   on the Rust side so the file-card path renders them.
    /// Reply fields   → in_reply_to_id is non-empty when this event is a reply.
    ///                   in_reply_to_sender_name and in_reply_to_body carry the
    ///                   replied-to event's display name and body snippet (populated
    ///                   only when the replied-to item is present in the local cache;
    ///                   both are empty strings when the cache doesn't have it yet).
    struct TimelineEvent {
        event_id:          String,
        room_id:           String,
        sender:            String,
        sender_name:       String,
        sender_avatar_url: String,
        body:              String,
        /// Unix timestamp in milliseconds.
        timestamp:         u64,
        /// "m.text" | "m.image" | "m.file" | "m.sticker" | "m.voice" | "m.redacted" | …
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
        /// JSON-serialised `MediaSource` for the voice clip (plain mxc:// or
        /// encrypted `EncryptedFile`). Non-empty when `msg_type == "m.voice"`.
        audio_source_json: String,
        /// Duration of the voice clip in milliseconds (MSC1767 `audio.duration`,
        /// falling back to `info.duration`). 0 when neither is provided.
        audio_duration_ms: u64,
        /// MSC1767 waveform samples, each clamped to 0..=1024. Empty when the
        /// sender did not include one (the UI renders flat placeholder bars).
        audio_waveform:    Vec<u16>,
        /// MIME type advertised by the sender (typically "audio/ogg"). May be
        /// empty when missing from `info.mimetype`.
        audio_mime:        String,
        /// Aggregated reactions, grouped by key. May be empty.
        reactions:         Vec<ReactionGroup>,
        /// Users (other than the current user) whose latest read receipt
        /// landed on this event. Order matches the SDK's iteration order.
        read_receipts:     Vec<ReadReceipt>,
        /// Event ID of the message being replied to. Empty when this is not a reply.
        in_reply_to_id:          String,
        /// Display name of the replied-to sender (room profile, or bare Matrix ID
        /// as fallback). Empty when not a reply or the event isn't cached yet.
        in_reply_to_sender_name: String,
        /// Short body snippet of the replied-to message. For non-text types this is
        /// "(image)", "(file)", "(voice)", "(sticker)", or "(deleted)" for redacted.
        /// Empty when not a reply.
        in_reply_to_body:        String,
        /// True when the body has been superseded by an `m.replace` edit.
        /// Only set for `msg_type == "m.text"`; always false for other types.
        is_edited:               bool,
    }

    /// Outcome of an asynchronous SDK operation.
    struct OpResult {
        ok:      bool,
        message: String,
    }

    /// Outcome of a back-pagination call. `reached_start` is `true` when
    /// matrix-sdk-ui reports the timeline has no further history to load
    /// (the room's first event has been seen). UIs use this to stop firing
    /// the "near top" pagination trigger.
    struct PaginateResult {
        ok:            bool,
        message:       String,
        reached_start: bool,
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

    /// One MSC2545 image pack surfaced to C++. Three sources:
    /// `source_kind == "user"` is the per-account pack stored in account_data;
    /// `source_kind == "room"` is a state event in `source_room` at the given
    /// `source_state_key`. `usage_mask` is a bitset where 1 = sticker and
    /// 2 = emoticon; per-image usage may override it. `attribution` is
    /// optional metadata from the pack author.
    struct ImagePackFfi {
        id:                String,
        display_name:      String,
        avatar_url:        String,
        attribution:       String,
        usage_mask:        u8,
        source_kind:       String,
        source_room:       String,
        source_state_key:  String,
    }

    /// One image entry inside a pack. `usage_mask` is the per-image usage
    /// after inheriting from the pack when not set on the image. `info_json`
    /// is the literal `info` object serialised as JSON (`"{}"` when absent).
    struct ImageEntryFfi {
        pack_id:    String,
        shortcode:  String,
        url:        String,
        body:       String,
        info_json:  String,
        usage_mask: u8,
        favorite:   bool,
    }

    // -------------------------------------------------------------------------
    // C++ types that Rust calls back into
    // -------------------------------------------------------------------------
    //
    // The four timeline callbacks below mirror matrix-sdk-ui's
    // `VectorDiff<TimelineItem>` semantics so that the C++ message vector is
    // a faithful, index-aligned mirror of the visible (non-virtual) prefix
    // of the SDK's timeline. Every operation carries the *visible* index
    // (virtual items such as day-dividers are filtered out on the Rust
    // side; the Rust dispatcher maintains a `Vec<bool>` visibility mirror
    // to translate raw indices into visible ones).
    unsafe extern "C++" {
        include!("tesseract/event_handler_bridge.h");

        type EventHandlerBridge;

        /// Atomically reset a room's timeline to `snapshot` (oldest-first).
        /// The callee clears its model for `room_id` and rebuilds it from
        /// the snapshot in a single update.
        fn on_timeline_reset(self: &EventHandlerBridge,
                              room_id: &str,
                              snapshot: &Vec<TimelineEvent>);
        /// Insert `event` at visible-index `index` in `room_id`'s
        /// timeline. `index == current_length` means "append at the end".
        fn on_message_inserted(self: &EventHandlerBridge,
                                room_id: &str,
                                index: u64,
                                event: &TimelineEvent);
        /// Replace the event currently at visible-index `index` with
        /// `event` (edit, redaction, reaction change, sender-profile
        /// resolution).
        fn on_message_updated(self: &EventHandlerBridge,
                               room_id: &str,
                               index: u64,
                               event: &TimelineEvent);
        /// Remove the event at visible-index `index`.
        fn on_message_removed(self: &EventHandlerBridge,
                               room_id: &str,
                               index: u64);

        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str, soft_logout: bool);
        fn on_session_refreshed(self: &EventHandlerBridge, session_json: &str);
        /// Fired when the key-backup state changes or when imported-key
        /// counters advance during a recover() call.
        fn on_backup_progress(self: &EventHandlerBridge, progress: &BackupProgress);
        /// Fired when the cached set of MSC2545 image packs changes
        /// (user-pack edit, room-pack subscription edit, or live state-event
        /// update on a referenced room). The UI re-queries via
        /// `list_image_packs` / `list_pack_images`.
        fn on_image_packs_updated(self: &EventHandlerBridge);
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

        /// Subscribe to a room's timeline. Fires an immediate empty
        /// `on_timeline_reset` so the UI clears prior content, then a
        /// follow-up `on_timeline_reset` carrying the initial cached
        /// snapshot once it has been converted. Subsequent live changes
        /// arrive as positional `on_message_inserted` /
        /// `on_message_updated` / `on_message_removed` callbacks. Call
        /// `paginate_back` to load older history.
        fn subscribe_room(self: &mut ClientFfi, room_id: &str) -> OpResult;

        /// Unsubscribe from a room's timeline and cancel its background task.
        fn unsubscribe_room(self: &mut ClientFfi, room_id: &str);

        /// Paginate backwards in a subscribed room's timeline. Older items
        /// arrive as `on_message_inserted` callbacks at the front of the
        /// timeline (index 0, or wherever the cache grafts them in).
        fn paginate_back(self: &mut ClientFfi, room_id: &str, count: u16) -> OpResult;

        /// Like `paginate_back` but also reports whether the timeline has
        /// reached its first event (no further pagination possible). UIs use
        /// `reached_start` to latch their scroll-up trigger off.
        fn paginate_back_with_status(self: &mut ClientFfi,
                                     room_id: &str,
                                     count: u16) -> PaginateResult;

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

        /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds the
        /// `m.in_reply_to` relation and sends via `room.send()`. Does not require
        /// `subscribe_room`. Does not add the plain-text fallback body (Tesseract
        /// renders its own quote block for the reply indicator).
        fn send_reply(self: &mut ClientFfi,
                      room_id:  &str,
                      event_id: &str,
                      body:     &str) -> OpResult;

        /// Send an image to `room_id`. `bytes` are the already-encoded image
        /// payload (PNG/JPEG/etc. as identified by `mime_type`); the SDK
        /// uploads them via the homeserver's media repository and posts an
        /// `m.image` event. `filename` is the user-visible file name (e.g.
        /// `clipboard-20260512-141503.png`). When `caption` is non-empty the
        /// event follows MSC2530 framing: `body` is set to the caption and
        /// the dedicated `filename` field carries the file name. When
        /// `caption` is empty, `body` is set to the filename and the
        /// MSC2530 `filename` field is omitted (legacy fallback). `width`
        /// and `height` populate `info.{w,h}`; pass 0 when unknown.
        fn send_image(self: &mut ClientFfi,
                      room_id: &str,
                      bytes: &[u8],
                      mime_type: &str,
                      filename: &str,
                      caption: &str,
                      width: u32,
                      height: u32,
                      /// When non-empty, adds an `m.in_reply_to` relation.
                      reply_event_id: &str) -> OpResult;

        /// Send an arbitrary file to `room_id` as an `m.file` event. `bytes`
        /// are the raw file payload (no client-side re-encoding); `mime_type`
        /// is best-effort guessed by the caller from extension / OS metadata
        /// (`application/octet-stream` is acceptable when unknown).
        /// `filename` is the user-visible file name. When `caption` is
        /// non-empty the event follows MSC2530 framing: `body` carries the
        /// caption and the dedicated `filename` field carries the file name.
        /// matrix-sdk handles plain and E2EE rooms transparently.
        fn send_file(self: &mut ClientFfi,
                     room_id: &str,
                     bytes: &[u8],
                     mime_type: &str,
                     filename: &str,
                     caption: &str,
                     /// When non-empty, adds an `m.in_reply_to` relation.
                     reply_event_id: &str) -> OpResult;

        /// Edit `event_id` in `room_id` replacing its body with `new_body`.
        /// Uses `Room::make_edit_event` + `RoomSendQueue::send` so the edit
        /// is correctly formatted (Replacement relation + fallback body). Only
        /// works on own `m.text` events; returns `ok=false` for non-own or
        /// non-text events.
        fn send_edit(self: &mut ClientFfi,
                     room_id:   &str,
                     event_id:  &str,
                     new_body:  &str) -> OpResult;

        /// Homeserver-reported maximum upload size in bytes
        /// (`/_matrix/media/v3/config`). Cached after the first successful
        /// query; returns `0` when the server does not advertise a limit, the
        /// query has not yet completed, or the client is not logged in.
        fn media_upload_limit(self: &mut ClientFfi) -> u64;

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

        // ----- MSC2545 image packs (Step 8) -----

        /// Snapshot of every MSC2545 image pack the client currently knows
        /// about. Aggregated from the user's `im.ponies.user_emotes` /
        /// `m.image_pack` account-data, plus every room pack referenced from
        /// the user's `im.ponies.emote_rooms` / `m.image_pack.rooms`. Reads
        /// the local cache only — no network roundtrip. The cache is rebuilt
        /// when sync delivers a relevant event and `on_image_packs_updated`
        /// fires; subscribe to that callback before calling this if the UI
        /// is open before the first sync settles.
        fn list_image_packs(self: &ClientFfi) -> Vec<ImagePackFfi>;

        /// Return every image entry in `pack_id` whose usage mask matches
        /// `usage_filter` ("sticker" | "emoticon" | "any"). Order is
        /// well-defined and stable for a given pack snapshot.
        fn list_pack_images(
            self: &ClientFfi,
            pack_id: &str,
            usage_filter: &str,
        ) -> Vec<ImageEntryFfi>;

        /// Return every image flagged as a favourite by the current user
        /// (across all packs). Backs the StickerPicker "Favorites" tab.
        fn list_favorite_stickers(self: &ClientFfi) -> Vec<ImageEntryFfi>;

        /// Send `m.sticker` to `room_id`. `body` is the description shown by
        /// fallback clients; `image_url` is the `mxc://` source; `info_json`
        /// is the literal MSC2545 `info` object (`"{}"` is acceptable).
        /// matrix-sdk handles E2EE rooms transparently.
        fn send_sticker(
            self: &mut ClientFfi,
            room_id: &str,
            body: &str,
            image_url: &str,
            info_json: &str,
        ) -> OpResult;

        /// Add a sticker to the user's MSC2545 personal pack
        /// (`im.ponies.user_emotes`), creating the pack on first use with
        /// display_name "Saved Stickers". When the suggested `shortcode`
        /// collides with an existing entry a numeric suffix is appended.
        /// GET-modify-PUT against the homeserver; on success the local cache
        /// updates on the next sync settle and `on_image_packs_updated`
        /// fires.
        fn save_sticker_to_user_pack(
            self: &mut ClientFfi,
            shortcode: &str,
            body: &str,
            image_url: &str,
            info_json: &str,
        ) -> OpResult;

        /// True when `image_url` is already present in the user's personal
        /// pack. Used by the right-click context menu to hide the "Add to
        /// Saved Stickers" item for stickers the user has already saved.
        fn user_pack_has_sticker(self: &ClientFfi, image_url: &str) -> bool;

        /// Flip the `im.tesseract.favorite` flag on the user-pack entry
        /// whose `url` matches `image_url`. No-op when the sticker isn't in
        /// the user pack (call `save_sticker_to_user_pack` first).
        fn toggle_favorite_sticker(
            self: &mut ClientFfi,
            image_url: &str,
        ) -> OpResult;

        // ----- Recent emoji (io.element.recent_emoji global account-data) -----

        /// Top-N glyphs from the user's `io.element.recent_emoji`
        /// account-data, most-used first. Reads the SDK's local sync cache;
        /// no network roundtrip. Returns an empty vector when not logged in
        /// or before the first sync has populated the cache.
        fn recent_emoji_top(self: &mut ClientFfi, n: u32) -> Vec<String>;

        /// Record one use of `glyph`. Fire-and-forget against the homeserver
        /// (the SDK call is GET-modify-PUT and would otherwise stall the UI).
        fn recent_emoji_bump(self: &mut ClientFfi, glyph: &str);

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
