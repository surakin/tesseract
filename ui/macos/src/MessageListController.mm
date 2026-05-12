#import "MessageListController.h"
#import "AvatarCache.h"

#include <tesseract/client.h>
#include <tesseract/visual.h>
#import <objc/runtime.h>

// Keys for objc_setAssociatedObject — stash (roomId, eventId, reactionKey)
// on each chip UIButton so `reactionChipClicked:` can forward to the client.
static const void* const kChipRoomIdKey   = &kChipRoomIdKey;
static const void* const kChipEventIdKey  = &kChipEventIdKey;
static const void* const kChipKeyKey      = &kChipKeyKey;

// Sizes/spacing live in client/include/tesseract/visual.h — see
// docs/UI-PARITY.md for the canonical anatomy. Per-platform constants
// kept here are limited to inner content padding inside the (now
// invisible) bubble layout container.
static NSString* const kCellId     = @"MsgCell";
static const CGFloat kAvatarSize   = tesseract::visual::kMsgAvatarSize;
static const CGFloat kBubblePadH   = 10;
static const CGFloat kBubblePadV   = 7;
static const CGFloat kRowPadV      = tesseract::visual::kMsgRowVerticalPad;
static const CGFloat kMaxBubbleW   = 520;
static const CGFloat kSenderH      = tesseract::visual::kMsgSenderNameHeight;
static const CGFloat kTsH          = tesseract::visual::kMsgTimestampHeight;
static const CGFloat kAvatarGap    = tesseract::visual::kMsgAvatarGap;
static const CGFloat kMaxImageW    = tesseract::visual::kMaxInlineImageWidth;
static const CGFloat kMaxImageH    = tesseract::visual::kMaxInlineImageHeight;
static const CGFloat kMaxStickerSz = tesseract::visual::kStickerSize;
static const CGFloat kChipH        = tesseract::visual::kReactionChipHeight;
static const CGFloat kChipGap      = tesseract::visual::kReactionChipGap;

// ── Data ──────────────────────────────────────────────────────────────────────

struct MessageData {
    std::string event_id;
    std::string room_id;
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
    std::string image_filename;

    // File
    std::string file_name;
    uint64_t    file_size = 0;

    // Reactions
    std::vector<tesseract::Reaction> reactions;
};

