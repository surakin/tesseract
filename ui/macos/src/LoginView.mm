#import "LoginView.h"

#include <atomic>
#include <string>
#include <thread>

namespace {

std::string nsstr(NSString* s) {
    return s ? std::string(s.UTF8String) : std::string{};
}

} // namespace

@implementation LoginView {
    tesseract::Client* _client;  // non-owning

    NSView*       _card;
    NSTextField*  _titleLabel;
    NSTextField*  _statusLabel;

    // Form subviews
    NSView*       _formView;
    NSTextField*  _hsField;
    NSTextField*  _errorLabel;
    NSButton*     _signInBtn;

    // Waiting subviews
    NSView*               _waitingView;
    NSProgressIndicator*  _spinner;
    NSTextField*          _waitingLabel;
    NSButton*             _cancelBtn;

    std::thread       _worker;
    std::atomic<bool> _cancelled;
}

- (instancetype)initWithClient:(tesseract::Client*)client {
    if (!(self = [super initWithFrame:NSZeroRect])) return nil;
    _client = client;
    _cancelled.store(false);

    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.wantsLayer = YES;
    self.layer.backgroundColor =
        [NSColor colorWithCalibratedWhite:0.94 alpha:1.0].CGColor;

    [self _buildCard];
    [self _buildFormView];
    [self _buildWaitingView];
    [self _showFormView];
    return self;
}

- (void)dealloc {
    _cancelled.store(true);
    if (_client) _client->cancel_oauth();
    if (_worker.joinable()) _worker.join();
}

// ── Card / title / status ─────────────────────────────────────────────────────

- (void)_buildCard {
    _card = [[NSView alloc] init];
    _card.translatesAutoresizingMaskIntoConstraints = NO;
    _card.wantsLayer = YES;
    _card.layer.backgroundColor = [NSColor whiteColor].CGColor;
    _card.layer.cornerRadius    = 8;
    _card.layer.borderWidth     = 1;
    _card.layer.borderColor     =
        [NSColor colorWithCalibratedWhite:0.82 alpha:1.0].CGColor;
    [self addSubview:_card];

    _titleLabel = [NSTextField labelWithString:@"Sign in to Tesseract"];
    _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _titleLabel.font = [NSFont boldSystemFontOfSize:18];
    [_card addSubview:_titleLabel];

    _statusLabel = [NSTextField labelWithString:@""];
    _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _statusLabel.textColor = [NSColor secondaryLabelColor];
    _statusLabel.font      = [NSFont systemFontOfSize:11];
    _statusLabel.hidden    = YES;
    [_card addSubview:_statusLabel];

    [NSLayoutConstraint activateConstraints:@[
        // Center the card in self.
        [_card.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [_card.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_card.widthAnchor   constraintEqualToConstant:420],

        [_titleLabel.topAnchor      constraintEqualToAnchor:_card.topAnchor constant:24],
        [_titleLabel.leadingAnchor  constraintEqualToAnchor:_card.leadingAnchor constant:24],
        [_titleLabel.trailingAnchor constraintEqualToAnchor:_card.trailingAnchor constant:-24],

        [_statusLabel.topAnchor      constraintEqualToAnchor:_titleLabel.bottomAnchor constant:8],
        [_statusLabel.leadingAnchor  constraintEqualToAnchor:_card.leadingAnchor constant:24],
        [_statusLabel.trailingAnchor constraintEqualToAnchor:_card.trailingAnchor constant:-24],
    ]];
}

// ── View building ─────────────────────────────────────────────────────────────

- (void)_buildFormView {
    _formView = [[NSView alloc] init];
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
        [label.topAnchor      constraintEqualToAnchor:_formView.topAnchor],
        [label.leadingAnchor  constraintEqualToAnchor:_formView.leadingAnchor],

        [_hsField.topAnchor      constraintEqualToAnchor:label.bottomAnchor constant:6],
        [_hsField.leadingAnchor  constraintEqualToAnchor:_formView.leadingAnchor],
        [_hsField.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor],

        [_errorLabel.topAnchor      constraintEqualToAnchor:_hsField.bottomAnchor constant:6],
        [_errorLabel.leadingAnchor  constraintEqualToAnchor:_formView.leadingAnchor],
        [_errorLabel.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor],

        [_signInBtn.topAnchor      constraintEqualToAnchor:_errorLabel.bottomAnchor constant:16],
        [_signInBtn.trailingAnchor constraintEqualToAnchor:_formView.trailingAnchor],

        [_formView.bottomAnchor constraintEqualToAnchor:_signInBtn.bottomAnchor],
    ]];
}

