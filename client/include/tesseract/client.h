#pragma once
#include "event_handler.h"
#include "image_pack.h"
#include "maps_link.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <optional>
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

/// Extended profile fields from MSC4133 companion proposals.
/// Populated by Client::get_extended_profile().
struct ExtendedProfile {
    std::vector<PronounEntry> pronouns; ///< MSC4247 entries, one per language; empty if not set
    std::string tz;         ///< MSC4175 IANA timezone string, empty if not set
    std::string biography;  ///< MSC4440 plain-text body, empty if not set
};

/// Information about the connected homeserver fetched after login.
/// Bool capability fields default to `true` (permissive) when absent —
/// old servers omit capabilities they support.
struct ServerInfo
{
    std::string homeserver_url;
    std::vector<SpecVersion> spec_versions;  ///< parsed from e.g. ["v1.1","v1.6"]
    bool supports_msc3030          = false;  ///< Jump-to-Date; true if server advertises MSC3030 or spec >= v1.6
    bool can_change_password       = true;
    bool can_set_displayname       = true;
    bool can_set_avatar            = true;
    bool supports_profile_fields   = false;  ///< server advertises uk.tcpip.msc4133
    bool profile_fields_enabled    = true;   ///< m.profile_fields.enabled capability
    bool supports_qr_grant         = false;  ///< server advertises org.matrix.msc4108 (QR grant login)
    bool supports_calls            = false;  ///< server has a livekit RTC transport (MSC4195/MSC4143/well-known)
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
#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
        /// Best-effort: does the homeserver advertise m.login.password among
        /// its login flows? Drives LoginView's auto-detect between OAuth and
        /// username/password fields.
        bool supports_password = false;
#endif

