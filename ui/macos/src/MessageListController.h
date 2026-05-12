#pragma once
#import <UIKit/UIKit.h>
#include <tesseract/types.h>
#include <memory>
#include <string>

namespace tesseract { class Client; }

@protocol MessageListDelegate <NSObject>
- (void)messageListDidScrollToTop;
@end

@interface MessageListController : UITableViewController

@property (nonatomic, weak)   id<MessageListDelegate> delegate;
@property (nonatomic, copy)   NSString*               myUserId;
@property (nonatomic, assign) tesseract::Client*      client;

- (void)clearMessages;
- (void)pushEvent:(std::unique_ptr<tesseract::Event>)ev;

@end
