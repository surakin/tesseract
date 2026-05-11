#import "RecoveryWindowController.h"
#include <tesseract/client.h>
#include <atomic>
#include <thread>
#include <string>

static std::string nsstr(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string{};
}

@interface RecoveryWindowController ()
@end

@implementation RecoveryWindowController {
    tesseract::Client* _client;  // non-owning

    NSView*       _formView;
    NSSecureTextField* _keyField;
    NSTextField*  _errorLabel;
    NSButton*     _verifyBtn;
    NSButton*     _skipBtn;

    NSView*               _waitingView;
    NSProgressIndicator*  _spinner;
    NSTextField*          _progressLabel;
    NSButton*             _closeBtn;

    std::thread       _worker;
    std::atomic<bool> _cancelled;
    BOOL              _recoverDone;
}

- (instancetype)initWithClient:(tesseract::Client*)client {
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 420, 220)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = @"Verify this device";
    win.releasedWhenClosed = NO;

    if (!(self = [super initWithWindow:win])) return nil;
    _client      = client;
    _cancelled.store(false);
    _recoverDone = NO;

    [self _buildFormView];
    [self _buildWaitingView];
    [self _showFormView];
    return self;
}

- (void)dealloc {
    if (_worker.joinable()) _worker.detach();
}

- (void)_buildFormView {
    _formView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 420, 220)];
    _formView.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* intro = [NSTextField wrappingLabelWithString:
        @"Enter your recovery key or passphrase to verify this device and "
        @"decrypt historical messages."];
    intro.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* label = [NSTextField labelWithString:@"Recovery key:"];
    label.translatesAutoresizingMaskIntoConstraints = NO;

    _keyField = [[NSSecureTextField alloc] init];
    _keyField.translatesAutoresizingMaskIntoConstraints = NO;
    _keyField.placeholderString = @"Recovery key or passphrase";

    _errorLabel = [NSTextField labelWithString:@""];
    _errorLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _errorLabel.textColor = [NSColor systemRedColor];
    _errorLabel.font      = [NSFont systemFontOfSize:11];
    _errorLabel.hidden    = YES;

    _skipBtn = [NSButton buttonWithTitle:@"Skip"
                                  target:self
                                  action:@selector(_skipClicked)];
    _skipBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _skipBtn.bezelStyle = NSBezelStyleRounded;

    _verifyBtn = [NSButton buttonWithTitle:@"Verify"
                                    target:self
                                    action:@selector(_verifyClicked)];
    _verifyBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _verifyBtn.bezelStyle    = NSBezelStyleRounded;
    _verifyBtn.keyEquivalent = @"\r";

    [_formView addSubview:intro];
    [_formView addSubview:label];
    [_formView addSubview:_keyField];
    [_formView addSubview:_errorLabel];
    [_formView addSubview:_skipBtn];
    [_formView addSubview:_verifyBtn];

    [NSLayoutConstraint activateConstraints:@[
        [intro.topAnchor      constraintEqualToAnchor:_formView.topAnchor constant:20],
        [intro.leadingAnchor  constraintEqualToAnchor:_formView.leadingAnchor constant:20],
        [intro.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor constant:-20],

        [label.topAnchor     constraintEqualToAnchor:intro.bottomAnchor constant:14],
        [label.leadingAnchor constraintEqualToAnchor:_formView.leadingAnchor constant:20],

        [_keyField.topAnchor       constraintEqualToAnchor:label.bottomAnchor constant:6],
        [_keyField.leadingAnchor   constraintEqualToAnchor:_formView.leadingAnchor constant:20],
        [_keyField.trailingAnchor  constraintEqualToAnchor:_formView.trailingAnchor constant:-20],

        [_errorLabel.topAnchor       constraintEqualToAnchor:_keyField.bottomAnchor constant:6],
        [_errorLabel.leadingAnchor   constraintEqualToAnchor:_keyField.leadingAnchor],
        [_errorLabel.trailingAnchor  constraintEqualToAnchor:_keyField.trailingAnchor],

        [_verifyBtn.topAnchor      constraintEqualToAnchor:_errorLabel.bottomAnchor constant:16],
        [_verifyBtn.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor constant:-20],
        [_skipBtn.topAnchor        constraintEqualToAnchor:_verifyBtn.topAnchor],
        [_skipBtn.trailingAnchor   constraintEqualToAnchor:_verifyBtn.leadingAnchor constant:-8],

        [_formView.bottomAnchor constraintEqualToAnchor:_verifyBtn.bottomAnchor constant:20],
    ]];
}

