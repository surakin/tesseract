#import "MessageListController.h"
#import "AvatarCache.h"

#include <tesseract/client.h>

static NSString* const kCellId     = @"MsgCell";
static const CGFloat kAvatarSize   = 28;
static const CGFloat kBubblePadH   = 10;
static const CGFloat kBubblePadV   = 7;
static const CGFloat kRowPadV      = 6;
static const CGFloat kMaxBubbleW   = 420;
static const CGFloat kSenderH      = 16;
static const CGFloat kTsH          = 14;
static const CGFloat kAvatarGap    = 8;
static const CGFloat kCornerRadius = 12;
static const CGFloat kMaxImageW    = 320;
static const CGFloat kMaxImageH    = 200;
static const CGFloat kMaxStickerSz = 256;

// ── Data ──────────────────────────────────────────────────────────────────────

struct MessageData {
    std::string event_id;
    std::string sender;
    std::string sender_name;
    std::string sender_avatar_url;
    std::string body;
    uint64_t    timestamp  = 0;
    bool        is_own     = false;
    tesseract::EventType type = tesseract::EventType::Text;

    // Image / sticker
    std::string image_url;
    uint64_t    image_w   = 0;
    uint64_t    image_h   = 0;
    std::string image_filename; // MSC2530: only set when sender supplied a distinct filename

    // File
    std::string file_name;
    uint64_t    file_size = 0;
};

static MessageData makeMessageData(const tesseract::Event& ev, NSString* myUserId) {
    MessageData md;
    md.event_id          = ev.event_id;
    md.sender            = ev.sender;
    md.sender_name       = ev.sender_name;
    md.sender_avatar_url = ev.sender_avatar_url;
    md.body              = ev.body;
    md.timestamp         = ev.timestamp;
    md.type              = ev.type;
    md.is_own            = myUserId
        && [@(ev.sender.c_str()) isEqualToString:myUserId];

    if (ev.type == tesseract::EventType::Image) {
        const auto& img = static_cast<const tesseract::ImageEvent&>(ev);
        md.image_url      = img.image_url;
        md.image_w        = img.width;
        md.image_h        = img.height;
        md.image_filename = img.filename;
    } else if (ev.type == tesseract::EventType::Sticker) {
        const auto& s = static_cast<const tesseract::StickerEvent&>(ev);
        md.image_url    = s.image_url;
        md.image_w      = s.width;
        md.image_h      = s.height;
    } else if (ev.type == tesseract::EventType::File) {
        const auto& f = static_cast<const tesseract::FileEvent&>(ev);
        md.file_name = f.file_name;
        md.file_size = f.file_size;
    }
    return md;
}

static NSString* formatTimestamp(uint64_t ms) {
    if (ms == 0) return @"";
    NSDate* d = [NSDate dateWithTimeIntervalSince1970:(double)ms / 1000.0];
    static NSDateFormatter* fmt;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        fmt = [[NSDateFormatter alloc] init];
        fmt.dateFormat = @"HH:mm";
    });
    return [fmt stringFromDate:d];
}

static NSString* formatFileBody(const MessageData& m) {
    NSMutableString* s = [NSMutableString stringWithFormat:@"📎 %s", m.file_name.c_str()];
    if (m.file_size > 0) {
        double kb = m.file_size / 1024.0;
        if (kb < 1024)
            [s appendFormat:@" (%.1f KB)", kb];
        else
            [s appendFormat:@" (%.1f MB)", kb / 1024.0];
    }
    return s;
}

static NSSize scaledImageSize(NSSize src, CGFloat maxW, CGFloat maxH) {
    if (src.width <= 0 || src.height <= 0) return NSMakeSize(maxW, maxH);
    CGFloat scale = MIN(maxW / src.width, maxH / src.height);
    if (scale >= 1.0) return src;
    return NSMakeSize(floor(src.width * scale), floor(src.height * scale));
}

// ── Bubble cell view ──────────────────────────────────────────────────────────

@interface BubbleCellView : NSView
- (void)configureWith:(const MessageData&)msg
           tableWidth:(CGFloat)w
            myUserId:(NSString*)myUserId
          mediaImage:(NSImage*)mediaImage;
@end

