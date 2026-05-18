#import "MacScreenLock.h"
#import <Foundation/Foundation.h>

namespace mac
{

MacScreenLock::MacScreenLock()
{
    NSDistributedNotificationCenter* c =
        [NSDistributedNotificationCenter defaultCenter];
    auto locked = locked_; // capture the shared_ptr (block owns a copy)

    observer_locked_ = (__bridge_retained void*)[c
        addObserverForName:@"com.apple.screenIsLocked"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification*) {
                    locked->store(true, std::memory_order_relaxed);
                }];
    observer_unlocked_ = (__bridge_retained void*)[c
        addObserverForName:@"com.apple.screenIsUnlocked"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification*) {
                    locked->store(false, std::memory_order_relaxed);
                }];
}

MacScreenLock::~MacScreenLock()
{
    NSDistributedNotificationCenter* c =
        [NSDistributedNotificationCenter defaultCenter];
    if (observer_locked_)
    {
        id obs = (__bridge_transfer id)observer_locked_;
        [c removeObserver:obs];
        observer_locked_ = nullptr;
    }
    if (observer_unlocked_)
    {
        id obs = (__bridge_transfer id)observer_unlocked_;
        [c removeObserver:obs];
        observer_unlocked_ = nullptr;
    }
}

} // namespace mac
