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

pub fn highlight_code(code: &str, lang: &str, dark: bool) -> Vec<ffi::HighlightSpan> {
    super::highlight::highlight_code(code, lang, dark)
}

pub fn parse_matrix_link(uri: &str) -> ffi::MatrixLinkResult {
    super::matrix_uri::parse_matrix_link(uri)
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
        /// Number of unread messages that matched a notify push-rule action.
        notification_count: u64,
        /// Subset of notification_count that matched a highlight (mention) action.
        highlight_count: u64,
        /// Total unread messages (client-side, regardless of push rules). Drives
        /// the room-list "quiet unread" dot for rooms whose activity doesn't
        /// notify (e.g. "mentions only"). Superset of `notification_count`.
        unread_count: u64,
        /// True when the user set this room's notification mode to Mute. A muted
        /// room is excluded from the quiet-unread dot (silenced on purpose).
        muted: bool,
        is_direct: bool,
        /// mxc:// URI of the room avatar, empty string if none.
        avatar_url: String,
        /// Fallback avatar mxc:// URI for an avatar-less DM: the other
        /// participant's avatar, with functional members (bridge bots, per
        /// MSC4171 `io.element.functional_members`) excluded so bridged
        /// DMs show the puppet user's avatar instead of the bot's. Empty
        /// when the room is not a DM, when it has its own avatar, or when
        /// no real counterpart could be identified.
        dm_avatar_url: String,
        /// Bare Matrix ID (e.g. `@alice:server`) of the DM counterpart.
        /// Empty when the room is not a 1:1 DM or no real counterpart was
        /// identified. Presence lookups should key on this field.
        dm_counterpart_user_id: String,
        /// Body text of the most recent message (best-effort, may be empty).
        last_message_body: String,
        /// Display name of the last-message sender; empty when the sender is
        /// the current user (render as "You"), or when there is no last message.
        last_message_sender_name: String,
        /// Kind of the latest event for preview rendering:
        /// "text" | "image" | "video" | "gif" | "file" | "audio" | "sticker" | "".
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
        /// True when the room is tagged `m.lowpriority` by the current user.
        is_low_priority: bool,
        /// True when the room has encryption enabled.
        is_encrypted: bool,
        /// Room history visibility: "world_readable" | "shared" | "invited" | "joined".
        history_visibility: String,
        /// Snapshot of `m.room.pinned_events` resolved against the local event
        /// cache (sender + body snippet + timestamp). Sorted newest-first so
        /// the pinned-events banner can render without a separate fetch.
        /// `body_preview` is `(image)` / `(file)` / etc. for non-text events
        /// and `(unavailable)` when the id cannot be resolved.
        pinned_events: Vec<PinnedEvent>,
        /// Canonical alias of the room (`#alias:server`), or empty when none
        /// is set.  Read from local state — no network round-trip.
        canonical_alias: String,
    }

    /// One entry from `m.room.pinned_events` resolved for the banner UI.
    /// `event_id` is the canonical Matrix event id; the rest is best-effort
    /// metadata pulled from the local event cache. `body_preview` falls back
    /// to `(unavailable)` when the id cannot be resolved without a network
    /// round-trip — in that case `sender_name` is empty and `timestamp` is 0,
    /// but click-to-jump still works for events present in loaded history.
    struct PinnedEvent {
        event_id: String,
        sender_name: String,
        body_preview: String,
        timestamp: u64,
    }

    /// Lightweight descriptor for a pending room invitation, returned by
    /// `list_invites()` and carried by `on_invites_updated()` callbacks.
    /// `invited_at_ts` is the Unix timestamp in milliseconds of the invite
    /// event; 0 when unavailable (stripped-state events omit the timestamp
    /// unless the homeserver implements MSC4319).
    struct InviteInfo {
        room_id: String,
        room_name: String,
        room_avatar_url: String,
        room_topic: String,
        is_direct: bool,
        inviter_user_id: String,
        inviter_display_name: String,
        inviter_avatar_url: String,
        invited_at_ts: u64,
    }

    /// One colored run of a syntax-highlighted code block. `text` may contain
    /// newlines; concatenating every span's `text` reproduces the input code.
    /// `r`/`g`/`b` are the run's foreground color from the active syntect theme.
    struct HighlightSpan {
        text: String,
        r: u8,
        g: u8,
        b: u8,
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
        /// mxc:// URI when this is an MSC 4027 custom-image reaction.
        /// Empty string for plain Unicode reactions.
        source_url: String,
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

    /// A joined member of a room. `display_name` resolves to the user's
    /// localpart when no display name is set. `avatar_url` is the mxc://
    /// URI of the member's avatar, or empty when unset.
    struct RoomMember {
        user_id: String,
        display_name: String,
        avatar_url: String,
    }

    /// Result of a `resolve_user_profile` lookup. `exists` is true only when
    /// the homeserver returned a profile for the requested mxid (i.e. the user
    /// exists). `display_name` falls back to the localpart when the user has no
    /// display name set; `avatar_url` is the mxc:// URI or empty when unset.
    /// When `exists` is false the other fields are empty.
    struct UserProfile {
        exists: bool,
        user_id: String,
        display_name: String,
        avatar_url: String,
    }

    /// A single timeline event (message).
    /// Discriminated union: inspect `msg_type` to determine which fields are valid.
    /// For `m.image`   → source_url / source_encrypted_json, width, height are populated.
    ///                   image_filename is non-empty only when the sender supplied
    ///                   an explicit MSC2530 filename; in that case `body` is a caption.
    ///                   image_animated is true when MSC4230 is_animated flag is set.
    /// For `m.file`    → file_url / file_encrypted_json, file_name, file_size are populated.
    /// For `m.sticker` → source_url / source_encrypted_json, width, height (same as m.image).
    ///                   image_animated is true when MSC4230 is_animated flag is set.
    /// For `m.voice`   → audio_url / audio_encrypted_json, audio_duration_ms, audio_waveform,
    ///                   audio_mime are populated (MSC3245 voice messages).
    ///                   Non-voice `m.audio` events are converted to "m.file"
    ///                   on the Rust side so the file-card path renders them.
    /// For `m.video`   → source_url / source_encrypted_json (video source), width, height,
    ///                   image_filename (MSC2530 caption filename),
    ///                   video_thumbnail_url / video_thumbnail_encrypted_json,
    ///                   video_duration_ms, video_mime.
    /// Thumbnail fields → *_url is always the mxc:// URI; *_encrypted_json is non-empty
    ///                   only when the thumbnail itself is encrypted.
    /// Reply fields   → in_reply_to_id is non-empty when this event is a reply.
    ///                   in_reply_to_sender_name and in_reply_to_body carry the
    ///                   replied-to event's display name and body snippet (populated
    ///                   only when the replied-to item is present in the local cache;
    ///                   both are empty strings when the cache doesn't have it yet).
    /// Thread fields  → thread_root_id is non-empty when this event is an in-thread
    ///                   reply (MSC3440). is_thread_root is true when this event roots
    ///                   a thread; only then are thread_reply_count and the
    ///                   thread_latest_* preview fields meaningful (they describe the
    ///                   most recent reply and stay empty/0 until the summary resolves).
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
        /// mxc:// URI of the primary media source (m.image, m.sticker, m.video).
        source_url: String,
        /// Non-empty only when the primary source is encrypted; contains the full
        /// JSON MediaSource blob for Client::fetch_source_bytes.
        source_encrypted_json: String,
        width: u64,
        height: u64,
        /// mxc:// URI of the file attachment (m.file).
        file_url: String,
        /// Non-empty only when the file attachment is encrypted.
        file_encrypted_json: String,
        file_name: String,
        file_size: u64,
        /// Non-empty when msg_type is "m.image" and the sender provided an explicit
        /// MSC2530 `filename` field (distinct from `body`).  When set, `body` is a
        /// user-written caption and should be displayed below the image.
        image_filename: String,
        /// mxc:// URI of the audio/voice clip (m.voice / m.audio).
        audio_url: String,
        /// Non-empty only when the audio source is encrypted.
        audio_encrypted_json: String,
        /// Duration of the voice clip in milliseconds (MSC1767 `audio.duration`,
        /// falling back to `info.duration`). 0 when neither is provided.
        audio_duration_ms: u64,
        /// MSC1767 waveform samples, each clamped to 0..=1024. Empty when the
        /// sender did not include one (the UI renders flat placeholder bars).
        audio_waveform: Vec<u16>,
        /// MIME type advertised by the sender (typically "audio/ogg"). May be
        /// empty when missing from `info.mimetype`.
        audio_mime: String,
        /// mxc:// URI of the video thumbnail (m.video). Empty when absent.
        video_thumbnail_url: String,
        /// Non-empty only when the video thumbnail is encrypted.
        video_thumbnail_encrypted_json: String,
        /// mxc:// URI of the image/sticker thumbnail. Empty when absent.
        image_thumbnail_url: String,
        /// Non-empty only when the image thumbnail is encrypted.
        image_thumbnail_encrypted_json: String,
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
        /// mxc:// URI of the replied-to image thumbnail (or full-res when no
        /// thumbnail exists). Non-empty only when the reply target is an m.image
        /// event whose content is present in the local cache.
        in_reply_to_image_url: String,
        /// JSON-serialised EncryptedFile for in_reply_to_image_url when the
        /// media is E2EE-encrypted. Empty for unencrypted media.
        in_reply_to_image_encrypted_json: String,
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
        // ----- MSC3440 threads -----
        /// Event ID of the thread root when this event is an in-thread reply
        /// (`content().thread_root()`); empty otherwise.
        thread_root_id: String,
        /// True when this event roots a thread (`content().thread_summary().is_some()`).
        is_thread_root: bool,
        /// Replies in the thread excluding the root (`ThreadSummary.num_replies`).
        /// 0 when this event is not a thread root.
        thread_reply_count: u64,
        /// Display name of the latest thread reply's sender (resolved profile,
        /// bare Matrix ID fallback). Empty when not a root or summary not ready.
        thread_latest_sender_name: String,
        /// Snippet of the latest thread reply ("(image)" etc. for non-text).
        /// Empty when not a root or summary not ready.
        thread_latest_body: String,
        /// Unix-ms timestamp of the latest thread reply. 0 when unavailable.
        thread_latest_ts: u64,
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

    /// One GIF search result delivered to C++ via `on_gif_results`. URLs point
    /// at the provider CDN: `preview_url` is a small static JPEG for the result
    /// strip; `image_url` is the animated *send* form (MP4 preferred, WebP
    /// fallback, GIF last resort, per `image_mime`) fetched at send time;
    /// `strip_url` is the animated form the strip *displays* (WebP/GIF, decoded
    /// natively without a video pipeline; falls back to `image_url`).
    struct GifResult {
        id: String,
        preview_url: String,
        preview_w: u32,
        preview_h: u32,
        image_url: String,
        image_w: u32,
        image_h: u32,
        image_mime: String,
        strip_url: String,
        strip_mime: String,
    }

    /// One full-text message-search hit delivered to C++ via
    /// `on_search_results`. The local FTS5 index (per-account `app_cache.db`)
    /// stores only what is needed to render a result row and jump to the
    /// message; `room_name` is resolved at query time from the live client so
    /// the UI need not look it up. `body` is the decrypted plaintext that
    /// matched (encrypted-room messages included).
    struct SearchHit {
        event_id: String,
        room_id: String,
        room_name: String,
        sender: String,
        sender_name: String,
        body: String,
        /// Unix timestamp in milliseconds.
        timestamp_ms: u64,
    }

    /// Summary of the local search index for the Settings panel.
    /// `backfill_done` is true once the one-time history crawl has finished a
    /// full pass; `oldest_ts_ms` is 0 when the index is empty.
    /// `index_bytes` is populated separately via `search_index_size_bytes`
    /// (the `dbstat` walk is computed once on panel open, not on every poll).
    struct SearchIndexStats {
        message_count: u64,
        room_count: u64,
        oldest_ts_ms: u64,
        backfill_done: bool,
        /// Always 0 in this struct; populated C++-side from `search_index_size_bytes`.
        index_bytes: u64,
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

    /// One Matrix device/session for the current user, returned by
    /// `list_devices()`. `verification_state` is 0=Unknown, 1=Unverified,
    /// 2=Verified. `is_current` is true for the device this client is
    /// logged in as. `last_seen_ts` is unix milliseconds, 0 when absent.
    struct DeviceFfi {
        device_id: String,
        display_name: String,
        last_seen_ip: String,
        last_seen_ts: u64,
        verification_state: u8,
        is_current: bool,
    }

    /// One thread in a room, flattened from matrix-sdk-ui's ThreadListItem.
    /// `latest_*` fields are empty/0 when no reply summary is available.
    struct ThreadInfo {
        root_event_id: String,
        root_sender_name: String,
        root_body: String,
        root_timestamp: u64,
        latest_event_id: String,
        latest_sender_name: String,
        latest_body: String,
        latest_timestamp: u64,
        num_replies: u64,
    }

    /// Result of `begin_delete_device`. On a clean success `needs_uia` is
    /// false. When the homeserver requires User-Interactive Auth, `needs_uia`
    /// is true and `fallback_url` is a homeserver URL the UI must open in a
    /// browser; once the user authenticates there, the UI calls
    /// `complete_delete_device(device_id, session)` to finish the delete.
    struct DeleteDeviceBegin {
        ok: bool,
        message: String,
        needs_uia: bool,
        fallback_url: String,
        session: String,
    }

    /// Result of `begin_reset_crypto_identity`. On `needs_approval == true`
    /// the homeserver requires the user to approve the cross-signing reset in
    /// a browser: the UI opens `approval_url` and waits for the asynchronous
    /// `on_crypto_reset_result` callback (the SDK polls until approval). When
    /// `needs_approval` is false and `ok` is true the reset completed with no
    /// further auth. `ok == false` means the reset could not be started.
    struct CryptoResetBegin {
        ok: bool,
        message: String,
        needs_approval: bool,
        approval_url: String,
    }

    /// MSC4278 global media-preview config. `media_previews` is a u8 because
    /// cxx shared structs can't carry C++-side enums without boilerplate; the
    /// C++ wrapper maps it to `MediaPreviewConfig::Mode`:
    ///   0 = off, 1 = private (non-public rooms only), 2 = on.
    struct MediaPreviewConfigFfi {
        media_previews: u8,
        invite_avatars: bool,
    }

    /// Per-room MSC4278 context for the current room. `has_media_previews`
    /// is true only when the room's own account-data overrides the field;
    /// when false the caller falls back to the global value and
    /// `media_previews` is meaningless. `join_rule` is the room's local join
    /// rule ("public", "invite", "knock", "restricted", "knock_restricted",
    /// "private", or "" when indeterminate) — used to evaluate the `private`
    /// preview mode. Empty / unknown should be treated as public.
    struct MediaPreviewOverrideFfi {
        has_media_previews: bool,
        media_previews: u8,
        join_rule: String,
    }

    /// Result of `parse_matrix_link`.  `kind` values:
    ///   0 = unknown / not a matrix link
    ///   1 = room ID  (`!room:server`)
    ///   2 = room alias (`#alias:server`)
    ///   3 = user  (`@user:server`)
    ///   4 = event (`!room:server` + `$event:server`)
    struct MatrixLinkResult {
        kind: u8,
        primary: String,
        event_id: String,
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

        /// Thread-timeline twins of the four room-timeline callbacks. `room_id`
        /// is the host room; `thread_root` is the thread root event id. Indices
        /// follow the same visible-index VectorDiff semantics.
        fn on_thread_reset(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            snapshot: &Vec<TimelineEvent>,
        );
        fn on_thread_inserted(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            index: u64,
            event: &TimelineEvent,
        );
        fn on_thread_updated(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            index: u64,
            event: &TimelineEvent,
        );
        fn on_thread_removed(
            self: &EventHandlerBridge,
            room_id: &str,
            thread_root: &str,
            index: u64,
        );

        fn on_rooms_updated(self: &EventHandlerBridge, rooms: &Vec<RoomInfo>);
        /// Fired when the set of pending room invitations changes (first sync,
        /// accept/decline, or a new invite arriving via push). The UI should
        /// refresh its invitation list via `list_invites()` or use the snapshot
        /// carried by this callback directly.
        fn on_invites_updated(self: &EventHandlerBridge, invites: &Vec<InviteInfo>);
        fn on_error(self: &EventHandlerBridge, context: &str, message: &str, soft_logout: bool);
        fn on_session_refreshed(self: &EventHandlerBridge, session_json: &str);
        /// Synchronously persist a refreshed OAuth session blob to the
        /// platform-authoritative secret store (SessionStore::save_account ->
        /// SecretStore: Credential Manager / Keychain / libsecret, with a
        /// plaintext fallback when the backend is unavailable). Called from
        /// matrix-sdk's save_session_callback on a worker thread so a rotated
        /// refresh token survives an unclean shutdown even when the async
        /// TokensRefreshed watcher is aborted mid-flight. `user_id` is the full
        /// MXID the session belongs to.
        fn persist_session(user_id: &str, session_json: &str);
        /// Fired when the key-backup state changes or when imported-key
        /// counters advance during a recover() call.
        fn on_backup_progress(self: &EventHandlerBridge, progress: &BackupProgress);
        /// Fired as `enable_recovery()` progresses through setup stages.
        /// `step` encodes EnableProgress: 0=Starting 1=CreatingBackup
        /// 2=CreatingRecoveryKey 3=BackingUp 4=Done 5=Error 6=RoomKeyUploadError.
        /// Step 6 is non-fatal — the SDK will retry backup automatically.
        /// When step==4, `recovery_key` holds the generated key (empty if
        /// passphrase mode was chosen). When step==3, `backed_up`/`total`
        /// carry the running backup count. When step==5, `recovery_key` holds
        /// the error description.
        fn on_enable_recovery_progress(
            self: &EventHandlerBridge,
            step: u8,
            recovery_key: &str,
            backed_up: u32,
            total: u32,
        );
        /// Fired when an in-progress cross-signing reset (started via
        /// `begin_reset_crypto_identity`) resolves. `ok` is true when the
        /// browser approval succeeded and the new identity was uploaded;
        /// otherwise `message` carries the failure / cancellation reason.
        fn on_crypto_reset_result(self: &EventHandlerBridge, ok: bool, message: &str);
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
        /// Fired when the count of in-flight extra HTTP requests changes
        /// (media downloads, /messages back-pagination). Does not include
        /// the sync long-poll; C++ adds 1 for that when RoomListState is
        /// Running or Recovering. Fires on every increment and decrement so
        /// the status-bar dot tracks the exact concurrent-request count.
        fn on_inflight_changed(self: &EventHandlerBridge, count: u32);
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
        /// Fired when the global MSC4278 `m.media_preview_config` account-data
        /// event changes (first sync, local `set_media_preview_config`, or a
        /// change from another device). `json` is the raw event content, or
        /// `"{}"` when missing. The UI re-reads via `media_preview_config`.
        fn on_media_preview_config_updated(self: &EventHandlerBridge, json: &str);
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

        /// Fired when a presence event arrives for `user_id`. `state` encodes:
        ///   1 = Online  2 = Unavailable  3 = Offline
        fn on_presence_changed(self: &EventHandlerBridge, user_id: &str, state: u8);

        /// Fired when the cached thread list for `room_id` changes. The UI
        /// re-queries via list_room_threads (re-query ping, like
        /// on_image_packs_updated).
        fn on_threads_updated(self: &EventHandlerBridge, room_id: &str);

        /// Fired when an async media download started via `fetch_media_async`
        /// completes (or fails / times out / is cancelled — `bytes` is empty in
        /// those cases). `request_id` is the opaque correlation token the caller
        /// passed to `fetch_media_async`; the C++ side looks up the pending
        /// request, writes the disk cache, decodes and repaints. A late callback
        /// for an already-cancelled request is ignored on the C++ side.
        fn on_media_ready(self: &EventHandlerBridge, request_id: u64, bytes: &[u8]);

        /// Fired when an async URL-preview fetch started via
        /// `get_url_preview_async` completes. `request_id` is the correlation
        /// token; `preview_json` is the same JSON shape `get_url_preview`
        /// returns synchronously (an empty string on failure).
        fn on_url_preview_ready(
            self: &EventHandlerBridge,
            request_id: u64,
            preview_json: &str,
        );

        /// Fired when an async GIF search started via `gif_search_async`
        /// completes successfully. `request_id` is the correlation token the
        /// caller passed; the C++ side drops results whose id is stale (a newer
        /// search has since been issued).
        fn on_gif_results(
            self: &EventHandlerBridge,
            request_id: u64,
            results: &Vec<GifResult>,
        );

        /// Fired when an async GIF search fails (network / provider error).
        /// `request_id` is the correlation token; `message` is human-readable.
        fn on_gif_search_failed(
            self: &EventHandlerBridge,
            request_id: u64,
            message: &str,
        );

        /// Fired when an async full-text search started via
        /// `search_messages_async` completes successfully. `request_id` is the
        /// correlation token the caller passed; the C++ side drops results
        /// whose id is stale (a newer query has since been issued).
        fn on_search_results(
            self: &EventHandlerBridge,
            request_id: u64,
            results: &Vec<SearchHit>,
        );

        /// Fired when an async full-text search fails (e.g. the index is not
        /// open). `request_id` is the correlation token; `message` is
        /// human-readable.
        fn on_search_failed(
            self: &EventHandlerBridge,
            request_id: u64,
            message: &str,
        );

        /// Fired when an async pagination request started via
        /// `paginate_back_async` or `paginate_forward_async` completes (or
        /// fails). `request_id` is the correlation token. For backward
        /// pagination `reached_start` indicates no more history; for forward
        /// pagination `reached_end` indicates the live end was reached.
        fn on_paginate_result(
            self: &EventHandlerBridge,
            request_id: u64,
            ok: bool,
            reached_start: bool,
            reached_end: bool,
            message: &str,
        );

        /// Fired when an async room action (accept_invite_async,
        /// join_room_async, leave_room_async) completes or fails.
        /// `request_id` is the correlation token. `joined_room_id` carries
        /// the canonical room ID returned by a join (empty on failure or for
        /// non-join actions). `message` is a human-readable error on failure.
        fn on_room_action_complete(
            self: &EventHandlerBridge,
            request_id: u64,
            ok: bool,
            joined_room_id: &str,
            message: &str,
        );

        /// Fired when an async media upload started via send_image_async,
        /// send_file_async, send_audio_async, or send_video_async completes
        /// or fails. `request_id` is the correlation token. `message` is a
        /// human-readable error on failure (empty on success).
        fn on_upload_complete(
            self: &EventHandlerBridge,
            request_id: u64,
            ok: bool,
            message: &str,
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

        // ----- Code-block syntax highlighting -----

        /// Syntax-highlight `code` as language `lang` (a markdown fence token
        /// like "rust" or "py") for the given light/dark mode. Returns colored
        /// runs whose concatenated text equals `code`; an empty vec means the
        /// language was not recognized and the caller should render plain text.
        fn highlight_code(code: &str, lang: &str, dark: bool) -> Vec<HighlightSpan>;

        // ----- Matrix URI / matrix.to link parsing -----

        /// Parse a `https://matrix.to/#/…` URL or a `matrix:` URI (MSC2312)
        /// into a structured result.  Returns `kind == 0` for unrecognised
        /// input.  Both `?via=` and `?action=join` query params are stripped.
        /// Percent-encoded path components are decoded.
        fn parse_matrix_link(uri: &str) -> MatrixLinkResult;

        // ----- Data directory -----

        /// Override the matrix-sdk SQLite store path for this client. Must be
        /// called before `oauth_begin` / `restore_session` so the SDK opens
        /// the store at the correct location. Used by the multi-account host
        /// to scope each account's store under its own directory. Passing an
        /// empty string is a no-op (the client keeps the default path).
        fn set_data_dir(self: &mut ClientFfi, path: &str);

        // ----- OAuth -----

        /// Begin the OAuth login (or, when `register_account` is true,
        /// registration via OIDC prompt=create) flow. Returns the auth URL to
        /// open in a browser.
        fn oauth_begin(self: &mut ClientFfi, homeserver: &str, register_account: bool) -> OAuthBegin;

        /// Best-effort: does `homeserver` advertise OIDC registration support
        /// (`prompt_values_supported` contains `create`)? Blocks — worker thread.
        fn homeserver_supports_registration(
            self: &ClientFfi,
            homeserver: &str,
        ) -> bool;

        fn oauth_await_callback(self: &mut ClientFfi) -> OpResult;
        fn oauth_cancel(self: &mut ClientFfi);

        // ----- Session -----

        fn restore_session(self: &mut ClientFfi, session_json: &str) -> OpResult;
        fn export_session(self: &ClientFfi) -> String;

        /// Fetch homeserver spec versions and enabled capabilities.
        /// Returns JSON blob or empty string when not logged in / on error.
        /// Blocks — call from a worker thread only.
        fn get_server_info(self: &ClientFfi) -> String;

        // ----- Sync -----

        fn start_sync(self: &mut ClientFfi, handler: UniquePtr<EventHandlerBridge>);
        fn stop_sync(self: &mut ClientFfi);
        fn clear_caches(self: &mut ClientFfi) -> OpResult;

        /// Live count of extra in-flight HTTP operations (excludes the sync
        /// long-poll). Cheap atomic read; safe to call from any thread.
        fn in_flight_count(self: &ClientFfi) -> u32;

        // ----- Room list -----

        fn list_rooms(self: &ClientFfi) -> Vec<RoomInfo>;

        // ----- Invitations -----

        /// Snapshot of all pending room invitations. Reads the local SDK
        /// cache — no network roundtrip. The list updates when
        /// `on_invites_updated` fires.
        fn list_invites(self: &ClientFfi) -> Vec<InviteInfo>;

        /// Non-blocking accept. Spawns the join as a tokio task and delivers
        /// the result via `on_room_action_complete(request_id, ok, room_id, message)`.
        fn accept_invite_async(self: &ClientFfi, request_id: u64, room_id: &str);

        /// Non-blocking decline. Spawns `room.leave()` as a tokio task;
        /// no callback — failures are logged internally.
        fn decline_invite_async(self: &ClientFfi, room_id: &str);

        /// Non-blocking block-invite. Spawns leave + ignore_user as a tokio
        /// task; no callback — failures are logged internally.
        fn block_invite_async(
            self: &ClientFfi,
            room_id: &str,
            inviter_user_id: &str,
        );

        // ----- Timeline subscription (Step 2) -----

        /// Subscribe to a room's timeline. Fires an immediate empty
        /// `on_timeline_reset` so the UI clears prior content, then a
        /// follow-up `on_timeline_reset` carrying the initial cached
        /// snapshot once it has been converted. Subsequent live changes
        /// arrive as positional `on_message_inserted` /
        /// `on_message_updated` / `on_message_removed` callbacks. Call
        /// `paginate_back_with_status` to load older history.
        fn subscribe_room(self: &ClientFfi, room_id: &str) -> OpResult;

        /// Unsubscribe from a room's timeline and cancel its background task.
        fn unsubscribe_room(self: &ClientFfi, room_id: &str);

        /// Paginate backwards in a subscribed room's timeline, reporting
        /// whether the timeline has reached its first event (no further
        /// pagination possible). Older items arrive as `on_message_inserted`
        /// callbacks at the front of the timeline (index 0, or wherever the
        /// cache grafts them in). UIs use `reached_start` to latch their
        /// scroll-up trigger off.
        fn paginate_back_with_status(
            self: &ClientFfi,
            room_id: &str,
            count: u16,
        ) -> PaginateResult;

        /// Non-blocking counterpart of `paginate_back_with_status`. Spawns the
        /// HTTP paginate as a tokio task and fires
        /// `on_paginate_result(request_id, ok, reached_start, false, message)`
        /// on completion. Does not pin a C++ worker thread.
        fn paginate_back_async(self: &ClientFfi, request_id: u64, room_id: &str, count: u16);

        /// Non-blocking counterpart of `paginate_forward`. Spawns the HTTP
        /// paginate as a tokio task and fires
        /// `on_paginate_result(request_id, ok, false, reached_end, message)`
        /// on completion. Does not pin a C++ worker thread.
        fn paginate_forward_async(self: &ClientFfi, request_id: u64, room_id: &str, count: u16);

        /// MSC3030 Jump to Date: resolve a Unix millisecond timestamp to the
        /// nearest event ID in `room_id`. `dir` is `"f"` (forward — first
        /// event ≥ ts) or `"b"` (backward — last event ≤ ts). On success
        /// `OpResult.message` holds the event ID string; on failure it holds
        /// the error description.
        fn timestamp_to_event(
            self: &ClientFfi,
            room_id: &str,
            ts_ms: u64,
            dir: &str,
        ) -> OpResult;

        /// MSC3030 Jump to Date: subscribe to a room's timeline focused on a
        /// specific event. Behaves like `subscribe_room` but builds a
        /// `TimelineFocus::Event` timeline centered on `focus_event_id`.
        /// Fires `on_timeline_reset` + individual event callbacks identically
        /// to `subscribe_room`.
        fn subscribe_room_at(self: &ClientFfi, room_id: &str, focus_event_id: &str)
            -> OpResult;

        // ----- Thread timeline subscription -----

        /// Subscribe to the thread rooted at `root_event_id` in `room_id`.
        /// Fires `on_thread_reset` (empty) immediately, then live
        /// `on_thread_inserted` / `on_thread_updated` / `on_thread_removed`
        /// callbacks as replies arrive. Call `paginate_thread_back` for older
        /// replies.
        fn subscribe_thread(
            self: &ClientFfi,
            room_id: &str,
            root_event_id: &str,
        ) -> OpResult;

        /// Unsubscribe from a thread timeline and cancel its background tasks.
        fn unsubscribe_thread(
            self: &ClientFfi,
            room_id: &str,
            root_event_id: &str,
        );

        /// Paginate backwards in a subscribed thread timeline. Older replies
        /// arrive as `on_thread_inserted` callbacks at the front of the thread.
        /// `reached_start` is `true` when there are no more replies to fetch.
        fn paginate_thread_back(
            self: &ClientFfi,
            room_id: &str,
            root_event_id: &str,
            count: u16,
        ) -> PaginateResult;

        // ----- Thread list subscription -----

        /// Start watching the thread list for `room_id`: builds a
        /// ThreadListService, kicks an initial pagination, and fires
        /// on_threads_updated when the list changes.
        fn subscribe_room_threads(self: &ClientFfi, room_id: &str) -> OpResult;

        /// Stop watching the thread list for `room_id`.
        fn unsubscribe_room_threads(self: &ClientFfi, room_id: &str);

        /// Snapshot of the current thread list for `room_id` (most-recent
        /// first). Empty when not subscribed or none known yet.
        fn list_room_threads(self: &ClientFfi, room_id: &str) -> Vec<ThreadInfo>;

        /// Paginate older threads. reached_start == true means list exhausted.
        fn paginate_room_threads(self: &ClientFfi, room_id: &str) -> PaginateResult;

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

        /// Like `start_background_backfill` but auto-selects every joined
        /// room whose timestamp is absent from the in-memory backfill cache
        /// and whose SDK event cache has no `latest_event_timestamp`. Called
        /// when the "group inactive rooms" feature is active to ensure all
        /// rooms get a classification timestamp, not just those visible in
        /// the list.
        fn start_background_backfill_all_uncached(self: &mut ClientFfi) -> OpResult;

        /// One-shot prefetch of recent messages for the given unread rooms into
        /// the SDK event cache, so opening them is instant. The caller passes
        /// the already capped + LRU-ordered (most-recently-active first) set of
        /// unread, non-muted rooms. Unlike `start_background_backfill` this does
        /// NOT skip rooms that already have a timestamp / are cached — unread
        /// rooms always have a timestamp yet still need their *newest* messages.
        /// Skips rooms with a live timeline (open room + pop-outs). Bounded
        /// concurrency on a task independent of the inactive-grouping backfill,
        /// so the two never abort each other. Silent (no callbacks). Idempotent
        /// while a prefetch is in flight.
        fn start_unread_prefetch(
            self: &mut ClientFfi,
            room_ids: &CxxVector<CxxString>,
        ) -> OpResult;

        /// Cancel an in-progress unread prefetch. No-op if none is running.
        /// Also called automatically from `stop_sync` and `Drop`.
        fn stop_unread_prefetch(self: &mut ClientFfi);

        // ----- Messaging -----

        fn send_message(
            self: &ClientFfi,
            room_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Send an `m.emote` message (the `/me` slash command). Same arguments
        /// and semantics as `send_message` but the event carries an `m.emote`
        /// msgtype. Callers are expected to have already stripped the `/me `
        /// prefix from `body` / `formatted_body`.
        fn send_emote(
            self: &ClientFfi,
            room_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Re-enable the send queue for `room_id` after a recoverable failure.
        /// The SDK automatically retries all pending sends.
        fn retry_send(self: &ClientFfi, room_id: &str) -> OpResult;

        /// Abort a pending local echo identified by `txn_id` in `room_id`.
        fn abort_send(self: &ClientFfi, room_id: &str, txn_id: &str) -> OpResult;

        /// Send a typing notice to `room_id`. Fire-and-forget — errors are
        /// silently swallowed. Does not require `subscribe_room`.
        fn send_typing_notice(self: &ClientFfi, room_id: &str, typing: bool);

        /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds the
        /// `m.in_reply_to` relation and sends via `room.send()`. Does not require
        /// `subscribe_room`. Does not add the plain-text fallback body (Tesseract
        /// renders its own quote block for the reply indicator).
        fn send_reply(
            self: &ClientFfi,
            room_id: &str,
            event_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Send `body` as a message into the thread rooted at `thread_root`
        /// (MSC3440 `m.thread` relation). Does not require `subscribe_room`.
        fn send_thread_message(
            self: &ClientFfi,
            room_id: &str,
            thread_root: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Send `body` as a reply to `in_reply_to_event_id` *within* the thread
        /// rooted at `thread_root`. Does not require `subscribe_room`.
        fn send_thread_reply(
            self: &ClientFfi,
            room_id: &str,
            thread_root: &str,
            in_reply_to_event_id: &str,
            body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Trigger an async fetch of the details of the event referenced by
        /// `m.in_reply_to`. Requires `subscribe_room`. Returns immediately;
        /// the result arrives via `on_message_updated` for every message in
        /// the subscribed timeline that references `event_id`.
        fn fetch_reply_details(self: &ClientFfi, room_id: &str, event_id: &str) -> OpResult;

        /// Send an image to `room_id`. `bytes` are the already-encoded image
        /// payload (PNG/JPEG/etc. as identified by `mime_type`); the SDK
        /// uploads them via the homeserver's media repository and posts an
        /// `m.image` event. `filename` is the user-visible file name (e.g.
        /// `clipboard-20260512-141503.png`). When `caption` is non-empty the
        /// event follows MSC2530 framing: `body` is set to the caption and
        /// the dedicated `filename` field carries the file name. When
        /// `caption` is empty, `body` is set to the filename and the
        /// MSC2530 `filename` field is omitted (legacy fallback). `width`
        /// and `height` populate `info.{w,h}`; pass 0 when unknown. When
        /// `is_animated` is true the image is sent as a raw `m.image` event
        /// carrying the MSC4230 `org.matrix.msc4230.is_animated` flag and the
        /// `fi.mau.gif` vendor hint so animated GIFs/WebPs autoplay and
        /// loop on capable clients (the standard `send_attachment` path
        /// strips these fields).
        fn send_image(
            self: &ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            width: u32,
            height: u32,
            is_animated: bool,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
            /// When non-empty, sends the media into this thread root (MSC3440).
            thread_root: &str,
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
            self: &ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
            /// When non-empty, sends the media into this thread root (MSC3440).
            thread_root: &str,
        ) -> OpResult;

        /// Send an audio file to `room_id` as a plain `m.audio` event (not
        /// MSC3245 voice). `duration_ms` populates `info.duration`; pass 0
        /// when unknown. `caption`/`reply_event_id` follow MSC2530/m.in_reply_to
        /// framing identical to `send_file`.
        fn send_audio(
            self: &ClientFfi,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            duration_ms: u64,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
            /// When non-empty, sends the media into this thread root (MSC3440).
            thread_root: &str,
        ) -> OpResult;

        /// Send a video file to `room_id` as an `m.video` event. `width`/`height`
        /// are the video source dimensions; `thumb_bytes` is a JPEG first-frame
        /// thumbnail (empty slice if unavailable); `thumb_width`/`thumb_height`
        /// are its dimensions; `duration_ms` populates `info.duration`. The SDK
        /// uploads the thumbnail as a separate media item and sets
        /// `info.thumbnail_url`. E2EE rooms are handled transparently.
        fn send_video(
            self: &ClientFfi,
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
            /// When non-empty, sends the media into this thread root (MSC3440).
            thread_root: &str,
        ) -> OpResult;

        /// Non-blocking image send. Spawns the upload as a tokio task and
        /// delivers the result via `on_upload_complete`. `bytes` are cloned
        /// before returning; the caller need not keep them alive.
        fn send_image_async(
            self: &ClientFfi,
            request_id: u64,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            width: u32,
            height: u32,
            is_animated: bool,
            reply_event_id: &str,
            thread_root: &str,
        );

        /// Non-blocking file send. Spawns the upload as a tokio task and
        /// delivers the result via `on_upload_complete`.
        fn send_file_async(
            self: &ClientFfi,
            request_id: u64,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            reply_event_id: &str,
            thread_root: &str,
        );

        /// Non-blocking audio send. Spawns the upload as a tokio task and
        /// delivers the result via `on_upload_complete`.
        fn send_audio_async(
            self: &ClientFfi,
            request_id: u64,
            room_id: &str,
            bytes: &[u8],
            mime_type: &str,
            filename: &str,
            caption: &str,
            duration_ms: u64,
            reply_event_id: &str,
            thread_root: &str,
        );

        /// Non-blocking video send. Spawns the upload as a tokio task and
        /// delivers the result via `on_upload_complete`.
        fn send_video_async(
            self: &ClientFfi,
            request_id: u64,
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
            thread_root: &str,
        );

        /// Encode raw signed 16-bit mono 48 kHz PCM (`pcm` byte slice,
        /// little-endian) into an Ogg/Opus stream and send it as an MSC3245
        /// `m.voice` event in `room_id`. `waveform` carries up to 256 MSC1767
        /// amplitude samples (0–1024); `duration_ms` populates
        /// `info.duration`. `caption` / `reply_event_id` follow the same
        /// MSC2530 / m.in_reply_to framing as `send_image` and `send_file`.
        fn send_voice(
            self: &ClientFfi,
            room_id: &str,
            pcm: &[u8],
            duration_ms: u64,
            waveform: &[u16],
            caption: &str,
            /// When non-empty, adds an `m.in_reply_to` relation.
            reply_event_id: &str,
            /// When non-empty, sends the media into this thread root (MSC3440).
            thread_root: &str,
        ) -> OpResult;

        /// Edit `event_id` in `room_id` replacing its body with `new_body`.
        /// Uses `Room::make_edit_event` + `RoomSendQueue::send` so the edit
        /// is correctly formatted (Replacement relation + fallback body). Only
        /// works on own `m.text` events; returns `ok=false` for non-own or
        /// non-text events.
        fn send_edit(
            self: &ClientFfi,
            room_id: &str,
            event_id: &str,
            new_body: &str,
            formatted_body: &str,
        ) -> OpResult;

        /// Homeserver-reported maximum upload size in bytes
        /// (`/_matrix/media/v3/config`). Cached after the first successful
        /// query; returns `0` when the server does not advertise a limit, the
        /// query has not yet completed, or the client is not logged in.
        fn media_upload_limit(self: &ClientFfi) -> u64;

        /// Toggle the current user's `key` reaction on `event_id` in
        /// `room_id`. Adds the reaction when the user has not yet reacted
        /// with this key; redacts it when they have. Wraps matrix-sdk-ui's
        /// `Timeline::toggle_reaction`. Requires that `room_id` is
        /// currently subscribed via `subscribe_room`.
        fn send_reaction(
            self: &ClientFfi,
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
            self: &ClientFfi,
            room_id: &str,
            event_id: &str,
            key: &str,
            shortcode: &str,
        ) -> OpResult;

        /// Send public `m.read` and private `m.read.private` receipts for
        /// `event_id` in `room_id`. Does not require the room to be subscribed.
        fn send_read_receipt(self: &ClientFfi, room_id: &str, event_id: &str) -> OpResult;

        /// Send public `m.read` and private `m.read.private` receipts for the
        /// latest cached event in `room_id`. Used to clear the unread badge
        /// when the user opens a room. Does not require a subscription.
        fn mark_room_as_read(self: &ClientFfi, room_id: &str) -> OpResult;

        /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
        /// Wraps matrix-sdk-ui's `Timeline::redact`. Requires that the room
        /// is currently subscribed via `subscribe_room`. Server-side
        /// permission errors surface as `OpResult { ok: false, message: ... }`.
        fn redact_event(
            self: &ClientFfi,
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
            self: &ClientFfi,
            room_id: &str,
            body: &str,
            image_url: &str,
            info_json: &str,
        ) -> OpResult;

        /// Send `m.sticker` into the thread rooted at `thread_root` (MSC3440).
        /// Same semantics as `send_sticker` but emits a full `m.thread`
        /// relation so the sticker appears inside the thread timeline.
        fn send_thread_sticker(
            self: &ClientFfi,
            room_id: &str,
            thread_root: &str,
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
            self: &ClientFfi,
            shortcode: &str,
            body: &str,
            image_url: &str,
            info_json: &str,
        ) -> OpResult;

        /// True when `image_url` is already present in the user's personal
        /// pack. Used by the right-click context menu to hide the "Add to
        /// Saved Stickers" item for stickers the user has already saved.
        fn user_pack_has_sticker(self: &ClientFfi, image_url: &str, info_json: &str) -> bool;

        /// Flip the `im.tesseract.favorite` flag on the user-pack entry
        /// whose `url` matches `image_url`. No-op when the sticker isn't in
        /// the user pack (call `save_sticker_to_user_pack` first).
        fn toggle_favorite_sticker(self: &ClientFfi, image_url: &str) -> OpResult;

        // ----- Application prefs (im.gnomos.tesseract global account-data) -----

        /// Read the current `im.gnomos.tesseract` account-data event from
        /// the SDK's local sync cache. Returns the raw JSON content object,
        /// or `"{}"` when the event has never been written or the client is
        /// not logged in. No network roundtrip.
        fn load_prefs(self: &ClientFfi) -> String;

        /// Write `json` (the full content object) back to the homeserver as
        /// the `im.gnomos.tesseract` account-data event. Fire-and-forget —
        /// returns immediately; errors are silently swallowed. The next sync
        /// deliver of the event will trigger `on_account_prefs_updated`.
        fn save_prefs(self: &ClientFfi, json: &str);

        // ----- MSC4278 media-preview config (m.media_preview_config) -----

        /// Read the global MSC4278 media-preview config from the local sync
        /// cache (stable → unstable precedence). No network roundtrip;
        /// returns the MSC defaults (previews on, invite avatars on) when not
        /// logged in or before the first sync.
        fn media_preview_config(self: &ClientFfi) -> MediaPreviewConfigFfi;

        /// Read a room-level override of `media_previews` from that room's
        /// account-data. `has_media_previews` is false when the room sets no
        /// override (use the global value). No network roundtrip.
        fn room_media_preview_override(
            self: &ClientFfi,
            room_id: &str,
        ) -> MediaPreviewOverrideFfi;

        /// Write the global MSC4278 config, dual-writing the stable and
        /// unstable account-data types. Fire-and-forget; the echo arrives on
        /// the next sync and triggers `on_media_preview_config_updated`.
        fn set_media_preview_config(
            self: &ClientFfi,
            media_previews: u8,
            invite_avatars: bool,
        );

        // ----- Recent emoji (io.element.recent_emoji global account-data) -----

        /// Top-N glyphs from the user's `io.element.recent_emoji`
        /// account-data, most-used first. Reads the SDK's local sync cache;
        /// no network roundtrip. Returns an empty vector when not logged in
        /// or before the first sync has populated the cache.
        fn recent_emoji_top(self: &ClientFfi, n: u32) -> Vec<String>;

        /// Record one use of `glyph`. Fire-and-forget against the homeserver
        /// (the SDK call is GET-modify-PUT and would otherwise stall the UI).
        fn recent_emoji_bump(self: &ClientFfi, glyph: &str);

        // ----- Identity -----

        /// Returns the current user's Matrix ID (e.g. @alice:example.org), or
        /// an empty string if not logged in.
        fn user_id(self: &ClientFfi) -> String;

        /// Returns the current device ID (e.g. "ABCDEFGHIJ"), or an empty
        /// string if not logged in. Used by the Settings UI to mark the
        /// "This device" row in the sessions list.
        fn device_id(self: &ClientFfi) -> String;

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
        fn fetch_avatar_bytes(self: &ClientFfi, room_id: &str) -> Vec<u8>;

        /// Download arbitrary mxc:// media and return the raw bytes. Returns
        /// an empty Vec when the URL is invalid, media is unavailable, or the
        /// client is not logged in. The SDK media cache is consulted first so
        /// repeat calls for the same URL are instant.
        fn fetch_media_bytes(self: &ClientFfi, mxc_url: &str) -> Vec<u8>;

        /// Download media from a JSON-serialised `MediaSource` (plain mxc:// or
        /// encrypted `EncryptedFile`). Returns raw bytes on success or an
        /// empty Vec on failure (invalid JSON, decrypt error, network, etc.).
        fn fetch_source_bytes(self: &ClientFfi, source_json: &str) -> Vec<u8>;

        /// Fetch raw bytes from an arbitrary HTTP/HTTPS URL.
        /// Returns the response body on success, or an empty Vec on any error.
        /// Sets User-Agent to "Tesseract/0.1 (Matrix client)" per OSM tile policy.
        /// Capped at 1 MiB — intended for thumbnails / map tiles, not video.
        fn fetch_url_bytes(self: &ClientFfi, url: &str) -> Vec<u8>;

        /// Like `fetch_url_bytes` but capped at the full-media size (64 MiB)
        /// with the generous full-download timeout — for fetching a `/gif`
        /// picker MP4 before sending it.
        fn fetch_gif_bytes(self: &ClientFfi, url: &str) -> Vec<u8>;

        // ----- Async media downloads (non-blocking) -----

        /// Start an async media download. Spawns the fetch on the tokio runtime
        /// (so it does NOT pin a C++ worker thread for the network round-trip),
        /// bounded by a per-lane priority gate, and fires `on_media_ready(
        /// request_id, bytes)` on completion. `request_id` is a caller-owned
        /// correlation token. `group_id` groups downloads for bulk cancellation
        /// via `cancel_media_group` and scopes `prioritize_media` (0 =
        /// ungrouped/never-cancelled). `priority` orders pending fetches within a
        /// lane (see MediaPriority: 0 = normal, 1 = visible row); a higher value
        /// is granted a free slot first. `kind` selects the source shape and lane
        /// (see MEDIA_KIND_* in media.rs):
        ///   0 room-avatar thumbnail (room_id in `source`)
        ///   1 mxc thumbnail   2 source thumbnail   3 full source
        /// `w`/`h`/`animated` apply to the thumbnail kinds. Empty bytes on
        /// failure/timeout/cancel, mirroring the blocking variants.
        fn fetch_media_async(
            self: &ClientFfi,
            request_id: u64,
            group_id: u64,
            priority: u8,
            kind: u8,
            source: &str,
            w: u32,
            h: u32,
            animated: bool,
        );

        /// Raise the priority of still-pending `fetch_media_async` tasks in
        /// `group_id` whose `request_id` is in `request_ids`, so they are
        /// downloaded before lower-priority pending items. Called when rows
        /// scroll into view. No-op for already-running/finished requests (they
        /// hold a slot already) and for `group_id == 0`.
        fn prioritize_media(self: &ClientFfi, group_id: u64, request_ids: &[u64]);

        /// Abort every still-running `fetch_media_async` download registered
        /// under `group_id`. Used on room switch to drop the previous room's
        /// in-flight media so the new room's downloads get the slots.
        /// No-op for `group_id == 0` or an unknown group.
        fn cancel_media_group(self: &ClientFfi, group_id: u64);

        /// Async counterpart of `get_url_preview`: spawns the fetch and fires
        /// `on_url_preview_ready(request_id, preview_json)` on completion
        /// (an empty string on failure). Does not pin a worker thread.
        fn get_url_preview_async(self: &ClientFfi, request_id: u64, group_id: u64, url: &str);

        /// Non-blocking counterpart of `fetch_url_bytes` (map tiles). Spawns the
        /// fetch under the bulk lane and fires `on_media_ready(request_id,
        /// bytes)` on completion (empty on failure). Does not pin a thread.
        fn fetch_url_async(self: &ClientFfi, request_id: u64, group_id: u64, url: &str);

        // ----- GIF search + send -----

        /// Search the configured GIF provider (Klipy) for `query`, returning at
        /// most `limit` results via `on_gif_results(request_id, …)` (or
        /// `on_gif_search_failed`). Non-blocking; runs on a worker thread.
        /// `api_key` / `client_key` come from app settings.
        fn gif_search_async(
            self: &ClientFfi,
            request_id: u64,
            query: &str,
            api_key: &str,
            client_key: &str,
            limit: u32,
        );

        // ----- Full-text message search -----

        /// Enable or disable local message indexing for full-text search. Off
        /// by default (opt-in privacy setting). Turning it on kicks off a lazy
        /// background backfill of joined rooms; turning it off clears the index
        /// (decrypted plaintext is removed from disk). Cheap + synchronous: it
        /// only flips a flag and spawns/cleans up; safe to call from the UI
        /// thread on a settings toggle or once after login.
        fn set_search_indexing_enabled(self: &ClientFfi, enabled: bool);

        /// Full-text search the local index for `query`, returning at most
        /// `limit` hits via `on_search_results(request_id, …)` (or
        /// `on_search_failed`). A non-empty `room_id` scopes the search to one
        /// room (in-room find); empty searches the whole active account (global
        /// overlay). Non-blocking; runs on a worker thread. The C++ controller
        /// debounces input and drops stale `request_id`s.
        fn search_messages_async(
            self: &ClientFfi,
            request_id: u64,
            query: &str,
            room_id: &str,
            limit: u32,
        );

        /// Synchronous summary of the local search index (message/room counts,
        /// oldest indexed timestamp, backfill-complete flag) for the Settings
        /// panel. Cheap single-aggregate read; not for hot paths.
        /// `index_bytes` in the returned struct is always 0; use
        /// `search_index_size_bytes` for the on-disk size (dbstat walk, once on
        /// panel open only).
        fn search_index_stats(self: &ClientFfi) -> SearchIndexStats;

        /// On-disk size of the search index in bytes via the SQLite `dbstat`
        /// virtual table. This is an O(pages) B-tree walk — call only once when
        /// the Settings panel opens, not on the 2-second refresh poll.
        fn search_index_size_bytes(self: &ClientFfi) -> u64;

        /// Send a pre-fetched GIF MP4 into `room_id` as an `m.video` carrying
        /// the `fi.mau.gif` vendor hint. `thumb_bytes` is a static poster image
        /// (`thumb_mime`, e.g. `image/png`); empty to omit. MP4 + thumbnail are
        /// encrypted when the room is encrypted, plaintext otherwise. Blocks the
        /// calling thread.
        fn send_gif_video(
            self: &ClientFfi,
            room_id: &str,
            mp4_bytes: &[u8],
            mime_type: &str,
            body: &str,
            width: u32,
            height: u32,
            duration_ms: u64,
            thumb_bytes: &[u8],
            thumb_mime: &str,
            thumb_width: u32,
            thumb_height: u32,
            reply_event_id: &str,
            thread_root: &str,
        ) -> OpResult;

        // ----- MSC3266 room summary / join -----

        /// Fetch a room summary (name, topic, avatar, member count, join rule,
        /// encryption state) for any room the homeserver can see, whether or
        /// not the client is a member. Accepts a room ID (`!id:server`) or
        /// alias (`#alias:server`). Returns a JSON object on success or an
        /// empty string on error. Blocks the calling thread.
        fn get_room_summary(self: &ClientFfi, room_id_or_alias: &str) -> String;

        /// Join a room by its ID or alias. Returns the canonical room ID
        /// (e.g. `!id:server`) on success, or an empty string on failure.
        /// Blocks the calling thread — call only from a worker thread.
        fn join_room(self: &ClientFfi, room_id_or_alias: &str) -> String;

        /// Non-blocking join. Spawns the join as a tokio task and delivers
        /// the result via `on_room_action_complete(request_id, ok, joined_room_id, message)`.
        fn join_room_async(self: &ClientFfi, request_id: u64, room_id_or_alias: &str);

        /// Leave a room. Blocks the calling thread — call from a worker thread.
        fn leave_room(self: &ClientFfi, room_id: &str) -> OpResult;

        /// Non-blocking leave. Spawns the leave as a tokio task and delivers
        /// the result via `on_room_action_complete(request_id, ok, "", message)`.
        fn leave_room_async(self: &ClientFfi, request_id: u64, room_id: &str);

        /// Non-blocking invite. Spawns as a tokio task; no callback.
        fn invite_user_async(self: &ClientFfi, room_id: &str, user_id: &str);

        /// Fetch the joined member list for a room. Blocks — worker thread.
        fn get_room_members(self: &ClientFfi, room_id: &str) -> Vec<RoomMember>;

        /// Send an m.room.topic state event. Blocks — worker thread.
        fn set_room_topic(self: &ClientFfi, room_id: &str, topic: &str) -> OpResult;

        /// Append `event_id` to this room's `m.room.pinned_events` state
        /// event. No-op (returns ok) if already pinned. Blocks — worker thread.
        fn pin_event(self: &ClientFfi, room_id: &str, event_id: &str) -> OpResult;

        /// Remove `event_id` from this room's `m.room.pinned_events` state
        /// event. No-op (returns ok) if not pinned. Blocks — worker thread.
        fn unpin_event(self: &ClientFfi, room_id: &str, event_id: &str) -> OpResult;

        /// True iff the current user has permission to send
        /// `m.room.pinned_events` state events in this room. Reads cached
        /// power levels — no network round-trip. False on any uncertainty.
        fn can_pin_in_room(self: &ClientFfi, room_id: &str) -> bool;

        /// Add user_id to m.ignored_user_list account data. Blocks — worker thread.
        fn ignore_user(self: &ClientFfi, user_id: &str) -> OpResult;

        /// Remove user_id from m.ignored_user_list. Blocks — worker thread.
        fn unignore_user(self: &ClientFfi, user_id: &str) -> OpResult;

        /// Set the current user's display name. Blocks — worker thread.
        fn set_display_name(self: &ClientFfi, name: &str) -> OpResult;

        /// Upload an avatar for the current user. `bytes` is the image payload
        /// (PNG/JPEG/etc.); `mime_type` specifies the image format. Blocks — worker thread.
        fn upload_avatar(self: &ClientFfi, bytes: &[u8], mime_type: &str) -> OpResult;

        /// Upload bytes to the media server; returns the mxc:// URI in OpResult.message.
        /// Does NOT change the user's global profile avatar. Blocks — worker thread.
        fn upload_media(self: &ClientFfi, bytes: &[u8], mime_type: &str) -> OpResult;

        /// Remove the current user's avatar. Blocks — worker thread.
        fn remove_avatar(self: &ClientFfi) -> OpResult;

        /// Set the current user's display name in a specific room
        /// (m.room.member state event). Blocks — worker thread.
        fn set_room_display_name(
            self: &ClientFfi,
            room_id: &str,
            name: &str,
        ) -> OpResult;

        /// Set the current user's avatar in a specific room
        /// (m.room.member state event). Blocks — worker thread.
        fn set_room_avatar(
            self: &ClientFfi,
            room_id: &str,
            mxc_uri: &str,
        ) -> OpResult;

        // ----- Devices / sessions -----

        /// Fetch the user's full device list from the homeserver. Each entry
        /// is cross-referenced with the local crypto store for verification
        /// state. The current device sorts first; other devices follow in
        /// descending `last_seen_ts` order. Returns empty on error or when
        /// not logged in. Blocks — call from a worker thread.
        fn list_devices(self: &ClientFfi) -> Vec<DeviceFfi>;

        /// Rename a device on the homeserver (no UIA required). `device_id`
        /// must belong to the current user. Blocks — worker thread.
        fn set_device_display_name(
            self: &ClientFfi,
            device_id: &str,
            name: &str,
        ) -> OpResult;

        /// Begin deleting a device. If the homeserver returns a UIA challenge
        /// (which it always does on a fresh request for `/devices/{id}`), the
        /// returned `DeleteDeviceBegin` carries `needs_uia=true` plus the
        /// fallback URL and session id the UI uses to complete auth in a
        /// browser. Pass the session back to `complete_delete_device` once
        /// the user has authenticated. Blocks — worker thread.
        fn begin_delete_device(
            self: &ClientFfi,
            device_id: &str,
        ) -> DeleteDeviceBegin;

        /// Retry a device deletion after the user has completed UIA in a
        /// browser. `session` is the value returned by `begin_delete_device`.
        /// Blocks — worker thread.
        fn complete_delete_device(
            self: &ClientFfi,
            device_id: &str,
            session: &str,
        ) -> OpResult;

        // ----- Presence -----

        /// Publish the current user's Matrix presence via
        /// `PUT /presence/{userId}/status`. `state` is the same 1/2/3 wire
        /// encoding the receive side uses: 1=Online, 2=Unavailable, 3=Offline.
        /// Any other value is rejected with `ok=false`. Blocks — worker thread.
        fn set_presence(self: &ClientFfi, state: u8) -> OpResult;

        /// Enable or disable background presence polling of DM counterparts.
        /// When disabled the polling loop skips every tick; already-received
        /// presence values are left unchanged. Thread-safe; may be called on
        /// any thread.
        fn set_presence_polling_enabled(self: &ClientFfi, enabled: bool);

        /// Issue one immediate round of DM presence polls without waiting
        /// for the 60s interval. Used by the UI shell when the window
        /// returns to focus so contacts don't appear stale after un-minimize.
        /// No-op if sync isn't running or polling is disabled. Thread-safe.
        fn poll_presence_now(self: &mut ClientFfi);

        /// Return room ID of an existing DM with user_id, or create one.
        /// Returns empty string on error. Blocks — worker thread.
        fn get_or_create_dm(self: &ClientFfi, user_id: &str) -> String;

        /// Resolve a user's profile by mxid to confirm the user exists and
        /// fetch their display name / avatar. `exists` is false on a parse
        /// error or when the homeserver has no profile for the mxid.
        /// Blocks — worker thread.
        fn resolve_user_profile(self: &ClientFfi, user_id: &str) -> UserProfile;

        /// Discover the homeserver URL for a server name or Matrix ID.
        /// Accepts "matrix.org", "@user:matrix.org", or "https://matrix.org".
        /// Returns JSON: `{"base_url":"https://...","error":""}` on success or
        /// `{"base_url":"","error":"message"}` on failure.
        /// Blocks the calling thread — call only from a worker thread.
        fn discover_homeserver(self: &ClientFfi, server_name_or_mxid: &str) -> String;

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

        /// Returns the current recovery state as a u8:
        /// 0 = Unknown, 1 = Disabled, 2 = Enabled, 3 = Incomplete.
        fn recovery_state(self: &ClientFfi) -> u8;

        /// Whether a cross-signing identity already exists for our user
        /// (local crypto-store read, no network). Distinguishes a pristine
        /// account from one whose identity was set up on another device.
        fn own_identity_exists(self: &ClientFfi) -> bool;

        /// Whether this device is currently cross-signed / verified
        /// (synchronous local read of the verification state).
        fn device_verified(self: &ClientFfi) -> bool;

        /// Whether the cross-signing PRIVATE keys are present locally (this
        /// device bootstrapped the identity, or has recovered it). Unlike
        /// `device_verified()` this does not depend on the verification state
        /// having settled, so the UI can reliably tell a fresh first device
        /// (keys present → Fresh setup) from a foreign identity synced from
        /// another device (keys absent → Recover).
        fn have_cross_signing_keys(self: &ClientFfi) -> bool;

        /// Unlock the server-side secret storage with a recovery key or
        /// passphrase, import the cross-signing private keys + backup
        /// decryption key into this device, and start downloading historical
        /// room keys. Returns once the SDK reports a steady-state backup, or
        /// with an error if the key is wrong / no secret storage exists.
        fn recover(self: &ClientFfi, key_or_passphrase: &str) -> OpResult;

        /// Bootstrap cross-signing and key backup for a fresh account.
        /// Empty `passphrase` → generate a random recovery key.
        /// Non-empty → derive key from passphrase (reported key is "").
        /// Progress events fire via `on_enable_recovery_progress` before return.
        fn enable_recovery(self: &ClientFfi, passphrase: &str) -> OpResult;

        /// Current snapshot of the backup state and import counters.
        fn backup_state(self: &ClientFfi) -> BackupProgress;

        /// Export all Megolm room keys to a passphrase-encrypted file at
        /// `path` (standard Matrix key-export format). Blocks — worker thread.
        fn export_room_keys(self: &ClientFfi, path: &str, passphrase: &str) -> OpResult;

        /// Import Megolm room keys from the passphrase-encrypted file at
        /// `path` (standard Matrix key-export format). Blocks — worker thread.
        fn import_room_keys(self: &ClientFfi, path: &str, passphrase: &str) -> OpResult;

        /// Begin resetting the user's cross-signing identity (Element's "Reset
        /// cryptographic identity"). Uploads a fresh cross-signing identity;
        /// on OAuth/OIDC servers the user must approve in a browser. Returns
        /// quickly: if `needs_approval` is set, open `approval_url` and await
        /// the `on_crypto_reset_result` callback (the SDK polls in the
        /// background, so this does NOT block). Destructive — other sessions
        /// and contacts must re-verify afterward.
        fn begin_reset_crypto_identity(self: &mut ClientFfi) -> CryptoResetBegin;

        /// Abort an in-progress cross-signing reset started by
        /// `begin_reset_crypto_identity` (before `on_crypto_reset_result`
        /// fires). No-op when no reset is pending.
        fn cancel_reset_crypto_identity(self: &mut ClientFfi);

        // ----- SAS device verification -----

        /// Send an `m.key.verification.request` to-device event to every other
        /// device of the current user. When one accepts, `on_verification_request`
        /// fires with `incoming = false` and the UI should call `start_sas`.
        fn request_self_verification(self: &ClientFfi) -> OpResult;

        /// Accept an incoming verification request identified by `flow_id`.
        /// Call after receiving `on_verification_request(incoming=true)`.
        fn accept_verification(self: &ClientFfi, flow_id: &str) -> OpResult;

        /// Start the SAS key-exchange on a ready request. The SDK will fire
        /// `on_sas_ready` with the 7 emoji once both sides have exchanged keys.
        fn start_sas(self: &ClientFfi, flow_id: &str) -> OpResult;

        /// Confirm that the emoji shown on this device match the other device's
        /// display. Fires `on_verification_done` when both sides confirm.
        fn confirm_sas(self: &ClientFfi, flow_id: &str) -> OpResult;

        /// Cancel or decline a verification flow (e.g. emoji mismatch or user
        /// dismissed). Fires `on_verification_cancelled` on both sides.
        fn cancel_verification(self: &ClientFfi, flow_id: &str) -> OpResult;

        /// Return the 7 SAS emoji for `flow_id` after `on_sas_ready` has fired.
        /// Returns an empty Vec before the key exchange completes.
        fn get_sas_emojis(self: &ClientFfi, flow_id: &str) -> Vec<VerificationEmoji>;

        // ----- Server pushers (Step 12) -----

        /// Register (or update) an HTTP pusher on the homeserver.
        /// `pushkey` uniquely identifies this pusher instance (e.g. sanitised
        /// Matrix user ID + hostname). `endpoint_url` is the push gateway URL
        /// provided by the UnifiedPush distributor.
        fn register_pusher(
            self: &ClientFfi,
            pushkey: &str,
            app_id: &str,
            app_display_name: &str,
            device_display_name: &str,
            endpoint_url: &str,
            lang: &str,
        ) -> OpResult;

        /// Remove a pusher from the homeserver by `pushkey` / `app_id`.
        fn remove_pusher(self: &ClientFfi, pushkey: &str, app_id: &str) -> OpResult;

        /// Hint that a push notification arrived for `room_id`. Requests a
        /// targeted sliding-sync subscription for that room (union with any
        /// already-open rooms) so the next sync cycle delivers fresh state
        /// before the regular sync loop catches up.
        fn hint_push_room(self: &ClientFfi, room_id: &str) -> OpResult;

        // ----- Per-room notification mode -----

        /// Return the effective notification mode for a room from the local
        /// push-rules cache. Returns "default" | "all" | "mentions" | "off".
        /// Blocks — call from a worker thread.
        fn get_room_notification_mode(self: &ClientFfi, room_id: &str) -> String;

        /// Set the per-room notification mode by writing push rules to the
        /// homeserver. `mode` must be "default" | "all" | "mentions" | "off".
        /// Fire-and-forget; errors are logged. Blocks — worker thread.
        fn set_room_notification_mode(self: &ClientFfi, room_id: &str, mode: &str);

        // ----- Per-room tags (favourite / low priority) -----

        /// Add or remove the `m.favourite` tag for a room. Setting it removes
        /// `m.lowpriority` if present (mutually exclusive). Fire-and-forget.
        /// Blocks — call from a worker thread.
        fn set_room_favourite(self: &ClientFfi, room_id: &str, value: bool);

        /// Add or remove the `m.lowpriority` tag for a room. Setting it removes
        /// `m.favourite` if present (mutually exclusive). Fire-and-forget.
        /// Blocks — call from a worker thread.
        fn set_room_low_priority(self: &ClientFfi, room_id: &str, value: bool);

        // ----- Session teardown -----

        fn logout(self: &mut ClientFfi) -> OpResult;
    }
}

// cxx-bridge shared structs cannot use `#[derive(Clone)]`, but the sync
// watcher in `client::sync` needs to clone `RoomInfo` out of a cache when
// emitting the room-list snapshot to the UI. All fields are `String` / `bool`
// / `u64` — the field-by-field copy below has the same semantics as a derived
// `Clone`.
impl Clone for ffi::RoomInfo {
    fn clone(&self) -> Self {
        Self {
            id: self.id.clone(),
            name: self.name.clone(),
            topic: self.topic.clone(),
            topic_html: self.topic_html.clone(),
            notification_count: self.notification_count,
            highlight_count: self.highlight_count,
            unread_count: self.unread_count,
            muted: self.muted,
            is_direct: self.is_direct,
            avatar_url: self.avatar_url.clone(),
            dm_avatar_url: self.dm_avatar_url.clone(),
            dm_counterpart_user_id: self.dm_counterpart_user_id.clone(),
            last_message_body: self.last_message_body.clone(),
            last_message_sender_name: self.last_message_sender_name.clone(),
            last_message_kind: self.last_message_kind.clone(),
            last_message_sticker_url: self.last_message_sticker_url.clone(),
            last_message_thumbnail_url: self.last_message_thumbnail_url.clone(),
            last_activity_ts: self.last_activity_ts,
            is_space: self.is_space,
            is_favorite: self.is_favorite,
            is_low_priority: self.is_low_priority,
            is_encrypted: self.is_encrypted,
            history_visibility: self.history_visibility.clone(),
            pinned_events: self.pinned_events.clone(),
            canonical_alias: self.canonical_alias.clone(),
        }
    }
}

// PinnedEvent is held inside `RoomInfo.pinned_events: Vec<PinnedEvent>`, so
// it must implement Clone for the RoomInfo Clone impl above to compile.
impl Clone for ffi::PinnedEvent {
    fn clone(&self) -> Self {
        Self {
            event_id: self.event_id.clone(),
            sender_name: self.sender_name.clone(),
            body_preview: self.body_preview.clone(),
            timestamp: self.timestamp,
        }
    }
}
