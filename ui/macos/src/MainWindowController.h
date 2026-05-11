#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/types.h>
#include <memory>
#include <string>
#include <vector>

// Called by EventBridge (from the main thread after GCD dispatch).
@interface MainWindowController : NSWindowController

- (void)doLogin;
- (void)stopSync;

// EventBridge callbacks — all called on main thread.
- (void)pushEvent:(std::unique_ptr<tesseract::Event>)ev;
- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms;
- (void)handleSyncErrorContext:(NSString*)ctx
                   description:(NSString*)desc
                   softLogout:(BOOL)soft;
- (void)handleTimelineReset:(NSString*)roomId;

@end
