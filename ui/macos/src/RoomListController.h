#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/types.h>
#include <vector>

namespace tesseract { class Client; }

@protocol RoomListDelegate <NSObject>
- (void)roomListDidSelectRoomId:(NSString*)roomId;
- (void)roomListDidSelectSpaceId:(NSString*)spaceId;
@end

@interface RoomListController : NSViewController <NSTableViewDelegate, NSTableViewDataSource>

@property (nonatomic, weak)   id<RoomListDelegate> delegate;
@property (nonatomic, assign) tesseract::Client*   client;

- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms;

@end
