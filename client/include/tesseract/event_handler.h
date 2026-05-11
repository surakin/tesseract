#pragma once
#include "types.h"
#include <memory>
#include <string>
#include <vector>

namespace tesseract {

/// Interface the UI layer implements to receive async events from the sync loop.
/// All callbacks are delivered on a background thread – implementations must
/// marshal work to the UI thread if needed.
class IEventHandler {
public:
    virtual ~IEventHandler() = default;

    virtual void on_message(Event* ev) = 0;
    virtual void on_rooms_updated(const std::vector<RoomInfo>& rooms) = 0;
    virtual void on_sync_error(const std::string& context,
                                const std::string& description,
                                bool soft_logout) = 0;

    /// Fired when a room's timeline subscription is reset (room selected).
    /// The UI should clear its message view for this room and await fresh
    /// on_message callbacks for the initial cached items.
    virtual void on_timeline_reset(const std::string& /*room_id*/) {}

    /// Fired whenever the SDK rotates OAuth tokens. Persist the JSON so the next
    /// launch can call restore_session().
    virtual void on_session_saved(const std::string& /*session_json*/) {}

    /// Fired when the server-side key-backup state changes, or as room keys
    /// are imported from the backup during/after `recover()`. UIs use this
    /// to drive the recovery banner and the RecoveryDialog progress text.
    virtual void on_backup_progress(const BackupProgress& /*progress*/) {}
};

} // namespace tesseract