        explicit operator bool() const noexcept
        {
            return ok;
        }
    };

    /// Result of Client::begin_qr_grant(): RGBA pixel bitmap for QR display.
    struct QrGrantBitmap
    {
        bool ok = false;
        std::string message;
        std::vector<uint8_t> pixels; ///< RGBA, side×side×4 bytes
        uint32_t side = 0;           ///< width == height (QR is square)

        explicit operator bool() const noexcept { return ok; }
    };

    /// Returned when the QR grant flow reaches WaitingForAuth.
    struct QrGrantAuth
    {
        bool ok = false;
        std::string message;
        std::string verification_uri;

        explicit operator bool() const noexcept { return ok; }
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

    /// Best-effort: does `homeserver` advertise OAuth2/OIDC support at all?
    /// Blocks the calling thread — invoke from a worker thread.
    bool homeserver_supports_oauth(const std::string& homeserver);

    Result await_oauth();
    void cancel_oauth();

#ifdef TESSERACT_LEGACY_LOGIN_ENABLED
    // ------------------------------------------------------------------
    // Legacy username/password login (m.login.password)
    // Fallback for homeservers without an OIDC/MAS provider. No-op /
    // absent entirely when TESSERACT_ENABLE_LEGACY_LOGIN is not set (guard
    // prevents compilation).
    // ------------------------------------------------------------------

    /// Log in with a Matrix user ID (e.g. "@user:example.org") or bare
    /// localpart, plus password. Blocks the calling thread — invoke from a
    /// worker thread.
    Result login_password(const std::string& homeserver,
                           const std::string& username,
                           const std::string& password);
#endif

    // ------------------------------------------------------------------
    // QR grant login (MSC4108)
    // Generates a QR code on this (logged-in) device for a new device
    // to scan, completing login on the new device.
    // All blocking methods must be called from a worker thread.
    // ------------------------------------------------------------------

    /// Start the QR grant flow. Blocks until the QR bitmap is ready.
    /// Returns the RGBA pixel data for rendering the QR code.
    QrGrantBitmap begin_qr_grant();

    /// Blocks until the new device has scanned the QR code.
    Result await_qr_scanned();

    /// Fast (non-blocking I/O), but still serialised by the FFI lock: sends
    /// the check code (shown on the new device) into the running flow.
    Result submit_qr_check_code(uint8_t code);

    /// Blocks until the flow reaches WaitingForAuth. Returns the
    /// verification URI to open in a browser for OIDC device authorization.
    QrGrantAuth await_qr_auth();

    /// Blocks until the grant flow completes (Done or error).
    Result await_qr_complete();

    /// Cancel the QR grant flow at any phase.
    void cancel_qr_grant();

    static bool open_in_browser(const std::string& url);

    /// Parsed representation of a `https://matrix.to/#/…` URL or a
    /// `matrix:` URI (MSC2312).  `kind == Unknown` for unrecognised input.
    struct MatrixLink
    {
        enum class Kind : uint8_t
        {
            Unknown   = 0,
            Room      = 1, // primary = !room:server
            RoomAlias = 2, // primary = #alias:server
            User      = 3, // primary = @user:server
            Event     = 4, // primary = !room:server, event_id = $evt:server
        };
        Kind        kind     = Kind::Unknown;
        std::string primary;
        std::string event_id;
    };

    /// Parse a `matrix.to` URL or `matrix:` URI.  Never throws.
    static MatrixLink parse_matrix_link(const std::string& uri);

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

    /// Live count of extra in-flight HTTP operations (media downloads, back-
    /// pagination); excludes the sync long-poll. Cheap atomic read, any thread.
    std::uint32_t in_flight_count() const;

    // ------------------------------------------------------------------
    // Invitations
    // ------------------------------------------------------------------

    /// Snapshot of all pending room invitations. Reads the local SDK cache —
    /// no network roundtrip. The list is refreshed whenever
    /// `IEventHandler::on_invites_updated` fires.
    std::vector<InviteInfo> list_invites() const;

    /// Non-blocking accept. Spawns the join as a tokio task; result delivered
    /// via IEventHandler::on_room_action_complete.
    void accept_invite_async(std::uint64_t request_id,
                             const std::string& room_id);

    /// Non-blocking decline. Fire-and-forget; no callback.
    void decline_invite_async(const std::string& room_id);

    /// Non-blocking block-invite. Fire-and-forget; no callback.
    void block_invite_async(const std::string& room_id,
                            const std::string& inviter_user_id);

    // ------------------------------------------------------------------
    // Timeline subscription (Step 2)
    // ------------------------------------------------------------------

    /// Subscribe to a room's timeline. Emits on_timeline_reset then
    /// on_message for cached items, followed by live on_message callbacks.
    /// Call paginate_back_with_status() immediately after to load history.
    Result subscribe_room(const std::string& room_id);

    /// Unsubscribe from a room's timeline and cancel its background task.
    void unsubscribe_room(const std::string& room_id);

    /// Paginate backwards in a subscribed room, reporting whether the timeline
    /// has reached its first event. Older history arrives via
    /// `IEventHandler::on_message_prepended`. UIs use `reached_start` to disable the
    /// scroll-up pagination trigger once no more history can be fetched.
    PaginateResult paginate_back_with_status(const std::string& room_id,
                                             std::uint16_t count);

    /// Non-blocking counterpart of `paginate_back_with_status`. Spawns the
    /// HTTP request as a tokio task and delivers the result via
    /// `IEventHandler::on_paginate_result(request_id, …)`. Does not pin a
    /// C++ worker thread during the network round-trip.
    void paginate_back_async(std::uint64_t request_id,
                             const std::string& room_id,
                             std::uint16_t count);

    /// Backward pagination dedicated to the room-media gallery. Same
    /// underlying call as `paginate_back_async`, but delivers the result via
    /// `IEventHandler::on_media_view_paginate_result(request_id, …)`, which
    /// carries an authoritative Image/Video count read directly from the SDK
    /// timeline — decoupled from the separate, slower diff-streaming task
    /// that delivers rendered rows to the UI.
    void paginate_media_view_back_async(std::uint64_t request_id,
                                        const std::string& room_id,
                                        std::uint16_t count);

    /// Abort an in-flight `paginate_back_async` or
    /// `paginate_media_view_back_async` request if one is still running
    /// under `request_id`. No-op if it already completed or was never
    /// registered. No result callback fires for a cancelled request —
    /// callers should tear down their own bookkeeping for `request_id`
    /// immediately rather than waiting on the callback.
    void cancel_paginate_back(std::uint64_t request_id);

    /// Non-blocking counterpart of `paginate_forward`. Delivers result via
    /// `IEventHandler::on_paginate_result(request_id, …)`.
    void paginate_forward_async(std::uint64_t request_id,
                                const std::string& room_id,
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
    /// timeline on `focus_event_id`. Call `paginate_forward_async` (and
    /// `paginate_back_async`) to load events in either direction from the focus.
    Result subscribe_room_at(const std::string& room_id,
                             const std::string& focus_event_id);

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

    /// For each room_id with no cached entry in the bridge_status SQLite table,
    /// fetch GET /rooms/{id}/state and persist the uk.half-shot.bridge result.
    /// Fires on_rooms_updated if any newly discovered bridged room changes what
    /// the UI shows. Idempotent while a check is already in flight. Call when
    /// the visible room set changes (same pattern as start_background_backfill).
    Result start_bridge_status_check(const std::vector<std::string>& room_ids);

    /// One-shot prefetch of recent messages for the given unread rooms into the
    /// SDK event cache, so opening them is instant. The caller passes the
    /// already capped + LRU-ordered (most-recently-active first) set of unread,
    /// non-muted room ids. Unlike `start_background_backfill` this does NOT skip
    /// rooms that already have a timestamp / are cached — unread rooms need
    /// their *newest* messages. Skips rooms with a live timeline. Bounded
    /// concurrency on a task independent of the inactive-grouping backfill.
    /// Idempotent while a prefetch is in flight.
    Result start_unread_prefetch(const std::vector<std::string>& room_ids);

    /// Cancel an in-progress unread prefetch (also called automatically on
    /// stop_sync / destruction). No-op if none is running.
    void stop_unread_prefetch();

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
    /// `fi.mau.gif` vendor hint so animated GIFs/WebPs autoplay and
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

    /// Non-blocking image send. Spawns the upload as a tokio task; result
    /// delivered via IEventHandler::on_upload_complete.
    void send_image_async(std::uint64_t request_id,
                          const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
                          const std::string& caption,
                          std::uint32_t width, std::uint32_t height,
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

    /// Non-blocking video send. Spawns the upload as a tokio task; result
    /// delivered via IEventHandler::on_upload_complete.
    void send_video_async(std::uint64_t request_id,
                          const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
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

    /// Non-blocking audio send. Spawns the upload as a tokio task; result
    /// delivered via IEventHandler::on_upload_complete.
    void send_audio_async(std::uint64_t request_id,
                          const std::string& room_id,
                          const std::vector<uint8_t>& bytes,
                          const std::string& mime_type,
                          const std::string& filename,
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

    /// Non-blocking file send. Spawns the upload as a tokio task; result
    /// delivered via IEventHandler::on_upload_complete.
    void send_file_async(std::uint64_t request_id,
                         const std::string& room_id,
                         const std::vector<uint8_t>& bytes,
                         const std::string& mime_type,
                         const std::string& filename,
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

    /// Follow `url`'s HTTP redirects (best-effort, `timeout_ms` budget) to
    /// resolve a maps shortlink (goo.gl/maps, maps.app.goo.gl, osm.org/go) to
    /// direct coordinates. Blocking — call only from a background thread,
    /// never the UI thread. Returns `matched=false` on any failure,
    /// non-coordinate target, or timeout; callers should fall back to
    /// sending the original text as a plain message.
    MapsLinkClassification resolve_maps_shortlink(const std::string& url,
                                                  std::uint64_t timeout_ms = 2500);

    /// Send `body` as an `m.location` event (MSC3488) at the given
    /// coordinates. `body` is the plain-text fallback shown by clients that
    /// don't render locations (typically the trimmed maps URL itself).
    Result send_location(const std::string& room_id, double lat, double lon,
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
    Result send_edit(const std::string& room_id, const std::string& event_id,
                     const std::string& new_body,
                     const std::string& formatted_body = "");

    /// Edit the caption of an image/file/video/audio/voice `event_id` in
    /// `room_id`. Preserves the original media content; only patches the
    /// caption. Empty `new_caption` removes the caption. Does not require
    /// `subscribe_room`.
    Result send_caption_edit(const std::string& room_id, const std::string& event_id,
                             const std::string& new_caption);

    /// Forward `event_id` from `source_room_id` to `target_room_id`.
    /// Fetches the event (decrypting for E2EE rooms), strips `m.relates_to`,
    /// and sends it as a new free-standing event in the target room.
    /// Non-blocking: result delivered via IEventHandler::on_forward_done /
    /// on_forward_failed with the supplied request_id.
    void forward_event(std::uint64_t      request_id,
                       const std::string& source_room_id,
                       const std::string& event_id,
                       const std::string& target_room_id);

    /// Read the raw, pretty-printed JSON of `event_id`'s original event (not
    /// a later edit) from the room's already-loaded timeline state. No
    /// network roundtrip — only works for an event currently held in the
    /// room's subscribed timeline's item list (true for any event whose
    /// message row is currently on screen). Returns an empty string if the
    /// room isn't subscribed or the event isn't in its loaded item list.
    std::string get_event_source(const std::string& room_id,
                                  const std::string& event_id);

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

    // ------------------------------------------------------------------
    // MSC4278 media-preview config ("m.media_preview_config" account-data)
    // ------------------------------------------------------------------

    /// Write the global MSC4278 config, dual-writing the stable and unstable
    /// account-data types. Fire-and-forget — returns immediately; the echo
    /// arrives on the next sync and triggers `on_media_preview_config_updated`.
    void save_media_preview_config(MediaPreviewConfig::Mode media_previews,
                                   bool invite_avatars);

    /// Write (or clear) the per-room MSC4278 `media_previews` override for
    /// `room_id`, dual-writing the stable and unstable room-account-data
    /// types. Fire-and-forget — returns immediately; errors are silently
    /// swallowed and there is no completion callback (unlike the global
    /// config there is also no room-scoped sync-driven update callback, so
    /// the caller must update its own cache optimistically — see
    /// ShellBase::apply_room_media_preview_override_).
    /// `has_override == false` clears the override (the room reverts to
    /// inheriting the global config); `media_previews` is ignored in that
    /// case.
    void save_room_media_preview_override(const std::string& room_id,
                                          bool has_override,
                                          MediaPreviewConfig::Mode media_previews);

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
    /// string if none is set / not logged in / the fetch fails. Cached by the SDK.
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

    /// Upload bytes to the media server and return the mxc:// URI in
    /// Result.message. Does NOT change the user's global profile avatar.
    /// Blocks the calling thread — call from a worker thread.
    Result upload_media(const std::vector<uint8_t>& bytes,
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

    /// Non-blocking counterpart of `set_presence`. Returns immediately;
    /// the PUT runs on the Rust tokio runtime. No callback on completion.
    void set_presence_async(PresenceState state);

    // ------------------------------------------------------------------
    // Media
    // ------------------------------------------------------------------

    /// Download the avatar image for the given room and return the raw bytes
    /// (JPEG or PNG). Returns an empty vector when no avatar is set or on
    /// error. The SDK's SQLite media cache is consulted first, so repeat
    /// calls for the same avatar are instant.

    /// Download media from either a plain mxc:// URI or a JSON-serialised
    /// `MediaSource` (an opaque token that may carry an `EncryptedFile` for
    /// MSC2545 stickers and encrypted images). The two forms are detected by
    /// the leading `mxc://` prefix. Returns an empty vector on any failure.
    std::vector<uint8_t> fetch_source_bytes(const std::string& source);

    /// Non-blocking counterpart of `fetch_source_bytes`. Fires
    /// `IEventHandler::on_media_ready(request_id, bytes)` when done.
    void fetch_source_bytes_async(std::uint64_t request_id,
                                   const std::string& source_json);

    // ------------------------------------------------------------------
    // Update checking
    // ------------------------------------------------------------------

    /// Result of a GitHub release update check.
    struct UpdateResult
    {
        bool has_update = false;
        std::string version; ///< latest release tag (leading 'v' stripped); empty when no update
        std::string url;     ///< GitHub release page URL; empty when no update
    };

    /// Check the GitHub Releases API for a newer version than `current_version`.
    /// `repo` is the `owner/repo` slug (e.g. `"acme/tesseract"`).
    /// Blocks the calling thread — call from a worker pool thread, never the UI thread.
    /// Returns `has_update == false` on any network error or when already up-to-date.
    UpdateResult check_for_update(const std::string& repo,
                                  const std::string& current_version);

    // ------------------------------------------------------------------
    // Async media downloads (non-blocking; complete via IEventHandler)
    // ------------------------------------------------------------------

    /// `kind` discriminants for `fetch_media_async`. Keep in sync with the Rust
    /// MEDIA_KIND_* constants in sdk/src/client/media.rs.
    enum class MediaReqKind : std::uint8_t
    {
        RoomAvatar   = 0, ///< room.avatar thumbnail; `source` is the room id
        MxcThumbnail = 1, ///< plain mxc:// thumbnail
        SourceThumb  = 2, ///< source (plain/encrypted) thumbnail
        SourceFull   = 3, ///< full source (plain/encrypted) → bulk lane
    };

    /// Scheduling priority for `fetch_media_async`. Within a lane, a pending
    /// fetch with higher priority is granted a free download slot first, so the
    /// media for a row the user is looking at jumps ahead of the off-screen
    /// backlog. Keep in sync with the Rust PRIO_BACKOFF/PRIO_NORMAL/PRIO_VISIBLE
    /// constants in media_queue.rs.
    enum class MediaPriority : std::uint8_t
    {
        Backoff = 0, ///< backed-off retry (prior failure); never raised by prioritize_media
        Normal  = 1, ///< eager whole-timeline prefetch
        Visible = 2, ///< backs a currently-visible row
    };

    /// Start an async media download. Returns immediately; the bytes arrive via
    /// `IEventHandler::on_media_ready(request_id, bytes)` (empty on failure /
    /// timeout / cancel). Unlike `fetch_*_bytes` this does NOT pin a worker
    /// thread for the network round-trip. `request_id` is a caller-owned
    /// correlation token; `group_id` groups downloads for `cancel_media_group`
    /// and scopes `prioritize_media` (0 = ungrouped/never-cancelled).
    /// `w`/`h`/`animated` apply to the thumbnail kinds. `priority` defaults to
    /// Normal; pass Visible (or call `prioritize_media`) for on-screen rows.
    void fetch_media_async(std::uint64_t request_id, std::uint64_t group_id,
                           MediaReqKind kind, const std::string& source,
                           std::uint32_t w, std::uint32_t h, bool animated,
                           MediaPriority priority = MediaPriority::Normal);

    /// Raise the priority of still-pending `fetch_media_async` downloads in
    /// `group_id` whose request id is in `request_ids` to Visible, so they are
    /// fetched before the off-screen backlog. Called when rows scroll into view.
    /// No-op for already-running/finished requests and for group 0.
    void prioritize_media(std::uint64_t group_id,
                          const std::vector<std::uint64_t>& request_ids);

    /// Abort every in-flight `fetch_media_async` / `get_url_preview_async`
    /// registered under `group_id` (e.g. on room switch). No-op for group 0.
    void cancel_media_group(std::uint64_t group_id);

    /// Fetch OpenGraph preview metadata for an http(s) URL: returns
    /// immediately; the result arrives via
    /// `IEventHandler::on_url_preview_ready(request_id, json)`. Parse the JSON
    /// with `parse_url_preview`.
    void get_url_preview_async(std::uint64_t request_id, std::uint64_t group_id,
                               const std::string& url);

    /// Async counterpart of `fetch_url_bytes` (map tiles, etc.): returns
    /// immediately; the bytes arrive via `IEventHandler::on_media_ready`.
    void fetch_url_async(std::uint64_t request_id, std::uint64_t group_id,
                         const std::string& url);

    // ------------------------------------------------------------------
    // GIF search + send
    // ------------------------------------------------------------------

    /// Search the configured GIF provider for `query` (async, non-blocking).
    /// Results arrive via `IEventHandler::on_gif_results(request_id, …)`, or
    /// `on_gif_search_failed` on error. `request_id` lets the caller drop stale
    /// responses; `api_key`/`client_key` come from app settings; `limit` caps
    /// the result count.
    void gif_search(std::uint64_t request_id, const std::string& query,
                    const std::string& api_key, const std::string& client_key,
                    std::uint32_t limit);

    // ------------------------------------------------------------------
    // Full-text message search
    // ------------------------------------------------------------------

    /// Enable or disable local full-text indexing of decrypted message bodies
    /// (opt-in privacy setting, off by default). Enabling kicks off a lazy
    /// background backfill of existing history; disabling clears the on-disk
    /// index so no decrypted plaintext remains at rest. Cheap + synchronous;
    /// call from the UI thread on a settings toggle, and once after login to
    /// push the persisted preference into the SDK.
    void set_search_indexing_enabled(bool enabled);

    /// Full-text search indexed messages for `query` (async, non-blocking).
    /// Results arrive via `IEventHandler::on_search_results(request_id, …)`, or
    /// `on_search_failed` on error. A non-empty `room_id` scopes the search to
    /// one room (in-room find); empty searches the whole active account (global
    /// overlay). `request_id` lets the caller drop stale responses; `limit`
    /// caps the result count.
    void search_messages(std::uint64_t request_id, const std::string& query,
                         const std::string& room_id, std::uint32_t limit);

    /// Summary of the local search index (message/room counts, oldest indexed
    /// timestamp, backfill-complete flag) for the Settings panel. Synchronous,
    /// cheap single-aggregate read; not for hot paths.
    SearchIndexStats search_index_stats() const;

    /// On-disk size of the search index in bytes, measured via the SQLite
    /// `dbstat` virtual table (an O(pages) B-tree walk). Call once when the
    /// Settings panel opens — not on the 2-second poll tick.
    std::uint64_t search_index_size_bytes() const;

    /// Load all persisted media-backoff entries from `app_cache.db`.
    /// Returns an empty vector when the DB is not open (before sync-start).
    std::vector<MediaBackoffEntry> load_media_backoff() const;

    /// Upsert a backoff entry for `url` (called on fetch failure).
    void note_media_backoff_failed(const std::string& url,
                                   std::uint32_t       attempts,
                                   std::int64_t        deadline_secs);

    /// Delete the backoff entry for `url` (called on fetch success).
    void note_media_backoff_ok(const std::string& url);

    /// Delete all media-backoff entries (called on cache wipe / logout).
    void clear_media_backoff_db();

    /// Load all persisted room-summary-backoff entries from `app_cache.db`.
    /// Returns an empty vector when the DB is not open (before sync-start).
    std::vector<RoomSummaryBackoffEntry> load_room_summary_backoff() const;

    /// Upsert a backoff entry for `room_id` (called on fetch failure).
    void note_room_summary_backoff_failed(const std::string& room_id,
                                          std::uint32_t       attempts,
                                          std::int64_t        deadline_secs);

    /// Delete the backoff entry for `room_id` (called on fetch success).
    void note_room_summary_backoff_ok(const std::string& room_id);

    /// Send a pre-fetched GIF MP4 into `room_id` as an `m.video` carrying the
    /// `fi.mau.gif` vendor hint (autoplay/loop/muted). `thumb_bytes` is a
    /// static poster image (`thumb_mime`, e.g. "image/png"; empty to omit).
    /// MP4 + thumbnail are encrypted in E2EE rooms, plaintext otherwise.
    /// `reply_event_id`/`thread_root` follow the same framing as send_video().
    Result send_gif_video(const std::string& room_id,
                          const std::vector<uint8_t>& mp4_bytes,
                          const std::string& mime_type, const std::string& body,
                          std::uint32_t width, std::uint32_t height,
                          std::uint64_t duration_ms,
                          const std::vector<uint8_t>& thumb_bytes,
                          const std::string& thumb_mime,
                          std::uint32_t thumb_width, std::uint32_t thumb_height,
                          const std::string& reply_event_id,
                          const std::string& thread_root);

    /// Fetch `image_url` (and optionally `preview_url`) from the CDN and send
    /// the GIF into `room_id` without blocking the calling thread. Fires
    /// `IEventHandler::on_upload_complete(request_id, ok, message)` on completion.
    void send_gif_from_urls_async(std::uint64_t request_id,
                                   const std::string& room_id,
                                   const std::string& image_url,
                                   const std::string& image_mime,
                                   const std::string& body,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   const std::string& preview_url,
                                   std::uint32_t preview_w,
                                   std::uint32_t preview_h,
                                   const std::string& reply_event_id,
                                   const std::string& thread_root);

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

    /// Parse the raw OpenGraph JSON (as delivered by `get_url_preview_async`'s
    /// `on_url_preview_ready` callback) into a UrlPreview. Empty/contentless
    /// JSON yields `UrlPreview{failed=true}`.
    static UrlPreview parse_url_preview(const std::string& json);

    // ------------------------------------------------------------------
    // MSC3266 room summary / join
    // ------------------------------------------------------------------

    /// Fetch a room summary (name, topic, avatar, member count, join rule,
    /// encryption state) for any room the homeserver can see, whether or
    /// not the client is a member. Accepts a room ID (`!id:server`) or
    /// alias (`#alias:server`). Returns RoomSummary with ok()==false on error.
    /// Blocks the calling thread — invoke only from a worker thread.
    RoomSummary get_room_summary(const std::string& room_id_or_alias);
    /// Fetch MSC3266 summaries for multiple unjoined space child rooms in one
    /// Fetch the MSC3266 room preview for a single unjoined space child.
    /// Returns the summary on success, std::nullopt on failure (timeout /
    /// server error / stop signal). Blocks the calling thread up to 30 s —
    /// invoke only from a worker thread.
    std::optional<RoomSummary>
    get_space_child_summary(const std::string& space_id,
                            const std::string& room_id);
    /// Async counterpart of `get_space_child_summary`. Spawns the fetch on the
    /// tokio runtime; result delivered via
    /// IEventHandler::on_space_child_summary_ready. Does not pin a thread.
    void get_space_child_summary_async(std::uint64_t request_id,
                                       const std::string& space_id,
                                       const std::string& room_id);
    /// Abort every still-running `get_space_child_summary_async` fetch
    /// registered under `space_id`. Call when the user navigates away from a
    /// space so its still-pending unjoined-room preview fetches stop hitting
    /// the homeserver once their results can no longer be used.
    void cancel_space_summaries(const std::string& space_id);
    /// Return the locally-cached MSC3266 room summary for `room_id`, or
    /// std::nullopt when no cached entry exists. No network, safe on any thread.
    std::optional<RoomSummary>
    get_cached_room_summary(const std::string& room_id) const;

    /// Async counterpart of `get_server_info`. Spawns the fetch on the tokio
    /// runtime; result delivered via IEventHandler::on_server_info_ready.
    /// Does not pin a thread.
    void get_server_info_async(std::uint64_t request_id);

    /// Async counterpart of `media_preview_config`. Spawns the cache read on
    /// the tokio runtime; result delivered via
    /// IEventHandler::on_media_preview_config_ready. Does not pin a thread.
    void media_preview_config_async(std::uint64_t request_id);

    /// Async counterpart of `room_media_preview_override`. Spawns the cache
    /// read on the tokio runtime; result delivered via
    /// IEventHandler::on_room_preview_override_ready. Does not pin a thread.
    void room_media_preview_override_async(std::uint64_t request_id,
                                           const std::string& room_id);

    /// Fetch the room's current m.room.encryption/m.room.join_rules/
    /// m.room.guest_access/m.room.history_visibility state directly from the
    /// homeserver via GET /rooms/{id}/state, bypassing the local sync cache
    /// entirely — needed because guest_access is never delivered via sliding
    /// sync and the other three fields are subject to a separate
    /// update-notification filter that can leave locally cached values
    /// stale after a write. Spawns on the tokio runtime; result delivered
    /// via IEventHandler::on_room_security_state_ready. Does not pin a thread.
    void fetch_room_security_state_async(std::uint64_t request_id,
                                         const std::string& room_id);

    /// Join a room by its ID or alias.
    /// Returns the canonical room ID (e.g. `!id:server`) on success, or an
    /// empty string on failure. Blocks the calling thread — invoke only from
    /// a worker thread.
    std::string join_room(const std::string& room_id_or_alias);

    /// Non-blocking join. Spawns the join as a tokio task; result delivered
    /// via IEventHandler::on_room_action_complete.
    void join_room_async(std::uint64_t request_id,
                         const std::string& room_id_or_alias);

    /// Create a new room from `options`. Returns the canonical room ID
    /// (`!id:server`) on success, or an empty string on failure. Blocks the
    /// calling thread — invoke only from a worker thread.
    std::string create_room(const RoomCreateOptions& options);

    /// Non-blocking counterpart. Spawns the create as a tokio task; result
    /// delivered via IEventHandler::on_room_action_complete (reused — its
    /// ok/room_id/message shape fits creation exactly, same as it does join).
    void create_room_async(std::uint64_t request_id,
                           const RoomCreateOptions& options);

    /// Leave a room. Blocks the calling thread — call from a worker thread.
    Result leave_room(const std::string& room_id);

    /// Non-blocking leave. Spawns the leave as a tokio task; result delivered
    /// via IEventHandler::on_room_action_complete.
    void leave_room_async(std::uint64_t request_id, const std::string& room_id);

    /// Non-blocking invite. Fire-and-forget; no callback.
    void invite_user_async(const std::string& room_id, const std::string& user_id);

    /// Fetch the joined member list for a room.
    /// Blocks the calling thread — call from a worker thread.
    std::vector<RoomMember> get_room_members(const std::string& room_id);

    /// Send an m.room.topic state event to update the room topic.
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_topic(const std::string& room_id, const std::string& topic);

    /// Set the current user's display name in a specific room
    /// (m.room.member state event) — distinct from set_room_display_name,
    /// which sets the room's own m.room.name (visible to all members).
    /// Blocks the calling thread — call from a worker thread.
    Result set_user_room_display_name(const std::string& room_id,
                                       const std::string& name);

    /// Set the current user's avatar in a specific room
    /// (m.room.member state event) — distinct from set_room_avatar, which
    /// sets the room's own m.room.avatar (visible to all members).
    /// Blocks the calling thread — call from a worker thread.
    Result set_user_room_avatar(const std::string& room_id,
                                const std::string& mxc_uri);

    /// Send an m.room.name state event to set the room's own display name
    /// (visible to all members) — distinct from set_user_room_display_name,
    /// which only sets the current user's per-room member override.
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_display_name(const std::string& room_id,
                                  const std::string& name);

    /// Set or clear the room's m.room.avatar state event (visible to all
    /// members) — distinct from set_user_room_avatar, which only sets the
    /// current user's per-room member override. Pass an empty mxc_uri to
    /// clear the room avatar. Blocks the calling thread — call from a
    /// worker thread.
    Result set_room_avatar(const std::string& room_id,
                           const std::string& mxc_uri);

    /// Enable encryption for a room (m.room.encryption). No-op if already
    /// encrypted; there is no operation to disable it — matrix-sdk has none.
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_encryption(const std::string& room_id);

    /// Send an m.room.join_rules state event. `join_rule` must be one of
    /// "public"/"invite"/"knock" — any other value is rejected (the room
    /// settings UI only ever offers these three as editable choices).
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_join_rule(const std::string& room_id,
                              const std::string& join_rule);

    /// Send an m.room.guest_access state event.
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_guest_access(const std::string& room_id, bool allow);

    /// Send an m.room.history_visibility state event. `visibility` must be
    /// one of "world_readable"/"shared"/"invited"/"joined".
    /// Blocks the calling thread — call from a worker thread.
    Result set_room_history_visibility(const std::string& room_id,
                                       const std::string& visibility);

    /// Append event_id to the room's m.room.pinned_events state event.
    /// Idempotent (returns ok when already pinned). Server enforces PL —
    /// failure surfaces as Result{ ok=false, message="<server error>" }.
    /// Blocks the calling thread — call from a worker thread.
    Result pin_event(const std::string& room_id, const std::string& event_id);

    /// Remove event_id from m.room.pinned_events. Idempotent.
    /// Blocks the calling thread — call from a worker thread.
    Result unpin_event(const std::string& room_id, const std::string& event_id);

    /// True iff the current user's PL meets the requirement for sending
    /// m.room.pinned_events in this room. Cached read — no network.
    /// Returns false on any uncertainty.
    bool can_pin_in_room(const std::string& room_id);

    /// True iff the current user's PL meets the requirement for sending
    /// org.matrix.msc3401.call.member state events in this room — required
    /// to start or join a MatrixRTC call. Cached read — no network.
    /// Returns false on any uncertainty.
    bool can_start_call_in_room(const std::string& room_id);

    /// True iff the current user's PL meets the requirement for sending
    /// m.room.name/m.room.topic/m.room.avatar respectively in this room.
    /// Cached reads — no network. Independent per field. Returns false on
    /// any uncertainty.
    bool can_set_room_name(const std::string& room_id);
    bool can_set_room_topic(const std::string& room_id);
    bool can_set_room_avatar(const std::string& room_id);

    /// True iff the current user's PL meets the requirement for sending
    /// m.room.encryption/m.room.join_rules/m.room.guest_access/
    /// m.room.history_visibility respectively in this room. Cached reads —
    /// no network. Independent per field. Returns false on any uncertainty.
    bool can_set_room_encryption(const std::string& room_id);
    bool can_set_room_join_rules(const std::string& room_id);
    bool can_set_room_guest_access(const std::string& room_id);
    bool can_set_room_history_visibility(const std::string& room_id);

    /// True iff the current user's PL meets the requirement for sending
    /// m.room.power_levels in this room — the single all-or-nothing gate
    /// for the whole Permissions tab. Cached read — no network. Returns
    /// false on any uncertainty.
    bool can_set_room_power_levels(const std::string& room_id);

    /// True iff the current user's PL meets the requirement for sending the
    /// room's MSC2545 image-pack state event (either the stable
    /// m.room.image_pack or unstable im.ponies.room_emotes type —
    /// permission to send either is enough to edit a room's packs). Cached
    /// read — no network. Returns false on any uncertainty.
    bool can_set_room_image_packs(const std::string& room_id);

    /// Read the room's current power levels, narrowed to the fields the
    /// Permissions tab edits. Synchronous — cached local read, no network
    /// round-trip (unlike RoomSecurityState, which needs an async GET
    /// /state fetch). Returns Matrix spec defaults on any error. Blocks
    /// briefly — call from a worker thread.
    RoomPermissions room_power_levels(const std::string& room_id);

    /// Send an updated m.room.power_levels state event with the fields
    /// from `levels`. Blocks the calling thread — call from a worker thread.
    Result set_room_power_levels(const std::string& room_id,
                                 const RoomPermissions& levels);

    /// The current user's own effective power level in this room — used to
    /// check whether a staged Permissions-tab change would lock the user
    /// out of ever editing permissions again. Cached read — no network
    /// round-trip. Blocks briefly — call from a worker thread.
    RoomOwnPowerLevel room_own_power_level(const std::string& room_id);

    /// Returns "default" | "all" | "mentions" | "off" from the local push-rule
    /// cache. Blocks the calling thread — call from a worker thread.
    std::string get_room_notification_mode(std::string room_id) const;

    /// Set the per-room notification mode by writing push rules to the server.
    /// mode must be "default" | "all" | "mentions" | "off". Fire-and-forget.
    /// Blocks the calling thread — call from a worker thread.
    void set_room_notification_mode(std::string room_id, std::string mode);

    /// Add/remove the m.favourite tag. Setting it clears m.lowpriority.
    /// Fire-and-forget. Blocks — call from a worker thread.
    void set_room_favourite(std::string room_id, bool value);

    /// Add/remove the m.lowpriority tag. Setting it clears m.favourite.
    /// Fire-and-forget. Blocks — call from a worker thread.
    void set_room_low_priority(std::string room_id, bool value);

    /// Non-blocking counterpart of `ignore_user`. Returns immediately;
    /// the SDK call runs on the Rust tokio runtime. No callback on completion.
    void ignore_user_async(const std::string& user_id);

    /// Non-blocking counterpart of `unignore_user`. Returns immediately;
    /// the SDK call runs on the Rust tokio runtime. No callback on completion.
    void unignore_user_async(const std::string& user_id);

    /// Return the room ID of an existing DM with user_id, or create a new DM.
    /// Returns an empty string on error.
    /// Blocks the calling thread — call from a worker thread.
    std::string get_or_create_dm(const std::string& user_id);

    /// Async counterpart of `get_extended_profile`. Result delivered via
    /// IEventHandler::on_extended_profile_ready. Does not pin a thread.
    void get_extended_profile_async(std::uint64_t request_id,
                                    const std::string& user_id);

    /// Async counterpart of `resolve_user_profile`. Result delivered via
    /// IEventHandler::on_extended_profile_ready. Does not pin a thread.
    void resolve_user_profile_async(std::uint64_t request_id,
                                    const std::string& user_id);

    /// Async entry point that unifies set and delete. Branches on
    /// `value_json == "null"` (delete) vs. any other value (set). Result
    /// delivered via IEventHandler::on_profile_field_result. Does not pin a
    /// thread.
    void set_or_delete_profile_field_async(std::uint64_t request_id,
                                           const std::string& key,
                                           const std::string& value_json);

    // ------------------------------------------------------------------
    // MSC2545 image packs (Step 8)
    // ------------------------------------------------------------------

    /// Snapshot of every image pack the client knows about — the user's
    /// personal pack plus every globally-enabled room pack. Reads the local
    /// cache only. The cache is refreshed by the sync watcher; the UI is
    /// notified via `IEventHandler::on_image_packs_updated`.
    std::vector<ImagePack> list_image_packs() const;

    /// Every room-sourced MSC2545 pack known so far, for the "Known Packs"
    /// settings page (browse rooms with a pack, subscribe/unsubscribe). A
    /// fast local read — no network I/O: rooms are discovered lazily as
    /// the user visits them, or as soon as they're in
    /// m.image_pack.rooms/im.ponies.emote_rooms, and cached persistently.
    /// A room that's neither subscribed nor yet visited won't appear
    /// until visited.
    std::vector<ImagePack> list_known_room_packs() const;

    /// Record `room_id` as recently active (main-window tab switch, pop-out
    /// window open) so the image-pack cache keeps that room's own MSC2545
    /// pack fetched over the network even without an explicit
    /// m.image_pack.rooms subscription — see ImagePack's own doc comment.
    /// Thread-safe; no-op for an empty room_id.
    void set_active_room(const std::string& room_id);

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
    /// transparently. When non-empty, `reply_event_id` adds an
    /// `m.in_reply_to` relation.
    Result send_sticker(const std::string& room_id, const std::string& body,
                        const std::string& image_url,
                        const std::string& info_json,
                        const std::string& reply_event_id = "");

    /// Send `m.sticker` into the MSC3440 thread rooted at `thread_root`.
    /// When non-empty, `reply_event_id` makes the sticker a threaded reply
    /// to that event instead of falling back to the thread root.
    Result send_thread_sticker(const std::string& room_id,
                               const std::string& thread_root,
                               const std::string& body,
                               const std::string& image_url,
                               const std::string& info_json,
                               const std::string& reply_event_id = "");

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

    /// Remove `shortcode` from the user's personal pack. No-op (ok:true) if
    /// the shortcode doesn't exist. GET-modify-PUT; local cache updates
    /// immediately.
    Result remove_user_pack_image(const std::string& shortcode);

    /// Rename `old_shortcode` to `new_shortcode` in the user's personal
    /// pack. If `new_shortcode` collides with an existing entry a numeric
    /// suffix is appended (mirrors `save_sticker_to_user_pack`'s collision
    /// handling) — on success, `Result::message` carries the
    /// actually-applied shortcode, which may differ from the requested one.
    Result rename_user_pack_image(const std::string& old_shortcode,
                                  const std::string& new_shortcode);

    /// Explicitly subscribe/unsubscribe `(room_id, state_key)`'s image pack
    /// via the user's `m.image_pack.rooms` / `im.ponies.emote_rooms`
    /// account data (dual-written). Local cache + `ImagePack::is_subscribed`
    /// refresh synchronously before this returns; `on_image_packs_updated`
    /// fires too.
    Result set_pack_room_subscribed(const std::string& room_id,
                                    const std::string& state_key,
                                    bool subscribed);

    /// Create or replace one of a room's MSC2545 packs — a wholesale
    /// replace of its images with exactly `images` (not an upsert), since
    /// callers (ImagePackEditorView) always stage a full snapshot per
    /// pack, not a diff. Pass `is_new = true` with an empty `state_key`
    /// for a brand-new pack — a fresh, collision-free key is assigned
    /// from `display_name` and returned in `Result::message` on success.
    /// For an existing pack, `state_key` must name it; the write goes to
    /// whichever of the stable `m.room.image_pack` / unstable
    /// `im.ponies.room_emotes` types it currently has content under
    /// (both, if both) so this never introduces a duplicate copy under a
    /// type the room didn't already use. Refreshes the local pack cache
    /// synchronously before returning (`on_image_packs_updated` fires
    /// too). Blocks — call from a worker thread.
    Result save_room_pack(const std::string& room_id,
                          const std::string& state_key, bool is_new,
                          const std::string& display_name,
                          std::uint8_t usage_mask,
                          const std::vector<PackImageInput>& images);

    /// Empty an existing room pack's images (`{"images": {}}`), written to
    /// whichever event type(s) it currently has content under. Matrix
    /// state events cannot be truly deleted — the pack will keep
    /// appearing as a zero-image entry anywhere packs are listed until
    /// the room's state history is redacted (out of scope here). Same
    /// cache-refresh tail as `save_room_pack`. Blocks — worker thread.
    Result remove_room_pack(const std::string& room_id,
                            const std::string& state_key);

    // ------------------------------------------------------------------
    // Spaces
    // ------------------------------------------------------------------

    /// Returns the room IDs of all direct children of a space (via
    /// `m.space.child` state events). Only returns IDs of rooms the
    /// client is a member of. Returns an empty vector when not logged in
    /// or when `space_id` is not a known space.
    std::vector<std::string> space_children(const std::string& space_id) const;

    /// Like `space_children` but returns ALL child room IDs regardless of
    /// membership — includes rooms the user has not joined.
    std::vector<std::string> space_children_all(const std::string& space_id) const;

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

    /// Returns the current recovery state:
    /// 0 = Unknown, 1 = Disabled (fresh account, no encryption set up),
    /// 2 = Enabled, 3 = Incomplete (exists on server, device missing secrets).
    uint8_t recovery_state() const;

    /// Whether a cross-signing identity already exists for the current user
    /// (local read, no network). Used to distinguish a pristine account from
    /// one whose cross-signing identity was set up on another device.
    bool own_identity_exists() const;

    /// Whether this device is currently cross-signed / verified.
    bool device_verified() const;

    /// Whether the cross-signing PRIVATE keys are present locally (this device
    /// bootstrapped the identity, or has recovered it). Unlike device_verified()
    /// this does not depend on the verification state having settled, so it
    /// reliably distinguishes a fresh first device (keys present) from a foreign
    /// identity synced from another device (keys absent).
    bool have_cross_signing_keys() const;

    /// Bootstrap cross-signing + key backup for a fresh account.
    /// Pass an empty `passphrase` to generate a random recovery key; non-empty
    /// to derive the key from the passphrase. Progress is reported via
    /// IEventHandler::on_enable_recovery_progress before this call returns.
    Result enable_recovery(const std::string& passphrase);

    /// Export all Megolm room keys to a passphrase-encrypted file at `path`
    /// (standard Matrix key-export format). Blocks — call from a worker thread.
    Result export_room_keys(const std::string& path,
                            const std::string& passphrase);

    /// Import Megolm room keys from the passphrase-encrypted file at `path`.
    /// Blocks — call from a worker thread.
    Result import_room_keys(const std::string& path,
                            const std::string& passphrase);

    /// Result of `begin_reset_crypto_identity`. When `needs_approval` is true
    /// the homeserver requires the user to approve the cross-signing reset in
    /// a browser: open `approval_url`, then await
    /// `IEventHandler::on_crypto_reset_result` (the SDK polls in the
    /// background). When `needs_approval` is false and `ok` is true the reset
    /// completed with no further auth. `ok == false` means it couldn't start.
    struct CryptoResetBegin
    {
        bool ok = false;
        std::string message;
        bool needs_approval = false;
        std::string approval_url;

        explicit operator bool() const noexcept { return ok; }
    };

    /// Begin resetting the user's cross-signing identity ("Reset cryptographic
    /// identity"). Destructive: uploads a brand-new identity, so other sessions
    /// and contacts must re-verify. Returns quickly; if `needs_approval` is
    /// set, open `approval_url` and await `on_crypto_reset_result`. The browser
    /// approval poll runs in the background (this does NOT block for it).
    /// Blocks only briefly for the initial request — call from a worker thread.
    CryptoResetBegin begin_reset_crypto_identity();

    /// Abort an in-progress cross-signing reset before
    /// `on_crypto_reset_result` fires. No-op when none is pending.
    void cancel_reset_crypto_identity();

    /// Enable or disable background presence polling. Thread-safe; may be
    /// called from the UI thread.
    void set_presence_polling_enabled(bool enabled);

    /// Enable or disable rendering of room membership-change rows
    /// (join/leave/kick/ban/invite/knock and their accept/reject/revoke
    /// variants) in the timeline. Thread-safe. Takes effect on the next
    /// timeline reset for currently-subscribed rooms — call
    /// `subscribe_room_at`/re-subscribe the active room after toggling to
    /// refresh it immediately.
    void set_show_membership_events(bool enabled);

    /// Enable/disable MSC2545 "historical compatibility" — see
    /// Settings::msc2545_legacy_compat's doc comment for the full contract.
    /// Thread-safe.
    void set_msc2545_legacy_compat(bool enabled);

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

    // ------------------------------------------------------------------
    // MatrixRTC voice/video call control
    // These wrap the rtc_* FFI methods on ClientFfi.
    // ------------------------------------------------------------------

    /// Start a MatrixRTC call in `room_id` / `slot_id` (`"call#default"` by
    /// convention). Returns an error when not logged in or already in a call.
    /// `audio_only` controls the `m.call.intent` advertised to other clients.
    Result rtc_start_call(const std::string& room_id,
                          const std::string& slot_id,
                          bool audio_only);

    /// Gracefully leave the active call. No-op when no call is active.
    void rtc_end_call();

    /// Mute or unmute the local audio track. No-op when no call is active.
    void rtc_set_audio_muted(bool muted);

    /// Mute or unmute the local video track. No-op when no call is active.
    void rtc_set_video_muted(bool muted);

    /// Push a raw I420 video frame into the active session.
    /// No-op when no call is active.
    void rtc_push_video_frame_i420(const std::uint8_t* y,
                                   const std::uint8_t* u,
                                   const std::uint8_t* v,
                                   std::uint32_t width, std::uint32_t height,
                                   std::uint32_t stride_y,
                                   std::uint32_t stride_u,
                                   std::uint32_t stride_v);

    /// Start publishing a screen-share track in the active session. Blocks
    /// until the LiveKit publish round-trip completes. No-op (returns a
    /// failed Result) when no call is active.
    Result rtc_start_screen_share();

    /// Stop the screen-share track. No-op when no call is active or not sharing.
    void rtc_stop_screen_share();

    /// Push a raw I420 screen frame into the active session's screen track.
    /// No-op when no call is active or no screen-share track is published.
    void rtc_push_screen_frame_i420(const std::uint8_t* y,
                                    const std::uint8_t* u,
                                    const std::uint8_t* v,
                                    std::uint32_t width, std::uint32_t height,
                                    std::uint32_t stride_y,
                                    std::uint32_t stride_u,
                                    std::uint32_t stride_v);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tesseract
