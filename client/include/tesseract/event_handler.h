#pragma once
#include "types.h"
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace tesseract {

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
class IEventHandler {
public:
    virtual ~IEventHandler() = default;

    /// Atomically reset a room's timeline to `snapshot` (oldest-first).
    /// Implementations should clear their model for `room_id` and rebuild
    /// it from the snapshot in a single update. Fires:
    ///   - Once with an empty snapshot when a room is first subscribed
    ///     (synchronous clear so the UI doesn't flash the previous room).
    ///   - Once with the cached snapshot once it has been converted.
    ///   - On any `VectorDiff::Reset` / `Clear` from matrix-sdk-ui.
    virtual void on_timeline_reset(const std::string& room_id,
                                    std::vector<std::unique_ptr<Event>> snapshot) = 0;

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

    virtual void on_rooms_updated(const std::vector<RoomInfo>& rooms) = 0;
    virtual void on_sync_error(const std::string& context,
                                const std::string& description,
                                bool soft_logout) = 0;

    /// Fired whenever the SDK rotates OAuth tokens. Persist the JSON so the next
    /// launch can call restore_session().
    virtual void on_session_saved(const std::string& /*session_json*/) {}

    /// Fired when the server-side key-backup state changes, or as room keys
    /// are imported from the backup during/after `recover()`. UIs use this
    /// to drive the recovery banner and the RecoveryDialog progress text.
    virtual void on_backup_progress(const BackupProgress& /*progress*/) {}

    /// Fired when the sliding-sync `RoomListService` changes phase (Init →
    /// SettingUp → Running, plus Recovering on reconnect). UIs use this to
    /// drive a "Syncing rooms…" status while the joined-room set is still
    /// being hydrated after login / restore. An initial snapshot is emitted
    /// shortly after `Client::start_sync()` so a UI that opens before the
    /// first transition still has a starting value.
    virtual void on_room_list_state(RoomListState /*state*/) {}

    /// Fired when the cached set of MSC2545 image packs changes (user-pack
    /// edit, room-pack subscription, or live state-event update on a
    /// referenced room). UIs re-query via `Client::list_image_packs` and
    /// repaint any open StickerPicker / EmojiPicker custom tabs.
    virtual void on_image_packs_updated() {}

    /// Fired once at startup (after `start_sync` initialises the room-info
    /// watcher) and again whenever the `im.gnomos.tesseract` global
    /// account-data event changes. `json` is the raw event content object,
    /// or `"{}"` when the event has never been written. UIs re-read
    /// `Client::load_prefs_json()` and apply the updated fields (e.g. the
    /// last-opened room).
    virtual void on_account_prefs_updated(const std::string& /*json*/) {}

    /// Fired when a new incoming message matches the user's push rules and
    /// should trigger a notification. Called on a background thread; marshal to
    /// the UI thread before showing a system notification. `is_mention` is true
    /// when the push rules matched a highlight rule (@ mention). Only fired for
    /// live PushBack events — pagination does not trigger notifications.
    virtual void on_notification(const std::string&          /*room_id*/,
                                  const std::string&          /*room_name*/,
                                  const std::string&          /*sender*/,
                                  const std::string&          /*body*/,
                                  bool                        /*is_mention*/,
                                  const std::vector<uint8_t>& /*avatar_bytes*/) {}

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
                                          bool               /*incoming*/) {}

    /// The 7 SAS emoji are ready for display. Called after both devices
    /// have exchanged keys; the UI switches to ShowEmojis state.
    virtual void on_sas_ready(const std::string&                    /*flow_id*/,
                               std::vector<VerificationEmoji>        /*emojis*/) {}

    /// The SAS flow completed successfully. The UI should show Done state
    /// briefly then dismiss the banner.
    virtual void on_verification_done(const std::string& /*flow_id*/) {}

    /// The SAS flow was cancelled (mismatch, timeout, or remote decline).
    /// `reason` is the human-readable cancel code from matrix-sdk.
    virtual void on_verification_cancelled(const std::string& /*flow_id*/,
                                            const std::string& /*reason*/) {}

    /// The device's cross-signing verification state changed. `is_verified`
    /// is true when the device is now fully cross-signed. Called once at
    /// startup with the current state and again on every transition.
    virtual void on_verification_state_changed(bool /*is_verified*/) {}

    /// Fired when the set of typing users in `room_id` changes. `names`
    /// contains the localpart of each typing user (excluding the local user).
    /// Empty `names` means no one is typing. Called on a background thread.
    virtual void on_typing_changed(const std::string& /*room_id*/,
                                    const std::vector<std::string>& /*names*/) {}
};

} // namespace tesseract