static MessageData makeMessageData(const tesseract::Event& ev, NSString* myUserId) {
    MessageData md;
    md.event_id          = ev.event_id;
    md.room_id           = ev.room_id;
    md.sender            = ev.sender;
    md.sender_name       = ev.sender_name;
    md.sender_avatar_url = ev.sender_avatar_url;
    md.body              = ev.body;
    md.timestamp         = ev.timestamp;
    md.type              = ev.type;
    md.is_own            = myUserId
        && [@(ev.sender.c_str()) isEqualToString:myUserId];
    md.reactions         = ev.reactions;

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

static CGSize scaledImageSize(CGSize src, CGFloat maxW, CGFloat maxH) {
    if (src.width <= 0 || src.height <= 0) return CGSizeMake(maxW, maxH);
    CGFloat scale = MIN(maxW / src.width, maxH / src.height);
    if (scale >= 1.0) return src;
    return CGSizeMake(floor(src.width * scale), floor(src.height * scale));
}

// ── Bubble cell view (inner content view inside UITableViewCell) ──────────────

@interface BubbleCellView : UITableViewCell
- (void)configureWith:(const MessageData&)msg
           tableWidth:(CGFloat)w
            myUserId:(NSString*)myUserId
          mediaImage:(UIImage*)mediaImage
        chipIcons:(NSDictionary<NSString*, UIImage*>*)chipIcons
       chipTarget:(id)chipTarget;
@end

@implementation BubbleCellView {
    UIImageView*  _avatarView;
    UILabel*      _senderLabel;
    UILabel*      _bodyLabel;
    UIImageView*  _mediaView;
    UILabel*      _timestampLabel;
    UIView*       _bubble;
    UIStackView*  _chipStack;
    NSArray<NSLayoutConstraint*>* _outerConstraints;
    NSArray<NSLayoutConstraint*>* _innerConstraints;
}

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString*)reuseIdentifier {
    if (!(self = [super initWithStyle:style reuseIdentifier:reuseIdentifier]))
        return nil;

    self.selectionStyle = UITableViewCellSelectionStyleNone;

    UIView* cv = self.contentView;

    // Invisible layout container — flat-text rendering on every platform.
    _bubble = [[UIView alloc] init];
    _bubble.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:_bubble];

    _avatarView = [[UIImageView alloc] init];
    _avatarView.translatesAutoresizingMaskIntoConstraints = NO;
    _avatarView.contentMode    = UIViewContentModeScaleAspectFill;
    _avatarView.clipsToBounds  = YES;
    _avatarView.layer.cornerRadius = kAvatarSize / 2;
    [cv addSubview:_avatarView];

    _senderLabel = [[UILabel alloc] init];
    _senderLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _senderLabel.font      = [UIFont boldSystemFontOfSize:11];
    _senderLabel.textColor = [UIColor secondaryLabelColor];
    [cv addSubview:_senderLabel];

    _mediaView = [[UIImageView alloc] init];
    _mediaView.translatesAutoresizingMaskIntoConstraints = NO;
    _mediaView.contentMode    = UIViewContentModeScaleAspectFill;
    _mediaView.clipsToBounds  = YES;
    _mediaView.layer.cornerRadius = 8;
    _mediaView.hidden = YES;
    [_bubble addSubview:_mediaView];

    _bodyLabel = [[UILabel alloc] init];
    _bodyLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _bodyLabel.font          = [UIFont systemFontOfSize:13];
    _bodyLabel.numberOfLines = 0;
    [_bubble addSubview:_bodyLabel];

    _timestampLabel = [[UILabel alloc] init];
    _timestampLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _timestampLabel.font          = [UIFont systemFontOfSize:10];
    _timestampLabel.textColor     = [UIColor tertiaryLabelColor];
    _timestampLabel.textAlignment = NSTextAlignmentRight;
    [_bubble addSubview:_timestampLabel];

    _chipStack = [[UIStackView alloc] init];
    _chipStack.axis      = UILayoutConstraintAxisHorizontal;
    _chipStack.alignment = UIStackViewAlignmentCenter;
    _chipStack.spacing   = kChipGap;
    _chipStack.translatesAutoresizingMaskIntoConstraints = NO;
    [_bubble addSubview:_chipStack];

    return self;
}