@implementation BubbleCellView {
    NSImageView* _avatarView;
    NSTextField* _senderLabel;
    NSTextField* _bodyLabel;
    NSImageView* _mediaView;
    NSTextField* _timestampLabel;
    NSView*      _bubble;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;

    _bubble = [[NSView alloc] init];
    _bubble.wantsLayer            = YES;
    _bubble.layer.cornerRadius    = kCornerRadius;
    _bubble.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_bubble];

    _avatarView = [[NSImageView alloc] init];
    _avatarView.translatesAutoresizingMaskIntoConstraints = NO;
    _avatarView.wantsLayer              = YES;
    _avatarView.layer.cornerRadius      = kAvatarSize / 2;
    _avatarView.layer.masksToBounds     = YES;
    _avatarView.imageScaling            = NSImageScaleAxesIndependently;
    [self addSubview:_avatarView];

    _senderLabel = [NSTextField labelWithString:@""];
    _senderLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _senderLabel.font      = [NSFont boldSystemFontOfSize:11];
    _senderLabel.textColor = [NSColor secondaryLabelColor];
    [self addSubview:_senderLabel];

    _mediaView = [[NSImageView alloc] init];
    _mediaView.translatesAutoresizingMaskIntoConstraints = NO;
    _mediaView.imageScaling = NSImageScaleProportionallyUpOrDown;
    _mediaView.imageAlignment = NSImageAlignTopLeft;
    _mediaView.wantsLayer = YES;
    _mediaView.layer.cornerRadius = 8;
    _mediaView.layer.masksToBounds = YES;
    _mediaView.hidden = YES;
    [_bubble addSubview:_mediaView];

    _bodyLabel = [NSTextField wrappingLabelWithString:@""];
    _bodyLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _bodyLabel.font       = [NSFont systemFontOfSize:13];
    _bodyLabel.selectable = YES;
    [_bubble addSubview:_bodyLabel];

    _timestampLabel = [NSTextField labelWithString:@""];
    _timestampLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _timestampLabel.font      = [NSFont systemFontOfSize:10];
    _timestampLabel.textColor = [NSColor tertiaryLabelColor];
    _timestampLabel.alignment = NSTextAlignmentRight;
    [_bubble addSubview:_timestampLabel];

    return self;
}

