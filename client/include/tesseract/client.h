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
namespace tesseract_ffi
{
class ClientFfi;
}

namespace tesseract
{

/// Result of a client operation.
struct Result
{
    bool ok = false;
    std::string message;

    explicit operator bool() const noexcept
    {
        return ok;
    }
};

/// Result of `Client::paginate_back_with_status` and `paginate_forward`.
/// `reached_start` is true when matrix-sdk-ui reports the timeline has no
/// further history to load. `reached_end` is true when a focused timeline
/// has caught up to the live end. UIs latch their scroll triggers off these.
struct PaginateResult
{
    bool ok = false;
    std::string message;
    bool reached_start = false;
    bool reached_end = false;

    explicit operator bool() const noexcept
    {
        return ok;
    }
};

/// A parsed Matrix spec version string (e.g. "v1.6" → {1, 6}).
struct SpecVersion
{
    int major = 0;
    int minor = 0;

    bool at_least(int maj, int min) const noexcept
    {
        return major > maj || (major == maj && minor >= min);
    }
};

/// Information about the connected homeserver fetched after login.
/// Bool capability fields default to `true` (permissive) when absent —
/// old servers omit capabilities they support.
struct ServerInfo
{
    std::string homeserver_url;
    std::vector<SpecVersion> spec_versions;  ///< parsed from e.g. ["v1.1","v1.6"]
    bool supports_msc3030    = false;        ///< Jump-to-Date; true if server advertises MSC3030 or spec >= v1.6
    bool can_change_password = true;
    bool can_set_displayname = true;
    bool can_set_avatar      = true;
    std::string default_room_version;        ///< e.g. "10"; empty when absent

    /// Parse from the JSON blob returned by `Client::get_server_info()`.
    /// Missing/malformed fields use the defaults above.
    static ServerInfo from_json(const std::string& json);
};

/// High-level C++ Matrix client.
///
/// Thread-safety: methods may be called from any thread; the underlying Rust
/// runtime serialises concurrent calls.  Event callbacks arrive on a background
/// thread – marshal them to the UI thread as required.
class Client
{
public:
    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    // Movable. A moved-from Client is in the standard "valid but unspecified,
    // do not use" state: its members dereference an internal pimpl that is
    // null after a move, so calling any method other than the destructor or
    // move-assignment on a moved-from instance is undefined. Do not call
    // methods on a Client after moving out of it.
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

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
    // Server information
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // Authentication (OAuth 2.0 / Matrix Authentication Service)
    // ------------------------------------------------------------------

    struct OAuthFlow
    {
        bool ok = false;
        std::string message;
        std::string auth_url;
        std::string redirect_uri;

        explicit operator bool() const noexcept
        {
            return ok;
        }
    };

    struct DiscoveryResult
    {
        bool ok = false; ///< false when the server is unreachable
        std::string
            base_url; ///< resolved base URL, e.g. "https://matrix-client.matrix.org"
        std::string error; ///< human-readable error when ok==false

        explicit operator bool() const noexcept
        {
            return ok;
        }
    };

    /// Discover the homeserver URL for a server name or Matrix ID.
    /// Accepts: "matrix.org", "@user:matrix.org", "https://matrix.org".
    /// Fetches .well-known/matrix/client and validates the result.
    /// Does not require the client to be logged in.
    /// Blocks the calling thread — invoke only from a worker thread.
    DiscoveryResult discover_homeserver(const std::string& server_name_or_mxid);

    /// Begin OAuth login, or account registration when `register_account` is
    /// true (OIDC prompt=create). Returns the auth URL to open in a browser.
    OAuthFlow begin_oauth(const std::string& homeserver,
                          bool register_account = false);

    /// Best-effort: does `homeserver` advertise OIDC registration support?
    /// Blocks the calling thread — invoke from a worker thread.
    bool homeserver_supports_registration(const std::string& homeserver);

    Result await_oauth();
    void cancel_oauth();

    static bool open_in_browser(const std::string& url);

    Result restore_session(const std::string& session_json);
    std::string export_session() const;

    /// Fetch homeserver spec versions and enabled capabilities.
    /// Blocks the calling thread — must be called from a worker thread.
    /// Returns a default-constructed `ServerInfo` on network error or when
    /// not logged in.
    tesseract::ServerInfo get_server_info() const;

    Result logout();

