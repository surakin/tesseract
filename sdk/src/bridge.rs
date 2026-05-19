pub use super::client::ClientFfi;

pub fn client_create() -> Box<ClientFfi> {
    Box::new(ClientFfi::new())
}

pub fn compute_waveform_from_ogg(bytes: &[u8]) -> Vec<u16> {
    super::waveform::compute_waveform_from_ogg(bytes)
}

pub fn init_waveform_store(path: &str) {
    super::waveform_store::init(std::path::Path::new(path))
}

pub fn load_voice_waveform(mxc_uri: &str) -> Vec<u16> {
    super::waveform_store::load(mxc_uri)
}

pub fn store_voice_waveform(mxc_uri: &str, waveform: &[u16]) {
    super::waveform_store::store(mxc_uri, waveform)
}

pub fn evict_voice_waveform(mxc_uri: &str) {
    super::waveform_store::evict(mxc_uri)
}

#[cxx::bridge(namespace = "tesseract_ffi")]
pub mod ffi {
    // -------------------------------------------------------------------------
    // Shared plain-data types
    // -------------------------------------------------------------------------

    /// Lightweight room descriptor returned by list_rooms().
    struct RoomInfo {
        id: String,
        name: String,
        topic: String,
        /// HTML body from the MSC3765 `m.topic` block; empty when absent.
        topic_html: String,
        unread_count: u64,
        is_direct: bool,
        /// mxc:// URI of the room avatar, empty string if none.
        avatar_url: String,
        /// Body text of the most recent message (best-effort, may be empty).
        last_message_body: String,
        /// Display name of the last-message sender; empty when the sender is
        /// the current user (render as "You"), or when there is no last message.
        last_message_sender_name: String,
        /// Kind of the latest event for preview rendering:
        /// "text" | "image" | "video" | "file" | "audio" | "sticker" | "".
        last_message_kind: String,
        /// mxc:// URI of the sticker image when last_message_kind == "sticker";
        /// empty otherwise.
        last_message_sticker_url: String,
        /// mxc:// URI used for the room-list thumbnail chip:
        /// populated for "image" (the image mxc) and "sticker" (same as sticker_url);
        /// empty for all other kinds.
        last_message_thumbnail_url: String,
        /// Unix timestamp in milliseconds of the most recent activity, or 0.
        last_activity_ts: u64,
        /// True when this room's type is "m.space".
        is_space: bool,
        /// True when the room is tagged `m.favourite` by the current user.
        is_favorite: bool,
    }

    /// One aggregated reaction key on a `TimelineEvent`.
    /// `count` always equals `senders.len()`.
    struct ReactionGroup {
        /// Unicode emoji string for normal reactions, or `:shortcode:` for
        /// MSC 4027 custom-image reactions.
        key: String,
        count: u64,
        /// True when the current user is among the senders for this key.
        reacted_by_me: bool,
        /// JSON-serialised `MediaSource` (compatible with
        /// `fetch_source_bytes`) when this is an MSC 4027 custom-image
        /// reaction. Empty string for plain Unicode reactions.
        source_json: String,
        /// Display label for each sender, in iteration order from the SDK.
        /// Each entry is the member's display name when resolvable from the
        /// room state, otherwise the bare Matrix ID.
        senders: Vec<String>,
    }

    /// One user's most recent read receipt landing on a timeline event.
    /// `display_name` is the room member's resolved name (falls back to the
    /// bare Matrix ID); `avatar_url` is the member's mxc:// URI (empty when
    /// unset). Receipts for the current user are filtered on the Rust side
    /// so the UI never has to render its own avatar on every message.
    struct ReadReceipt {
        user_id: String,
        display_name: String,
        avatar_url: String,
    }

