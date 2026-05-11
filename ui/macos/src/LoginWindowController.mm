#import "LoginWindowController.h"
#include <tesseract/client.h>
#include <atomic>
#include <thread>
#include <string>

// ── Helper ────────────────────────────────────────────────────────────────────

static std::string nsstr(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string{};
}

// ── View tags ─────────────────────────────────────────────────────────────────

static const NSInteger kTagForm    = 1;
static const NSInteger kTagWaiting = 2;

// ── Controller ────────────────────────────────────────────────────────────────

@interface LoginWindowController ()
@end

@implementation LoginWindowController {
    tesseract::Client* _client;  // non-owning; AppDelegate owns the Client

    // Form view
    NSView*       _formView;
    NSTextField*  _hsField;
    NSTextField*  _errorLabel;
    NSButton*     _signInBtn;

    // Waiting view
    NSView*               _waitingView;
    NSProgressIndicator*  _spinner;
    NSTextField*          _waitingLabel;
    NSButton*             _cancelBtn;

    // Worker
    std::thread            _worker;
    std::atomic<bool>      _cancelled;
    BOOL                   _accepted;
}

- (instancetype)initWithClient:(tesseract::Client*)client {
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 400, 200)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = @"Sign in to Matrix";
    win.releasedWhenClosed = NO;

    if (!(self = [super initWithWindow:win])) return nil;
    _client    = client;
    _cancelled.store(false);
    _accepted  = NO;

    [self _buildFormView];
    [self _buildWaitingView];
    [self _showFormView];
    return self;
}

- (void)dealloc {
    if (_worker.joinable()) _worker.detach();
}

// ── View building ─────────────────────────────────────────────────────────────

- (void)_buildFormView {
    _formView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 200)];
    _formView.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* label = [NSTextField labelWithString:@"Homeserver URL:"];
    label.translatesAutoresizingMaskIntoConstraints = NO;

    _hsField = [NSTextField textFieldWithString:@"https://matrix.org"];
    _hsField.translatesAutoresizingMaskIntoConstraints = NO;
    _hsField.placeholderString = @"https://matrix.org";

    _errorLabel = [NSTextField labelWithString:@""];
    _errorLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _errorLabel.textColor = [NSColor systemRedColor];
    _errorLabel.font      = [NSFont systemFontOfSize:11];
    _errorLabel.hidden    = YES;

    _signInBtn = [NSButton buttonWithTitle:@"Sign In"
                                    target:self
                                    action:@selector(_signInClicked)];
    _signInBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _signInBtn.bezelStyle    = NSBezelStyleRounded;
    _signInBtn.keyEquivalent = @"\r";

    [_formView addSubview:label];
    [_formView addSubview:_hsField];
    [_formView addSubview:_errorLabel];
    [_formView addSubview:_signInBtn];

    [NSLayoutConstraint activateConstraints:@[
        [label.topAnchor    constraintEqualToAnchor:_formView.topAnchor constant:24],
        [label.leadingAnchor constraintEqualToAnchor:_formView.leadingAnchor constant:20],

        [_hsField.topAnchor      constraintEqualToAnchor:label.bottomAnchor constant:6],
        [_hsField.leadingAnchor  constraintEqualToAnchor:_formView.leadingAnchor constant:20],
        [_hsField.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor constant:-20],

        [_errorLabel.topAnchor    constraintEqualToAnchor:_hsField.bottomAnchor constant:6],
        [_errorLabel.leadingAnchor constraintEqualToAnchor:_hsField.leadingAnchor],
        [_errorLabel.trailingAnchor constraintEqualToAnchor:_hsField.trailingAnchor],

        [_signInBtn.topAnchor      constraintEqualToAnchor:_errorLabel.bottomAnchor constant:16],
        [_signInBtn.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor constant:-20],

        [_formView.bottomAnchor constraintEqualToAnchor:_signInBtn.bottomAnchor constant:20],
    ]];
}