    // ------------------------------------------------------------------
    // Sync loop
    // ------------------------------------------------------------------

    void start_sync(IEventHandler* handler);
    void stop_sync();
    /// Clear non-crypto SDK caches (event cache + state store file).
    /// Call after stop_sync() and before restore_session().
    Result clear_caches();

    // ------------------------------------------------------------------
    // Room list (utility — push pipeline drives UI updates via callbacks)
    // ------------------------------------------------------------------

    std::vector<RoomInfo> list_rooms() const;

    // ------------------------------------------------------------------
    // Invitations
    // ------------------------------------------------------------------

    /// Snapshot of all pending room invitations. Reads the local SDK cache —
    /// no network roundtrip. The list is refreshed whenever
    /// `IEventHandler::on_invites_updated` fires.
    std::vector<InviteInfo> list_invites() const;

    /// Accept the pending invitation to `room_id`.
    /// Blocks the calling thread — call from a worker thread.
    Result accept_invite(const std::string& room_id);

    /// Decline the pending invitation to `room_id`.
    /// Blocks the calling thread — call from a worker thread.
    Result decline_invite(const std::string& room_id);

    /// Decline a room invitation and ignore the inviter. Calls leave() then
    /// ignores `inviter_user_id` via m.ignored_user_list account data.
    /// Blocks the calling thread — call from a worker thread.
    Result block_invite(const std::string& room_id,
                        const std::string& inviter_user_id);

    // ------------------------------------------------------------------
    // Timeline subscription (Step 2)
    // ------------------------------------------------------------------

    /// Subscribe to a room's timeline. Emits on_timeline_reset then
    /// on_message for cached items, followed by live on_message callbacks.
    /// Call paginate_back() immediately after to load message history.
    Result subscribe_room(const std::string& room_id);

    /// Unsubscribe from a room's timeline and cancel its background task.
    void unsubscribe_room(const std::string& room_id);

    /// Paginate backwards in a subscribed room. Older history arrives via
    /// `IEventHandler::on_message_prepended`.
    Result paginate_back(const std::string& room_id, std::uint16_t count);

    /// Like `paginate_back` but also reports whether the timeline has
    /// reached its first event. UIs use `reached_start` to disable the
    /// scroll-up pagination trigger once no more history can be fetched.
    PaginateResult paginate_back_with_status(const std::string& room_id,
                                             std::uint16_t count);

    /// MSC3030 Jump to Date: resolve a Unix millisecond timestamp to the
    /// nearest event ID in `room_id`. `dir` is `"f"` (forward — first event
    /// ≥ ts) or `"b"` (backward — last event ≤ ts). On success
    /// `Result::message` holds the resolved event ID; on failure it holds
    /// the error description.
    Result timestamp_to_event(const std::string& room_id, uint64_t ts_ms,
                              const std::string& dir);

    /// MSC3030 Jump to Date: subscribe to a room's timeline focused on a
    /// specific event. Behaves like `subscribe_room` but centers the
    /// timeline on `focus_event_id`. Call `paginate_forward` (and
    /// `paginate_back`) to load events in either direction from the focus.
    Result subscribe_room_at(const std::string& room_id,
                             const std::string& focus_event_id);

    /// MSC3030 Jump to Date: paginate forward in a focused timeline.
    /// Only valid after `subscribe_room_at`. `reached_end` is true when the
    /// live end has been reached; the UI should then switch to a live
    /// subscription via `subscribe_room`.
    PaginateResult paginate_forward(const std::string& room_id,
                                    std::uint16_t count);

    /// Subscribe to the thread rooted at `root_event_id` in `room_id`. Fires
    /// IEventHandler::on_thread_* callbacks.
    Result subscribe_thread(const std::string& room_id,
                            const std::string& root_event_id);

    /// Unsubscribe from a thread timeline.
    void unsubscribe_thread(const std::string& room_id,
                            const std::string& root_event_id);

    /// Paginate backwards in a subscribed thread timeline.
    PaginateResult paginate_thread_back(const std::string& room_id,
                                        const std::string& root_event_id,
                                        std::uint16_t count);

    /// Start watching the thread list for `room_id` (fires
    /// IEventHandler::on_threads_updated on changes).
    Result subscribe_room_threads(const std::string& room_id);
    void unsubscribe_room_threads(const std::string& room_id);
    /// Snapshot of the current thread list for `room_id`.
    std::vector<ThreadInfo> list_room_threads(const std::string& room_id);
    PaginateResult paginate_room_threads(const std::string& room_id);