    /// A single timeline event (message).
    /// Discriminated union: inspect `msg_type` to determine which fields are valid.
    /// For `m.image`   → source_json, width, height are populated.
    ///                   image_filename is non-empty only when the sender supplied
    ///                   an explicit MSC2530 filename; in that case `body` is a caption.
    ///                   image_animated is true when MSC4230 is_animated flag is set.
    /// For `m.file`    → file_json, file_name, file_size are populated.
    /// For `m.sticker` → source_json, width, height are populated (same as m.image).
    ///                   image_animated is true when MSC4230 is_animated flag is set.
    /// For `m.voice`   → audio_source_json, audio_duration_ms, audio_waveform,
    ///                   audio_mime are populated (MSC3245 voice messages).
    ///                   Non-voice `m.audio` events are converted to "m.file"
    ///                   on the Rust side so the file-card path renders them.
    /// For `m.video`   → source_json (video MediaSource), width, height,
    ///                   image_filename (MSC2530 caption filename),
    ///                   video_thumbnail_json, video_duration_ms, video_mime.
    /// Reply fields   → in_reply_to_id is non-empty when this event is a reply.
    ///                   in_reply_to_sender_name and in_reply_to_body carry the
    ///                   replied-to event's display name and body snippet (populated
    ///                   only when the replied-to item is present in the local cache;
    ///                   both are empty strings when the cache doesn't have it yet).
    struct TimelineEvent {
        event_id: String,
        room_id: String,
        sender: String,
        sender_name: String,
        sender_avatar_url: String,
        body: String,
        /// Unix timestamp in milliseconds.
        timestamp: u64,
        /// "m.text" | "m.image" | "m.file" | "m.sticker" | "m.voice" | "m.redacted" | …
        /// "m.redacted" → body is empty; render as a tombstone placeholder.
        /// Virtual items passed through from matrix-sdk-ui:
        ///   "virtual.date_divider"  → timestamp = day epoch in ms (local midnight).
        ///   "virtual.read_marker"   → marks the user's last-read position.
        ///   "virtual.timeline_start"→ no earlier history to paginate.
        msg_type: String,
        /// mxc:// URI of the image (valid when msg_type is "m.image" or "m.sticker").
        source_json: String,
        width: u64,
        height: u64,
        /// JSON serialisation of a `MediaSource` for the file attachment.
        /// Non-empty when msg_type is "m.file".
        file_json: String,
        file_name: String,
        file_size: u64,
        /// Non-empty when msg_type is "m.image" and the sender provided an explicit
        /// MSC2530 `filename` field (distinct from `body`).  When set, `body` is a
        /// user-written caption and should be displayed below the image.
        image_filename: String,
        /// JSON-serialised `MediaSource` for the voice clip (plain mxc:// or
        /// encrypted `EncryptedFile`). Non-empty when `msg_type == "m.voice"`.
        audio_source_json: String,
        /// Duration of the voice clip in milliseconds (MSC1767 `audio.duration`,
        /// falling back to `info.duration`). 0 when neither is provided.
        audio_duration_ms: u64,
        /// MSC1767 waveform samples, each clamped to 0..=1024. Empty when the
        /// sender did not include one (the UI renders flat placeholder bars).
        audio_waveform: Vec<u16>,
        /// MIME type advertised by the sender (typically "audio/ogg"). May be
        /// empty when missing from `info.mimetype`.
        audio_mime: String,
        /// Thumbnail MediaSource JSON for `m.video` events (plain mxc:// or
        /// encrypted JSON). Empty when the server omits a thumbnail.
        video_thumbnail_json: String,
        /// Thumbnail MediaSource JSON for `m.image` (and sticker) events (plain
        /// mxc:// or encrypted JSON). Empty when the server omits a thumbnail.
        image_thumbnail_json: String,
        /// Duration of the video in milliseconds. 0 when not provided.
        video_duration_ms: u64,
        /// MIME type of the video (e.g. "video/mp4"). May be empty.
        video_mime: String,
        /// fi.mau.* vendor hints from content.info. All false when absent.
        /// autoplay: start playback immediately on load.
        /// loop: restart the clip at end-of-stream.
        /// no_audio: mute the audio track.
        /// hide_controls: suppress the player controls bar.
        /// gif: composite marker — implies all four above.
        video_autoplay: bool,
        video_loop: bool,
        video_no_audio: bool,
        video_hide_controls: bool,
        video_gif: bool,
        /// Aggregated reactions, grouped by key. May be empty.
        reactions: Vec<ReactionGroup>,
        /// Users (other than the current user) whose latest read receipt
        /// landed on this event. Order matches the SDK's iteration order.
        read_receipts: Vec<ReadReceipt>,
        /// Event ID of the message being replied to. Empty when this is not a reply.
        in_reply_to_id: String,
        /// Display name of the replied-to sender (room profile, or bare Matrix ID
        /// as fallback). Empty when not a reply or the event isn't cached yet.
        in_reply_to_sender_name: String,
        /// Short body snippet of the replied-to message. For non-text types this is
        /// "(image)", "(file)", "(voice)", "(sticker)", or "(deleted)" for redacted.
        /// Empty when not a reply.
        in_reply_to_body: String,
        /// True when the body has been superseded by an `m.replace` edit.
        /// Only set for `msg_type == "m.text"`; always false for other types.
        is_edited: bool,
        /// HTML body from `formatted_body` when `format == "org.matrix.custom.html"`.
        /// Empty string for all non-text message types and plain-text messages.
        formatted_body: String,
        /// BlurHash placeholder string for this media item (MSC2448).
        /// Empty when absent. Decoded by the UI layer before the real
        /// bytes arrive; the unstable field name is xyz.amorgan.blurhash.
        blurhash: String,
        /// JSON-serialised `ImageInfo` for `m.sticker` events (width, height,
        /// mimetype, size, etc. as sent by the sender). Empty string for all
        /// other message types. Lets right-click handlers pass real metadata
        /// to `save_sticker_to_user_pack` instead of `"{}"`.
        sticker_info_json: String,
        /// True when the sender set MSC4230 `org.matrix.msc4230.is_animated` to true
        /// in the event `info` block. Valid for `m.image` and `m.sticker`; always
        /// false for all other message types.
        image_animated: bool,
        /// Local-echo send state: "sending" | "failed" | "" (server event).
        pending_state: String,
        /// Human-readable error message when `pending_state == "failed"`.
        pending_error: String,
        /// True when the failure is recoverable (retry re-enables the queue).
        pending_recoverable: bool,
        /// Transaction ID of the pending local echo (for abort_send).
        pending_txn_id: String,
        // m.location / MSC3488 (valid when msg_type == "m.location")
        location_lat: f64,
        location_lon: f64,
        location_description: String,
    }