- (void)_buildWaitingView {
    _waitingView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 160)];
    _waitingView.translatesAutoresizingMaskIntoConstraints = NO;

    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.translatesAutoresizingMaskIntoConstraints = NO;
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeRegular;

    _waitingLabel = [NSTextField
        labelWithString:@"Complete sign-in in your browser…"];
    _waitingLabel.translatesAutoresizingMaskIntoConstraints = NO;

    _cancelBtn = [NSButton buttonWithTitle:@"Cancel"
                                    target:self
                                    action:@selector(_cancelClicked)];
    _cancelBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _cancelBtn.bezelStyle = NSBezelStyleRounded;

    [_waitingView addSubview:_spinner];
    [_waitingView addSubview:_waitingLabel];
    [_waitingView addSubview:_cancelBtn];

    [NSLayoutConstraint activateConstraints:@[
        [_spinner.centerXAnchor constraintEqualToAnchor:_waitingView.centerXAnchor],
        [_spinner.topAnchor     constraintEqualToAnchor:_waitingView.topAnchor constant:30],

        [_waitingLabel.topAnchor    constraintEqualToAnchor:_spinner.bottomAnchor constant:16],
        [_waitingLabel.centerXAnchor constraintEqualToAnchor:_waitingView.centerXAnchor],

        [_cancelBtn.topAnchor      constraintEqualToAnchor:_waitingLabel.bottomAnchor constant:20],
        [_cancelBtn.centerXAnchor  constraintEqualToAnchor:_waitingView.centerXAnchor],

        [_waitingView.bottomAnchor constraintEqualToAnchor:_cancelBtn.bottomAnchor constant:20],
    ]];
}

// ── View switching ────────────────────────────────────────────────────────────

- (void)_showFormView {
    _waitingView.hidden = YES;
    [self.window.contentView addSubview:_formView];
    _formView.hidden = NO;

    [NSLayoutConstraint activateConstraints:@[
        [_formView.topAnchor    constraintEqualToAnchor:self.window.contentView.topAnchor],
        [_formView.leadingAnchor constraintEqualToAnchor:self.window.contentView.leadingAnchor],
        [_formView.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor],
        [_formView.bottomAnchor  constraintEqualToAnchor:self.window.contentView.bottomAnchor],
    ]];
    [self.window.contentView layoutSubtreeIfNeeded];
    [self.window setContentSize:_formView.fittingSize];
}

- (void)_showWaitingView {
    _formView.hidden = YES;
    [self.window.contentView addSubview:_waitingView];
    _waitingView.hidden = NO;

    [NSLayoutConstraint activateConstraints:@[
        [_waitingView.topAnchor    constraintEqualToAnchor:self.window.contentView.topAnchor],
        [_waitingView.leadingAnchor constraintEqualToAnchor:self.window.contentView.leadingAnchor],
        [_waitingView.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor],
        [_waitingView.bottomAnchor  constraintEqualToAnchor:self.window.contentView.bottomAnchor],
    ]];
    [self.window.contentView layoutSubtreeIfNeeded];
    [self.window setContentSize:_waitingView.fittingSize];
    [_spinner startAnimation:nil];
}

// ── Actions ───────────────────────────────────────────────────────────────────

- (void)_signInClicked {
    NSString* hs = [_hsField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (hs.length == 0) {
        [self _showError:@"Please enter a homeserver URL."];
        return;
    }
    _errorLabel.hidden = YES;
    _signInBtn.enabled = NO;
    _hsField.enabled   = NO;
    [self _showWaitingView];
    [self _startPhase1:nsstr(hs)];
}

- (void)_cancelClicked {
    _cancelled.store(true);
    _client->cancel_oauth();
    if (_worker.joinable()) _worker.detach();
    [NSApp endSheet:self.window returnCode:NSModalResponseCancel];
}

// ── OAuth two-phase flow ──────────────────────────────────────────────────────

- (void)_startPhase1:(std::string)hs {
    _worker = std::thread([self, hs]() {
        auto flow = _client->begin_oauth(hs);
        if (_cancelled.load()) return;
        bool ok          = flow.ok;
        std::string data = ok ? flow.auth_url : flow.message;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _onPhase1Done:ok data:@(data.c_str())];
        });
    });
}

- (void)_onPhase1Done:(BOOL)ok data:(NSString*)data {
    if (!ok) {
        [self _showError:data];
        [self _showFormView];
        _signInBtn.enabled = YES;
        _hsField.enabled   = YES;
        return;
    }
    if (_worker.joinable()) _worker.join();

    tesseract::Client::open_in_browser(nsstr(data));
    [self _startPhase2];
}

- (void)_startPhase2 {
    _worker = std::thread([self]() {
        auto res = _client->await_oauth();
        if (_cancelled.load()) return;
        bool ok       = res.ok;
        std::string msg = res.message;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _onPhase2Done:ok message:@(msg.c_str())];
        });
    });
}

- (void)_onPhase2Done:(BOOL)ok message:(NSString*)msg {
    if (_worker.joinable()) _worker.join();
    if (ok) {
        _accepted = YES;
        [NSApp endSheet:self.window returnCode:NSModalResponseOK];
        return;
    }
    [self _showError:msg];
    [self _showFormView];
    _signInBtn.enabled = YES;
    _hsField.enabled   = YES;
}

- (void)_showError:(NSString*)msg {
    _errorLabel.stringValue = msg;
    _errorLabel.hidden      = NO;
}

@end
