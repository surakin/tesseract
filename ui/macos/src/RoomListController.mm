#import "RoomListController.h"
#import "AvatarCache.h"

#include <tesseract/client.h>
#include <tesseract/visual.h>

static const CGFloat kRowHeight   = tesseract::visual::kRoomRowHeight;
static const CGFloat kAvatarSize  = tesseract::visual::kRoomAvatarSize;
static const CGFloat kPadH        = 10;
static const CGFloat kPadV        = 6;
static NSString* const kCellId    = @"RoomCell";

// ── Custom cell view ─────────────────────────────────────────────────────────

@interface RoomCellView : UITableViewCell
@property (nonatomic, strong) UIImageView* avatarView;
@property (nonatomic, strong) UILabel*     nameLabel;
@property (nonatomic, strong) UILabel*     previewLabel;
@property (nonatomic, strong) UILabel*     badgeLabel;
@end

@implementation RoomCellView

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString*)reuseIdentifier {
    if (!(self = [super initWithStyle:style reuseIdentifier:reuseIdentifier])) return nil;

    _avatarView = [[UIImageView alloc] init];
    _avatarView.translatesAutoresizingMaskIntoConstraints = NO;
    _avatarView.contentMode    = UIViewContentModeScaleAspectFill;
    _avatarView.clipsToBounds  = YES;
    _avatarView.layer.cornerRadius = kAvatarSize / 2;
    [self.contentView addSubview:_avatarView];

    _nameLabel = [[UILabel alloc] init];
    _nameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _nameLabel.font          = [UIFont boldSystemFontOfSize:13];
    _nameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.contentView addSubview:_nameLabel];

    _previewLabel = [[UILabel alloc] init];
    _previewLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _previewLabel.font          = [UIFont systemFontOfSize:11];
    _previewLabel.textColor     = [UIColor secondaryLabelColor];
    _previewLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [self.contentView addSubview:_previewLabel];

    _badgeLabel = [[UILabel alloc] init];
    _badgeLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _badgeLabel.font             = [UIFont boldSystemFontOfSize:11];
    _badgeLabel.textColor        = [UIColor whiteColor];
    _badgeLabel.textAlignment    = NSTextAlignmentCenter;
    _badgeLabel.backgroundColor  = [UIColor systemBlueColor];
    _badgeLabel.layer.cornerRadius  = 9;
    _badgeLabel.layer.masksToBounds = YES;
    _badgeLabel.hidden           = YES;
    [self.contentView addSubview:_badgeLabel];

    CGFloat textX = kPadH + kAvatarSize + kPadH;
    UIView* cv = self.contentView;
    [NSLayoutConstraint activateConstraints:@[
        // Avatar
        [_avatarView.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor
                                                   constant:kPadH],
        [_avatarView.centerYAnchor  constraintEqualToAnchor:cv.centerYAnchor],
        [_avatarView.widthAnchor    constraintEqualToConstant:kAvatarSize],
        [_avatarView.heightAnchor   constraintEqualToConstant:kAvatarSize],

        // Badge (top-right of avatar)
        [_badgeLabel.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor
                                                   constant:-kPadH],
        [_badgeLabel.centerYAnchor  constraintEqualToAnchor:cv.centerYAnchor],
        [_badgeLabel.widthAnchor    constraintGreaterThanOrEqualToConstant:18],
        [_badgeLabel.heightAnchor   constraintEqualToConstant:18],

        // Room name
        [_nameLabel.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor
                                                  constant:textX],
        [_nameLabel.trailingAnchor constraintEqualToAnchor:_badgeLabel.leadingAnchor
                                                  constant:-4],
        [_nameLabel.topAnchor      constraintEqualToAnchor:cv.topAnchor
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
    // Display order; pointers refer to `_rooms` storage which lives for the
    // lifetime of the row. Rebuilt every updateRooms:.
    std::vector<tesseract::RoomInfo> _rooms;
    NSString*                       _selectedRoomId;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStylePlain];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.rowHeight       = kRowHeight;
    self.tableView.separatorStyle  = UITableViewCellSeparatorStyleNone;
    self.tableView.allowsSelection = YES;
    [self.tableView registerClass:[RoomCellView class]
           forCellReuseIdentifier:kCellId];
}

- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms {
    // Sort: regular rooms first, spaces at the bottom.
    std::vector<tesseract::RoomInfo> sorted;
    sorted.reserve(rooms.size());
    for (const auto& r : rooms) if (!r.is_space) sorted.push_back(r);
    for (const auto& r : rooms) if ( r.is_space) sorted.push_back(r);
    _rooms = std::move(sorted);
    [self.tableView reloadData];

    // Restore selection if the room is still present.
    if (_selectedRoomId) {
        for (NSInteger i = 0; i < (NSInteger)_rooms.size(); ++i) {
            if ([@(_rooms[i].id.c_str()) isEqualToString:_selectedRoomId]) {
                [self.tableView
                    selectRowAtIndexPath:[NSIndexPath indexPathForRow:i inSection:0]
                                animated:NO
                          scrollPosition:UITableViewScrollPositionNone];
                break;
            }
        }
    }
}

- (NSInteger)tableView:(UITableView*)tv numberOfRowsInSection:(NSInteger)section {
    return (NSInteger)_rooms.size();
}

- (UITableViewCell*)tableView:(UITableView*)tv
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    RoomCellView* cell = (RoomCellView*)[tv dequeueReusableCellWithIdentifier:kCellId
                                                                 forIndexPath:indexPath];
    const tesseract::RoomInfo& room = _rooms[indexPath.row];
    NSString* roomId  = @(room.id.c_str());
    NSString* rawName = @(room.name.c_str());
    NSString* display = room.is_space
        ? [@"# " stringByAppendingString:rawName]
        : rawName;

    cell.nameLabel.text    = display;
    cell.previewLabel.text = @(room.last_message_body.c_str());

    if (room.unread_count > 0) {
        cell.badgeLabel.hidden = NO;
        cell.badgeLabel.text   = @(room.unread_count).stringValue;
    } else {
        cell.badgeLabel.hidden = YES;
    }

    NSString* key = !room.avatar_url.empty()
        ? @(room.avatar_url.c_str())
        : roomId;
    UIImage* cached = [[AvatarCache shared] cachedImageForKey:key];
    if (cached) {
        cell.avatarView.image = cached;
    } else {
        cell.avatarView.image = [AvatarCache initialsImageForName:rawName
                                                             size:kAvatarSize];
        if (_client && !room.avatar_url.empty()) {
            tesseract::Client* client = _client;
            std::string room_id = room.id;
            __weak UITableView* weakTable = self.tableView;
            __weak typeof(self) weakSelf  = self;
            [[AvatarCache shared] avatarForKey:key
                                         fetch:[client, room_id] {
                                             return client->fetch_avatar_bytes(room_id);
                                         }
                                    completion:^(UIImage*) {
                UITableView* t = weakTable;
                typeof(self) s = weakSelf;
                if (!t || !s) return;
                NSInteger idx = NSNotFound;
                for (NSInteger i = 0; i < (NSInteger)s->_rooms.size(); ++i) {
                    if (s->_rooms[i].id == room_id) { idx = i; break; }
                }
                if (idx != NSNotFound) {
                    [t reloadRowsAtIndexPaths:@[[NSIndexPath indexPathForRow:idx inSection:0]]
                             withRowAnimation:UITableViewRowAnimationNone];
                }
            }];
        }
    }

    return cell;
}

- (void)tableView:(UITableView*)tv didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    if (indexPath.row < 0 || indexPath.row >= (NSInteger)_rooms.size()) return;
    const tesseract::RoomInfo& room = _rooms[indexPath.row];
    NSString* roomId = @(room.id.c_str());
    if (room.is_space) {
        if ([_delegate respondsToSelector:@selector(roomListDidSelectSpaceId:)])
            [_delegate roomListDidSelectSpaceId:roomId];
        // Clear selection so re-entering the same space is detected.
        [tv deselectRowAtIndexPath:indexPath animated:NO];
        return;
    }
    _selectedRoomId = roomId;
    [_delegate roomListDidSelectRoomId:roomId];
}

@end
