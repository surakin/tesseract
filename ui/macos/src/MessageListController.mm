#import "MessageListController.h"
#import "AvatarCache.h"

static NSString* const kCellId     = @"MsgCell";
static const CGFloat kAvatarSize   = 28;
static const CGFloat kBubblePadH   = 10;
static const CGFloat kBubblePadV   = 7;
static const CGFloat kRowPadV      = 6;
static const CGFloat kMaxBubbleW   = 420;
static const CGFloat kSenderH      = 16;
static const CGFloat kAvatarGap    = 8;
static const CGFloat kCornerRadius = 12;

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
};

// ── Bubble cell view ──────────────────────────────────────────────────────────

@interface BubbleCellView : NSView
- (void)configureWith:(const MessageData&)msg
           tableWidth:(CGFloat)w
            myUserId:(NSString*)myUserId;
@end

@implementation BubbleCellView {
    NSImageView* _avatarView;
    NSTextField* _senderLabel;
    NSTextField* _bodyLabel;
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

    _bodyLabel = [NSTextField wrappingLabelWithString:@""];
    _bodyLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _bodyLabel.font      = [NSFont systemFontOfSize:13];
    _bodyLabel.selectable = YES;
    [self addSubview:_bodyLabel];

    return self;
}

- (void)configureWith:(const MessageData&)msg
           tableWidth:(CGFloat)tableW
            myUserId:(NSString*)myUserId {
    BOOL own = msg.is_own;
    NSString* body   = @(msg.body.c_str());
    NSString* sender = @(msg.sender_name.empty()
                           ? msg.sender.c_str()
                           : msg.sender_name.c_str());

    // Colours
    if (own) {
        _bubble.layer.backgroundColor = [NSColor systemBlueColor].CGColor;
        _bodyLabel.textColor          = [NSColor whiteColor];
    } else {
        _bubble.layer.backgroundColor =
            [NSColor colorNamed:@"BubbleGray"]
            ?: [NSColor colorWithRed:0.91 green:0.91 blue:0.91 alpha:1];
        _bodyLabel.textColor = [NSColor labelColor];
    }

    _bodyLabel.stringValue   = body;
    _senderLabel.stringValue = own ? @"" : sender;
    _senderLabel.hidden      = own;
    _avatarView.hidden       = own;

    if (!own) {
        NSString* key = @(msg.sender_avatar_url.c_str());
        NSString* name = sender;
        NSImage* cached = [[AvatarCache shared] cachedImageForKey:key];
        _avatarView.image = cached
            ?: [AvatarCache initialsImageForName:name size:kAvatarSize];
    }

    // Remove old constraints before re-applying
    [self removeConstraints:self.constraints];
    [_bubble removeConstraints:_bubble.constraints];

    CGFloat maxBubbleW = MIN(kMaxBubbleW, tableW - kAvatarSize - kAvatarGap - 32);

    if (own) {
        // Right-aligned, no avatar
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

            [_bodyLabel.leadingAnchor  constraintEqualToAnchor:_bubble.leadingAnchor
                                                      constant:kBubblePadH],
            [_bodyLabel.trailingAnchor constraintEqualToAnchor:_bubble.trailingAnchor
                                                      constant:-kBubblePadH],
            [_bodyLabel.topAnchor      constraintEqualToAnchor:_bubble.topAnchor
                                                      constant:kBubblePadV],
            [_bodyLabel.bottomAnchor   constraintEqualToAnchor:_bubble.bottomAnchor
                                                      constant:-kBubblePadV],
        ]];
    } else {
        // Left-aligned with avatar and sender name
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

            [_bodyLabel.leadingAnchor  constraintEqualToAnchor:_bubble.leadingAnchor
                                                      constant:kBubblePadH],
            [_bodyLabel.trailingAnchor constraintEqualToAnchor:_bubble.trailingAnchor
                                                      constant:-kBubblePadH],
            [_bodyLabel.topAnchor      constraintEqualToAnchor:_bubble.topAnchor
                                                      constant:kBubblePadV],
            [_bodyLabel.bottomAnchor   constraintEqualToAnchor:_bubble.bottomAnchor
                                                      constant:-kBubblePadV],
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
    BOOL                    _pendingScrollToBottom;
}

- (void)loadView {
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
                _messages[i].body              = ev->body;
                _messages[i].sender_name       = ev->sender_name;
                _messages[i].sender_avatar_url = ev->sender_avatar_url;
                [_table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:i]
                                  columnIndexes:[NSIndexSet indexSetWithIndex:0]];
                return;
            }
        }
    }

    MessageData md;
    md.event_id         = ev->event_id;
    md.sender           = ev->sender;
    md.sender_name      = ev->sender_name;
    md.sender_avatar_url= ev->sender_avatar_url;
    md.body             = ev->body;
    md.timestamp        = ev->timestamp;
    md.type             = ev->type;
    md.is_own           = _myUserId
        && [@(ev->sender.c_str()) isEqualToString:_myUserId];

    NSInteger row = (NSInteger)_messages.size();
    _messages.push_back(std::move(md));

    [_table beginUpdates];
    [_table insertRowsAtIndexes:[NSIndexSet indexSetWithIndex:row]
                  withAnimation:NSTableViewAnimationEffectNone];
    [_table endUpdates];

    [self _scrollToBottomIfNeeded];
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
    [cell configureWith:_messages[row]
             tableWidth:tv.bounds.size.width
              myUserId:_myUserId];
    return cell;
}

- (CGFloat)tableView:(NSTableView*)tv heightOfRow:(NSInteger)row {
    const MessageData& msg = _messages[row];
    NSString* body = @(msg.body.c_str());
    BOOL own = msg.is_own;

    CGFloat tableW  = tv.bounds.size.width;
    CGFloat maxW    = MIN(kMaxBubbleW, tableW - kAvatarSize - kAvatarGap - 32)
                      - 2 * kBubblePadH;

    NSDictionary* attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:13] };
    NSRect bounds = [body boundingRectWithSize:NSMakeSize(maxW, CGFLOAT_MAX)
                                       options:NSStringDrawingUsesLineFragmentOrigin
                                    attributes:attrs
                                       context:nil];
    CGFloat bodyH    = ceil(bounds.size.height);
    CGFloat senderH  = own ? 0 : kSenderH + 2;
    return senderH + bodyH + 2 * kBubblePadV + 2 * kRowPadV;
}

@end