    /// Outcome of an asynchronous SDK operation.
    struct OpResult {
        ok: bool,
        message: String,
    }

    /// Outcome of a back-pagination or forward-pagination call.
    /// `reached_start` is `true` when matrix-sdk-ui reports the timeline has
    /// no further history to load (the room's first event has been seen). UIs
    /// use this to stop firing the "near top" pagination trigger.
    /// `reached_end` is `true` when forward-pagination signals that the live
    /// end of the timeline has been reached (only meaningful for focused
    /// timelines; always `false` for live timelines). UIs use this to switch
    /// back to live mode after the user scrolls past the focus point.
    struct PaginateResult {
        ok: bool,
        message: String,
        reached_start: bool,
        reached_end: bool,
    }

    /// First-phase result of an OAuth flow.
    struct OAuthBegin {
        ok: bool,
        message: String,
        auth_url: String,
        redirect_uri: String,
    }

    /// One of the 7 emoji displayed during an SAS device-verification flow.
    /// `symbol` is the Unicode emoji string (one or more codepoints);
    /// `description` is the English label from the Matrix spec table (e.g. "Dog").
    /// The UI renders both side-by-side for each of the 7 tiles so the user
    /// can compare them with the other device's display.
    struct VerificationEmoji {
        symbol: String,
        description: String,
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
        state: u8,
        /// Room keys imported into the local store since recover() started,
        /// or 0 when no recover is in progress.
        imported_keys: u64,
        /// Best-effort total of room keys present on the server-side backup,
        /// or 0 when unknown.
        total_keys: u64,
    }

    /// One MSC2545 image pack surfaced to C++. Three sources:
    /// `source_kind == "user"` is the per-account pack stored in account_data;
    /// `source_kind == "room"` is a state event in `source_room` at the given
    /// `source_state_key`. `usage_mask` is a bitset where 1 = sticker and
    /// 2 = emoticon; per-image usage may override it. `attribution` is
    /// optional metadata from the pack author.
    struct ImagePackFfi {
        id: String,
        display_name: String,
        avatar_url: String,
        attribution: String,
        usage_mask: u8,
        source_kind: String,
        source_room: String,
        source_state_key: String,
    }

