#import "ComposeBar.h"

static const CGFloat kMinHeight = 38;
static const CGFloat kMaxHeight = 120;
static const CGFloat kPad       = 8;
static const CGFloat kBtnW      = 64;

@interface ComposeBar () <NSTextViewDelegate>
@end

@implementation ComposeBar {
    NSScrollView* _scroll;
    NSTextView*   _tv;
    NSButton*     _sendBtn;
    NSLayoutConstraint* _heightConstraint;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    [self _setup];
    return self;
}

- (instancetype)initWithCoder:(NSCoder*)coder {
    if (!(self = [super initWithCoder:coder])) return nil;
    [self _setup];
    return self;
}

- (void)_setup {
    self.wantsLayer = YES;
    self.layer.backgroundColor =
        [NSColor windowBackgroundColor].CGColor;

    // Top separator line
    NSBox* sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:sep];

    // Text view inside a scroll view
    _scroll = [[NSScrollView alloc] init];
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
    _scroll.hasVerticalScroller   = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers    = YES;
    _scroll.borderType            = NSNoBorder;

    _tv = [[NSTextView alloc] init];
    _tv.delegate                    = self;
    _tv.font                        = [NSFont systemFontOfSize:13];
    _tv.textContainerInset          = NSMakeSize(4, 6);
    _tv.richText                    = NO;
    _tv.allowsUndo                  = YES;
    _tv.drawsBackground             = YES;
    _tv.backgroundColor             = [NSColor controlBackgroundColor];
    _tv.textContainer.widthTracksTextView = YES;
    _tv.minSize                     = NSMakeSize(0, kMinHeight - 2 * kPad);
    _tv.maxSize                     = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _tv.autoresizingMask            = NSViewWidthSizable;

    _scroll.documentView = _tv;
    [self addSubview:_scroll];

    // Send button
    _sendBtn = [NSButton buttonWithTitle:@"Send"
                                  target:self
                                  action:@selector(_sendClicked)];
    _sendBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _sendBtn.bezelStyle = NSBezelStyleRounded;
    _sendBtn.keyEquivalent = @"";
    [self addSubview:_sendBtn];

    // Self has an intrinsic height constraint that updates as text grows
    _heightConstraint = [self.heightAnchor constraintEqualToConstant:kMinHeight];
    _heightConstraint.active = YES;

    [NSLayoutConstraint activateConstraints:@[
        // Separator at top
        [sep.topAnchor    constraintEqualToAnchor:self.topAnchor],
        [sep.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [sep.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [sep.heightAnchor   constraintEqualToConstant:1],

        // Scroll view
        [_scroll.topAnchor     constraintEqualToAnchor:sep.bottomAnchor
                                              constant:kPad],
        [_scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor
                                              constant:kPad],
        [_scroll.trailingAnchor constraintEqualToAnchor:_sendBtn.leadingAnchor
                                               constant:-kPad],
        [_scroll.bottomAnchor  constraintEqualToAnchor:self.bottomAnchor
                                              constant:-kPad],

        // Send button
        [_sendBtn.trailingAnchor constraintEqualToAnchor:self.trailingAnchor
                                                constant:-kPad],
        [_sendBtn.centerYAnchor  constraintEqualToAnchor:self.centerYAnchor],
        [_sendBtn.widthAnchor    constraintEqualToConstant:kBtnW],
    ]];
}

- (void)textDidChange:(NSNotification*)n {
    [self _updateHeight];
}

- (void)_updateHeight {
    NSLayoutManager* lm = _tv.layoutManager;
    NSTextContainer* tc = _tv.textContainer;
    [lm ensureLayoutForTextContainer:tc];
    CGFloat usedH = [lm usedRectForTextContainer:tc].size.height;
    CGFloat desired = usedH + 2 * kPad + 1 /* sep */ + 2 * _tv.textContainerInset.height;
    desired = MAX(kMinHeight, MIN(kMaxHeight, desired));

    if (fabs(desired - _heightConstraint.constant) > 0.5) {
        _heightConstraint.constant = desired;
        [self.superview layoutSubtreeIfNeeded];
    }
}

// NSTextViewDelegate: intercept Return key.
- (BOOL)textView:(NSTextView*)tv doCommandBySelector:(SEL)cmd {
    if (cmd == @selector(insertNewline:)) {
        BOOL shift = ([NSApp currentEvent].modifierFlags
                      & NSEventModifierFlagShift) != 0;
        if (!shift) {
            [self _sendClicked];
            return YES;
        }
    }
    return NO;
}

- (void)_sendClicked {
    NSString* body = [_tv.string
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (body.length == 0) return;
    if (_onSend) _onSend(body);
    [self clear];
}

- (void)clear {
    [_tv setString:@""];
    [self _updateHeight];
}

- (void)focusInput {
    [_tv.window makeFirstResponder:_tv];
}

@end
