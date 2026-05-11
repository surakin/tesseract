#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/types.h>
#include <memory>
#include <string>

@protocol MessageListDelegate <NSObject>
- (void)messageListDidScrollToTop;
@end

@interface MessageListController : NSViewController
    <NSTableViewDelegate, NSTableViewDataSource>

@property (nonatomic, weak) id<MessageListDelegate> delegate;
@property (nonatomic, copy) NSString* myUserId;

- (void)clearMessages;
- (void)pushEvent:(std::unique_ptr<tesseract::Event>)ev;

@end
