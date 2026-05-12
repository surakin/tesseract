#import "MainViewController.h"
#import "LoginView.h"
#import "RoomListController.h"
#import "MessageListController.h"
#import "ComposeBar.h"
#import "EventBridge.h"
#import "AvatarCache.h"

#include <tesseract/client.h>
#include <tesseract/session_store.h>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string nsstr(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string{};
}

// ── User strip: a UIView that surfaces a "Logout" menu on right-click /
// long-press via UIContextMenuInteraction (Catalyst Mac idiom).
// ─────────────────────────────────────────────────────────────────────────────

@class MainViewController;

@interface UserStripView : UIView <UIContextMenuInteractionDelegate>
@property (nonatomic, weak) MainViewController* owner;
@end

// ── Controller ────────────────────────────────────────────────────────────────

@interface MainViewController ()
    <RoomListDelegate, MessageListDelegate, LoginViewDelegate>
- (void)_doLogout;
@end

@implementation MainViewController {
    // C++ members — constructed in init, destroyed in dealloc.
    struct Impl {
        tesseract::Client        client;
        std::unique_ptr<EventBridge> bridge;
    };
    std::unique_ptr<Impl> _impl;

    // Child controllers
    RoomListController*    _roomList;
    MessageListController* _msgList;
    ComposeBar*            _compose;
    LoginView*             _loginView;
    NSArray<UIView*>*      _mainContentViews;  // toggled vs _loginView visibility

    // User identity strip (sidebar footer)
    UserStripView*       _userStrip;
    UIImageView*         _userAvatar;
    UILabel*             _userNameLabel;
    std::string          _myDisplayName;
    std::string          _myAvatarUrl;

    // Recovery banner — inline key entry, no modal dialog.
    UIView*              _recoveryBanner;
    UILabel*             _recoveryLabel;
    UITextField*         _recoveryKeyField;
    UIButton*            _recoveryVerifyBtn;
    NSLayoutConstraint*  _recoveryBannerHeight;
    BOOL                 _recoveryBannerDismissed;
    BOOL                 _recoveryInFlight;

    // State
    NSString*    _currentRoomId;
    NSString*    _myUserId;
    UILabel*     _statusLabel;
    std::vector<tesseract::RoomInfo> _rooms;
    std::vector<std::string>         _spaceStack;

    // Sidebar nav bar (visible when inside a space)
    UIView*      _sidebarContainer;
    UIView*      _navBar;
    UIButton*    _backButton;
    UILabel*     _spaceNameLabel;

    // Room header
    UIView*      _msgContainer;
    UIView*      _roomHeaderView;
    UIImageView* _roomHeaderAvatar;
    UILabel*     _roomHeaderName;
    UILabel*     _roomHeaderTopic;
}

- (instancetype)init {
    if (!(self = [super initWithNibName:nil bundle:nil])) return nil;
    _impl = std::make_unique<Impl>();
    self.title = @"Tesseract";
    // Catalyst respects preferredContentSize for window-default sizing on
    // the Mac idiom.
    self.preferredContentSize = CGSizeMake(900, 620);
    return self;
}

- (void)dealloc {
    // _loginView holds a non-owning pointer to _impl->client and calls
    // cancel_oauth() + joins its worker on dealloc. Release it explicitly
    // before _impl unwinds at the end of -dealloc.
    [_loginView removeFromSuperview];
    _loginView = nil;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];
    [self _buildUI];
}

// ── UI construction ───────────────────────────────────────────────────────────

