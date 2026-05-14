#pragma once
#include "event_handler.h"
#include "image_pack.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward-declare the Rust opaque type so we don't need to pull in cxx headers
// in every translation unit that just uses this interface.
namespace tesseract_ffi { class ClientFfi; }

namespace tesseract {

/// Result of a client operation.
struct Result {
    bool        ok      = false;
    std::string message;

    explicit operator bool() const noexcept { return ok; }
};

/// Result of `Client::paginate_back_with_status`. `reached_start` is true
/// when matrix-sdk-ui reports the timeline has no further history to load.
/// UIs latch their scroll-up trigger off this signal.
struct PaginateResult {
    bool        ok            = false;
    std::string message;
    bool        reached_start = false;

    explicit operator bool() const noexcept { return ok; }
};

/// High-level C++ Matrix client.
///
/// Thread-safety: methods may be called from any thread; the underlying Rust
/// runtime serialises concurrent calls.  Event callbacks arrive on a background
/// thread – marshal them to the UI thread as required.
class Client {
public:
    Client();
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&)                 noexcept;
    Client& operator=(Client&&)      noexcept;

    // ------------------------------------------------------------------
    // Data directory
    // ------------------------------------------------------------------

    /// Override the matrix-sdk SQLite store path for this client. Must be
    /// called before `begin_oauth` / `restore_session`. Used by the multi-
    /// account host to scope each account's store under
    /// `<config>/tesseract/accounts/<sanitized-uid>/matrix-store/`. Passing
    /// an empty path is a no-op (the client keeps the default location).
    void set_data_dir(const std::string& path);

    // ------------------------------------------------------------------
    // Authentication (OAuth 2.0 / Matrix Authentication Service)
    // ------------------------------------------------------------------

    struct OAuthFlow {
        bool        ok = false;
        std::string message;
        std::string auth_url;
        std::string redirect_uri;

        explicit operator bool() const noexcept { return ok; }
    };

    OAuthFlow   begin_oauth(const std::string& homeserver);
    Result      await_oauth();
    void        cancel_oauth();

    static bool open_in_browser(const std::string& url);

    Result      restore_session(const std::string& session_json);
    std::string export_session() const;
    Result      logout();

    // ------------------------------------------------------------------
    // Sync loop
    // ------------------------------------------------------------------

    void start_sync(IEventHandler* handler);
    void stop_sync();

    // ------------------------------------------------------------------
    // Room list (utility — push pipeline drives UI updates via callbacks)
    // ------------------------------------------------------------------

    std::vector<RoomInfo> list_rooms() const;

    // ------------------------------------------------------------------
    // Timeline subscription (Step 2)
    // ------------------------------------------------------------------

    /// Subscribe to a room's timeline. Emits on_timeline_reset then
    /// on_message for cached items, followed by live on_message callbacks.
    /// Call paginate_back() immediately after to load message history.
    Result subscribe_room(const std::string& room_id);

    /// Unsubscribe from a room's timeline and cancel its background task.
    void   unsubscribe_room(const std::string& room_id);

    /// Paginate backwards in a subscribed room. Older history arrives via
    /// `IEventHandler::on_message_prepended`.
    Result paginate_back(const std::string& room_id, std::uint16_t count);

    /// Like `paginate_back` but also reports whether the timeline has
    /// reached its first event. UIs use `reached_start` to disable the
    /// scroll-up pagination trigger once no more history can be fetched.
    PaginateResult paginate_back_with_status(const std::string& room_id,
                                             std::uint16_t count);

    /// Kick off a background pass that paginates the given rooms (those
    /// currently visible in the room list), up to ~50 events each, with
    /// bounded concurrency. Idempotent — safe to call from every room-open
    /// path. Silent: no event callbacks fire for the rooms it visits; only
    /// the SDK's persistent event cache is warmed.
    Result start_background_backfill(const std::vector<std::string>& room_ids);

    /// Cancel an in-progress background backfill (also called automatically
    /// on stop_sync / destruction). No-op if none is running.
    void   stop_background_backfill();

    // ------------------------------------------------------------------
    // Messaging
    // ------------------------------------------------------------------

    Result send_message(const std::string& room_id, const std::string& body);

    /// Send a typing notice to `room_id`. Fire-and-forget — returns immediately;
    /// errors are silently swallowed. Pass `typing=true` when the compose field
    /// transitions to non-empty, `typing=false` when it clears or on room leave.
    void send_typing_notice(const std::string& room_id, bool typing);

