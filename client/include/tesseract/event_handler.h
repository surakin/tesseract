#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace tesseract {

/// Interface the UI layer implements to receive async events from the sync loop.
/// All callbacks are delivered on a background thread – implementations must
/// marshal work to the UI thread if needed.
class IEventHandler {
public:
    virtual ~IEventHandler() = default;

    virtual void on_message(const Message& msg) = 0;
    virtual void on_rooms_updated(const std::vector<RoomInfo>& rooms) = 0;
    virtual void on_sync_error(const std::string& context,
                                const std::string& description) = 0;

    /// Fired when a room's timeline subscription is reset (room selected).
    /// The UI should clear its message view for this room and await fresh
    /// on_message callbacks for the initial cached items.
    virtual void on_timeline_reset(const std::string& /*room_id*/) {}

    /// Fired whenever the SDK rotates OAuth tokens. Persist the JSON so the next
    /// launch can call restore_session().
    virtual void on_session_saved(const std::string& /*session_json*/) {}
};

} // namespace tesseract