- (void)_buildWaitingView {
    _waitingView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 420, 180)];
    _waitingView.translatesAutoresizingMaskIntoConstraints = NO;

    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.translatesAutoresizingMaskIntoConstraints = NO;
    _spinner.style = NSProgressIndicatorStyleSpinning;

    _progressLabel = [NSTextField labelWithString:@"Unlocking secret storage…"];
    _progressLabel.translatesAutoresizingMaskIntoConstraints = NO;

    _closeBtn = [NSButton buttonWithTitle:@"Close"
                                   target:self
                                   action:@selector(_closeClicked)];
    _closeBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _closeBtn.bezelStyle = NSBezelStyleRounded;
    _closeBtn.enabled    = NO;

    [_waitingView addSubview:_spinner];
    [_waitingView addSubview:_progressLabel];
    [_waitingView addSubview:_closeBtn];

    [NSLayoutConstraint activateConstraints:@[
        [_spinner.centerXAnchor constraintEqualToAnchor:_waitingView.centerXAnchor],
        [_spinner.topAnchor     constraintEqualToAnchor:_waitingView.topAnchor constant:30],

        [_progressLabel.topAnchor     constraintEqualToAnchor:_spinner.bottomAnchor constant:16],
        [_progressLabel.centerXAnchor constraintEqualToAnchor:_waitingView.centerXAnchor],

        [_closeBtn.topAnchor      constraintEqualToAnchor:_progressLabel.bottomAnchor constant:20],
        [_closeBtn.centerXAnchor  constraintEqualToAnchor:_waitingView.centerXAnchor],

        [_waitingView.bottomAnchor constraintEqualToAnchor:_closeBtn.bottomAnchor constant:20],
    ]];
}

- (void)_showFormView {
    _waitingView.hidden = YES;
    [self.window.contentView addSubview:_formView];
    _formView.hidden = NO;

    [NSLayoutConstraint activateConstraints:@[
        [_formView.topAnchor      constraintEqualToAnchor:self.window.contentView.topAnchor],
        [_formView.leadingAnchor  constraintEqualToAnchor:self.window.contentView.leadingAnchor],
        [_formView.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor],
        [_formView.bottomAnchor   constraintEqualToAnchor:self.window.contentView.bottomAnchor],
    ]];
    [self.window.contentView layoutSubtreeIfNeeded];
    [self.window setContentSize:_formView.fittingSize];
}

- (void)_showWaitingView {
    _formView.hidden = YES;
    [self.window.contentView addSubview:_waitingView];
    _waitingView.hidden = NO;

    [NSLayoutConstraint activateConstraints:@[
        [_waitingView.topAnchor      constraintEqualToAnchor:self.window.contentView.topAnchor],
        [_waitingView.leadingAnchor  constraintEqualToAnchor:self.window.contentView.leadingAnchor],
        [_waitingView.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor],
        [_waitingView.bottomAnchor   constraintEqualToAnchor:self.window.contentView.bottomAnchor],
    ]];
    [self.window.contentView layoutSubtreeIfNeeded];
    [self.window setContentSize:_waitingView.fittingSize];
    [_spinner startAnimation:nil];
}

- (void)_verifyClicked {
    NSString* key = [_keyField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (key.length == 0) {
        [self _showError:@"Please enter a recovery key or passphrase."];
        return;
    }
    _errorLabel.hidden = YES;
    _verifyBtn.enabled = NO;
    _skipBtn.enabled   = NO;
    _keyField.enabled  = NO;
    [self _showWaitingView];
    [self _startRecover:nsstr(key)];
}

- (void)_skipClicked {
    _cancelled.store(true);
    if (_worker.joinable()) _worker.detach();
    [NSApp endSheet:self.window returnCode:NSModalResponseCancel];
}

- (void)_closeClicked {
    [NSApp endSheet:self.window returnCode:NSModalResponseOK];
}

- (void)_startRecover:(std::string)key {
    _worker = std::thread([self, key]() {
        auto res = _client->recover(key);
        if (_cancelled.load()) return;
        bool ok         = res.ok;
        std::string msg = res.message;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _onRecoverDone:ok message:@(msg.c_str())];
        });
    });
}

- (void)_onRecoverDone:(BOOL)ok message:(NSString*)msg {
    if (_worker.joinable()) _worker.join();
    if (!ok) {
        [self _showError:[NSString stringWithFormat:@"Recovery failed: %@", msg]];
        [self _showFormView];
        _verifyBtn.enabled = YES;
        _skipBtn.enabled   = YES;
        _keyField.enabled  = YES;
        return;
    }
    _recoverDone = YES;
    _progressLabel.stringValue = @"Downloading historical keys…";
    // _closeBtn stays disabled until backup state reaches Enabled.
}

- (void)updateProgress:(const tesseract::BackupProgress&)progress {
    if (!_recoverDone) return;

    switch (progress.state) {
        case tesseract::BackupState::Enabled:
            _progressLabel.stringValue = [NSString
                stringWithFormat:@"Done. Imported %llu keys.",
                (unsigned long long)progress.imported_keys];
            _closeBtn.enabled = YES;
            break;
        case tesseract::BackupState::Downloading:
            _progressLabel.stringValue = [NSString
                stringWithFormat:@"Importing keys… %llu imported.",
                (unsigned long long)progress.imported_keys];
            break;
        case tesseract::BackupState::Disabled:
            _progressLabel.stringValue = @"Backup is not enabled on the server.";
            _closeBtn.enabled = YES;
            break;
        default:
            break;
    }
}

- (void)_showError:(NSString*)msg {
    _errorLabel.stringValue = msg;
    _errorLabel.hidden      = NO;
}

@end