    /// Send an image to `room_id`. `bytes` is the already-encoded image
    /// payload (PNG/JPEG/etc. — identified by `mime_type`, e.g. "image/png"
    /// or "image/jpeg"). `filename` is the user-visible file name shown in
    /// the timeline (e.g. "clipboard-20260512-141503.png").
    ///
    /// When `caption` is non-empty, the resulting `m.image` event follows
    /// MSC2530: `body` is the caption and the dedicated `filename` field
    /// carries the file name. When `caption` is empty, `body` is the
    /// filename and no MSC2530 `filename` field is emitted (legacy fallback).
    ///
    /// `width`/`height` populate `info.{w,h}`; pass 0 when unknown. The
    /// SDK uploads the bytes via the homeserver media repository (transparent
    /// encryption in E2EE rooms) before posting the event.
    Result send_image(const std::string& room_id,
                      const std::vector<uint8_t>& bytes,
                      const std::string& mime_type,
                      const std::string& filename,
                      const std::string& caption,
                      std::uint32_t width,
                      std::uint32_t height,
                      const std::string& reply_event_id = "");

    /// Send an arbitrary file to `room_id` as an `m.file` event. `bytes` is
    /// the raw file payload (no re-encoding); `mime_type` is best-effort —
    /// "application/octet-stream" is acceptable when unknown. When `caption`
    /// is non-empty the event follows MSC2530 framing (`body` = caption,
    /// dedicated `filename` field carries the file name). E2EE rooms are
    /// handled transparently by matrix-sdk.
    Result send_file(const std::string& room_id,
                     const std::vector<uint8_t>& bytes,
                     const std::string& mime_type,
                     const std::string& filename,
                     const std::string& caption,
                     const std::string& reply_event_id = "");

    /// Homeserver-reported maximum upload size, in bytes. Cached after the
    /// first successful call. Returns 0 when not yet fetched, the server
    /// doesn't advertise a limit, or the client is not logged in. UIs use
    /// this to reject oversize attachments locally rather than waiting for
    /// a long upload to fail server-side.
    std::uint64_t media_upload_limit();

    /// Send public `m.read` and private `m.read.private` receipts for
    /// `event_id` in `room_id`. Blocks until acknowledged. Does not require
    /// `subscribe_room`.
    Result send_read_receipt(const std::string& room_id,
                             const std::string& event_id);

    /// Send public `m.read` and private `m.read.private` receipts for the
    /// latest cached event in `room_id`. Clears the unread count. Does not
    /// require `subscribe_room`.
    Result mark_room_as_read(const std::string& room_id);

    /// Toggle the current user's `key` reaction on `event_id` in `room_id`.
    /// First call adds the reaction; second redacts it. Requires that the
    /// room is currently subscribed via `subscribe_room`. `key` may be a
    /// Unicode emoji (e.g. "👍") or, in a future MSC 4027 send pass, a
    /// shortcode like ":partyparrot:".
    Result send_reaction(const std::string& room_id,
                         const std::string& event_id,
                         const std::string& key);

    /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
    /// Requires that the room is currently subscribed via `subscribe_room`.
    /// Server-side permission errors (e.g. lacking power to redact someone
    /// else's event) surface in `Result::message`. On success the SDK
    /// re-emits the timeline item as a redacted tombstone, which the UI
    /// receives via the existing `on_message_event` re-render path.
    Result redact_event(const std::string& room_id,
                        const std::string& event_id,
                        const std::string& reason = "");

    /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds
    /// the `m.in_reply_to` relation. Does not require `subscribe_room`.
    Result send_reply(const std::string& room_id,
                      const std::string& event_id,
                      const std::string& body);

    /// Request async resolution of the replied-to event whose ID is
    /// `event_id` in `room_id`. Requires `subscribe_room`. Returns
    /// immediately; when the SDK resolves the event it fires
    /// `on_message_updated` for every timeline item referencing it, so
    /// the UI re-paints the quote block with the sender name and snippet.
    Result fetch_reply_details(const std::string& room_id,
                               const std::string& event_id);

    /// Edit `event_id` in `room_id` replacing its body with `new_body`.
    /// Only works on own `m.text` events. Does not require `subscribe_room`.
    Result send_edit(const std::string& room_id,
                     const std::string& event_id,
                     const std::string& new_body);

