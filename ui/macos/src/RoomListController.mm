#import "RoomListController.h"
#import "AvatarCache.h"

static const CGFloat kRowHeight   = 64;
static const CGFloat kAvatarSize  = 36;
static const CGFloat kPadH        = 10;
static const CGFloat kPadV        = 6;
static NSString* const kCellId    = @"RoomCell";

// ── Custom cell view ─────────────────────────────────────────────────────────

@interface RoomCellView : NSTableCellView
@property (nonatomic, strong) NSImageView* avatarView;
@property (nonatomic, strong) NSTextField* nameLabel;
@property (nonatomic, strong) NSTextField* previewLabel;
@property (nonatomic, strong) NSTextField* badgeLabel;
@end

@implementation RoomCellView

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;

    _avatarView = [[NSImageView alloc] init];
    _avatarView.translatesAutoresizingMaskIntoConstraints = NO;
    _avatarView.wantsLayer    = YES;
    _avatarView.imageScaling  = NSImageScaleAxesIndependently;
    _avatarView.layer.cornerRadius  = kAvatarSize / 2;
    _avatarView.layer.masksToBounds = YES;
    [self addSubview:_avatarView];

    _nameLabel = [NSTextField labelWithString:@""];
    _nameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _nameLabel.font           = [NSFont boldSystemFontOfSize:13];
    _nameLabel.lineBreakMode  = NSLineBreakByTruncatingTail;
    [self addSubview:_nameLabel];

    _previewLabel = [NSTextField labelWithString:@""];
    _previewLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _previewLabel.font           = [NSFont systemFontOfSize:11];
    _previewLabel.textColor      = [NSColor secondaryLabelColor];
    _previewLabel.lineBreakMode  = NSLineBreakByTruncatingTail;
    [self addSubview:_previewLabel];

    _badgeLabel = [NSTextField labelWithString:@""];
    _badgeLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _badgeLabel.font             = [NSFont boldSystemFontOfSize:11];
    _badgeLabel.textColor        = [NSColor whiteColor];
    _badgeLabel.alignment        = NSTextAlignmentCenter;
    _badgeLabel.wantsLayer       = YES;
    _badgeLabel.layer.cornerRadius  = 9;
    _badgeLabel.layer.masksToBounds = YES;
    _badgeLabel.hidden           = YES;
    [self addSubview:_badgeLabel];

    CGFloat textX = kPadH + kAvatarSize + kPadH;
    [NSLayoutConstraint activateConstraints:@[
        // Avatar
        [_avatarView.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor
                                                   constant:kPadH],
        [_avatarView.centerYAnchor  constraintEqualToAnchor:self.centerYAnchor],
        [_avatarView.widthAnchor    constraintEqualToConstant:kAvatarSize],
        [_avatarView.heightAnchor   constraintEqualToConstant:kAvatarSize],

        // Badge (top-right of avatar)
        [_badgeLabel.trailingAnchor constraintEqualToAnchor:self.trailingAnchor
                                                   constant:-kPadH],
        [_badgeLabel.centerYAnchor  constraintEqualToAnchor:self.centerYAnchor],
        [_badgeLabel.widthAnchor    constraintGreaterThanOrEqualToConstant:18],
        [_badgeLabel.heightAnchor   constraintEqualToConstant:18],

        // Room name
        [_nameLabel.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor
                                                  constant:textX],
        [_nameLabel.trailingAnchor constraintEqualToAnchor:_badgeLabel.leadingAnchor
                                                  constant:-4],
        [_nameLabel.topAnchor      constraintEqualToAnchor:self.topAnchor
                                                  constant:kPadV + 6],

        // Preview
        [_previewLabel.leadingAnchor  constraintEqualToAnchor:_nameLabel.leadingAnchor],
        [_previewLabel.trailingAnchor constraintEqualToAnchor:_badgeLabel.leadingAnchor
                                                     constant:-4],
        [_previewLabel.topAnchor      constraintEqualToAnchor:_nameLabel.bottomAnchor
                                                     constant:2],
    ]];
    return self;
}

@end

// ── Controller ───────────────────────────────────────────────────────────────

@implementation RoomListController {
    NSTableView*                    _table;
    NSScrollView*                   _scroll;
    std::vector<tesseract::RoomInfo> _rooms;
    NSString*                       _selectedRoomId;
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
    _table.rowHeight              = kRowHeight;
    _table.selectionHighlightStyle = NSTableViewSelectionHighlightStyleSourceList;
    _table.allowsEmptySelection   = YES;
    _table.allowsMultipleSelection = NO;
    _table.intercellSpacing       = NSMakeSize(0, 0);

    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"Room"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _scroll.documentView = _table;

    self.view = _scroll;
}

- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms {
    _rooms = std::move(rooms);
    [_table reloadData];

    // Restore selection if the room is still present.
    if (_selectedRoomId) {
        for (NSInteger i = 0; i < (NSInteger)_rooms.size(); ++i) {
            if ([@(_rooms[i].id.c_str()) isEqualToString:_selectedRoomId]) {
                [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:i]
                    byExtendingSelection:NO];
                break;
            }
        }
    }
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)_rooms.size();
}

- (NSView*)tableView:(NSTableView*)tv
  viewForTableColumn:(NSTableColumn*)col
                 row:(NSInteger)row {
    RoomCellView* cell = [tv makeViewWithIdentifier:kCellId owner:self];
    if (!cell) {
        cell = [[RoomCellView alloc] initWithFrame:NSMakeRect(0, 0, 200, kRowHeight)];
        cell.identifier = kCellId;
    }

    const tesseract::RoomInfo& room = _rooms[row];
    NSString* roomId = @(room.id.c_str());
    NSString* name   = @(room.name.c_str());

    cell.nameLabel.stringValue    = name;
    cell.previewLabel.stringValue = @(room.last_message_body.c_str());

    if (room.unread_count > 0) {
        cell.badgeLabel.hidden        = NO;
        cell.badgeLabel.stringValue   = @(room.unread_count).stringValue;
        cell.badgeLabel.layer.backgroundColor =
            [NSColor systemBlueColor].CGColor;
    } else {
        cell.badgeLabel.hidden = YES;
    }

    // Avatar: use cached image or kick off a background fetch.
    NSImage* cached = [[AvatarCache shared] cachedImageForKey:roomId];
    if (cached) {
        cell.avatarView.image = cached;
    } else {
        cell.avatarView.image = [AvatarCache initialsImageForName:name
                                                             size:kAvatarSize];
    }

    return cell;
}

- (CGFloat)tableView:(NSTableView*)tv heightOfRow:(NSInteger)row {
    return kRowHeight;
}

- (void)tableViewSelectionDidChange:(NSNotification*)n {
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_rooms.size()) return;
    _selectedRoomId = @(_rooms[row].id.c_str());
    [_delegate roomListDidSelectRoomId:_selectedRoomId];
}

@end
