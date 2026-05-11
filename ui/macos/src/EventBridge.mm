#import "EventBridge.h"
#import "MainWindowController.h"

#include <tesseract/session_store.h>
#include <memory>

EventBridge::EventBridge(MainWindowController* ctrl) : ctrl_(ctrl) {}

void EventBridge::on_message(tesseract::Event* ev) {
    struct P {
        __unsafe_unretained MainWindowController* ctrl;
        std::unique_ptr<tesseract::Event> ev;
    };
    auto* p = new P{ctrl_, std::unique_ptr<tesseract::Event>(ev)};
    dispatch_async(dispatch_get_main_queue(), ^{
        [p->ctrl pushEvent:std::move(p->ev)];
        delete p;
    });
}

void EventBridge::on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) {
    struct P {
        __unsafe_unretained MainWindowController* ctrl;
        std::vector<tesseract::RoomInfo> rooms;
    };
    auto* p = new P{ctrl_, rooms};
    dispatch_async(dispatch_get_main_queue(), ^{
        [p->ctrl updateRooms:std::move(p->rooms)];
        delete p;
    });
}

void EventBridge::on_sync_error(const std::string& context,
                                 const std::string& description,
                                 bool soft_logout) {
    struct P {
        __unsafe_unretained MainWindowController* ctrl;
        NSString* context;
        NSString* description;
        BOOL soft_logout;
    };
    auto* p = new P{
        ctrl_,
        [NSString stringWithUTF8String:context.c_str()],
        [NSString stringWithUTF8String:description.c_str()],
        static_cast<BOOL>(soft_logout)
    };
    dispatch_async(dispatch_get_main_queue(), ^{
        [p->ctrl handleSyncErrorContext:p->context
                            description:p->description
                            softLogout:p->soft_logout];
        delete p;
    });
}

void EventBridge::on_timeline_reset(const std::string& room_id) {
    struct P {
        __unsafe_unretained MainWindowController* ctrl;
        NSString* room_id;
    };
    auto* p = new P{ctrl_, [NSString stringWithUTF8String:room_id.c_str()]};
    dispatch_async(dispatch_get_main_queue(), ^{
        [p->ctrl handleTimelineReset:p->room_id];
        delete p;
    });
}

void EventBridge::on_session_saved(const std::string& session_json) {
    // Safe to call from any thread — file I/O with an internal atomic rename.
    tesseract::SessionStore::save(session_json);
}