    // ------------------------------------------------------------------
    // Application prefs ("im.gnomos.tesseract" global account-data)
    // ------------------------------------------------------------------

    /// Read the raw JSON content of the `im.gnomos.tesseract` account-data
    /// event from the SDK's local sync cache. Returns `"{}"` when the event
    /// has never been written or the client is not logged in. No network
    /// roundtrip. Not const: drives the tokio runtime via `block_on`.
    std::string load_prefs_json();

    /// Write `json` (full content object) to the homeserver as the
    /// `im.gnomos.tesseract` account-data event. Fire-and-forget — returns
    /// immediately; errors are silently swallowed.
    void save_prefs_json(const std::string& json);

    // Recent emoji ("io.element.recent_emoji" global account-data)
    // ------------------------------------------------------------------

    /// Top-N glyphs from the user's `io.element.recent_emoji` account-data,
    /// most-used first. Reads the SDK's local sync cache; cheap, no
    /// network roundtrip. Returns an empty vector when not logged in or
    /// when the user has never picked an emoji on any device.
    ///
    /// Not const: the underlying SDK call drives the tokio runtime via
    /// `block_on`, mirroring `send_reaction` and `redact_event`.
    std::vector<std::string> recent_emoji_top(std::uint32_t n);

    /// Record one use of `glyph`. Fire-and-forget — the SDK's
    /// `add_recent_emoji` does a GET-modify-PUT against the homeserver,
    /// so we run it on the background runtime and return immediately.
    void recent_emoji_bump(const std::string& glyph);

    // ------------------------------------------------------------------
    // Identity
    // ------------------------------------------------------------------

    /// Returns the current user's Matrix ID, e.g. @alice:example.org.
    /// Returns an empty string when not logged in.
    std::string get_user_id() const;

    /// Returns the current user's display name as set on the homeserver, or
    /// an empty string if none is set / not logged in / the fetch fails.
    /// The result is cached by the SDK after the first call.
    std::string get_display_name() const;

    /// Returns the mxc:// URI of the current user's avatar, or an empty
    /// string if none is set / not logged in / the fetch fails. Pair with
    /// `fetch_media_bytes(mxc_url)` to render. Cached by the SDK.
    std::string get_avatar_url() const;

    // ------------------------------------------------------------------
    // Media
    // ------------------------------------------------------------------

    /// Download the avatar image for the given room and return the raw bytes
    /// (JPEG or PNG). Returns an empty vector when no avatar is set or on
    /// error. The SDK's SQLite media cache is consulted first, so repeat
    /// calls for the same avatar are instant.
    std::vector<uint8_t> fetch_avatar_bytes(const std::string& room_id);

    /// Download arbitrary mxc:// media and return the raw bytes. Returns an
    /// empty vector when the URL is invalid, the media is unavailable, or the
    /// client is not logged in. The SDK's SQLite media cache is consulted
    /// first, so repeat calls for the same URL are instant.
    std::vector<uint8_t> fetch_media_bytes(const std::string& mxc_url);

    /// Download media from either a plain mxc:// URI or a JSON-serialised
    /// `MediaSource` (an opaque token that may carry an `EncryptedFile` for
    /// MSC2545 stickers and encrypted images). The two forms are detected by
    /// the leading `mxc://` prefix. Returns an empty vector on any failure.
    std::vector<uint8_t> fetch_source_bytes(const std::string& source);

    // ------------------------------------------------------------------
    // MSC2545 image packs (Step 8)
    // ------------------------------------------------------------------

    /// Snapshot of every image pack the client knows about — the user's
    /// personal pack plus every globally-enabled room pack. Reads the local
    /// cache only. The cache is refreshed by the sync watcher; the UI is
    /// notified via `IEventHandler::on_image_packs_updated`.
    std::vector<ImagePack> list_image_packs() const;

    /// Return every image entry in `pack_id` whose usage intersects
    /// `filter`. Use `PackUsageFilter::Sticker` for the StickerPicker and
    /// `PackUsageFilter::Emoticon` for the EmojiPicker's custom tab.
    std::vector<ImagePackImage> list_pack_images(const std::string& pack_id,
                                                  PackUsageFilter filter) const;

    /// Flatten every favourite-marked entry across all packs. Sticker-only;
    /// emoticon favourites are out of scope.
    std::vector<ImagePackImage> list_favorite_stickers() const;