- (void)configureWith:(const MessageData&)msg
           tableWidth:(CGFloat)tableW
            myUserId:(NSString*)myUserId
          mediaImage:(NSImage*)mediaImage {
    BOOL own = msg.is_own;
    NSString* sender = @(msg.sender_name.empty()
                           ? msg.sender.c_str()
                           : msg.sender_name.c_str());

    // Stickers are borderless; other types get a bubble background.
    BOOL borderless = (msg.type == tesseract::EventType::Sticker);
    if (borderless) {
        _bubble.layer.backgroundColor = [NSColor clearColor].CGColor;
        _bodyLabel.textColor = [NSColor labelColor];
    } else if (own) {
        _bubble.layer.backgroundColor = [NSColor systemBlueColor].CGColor;
        _bodyLabel.textColor          = [NSColor whiteColor];
    } else {
        _bubble.layer.backgroundColor =
            [NSColor colorNamed:@"BubbleGray"]
            ?: [NSColor colorWithRed:0.91 green:0.91 blue:0.91 alpha:1];
        _bodyLabel.textColor = [NSColor labelColor];
    }

    // Body text per event type.
    NSString* bodyText = nil;
    switch (msg.type) {
        case tesseract::EventType::Image:
            // MSC2530: show body only when sender supplied a distinct filename.
            if (!msg.image_filename.empty() && !msg.body.empty())
                bodyText = @(msg.body.c_str());
            break;
        case tesseract::EventType::Sticker:
            // Sticker body is alt-text only; never displayed.
            break;
        case tesseract::EventType::File:
            bodyText = formatFileBody(msg);
            break;
        case tesseract::EventType::Text:
        default:
            bodyText = @(msg.body.c_str());
            break;
    }

    _bodyLabel.stringValue   = bodyText ?: @"";
    _bodyLabel.hidden        = (bodyText == nil) || bodyText.length == 0;
    _senderLabel.stringValue = own ? @"" : sender;
    _senderLabel.hidden      = own;
    _avatarView.hidden       = own;

    // Media: image or sticker
    BOOL hasMedia = (msg.type == tesseract::EventType::Image ||
                     msg.type == tesseract::EventType::Sticker);
    _mediaView.hidden = !hasMedia || (mediaImage == nil);
    _mediaView.image  = (hasMedia ? mediaImage : nil);

    NSString* ts = formatTimestamp(msg.timestamp);
    _timestampLabel.stringValue = ts ?: @"";
    _timestampLabel.hidden = (ts.length == 0);
    if (borderless) _timestampLabel.hidden = YES;  // no timestamp under stickers

    if (!own) {
        NSString* name = sender;
        NSString* key  = msg.sender_avatar_url.empty()
                           ? @(msg.sender.c_str())
                           : @(msg.sender_avatar_url.c_str());
        NSImage* cached = [[AvatarCache shared] cachedImageForKey:key];
        _avatarView.image = cached
            ?: [AvatarCache initialsImageForName:name size:kAvatarSize];
    }

    // Recompute layout.
    [self removeConstraints:self.constraints];
    [_bubble removeConstraints:_bubble.constraints];

    CGFloat maxBubbleW = MIN(kMaxBubbleW, tableW - kAvatarSize - kAvatarGap - 32);

    // Media size constraints (Image: 320x200, Sticker: 256x256).
    NSSize mediaSize = NSZeroSize;
    if (hasMedia && mediaImage) {
        CGFloat maxW = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageW;
        CGFloat maxH = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageH;
        mediaSize = scaledImageSize(mediaImage.size, maxW, maxH);
    } else if (hasMedia) {
        // Placeholder dimensions while loading: derive from event metadata when
        // available, otherwise a sensible default.
        NSSize hint = NSMakeSize((CGFloat)msg.image_w, (CGFloat)msg.image_h);
        CGFloat maxW = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageW;
        CGFloat maxH = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageH;
        if (hint.width > 0 && hint.height > 0)
            mediaSize = scaledImageSize(hint, maxW, maxH);
        else
            mediaSize = NSMakeSize(160, 120);
    }

    // Inner bubble layout: optional media on top, optional body below it,
    // optional timestamp pinned right.
    NSMutableArray* bubbleC = [NSMutableArray array];

    NSView* topAnchor = nil;
    if (hasMedia) {
        [bubbleC addObjectsFromArray:@[
            [_mediaView.leadingAnchor  constraintEqualToAnchor:_bubble.leadingAnchor
                                                      constant:borderless ? 0 : kBubblePadH],
            [_mediaView.topAnchor      constraintEqualToAnchor:_bubble.topAnchor
                                                      constant:borderless ? 0 : kBubblePadV],
            [_mediaView.widthAnchor    constraintEqualToConstant:mediaSize.width],
            [_mediaView.heightAnchor   constraintEqualToConstant:mediaSize.height],
        ]];
        topAnchor = _mediaView;
    }

    BOOL showBody = !_bodyLabel.hidden;
    if (showBody) {
        if (topAnchor) {
            [bubbleC addObject:[_bodyLabel.topAnchor
                constraintEqualToAnchor:topAnchor.bottomAnchor constant:4]];
        } else {
            [bubbleC addObject:[_bodyLabel.topAnchor
                constraintEqualToAnchor:_bubble.topAnchor constant:kBubblePadV]];
        }
        [bubbleC addObjectsFromArray:@[
            [_bodyLabel.leadingAnchor  constraintEqualToAnchor:_bubble.leadingAnchor
                                                      constant:kBubblePadH],
            [_bodyLabel.trailingAnchor constraintEqualToAnchor:_bubble.trailingAnchor
                                                      constant:-kBubblePadH],
        ]];
        topAnchor = _bodyLabel;
    }

    // Timestamp pinned to bubble bottom-right (unless hidden).
    if (!_timestampLabel.hidden) {
        [bubbleC addObjectsFromArray:@[
            [_timestampLabel.trailingAnchor constraintEqualToAnchor:_bubble.trailingAnchor
                                                           constant:-kBubblePadH],
            [_timestampLabel.bottomAnchor   constraintEqualToAnchor:_bubble.bottomAnchor
                                                           constant:-4],
        ]];
        if (topAnchor) {
            [bubbleC addObject:[_timestampLabel.topAnchor
                constraintGreaterThanOrEqualToAnchor:topAnchor.bottomAnchor constant:2]];
        } else {
            [bubbleC addObject:[_timestampLabel.topAnchor
                constraintEqualToAnchor:_bubble.topAnchor constant:kBubblePadV]];
        }
        topAnchor = _timestampLabel;
    }

    // Bubble bottom anchor — pinned to the last element.
    if (topAnchor) {
        if (topAnchor == _timestampLabel || topAnchor == _bodyLabel) {
            [bubbleC addObject:[_bubble.bottomAnchor
                constraintEqualToAnchor:topAnchor.bottomAnchor constant:kBubblePadV]];
        } else {
            // Media-only — match media bottom
            [bubbleC addObject:[_bubble.bottomAnchor
                constraintEqualToAnchor:topAnchor.bottomAnchor
                               constant:borderless ? 0 : kBubblePadV]];
        }
    }

    [_bubble addConstraints:bubbleC];

    // Outer layout: own messages right-align without avatar; others get avatar+sender.
    if (own) {
        [NSLayoutConstraint activateConstraints:@[
            [_avatarView.widthAnchor  constraintEqualToConstant:0],
            [_avatarView.heightAnchor constraintEqualToConstant:0],

            [_bubble.trailingAnchor constraintEqualToAnchor:self.trailingAnchor
                                                   constant:-12],
            [_bubble.topAnchor      constraintEqualToAnchor:self.topAnchor
                                                   constant:kRowPadV],
            [_bubble.bottomAnchor   constraintEqualToAnchor:self.bottomAnchor
                                                   constant:-kRowPadV],
            [_bubble.widthAnchor    constraintLessThanOrEqualToConstant:maxBubbleW],
        ]];
    } else {
        [NSLayoutConstraint activateConstraints:@[
            [_avatarView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor
                                                      constant:12],
            [_avatarView.bottomAnchor  constraintEqualToAnchor:self.bottomAnchor
                                                      constant:-kRowPadV],
            [_avatarView.widthAnchor   constraintEqualToConstant:kAvatarSize],
            [_avatarView.heightAnchor  constraintEqualToConstant:kAvatarSize],

            [_senderLabel.leadingAnchor constraintEqualToAnchor:_avatarView.trailingAnchor
                                                       constant:kAvatarGap],
            [_senderLabel.topAnchor     constraintEqualToAnchor:self.topAnchor
                                                       constant:kRowPadV],
            [_senderLabel.heightAnchor  constraintEqualToConstant:kSenderH],

            [_bubble.leadingAnchor  constraintEqualToAnchor:_avatarView.trailingAnchor
                                                   constant:kAvatarGap],
            [_bubble.topAnchor      constraintEqualToAnchor:_senderLabel.bottomAnchor
                                                   constant:2],
            [_bubble.bottomAnchor   constraintEqualToAnchor:self.bottomAnchor
                                                   constant:-kRowPadV],
            [_bubble.widthAnchor    constraintLessThanOrEqualToConstant:maxBubbleW],
        ]];
    }
}

