#pragma once
#include "types.hpp"
#include <string>
#include <vector>

namespace matrix {

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
};

} // namespace matrix
