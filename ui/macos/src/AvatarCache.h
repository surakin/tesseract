#pragma once
#import <UIKit/UIKit.h>
#include <functional>
#include <string>
#include <vector>

// Thread-safe image cache for room and user avatars.
// avatarForKey:fetch: returns a placeholder immediately and delivers the real
// image asynchronously via the completion block on the main thread.
@interface AvatarCache : NSObject

+ (instancetype)shared;

// Returns a cached UIImage or a generated initials placeholder. If no image
// is cached yet, schedules a background fetch via `fetch` and calls
// `completion` on the main thread when the image is ready.
- (UIImage*)avatarForKey:(NSString*)key
                   fetch:(std::function<std::vector<uint8_t>()>)fetch
              completion:(void (^)(UIImage* img))completion;

// Synchronously return any already-cached image (nil if not yet loaded).
- (UIImage*)cachedImageForKey:(NSString*)key;

// Pre-populate the cache (used when image data is already in hand).
- (void)setImage:(UIImage*)image forKey:(NSString*)key;

// Draw an initials circle for a display name or room name.
+ (UIImage*)initialsImageForName:(NSString*)name size:(CGFloat)size;

@end