- (void)configureWith:(const MessageData&)msg
           tableWidth:(CGFloat)tableW
            myUserId:(NSString*)myUserId
          mediaImage:(UIImage*)mediaImage
        chipIcons:(NSDictionary<NSString*, UIImage*>*)chipIcons
       chipTarget:(id)chipTarget {
    NSString* sender = @(msg.sender_name.empty()
                           ? msg.sender.c_str()
                           : msg.sender_name.c_str());

    _bodyLabel.textColor = [UIColor labelColor];

    // Body text per event type.
    NSString* bodyText = nil;
    BOOL bodyIsRedactedTombstone = NO;
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
        case tesseract::EventType::Redacted:
            bodyText = @"Message deleted";
            bodyIsRedactedTombstone = YES;
            break;
        case tesseract::EventType::Text:
        default:
            bodyText = @(msg.body.c_str());
            break;
    }

    _bodyLabel.text   = bodyText ?: @"";
    _bodyLabel.hidden = (bodyText == nil) || bodyText.length == 0;
    if (bodyIsRedactedTombstone) {
        UIFont* base = _bodyLabel.font ?: [UIFont systemFontOfSize:[UIFont systemFontSize]];
        UIFontDescriptor* desc = [base.fontDescriptor
            fontDescriptorWithSymbolicTraits:UIFontDescriptorTraitItalic];
        UIFont* italic = desc ? [UIFont fontWithDescriptor:desc size:base.pointSize] : base;
        _bodyLabel.font      = italic ?: base;
        _bodyLabel.textColor = [UIColor secondaryLabelColor];
    } else {
        _bodyLabel.font      = [UIFont systemFontOfSize:[UIFont systemFontSize]];
        _bodyLabel.textColor = [UIColor labelColor];
    }
    _senderLabel.text   = sender;
    _senderLabel.hidden = NO;
    _avatarView.hidden  = NO;

    // Media: image or sticker
    BOOL hasMedia = (msg.type == tesseract::EventType::Image ||
                     msg.type == tesseract::EventType::Sticker);
    _mediaView.hidden = !hasMedia || (mediaImage == nil);
    _mediaView.image  = (hasMedia ? mediaImage : nil);

    NSString* ts = formatTimestamp(msg.timestamp);
    _timestampLabel.text   = ts ?: @"";
    _timestampLabel.hidden = (ts.length == 0);

    // Rebuild the reaction chip row.
    for (UIView* v in _chipStack.arrangedSubviews.copy)
        [_chipStack removeArrangedSubview:v], [v removeFromSuperview];
    _chipStack.hidden = msg.reactions.empty();
    if (!_chipStack.hidden) {
        for (const auto& r : msg.reactions) {
            UIButton* chip = [UIButton buttonWithType:UIButtonTypeSystem];
            chip.translatesAutoresizingMaskIntoConstraints = NO;
            chip.layer.cornerRadius = kChipH / 2;
            chip.layer.borderWidth  = 1;
            UIColor* fill   = r.reacted_by_me
                ? [[UIColor systemBlueColor] colorWithAlphaComponent:0.20]
                : [[UIColor separatorColor]  colorWithAlphaComponent:0.25];
            UIColor* border = r.reacted_by_me
                ? [[UIColor systemBlueColor] colorWithAlphaComponent:0.6]
                : [UIColor separatorColor];
            chip.backgroundColor      = fill;
            chip.layer.borderColor    = border.CGColor;
            chip.titleLabel.font      = [UIFont systemFontOfSize:10];
            chip.contentEdgeInsets    = UIEdgeInsetsMake(0, 6, 0, 6);

            NSString* qsrc = r.source_json.empty() ? nil : @(r.source_json.c_str());
            UIImage* icon  = qsrc ? chipIcons[qsrc] : nil;
            NSString* count = [NSString stringWithFormat:@"%llu",
                               (unsigned long long)r.count];
            if (icon) {
                [chip setImage:icon forState:UIControlStateNormal];
                [chip setTitle:count forState:UIControlStateNormal];
            } else {
                NSString* title = [NSString stringWithFormat:@"%s %@",
                                   r.key.c_str(), count];
                [chip setTitle:title forState:UIControlStateNormal];
            }

            if (!r.senders.empty()) {
                NSMutableString* tip = [@"Reacted by:" mutableCopy];
                for (const auto& s : r.senders)
                    [tip appendFormat:@"\n  %s", s.c_str()];
                chip.accessibilityHint = tip;
            }

            objc_setAssociatedObject(chip, kChipRoomIdKey,
                @(msg.room_id.c_str()),
                OBJC_ASSOCIATION_COPY_NONATOMIC);
            objc_setAssociatedObject(chip, kChipEventIdKey,
                @(msg.event_id.c_str()),
                OBJC_ASSOCIATION_COPY_NONATOMIC);
            objc_setAssociatedObject(chip, kChipKeyKey,
                @(r.key.c_str()),
                OBJC_ASSOCIATION_COPY_NONATOMIC);

            [chip addTarget:chipTarget
                     action:@selector(reactionChipClicked:)
           forControlEvents:UIControlEventTouchUpInside];

            [chip.heightAnchor constraintEqualToConstant:kChipH].active = YES;
            [_chipStack addArrangedSubview:chip];
        }
    }

    {
        NSString* name = sender;
        NSString* key  = msg.sender_avatar_url.empty()
                           ? @(msg.sender.c_str())
                           : @(msg.sender_avatar_url.c_str());
        UIImage* cached = [[AvatarCache shared] cachedImageForKey:key];
        _avatarView.image = cached
            ?: [AvatarCache initialsImageForName:name size:kAvatarSize];
    }

    // Recompute layout.
    if (_outerConstraints) [NSLayoutConstraint deactivateConstraints:_outerConstraints];
    if (_innerConstraints) [NSLayoutConstraint deactivateConstraints:_innerConstraints];

    CGFloat maxBubbleW = MIN(kMaxBubbleW, tableW - kAvatarSize - kAvatarGap - 32);

    // Media size constraints.
    CGSize mediaSize = CGSizeZero;
    if (hasMedia && mediaImage) {
        CGFloat maxW = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageW;
        CGFloat maxH = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageH;
        mediaSize = scaledImageSize(mediaImage.size, maxW, maxH);
    } else if (hasMedia) {
        CGSize hint = CGSizeMake((CGFloat)msg.image_w, (CGFloat)msg.image_h);
        CGFloat maxW = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageW;
        CGFloat maxH = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageH;
        if (hint.width > 0 && hint.height > 0)
            mediaSize = scaledImageSize(hint, maxW, maxH);
        else
            mediaSize = CGSizeMake(160, 120);
    }

    // Inner bubble layout.
    NSMutableArray<NSLayoutConstraint*>* inner = [NSMutableArray array];
    UIView* topAnchor = nil;
    if (hasMedia) {
        [inner addObjectsFromArray:@[
            [_mediaView.leadingAnchor constraintEqualToAnchor:_bubble.leadingAnchor
                                                     constant:kBubblePadH],
            [_mediaView.topAnchor     constraintEqualToAnchor:_bubble.topAnchor
                                                     constant:kBubblePadV],
            [_mediaView.widthAnchor   constraintEqualToConstant:mediaSize.width],
            [_mediaView.heightAnchor  constraintEqualToConstant:mediaSize.height],
        ]];
        topAnchor = _mediaView;
    }

    BOOL showBody = !_bodyLabel.hidden;
    if (showBody) {
        if (topAnchor) {
            [inner addObject:[_bodyLabel.topAnchor
                constraintEqualToAnchor:topAnchor.bottomAnchor constant:4]];
        } else {
            [inner addObject:[_bodyLabel.topAnchor
                constraintEqualToAnchor:_bubble.topAnchor constant:kBubblePadV]];
        }
        [inner addObjectsFromArray:@[
            [_bodyLabel.leadingAnchor  constraintEqualToAnchor:_bubble.leadingAnchor
                                                      constant:kBubblePadH],
            [_bodyLabel.trailingAnchor constraintEqualToAnchor:_bubble.trailingAnchor
                                                      constant:-kBubblePadH],
        ]];
        topAnchor = _bodyLabel;
    }

    BOOL hasFooter = !_timestampLabel.hidden || !_chipStack.hidden;
    if (hasFooter) {
        [inner addObjectsFromArray:@[
            [_chipStack.leadingAnchor constraintEqualToAnchor:_bubble.leadingAnchor
                                                     constant:kBubblePadH],
            [_chipStack.bottomAnchor  constraintEqualToAnchor:_bubble.bottomAnchor
                                                     constant:-4],
            [_timestampLabel.trailingAnchor constraintEqualToAnchor:_bubble.trailingAnchor
                                                           constant:-kBubblePadH],
            [_timestampLabel.centerYAnchor  constraintEqualToAnchor:_chipStack.centerYAnchor],
            [_timestampLabel.leadingAnchor  constraintGreaterThanOrEqualToAnchor:
                                              _chipStack.trailingAnchor constant:8],
        ]];
        if (topAnchor) {
            [inner addObject:[_chipStack.topAnchor
                constraintGreaterThanOrEqualToAnchor:topAnchor.bottomAnchor constant:2]];
        } else {
            [inner addObject:[_chipStack.topAnchor
                constraintEqualToAnchor:_bubble.topAnchor constant:kBubblePadV]];
        }
        topAnchor = _chipStack;
    }

    if (topAnchor) {
        [inner addObject:[_bubble.bottomAnchor
            constraintEqualToAnchor:topAnchor.bottomAnchor constant:kBubblePadV]];
    }

    _innerConstraints = inner;
    [NSLayoutConstraint activateConstraints:inner];

    // Outer layout — same for own and other.
    UIView* cv = self.contentView;
    _outerConstraints = @[
        [_avatarView.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor
                                                  constant:12],
        [_avatarView.bottomAnchor  constraintEqualToAnchor:cv.bottomAnchor
                                                  constant:-kRowPadV],
        [_avatarView.widthAnchor   constraintEqualToConstant:kAvatarSize],
        [_avatarView.heightAnchor  constraintEqualToConstant:kAvatarSize],

        [_senderLabel.leadingAnchor constraintEqualToAnchor:_avatarView.trailingAnchor
                                                   constant:kAvatarGap],
        [_senderLabel.topAnchor     constraintEqualToAnchor:cv.topAnchor
                                                   constant:kRowPadV],
        [_senderLabel.heightAnchor  constraintEqualToConstant:kSenderH],

        [_bubble.leadingAnchor  constraintEqualToAnchor:_avatarView.trailingAnchor
                                               constant:kAvatarGap],
        [_bubble.topAnchor      constraintEqualToAnchor:_senderLabel.bottomAnchor
                                               constant:2],
        [_bubble.bottomAnchor   constraintLessThanOrEqualToAnchor:cv.bottomAnchor
                                                         constant:-kRowPadV],
        [_bubble.widthAnchor    constraintLessThanOrEqualToConstant:maxBubbleW],
    ];
    [NSLayoutConstraint activateConstraints:_outerConstraints];
}