    /// Kick off a background pass that paginates the given rooms (those
    /// currently visible in the room list), up to ~50 events each, with
    /// bounded concurrency. Idempotent — safe to call from every room-open
    /// path. Silent: no event callbacks fire for the rooms it visits; only
    /// the SDK's persistent event cache is warmed.
    Result start_background_backfill(const std::vector<std::string>& room_ids);

    /// Like `start_background_backfill` but selects all joined rooms not yet
    /// cached in `backfill_ts`. Called when inactive-room grouping is enabled
    /// to populate `last_activity_ts` for off-screen rooms. Idempotent.
    Result start_background_backfill_all_uncached();

    /// Cancel an in-progress background backfill (also called automatically
    /// on stop_sync / destruction). No-op if none is running.
    void stop_background_backfill();

    // ------------------------------------------------------------------
    // Messaging
    // ------------------------------------------------------------------

    Result send_message(const std::string& room_id, const std::string& body,
                        const std::string& formatted_body = "");

    /// Send an `m.emote` message (the `/me` slash command). Same arguments
    /// and semantics as `send_message` but the event carries an `m.emote`
    /// msgtype. Callers are expected to have already stripped the `/me `
    /// prefix from `body` / `formatted_body`.
    Result send_emote(const std::string& room_id, const std::string& body,
                      const std::string& formatted_body = "");

    /// Re-enable the send queue for `room_id` after a recoverable failure.
    Result retry_send(const std::string& room_id);

    /// Abort the pending local echo with `txn_id` in `room_id`.
    Result abort_send(const std::string& room_id, const std::string& txn_id);

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
    ///
    /// When `is_animated` is true the image is sent as a raw `m.image` event
    /// carrying the MSC4230 `org.matrix.msc4230.is_animated` flag and the
    /// `fi.mau.video.gif` vendor hint so animated GIFs/WebPs autoplay and
    /// loop on capable clients (the standard upload path strips these fields).
    ///
    /// When non-empty, sends into this thread root (MSC3440).
    Result send_image(const std::string& room_id,
                      const std::vector<uint8_t>& bytes,
                      const std::string& mime_type, const std::string& filename,
                      const std::string& caption, std::uint32_t width,
                      std::uint32_t height,
                      bool is_animated,
                      const std::string& reply_event_id = "",
                      const std::string& thread_root = std::string{});

    /// Send a video to `room_id` as an `m.video` event. `width`/`height` are
    /// the video source dimensions; `thumb_bytes` is a JPEG first-frame
    /// thumbnail (pass an empty vector when unavailable) with dimensions
    /// `thumb_width`/`thumb_height`; `duration_ms` populates `info.duration`
    /// (pass 0 when unknown). `caption`/`reply_event_id` follow the same
    /// MSC2530 / m.in_reply_to framing as send_image(). The SDK uploads the
    /// thumbnail as a separate media item and sets `info.thumbnail_url`.
    /// E2EE rooms are handled transparently.
    ///
    /// When non-empty, sends into this thread root (MSC3440).
    Result send_video(const std::string& room_id,
                      const std::vector<uint8_t>& bytes,
                      const std::string& mime_type, const std::string& filename,
                      const std::string& caption,
                      std::uint32_t width, std::uint32_t height,
                      const std::vector<uint8_t>& thumb_bytes,
                      std::uint32_t thumb_width, std::uint32_t thumb_height,
                      std::uint64_t duration_ms,
                      const std::string& reply_event_id = "",
                      const std::string& thread_root = std::string{});

    /// Send an audio file to `room_id` as a plain `m.audio` event (not an
    /// MSC3245 voice message). `duration_ms` populates `info.duration` (pass
    /// 0 when unknown). `caption`/`reply_event_id` follow the same MSC2530 /
    /// m.in_reply_to framing as send_image(). E2EE rooms are handled
    /// transparently.
    ///
    /// When non-empty, sends into this thread root (MSC3440).
    Result send_audio(const std::string& room_id,
                      const std::vector<uint8_t>& bytes,
                      const std::string& mime_type, const std::string& filename,
                      const std::string& caption,
                      std::uint64_t duration_ms,
                      const std::string& reply_event_id = "",
                      const std::string& thread_root = std::string{});

