#pragma once
#include <tesseract/event_handler.h>
#include <string>
#include <vector>

@class MainWindowController;

// Pure C++ class: implements the IEventHandler interface and dispatches all
// callbacks to the main thread via GCD, mirroring the Win32 PostMessage and
// GTK g_idle_add patterns in the other backends.
class EventBridge final : public tesseract::IEventHandler {
public:
    explicit EventBridge(MainWindowController* ctrl);

    void on_message(tesseract::Event* ev)                                override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout)                                 override;
    void on_timeline_reset(const std::string& room_id)                   override;
    void on_session_saved(const std::string& session_json)               override;
    void on_backup_progress(const tesseract::BackupProgress& progress)   override;

private:
    // __unsafe_unretained: non-owning ARC pointer. MainWindowController
    // calls stop_sync() before dealloc, guaranteeing no callbacks arrive
    // after the controller is gone.
    __unsafe_unretained MainWindowController* ctrl_;
};