@end

// ── Controller ────────────────────────────────────────────────────────────────

@interface MessageListController () <UIScrollViewDelegate>
@end

@implementation MessageListController {
    std::vector<MessageData> _messages;
    NSMutableDictionary<NSString*, UIImage*>* _imageCache;
    NSMutableSet<NSString*>* _imageInflight;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStylePlain];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    _imageCache    = [NSMutableDictionary dictionary];
    _imageInflight = [NSMutableSet set];

    self.tableView.separatorStyle  = UITableViewCellSeparatorStyleNone;
    self.tableView.allowsSelection = NO;
    self.tableView.estimatedRowHeight = 60;
    [self.tableView registerClass:[BubbleCellView class]
           forCellReuseIdentifier:kCellId];
}

- (void)clearMessages {
    _messages.clear();
    [self.tableView reloadData];
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
                [self _prefetchChipIconsFor:_messages[i]];
                [self.tableView reloadRowsAtIndexPaths:
                    @[[NSIndexPath indexPathForRow:i inSection:0]]
                                       withRowAnimation:UITableViewRowAnimationNone];
                return;
            }
        }
    }

    MessageData md = makeMessageData(*ev, _myUserId);
    [self _prefetchAvatarFor:md];
    [self _prefetchMediaFor:md];
    [self _prefetchChipIconsFor:md];

    NSInteger row = (NSInteger)_messages.size();
    _messages.push_back(std::move(md));

    [self.tableView insertRowsAtIndexPaths:
        @[[NSIndexPath indexPathForRow:row inSection:0]]
                           withRowAnimation:UITableViewRowAnimationNone];

    [self _scrollToBottomIfNeeded];
}

