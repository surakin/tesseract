#import "AvatarCache.h"

// Deterministic hue from a name string (same logic as Win32 initials circles).
static UIColor* colorForName(NSString* name) {
    static UIColor* palette[] = {
        [UIColor colorWithRed:0.76 green:0.13 blue:0.28 alpha:1], // red
        [UIColor colorWithRed:0.33 green:0.49 blue:0.82 alpha:1], // blue
        [UIColor colorWithRed:0.13 green:0.59 blue:0.33 alpha:1], // green
        [UIColor colorWithRed:0.61 green:0.15 blue:0.69 alpha:1], // purple
        [UIColor colorWithRed:0.95 green:0.42 blue:0.07 alpha:1], // orange
        [UIColor colorWithRed:0.00 green:0.59 blue:0.53 alpha:1], // teal
        [UIColor colorWithRed:0.55 green:0.35 blue:0.24 alpha:1], // brown
        [UIColor colorWithRed:0.24 green:0.51 blue:0.76 alpha:1], // steel
    };
    NSUInteger hash = 0;
    for (NSUInteger i = 0; i < name.length; i++)
        hash = hash * 31 + [name characterAtIndex:i];
    return palette[hash % 8];
}

@implementation AvatarCache {
    NSMutableDictionary<NSString*, UIImage*>* _cache;
    NSMutableSet<NSString*>*                  _inflight;
    dispatch_queue_t                           _q;
}

+ (instancetype)shared {
    static AvatarCache* s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[AvatarCache alloc] init]; });
    return s;
}

- (instancetype)init {
    if ((self = [super init])) {
        _cache    = [NSMutableDictionary dictionary];
        _inflight = [NSMutableSet set];
        _q        = dispatch_queue_create("im.gnomos.tesseract.avatarcache",
                                          DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (UIImage*)avatarForKey:(NSString*)key
                   fetch:(std::function<std::vector<uint8_t>()>)fetch
              completion:(void (^)(UIImage*))completion {
    __block UIImage* cached = nil;
    dispatch_sync(_q, ^{ cached = _cache[key]; });
    if (cached) return cached;

    // Kick off a background fetch if not already in progress.
    __block BOOL alreadyInflight = NO;
    dispatch_sync(_q, ^{
        alreadyInflight = [_inflight containsObject:key];
        if (!alreadyInflight) [_inflight addObject:key];
    });

    if (!alreadyInflight) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            auto bytes = fetch();
            UIImage* img = nil;
            if (!bytes.empty()) {
                NSData* data = [NSData dataWithBytes:bytes.data()
                                              length:bytes.size()];
                img = [UIImage imageWithData:data];
            }
            if (!img)
                img = [AvatarCache initialsImageForName:key size:36];

            dispatch_sync(self->_q, ^{
                self->_cache[key] = img;
                [self->_inflight removeObject:key];
            });
            dispatch_async(dispatch_get_main_queue(), ^{
                completion(img);
            });
        });
    }

    return [AvatarCache initialsImageForName:key size:36];
}

- (UIImage*)cachedImageForKey:(NSString*)key {
    __block UIImage* img = nil;
    dispatch_sync(_q, ^{ img = _cache[key]; });
    return img;
}

- (void)setImage:(UIImage*)image forKey:(NSString*)key {
    dispatch_sync(_q, ^{ _cache[key] = image; });
}

+ (UIImage*)initialsImageForName:(NSString*)name size:(CGFloat)size {
    NSString* letter = (name.length > 0)
        ? [[name substringToIndex:1] uppercaseString]
        : @"?";

    UIGraphicsImageRendererFormat* fmt = [UIGraphicsImageRendererFormat preferredFormat];
    fmt.opaque = NO;
    UIGraphicsImageRenderer* renderer =
        [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(size, size)
                                                format:fmt];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* ctx) {
        CGRect r = CGRectMake(0, 0, size, size);
        UIColor* bg = colorForName(name);
        UIBezierPath* circle = [UIBezierPath bezierPathWithOvalInRect:r];
        [bg setFill];
        [circle fill];

        NSDictionary* attrs = @{
            NSFontAttributeName:            [UIFont boldSystemFontOfSize:size * 0.42],
            NSForegroundColorAttributeName: [UIColor whiteColor],
        };
        CGSize ts = [letter sizeWithAttributes:attrs];
        CGPoint pt = CGPointMake((r.size.width  - ts.width)  / 2,
                                 (r.size.height - ts.height) / 2);
        [letter drawAtPoint:pt withAttributes:attrs];
        (void)ctx;
    }];
}

@end