- (void)_buildUI {
    UIView* content = self.view;

    // ── Status bar at the bottom ──────────────────────────────────────────────
    _statusLabel = [[UILabel alloc] init];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.text          = @"Not connected";
    _statusLabel.textColor     = [UIColor secondaryLabelColor];
    _statusLabel.font          = [UIFont systemFontOfSize:11];
    _statusLabel.textAlignment = NSTextAlignmentCenter;
    [content addSubview:_statusLabel];

    UIView* statusSep = [self _makeSeparator];
    [content addSubview:statusSep];

    // ── Compose bar ───────────────────────────────────────────────────────────
    _compose = [[ComposeBar alloc] initWithFrame:CGRectZero];
    _compose.translatesAutoresizingMaskIntoConstraints = NO;
    _compose.client = &_impl->client;
    __weak typeof(self) weakSelf = self;
    _compose.onSend = ^(NSString* body) {
        [weakSelf _sendMessage:body];
    };
    [content addSubview:_compose];

    // ── Sidebar (nav bar + room list) ─────────────────────────────────────────
    _sidebarContainer = [[UIView alloc] init];
    _sidebarContainer.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_sidebarContainer];

    _navBar = [[UIView alloc] init];
    _navBar.translatesAutoresizingMaskIntoConstraints = NO;
    _navBar.hidden = YES;
    [_sidebarContainer addSubview:_navBar];

    _backButton = [UIButton buttonWithType:UIButtonTypeSystem];
    _backButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_backButton setTitle:@"←" forState:UIControlStateNormal];
    [_backButton addTarget:self
                    action:@selector(_onBackClicked)
          forControlEvents:UIControlEventTouchUpInside];
    [_navBar addSubview:_backButton];

    _spaceNameLabel = [[UILabel alloc] init];
    _spaceNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _spaceNameLabel.font          = [UIFont boldSystemFontOfSize:12];
    _spaceNameLabel.textColor     = [UIColor labelColor];
    _spaceNameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_navBar addSubview:_spaceNameLabel];

    UIView* navSep = [self _makeSeparator];
    [_navBar addSubview:navSep];

    _roomList = [[RoomListController alloc] init];
    _roomList.delegate = self;
    _roomList.client   = &_impl->client;
    [self addChildViewController:_roomList];
    _roomList.view.translatesAutoresizingMaskIntoConstraints = NO;
    [_sidebarContainer addSubview:_roomList.view];
    [_roomList didMoveToParentViewController:self];

    // ── User identity strip (sidebar footer) ──────────────────────────────────
    _userStrip = [[UserStripView alloc] init];
    _userStrip.owner = self;
    _userStrip.translatesAutoresizingMaskIntoConstraints = NO;
    _userStrip.backgroundColor = [UIColor secondarySystemBackgroundColor];
    _userStrip.hidden = YES;
    [_sidebarContainer addSubview:_userStrip];

    _userAvatar = [[UIImageView alloc] init];
    _userAvatar.translatesAutoresizingMaskIntoConstraints = NO;
    _userAvatar.contentMode = UIViewContentModeScaleAspectFill;
    _userAvatar.clipsToBounds = YES;
    _userAvatar.layer.cornerRadius = 16;
    [_userStrip addSubview:_userAvatar];

    _userNameLabel = [[UILabel alloc] init];
    _userNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _userNameLabel.font          = [UIFont boldSystemFontOfSize:13];
    _userNameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_userStrip addSubview:_userNameLabel];

    // ── Message container: header + recovery banner + message list ────────────
    _msgList = [[MessageListController alloc] init];
    _msgList.delegate = self;
    _msgList.client   = &_impl->client;
    [self addChildViewController:_msgList];

    _msgContainer = [[UIView alloc] init];
    _msgContainer.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_msgContainer];

    // Room header bar
    _roomHeaderView = [[UIView alloc] init];
    _roomHeaderView.translatesAutoresizingMaskIntoConstraints = NO;
    _roomHeaderView.backgroundColor = [UIColor secondarySystemBackgroundColor];
    [_msgContainer addSubview:_roomHeaderView];

    // Avatar
    _roomHeaderAvatar = [[UIImageView alloc] init];
    _roomHeaderAvatar.translatesAutoresizingMaskIntoConstraints = NO;
    _roomHeaderAvatar.contentMode    = UIViewContentModeScaleAspectFill;
    _roomHeaderAvatar.clipsToBounds  = YES;
    _roomHeaderAvatar.layer.cornerRadius = 20;
    [_roomHeaderView addSubview:_roomHeaderAvatar];

    // Name label
    _roomHeaderName = [[UILabel alloc] init];
    _roomHeaderName.translatesAutoresizingMaskIntoConstraints = NO;
    _roomHeaderName.font          = [UIFont boldSystemFontOfSize:15];
    _roomHeaderName.textColor     = [UIColor labelColor];
    _roomHeaderName.lineBreakMode = NSLineBreakByTruncatingTail;
    [_roomHeaderView addSubview:_roomHeaderName];

    // Topic label
    _roomHeaderTopic = [[UILabel alloc] init];
    _roomHeaderTopic.translatesAutoresizingMaskIntoConstraints = NO;
    _roomHeaderTopic.font          = [UIFont systemFontOfSize:12];
    _roomHeaderTopic.textColor     = [UIColor secondaryLabelColor];
    _roomHeaderTopic.lineBreakMode = NSLineBreakByTruncatingTail;
    _roomHeaderTopic.hidden        = YES;
    [_roomHeaderView addSubview:_roomHeaderTopic];

    // Recovery banner — hidden until needs_recovery() is true.
    _recoveryBanner = [[UIView alloc] init];
    _recoveryBanner.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryBanner.backgroundColor =
        [UIColor colorWithRed:1.0 green:0.96 blue:0.84 alpha:1.0];
    _recoveryBanner.hidden = YES;
    [_msgContainer addSubview:_recoveryBanner];

    _recoveryLabel = [[UILabel alloc] init];
    _recoveryLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryLabel.text          = @"Verify this device:";
    _recoveryLabel.textColor     =
        [UIColor colorWithRed:0.36 green:0.27 blue:0.0 alpha:1.0];
    _recoveryLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_recoveryBanner addSubview:_recoveryLabel];

    _recoveryKeyField = [[UITextField alloc] init];
    _recoveryKeyField.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryKeyField.placeholder       = @"Recovery key or passphrase";
    _recoveryKeyField.secureTextEntry   = YES;
    _recoveryKeyField.borderStyle       = UITextBorderStyleRoundedRect;
    _recoveryKeyField.returnKeyType     = UIReturnKeyGo;
    [_recoveryKeyField addTarget:self
                          action:@selector(_onRecoveryVerifyClicked)
                forControlEvents:UIControlEventEditingDidEndOnExit];
    [_recoveryBanner addSubview:_recoveryKeyField];

    _recoveryVerifyBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    _recoveryVerifyBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [_recoveryVerifyBtn setTitle:@"Verify" forState:UIControlStateNormal];
    [_recoveryVerifyBtn addTarget:self
                           action:@selector(_onRecoveryVerifyClicked)
                 forControlEvents:UIControlEventTouchUpInside];
    [_recoveryBanner addSubview:_recoveryVerifyBtn];

    UIButton* dismissBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    dismissBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [dismissBtn setTitle:@"✕" forState:UIControlStateNormal];
    [dismissBtn addTarget:self
                   action:@selector(_onRecoveryDismissClicked)
         forControlEvents:UIControlEventTouchUpInside];
    [_recoveryBanner addSubview:dismissBtn];

    // Message list fills remaining space below the (possibly-hidden) banner.
    _msgList.view.translatesAutoresizingMaskIntoConstraints = NO;
    [_msgContainer addSubview:_msgList.view];
    [_msgList didMoveToParentViewController:self];

    _roomHeaderView.hidden = YES;

    // ── Auto Layout ───────────────────────────────────────────────────────────
    UILayoutGuide* safe = content.safeAreaLayoutGuide;

    [NSLayoutConstraint activateConstraints:@[
        // Sidebar fills the top-left of the safe area, fixed 220 wide.
        [_sidebarContainer.topAnchor     constraintEqualToAnchor:safe.topAnchor],
        [_sidebarContainer.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [_sidebarContainer.bottomAnchor  constraintEqualToAnchor:_compose.topAnchor],
        [_sidebarContainer.widthAnchor   constraintEqualToConstant:220],

        // Message container fills the area to the right of the sidebar.
        [_msgContainer.topAnchor      constraintEqualToAnchor:safe.topAnchor],
        [_msgContainer.leadingAnchor  constraintEqualToAnchor:_sidebarContainer.trailingAnchor],
        [_msgContainer.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_msgContainer.bottomAnchor   constraintEqualToAnchor:_compose.topAnchor],

        // Nav bar at top of sidebar (hidden by default)
        [_navBar.topAnchor      constraintEqualToAnchor:_sidebarContainer.topAnchor],
        [_navBar.leadingAnchor  constraintEqualToAnchor:_sidebarContainer.leadingAnchor],
        [_navBar.trailingAnchor constraintEqualToAnchor:_sidebarContainer.trailingAnchor],
        [_navBar.heightAnchor   constraintEqualToConstant:36],

        [_backButton.leadingAnchor constraintEqualToAnchor:_navBar.leadingAnchor constant:6],
        [_backButton.centerYAnchor constraintEqualToAnchor:_navBar.centerYAnchor],
        [_backButton.widthAnchor   constraintEqualToConstant:32],

        [_spaceNameLabel.leadingAnchor  constraintEqualToAnchor:_backButton.trailingAnchor constant:6],
        [_spaceNameLabel.trailingAnchor constraintEqualToAnchor:_navBar.trailingAnchor constant:-6],
        [_spaceNameLabel.centerYAnchor  constraintEqualToAnchor:_navBar.centerYAnchor],

        [navSep.leadingAnchor  constraintEqualToAnchor:_navBar.leadingAnchor],
        [navSep.trailingAnchor constraintEqualToAnchor:_navBar.trailingAnchor],
        [navSep.bottomAnchor   constraintEqualToAnchor:_navBar.bottomAnchor],
        [navSep.heightAnchor   constraintEqualToConstant:1],

        // Room list fills sidebar below the (possibly-hidden) nav bar, above
        // the user strip pinned to the bottom.
        [_roomList.view.topAnchor      constraintEqualToAnchor:_navBar.bottomAnchor],
        [_roomList.view.leadingAnchor  constraintEqualToAnchor:_sidebarContainer.leadingAnchor],
        [_roomList.view.trailingAnchor constraintEqualToAnchor:_sidebarContainer.trailingAnchor],
        [_roomList.view.bottomAnchor   constraintEqualToAnchor:_userStrip.topAnchor],

        [_userStrip.leadingAnchor  constraintEqualToAnchor:_sidebarContainer.leadingAnchor],
        [_userStrip.trailingAnchor constraintEqualToAnchor:_sidebarContainer.trailingAnchor],
        [_userStrip.bottomAnchor   constraintEqualToAnchor:_sidebarContainer.bottomAnchor],
        [_userStrip.heightAnchor   constraintEqualToConstant:48],

        [_userAvatar.leadingAnchor constraintEqualToAnchor:_userStrip.leadingAnchor constant:8],
        [_userAvatar.centerYAnchor constraintEqualToAnchor:_userStrip.centerYAnchor],
        [_userAvatar.widthAnchor   constraintEqualToConstant:32],
        [_userAvatar.heightAnchor  constraintEqualToConstant:32],

        [_userNameLabel.leadingAnchor  constraintEqualToAnchor:_userAvatar.trailingAnchor constant:10],
        [_userNameLabel.trailingAnchor constraintEqualToAnchor:_userStrip.trailingAnchor constant:-8],
        [_userNameLabel.centerYAnchor  constraintEqualToAnchor:_userStrip.centerYAnchor],

        // Room header: full width of msg container, 60pt tall
        [_roomHeaderView.topAnchor     constraintEqualToAnchor:_msgContainer.topAnchor],
        [_roomHeaderView.leadingAnchor constraintEqualToAnchor:_msgContainer.leadingAnchor],
        [_roomHeaderView.trailingAnchor constraintEqualToAnchor:_msgContainer.trailingAnchor],
        [_roomHeaderView.heightAnchor  constraintEqualToConstant:60],

        // Header avatar: 40x40, centered vertically, 16px from left
        [_roomHeaderAvatar.leadingAnchor constraintEqualToAnchor:_roomHeaderView.leadingAnchor constant:16],
        [_roomHeaderAvatar.centerYAnchor constraintEqualToAnchor:_roomHeaderView.centerYAnchor],
        [_roomHeaderAvatar.widthAnchor   constraintEqualToConstant:40],
        [_roomHeaderAvatar.heightAnchor  constraintEqualToConstant:40],

        // Header name: right of avatar, top portion
        [_roomHeaderName.leadingAnchor  constraintEqualToAnchor:_roomHeaderAvatar.trailingAnchor constant:12],
        [_roomHeaderName.trailingAnchor constraintEqualToAnchor:_roomHeaderView.trailingAnchor constant:-16],
        [_roomHeaderName.topAnchor      constraintEqualToAnchor:_roomHeaderView.topAnchor constant:14],

        // Header topic: right of avatar, below name
        [_roomHeaderTopic.leadingAnchor  constraintEqualToAnchor:_roomHeaderAvatar.trailingAnchor constant:12],
        [_roomHeaderTopic.trailingAnchor constraintEqualToAnchor:_roomHeaderView.trailingAnchor constant:-16],
        [_roomHeaderTopic.topAnchor      constraintEqualToAnchor:_roomHeaderName.bottomAnchor constant:2],

        // Recovery banner sits between header and message list. Height is 0
        // when hidden; toggled to 36 by _maybeShowRecoveryBanner.
        [_recoveryBanner.topAnchor      constraintEqualToAnchor:_roomHeaderView.bottomAnchor],
        [_recoveryBanner.leadingAnchor  constraintEqualToAnchor:_msgContainer.leadingAnchor],
        [_recoveryBanner.trailingAnchor constraintEqualToAnchor:_msgContainer.trailingAnchor],

        // Message list fills remaining space below the banner.
        [_msgList.view.topAnchor      constraintEqualToAnchor:_recoveryBanner.bottomAnchor],
        [_msgList.view.leadingAnchor  constraintEqualToAnchor:_msgContainer.leadingAnchor],
        [_msgList.view.trailingAnchor constraintEqualToAnchor:_msgContainer.trailingAnchor],
        [_msgList.view.bottomAnchor   constraintEqualToAnchor:_msgContainer.bottomAnchor],

        // Compose bar
        [_compose.leadingAnchor  constraintEqualToAnchor:safe.leadingAnchor],
        [_compose.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_compose.bottomAnchor   constraintEqualToAnchor:statusSep.topAnchor],

        // Status separator + label
        [statusSep.leadingAnchor  constraintEqualToAnchor:safe.leadingAnchor],
        [statusSep.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [statusSep.bottomAnchor   constraintEqualToAnchor:_statusLabel.topAnchor],
        [statusSep.heightAnchor   constraintEqualToConstant:1],

        [_statusLabel.leadingAnchor  constraintEqualToAnchor:safe.leadingAnchor],
        [_statusLabel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [_statusLabel.bottomAnchor   constraintEqualToAnchor:safe.bottomAnchor],
        [_statusLabel.heightAnchor   constraintEqualToConstant:20],
    ]];

    // Recovery banner: collapsible height (0 when hidden, 36 when shown)
    // plus internal layout.
    _recoveryBannerHeight = [_recoveryBanner.heightAnchor constraintEqualToConstant:0];
    _recoveryBannerHeight.active = YES;

    [NSLayoutConstraint activateConstraints:@[
        [_recoveryLabel.leadingAnchor      constraintEqualToAnchor:_recoveryBanner.leadingAnchor constant:12],
        [_recoveryLabel.centerYAnchor      constraintEqualToAnchor:_recoveryBanner.centerYAnchor],

        [_recoveryKeyField.leadingAnchor   constraintEqualToAnchor:_recoveryLabel.trailingAnchor constant:8],
        [_recoveryKeyField.centerYAnchor   constraintEqualToAnchor:_recoveryBanner.centerYAnchor],
        [_recoveryKeyField.trailingAnchor  constraintEqualToAnchor:_recoveryVerifyBtn.leadingAnchor constant:-8],

        [_recoveryVerifyBtn.centerYAnchor  constraintEqualToAnchor:_recoveryBanner.centerYAnchor],
        [_recoveryVerifyBtn.trailingAnchor constraintEqualToAnchor:dismissBtn.leadingAnchor constant:-6],

        [dismissBtn.centerYAnchor          constraintEqualToAnchor:_recoveryBanner.centerYAnchor],
        [dismissBtn.trailingAnchor         constraintEqualToAnchor:_recoveryBanner.trailingAnchor constant:-6],
    ]];

    // ── Inline login view ─────────────────────────────────────────────────────
    // Sibling of sidebar/msg/compose/statusSep/statusLabel; toggled via -hidden.
    _mainContentViews = @[_sidebarContainer, _msgContainer, _compose, statusSep, _statusLabel];

    _loginView = [[LoginView alloc] initWithClient:&_impl->client];
    _loginView.delegate = self;
    _loginView.hidden   = YES;
    _loginView.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_loginView];

    [NSLayoutConstraint activateConstraints:@[
        [_loginView.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [_loginView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_loginView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_loginView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
    ]];
}

- (UIView*)_makeSeparator {
    UIView* sep = [[UIView alloc] init];
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    sep.backgroundColor = [UIColor separatorColor];
    return sep;
}

// ── Login flow ────────────────────────────────────────────────────────────────

- (void)doLogin {
    NSString* statusMsg = nil;
    if (auto saved = tesseract::SessionStore::load()) {
        auto res = _impl->client.restore_session(*saved);
        if (res) {
            _myUserId = @(_impl->client.get_user_id().c_str());
            _myDisplayName = _impl->client.get_display_name();
            _myAvatarUrl   = _impl->client.get_avatar_url();
            _msgList.myUserId = _myUserId;
            [self _populateUserStrip];
            [self _startSync];
            [self _setStatus:@"Connected"];
            [self _showMainContent];
            [self _maybeShowRecoveryBanner];
            return;
        }
        tesseract::SessionStore::clear();
        statusMsg = [@"Saved session expired: " stringByAppendingString:
                     @(res.message.c_str())];
    }

    [_loginView reset];
    [_loginView setStatusMessage:(statusMsg ?: @"")];
    [self _showLoginView];
    [self _setStatus:@"Not logged in"];
}

- (void)_showLoginView {
    for (UIView* v in _mainContentViews) v.hidden = YES;
    _loginView.hidden = NO;
}

- (void)_showMainContent {
    _loginView.hidden = YES;
    for (UIView* v in _mainContentViews) v.hidden = NO;
}

- (void)loginViewDidSucceed:(LoginView*)view {
    _myUserId = @(_impl->client.get_user_id().c_str());
    _myDisplayName = _impl->client.get_display_name();
    _myAvatarUrl   = _impl->client.get_avatar_url();
    _msgList.myUserId = _myUserId;
    [self _populateUserStrip];
    tesseract::SessionStore::save(_impl->client.export_session());
    [self _startSync];
    [self _setStatus:@"Connected"];
    [self _showMainContent];
    [self _maybeShowRecoveryBanner];
}

- (void)_startSync {
    _impl->bridge = std::make_unique<EventBridge>(self);
    _impl->client.start_sync(_impl->bridge.get());
}

- (void)stopSync {
    if (_impl && _impl->bridge) {
        _impl->client.stop_sync();
        _impl->bridge.reset();
    }
}

- (void)_setStatus:(NSString*)text {
    _statusLabel.text = text;
}

// ── RoomListDelegate ──────────────────────────────────────────────────────────

- (void)roomListDidSelectRoomId:(NSString*)roomId {
    if ([roomId isEqualToString:_currentRoomId]) return;
    if (_currentRoomId)
        _impl->client.unsubscribe_room(nsstr(_currentRoomId));

    _currentRoomId = roomId;
    [_msgList clearMessages];

    // Find room info from cached rooms for header
    std::string target = nsstr(roomId);
    for (const auto& r : _rooms) {
        if (r.id == target) {
            [self updateRoomHeader:r];
            break;
        }
    }

    auto res = _impl->client.subscribe_room(nsstr(roomId));
    if (!res.ok) {
        [self _setStatus:[@"Error: " stringByAppendingString:
                           @(res.message.c_str())]];
        return;
    }
    _impl->client.paginate_back(nsstr(roomId), 50);
    _impl->client.start_background_backfill();
}

- (void)roomListDidSelectSpaceId:(NSString*)spaceId {
    _spaceStack.push_back(nsstr(spaceId));
    [self _refreshRoomList];
}

- (void)_onBackClicked {
    if (!_spaceStack.empty()) _spaceStack.pop_back();
    [self _refreshRoomList];
}

- (void)_refreshRoomList {
    if (_spaceStack.empty()) {
        [_roomList updateRooms:_rooms];
        _navBar.hidden = YES;
    } else {
        const std::string& space_id = _spaceStack.back();
        auto child_ids = _impl->client.space_children(space_id);
        std::vector<tesseract::RoomInfo> filtered;
        for (const auto& r : _rooms)
            if (std::find(child_ids.begin(), child_ids.end(), r.id) != child_ids.end())
                filtered.push_back(r);
        [_roomList updateRooms:std::move(filtered)];
        for (const auto& r : _rooms)
            if (r.id == space_id) {
                _spaceNameLabel.text = @(r.name.c_str());
                break;
            }
        _navBar.hidden = NO;
    }
}

// ── MessageListDelegate ───────────────────────────────────────────────────────

- (void)messageListDidScrollToTop {
    if (!_currentRoomId) return;
    _impl->client.paginate_back(nsstr(_currentRoomId), 20);
}

// ── Send ──────────────────────────────────────────────────────────────────────

- (void)_sendMessage:(NSString*)body {
    if (!_currentRoomId || body.length == 0) return;
    auto res = _impl->client.send_message(nsstr(_currentRoomId), nsstr(body));
    if (!res.ok)
        [self _setStatus:[@"Send failed: " stringByAppendingString:
                           @(res.message.c_str())]];
}

// ── EventBridge callbacks (main thread) ───────────────────────────────────────

- (void)pushEvent:(std::unique_ptr<tesseract::Event>)ev {
    [_msgList pushEvent:std::move(ev)];
}

- (void)updateRooms:(std::vector<tesseract::RoomInfo>)rooms {
    _rooms = std::move(rooms);
    [self _refreshRoomList];
    if (_currentRoomId) {
        std::string target = nsstr(_currentRoomId);
        for (const auto& r : _rooms)
            if (r.id == target) { [self updateRoomHeader:r]; break; }
    }
}

- (void)handleSyncErrorContext:(NSString*)ctx
                   description:(NSString*)desc
                   softLogout:(BOOL)soft {
    if ([ctx isEqualToString:@"sync_auth_error"]) {
        if (soft) {
            // Soft logout: try to restore without clearing the store.
            [self _setStatus:@"Session expired, reconnecting…"];
            [self stopSync];
            [self doLogin];
        } else {
            tesseract::SessionStore::clear();
            [self _setStatus:@"Signed out. Please sign in again."];
            [self stopSync];
            [self doLogin];
        }
    } else {
        [self _setStatus:[@"Sync error: " stringByAppendingString:desc]];
    }
}

- (void)handleTimelineReset:(NSString*)roomId {
    if ([roomId isEqualToString:_currentRoomId])
        [_msgList clearMessages];
}

// ── Recovery banner — inline key entry, no modal dialog. ─────────────────────

- (void)_maybeShowRecoveryBanner {
    if (_recoveryBannerDismissed) return;
    if (!_impl->client.needs_recovery()) return;
    if (_recoveryBanner.hidden) {
        // Fresh prompt — restore the input row.
        _recoveryLabel.text = @"Verify this device:";
        _recoveryKeyField.text     = @"";
        _recoveryKeyField.hidden   = NO;
        _recoveryKeyField.enabled  = YES;
        _recoveryVerifyBtn.hidden  = NO;
        _recoveryVerifyBtn.enabled = YES;
        _recoveryBanner.hidden = NO;
        _recoveryBannerHeight.constant = 36;
        _recoveryInFlight = NO;
    }
}

- (void)_onRecoveryVerifyClicked {
    NSString* key = [_recoveryKeyField.text
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (key.length == 0) {
        _recoveryLabel.text = @"Please enter a recovery key or passphrase.";
        return;
    }
    _recoveryKeyField.enabled  = NO;
    _recoveryVerifyBtn.enabled = NO;
    _recoveryKeyField.hidden   = YES;
    _recoveryVerifyBtn.hidden  = YES;
    _recoveryLabel.text        = @"Verifying…";
    _recoveryInFlight = YES;

    std::string k = key.UTF8String ? key.UTF8String : "";
    std::thread([self, k]() {
        auto res = self->_impl->client.recover(k);
        bool        ok  = res.ok;
        std::string msg = res.message;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _onRecoverDone:ok message:@(msg.c_str())];
        });
    }).detach();
}

- (void)_onRecoverDone:(BOOL)ok message:(NSString*)msg {
    if (ok) {
        // Backup watcher will repaint into "Importing keys…" and hide the
        // banner once state reaches Enabled.
        _recoveryLabel.text = @"Downloading historical keys…";
        return;
    }
    _recoveryLabel.text =
        [NSString stringWithFormat:@"Recovery failed: %@", msg];
    _recoveryKeyField.hidden   = NO;
    _recoveryKeyField.enabled  = YES;
    _recoveryVerifyBtn.hidden  = NO;
    _recoveryVerifyBtn.enabled = YES;
    [_recoveryKeyField becomeFirstResponder];
    _recoveryInFlight = NO;
}

- (void)_onRecoveryDismissClicked {
    _recoveryBannerDismissed = YES;
    _recoveryBanner.hidden = YES;
    _recoveryBannerHeight.constant = 0;
}

- (void)handleBackupProgress:(const tesseract::BackupProgress&)progress {
    // Recovery state is populated asynchronously by the first sync cycle, so
    // re-evaluate the banner each time the SDK pings us.
    [self _maybeShowRecoveryBanner];

    // Live progress only when the input field is hidden, so we don't clobber
    // "Verify this device:" while the user is typing.
    if (!_recoveryBanner.hidden
        && _recoveryKeyField.hidden
        && progress.state == tesseract::BackupState::Downloading
        && progress.imported_keys > 0)
    {
        _recoveryLabel.text = [NSString
            stringWithFormat:@"Importing keys from backup… %llu imported.",
            (unsigned long long)progress.imported_keys];
    }
    if (progress.state == tesseract::BackupState::Enabled
        && !_impl->client.needs_recovery())
    {
        _recoveryBanner.hidden = YES;
        _recoveryBannerHeight.constant = 0;
    }
}

// ── Room header ──────────────────────────────────────────────────────────────

- (void)updateRoomHeader:(const tesseract::RoomInfo&)info {
    if (info.id.empty()) {
        _roomHeaderView.hidden = YES;
        return;
    }

    _roomHeaderName.text = @(info.name.c_str());

    if (!info.topic.empty()) {
        _roomHeaderTopic.text = @(info.topic.c_str());
        _roomHeaderTopic.hidden = NO;
    } else {
        _roomHeaderTopic.hidden = YES;
    }

    NSString* key = @(info.avatar_url.empty() ? info.id.c_str() : info.avatar_url.c_str());
    UIImage* cached = [[AvatarCache shared] cachedImageForKey:key];
    if (cached) {
        _roomHeaderAvatar.image = cached;
    } else {
        _roomHeaderAvatar.image = [AvatarCache initialsImageForName:@(info.name.c_str()) size:40];
        __weak typeof(self) weakSelf = self;
        std::string room_id = info.id;
        std::string avatar_url = info.avatar_url;
        tesseract::Client* client = &_impl->client;
        [[AvatarCache shared] avatarForKey:key
                                    fetch:[client, room_id] { return client->fetch_avatar_bytes(room_id); }
                               completion:^(UIImage* img) {
            __strong typeof(self) s = weakSelf;
            if (!s) return;
            NSString* expected = @(avatar_url.empty() ? room_id.c_str() : avatar_url.c_str());
            if ([key isEqualToString:expected])
                s->_roomHeaderAvatar.image = img;
        }];
    }

    _roomHeaderView.hidden = NO;
}

// ── User identity strip + logout ─────────────────────────────────────────────

- (void)_populateUserStrip {
    NSString* shown = _myDisplayName.empty()
        ? _myUserId
        : @(_myDisplayName.c_str());
    _userNameLabel.text = shown ?: @"";

    if (!_myAvatarUrl.empty()) {
        NSString* key = @(_myAvatarUrl.c_str());
        std::string url_copy = _myAvatarUrl;
        auto* impl_ptr = _impl.get();
        __weak typeof(self) weakSelf = self;
        _userAvatar.image = [[AvatarCache shared]
            avatarForKey:key
                   fetch:[impl_ptr, url_copy]() {
                       return impl_ptr->client.fetch_media_bytes(url_copy);
                   }
              completion:^(UIImage* img) {
                  __strong typeof(self) s = weakSelf;
                  s->_userAvatar.image = img;
              }];
    } else {
        _userAvatar.image = [AvatarCache initialsImageForName:shown size:32];
    }

    _userStrip.hidden = NO;
}

- (void)_doLogout {
    auto res = _impl->client.logout();
    tesseract::SessionStore::clear();
    [self stopSync];

    // Reset visible state.
    if (_currentRoomId)
        _impl->client.unsubscribe_room(nsstr(_currentRoomId));
    _currentRoomId  = nil;
    _myUserId       = nil;
    _myDisplayName.clear();
    _myAvatarUrl.clear();
    _rooms.clear();
    [self _refreshRoomList];
    [_msgList clearMessages];
    _roomHeaderView.hidden = YES;
    _userStrip.hidden = YES;
    _recoveryBanner.hidden = YES;
    _recoveryBannerHeight.constant = 0;
    _recoveryBannerDismissed = NO;

    [self _setStatus:(res.ok ? @"Signed out" : @"Sign out failed")];

    [_loginView reset];
    [_loginView setStatusMessage:@""];
    [self _showLoginView];
}

@end

// ── UserStripView impl ────────────────────────────────────────────────────────

@implementation UserStripView

- (instancetype)init {
    if (!(self = [super init])) return nil;
    UIContextMenuInteraction* interaction =
        [[UIContextMenuInteraction alloc] initWithDelegate:self];
    [self addInteraction:interaction];
    return self;
}

- (UIContextMenuConfiguration*)contextMenuInteraction:(UIContextMenuInteraction*)i
                       configurationForMenuAtLocation:(CGPoint)location {
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration
        configurationWithIdentifier:nil
                    previewProvider:nil
                     actionProvider:^UIMenu*(NSArray<UIMenuElement*>* suggested) {
        UIAction* logout = [UIAction
            actionWithTitle:@"Logout"
                      image:nil
                 identifier:nil
                    handler:^(__kindof UIAction* action) {
            __strong UserStripView* s = weakSelf;
            [s.owner _doLogout];
        }];
        return [UIMenu menuWithChildren:@[logout]];
    }];
}

@end