    /// Send an arbitrary file to `room_id` as an `m.file` event. `bytes` is
    /// the raw file payload (no re-encoding); `mime_type` is best-effort —
    /// "application/octet-stream" is acceptable when unknown. When `caption`
    /// is non-empty the event follows MSC2530 framing (`body` = caption,
    /// dedicated `filename` field carries the file name). E2EE rooms are
    /// handled transparently by matrix-sdk.
    ///
    /// When non-empty, sends into this thread root (MSC3440).
    Result send_file(const std::string& room_id,
                     const std::vector<uint8_t>& bytes,
                     const std::string& mime_type, const std::string& filename,
                     const std::string& caption,
                     const std::string& reply_event_id = "",
                     const std::string& thread_root = std::string{});

    /// Encode `pcm_size` bytes of 48kHz/16-bit/mono PCM as an Opus/OGG voice
    /// message and send it to `room_id` as an MSC3245 m.audio event.
    /// `waveform` provides MSC1767 amplitude samples (clamped to 256 server-side).
    /// `caption` and `reply_event_id` follow the same semantics as send_image().
    ///
    /// When non-empty, sends into this thread root (MSC3440).
    Result send_voice(const std::string& room_id,
                      const std::uint8_t* pcm, std::size_t pcm_size,
                      std::uint64_t duration_ms,
                      const std::vector<std::uint16_t>& waveform,
                      const std::string& caption,
                      const std::string& reply_event_id,
                      const std::string& thread_root = std::string{});

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
    /// Unicode emoji (e.g. "👍") or an mxc:// URI for MSC4027 image reactions.
    Result send_reaction(const std::string& room_id,
                         const std::string& event_id, const std::string& key);

    /// Send an MSC4027 custom-image reaction (always adds, never toggles).
    /// `key` is the mxc:// URI; `shortcode` is stored as
    /// `com.beeper.reaction.shortcode` and may be empty. Use `send_reaction`
    /// (with the same mxc:// key) to redact/toggle.
    Result send_reaction_custom(const std::string& room_id,
                                const std::string& event_id,
                                const std::string& key,
                                const std::string& shortcode);

    /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
    /// Requires that the room is currently subscribed via `subscribe_room`.
    /// Server-side permission errors (e.g. lacking power to redact someone
    /// else's event) surface in `Result::message`. On success the SDK
    /// re-emits the timeline item as a redacted tombstone, which the UI
    /// receives via the existing `on_message_event` re-render path.
    Result redact_event(const std::string& room_id, const std::string& event_id,
                        const std::string& reason = "");

    /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds
    /// the `m.in_reply_to` relation. Does not require `subscribe_room`.
    Result send_reply(const std::string& room_id, const std::string& event_id,
                      const std::string& body,
                      const std::string& formatted_body = "");

    /// Send `body` into the thread rooted at `thread_root` (MSC3440). Does not
    /// require subscribe_room.
    Result send_thread_message(const std::string& room_id,
                               const std::string& thread_root,
                               const std::string& body,
                               const std::string& formatted_body);

    /// Reply to `in_reply_to_event_id` within the thread rooted at
    /// `thread_root`. Does not require subscribe_room.
    Result send_thread_reply(const std::string& room_id,
                             const std::string& thread_root,
                             const std::string& in_reply_to_event_id,
                             const std::string& body,
                             const std::string& formatted_body);

    /// Request async resolution of the replied-to event whose ID is
    /// `event_id` in `room_id`. Requires `subscribe_room`. Returns
    /// immediately; when the SDK resolves the event it fires
    /// `on_message_updated` for every timeline item referencing it, so
    /// the UI re-paints the quote block with the sender name and snippet.
    Result fetch_reply_details(const std::string& room_id,
                               const std::string& event_id);

    /// Edit `event_id` in `room_id` replacing its body with `new_body`.
    /// Only works on own `m.text` events. Does not require `subscribe_room`.
    Result send_edit(const std::string& room_id, const std::string& event_id,
                     const std::string& new_body,
                     const std::string& formatted_body = "");

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

    /// Set the current user's display name on the homeserver.
    /// Blocks the calling thread — call from a worker thread.
    Result set_display_name(const std::string& name);

