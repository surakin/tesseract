#import "ComposeBar.h"
#import "EmojiPicker.h"

#include <tesseract/client.h>
#include <tesseract/visual.h>

static const CGFloat kMinHeight = tesseract::visual::kComposeMinHeight;
static const CGFloat kMaxHeight = tesseract::visual::kComposeMaxHeight;
static const CGFloat kPad       = 8;
static const CGFloat kBtnW      = 64;
static const CGFloat kEmojiBtnW = 36;

// Custom UITextView subclass that maps a hardware-keyboard Return (no
// modifiers) to the compose bar's send action. Shift+Return falls through
// to the default newline insertion. Implemented with a UIKeyCommand so
// system input methods still work.
@interface ComposeTextView : UITextView
@property (nonatomic, copy) void (^onSubmit)(void);
@end

@implementation ComposeTextView
- (NSArray<UIKeyCommand*>*)keyCommands {
    UIKeyCommand* send = [UIKeyCommand keyCommandWithInput:@"\r"
                                              modifierFlags:0
                                                     action:@selector(_keySend:)];
    return @[ send ];
}
- (void)_keySend:(UIKeyCommand*)cmd {
    if (_onSubmit) _onSubmit();
}
@end

@interface ComposeBar () <UITextViewDelegate, UIPopoverPresentationControllerDelegate>
@end

@implementation ComposeBar {
    ComposeTextView*        _tv;
    UIButton*               _sendBtn;
    UIButton*               _emojiBtn;
    EmojiPickerController*  _emojiController;
    NSLayoutConstraint*     _heightConstraint;
}

- (instancetype)initWithFrame:(CGRect)frame {
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
    self.backgroundColor = [UIColor systemBackgroundColor];

    // Top separator line
    UIView* sep = [[UIView alloc] init];
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    sep.backgroundColor = [UIColor separatorColor];
    [self addSubview:sep];

    // Text view (UITextView has its own scrolling)
    _tv = [[ComposeTextView alloc] init];
    _tv.translatesAutoresizingMaskIntoConstraints = NO;
    _tv.delegate                    = self;
    _tv.font                        = [UIFont systemFontOfSize:13];
    _tv.textContainerInset          = UIEdgeInsetsMake(6, 4, 6, 4);
    _tv.backgroundColor             = [UIColor secondarySystemBackgroundColor];
    _tv.layer.cornerRadius          = 6;
    _tv.scrollEnabled               = YES;
    _tv.autocorrectionType          = UITextAutocorrectionTypeYes;
    __weak typeof(self) weakSelf = self;
    _tv.onSubmit = ^{ [weakSelf _sendClicked]; };
    [self addSubview:_tv];

    // Emoji button (opens UIPopover anchored to itself).
    _emojiBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    _emojiBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [_emojiBtn setTitle:@"\U0001F600"  // 😀
               forState:UIControlStateNormal];
    [_emojiBtn addTarget:self
                  action:@selector(_emojiClicked)
        forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:_emojiBtn];

    // Send button
    _sendBtn = [UIButton buttonWithType:UIButtonTypeSystem];
    _sendBtn.translatesAutoresizingMaskIntoConstraints = NO;
    [_sendBtn setTitle:@"Send" forState:UIControlStateNormal];
    [_sendBtn addTarget:self
                 action:@selector(_sendClicked)
       forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:_sendBtn];

    _heightConstraint = [self.heightAnchor constraintEqualToConstant:kMinHeight];
    _heightConstraint.active = YES;

    [NSLayoutConstraint activateConstraints:@[
        // Separator at top
        [sep.topAnchor      constraintEqualToAnchor:self.topAnchor],
        [sep.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [sep.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [sep.heightAnchor   constraintEqualToConstant:1],

        // Text view
        [_tv.topAnchor      constraintEqualToAnchor:sep.bottomAnchor constant:kPad],
        [_tv.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor constant:kPad],
        [_tv.trailingAnchor constraintEqualToAnchor:_emojiBtn.leadingAnchor constant:-kPad],
        [_tv.bottomAnchor   constraintEqualToAnchor:self.bottomAnchor constant:-kPad],

        // Emoji button
        [_emojiBtn.trailingAnchor constraintEqualToAnchor:_sendBtn.leadingAnchor constant:-kPad],
        [_emojiBtn.centerYAnchor  constraintEqualToAnchor:self.centerYAnchor],
        [_emojiBtn.widthAnchor    constraintEqualToConstant:kEmojiBtnW],

        // Send button
        [_sendBtn.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-kPad],
        [_sendBtn.centerYAnchor  constraintEqualToAnchor:self.centerYAnchor],
        [_sendBtn.widthAnchor    constraintEqualToConstant:kBtnW],
    ]];
}

- (void)setClient:(tesseract::Client*)client {
    _client = client;
    if (_emojiController) _emojiController.client = client;
}

- (void)_emojiClicked {
    if (_emojiController.presentingViewController) {
        [_emojiController dismissViewControllerAnimated:YES completion:nil];
        return;
    }
    if (!_emojiController) {
        _emojiController = [[EmojiPickerController alloc] init];
        _emojiController.client = _client;
        __weak typeof(self) weakSelf = self;
        _emojiController.onSelect = ^(NSString* glyph) {
            [weakSelf insertEmoji:glyph];
        };
    }
    [_emojiController refreshFrequents];

    _emojiController.modalPresentationStyle = UIModalPresentationPopover;
    UIPopoverPresentationController* pop = _emojiController.popoverPresentationController;
    pop.delegate                  = self;
    pop.sourceView                = _emojiBtn;
    pop.sourceRect                = _emojiBtn.bounds;
    pop.permittedArrowDirections  = UIPopoverArrowDirectionDown;

    UIViewController* host = [self _hostingViewController];
    [host presentViewController:_emojiController animated:YES completion:nil];
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)c {
    // Keep the popover compact on Catalyst Mac — don't expand to full screen.
    return UIModalPresentationNone;
}

- (UIViewController*)_hostingViewController {
    UIResponder* r = self;
    while (r) {
        if ([r isKindOfClass:[UIViewController class]]) return (UIViewController*)r;
        r = r.nextResponder;
    }
    return nil;
}

- (void)insertEmoji:(NSString*)glyph {
    if (glyph.length == 0) return;
    NSRange range = _tv.selectedRange;
    NSMutableString* s = [_tv.text mutableCopy] ?: [NSMutableString string];
    [s replaceCharactersInRange:range withString:glyph];
    _tv.text = s;
    _tv.selectedRange = NSMakeRange(range.location + glyph.length, 0);
    [self _updateHeight];
    if (_client) _client->recent_emoji_bump(std::string(glyph.UTF8String));
}

- (void)textViewDidChange:(UITextView*)tv {
    [self _updateHeight];
}

- (void)_updateHeight {
    CGSize size = [_tv sizeThatFits:CGSizeMake(_tv.bounds.size.width, CGFLOAT_MAX)];
    CGFloat desired = size.height + 2 * kPad + 1 /* sep */;
    desired = MAX(kMinHeight, MIN(kMaxHeight, desired));

    if (fabs(desired - _heightConstraint.constant) > 0.5) {
        _heightConstraint.constant = desired;
        [self.superview layoutIfNeeded];
    }
}

- (void)_sendClicked {
    NSString* body = [_tv.text
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (body.length == 0) return;
    if (_onSend) _onSend(body);
    [self clear];
}

- (void)clear {
    _tv.text = @"";
    [self _updateHeight];
}

- (void)focusInput {
    [_tv becomeFirstResponder];
}

@end