@end

// ── Controller ────────────────────────────────────────────────────────────────

@interface MessageListController () <NSScrollViewDelegate>
@end

@implementation MessageListController {
    NSTableView*            _table;
    NSScrollView*           _scroll;
    std::vector<MessageData> _messages;
    // Image cache keyed by mxc:// URL, holding decoded NSImage.
    NSMutableDictionary<NSString*, NSImage*>* _imageCache;
    // Set of URLs currently being fetched.
    NSMutableSet<NSString*>* _imageInflight;
}

- (void)loadView {
    _imageCache    = [NSMutableDictionary dictionary];
    _imageInflight = [NSMutableSet set];

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.hasVerticalScroller   = YES;
    _scroll.autohidesScrollers    = YES;
    _scroll.borderType            = NSNoBorder;

    _table = [[NSTableView alloc] init];
    _table.delegate               = self;
    _table.dataSource             = self;
    _table.headerView             = nil;
    _table.rowHeight              = 60;
    _table.allowsEmptySelection   = YES;
    _table.intercellSpacing       = NSMakeSize(0, 0);
    _table.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;

    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"Msg"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _scroll.documentView = _table;

    // Observe scroll position for paginate-back trigger
    [[NSNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(_scrolled:)
               name:NSScrollViewDidLiveScrollNotification
             object:_scroll];

    self.view = _scroll;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)clearMessages {
    _messages.clear();
    [_table reloadData];
}

- (void)pushEvent:(std::unique_ptr<tesseract::Event>)ev {
    if (!ev) return;
    if (ev->type == tesseract::EventType::Unhandled) return;

    // Update in place if we already have this event (sender profile resolved / edit).
    if (!ev->event_id.empty()) {
        for (NSInteger i = 0; i < (NSInteger)_messages.size(); ++i) {
            if (_messages[i].event_id == ev->event_id) {
                _messages[i] = makeMessageData(*ev, _myUserId);
                [self _prefetchAvatarFor:_messages[i]];
                [self _prefetchMediaFor:_messages[i]];
                [_table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:i]
                                  columnIndexes:[NSIndexSet indexSetWithIndex:0]];
                return;
            }
        }
    }

    MessageData md = makeMessageData(*ev, _myUserId);
    [self _prefetchAvatarFor:md];
    [self _prefetchMediaFor:md];

    NSInteger row = (NSInteger)_messages.size();
    _messages.push_back(std::move(md));

    [_table beginUpdates];
    [_table insertRowsAtIndexes:[NSIndexSet indexSetWithIndex:row]
                  withAnimation:NSTableViewAnimationEffectNone];
    [_table endUpdates];

    [self _scrollToBottomIfNeeded];
}