    /// Upload an avatar image for the current user to the homeserver.
    /// `bytes` is the raw image payload (PNG/JPEG/etc. — identified by
    /// `mime_type`, e.g. "image/png" or "image/jpeg").
    /// Blocks the calling thread — call from a worker thread.
    Result upload_avatar(const std::vector<uint8_t>& bytes,
                         const std::string& mime_type);

    /// Remove the current user's avatar from the homeserver.
    /// Blocks the calling thread — call from a worker thread.
    Result remove_avatar();

    // ------------------------------------------------------------------
    // Devices / sessions
    // ------------------------------------------------------------------

    /// Cross-signing verification state of a device. `Unknown` means the
    /// device is not present in the local crypto store yet (e.g. on a fresh
    /// login before sync has populated it). `Unverified` means the device is
    /// known but not signed by the user's self-signing key.
    enum class DeviceVerification : std::uint8_t
    {
        Unknown = 0,
        Unverified = 1,
        Verified = 2,
    };

    /// One Matrix device/session for the current user.
    struct Device
    {
        std::string id;
        std::string display_name;
        std::string last_seen_ip;
        std::uint64_t last_seen_ts = 0;
        DeviceVerification verification = DeviceVerification::Unknown;
        bool is_current = false;
    };

    /// Result of `begin_delete_device`. On a fresh request the homeserver
    /// always returns a UIA challenge, in which case `needs_uia=true` and
    /// `fallback_url` is a homeserver URL the UI must open in a browser.
    /// After the user authenticates there, call `complete_delete_device`
    /// passing the same `session` back.
    struct DeleteDeviceBegin
    {
        bool ok = false;
        std::string message;
        bool needs_uia = false;
        std::string fallback_url;
        std::string session;

        explicit operator bool() const noexcept { return ok; }
    };

    /// Returns the current device ID (e.g. "ABCDEFGHIJ"), or an empty
    /// string if not logged in.
    std::string get_device_id() const;

    /// List the user's devices/sessions on the homeserver, with verification
    /// state and a current-device marker. Returns an empty vector on error.
    /// Blocks the calling thread — call from a worker thread.
    std::vector<Device> list_devices() const;

    /// Rename a device (no UIA required). Blocks — worker thread.
    Result set_device_display_name(const std::string& device_id,
                                   const std::string& name);

    /// Start deleting a device. The homeserver will normally respond with a
    /// UIA challenge; see `DeleteDeviceBegin` for the follow-up flow.
    /// Blocks — worker thread.
    DeleteDeviceBegin begin_delete_device(const std::string& device_id);

    /// Finish deleting a device once the user has completed UIA in a
    /// browser via the fallback URL returned by `begin_delete_device`.
    /// `session` is the value from `DeleteDeviceBegin::session`.
    /// Blocks — worker thread.
    Result complete_delete_device(const std::string& device_id,
                                  const std::string& session);

    // ------------------------------------------------------------------
    // Presence
    // ------------------------------------------------------------------

    /// Publish the current user's Matrix presence via
    /// `PUT /presence/{userId}/status`. Reuses the same 1/2/3 wire encoding
    /// as the receive-side `IEventHandler::on_presence_changed` path.
    /// Blocks — call from a worker thread.
    Result set_presence(PresenceState state);

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

    /// Fetch raw bytes from an arbitrary HTTP/HTTPS URL.
    /// Returns an empty vector on any error. Blocks the calling thread.
    std::vector<uint8_t> fetch_url_bytes(const std::string& url);

    // ------------------------------------------------------------------
    // URL preview
    // ------------------------------------------------------------------

    /// OpenGraph metadata fetched from the homeserver for a given URL.
    struct UrlPreview
    {
        std::string title;
        std::string description;
        std::string image_mxc; ///< mxc:// URI of the og:image, or empty
        int image_w = 0;
        int image_h = 0;
        bool failed = false; ///< true when server returned no data

        bool has_content() const
        {
            return !title.empty() || !description.empty();
        }
    };

    /// Fetch OpenGraph preview metadata for an http(s) URL from the homeserver.
    /// Blocks the calling thread — invoke only from a worker thread.
    /// Returns `UrlPreview{failed=true}` on any error.
    UrlPreview get_url_preview(const std::string& url);

    // ------------------------------------------------------------------
    // MSC3266 room summary / join
    // ------------------------------------------------------------------