    /// One image entry inside a pack. `usage_mask` is the per-image usage
    /// after inheriting from the pack when not set on the image. `info_json`
    /// is the literal `info` object serialised as JSON (`"{}"` when absent).
    struct ImageEntryFfi {
        pack_id: String,
        shortcode: String,
        url: String,
        body: String,
        info_json: String,
        usage_mask: u8,
        favorite: bool,
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
        fn on_timeline_reset(
            self: &EventHandlerBridge,
            room_id: &str,
            snapshot: &Vec<TimelineEvent>,
        );
        /// Insert `event` at visible-index `index` in `room_id`'s
        /// timeline. `index == current_length` means "append at the end".
        fn on_message_inserted(
            self: &EventHandlerBridge,
            room_id: &str,
            index: u64,
            event: &TimelineEvent,
        );
        /// Replace the event currently at visible-index `index` with
        /// `event` (edit, redaction, reaction change, sender-profile
        /// resolution).
        fn on_message_updated(
            self: &EventHandlerBridge,
            room_id: &str,
            index: u64,
            event: &TimelineEvent,
        );
        /// Remove the event at visible-index `index`.
        fn on_message_removed(self: &EventHandlerBridge, room_id: &str, index: u64);

        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str, soft_logout: bool);
        fn on_session_refreshed(self: &EventHandlerBridge, session_json: &str);
        /// Fired when the key-backup state changes or when imported-key
        /// counters advance during a recover() call.
        fn on_backup_progress(self: &EventHandlerBridge, progress: &BackupProgress);
        /// Fired when the `RoomListService` state changes (Init →
        /// SettingUp → Running, etc.). `state` is one of the
        /// `ROOM_LIST_STATE_*` constants:
        ///   0 = Init        (initial state — no sync yet)
        ///   1 = SettingUp   (first rooms are being synced)
        ///   2 = Recovering  (recovering from error/terminated/stale)
        ///   3 = Running     (steady-state — all rooms syncing)
        ///   4 = Error       (sync stopped due to error)
        ///   5 = Terminated  (sync stopped by request)
        /// UIs use this to surface "Syncing rooms…" progress in the status
        /// bar while the joined-room set is still being hydrated.
        fn on_room_list_state(self: &EventHandlerBridge, state: u8);
        /// Fired when the cached set of MSC2545 image packs changes
        /// (user-pack edit, room-pack subscription edit, or live state-event
        /// update on a referenced room). The UI re-queries via
        /// `list_image_packs` / `list_pack_images`.
        fn on_image_packs_updated(self: &EventHandlerBridge);
        /// Fired when the `im.gnomos.tesseract` global account-data event
        /// changes (either on first sync or after `save_prefs` writes a new
        /// value). `json` is the raw event content, or `"{}"` when missing.
        /// The UI re-reads via `load_prefs` and applies any changed fields.
        fn on_account_prefs_updated(self: &EventHandlerBridge, json: &str);
        /// Fired when a new message matches push rules and a notification
        /// should be shown. Only called for non-self messages, only for live
        /// PushBack (not pagination). `is_mention` is true when the push rules
        /// returned a `SetHighlight(true)` tweak (@ mention or highlight rule).
        fn on_notification(
            self: &EventHandlerBridge,
            room_id: &str,
            room_name: &str,
            sender: &str,
            body: &str,
            is_mention: bool,
            avatar_bytes: &[u8],
            image_bytes: &[u8],
        );

        /// Fired when an incoming SAS verification request arrives from another
        /// device (`incoming = true`), or when an outgoing request we sent has
        /// been accepted and is ready for key exchange (`incoming = false`).
        /// `flow_id` is the opaque transaction ID that identifies this request
        /// across all subsequent verification calls.
        fn on_verification_request(
            self: &EventHandlerBridge,
            flow_id: &str,
            user_id: &str,
            device_id: &str,
            incoming: bool,
        );

