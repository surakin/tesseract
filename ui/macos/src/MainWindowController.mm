#import "MainWindowController.h"
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

// User-strip view that surfaces an NSMenu on right-click / Ctrl-click.
@interface UserStripView : NSView
@property (nonatomic, strong) NSMenu* contextMenu;
@end

@implementation UserStripView
- (NSMenu*)menuForEvent:(NSEvent*)__unused event { return _contextMenu; }
@end

// ── Controller ────────────────────────────────────────────────────────────────

@interface MainWindowController ()
    <RoomListDelegate, MessageListDelegate, LoginViewDelegate>
@end

@implementation MainWindowController {
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
    NSArray<NSView*>*      _mainContentViews;  // toggled vs _loginView visibility

    // User identity strip (sidebar footer)
    UserStripView*       _userStrip;
    NSImageView*         _userAvatar;
    NSTextField*         _userNameLabel;
    std::string          _myDisplayName;
    std::string          _myAvatarUrl;

    // Recovery banner widgets (Step 6) — inline key entry, no modal dialog.
    NSView*              _recoveryBanner;
    NSTextField*         _recoveryLabel;
    NSSecureTextField*   _recoveryKeyField;
    NSButton*            _recoveryVerifyBtn;
    NSLayoutConstraint*  _recoveryBannerHeight;
    BOOL                 _recoveryBannerDismissed;
    BOOL                 _recoveryInFlight;

    // State
    NSString* _currentRoomId;
    NSString* _myUserId;
    NSTextField* _statusLabel;
    std::vector<tesseract::RoomInfo> _rooms;
    std::vector<std::string>         _spaceStack;

    // Sidebar nav bar (visible when inside a space)
    NSView*      _sidebarContainer;
    NSView*      _navBar;
    NSButton*    _backButton;
    NSTextField* _spaceNameLabel;

    // Room header
    NSView* _msgContainer;
    NSView* _roomHeaderView;
    NSImageView* _roomHeaderAvatar;
    NSTextField* _roomHeaderName;
    NSTextField* _roomHeaderTopic;
}

- (instancetype)init {
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 900, 620)
                  styleMask:(NSWindowStyleMaskTitled          |
                             NSWindowStyleMaskClosable        |
                             NSWindowStyleMaskMiniaturizable  |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title              = @"Tesseract";
    win.minSize            = NSMakeSize(600, 400);
    win.releasedWhenClosed = NO;
    [win center];

    if (!(self = [super initWithWindow:win])) return nil;
    _impl = std::make_unique<Impl>();
    [self _buildUI];
    return self;
}

- (void)dealloc {
    // _loginView holds a non-owning pointer to _impl->client and calls
    // cancel_oauth() + joins its worker on dealloc. Release it now so the
    // client is still alive — _impl destructs later when ARC tears down
    // C++ ivars after this method returns.
    [_loginView removeFromSuperview];
    _loginView = nil;
}

// ── UI construction ───────────────────────────────────────────────────────────

