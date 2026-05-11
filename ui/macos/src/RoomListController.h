#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/types.h>
#include <vector>

@protocol RoomListDelegate <NSObject>
- (void)roomListDidSelectRoomId:(NSString*)roomId;
@end

@interface RoomListController : NSViewController <NSTableViewDelegate, NSTableViewDataSource>

@property (nonatomic, weak) id<RoomListDelegate> delegate;

- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms;

@end