        /// Fired when the SAS short-auth-string key exchange completes and the
        /// 7 emoji are ready to compare. The UI should transition to its
        /// ShowEmojis state and render the tiles. `flow_id` matches the one
        /// supplied by `on_verification_request`.
        fn on_sas_ready(self: &EventHandlerBridge, flow_id: &str, emojis: &Vec<VerificationEmoji>);

        /// Fired after both sides called `confirm_sas` — the device is now
        /// cross-signing verified. The UI should transition to Done state and
        /// then dismiss the banner.
        fn on_verification_done(self: &EventHandlerBridge, flow_id: &str);

        /// Fired when a verification flow is cancelled for any reason
        /// (mismatch, timeout, explicit cancel by either party). `reason` is a
        /// human-readable description from the cancel code.
        fn on_verification_cancelled(self: &EventHandlerBridge, flow_id: &str, reason: &str);

        /// Fired when the cross-signing verification state for the current
        /// account changes. `verified` is `true` when the SDK considers the
        /// current device's own identity fully verified (all cross-signing keys
        /// are consistent and confirmed). Use this to show/hide the
        /// "Verify this device" banner.
        fn on_verification_state_changed(self: &EventHandlerBridge, verified: bool);

        /// Fired when the set of typing users in `room_id` changes. `typing_user_ids`
        /// contains the localpart of each typing user (excluding the local user).
        /// An empty vec means no one is typing.
        fn on_typing_changed(
            self: &EventHandlerBridge,
            room_id: &str,
            typing_user_ids: &Vec<String>,
        );
    }

    // -------------------------------------------------------------------------
    // Rust-owned opaque type exposed to C++
    // -------------------------------------------------------------------------
    extern "Rust" {
        type ClientFfi;

        fn client_create() -> Box<ClientFfi>;

        // ----- Local waveform generation -----

        /// Decode an Ogg/Opus buffer and compute MSC1767 waveform samples
        /// (0..=1024, up to 200 values). Returns empty on invalid input.
        fn compute_waveform_from_ogg(bytes: &[u8]) -> Vec<u16>;

        /// Open (or create) the waveform SQLite store at `path`. Idempotent —
        /// safe to call multiple times; only the first call takes effect.
        fn init_waveform_store(path: &str);

        /// Look up a cached waveform by MXC URI. Returns empty when not found.
        fn load_voice_waveform(mxc_uri: &str) -> Vec<u16>;

        /// Persist a waveform for `mxc_uri`, evicting oldest entries beyond 2000.
        fn store_voice_waveform(mxc_uri: &str, waveform: &[u16]);

        /// Remove the cached waveform for `mxc_uri` (e.g. after media eviction).
        fn evict_voice_waveform(mxc_uri: &str);

        // ----- Data directory -----

        /// Override the matrix-sdk SQLite store path for this client. Must be
        /// called before `oauth_begin` / `restore_session` so the SDK opens
        /// the store at the correct location. Used by the multi-account host
        /// to scope each account's store under its own directory. Passing an
        /// empty string is a no-op (the client keeps the default path).
        fn set_data_dir(self: &mut ClientFfi, path: &str);

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
        fn paginate_back_with_status(
            self: &mut ClientFfi,
            room_id: &str,
            count: u16,
        ) -> PaginateResult;

        /// MSC3030 Jump to Date: resolve a Unix millisecond timestamp to the
        /// nearest event ID in `room_id`. `dir` is `"f"` (forward — first
        /// event ≥ ts) or `"b"` (backward — last event ≤ ts). On success
        /// `OpResult.message` holds the event ID string; on failure it holds
        /// the error description.
        fn timestamp_to_event(
            self: &mut ClientFfi,
            room_id: &str,
            ts_ms: u64,
            dir: &str,
        ) -> OpResult;

        /// MSC3030 Jump to Date: subscribe to a room's timeline focused on a
        /// specific event. Behaves like `subscribe_room` but builds a
        /// `TimelineFocus::Event` timeline centered on `focus_event_id`. Sets
        /// `is_focused = true` on the per-room state so `paginate_forward`
        /// can gate itself. Fires `on_timeline_reset` + individual event
        /// callbacks identically to `subscribe_room`.
        fn subscribe_room_at(self: &mut ClientFfi, room_id: &str, focus_event_id: &str)
            -> OpResult;

        /// MSC3030 Jump to Date: paginate forward in a focused timeline.
        /// Only valid after `subscribe_room_at`; returns
        /// `ok=false, message="not in focused mode"` for live timelines.
        /// `reached_end` is `true` when the live end of the timeline has been
        /// reached and the UI should switch back to a live subscription.
        fn paginate_forward(self: &mut ClientFfi, room_id: &str, count: u16) -> PaginateResult;

        /// Kick off a background pass that paginates every joined room not
        /// currently subscribed, up to ~50 events each, with bounded
        /// concurrency. Idempotent — safe to call from every room-open
        /// path. Silent: emits no `on_message_event` / `on_timeline_reset`
        /// callbacks for the rooms it visits. The persistent SDK event
        /// cache is what gets warmed.
        fn start_background_backfill(
            self: &mut ClientFfi,
            room_ids: &CxxVector<CxxString>,
        ) -> OpResult;

        /// Cancel an in-progress background backfill. No-op if none is
        /// running. Also called automatically from `stop_sync` and `Drop`.
        fn stop_background_backfill(self: &mut ClientFfi);

        // ----- Messaging -----

        fn send_message(
            self: &mut ClientFfi,
            room_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Re-enable the send queue for `room_id` after a recoverable failure.
        /// The SDK automatically retries all pending sends.
        fn retry_send(self: &mut ClientFfi, room_id: &str) -> OpResult;

        /// Abort a pending local echo identified by `txn_id` in `room_id`.
        fn abort_send(self: &mut ClientFfi, room_id: &str, txn_id: &str) -> OpResult;

        /// Send a typing notice to `room_id`. Fire-and-forget — errors are
        /// silently swallowed. Does not require `subscribe_room`.
        fn send_typing_notice(self: &mut ClientFfi, room_id: &str, typing: bool);

        /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds the
        /// `m.in_reply_to` relation and sends via `room.send()`. Does not require
        /// `subscribe_room`. Does not add the plain-text fallback body (Tesseract
        /// renders its own quote block for the reply indicator).
        fn send_reply(
            self: &mut ClientFfi,
            room_id: &str,
            event_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Trigger an async fetch of the details of the event referenced by
        /// `m.in_reply_to`. Requires `subscribe_room`. Returns immediately;
        /// the result arrives via `on_message_updated` for every message in
        /// the subscribed timeline that references `event_id`.
        fn fetch_reply_details(self: &mut ClientFfi, room_id: &str, event_id: &str) -> OpResult;

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
        fn send_image(
            self: &mut ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            width: u32,
            height: u32,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
        ) -> OpResult;

        /// Send an arbitrary file to `room_id` as an `m.file` event. `bytes`
        /// are the raw file payload (no client-side re-encoding); `mime_type`
        /// is best-effort guessed by the caller from extension / OS metadata
        /// (`application/octet-stream` is acceptable when unknown).
        /// `filename` is the user-visible file name. When `caption` is
        /// non-empty the event follows MSC2530 framing: `body` carries the
        /// caption and the dedicated `filename` field carries the file name.
        /// matrix-sdk handles plain and E2EE rooms transparently.
        fn send_file(
            self: &mut ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
        ) -> OpResult;

        /// Send an audio file to `room_id` as a plain `m.audio` event (not
        /// MSC3245 voice). `duration_ms` populates `info.duration`; pass 0
        /// when unknown. `caption`/`reply_event_id` follow MSC2530/m.in_reply_to
        /// framing identical to `send_file`.
        fn send_audio(
            self: &mut ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            duration_ms: u64,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
        ) -> OpResult;

        /// Send a video file to `room_id` as an `m.video` event. `width`/`height`
        /// are the video source dimensions; `thumb_bytes` is a JPEG first-frame
        /// thumbnail (empty slice if unavailable); `thumb_width`/`thumb_height`
        /// are its dimensions; `duration_ms` populates `info.duration`. The SDK
        /// uploads the thumbnail as a separate media item and sets
        /// `info.thumbnail_url`. E2EE rooms are handled transparently.
        fn send_video(
            self: &mut ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            width: u32,
            height: u32,
            thumb_bytes: &[u8],
            thumb_width: u32,
            thumb_height: u32,
            duration_ms: u64,
            reply_event_id: &str,
        ) -> OpResult;

        /// Encode raw signed 16-bit mono 48 kHz PCM (`pcm` byte slice,
        /// little-endian) into an Ogg/Opus stream and send it as an MSC3245
        /// `m.voice` event in `room_id`. `waveform` carries up to 256 MSC1767
        /// amplitude samples (0–1024); `duration_ms` populates
        /// `info.duration`. `caption` / `reply_event_id` follow the same
        /// MSC2530 / m.in_reply_to framing as `send_image` and `send_file`.
        fn send_voice(
            self: &mut ClientFfi,
            room_id: &str,
            pcm: &[u8],
            duration_ms: u64,
            waveform: &[u16],
            caption: &str,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
        ) -> OpResult;

        /// Edit `event_id` in `room_id` replacing its body with `new_body`.
        /// Uses `Room::make_edit_event` + `RoomSendQueue::send` so the edit
        /// is correctly formatted (Replacement relation + fallback body). Only
        /// works on own `m.text` events; returns `ok=false` for non-own or
        /// non-text events.
        fn send_edit(
            self: &mut ClientFfi,
            room_id: &str,
            event_id: &str,
            new_body: &str,
            formatted_body: &str,
        ) -> OpResult;

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
        fn send_reaction(
            self: &mut ClientFfi,
            room_id: &str,
            event_id: &str,
            key: &str,
        ) -> OpResult;

        /// Send an MSC4027 custom-image reaction. `key` is the mxc:// URI of
        /// the image; `shortcode` is the optional human-readable fallback
        /// (e.g. `:partyparrot:`) stored as `com.beeper.reaction.shortcode`.
        /// Omit shortcode (pass "") to skip the field. Unlike `send_reaction`
        /// this always sends (not toggles); redaction uses `send_reaction`.
        fn send_reaction_custom(
            self: &mut ClientFfi,
            room_id: &str,
            event_id: &str,
            key: &str,
            shortcode: &str,
        ) -> OpResult;

        /// Send public `m.read` and private `m.read.private` receipts for
        /// `event_id` in `room_id`. Does not require the room to be subscribed.
        fn send_read_receipt(self: &mut ClientFfi, room_id: &str, event_id: &str) -> OpResult;

        /// Send public `m.read` and private `m.read.private` receipts for the
        /// latest cached event in `room_id`. Used to clear the unread badge
        /// when the user opens a room. Does not require a subscription.
        fn mark_room_as_read(self: &mut ClientFfi, room_id: &str) -> OpResult;

        /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
        /// Wraps matrix-sdk-ui's `Timeline::redact`. Requires that the room
        /// is currently subscribed via `subscribe_room`. Server-side
        /// permission errors surface as `OpResult { ok: false, message: ... }`.
        fn redact_event(
            self: &mut ClientFfi,
            room_id: &str,
            event_id: &str,
            reason: &str,
        ) -> OpResult;

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
        fn toggle_favorite_sticker(self: &mut ClientFfi, image_url: &str) -> OpResult;

        // ----- Application prefs (im.gnomos.tesseract global account-data) -----

        /// Read the current `im.gnomos.tesseract` account-data event from
        /// the SDK's local sync cache. Returns the raw JSON content object,
        /// or `"{}"` when the event has never been written or the client is
        /// not logged in. No network roundtrip.
        fn load_prefs(self: &mut ClientFfi) -> String;

        /// Write `json` (the full content object) back to the homeserver as
        /// the `im.gnomos.tesseract` account-data event. Fire-and-forget —
        /// returns immediately; errors are silently swallowed. The next sync
        /// deliver of the event will trigger `on_account_prefs_updated`.
        fn save_prefs(self: &mut ClientFfi, json: &str);

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

        /// Fetch raw bytes from an arbitrary HTTP/HTTPS URL.
        /// Returns the response body on success, or an empty Vec on any error.
        /// Sets User-Agent to "Tesseract/0.1 (Matrix client)" per OSM tile policy.
        fn fetch_url_bytes(self: &mut ClientFfi, url: &str) -> Vec<u8>;

        /// Fetch OpenGraph preview metadata for an http(s) URL from the
        /// homeserver's `/_matrix/media/v3/preview_url` endpoint.
        /// Returns the raw JSON response body on success, or an empty String
        /// on any failure (not logged in, network error, rate-limit, no data).
        /// Blocks the calling thread — call only from a worker thread.
        fn get_url_preview(self: &mut ClientFfi, url: &str) -> String;

        // ----- MSC3266 room summary / join -----

        /// Fetch a room summary (name, topic, avatar, member count, join rule,
        /// encryption state) for any room the homeserver can see, whether or
        /// not the client is a member. Accepts a room ID (`!id:server`) or
        /// alias (`#alias:server`). Returns a JSON object on success or an
        /// empty string on error. Blocks the calling thread.
        fn get_room_summary(self: &mut ClientFfi, room_id_or_alias: &str) -> String;

        /// Join a room by its ID or alias. Returns the canonical room ID
        /// (e.g. `!id:server`) on success, or an empty string on failure.
        /// Blocks the calling thread — call only from a worker thread.
        fn join_room(self: &mut ClientFfi, room_id_or_alias: &str) -> String;

        /// Discover the homeserver URL for a server name or Matrix ID.
        /// Accepts "matrix.org", "@user:matrix.org", or "https://matrix.org".
        /// Returns JSON: `{"base_url":"https://...","error":""}` on success or
        /// `{"base_url":"","error":"message"}` on failure.
        /// Blocks the calling thread — call only from a worker thread.
        fn discover_homeserver(self: &mut ClientFfi, server_name_or_mxid: &str) -> String;

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

        // ----- SAS device verification -----

        /// Send an `m.key.verification.request` to-device event to every other
        /// device of the current user. When one accepts, `on_verification_request`
        /// fires with `incoming = false` and the UI should call `start_sas`.
        fn request_self_verification(self: &mut ClientFfi) -> OpResult;

        /// Accept an incoming verification request identified by `flow_id`.
        /// Call after receiving `on_verification_request(incoming=true)`.
        fn accept_verification(self: &mut ClientFfi, flow_id: &str) -> OpResult;

        /// Start the SAS key-exchange on a ready request. The SDK will fire
        /// `on_sas_ready` with the 7 emoji once both sides have exchanged keys.
        fn start_sas(self: &mut ClientFfi, flow_id: &str) -> OpResult;

        /// Confirm that the emoji shown on this device match the other device's
        /// display. Fires `on_verification_done` when both sides confirm.
        fn confirm_sas(self: &mut ClientFfi, flow_id: &str) -> OpResult;

        /// Cancel or decline a verification flow (e.g. emoji mismatch or user
        /// dismissed). Fires `on_verification_cancelled` on both sides.
        fn cancel_verification(self: &mut ClientFfi, flow_id: &str) -> OpResult;

        /// Return the 7 SAS emoji for `flow_id` after `on_sas_ready` has fired.
        /// Returns an empty Vec before the key exchange completes.
        fn get_sas_emojis(self: &ClientFfi, flow_id: &str) -> Vec<VerificationEmoji>;

        // ----- Server pushers (Step 12) -----

        /// Register (or update) an HTTP pusher on the homeserver.
        /// `pushkey` uniquely identifies this pusher instance (e.g. sanitised
        /// Matrix user ID + hostname). `endpoint_url` is the push gateway URL
        /// provided by the UnifiedPush distributor.
        fn register_pusher(
            self: &mut ClientFfi,
            pushkey: &str,
            app_id: &str,
            app_display_name: &str,
            device_display_name: &str,
            endpoint_url: &str,
            lang: &str,
        ) -> OpResult;

        /// Remove a pusher from the homeserver by `pushkey` / `app_id`.
        fn remove_pusher(self: &mut ClientFfi, pushkey: &str, app_id: &str) -> OpResult;

        /// Hint that a push notification arrived for `room_id`. Requests a
        /// targeted sliding-sync subscription for that room (union with any
        /// already-open rooms) so the next sync cycle delivers fresh state
        /// before the regular sync loop catches up.
        fn hint_push_room(self: &mut ClientFfi, room_id: &str) -> OpResult;

        // ----- Session teardown -----

        fn logout(self: &mut ClientFfi) -> OpResult;
    }
}