// ── Async fetchers ────────────────────────────────────────────────────────────

- (void)_prefetchAvatarFor:(const MessageData&)msg {
    if (msg.is_own || msg.sender_avatar_url.empty() || !_client) return;
    NSString* key = @(msg.sender_avatar_url.c_str());
    if ([[AvatarCache shared] cachedImageForKey:key]) return;

    tesseract::Client* client = _client;
    std::string url = msg.sender_avatar_url;
    std::string sender = msg.sender_name.empty() ? msg.sender : msg.sender_name;
    __weak typeof(self) weakSelf = self;
    [[AvatarCache shared] avatarForKey:key
                                 fetch:[client, url] {
                                     return client->fetch_media_bytes(url);
                                 }
                            completion:^(NSImage*) {
        [weakSelf _reloadRowsForSenderAvatarUrl:url];
    }];
    (void)sender;
}

- (void)_reloadRowsForSenderAvatarUrl:(std::string)url {
    NSMutableIndexSet* idx = [NSMutableIndexSet indexSet];
    for (NSInteger i = 0; i < (NSInteger)_messages.size(); ++i)
        if (_messages[i].sender_avatar_url == url)
            [idx addIndex:(NSUInteger)i];
    if (idx.count > 0)
        [_table reloadDataForRowIndexes:idx
                          columnIndexes:[NSIndexSet indexSetWithIndex:0]];
}

- (void)_prefetchMediaFor:(const MessageData&)msg {
    BOOL hasMedia = (msg.type == tesseract::EventType::Image ||
                     msg.type == tesseract::EventType::Sticker);
    if (!hasMedia || msg.image_url.empty() || !_client) return;

    NSString* key = @(msg.image_url.c_str());
    if (_imageCache[key]) return;
    if ([_imageInflight containsObject:key]) return;
    [_imageInflight addObject:key];

    tesseract::Client* client = _client;
    std::string url = msg.image_url;
    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        auto bytes = client->fetch_media_bytes(url);
        NSImage* img = nil;
        if (!bytes.empty()) {
            NSData* data = [NSData dataWithBytes:bytes.data()
                                          length:bytes.size()];
            img = [[NSImage alloc] initWithData:data];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) s = weakSelf;
            if (!s) return;
            [s->_imageInflight removeObject:key];
            if (!img) return;
            s->_imageCache[key] = img;

            NSMutableIndexSet* idx = [NSMutableIndexSet indexSet];
            for (NSInteger i = 0; i < (NSInteger)s->_messages.size(); ++i)
                if (s->_messages[i].image_url == url)
                    [idx addIndex:(NSUInteger)i];
            if (idx.count > 0) {
                // Row height depends on the loaded image — invalidate.
                [s->_table noteHeightOfRowsWithIndexesChanged:idx];
                [s->_table reloadDataForRowIndexes:idx
                                     columnIndexes:[NSIndexSet indexSetWithIndex:0]];
            }
        });
    });
}

