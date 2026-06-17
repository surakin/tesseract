#pragma once
#include "types.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace tesseract
{

/// Interface the UI layer implements to receive async events from the sync loop.
/// All callbacks are delivered on a background thread — implementations must
/// marshal work to the UI thread if needed.
///
/// The four timeline callbacks mirror matrix-sdk-ui's `VectorDiff` semantics
/// so that the UI's message vector is a faithful, index-aligned mirror of
/// the visible (non-virtual) prefix of the SDK's timeline. The `index`
/// arguments are *visible* indices — virtual items such as day-dividers are
/// filtered out on the Rust side and the index always refers to the
/// position in the UI's row vector.
class IEventHandler
{
public:
    virtual ~IEventHandler() = default;

    /// Atomically reset a room's timeline to `snapshot` (oldest-first).
    /// Implementations should clear their model for `room_id` and rebuild
    /// it from the snapshot in a single update. Fires:
    ///   - Once with an empty snapshot when a room is first subscribed
    ///     (synchronous clear so the UI doesn't flash the previous room).
    ///   - Once with the cached snapshot once it has been converted.
    ///   - On any `VectorDiff::Reset` / `Clear` from matrix-sdk-ui.
    virtual void
    on_timeline_reset(const std::string& room_id,
                      EventList snapshot) = 0;

    /// Insert `event` at visible-index `index` in `room_id`'s timeline.
    /// `index == current_length` means "append at the end".
    virtual void on_message_inserted(const std::string& room_id,
                                     std::size_t index,
                                     std::unique_ptr<Event> event) = 0;

    /// Replace the event currently at visible-index `index` with `event`
    /// (edit, redaction, reaction change, sender-profile resolution).
    virtual void on_message_updated(const std::string& room_id,
                                    std::size_t index,
                                    std::unique_ptr<Event> event) = 0;

    /// Remove the event at visible-index `index`.
    virtual void on_message_removed(const std::string& room_id,
                                    std::size_t index) = 0;

    /// Batch prepend: N older messages added at the front of `room_id`'s
    /// timeline in one shot (back-pagination result). Events are oldest-first.
    /// Replaces N individual on_message_inserted(idx=0) calls.
    virtual void on_messages_prepended(const std::string& /*room_id*/,
                                       EventList /*events*/)
    {
    }

    /// Batch append: N newer messages added at the end of `room_id`'s timeline
    /// (forward-pagination, VectorDiff::Append, or a live-sync burst).
    /// Events are oldest-first.
    virtual void on_messages_appended(const std::string& /*room_id*/,
                                      EventList /*events*/)
    {
    }

    /// Batch update: multiple visible events replaced simultaneously
    /// (receipt-refresh pass after fetch_members, or a VectorDiff::Set burst).
    /// `indices[i]` is the visible-index of `events[i]`; both vectors are
    /// always the same length.
    virtual void on_messages_updated_batch(const std::string& /*room_id*/,
                                           std::vector<std::size_t> /*indices*/,
                                           EventList /*events*/)
    {
    }

    /// Thread-timeline twins of the four room-timeline callbacks. `thread_root`
    /// is the thread root event id. Default no-ops so shells opt in later.
    virtual void on_thread_reset(const std::string& /*room_id*/,
                                 const std::string& /*thread_root*/,
                                 EventList /*snapshot*/)
    {
    }
    virtual void on_thread_inserted(const std::string& /*room_id*/,
                                    const std::string& /*thread_root*/,
                                    std::size_t /*index*/,
                                    std::unique_ptr<Event> /*event*/)
    {
    }
    virtual void on_thread_updated(const std::string& /*room_id*/,
                                   const std::string& /*thread_root*/,
                                   std::size_t /*index*/,
                                   std::unique_ptr<Event> /*event*/)
    {
    }
    virtual void on_thread_removed(const std::string& /*room_id*/,
                                   const std::string& /*thread_root*/,
                                   std::size_t /*index*/)
    {
    }
    /// Batch prepend for a thread timeline (back-pagination of thread history).
    /// Events are oldest-first.
    virtual void on_thread_messages_prepended(const std::string& /*room_id*/,
                                              const std::string& /*thread_root*/,
                                              EventList /*events*/)
    {
    }
    /// Batch append for a thread timeline (forward-pagination or live burst).
    /// Events are oldest-first.
    virtual void on_thread_messages_appended(const std::string& /*room_id*/,
                                             const std::string& /*thread_root*/,
                                             EventList /*events*/)
    {
    }

    /// The `rooms` reference is only valid for the duration of the call —
    /// it points at a bridge-owned temporary. Implementations that need to
    /// keep the data must copy it (the standard pattern is to copy/move it
    /// onto the UI thread), never store the reference or a pointer into it.
    virtual void on_rooms_updated(const std::vector<RoomInfo>& rooms) = 0;

    /// Fired when the set of pending room invitations changes. `invites` is a
    /// full snapshot — replace the cached list rather than diffing. The
    /// reference is only valid for the duration of the call; copy before
    /// marshalling to the UI thread.
    virtual void on_invites_updated(const std::vector<InviteInfo>& /*invites*/) { }

    virtual void on_sync_error(const std::string& context,
                               const std::string& description,
                               bool soft_logout) = 0;

    /// Fired whenever the SDK rotates OAuth tokens. Persist the JSON so the
    /// next launch can call restore_session().
    ///
    /// REQUIRED for any handler backing a persistent session: the default is
    /// a no-op, so failing to override this silently drops every refreshed
    /// token and forces a full re-login on the next launch. Only leave it
    /// defaulted for throwaway/test handlers that never persist a session.
    virtual void on_session_saved(const std::string& /*session_json*/)
    {
    }

    /// Fired when the server-side key-backup state changes, or as room keys
    /// are imported from the backup during/after `recover()`. UIs use this
    /// to drive the recovery banner and the RecoveryDialog progress text.
    virtual void on_backup_progress(const BackupProgress& /*progress*/)
    {
    }

    /// Fired as `Client::enable_recovery()` progresses through setup stages.
    /// `step` encodes EnableProgress: 0=Starting 1=CreatingBackup
    /// 2=CreatingRecoveryKey 3=BackingUp 4=Done 5=Error 6=RoomKeyUploadError.
    /// Step 6 is non-fatal — the SDK will retry backup automatically.
    /// When step==4, `recovery_key` holds the generated key (empty if
    /// passphrase mode was chosen). When step==3, `backed_up`/`total`
    /// carry the running backup count.
    virtual void on_enable_recovery_progress(uint8_t /*step*/,
                                             const std::string& /*recovery_key*/,
                                             uint32_t /*backed_up*/,
                                             uint32_t /*total*/)
    {
    }

    /// Fired when an in-progress cross-signing reset (started via
    /// `Client::begin_reset_crypto_identity()`) resolves. `ok` is true when the
    /// browser approval succeeded and the new identity was uploaded; otherwise
    /// `message` carries the failure / cancellation reason.
    virtual void on_crypto_reset_result(bool /*ok*/,
                                        const std::string& /*message*/)
    {
    }

    /// Fired when the sliding-sync `RoomListService` changes phase (Init →
    /// SettingUp → Running, plus Recovering on reconnect). UIs use this to
    /// drive a "Syncing rooms…" status while the joined-room set is still
    /// being hydrated after login / restore. An initial snapshot is emitted
    /// shortly after `Client::start_sync()` so a UI that opens before the
    /// first transition still has a starting value.
    virtual void on_room_list_state(RoomListState /*state*/)
    {
    }

    /// Fired when the count of in-flight extra HTTP requests changes
    /// (media downloads, /messages back-pagination). The sync long-poll
    /// is NOT included; add 1 when RoomListState is Running or Recovering.
    virtual void on_inflight_changed(std::uint32_t /*count*/)
    {
    }

    /// Debug-only alternative to on_inflight_changed. In debug builds only
    /// this callback fires (not on_inflight_changed); it carries the count
    /// plus a newline-separated list of active operation labels. In release
    /// builds neither this callback nor its Rust caller are compiled.
    /// Default no-op in all builds.
    virtual void on_inflight_changed_debug(std::uint32_t /*count*/,
                                           std::string /*urls*/)
    {
    }

    /// Fired when the cached set of MSC2545 image packs changes (user-pack
    /// edit, room-pack subscription, or live state-event update on a
    /// referenced room). UIs re-query via `Client::list_image_packs` and
    /// repaint any open StickerPicker / EmojiPicker custom tabs.
    virtual void on_image_packs_updated()
    {
    }

    /// Fired when the cached thread list for `room_id` changes. Re-query via
    /// Client::list_room_threads. Default no-op.
    virtual void on_threads_updated(const std::string& /*room_id*/)
    {
    }

    /// Fired when an async media download started via `Client::fetch_media_async`
    /// completes (or fails / times out / is cancelled — `bytes` is empty then).
    /// `request_id` is the correlation token passed to `fetch_media_async`. The
    /// UI looks up its pending request, writes the disk cache, decodes and
    /// repaints; a late callback for an already-cancelled request is ignored.
    /// Default no-op.
    virtual void on_media_ready(std::uint64_t /*request_id*/,
                                const std::vector<std::uint8_t>& /*bytes*/)
    {
    }

    /// Fired when an async URL-preview fetch (`Client::get_url_preview_async`)
    /// completes. `preview_json` matches the synchronous `get_url_preview`
    /// shape (empty on failure). Default no-op.
    virtual void on_url_preview_ready(std::uint64_t /*request_id*/,
                                      const std::string& /*preview_json*/)
    {
    }

    /// Fired when an async GIF search (`Client::gif_search`) completes. The UI
    /// drops results whose `request_id` is stale (a newer search was issued).
    /// Default no-op.
    virtual void on_gif_results(std::uint64_t /*request_id*/,
                                const std::vector<GifResult>& /*results*/)
    {
    }

    /// Fired when an async GIF search fails (network / provider error).
    /// Default no-op.
    virtual void on_gif_search_failed(std::uint64_t /*request_id*/,
                                      const std::string& /*message*/)
    {
    }

    /// Fired when an async full-text search (`Client::search_messages`)
    /// completes. The UI drops results whose `request_id` is stale (a newer
    /// query was issued). Default no-op.
    virtual void on_search_results(std::uint64_t /*request_id*/,
                                   const std::vector<SearchHit>& /*results*/)
    {
    }

    /// Fired when an async full-text search fails (e.g. the index is not open).
    /// Default no-op.
    virtual void on_search_failed(std::uint64_t /*request_id*/,
                                  const std::string& /*message*/)
    {
    }

    /// Fired when an async paginate request started via
    /// `Client::paginate_back_async` or `Client::paginate_forward_async`
    /// completes. `reached_start`/`reached_end` mirror the synchronous
    /// variants; both are false on error. Default no-op.
    virtual void on_paginate_result(std::uint64_t /*request_id*/, bool /*ok*/,
                                    bool /*reached_start*/,
                                    bool /*reached_end*/,
                                    const std::string& /*message*/)
    {
    }

    /// Fired when an async room action (accept_invite_async, join_room_async,
    /// leave_room_async) completes or fails. `joined_room_id` carries the
    /// canonical room ID returned by join; empty for other actions or on
    /// failure. Default no-op.
    virtual void on_room_action_complete(std::uint64_t /*request_id*/,
                                         bool /*ok*/,
                                         const std::string& /*joined_room_id*/,
                                         const std::string& /*message*/)
    {
    }

    /// Fired when an async media upload (send_image_async, send_file_async,
    /// send_audio_async, send_video_async) completes or fails.
    /// `message` is a human-readable error on failure; empty on success.
    /// Default no-op.
    virtual void on_upload_complete(std::uint64_t /*request_id*/, bool /*ok*/,
                                    const std::string& /*message*/)
    {
    }

    /// Fired once at startup (after `start_sync` initialises the room-info
    /// watcher) and again whenever the `im.gnomos.tesseract` global
    /// account-data event changes. `json` is the raw event content object,
    /// or `"{}"` when the event has never been written. UIs re-read
    /// `Client::load_prefs_json()` and apply the updated fields (e.g. the
    /// last-opened room).
    virtual void on_account_prefs_updated(const std::string& /*json*/)
    {
    }

    /// Fired once at startup and whenever the global MSC4278
    /// `m.media_preview_config` account-data event changes (local write or a
    /// change from another device). `json` is the raw event content object,
    /// or `"{}"` when missing. UIs re-read `Client::media_preview_config()`
    /// and update their media/invite-avatar gating.
    virtual void on_media_preview_config_updated(const std::string& /*json*/)
    {
    }

    /// Fired when a new incoming message matches the user's push rules and
    /// should trigger a notification. Called on a background thread; marshal to
    /// the UI thread before showing a system notification. `is_mention` is true
    /// when the push rules matched a highlight rule (@ mention). Only fired for
    /// live PushBack events — pagination does not trigger notifications.
    virtual void on_notification(const std::string& /*room_id*/,
                                 const std::string& /*room_name*/,
                                 const std::string& /*sender*/,
                                 const std::string& /*body*/,
                                 bool /*is_mention*/,
                                 const std::vector<uint8_t>& /*avatar_bytes*/,
                                 const std::vector<uint8_t>& /*image_bytes*/)
    {
    }

    // ------------------------------------------------------------------
    // Cross-signing / SAS device verification (Step: device verification)
    // ------------------------------------------------------------------

    /// A SAS verification flow became available. `incoming` is true when
    /// another device initiated the request (show IncomingRequest state);
    /// false when the local device sent it and the remote has accepted
    /// (show Waiting state and call `start_sas`). Called on a background
    /// thread — marshal to UI thread before touching widgets.
    virtual void on_verification_request(const std::string& /*flow_id*/,
                                         const std::string& /*user_id*/,
                                         const std::string& /*device_id*/,
                                         bool /*incoming*/)
    {
    }

    /// The 7 SAS emoji are ready for display. Called after both devices
    /// have exchanged keys; the UI switches to ShowEmojis state.
    virtual void on_sas_ready(const std::string& /*flow_id*/,
                              std::vector<VerificationEmoji> /*emojis*/)
    {
    }

    /// The SAS flow completed successfully. The UI should show Done state
    /// briefly then dismiss the banner.
    virtual void on_verification_done(const std::string& /*flow_id*/)
    {
    }

    /// The SAS flow was cancelled (mismatch, timeout, or remote decline).
    /// `reason` is the human-readable cancel code from matrix-sdk.
    virtual void on_verification_cancelled(const std::string& /*flow_id*/,
                                           const std::string& /*reason*/)
    {
    }

    /// The device's cross-signing verification state changed. `is_verified`
    /// is true when the device is now fully cross-signed. Called once at
    /// startup with the current state and again on every transition.
    virtual void on_verification_state_changed(bool /*is_verified*/)
    {
    }

    /// Fired when the set of typing users in `room_id` changes. `names`
    /// contains each typing user's resolved display name (falling back to
    /// the Matrix-ID localpart when no member profile is cached), excluding
    /// the local user. Empty `names` means no one is typing. Called on a
    /// background thread.
    virtual void on_typing_changed(const std::string& /*room_id*/,
                                   const std::vector<std::string>& /*names*/)
    {
    }

    /// Called when a presence event is received for `user_id`.
    /// `state` reflects the user's current presence state.
    /// Called on a background thread.
    virtual void on_presence_changed(const std::string& /*user_id*/,
                                     PresenceState /*state*/)
    {
    }

    /// Fired when an async space-child summary fetch
    /// (`Client::get_space_child_summary_async`) completes. `summary_json`
    /// matches the synchronous `get_space_child_summary` shape (empty on
    /// failure). Default no-op.
    virtual void on_space_child_summary_ready(std::uint64_t /*request_id*/,
                                              const std::string& /*summary_json*/)
    {
    }

    /// Fired when an async server-info fetch (`Client::get_server_info_async`)
    /// completes. `info_json` matches the synchronous `get_server_info` shape
    /// (empty on failure or when not logged in). Default no-op.
    virtual void on_server_info_ready(std::uint64_t /*request_id*/,
                                      const std::string& /*info_json*/)
    {
    }
};

} // namespace tesseract