- (void)_buildWaitingView {
    _waitingView = [[NSView alloc] init];
    _waitingView.translatesAutoresizingMaskIntoConstraints = NO;

    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.translatesAutoresizingMaskIntoConstraints = NO;
    _spinner.style       = NSProgressIndicatorStyleSpinning;
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
        [_spinner.topAnchor     constraintEqualToAnchor:_waitingView.topAnchor],

        [_waitingLabel.topAnchor     constraintEqualToAnchor:_spinner.bottomAnchor constant:16],
        [_waitingLabel.centerXAnchor constraintEqualToAnchor:_waitingView.centerXAnchor],

        [_cancelBtn.topAnchor     constraintEqualToAnchor:_waitingLabel.bottomAnchor constant:20],
        [_cancelBtn.centerXAnchor constraintEqualToAnchor:_waitingView.centerXAnchor],

        [_waitingView.bottomAnchor constraintEqualToAnchor:_cancelBtn.bottomAnchor],
    ]];
}

// ── View switching ────────────────────────────────────────────────────────────

- (void)_showFormView {
    [_waitingView removeFromSuperview];
    [_spinner stopAnimation:nil];

    if (_formView.superview != _card) {
        [_card addSubview:_formView];
        [NSLayoutConstraint activateConstraints:@[
            [_formView.topAnchor      constraintEqualToAnchor:_statusLabel.bottomAnchor constant:12],
            [_formView.leadingAnchor  constraintEqualToAnchor:_card.leadingAnchor constant:24],
            [_formView.trailingAnchor constraintEqualToAnchor:_card.trailingAnchor constant:-24],
            [_formView.bottomAnchor   constraintEqualToAnchor:_card.bottomAnchor constant:-24],
        ]];
    }
    _signInBtn.enabled = YES;
    _hsField.enabled   = YES;
    [self.window makeFirstResponder:_hsField];
}

- (void)_showWaitingView {
    [_formView removeFromSuperview];

    [_card addSubview:_waitingView];
    [NSLayoutConstraint activateConstraints:@[
        [_waitingView.topAnchor      constraintEqualToAnchor:_statusLabel.bottomAnchor constant:12],
        [_waitingView.leadingAnchor  constraintEqualToAnchor:_card.leadingAnchor constant:24],
        [_waitingView.trailingAnchor constraintEqualToAnchor:_card.trailingAnchor constant:-24],
        [_waitingView.bottomAnchor   constraintEqualToAnchor:_card.bottomAnchor constant:-24],
    ]];
    [_spinner startAnimation:nil];
    _cancelBtn.enabled = YES;
    _waitingLabel.stringValue = @"Complete sign-in in your browser…";
}

// ── Public API ────────────────────────────────────────────────────────────────

- (void)reset {
    _cancelled.store(true);
    if (_client) _client->cancel_oauth();
    if (_worker.joinable()) _worker.join();
    _cancelled.store(false);
    _errorLabel.hidden = YES;
    [self _showFormView];
}

- (void)setStatusMessage:(NSString*)message {
    if (message.length == 0) {
        _statusLabel.hidden = YES;
        _statusLabel.stringValue = @"";
    } else {
        _statusLabel.stringValue = message;
        _statusLabel.hidden = NO;
    }
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
    if (_client) _client->cancel_oauth();
    _waitingLabel.stringValue = @"Cancelling…";
    _cancelBtn.enabled = NO;
}

// ── OAuth two-phase flow ──────────────────────────────────────────────────────

- (void)_startPhase1:(std::string)hs {
    if (_worker.joinable()) _worker.join();
    _cancelled.store(false);

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
    if (_worker.joinable()) _worker.join();

    if (!ok) {
        [self _showError:[@"Sign-in failed: " stringByAppendingString:data]];
        [self _showFormView];
        return;
    }

    tesseract::Client::open_in_browser(nsstr(data));
    [self _startPhase2];
}

- (void)_startPhase2 {
    if (_worker.joinable()) _worker.join();
    _cancelled.store(false);

    _worker = std::thread([self]() {
        auto res = _client->await_oauth();
        if (_cancelled.load()) return;
        bool ok         = res.ok;
        std::string msg = res.message;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _onPhase2Done:ok message:@(msg.c_str())];
        });
    });
}

- (void)_onPhase2Done:(BOOL)ok message:(NSString*)msg {
    if (_worker.joinable()) _worker.join();
    if (ok) {
        [self.delegate loginViewDidSucceed:self];
        return;
    }
    [self _showError:[@"Sign-in failed: " stringByAppendingString:msg]];
    [self _showFormView];
}

- (void)_showError:(NSString*)msg {
    _errorLabel.stringValue = msg;
    _errorLabel.hidden      = NO;
}

@end
