#import "MainWindowController.h"
#import "LoginWindowController.h"
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

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string nsstr(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string{};
}

// ── Controller ────────────────────────────────────────────────────────────────

@interface MainWindowController ()
    <RoomListDelegate, MessageListDelegate>
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
    LoginWindowController* _loginWC;

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
    [self addChildViewController:_roomList];
    _roomList.view.translatesAutoresizingMaskIntoConstraints = NO;
    [_sidebarContainer addSubview:_roomList.view];

    _msgList = [[MessageListController alloc] init];
    _msgList.delegate = self;
    _msgList.client   = &_impl->client;
    [self addChildViewController:_msgList];

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

        // Room list fills sidebar below the (possibly-hidden) nav bar
        [_roomList.view.topAnchor      constraintEqualToAnchor:_navBar.bottomAnchor],
        [_roomList.view.leadingAnchor  constraintEqualToAnchor:_sidebarContainer.leadingAnchor],
        [_roomList.view.trailingAnchor constraintEqualToAnchor:_sidebarContainer.trailingAnchor],
        [_roomList.view.bottomAnchor   constraintEqualToAnchor:_sidebarContainer.bottomAnchor],

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
        [_roomHeaderName leadingAnchor constraintEqualToAnchor:_roomHeaderAvatar.trailingAnchor constant:12],
        [_roomHeaderName.trailingAnchor constraintEqualToAnchor:_roomHeaderView.trailingAnchor constant:-16],
        [_roomHeaderName.topAnchor constraintEqualToAnchor:_roomHeaderView.topAnchor constant:14],

        // Header topic: right of avatar, below name
        [_roomHeaderTopic leadingAnchor constraintEqualToAnchor:_roomHeaderAvatar.trailingAnchor constant:12],
        [_roomHeaderTopic.trailingAnchor constraintEqualToAnchor:_roomHeaderView.trailingAnchor constant:-16],
        [_roomHeaderTopic.topAnchor constraintEqualToAnchor:_roomHeaderName.bottomAnchor constant:2],

        // Message list fills remaining space below header
        [_msgList.view.topAnchor     constraintEqualToAnchor:_roomHeaderView.bottomAnchor],
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
}

// ── Login flow ────────────────────────────────────────────────────────────────

- (void)doLogin {
    if (auto saved = tesseract::SessionStore::load()) {
        auto res = _impl->client.restore_session(*saved);
        if (res) {
            _myUserId = @(_impl->client.get_user_id().c_str());
            _msgList.myUserId = _myUserId;
            [self _startSync];
            [self _setStatus:@"Connected"];
            return;
        }
        tesseract::SessionStore::clear();
    }

    _loginWC = [[LoginWindowController alloc]
        initWithClient:&_impl->client];
    [self.window beginSheet:_loginWC.window
          completionHandler:^(NSModalResponse resp) {
        if (resp == NSModalResponseOK) {
            _myUserId = @(_impl->client.get_user_id().c_str());
            _msgList.myUserId = _myUserId;
            tesseract::SessionStore::save(_impl->client.export_session());
            [self _startSync];
            [self _setStatus:@"Connected"];
        } else {
            [self _setStatus:@"Login cancelled"];
        }
        _loginWC = nil;
    }];
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
        [[AvatarCache shared] avatarForKey:key
                                    fetch:[&, info] { return _impl->client.fetch_avatar_bytes(info.id); }
                               completion:^(NSImage* img) {
            if ([key isEqualToString:@(info.avatar_url.empty() ? info.id.c_str() : info.avatar_url.c_str())])
                weakSelf.roomHeaderAvatar.image = img;
        }];
    }

    _roomHeaderView.hidden = NO;
}

@end