// ── Async fetchers ────────────────────────────────────────────────────────────

- (void)_prefetchAvatarFor:(const MessageData&)msg {
    if (msg.is_own || msg.sender_avatar_url.empty() || !_client) return;
    NSString* key = @(msg.sender_avatar_url.c_str());
    if ([[AvatarCache shared] cachedImageForKey:key]) return;

    tesseract::Client* client = _client;
    std::string url = msg.sender_avatar_url;
    __weak typeof(self) weakSelf = self;
    [[AvatarCache shared] avatarForKey:key
                                 fetch:[client, url] {
                                     return client->fetch_media_bytes(url);
                                 }
                            completion:^(UIImage*) {
        [weakSelf _reloadRowsForSenderAvatarUrl:url];
    }];
}

- (void)_reloadRowsForSenderAvatarUrl:(std::string)url {
    NSMutableArray<NSIndexPath*>* idx = [NSMutableArray array];
    for (NSInteger i = 0; i < (NSInteger)_messages.size(); ++i)
        if (_messages[i].sender_avatar_url == url)
            [idx addObject:[NSIndexPath indexPathForRow:i inSection:0]];
    if (idx.count > 0)
        [self.tableView reloadRowsAtIndexPaths:idx
                              withRowAnimation:UITableViewRowAnimationNone];
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
        UIImage* img = nil;
        if (!bytes.empty()) {
            NSData* data = [NSData dataWithBytes:bytes.data()
                                          length:bytes.size()];
            img = [UIImage imageWithData:data];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) s = weakSelf;
            if (!s) return;
            [s->_imageInflight removeObject:key];
            if (!img) return;
            s->_imageCache[key] = img;

            NSMutableArray<NSIndexPath*>* idx = [NSMutableArray array];
            for (NSInteger i = 0; i < (NSInteger)s->_messages.size(); ++i)
                if (s->_messages[i].image_url == url)
                    [idx addObject:[NSIndexPath indexPathForRow:i inSection:0]];
            if (idx.count > 0)
                [s.tableView reloadRowsAtIndexPaths:idx
                                   withRowAnimation:UITableViewRowAnimationNone];
        });
    });
}

