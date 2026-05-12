#pragma once
#include "event_handler.h"
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

    /// Paginate backwards in a subscribed room. History arrives via
    /// on_message callbacks in oldest-first order.
    Result paginate_back(const std::string& room_id, std::uint16_t count);

    /// Kick off a background pass that paginates every joined room not
    /// currently subscribed, up to ~50 events each, with bounded
    /// concurrency. Idempotent — safe to call from every room-open path.
    /// Silent: no event callbacks fire for the rooms it visits; only the
    /// SDK's persistent event cache is warmed, so the next time the user
    /// opens any of those rooms its history paints from cache.
    Result start_background_backfill();

    /// Cancel an in-progress background backfill (also called automatically
    /// on stop_sync / destruction). No-op if none is running.
    void   stop_background_backfill();

    // ------------------------------------------------------------------
    // Messaging
    // ------------------------------------------------------------------

    Result send_message(const std::string& room_id, const std::string& body);

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
                      std::uint32_t height);

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

    // ------------------------------------------------------------------
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tesseract
