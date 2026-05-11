#import "MainWindowController.h"
#import "LoginWindowController.h"
#import "RoomListController.h"
#import "MessageListController.h"
#import "ComposeBar.h"
#import "EventBridge.h"

#include <tesseract/client.h>
#include <tesseract/session_store.h>
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

    // ── Split view ────────────────────────────────────────────────────────────
    _roomList = [[RoomListController alloc] init];
    _roomList.delegate = self;
    [self addChildViewController:_roomList];

    _msgList = [[MessageListController alloc] init];
    _msgList.delegate = self;
    [self addChildViewController:_msgList];

    NSSplitView* split = [[NSSplitView alloc] init];
    split.translatesAutoresizingMaskIntoConstraints = NO;
    split.vertical        = YES;
    split.dividerStyle    = NSSplitViewDividerStyleThin;
    split.autosaveName    = @"TesseractMainSplit";

    [split addArrangedSubview:_roomList.view];
    [split addArrangedSubview:_msgList.view];
    [split setHoldingPriority:NSLayoutPriorityDefaultLow + 1
              forSubviewAtIndex:0];
    [content addSubview:split];

    // ── Auto Layout ───────────────────────────────────────────────────────────
    [NSLayoutConstraint activateConstraints:@[
        // Split view fills top portion
        [split.topAnchor    constraintEqualToAnchor:content.topAnchor],
        [split.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [split.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [split.bottomAnchor  constraintEqualToAnchor:_compose.topAnchor],

        // Left pane width
        [_roomList.view.widthAnchor constraintEqualToConstant:220],

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
        [_statusLabel.bottomAnchor   constraintEqualToAnchor:content.bottomAnchor],
        [_statusLabel.heightAnchor   constraintEqualToConstant:20],
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

    auto res = _impl->client.subscribe_room(nsstr(roomId));
    if (!res.ok) {
        [self _setStatus:[@"Error: " stringByAppendingString:
                           @(res.message.c_str())]];
        return;
    }
    _impl->client.paginate_back(nsstr(roomId), 50);
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
    [_roomList updateRooms:std::move(rooms)];
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

@end