- (void)_prefetchChipIconsFor:(const MessageData&)msg {
    if (msg.reactions.empty() || !_client) return;
    tesseract::Client* client = _client;
    __weak typeof(self) weakSelf = self;
    for (const auto& r : msg.reactions) {
        if (r.source_json.empty()) continue;
        NSString* key = @(r.source_json.c_str());
        if (_imageCache[key]) continue;
        if ([_imageInflight containsObject:key]) continue;
        [_imageInflight addObject:key];

        std::string url = r.source_json;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            auto bytes = client->fetch_media_bytes(url);
            UIImage* img = nil;
            if (!bytes.empty()) {
                NSData* data = [NSData dataWithBytes:bytes.data()
                                              length:bytes.size()];
                img = [UIImage imageWithData:data];
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) s = weakSelf;
                if (!s) return;
                [s->_imageInflight removeObject:key];
                if (!img) return;
                s->_imageCache[key] = img;
                NSMutableArray<NSIndexPath*>* idx = [NSMutableArray array];
                for (NSInteger i = 0; i < (NSInteger)s->_messages.size(); ++i) {
                    for (const auto& rr : s->_messages[i].reactions) {
                        if (rr.source_json == url) {
                            [idx addObject:[NSIndexPath indexPathForRow:i inSection:0]];
                            break;
                        }
                    }
                }
                if (idx.count > 0)
                    [s.tableView reloadRowsAtIndexPaths:idx
                                       withRowAnimation:UITableViewRowAnimationNone];
            });
        });
    }
}