    /// Fetch a room summary (name, topic, avatar, member count, join rule,
    /// encryption state) for any room the homeserver can see, whether or
    /// not the client is a member. Accepts a room ID (`!id:server`) or
    /// alias (`#alias:server`). Returns RoomSummary with ok()==false on error.
    /// Blocks the calling thread — invoke only from a worker thread.
    RoomSummary get_room_summary(const std::string& room_id_or_alias);

    /// Join a room by its ID or alias.
    /// Returns the canonical room ID (e.g. `!id:server`) on success, or an
    /// empty string on failure. Blocks the calling thread — invoke only from
    /// a worker thread.
    std::string join_room(const std::string& room_id_or_alias);

    /// Leave a room. Blocks the calling thread — call from a worker thread.
    Result leave_room(const std::string& room_id);

    /// Fetch the joined member list for a room.
    /// Blocks the calling thread — call from a worker thread.
    std::vector<RoomMember> get_room_members(const std::string& room_id);

    /// Send an m.room.topic state event to update the room topic.
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_topic(const std::string& room_id, const std::string& topic);

    /// Returns "default" | "all" | "mentions" | "off" from the local push-rule
    /// cache. Blocks the calling thread — call from a worker thread.
    std::string get_room_notification_mode(std::string room_id) const;

    /// Set the per-room notification mode by writing push rules to the server.
    /// mode must be "default" | "all" | "mentions" | "off". Fire-and-forget.
    /// Blocks the calling thread — call from a worker thread.
    void set_room_notification_mode(std::string room_id, std::string mode);

    /// Add user_id to m.ignored_user_list account data.
    /// Blocks the calling thread — call from a worker thread.
    Result ignore_user(const std::string& user_id);

    /// Remove user_id from m.ignored_user_list.
    /// Blocks the calling thread — call from a worker thread.
    Result unignore_user(const std::string& user_id);

    /// Return the room ID of an existing DM with user_id, or create a new DM.
    /// Returns an empty string on error.
    /// Blocks the calling thread — call from a worker thread.
    std::string get_or_create_dm(const std::string& user_id);

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
    Result send_sticker(const std::string& room_id, const std::string& body,
                        const std::string& image_url,
                        const std::string& info_json);

    /// Send `m.sticker` into the MSC3440 thread rooted at `thread_root`.
    Result send_thread_sticker(const std::string& room_id,
                               const std::string& thread_root,
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
    bool user_pack_has_sticker(const std::string& image_url,
                               const std::string& info_json) const;

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

    /// Export all Megolm room keys to a passphrase-encrypted file at `path`
    /// (standard Matrix key-export format). Blocks — call from a worker thread.
    Result export_room_keys(const std::string& path,
                            const std::string& passphrase);

    /// Import Megolm room keys from the passphrase-encrypted file at `path`.
    /// Blocks — call from a worker thread.
    Result import_room_keys(const std::string& path,
                            const std::string& passphrase);

    /// Enable or disable background presence polling. Thread-safe; may be
    /// called from the UI thread.
    void set_presence_polling_enabled(bool enabled);

    /// Issue one immediate round of DM presence polls without waiting for
    /// the 60s interval. Called by the shell when the window returns to
    /// focus so contacts don't appear stale after un-minimize. No-op if
    /// sync isn't running or polling is disabled. Thread-safe.
    void poll_presence_now();

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
    std::vector<VerificationEmoji>
    get_sas_emojis(const std::string& flow_id) const;

    // ------------------------------------------------------------------
    // Server pushers (Step 12)
    // ------------------------------------------------------------------

    /// Register (or update) an HTTP pusher on the homeserver for UnifiedPush.
    /// `pushkey` is a unique per-account identifier (e.g. sanitised Matrix ID
    /// + hostname). `endpoint_url` is the push gateway URL provided by the
    /// UnifiedPush distributor. Blocks until the homeserver acknowledges.
    Result register_pusher(const std::string& pushkey,
                           const std::string& app_id,
                           const std::string& app_display_name,
                           const std::string& device_display_name,
                           const std::string& endpoint_url,
                           const std::string& lang);

    /// Remove a pusher from the homeserver identified by `pushkey` / `app_id`.
    Result remove_pusher(const std::string& pushkey, const std::string& app_id);

    /// Hint that a push notification arrived for `room_id`. Triggers a
    /// targeted sliding-sync subscription so fresh state arrives in the next
    /// sync cycle before the regular loop catches up.
    Result hint_push_room(const std::string& room_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tesseract