- (void)_scrollToBottomIfNeeded {
    NSClipView* clip = _scroll.contentView;
    CGFloat maxY = _table.frame.size.height - clip.bounds.size.height;
    CGFloat curY = clip.bounds.origin.y;
    if (maxY - curY < 200 || _messages.size() <= 2) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (_messages.empty()) return;
            NSInteger last = (NSInteger)_messages.size() - 1;
            [_table scrollRowToVisible:last];
        });
    }
}

- (void)_scrolled:(NSNotification*)n {
    NSClipView* clip = _scroll.contentView;
    if (clip.bounds.origin.y < 20)
        [_delegate messageListDidScrollToTop];
}

// ── NSTableViewDataSource ─────────────────────────────────────────────────────

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)_messages.size();
}

// ── NSTableViewDelegate ───────────────────────────────────────────────────────

- (NSView*)tableView:(NSTableView*)tv
  viewForTableColumn:(NSTableColumn*)col
                 row:(NSInteger)row {
    BubbleCellView* cell = [tv makeViewWithIdentifier:kCellId owner:self];
    if (!cell) {
        cell = [[BubbleCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = kCellId;
    }
    const MessageData& m = _messages[row];
    NSImage* media = nil;
    if (!m.image_url.empty())
        media = _imageCache[@(m.image_url.c_str())];
    [cell configureWith:m
             tableWidth:tv.bounds.size.width
              myUserId:_myUserId
            mediaImage:media];
    return cell;
}

- (CGFloat)tableView:(NSTableView*)tv heightOfRow:(NSInteger)row {
    const MessageData& msg = _messages[row];
    BOOL own = msg.is_own;

    CGFloat tableW = tv.bounds.size.width;
    CGFloat maxBubbleW = MIN(kMaxBubbleW, tableW - kAvatarSize - kAvatarGap - 32);
    CGFloat innerW = maxBubbleW - 2 * kBubblePadH;
    if (innerW < 50) innerW = 50;

    // Body text height (if any).
    NSString* bodyText = nil;
    switch (msg.type) {
        case tesseract::EventType::Image:
            if (!msg.image_filename.empty() && !msg.body.empty())
                bodyText = @(msg.body.c_str());
            break;
        case tesseract::EventType::Sticker:
            break;
        case tesseract::EventType::File:
            bodyText = formatFileBody(msg);
            break;
        case tesseract::EventType::Text:
        default:
            bodyText = @(msg.body.c_str());
            break;
    }

    CGFloat bodyH = 0;
    if (bodyText.length > 0) {
        NSDictionary* attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSRect b = [bodyText boundingRectWithSize:NSMakeSize(innerW, CGFLOAT_MAX)
                                          options:NSStringDrawingUsesLineFragmentOrigin
                                       attributes:attrs
                                          context:nil];
        bodyH = ceil(b.size.height);
    }

    // Media height (if any).
    CGFloat mediaH = 0;
    BOOL hasMedia = (msg.type == tesseract::EventType::Image ||
                     msg.type == tesseract::EventType::Sticker);
    if (hasMedia) {
        CGFloat maxW = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageW;
        CGFloat maxH = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageH;
        NSImage* loaded = !msg.image_url.empty()
            ? _imageCache[@(msg.image_url.c_str())] : nil;
        NSSize sz = NSZeroSize;
        if (loaded) {
            sz = scaledImageSize(loaded.size, maxW, maxH);
        } else if (msg.image_w > 0 && msg.image_h > 0) {
            sz = scaledImageSize(NSMakeSize((CGFloat)msg.image_w,
                                            (CGFloat)msg.image_h), maxW, maxH);
        } else {
            sz = NSMakeSize(160, 120);
        }
        mediaH = sz.height;
    }

    BOOL borderless = (msg.type == tesseract::EventType::Sticker);

    CGFloat bubblePadV = borderless ? 0 : 2 * kBubblePadV;
    CGFloat innerSpacing = (mediaH > 0 && bodyH > 0) ? 4 : 0;
    CGFloat tsH = borderless ? 0 : (msg.timestamp > 0 ? kTsH + 4 : 0);
    CGFloat senderH  = own ? 0 : kSenderH + 2;

    CGFloat bubbleContentH = mediaH + innerSpacing + bodyH + tsH;
    if (bubbleContentH == 0) bubbleContentH = 18;  // minimum

    return senderH + bubblePadV + bubbleContentH + 2 * kRowPadV;
}

@end