- (void)_scrollToBottomIfNeeded {
    CGFloat maxY = self.tableView.contentSize.height
                 - self.tableView.bounds.size.height;
    CGFloat curY = self.tableView.contentOffset.y;
    if (maxY - curY < 200 || _messages.size() <= 2) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (_messages.empty()) return;
            NSInteger last = (NSInteger)_messages.size() - 1;
            [self.tableView scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:last inSection:0]
                                  atScrollPosition:UITableViewScrollPositionBottom
                                          animated:NO];
        });
    }
}

// ── UIScrollViewDelegate — fire paginate-back trigger near the top. ──────────

- (void)scrollViewDidScroll:(UIScrollView*)scrollView {
    if (scrollView.contentOffset.y < 20)
        [_delegate messageListDidScrollToTop];
}

// ── UITableViewDataSource / Delegate ──────────────────────────────────────────

- (NSInteger)tableView:(UITableView*)tv numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)_messages.size();
}

- (UITableViewCell*)tableView:(UITableView*)tv
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    BubbleCellView* cell = (BubbleCellView*)[tv dequeueReusableCellWithIdentifier:kCellId
                                                                     forIndexPath:indexPath];
    const MessageData& m = _messages[indexPath.row];
    UIImage* media = nil;
    if (!m.image_url.empty())
        media = _imageCache[@(m.image_url.c_str())];

    NSMutableDictionary<NSString*, UIImage*>* chipIcons =
        [NSMutableDictionary dictionary];
    for (const auto& r : m.reactions) {
        if (r.source_json.empty()) continue;
        NSString* key = @(r.source_json.c_str());
        UIImage* img  = _imageCache[key];
        if (img) chipIcons[key] = img;
    }

    [cell configureWith:m
             tableWidth:tv.bounds.size.width
              myUserId:_myUserId
            mediaImage:media
          chipIcons:chipIcons
         chipTarget:self];

    return cell;
}

- (CGFloat)tableView:(UITableView*)tv heightForRowAtIndexPath:(NSIndexPath*)indexPath {
    const MessageData& msg = _messages[indexPath.row];

    CGFloat tableW = tv.bounds.size.width;
    CGFloat maxBubbleW = MIN(kMaxBubbleW, tableW - kAvatarSize - kAvatarGap - 32);
    CGFloat innerW = maxBubbleW - 2 * kBubblePadH;
    if (innerW < 50) innerW = 50;

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
        case tesseract::EventType::Redacted:
            bodyText = @"Message deleted";
            break;
        case tesseract::EventType::Text:
        default:
            bodyText = @(msg.body.c_str());
            break;
    }

    CGFloat bodyH = 0;
    if (bodyText.length > 0) {
        NSDictionary* attrs = @{ NSFontAttributeName: [UIFont systemFontOfSize:13] };
        CGRect b = [bodyText boundingRectWithSize:CGSizeMake(innerW, CGFLOAT_MAX)
                                          options:NSStringDrawingUsesLineFragmentOrigin
                                       attributes:attrs
                                          context:nil];
        bodyH = ceil(b.size.height);
    }

    CGFloat mediaH = 0;
    BOOL hasMedia = (msg.type == tesseract::EventType::Image ||
                     msg.type == tesseract::EventType::Sticker);
    if (hasMedia) {
        CGFloat maxW = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageW;
        CGFloat maxH = (msg.type == tesseract::EventType::Sticker)
                          ? kMaxStickerSz : kMaxImageH;
        UIImage* loaded = !msg.image_url.empty()
            ? _imageCache[@(msg.image_url.c_str())] : nil;
        CGSize sz = CGSizeZero;
        if (loaded) {
            sz = scaledImageSize(loaded.size, maxW, maxH);
        } else if (msg.image_w > 0 && msg.image_h > 0) {
            sz = scaledImageSize(CGSizeMake((CGFloat)msg.image_w,
                                            (CGFloat)msg.image_h), maxW, maxH);
        } else {
            sz = CGSizeMake(160, 120);
        }
        mediaH = sz.height;
    }

    CGFloat bubblePadV = 2 * kBubblePadV;
    CGFloat innerSpacing = (mediaH > 0 && bodyH > 0) ? 4 : 0;
    BOOL hasChips = !msg.reactions.empty();
    CGFloat footerInnerH = 0;
    if (hasChips) {
        footerInnerH = MAX(kChipH, kTsH);
    } else if (msg.timestamp > 0) {
        footerInnerH = kTsH;
    }
    CGFloat footerH = footerInnerH > 0 ? footerInnerH + 4 : 0;
    CGFloat senderH = kSenderH + 2;

    CGFloat bubbleContentH = mediaH + innerSpacing + bodyH + footerH;
    if (bubbleContentH == 0) bubbleContentH = 18;

    return senderH + bubblePadV + bubbleContentH + 2 * kRowPadV;
}