    /// Send `m.sticker` to `room_id`. `body` is the fallback description;
    /// `image_url` is the mxc:// URI; `info_json` is the literal MSC2545
    /// `info` object (`"{}"` is fine). matrix-sdk handles E2EE rooms
    /// transparently.
    Result send_sticker(const std::string& room_id,
                        const std::string& body,
                        const std::string& image_url,
                        const std::string& info_json);

    /// Add a sticker to the user's MSC2545 personal pack
    /// (`im.ponies.user_emotes`). Creates the pack on first use with
    /// display_name "Saved Stickers". Shortcodes collide-resolve by
    /// numeric suffix. GET-modify-PUT against the homeserver; the local
    /// pack cache refreshes once the server round-trip completes.
    Result save_sticker_to_user_pack(const std::string& shortcode,
                                     const std::string& body,
                                     const std::string& image_url,
                                     const std::string& info_json);

    /// True when `image_url` is already in the user's personal pack. Used
    /// by the right-click context menu to suppress "Add to Saved Stickers"
    /// for stickers the user has already saved.
    bool user_pack_has_sticker(const std::string& image_url) const;

    /// Flip the `im.tesseract.favorite` flag on the user-pack entry whose
    /// `url` matches `image_url`. No-op when the sticker isn't in the user
    /// pack — call `save_sticker_to_user_pack` first.
    Result toggle_favorite_sticker(const std::string& image_url);

    // ------------------------------------------------------------------
    // Spaces
    // ------------------------------------------------------------------

    /// Returns the room IDs of all direct children of a space (via
    /// `m.space.child` state events). Only returns IDs of rooms the
    /// client is a member of. Returns an empty vector when not logged in
    /// or when `space_id` is not a known space.
    std::vector<std::string> space_children(const std::string& space_id) const;

    // ------------------------------------------------------------------
    // Recovery / key backup (Step 6)
    // ------------------------------------------------------------------

    /// True when this device is missing the cross-signing / backup secrets
    /// that already exist in server-side secret storage. The UI should
    /// surface a "Verify this device" banner when this returns true.
    bool needs_recovery() const;

    /// Unlock server-side secret storage with the user's recovery key (or
    /// passphrase), importing the cross-signing private keys + backup
    /// decryption key into this device. Returns once the SDK has imported
    /// the secrets; the historical key download continues asynchronously
    /// and is reported via `IEventHandler::on_backup_progress`.
    Result recover(const std::string& key_or_passphrase);

    /// Current snapshot of the backup state and the running imported-key
    /// counter. Cheap; reads atomics populated by the sync watcher.
    BackupProgress backup_state() const;

    // ------------------------------------------------------------------
    // Cross-signing / SAS device verification
    // ------------------------------------------------------------------

    /// Send a to-device `m.key.verification.request` to the current user's
    /// other devices. On success, `IEventHandler::on_verification_request`
    /// fires on the remote device; locally, a request-watcher is started and
    /// `on_verification_request(incoming=false)` fires when the remote accepts.
    Result request_self_verification();

    /// Accept an incoming verification request identified by `flow_id`.
    /// Call this when the UI is in IncomingRequest state and the user taps
    /// Accept. After accepting, call `start_sas` to begin key exchange.
    Result accept_verification(const std::string& flow_id);

    /// Begin the SAS key exchange for `flow_id`. Call after accepting an
    /// incoming request (or immediately after the remote accepts an outgoing
    /// one via `on_verification_request(incoming=false)`). When the keys are
    /// exchanged `IEventHandler::on_sas_ready` fires with the 7 emoji.
    Result start_sas(const std::string& flow_id);

    /// Confirm that the 7 SAS emoji match what the other device shows.
    /// Both sides must call this for verification to complete.
    Result confirm_sas(const std::string& flow_id);

    /// Cancel an in-progress verification flow (mismatch or user abort).
    /// Covers both the request phase and the SAS phase.
    Result cancel_verification(const std::string& flow_id);

    /// Return the cached 7 SAS emoji for `flow_id` after
    /// `IEventHandler::on_sas_ready` has fired. Returns an empty vector
    /// if the flow has no emoji yet (call too early) or is unknown.
    std::vector<VerificationEmoji> get_sas_emojis(const std::string& flow_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tesseract