- (void)_buildUI {
    NSView* content = self.window.contentView;

    // ── Status bar at the bottom ──────────────────────────────────────────────
    _statusLabel = [NSTextField labelWithString:@"Not connected"];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.textColor  = [NSColor secondaryLabelColor];
    _statusLabel.font       = [NSFont systemFontOfSize:11];
    _statusLabel.alignment  = NSTextAlignmentCenter;
    [content addSubview:_statusLabel];

    NSBox* statusSep = [[NSBox alloc] init];
    statusSep.boxType = NSBoxSeparator;
    statusSep.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:statusSep];

    // ── Compose bar ───────────────────────────────────────────────────────────
    _compose = [[ComposeBar alloc] initWithFrame:NSZeroRect];
    _compose.translatesAutoresizingMaskIntoConstraints = NO;
    _compose.client = &_impl->client;
    __weak typeof(self) weakSelf = self;
    _compose.onSend = ^(NSString* body) {
        [weakSelf _sendMessage:body];
    };
    [content addSubview:_compose];

    // ── Sidebar (nav bar + room list) ─────────────────────────────────────────
    _sidebarContainer = [[NSView alloc] init];
    _sidebarContainer.translatesAutoresizingMaskIntoConstraints = NO;

    _navBar = [[NSView alloc] init];
    _navBar.translatesAutoresizingMaskIntoConstraints = NO;
    _navBar.hidden = YES;

    _backButton = [NSButton buttonWithTitle:@"←"
                                     target:self
                                     action:@selector(_onBackClicked)];
    _backButton.translatesAutoresizingMaskIntoConstraints = NO;
    _backButton.bezelStyle = NSBezelStyleRounded;
    [_navBar addSubview:_backButton];

    _spaceNameLabel = [NSTextField labelWithString:@""];
    _spaceNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _spaceNameLabel.font = [NSFont boldSystemFontOfSize:12];
    _spaceNameLabel.textColor = [NSColor labelColor];
    _spaceNameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_navBar addSubview:_spaceNameLabel];

    NSBox* navSep = [[NSBox alloc] init];
    navSep.boxType = NSBoxSeparator;
    navSep.translatesAutoresizingMaskIntoConstraints = NO;
    [_navBar addSubview:navSep];

    [_sidebarContainer addSubview:_navBar];

    _roomList = [[RoomListController alloc] init];
    _roomList.delegate = self;
    _roomList.client   = &_impl->client;
    _roomList.view.translatesAutoresizingMaskIntoConstraints = NO;
    [_sidebarContainer addSubview:_roomList.view];

    // ── User identity strip (sidebar footer) ──────────────────────────────────
    _userStrip = [[UserStripView alloc] init];
    _userStrip.translatesAutoresizingMaskIntoConstraints = NO;
    _userStrip.wantsLayer = YES;
    _userStrip.layer.backgroundColor =
        [NSColor colorWithCalibratedWhite:0.91 alpha:1.0].CGColor;
    _userStrip.hidden = YES;
    [_sidebarContainer addSubview:_userStrip];

    _userAvatar = [[NSImageView alloc] init];
    _userAvatar.translatesAutoresizingMaskIntoConstraints = NO;
    [_userStrip addSubview:_userAvatar];

    _userNameLabel = [NSTextField labelWithString:@""];
    _userNameLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _userNameLabel.font = [NSFont boldSystemFontOfSize:13];
    _userNameLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_userStrip addSubview:_userNameLabel];

    NSMenu* userMenu = [[NSMenu alloc] init];
    [userMenu addItemWithTitle:@"Logout"
                        action:@selector(_doLogout)
                 keyEquivalent:@""];
    for (NSMenuItem* item in userMenu.itemArray) item.target = self;
    _userStrip.contextMenu = userMenu;

    _msgList = [[MessageListController alloc] init];
    _msgList.delegate = self;
    _msgList.client   = &_impl->client;

    // Message container: header + message list stacked vertically
    _msgContainer = [[NSView alloc] init];
    _msgContainer.translatesAutoresizingMaskIntoConstraints = NO;

    // Room header bar
    _roomHeaderView = [[NSView alloc] init];
    _roomHeaderView.translatesAutoresizingMaskIntoConstraints = NO;
    [_msgContainer addSubview:_roomHeaderView];

    // Avatar
    _roomHeaderAvatar = [[NSImageView alloc] init];
    _roomHeaderAvatar.translatesAutoresizingMaskIntoConstraints = NO;
    [_roomHeaderView addSubview:_roomHeaderAvatar];

    // Name label
    _roomHeaderName = [NSTextField labelWithString:@""];
    _roomHeaderName.translatesAutoresizingMaskIntoConstraints = NO;
    _roomHeaderName.font = [NSFont boldSystemFontOfSize:15];
    _roomHeaderName.textColor = [NSColor labelColor];
    _roomHeaderName.lineBreakMode = NSLineBreakByTruncatingTail;
    [_roomHeaderView addSubview:_roomHeaderName];

    // Topic label
    _roomHeaderTopic = [NSTextField labelWithString:@""];
    _roomHeaderTopic.translatesAutoresizingMaskIntoConstraints = NO;
    _roomHeaderTopic.font = [NSFont systemFontOfSize:12];
    _roomHeaderTopic.textColor = [NSColor secondaryLabelColor];
    _roomHeaderTopic.lineBreakMode = NSLineBreakByTruncatingTail;
    _roomHeaderTopic.hidden = YES;
    [_roomHeaderView addSubview:_roomHeaderTopic];

    // Recovery banner (Step 6) — hidden until needs_recovery() is true.
    _recoveryBanner = [[NSView alloc] init];
    _recoveryBanner.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryBanner.wantsLayer = YES;
    _recoveryBanner.layer.backgroundColor =
        [NSColor colorWithCalibratedRed:1.0 green:0.96 blue:0.84 alpha:1.0].CGColor;
    _recoveryBanner.hidden = YES;
    [_msgContainer addSubview:_recoveryBanner];

    _recoveryLabel = [NSTextField labelWithString:@"Verify this device:"];
    _recoveryLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryLabel.textColor = [NSColor colorWithCalibratedRed:0.36 green:0.27 blue:0.0 alpha:1.0];
    _recoveryLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    [_recoveryBanner addSubview:_recoveryLabel];

    _recoveryKeyField = [[NSSecureTextField alloc] init];
    _recoveryKeyField.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryKeyField.placeholderString = @"Recovery key or passphrase";
    _recoveryKeyField.target = self;
    _recoveryKeyField.action = @selector(_onRecoveryVerifyClicked);  // Enter triggers verify
    [_recoveryBanner addSubview:_recoveryKeyField];

    _recoveryVerifyBtn = [NSButton buttonWithTitle:@"Verify"
                                            target:self
                                            action:@selector(_onRecoveryVerifyClicked)];
    _recoveryVerifyBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _recoveryVerifyBtn.bezelStyle = NSBezelStyleRounded;
    _recoveryVerifyBtn.keyEquivalent = @"\r";
    [_recoveryBanner addSubview:_recoveryVerifyBtn];

    NSButton* dismissBtn = [NSButton buttonWithTitle:@"✕"
                                              target:self
                                              action:@selector(_onRecoveryDismissClicked)];
    dismissBtn.translatesAutoresizingMaskIntoConstraints = NO;
    dismissBtn.bezelStyle = NSBezelStyleInline;
    [_recoveryBanner addSubview:dismissBtn];

    // Message list fills remaining space
    _msgList.view.translatesAutoresizingMaskIntoConstraints = NO;
    [_msgContainer addSubview:_msgList.view];

    _roomHeaderView.hidden = YES;

    NSSplitView* split = [[NSSplitView alloc] init];
    split.translatesAutoresizingMaskIntoConstraints = NO;
    split.vertical        = YES;
    split.dividerStyle    = NSSplitViewDividerStyleThin;
    split.autosaveName    = @"TesseractMainSplit";

    [split addArrangedSubview:_sidebarContainer];
    [split addArrangedSubview:_msgContainer];
    [split setHoldingPriority:NSLayoutPriorityDefaultLow + 1
              forSubviewAtIndex:0];
    [content addSubview:split];

    // ── Auto Layout ───────────────────────────────────────────────────────────
    [NSLayoutConstraint activateConstraints:@[
        // Split view fills top portion
        [split.topAnchor     constraintEqualToAnchor:content.topAnchor],
        [split.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [split.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [split.bottomAnchor  constraintEqualToAnchor:_compose.topAnchor],

        // Left pane width
        [_sidebarContainer.widthAnchor constraintEqualToConstant:220],

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
        [_roomHeaderAvatar.widthAnchor  constraintEqualToConstant:40],
        [_roomHeaderAvatar.heightAnchor constraintEqualToConstant:40],

        // Header name: right of avatar, top portion
        [_roomHeaderName.leadingAnchor constraintEqualToAnchor:_roomHeaderAvatar.trailingAnchor constant:12],
        [_roomHeaderName.trailingAnchor constraintEqualToAnchor:_roomHeaderView.trailingAnchor constant:-16],
        [_roomHeaderName.topAnchor constraintEqualToAnchor:_roomHeaderView.topAnchor constant:14],

        // Header topic: right of avatar, below name
        [_roomHeaderTopic.leadingAnchor constraintEqualToAnchor:_roomHeaderAvatar.trailingAnchor constant:12],
        [_roomHeaderTopic.trailingAnchor constraintEqualToAnchor:_roomHeaderView.trailingAnchor constant:-16],
        [_roomHeaderTopic.topAnchor constraintEqualToAnchor:_roomHeaderName.bottomAnchor constant:2],

        // Recovery banner sits between header and message list. Height is 0
        // when hidden; toggled to 30 by _maybeShowRecoveryBanner.
        [_recoveryBanner.topAnchor      constraintEqualToAnchor:_roomHeaderView.bottomAnchor],
        [_recoveryBanner.leadingAnchor  constraintEqualToAnchor:_msgContainer.leadingAnchor],
        [_recoveryBanner.trailingAnchor constraintEqualToAnchor:_msgContainer.trailingAnchor],

        // Message list fills remaining space below the banner.
        [_msgList.view.topAnchor     constraintEqualToAnchor:_recoveryBanner.bottomAnchor],
        [_msgList.view.leadingAnchor constraintEqualToAnchor:_msgContainer.leadingAnchor],
        [_msgList.view.trailingAnchor constraintEqualToAnchor:_msgContainer.trailingAnchor],
        [_msgList.view.bottomAnchor  constraintEqualToAnchor:_msgContainer.bottomAnchor],

        // Compose bar
        [_compose.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_compose.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_compose.bottomAnchor   constraintEqualToAnchor:statusSep.topAnchor],

        // Status separator + label
        [statusSep.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [statusSep.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [statusSep.bottomAnchor   constraintEqualToAnchor:_statusLabel.topAnchor],

        [_statusLabel.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_statusLabel.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_statusLabel.bottomAnchor  constraintEqualToAnchor:content.bottomAnchor],
        [_statusLabel.heightAnchor  constraintEqualToConstant:20],
    ]];

    // Recovery banner: collapsible height (0 when hidden, 36 when shown) plus
    // internal layout. Subview order: 0=label, 1=key field, 2=verify, 3=dismiss.
    _recoveryBannerHeight = [_recoveryBanner.heightAnchor constraintEqualToConstant:0];
    _recoveryBannerHeight.active = YES;

    NSButton* dismissBtnLayout = _recoveryBanner.subviews[3];
    [NSLayoutConstraint activateConstraints:@[
        [_recoveryLabel.leadingAnchor      constraintEqualToAnchor:_recoveryBanner.leadingAnchor constant:12],
        [_recoveryLabel.centerYAnchor      constraintEqualToAnchor:_recoveryBanner.centerYAnchor],

        [_recoveryKeyField.leadingAnchor   constraintEqualToAnchor:_recoveryLabel.trailingAnchor constant:8],
        [_recoveryKeyField.centerYAnchor   constraintEqualToAnchor:_recoveryBanner.centerYAnchor],
        [_recoveryKeyField.trailingAnchor  constraintEqualToAnchor:_recoveryVerifyBtn.leadingAnchor constant:-8],

        [_recoveryVerifyBtn.centerYAnchor  constraintEqualToAnchor:_recoveryBanner.centerYAnchor],
        [_recoveryVerifyBtn.trailingAnchor constraintEqualToAnchor:dismissBtnLayout.leadingAnchor constant:-6],

        [dismissBtnLayout.centerYAnchor    constraintEqualToAnchor:_recoveryBanner.centerYAnchor],
        [dismissBtnLayout.trailingAnchor   constraintEqualToAnchor:_recoveryBanner.trailingAnchor constant:-6],
    ]];

    // ── Inline login view ─────────────────────────────────────────────────────
    // Sibling of split/compose/statusSep/_statusLabel; toggled via -hidden.
    _mainContentViews = @[split, _compose, statusSep, _statusLabel];

    _loginView = [[LoginView alloc] initWithClient:&_impl->client];
    _loginView.delegate = self;
    _loginView.hidden   = YES;
    [content addSubview:_loginView];

    [NSLayoutConstraint activateConstraints:@[
        [_loginView.topAnchor      constraintEqualToAnchor:content.topAnchor],
        [_loginView.leadingAnchor  constraintEqualToAnchor:content.leadingAnchor],
        [_loginView.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_loginView.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
    ]];
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
    for (NSView* v in _mainContentViews) v.hidden = YES;
    _loginView.hidden = NO;
}

- (void)_showMainContent {
    _loginView.hidden = YES;
    for (NSView* v in _mainContentViews) v.hidden = NO;
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
    _statusLabel.stringValue = text;
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
                _spaceNameLabel.stringValue = @(r.name.c_str());
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

// ── Recovery banner (Step 6) — inline key entry, no modal dialog. ────────────

- (void)_maybeShowRecoveryBanner {
    if (_recoveryBannerDismissed) return;
    if (!_impl->client.needs_recovery()) return;
    if (_recoveryBanner.hidden) {
        // Fresh prompt — restore the input row.
        _recoveryLabel.stringValue = @"Verify this device:";
        _recoveryKeyField.stringValue = @"";
        _recoveryKeyField.hidden  = NO;
        _recoveryKeyField.enabled = YES;
        _recoveryVerifyBtn.hidden  = NO;
        _recoveryVerifyBtn.enabled = YES;
        _recoveryBanner.hidden = NO;
        _recoveryBannerHeight.constant = 36;
        _recoveryInFlight = NO;
    }
}

- (void)_onRecoveryVerifyClicked {
    NSString* key = [_recoveryKeyField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (key.length == 0) {
        _recoveryLabel.stringValue = @"Please enter a recovery key or passphrase.";
        return;
    }
    _recoveryKeyField.enabled  = NO;
    _recoveryVerifyBtn.enabled = NO;
    _recoveryKeyField.hidden   = YES;
    _recoveryVerifyBtn.hidden  = YES;
    _recoveryLabel.stringValue = @"Verifying…";
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
        _recoveryLabel.stringValue = @"Downloading historical keys…";
        return;
    }
    _recoveryLabel.stringValue =
        [NSString stringWithFormat:@"Recovery failed: %@", msg];
    _recoveryKeyField.hidden   = NO;
    _recoveryKeyField.enabled  = YES;
    _recoveryVerifyBtn.hidden  = NO;
    _recoveryVerifyBtn.enabled = YES;
    [_recoveryKeyField selectText:nil];
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
        _recoveryLabel.stringValue = [NSString
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

    _roomHeaderName.stringValue = @(info.name.c_str());

    if (!info.topic.empty()) {
        _roomHeaderTopic.stringValue = @(info.topic.c_str());
        _roomHeaderTopic.toolTip     = @(info.topic.c_str());
        _roomHeaderTopic.hidden = NO;
    } else {
        _roomHeaderTopic.hidden = YES;
        _roomHeaderTopic.toolTip = nil;
    }

    NSString* key = @(info.avatar_url.empty() ? info.id.c_str() : info.avatar_url.c_str());
    NSImage* cached = [[AvatarCache shared] cachedImageForKey:key];
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
                               completion:^(NSImage* img) {
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
    _userNameLabel.stringValue = shown ?: @"";

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
              completion:^(NSImage* img) {
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