// ── Right-click / long-press "Delete message" context menu ───────────────────

- (UIContextMenuConfiguration*)tableView:(UITableView*)tv
    contextMenuConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath
                                        point:(CGPoint)point {
    if (indexPath.row < 0 || indexPath.row >= (NSInteger)_messages.size())
        return nil;
    const MessageData& m = _messages[indexPath.row];
    if (!m.is_own || m.type == tesseract::EventType::Redacted || m.event_id.empty())
        return nil;

    NSString* roomId  = @(m.room_id.c_str());
    NSString* eventId = @(m.event_id.c_str());
    __weak typeof(self) weakSelf = self;

    return [UIContextMenuConfiguration
        configurationWithIdentifier:nil
                    previewProvider:nil
                     actionProvider:^UIMenu*(NSArray<UIMenuElement*>* suggested) {
        UIAction* del = [UIAction
            actionWithTitle:@"Delete message"
                      image:[UIImage systemImageNamed:@"trash"]
                 identifier:nil
                    handler:^(__kindof UIAction* action) {
            [weakSelf _confirmDeleteRoom:roomId event:eventId];
        }];
        del.attributes = UIMenuElementAttributesDestructive;
        return [UIMenu menuWithChildren:@[del]];
    }];
}

- (void)_confirmDeleteRoom:(NSString*)roomId event:(NSString*)eventId {
    if (!_client || roomId.length == 0 || eventId.length == 0) return;

    UIAlertController* a = [UIAlertController
        alertControllerWithTitle:@"Delete message?"
                         message:@"This cannot be undone."
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                          style:UIAlertActionStyleCancel
                                        handler:nil]];
    __weak typeof(self) weakSelf = self;
    [a addAction:[UIAlertAction actionWithTitle:@"Delete"
                                          style:UIAlertActionStyleDestructive
                                        handler:^(UIAlertAction*) {
        typeof(self) s = weakSelf;
        if (!s || !s->_client) return;
        std::string rid(roomId.UTF8String);
        std::string eid(eventId.UTF8String);
        auto res = s->_client->redact_event(rid, eid, "");
        if (!res.ok) {
            UIAlertController* err = [UIAlertController
                alertControllerWithTitle:@"Delete failed"
                                 message:@(res.message.c_str())
                          preferredStyle:UIAlertControllerStyleAlert];
            [err addAction:[UIAlertAction actionWithTitle:@"OK"
                                                    style:UIAlertActionStyleDefault
                                                  handler:nil]];
            [s presentViewController:err animated:YES completion:nil];
        }
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)reactionChipClicked:(UIButton*)sender {
    if (!_client) return;
    NSString* roomId  = objc_getAssociatedObject(sender, kChipRoomIdKey);
    NSString* eventId = objc_getAssociatedObject(sender, kChipEventIdKey);
    NSString* key     = objc_getAssociatedObject(sender, kChipKeyKey);
    if (roomId.length == 0 || eventId.length == 0 || key.length == 0) return;

    std::string rid(roomId.UTF8String);
    std::string eid(eventId.UTF8String);
    std::string k  (key.UTF8String);
    auto result = _client->send_reaction(rid, eid, k);
    (void)result;
}

@end
