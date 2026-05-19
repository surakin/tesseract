#include "host_macos.h"
#include "canvas_cg.h"
#include "controls.h"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include <algorithm>
#include <cmath>
#include <utility>

namespace tk::macos
{
class NSTextFieldNative;
class NSTextViewNative;
} // namespace tk::macos

@class TKSurfaceView;
@class TKTextFieldBridge;
@class TKTextViewBridge;

namespace tk::macos
{

// ─────────────────────────────────────────────────────────────────────────
//  Host — owns the tree, paints via CoreGraphics + CoreText
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host
{
public:
    Host(TKSurfaceView* view, const Theme& theme, bool transparent = false)
        : view_(view), theme_(&theme), factory_(cg::make_factory()),
          transparent_(transparent)
    {
    }

    void request_repaint() override;
    void post_to_ui(std::function<void()> task) override;
    void post_delayed(int ms, std::function<void()> fn) override;
    std::unique_ptr<NativeTextField> make_text_field() override;
    std::unique_ptr<NativeTextArea> make_text_area() override;
    std::unique_ptr<AudioPlayer> make_audio_player() override;
    std::unique_ptr<AudioCapture> make_audio_capture() override;
    std::unique_ptr<VideoPlayer> make_video_player() override;
    EncodedImage encode_for_send(const std::uint8_t* data, std::size_t len,
                                 bool compress) override;

    void set_root(std::unique_ptr<Widget> root)
    {
        root_ = std::move(root);
        relayout();
    }
    Widget* root() const
    {
        return root_.get();
    }
    const Theme& theme() const
    {
        return *theme_;
    }
    void set_theme(const Theme& t)
    {
        theme_ = &t;
    }
    CanvasFactory& factory()
    {
        return *factory_;
    }
    TKSurfaceView* view() const
    {
        return view_;
    }
    void detach()
    {
        view_ = nil;
    }

    void relayout();
    void set_on_layout(std::function<void()> cb)
    {
        on_layout_ = std::move(cb);
    }

    void on_draw(CGContextRef ctx);
    void on_layout_changed();

    void on_pointer_down(NSPoint p);
    void on_pointer_up(NSPoint p);
    void on_pointer_move(NSPoint p);
    void on_pointer_leave();
    void on_wheel(NSPoint p, CGFloat dx, CGFloat dy);
    void on_right_click(NSPoint p);

    // Drag-and-drop. `pasteboard_has_dropable` is consulted from
    // -draggingEntered: to gate the cursor; `dispatch_file_drop` runs
    // the actual decode + handler invocation from -performDragOperation:.
    void set_on_file_drop(FileDropHandler cb)
    {
        on_file_drop_ = std::move(cb);
    }
    bool has_file_drop_handler() const
    {
        return static_cast<bool>(on_file_drop_);
    }
    void set_on_right_click(std::function<void(tk::Point)> cb)
    {
        on_right_click_ = std::move(cb);
    }
    bool pasteboard_has_dropable(NSPasteboard* pb) const;
    bool dispatch_file_drop(NSPasteboard* pb);
    void set_drag_active(bool active);
    bool drag_active() const
    {
        return drag_active_;
    }

private:
    TKSurfaceView* view_;
    const Theme* theme_;
    std::unique_ptr<CanvasFactory> factory_;
    bool transparent_ = false;
    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    Widget* pressed_widget_ = nullptr;
    Button* hovered_btn_ = nullptr;
    Widget* hovered_widget_ = nullptr;
    FileDropHandler on_file_drop_;
    std::function<void(tk::Point)> on_right_click_;
    bool drag_active_ = false;
};

} // namespace tk::macos

// ─────────────────────────────────────────────────────────────────────────
//  TKSurfaceView — the NSView subclass
// ─────────────────────────────────────────────────────────────────────────
//
// `isFlipped` returns YES so the CG context handed to `drawRect:` has its
// origin at the top-left of the view — matching the convention the shared
// widget tree (and `canvas_cg.cpp`) assumes.

@interface TKSurfaceView : NSView
@property(nonatomic, assign) tk::macos::Host* hostPtr;
@property(nonatomic, strong) NSTrackingArea* trackingArea;
@property(nonatomic, assign) BOOL transparent;
@end

@implementation TKSurfaceView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
    {
        self.wantsLayer = NO; // we paint via drawRect: into a fresh CG ctx

        // Drag-and-drop image-file ingest. Accept file URLs (Finder /
        // Safari / most apps), direct PNG / JPEG / TIFF payloads (in-app
        // image drags), and the generic public.image UTI (browser image
        // drags that promise a typed payload). The drop is gated by the
        // host having a handler installed; -draggingEntered: returns
        // NSDragOperationNone when no handler is wired.
        [self registerForDraggedTypes:@[
            NSPasteboardTypeFileURL,
            NSPasteboardTypePNG,
            NSPasteboardTypeTIFF,
            @"public.jpeg",
            @"public.image",
        ]];
    }
    return self;
}

- (BOOL)isFlipped
{
    return YES;
}
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (BOOL)isOpaque
{
    return !self.transparent;
}

- (void)drawRect:(NSRect)dirtyRect
{
    if (!self.hostPtr)
    {
        return;
    }
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextClipToRect(ctx, NSRectToCGRect(dirtyRect));
    self.hostPtr->on_draw(ctx);
}

// NSView calls -layout once per re-layout pass (after frame changes,
// constraint updates, etc.). It's the AppKit equivalent of UIKit's
// `layoutSubviews`.
- (void)layout
{
    [super layout];
    if (self.hostPtr)
    {
        self.hostPtr->on_layout_changed();
    }
}

// Tracking area covers the whole view bounds; hover requires it.
- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (self.trackingArea)
    {
        [self removeTrackingArea:self.trackingArea];
    }
    NSTrackingAreaOptions opts =
        NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
        NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
    self.trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                     options:opts
                                                       owner:self
                                                    userInfo:nil];
    [self addTrackingArea:self.trackingArea];
}

// ── Mouse events ────────────────────────────────────────────────────────

- (NSPoint)tkLocationFromEvent:(NSEvent*)e
{
    return [self convertPoint:e.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent*)e
{
    if (self.hostPtr)
    {
        self.hostPtr->on_pointer_down([self tkLocationFromEvent:e]);
    }
}

- (void)mouseUp:(NSEvent*)e
{
    if (self.hostPtr)
    {
        self.hostPtr->on_pointer_up([self tkLocationFromEvent:e]);
    }
}

- (void)rightMouseDown:(NSEvent*)e
{
    if (self.hostPtr)
    {
        self.hostPtr->on_right_click([self tkLocationFromEvent:e]);
    }
}

- (void)mouseMoved:(NSEvent*)e
{
    if (self.hostPtr)
    {
        self.hostPtr->on_pointer_move([self tkLocationFromEvent:e]);
    }
}

- (void)mouseDragged:(NSEvent*)e
{
    if (self.hostPtr)
    {
        self.hostPtr->on_pointer_move([self tkLocationFromEvent:e]);
    }
}

- (void)mouseExited:(NSEvent*)e
{
    if (self.hostPtr)
    {
        self.hostPtr->on_pointer_leave();
    }
}

// Scroll wheel + trackpad scroll. AppKit reports scroll deltas as pixels
// (with scrollingDeltaX/Y when hasPreciseScrollingDeltas == YES, or
// device units otherwise). Positive deltaY scrolls content downward in
// AppKit's flipped view space, matching the toolkit convention.
- (void)scrollWheel:(NSEvent*)e
{
    if (!self.hostPtr)
    {
        return;
    }
    NSPoint loc = [self tkLocationFromEvent:e];
    CGFloat dx, dy;
    if (e.hasPreciseScrollingDeltas)
    {
        dx = -e.scrollingDeltaX;
        dy = -e.scrollingDeltaY;
    }
    else
    {
        // 1 detent ≈ 30 pixels — matches the Win32 default.
        dx = -e.scrollingDeltaX * 10.0;
        dy = -e.scrollingDeltaY * 10.0;
    }
    self.hostPtr->on_wheel(loc, dx, dy);
}

// ── Drag-and-drop destination ───────────────────────────────────────────

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    if (!self.hostPtr || !self.hostPtr->has_file_drop_handler())
    {
        return NSDragOperationNone;
    }
    if (self.hostPtr->pasteboard_has_dropable(sender.draggingPasteboard))
    {
        self.hostPtr->set_drag_active(true);
        return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    return [self draggingEntered:sender];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    (void)sender;
    if (self.hostPtr)
    {
        self.hostPtr->set_drag_active(false);
    }
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
    return self.hostPtr &&
           self.hostPtr->pasteboard_has_dropable(sender.draggingPasteboard);
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    if (!self.hostPtr)
    {
        return NO;
    }
    BOOL ok =
        self.hostPtr->dispatch_file_drop(sender.draggingPasteboard) ? YES : NO;
    self.hostPtr->set_drag_active(false);
    return ok;
}

@end

// ─────────────────────────────────────────────────────────────────────────
//  TKTextFieldBridge — NSTextFieldDelegate helper
// ─────────────────────────────────────────────────────────────────────────

namespace tk::macos
{

class NSTextFieldNative : public NativeTextField
{
public:
    NSTextFieldNative(TKSurfaceView* superview);
    ~NSTextFieldNative() override;

    void set_rect(Rect r) override;
    void set_text(std::string text) override;
    std::string text() const override;
    void set_placeholder(std::string text) override;
    void set_focused(bool focused) override;
    void set_visible(bool visible) override;
    void set_enabled(bool enabled) override;
    void set_password(bool password) override;
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_focus_changed(std::function<void(bool)> cb) override
    {
        on_focus_changed_ = std::move(cb);
    }

    void notify_changed();
    void notify_submit();
    void notify_focus_gained();
    void notify_focus_lost();

private:
    TKSurfaceView* superview_ = nil;
    NSTextField* field_ = nil;
    TKTextFieldBridge* bridge_ = nil;
    bool is_password_ = false;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<void(bool)> on_focus_changed_;
};

} // namespace tk::macos

@interface TKTextFieldBridge : NSObject <NSTextFieldDelegate>
@property(nonatomic, assign) tk::macos::NSTextFieldNative* owner;
@end

@implementation TKTextFieldBridge

// Fires on every keystroke.
- (void)controlTextDidChange:(NSNotification*)note
{
    if (self.owner)
    {
        self.owner->notify_changed();
    }
}

// Fires when editing begins (field gains focus / field editor appears).
- (void)controlTextDidBeginEditing:(NSNotification*)note
{
    if (self.owner)
    {
        self.owner->notify_focus_gained();
    }
}

// Fires when the field ends editing (return key, focus loss). The Return
// key sets userInfo[@"NSTextMovement"] to NSReturnTextMovement, which is
// how we distinguish "submit" from "tab away".
- (void)controlTextDidEndEditing:(NSNotification*)note
{
    NSNumber* movement = note.userInfo[@"NSTextMovement"];
    if (movement && movement.intValue == NSReturnTextMovement)
    {
        if (self.owner)
        {
            self.owner->notify_submit();
        }
    }
    if (self.owner)
    {
        self.owner->notify_focus_lost();
    }
}

@end

namespace tk::macos
{

NSTextFieldNative::NSTextFieldNative(TKSurfaceView* superview)
    : superview_(superview)
{
    field_ = [[NSTextField alloc] initWithFrame:NSZeroRect];
    field_.bezeled = NO;
    field_.bordered = NO;
    field_.drawsBackground = NO;
    field_.focusRingType = NSFocusRingTypeNone;
    field_.editable = YES;
    field_.selectable = YES;
    field_.usesSingleLineMode = YES;
    field_.translatesAutoresizingMaskIntoConstraints = YES;
    [superview_ addSubview:field_];

    bridge_ = [[TKTextFieldBridge alloc] init];
    bridge_.owner = this;
    field_.delegate = bridge_;
}

NSTextFieldNative::~NSTextFieldNative()
{
    if (bridge_)
    {
        bridge_.owner = nullptr;
    }
    field_.delegate = nil;
    [field_ removeFromSuperview];
    field_ = nil;
    bridge_ = nil;
}

void NSTextFieldNative::set_rect(Rect r)
{
    // Superview reports `isFlipped == YES`, so y grows downward —
    // matches the toolkit convention. Use the field's intrinsic height
    // and centre it vertically within the allocated rect.
    CGFloat nat_h = field_.intrinsicContentSize.height;
    CGFloat h = (nat_h > 0) ? nat_h : std::round(r.h);
    CGFloat y = std::floor(r.y) + (std::round(r.h) - h) / 2.0;
    field_.frame = NSMakeRect(std::floor(r.x), y, std::round(r.w), h);
}

void NSTextFieldNative::set_text(std::string text)
{
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    field_.stringValue = s ?: @"";
}

std::string NSTextFieldNative::text() const
{
    NSString* s = field_.stringValue ?: @"";
    return [s UTF8String] ? std::string([s UTF8String]) : std::string{};
}

void NSTextFieldNative::set_placeholder(std::string text)
{
    field_.placeholderString =
        [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

void NSTextFieldNative::set_focused(bool focused)
{
    if (focused)
    {
        [field_.window makeFirstResponder:field_];
    }
}

void NSTextFieldNative::set_visible(bool visible)
{
    field_.hidden = !visible;
}
void NSTextFieldNative::set_enabled(bool enabled)
{
    field_.enabled = enabled;
}

void NSTextFieldNative::set_password(bool password)
{
    if (password == is_password_)
    {
        return;
    }
    is_password_ = password;

    // NSSecureTextField is a distinct class from NSTextField; there is no
    // in-place echo-mode toggle like Qt's setEchoMode. Swap the subview.
    NSString* val = field_.stringValue ?: @"";
    NSRect frame = field_.frame;
    NSString* placeholder = field_.placeholderString ?: @"";
    BOOL hidden = field_.hidden;
    BOOL enabled = field_.enabled;
    NSWindow* win = field_.window;

    // Detect whether the field currently owns the key focus (the window's
    // field editor is active and its delegate points back to this field).
    BOOL wasFocused = NO;
    if (win)
    {
        NSResponder* fr = win.firstResponder;
        wasFocused = (fr == field_) ||
                     ([fr isKindOfClass:[NSText class]] &&
                      [(NSText*)fr delegate] == (id<NSTextDelegate>)field_);
    }

    field_.delegate = nil;
    bridge_.owner = nullptr;
    [field_ removeFromSuperview];

    NSTextField* newField =
        password ? [[NSSecureTextField alloc] initWithFrame:frame]
                 : [[NSTextField alloc] initWithFrame:frame];
    newField.bezeled = NO;
    newField.bordered = NO;
    newField.drawsBackground = NO;
    newField.focusRingType = NSFocusRingTypeNone;
    newField.editable = YES;
    newField.selectable = YES;
    newField.usesSingleLineMode = YES;
    newField.translatesAutoresizingMaskIntoConstraints = YES;
    newField.stringValue = val;
    newField.placeholderString = placeholder;
    newField.hidden = hidden;
    newField.enabled = enabled;
    [superview_ addSubview:newField];

    newField.delegate = bridge_;
    bridge_.owner = this;
    field_ = newField;

    if (wasFocused && win)
    {
        [win makeFirstResponder:field_];
    }
}

void NSTextFieldNative::notify_changed()
{
    if (on_changed_)
    {
        on_changed_(text());
    }
}
void NSTextFieldNative::notify_submit()
{
    if (on_submit_)
    {
        on_submit_();
    }
}
void NSTextFieldNative::notify_focus_gained()
{
    if (on_focus_changed_)
    {
        on_focus_changed_(true);
    }
}
void NSTextFieldNative::notify_focus_lost()
{
    if (on_focus_changed_)
    {
        on_focus_changed_(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  NSTextViewNative — multi-line NSTextView in an NSScrollView overlay
// ─────────────────────────────────────────────────────────────────────────

class NSTextViewNative : public NativeTextArea
{
public:
    NSTextViewNative(TKSurfaceView* superview);
    ~NSTextViewNative() override;

    void set_rect(Rect r) override;
    void set_text(std::string text) override;
    std::string text() const override;
    void set_placeholder(std::string text) override;
    void set_focused(bool focused) override;
    void set_visible(bool visible) override;
    void set_enabled(bool enabled) override;
    float natural_height() const override;
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override
    {
        on_height_changed_ = std::move(cb);
    }
    void set_on_image_paste(ImagePasteHandler cb) override
    {
        on_image_paste_ = std::move(cb);
    }
    void insert_at_cursor(std::string text) override;

    void notify_changed();
    void notify_submit();
    // Called from the NSTextView subclass when `-paste:` runs. Returns
    // true when the handler consumed the paste; false to let AppKit
    // proceed with the default text paste.
    bool maybe_handle_paste();

    tk::Rect cursor_rect() const override;
    void replace_range(int start, int end, std::string text) override;
    void set_on_popup_nav(std::function<bool(NavKey)> fn) override
    {
        popup_nav_ = std::move(fn);
    }

    void set_on_edit_last(std::function<bool()> fn) override
    {
        on_edit_last_ = std::move(fn);
    }

    std::function<bool(NavKey)> popup_nav_;
    std::function<bool()> on_edit_last_;

private:
    TKSurfaceView* superview_ = nil;
    NSScrollView* scroll_ = nil;
    NSTextView* view_ = nil;
    TKTextViewBridge* bridge_ = nil;
    NSTextField* placeholder_ = nil;
    float last_height_ = 0.f;
    std::string placeholder_text_;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<void(float)> on_height_changed_;
    ImagePasteHandler on_image_paste_;
};

} // namespace tk::macos

// NSTextView subclass that defers paste: to the C++ owner. When the owner
// reports it consumed the paste (because there's image data on the
// pasteboard), we skip the super implementation so AppKit doesn't insert
// a placeholder character or attachment for the image.
@interface TKComposeTextView : NSTextView
@property(nonatomic, assign) tk::macos::NSTextViewNative* owner;
@end

@implementation TKComposeTextView
- (void)paste:(id)sender
{
    if (self.owner && self.owner->maybe_handle_paste())
    {
        return;
    }
    [super paste:sender];
}
// Cmd+Shift+V — paste-as-plain-text on macOS. Same routing.
- (void)pasteAsPlainText:(id)sender
{
    if (self.owner && self.owner->maybe_handle_paste())
    {
        return;
    }
    [super pasteAsPlainText:sender];
}
- (void)keyDown:(NSEvent*)event
{
    if (self.owner && self.owner->popup_nav_)
    {
        unsigned short kc = event.keyCode;
        tk::NativeTextArea::NavKey nk{};
        bool is_nav = true;
        if (kc == 126)
        {
            nk = tk::NativeTextArea::NavKey::Up;
        }
        else if (kc == 125)
        {
            nk = tk::NativeTextArea::NavKey::Down;
        }
        else if (kc == 53)
        {
            nk = tk::NativeTextArea::NavKey::Escape;
        }
        else if (kc == 48)
        {
            nk = (event.modifierFlags & NSEventModifierFlagShift)
                     ? tk::NativeTextArea::NavKey::ShiftTab
                     : tk::NativeTextArea::NavKey::Tab;
        }
        else
        {
            is_nav = false;
        }
        if (is_nav && self.owner->popup_nav_(nk))
        {
            return;
        }
    }
    // Up in an empty composer (popup didn't consume it) → edit the last
    // own message (Element/Slack convention).
    if (self.owner && self.owner->on_edit_last_ && event.keyCode == 126 &&
        self.string.length == 0)
    {
        if (self.owner->on_edit_last_())
        {
            return;
        }
    }
    [super keyDown:event];
}
@end

// `NSTextViewDelegate` gives us textDidChange + the Return-key trap via
// `textView:doCommandBySelector:`. Shift+Return falls through to
// insertNewline:, so we only swallow plain Return.
@interface TKTextViewBridge : NSObject <NSTextViewDelegate>
@property(nonatomic, assign) tk::macos::NSTextViewNative* owner;
@end

@implementation TKTextViewBridge

- (void)textDidChange:(NSNotification*)note
{
    if (self.owner)
    {
        self.owner->notify_changed();
    }
}

- (BOOL)textView:(NSTextView*)tv doCommandBySelector:(SEL)sel
{
    if (sel == @selector(insertNewline:))
    {
        if (self.owner)
        {
            self.owner->notify_submit();
        }
        return YES; // swallowed — caller doesn't insert the newline
    }
    return NO;
}

@end

namespace tk::macos
{

NSTextViewNative::NSTextViewNative(TKSurfaceView* superview)
    : superview_(superview)
{
    scroll_ = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll_.borderType = NSNoBorder;
    scroll_.hasVerticalScroller = YES;
    scroll_.hasHorizontalScroller = NO;
    scroll_.autohidesScrollers = YES;
    scroll_.drawsBackground = NO;

    TKComposeTextView* tv =
        [[TKComposeTextView alloc] initWithFrame:NSMakeRect(0, 0, 200, 40)];
    tv.owner = this;
    view_ = tv;
    view_.minSize = NSMakeSize(0, 0);
    view_.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view_.verticallyResizable = YES;
    view_.horizontallyResizable = NO;
    view_.autoresizingMask = NSViewWidthSizable;
    view_.textContainer.widthTracksTextView = YES;
    view_.textContainer.containerSize =
        NSMakeSize(scroll_.contentSize.width, FLT_MAX);
    view_.richText = NO;
    view_.usesFontPanel = NO;
    view_.allowsUndo = YES;
    view_.font = [NSFont systemFontOfSize:13];
    view_.textContainerInset = NSMakeSize(4, 6);

    scroll_.documentView = view_;
    [superview_ addSubview:scroll_];

    // Placeholder overlay — shown when text is empty and a placeholder string
    // is set. Positioned as a sibling of scroll_ above it in z-order so it
    // appears inside the text area at the first-line origin.
    placeholder_ = [NSTextField labelWithString:@""];
    placeholder_.font = view_.font;
    placeholder_.textColor = NSColor.placeholderTextColor;
    placeholder_.hidden = YES;
    [superview_ addSubview:placeholder_];

    bridge_ = [[TKTextViewBridge alloc] init];
    bridge_.owner = this;
    view_.delegate = bridge_;
}

NSTextViewNative::~NSTextViewNative()
{
    if (bridge_)
    {
        bridge_.owner = nullptr;
    }
    view_.delegate = nil;
    if ([view_ isKindOfClass:[TKComposeTextView class]])
    {
        static_cast<TKComposeTextView*>(view_).owner = nullptr;
    }
    [scroll_ removeFromSuperview];
    [placeholder_ removeFromSuperview];
    scroll_ = nil;
    view_ = nil;
    bridge_ = nil;
    placeholder_ = nil;
}

void NSTextViewNative::set_rect(Rect r)
{
    // Superview is flipped (y grows downward). NSTextView draws text
    // top-aligned, so centre the scroller within the rect when its
    // natural height is shorter than the rect (a single line in a tall
    // card); fill the rect when content overflows so it scrolls instead.
    // Mirrors NSTextFieldNative::set_rect.
    CGFloat rh = std::round(r.h);
    CGFloat nh = natural_height();
    CGFloat h = (nh > 0 && nh < rh) ? nh : rh;
    CGFloat y = std::floor(r.y) + (rh - h) / 2.0;
    scroll_.frame = NSMakeRect(std::floor(r.x), y, std::round(r.w), h);
    if (placeholder_)
    {
        // Offsets match textContainerInset (width=4, height=6); no bezel.
        placeholder_.frame = NSMakeRect(
            scroll_.frame.origin.x + 4,
            scroll_.frame.origin.y + 6,
            std::max(0.0, scroll_.frame.size.width - 12),
            20);
    }
}

void NSTextViewNative::set_text(std::string t)
{
    NSString* s = [NSString stringWithUTF8String:t.c_str()];
    [view_.textStorage.mutableString setString:(s ?: @"")];
    if (placeholder_)
        placeholder_.hidden = !t.empty() || placeholder_text_.empty();
}

std::string NSTextViewNative::text() const
{
    NSString* s = view_.textStorage.string ?: @"";
    return [s UTF8String] ? std::string([s UTF8String]) : std::string{};
}

void NSTextViewNative::set_placeholder(std::string ph)
{
    placeholder_text_ = ph;
    NSString* s = [NSString stringWithUTF8String:ph.c_str()];
    placeholder_.stringValue = s ?: @"";
    placeholder_.hidden = scroll_.hidden || !text().empty() || ph.empty();
}

void NSTextViewNative::set_focused(bool focused)
{
    if (focused)
    {
        [view_.window makeFirstResponder:view_];
    }
}
void NSTextViewNative::set_visible(bool visible)
{
    scroll_.hidden = !visible;
    if (placeholder_)
        placeholder_.hidden = !visible || !text().empty() || placeholder_text_.empty();
}
void NSTextViewNative::set_enabled(bool enabled)
{
    view_.editable = enabled;
    view_.selectable = enabled;
}

float NSTextViewNative::natural_height() const
{
    if (!view_)
    {
        return 0.f;
    }
    [view_.layoutManager ensureLayoutForTextContainer:view_.textContainer];
    NSRect used =
        [view_.layoutManager usedRectForTextContainer:view_.textContainer];
    return static_cast<float>(used.size.height +
                              view_.textContainerInset.height * 2);
}

void NSTextViewNative::notify_changed()
{
    if (on_changed_)
    {
        on_changed_(text());
    }
    if (placeholder_)
        placeholder_.hidden = scroll_.hidden || !text().empty() || placeholder_text_.empty();
    float h = natural_height();
    if (h != last_height_ && on_height_changed_)
    {
        last_height_ = h;
        on_height_changed_(h);
    }
}
void NSTextViewNative::notify_submit()
{
    if (on_submit_)
    {
        on_submit_();
    }
}

void NSTextViewNative::insert_at_cursor(std::string text)
{
    if (!view_)
    {
        return;
    }
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    if (!s)
    {
        return;
    }
    NSRange sel = view_.selectedRange;
    if ([view_ shouldChangeTextInRange:sel replacementString:s])
    {
        [view_.textStorage replaceCharactersInRange:sel withString:s];
        [view_ didChangeText];
        NSRange after = NSMakeRange(sel.location + s.length, 0);
        [view_ setSelectedRange:after];
    }
}

tk::Rect NSTextViewNative::cursor_rect() const
{
    if (!view_)
    {
        return {};
    }
    NSRange sel = view_.selectedRange;
    NSRange glyph = [view_.layoutManager
        glyphRangeForCharacterRange:NSMakeRange(sel.location, 0)
               actualCharacterRange:nullptr];
    NSRect cr = [view_.layoutManager
        boundingRectForGlyphRange:NSMakeRange(glyph.location, 0)
                  inTextContainer:view_.textContainer];
    cr.origin.x += view_.textContainerInset.width;
    cr.origin.y += view_.textContainerInset.height;
    // Convert to the TKSurfaceView (superview_) so the result is in the same
    // y-down coordinate space as the rest of the tk widget tree — not to
    // view_.superview (NSClipView), which is buried inside the NSScrollView
    // and has a different origin.
    NSRect inSuper = [view_ convertRect:cr toView:superview_];
    return {float(inSuper.origin.x), float(inSuper.origin.y),
            float(inSuper.size.width), float(inSuper.size.height)};
}

void NSTextViewNative::replace_range(int start, int end, std::string text)
{
    if (!view_)
    {
        return;
    }
    NSString* ns = view_.string;
    NSData* utf8 = [ns dataUsingEncoding:NSUTF8StringEncoding];
    int bounded_start = std::min(start, (int)utf8.length);
    int bounded_end = std::min(end, (int)utf8.length);
    NSString* prefix_s = [[NSString alloc]
        initWithData:[utf8 subdataWithRange:NSMakeRange(0, bounded_start)]
            encoding:NSUTF8StringEncoding];
    NSString* prefix_e = [[NSString alloc]
        initWithData:[utf8 subdataWithRange:NSMakeRange(0, bounded_end)]
            encoding:NSUTF8StringEncoding];
    NSRange range =
        NSMakeRange(prefix_s.length, prefix_e.length - prefix_s.length);
    [view_ insertText:[NSString stringWithUTF8String:text.c_str()]
        replacementRange:range];
}

bool NSTextViewNative::maybe_handle_paste()
{
    if (!on_image_paste_)
    {
        return false;
    }
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    if (!pb)
    {
        return false;
    }

    // Prefer pre-encoded payloads to avoid a TIFF → PNG re-encode.
    NSDictionary<NSPasteboardType, NSString*>* type_to_mime = @{
        NSPasteboardTypePNG : @"image/png",
        (NSPasteboardType) @"public.jpeg" : @"image/jpeg",
        (NSPasteboardType) @"public.webp" : @"image/webp",
    };
    for (NSPasteboardType pt in type_to_mime)
    {
        NSData* d = [pb dataForType:pt];
        if (d.length > 0)
        {
            std::vector<std::uint8_t> bytes(
                static_cast<const std::uint8_t*>(d.bytes),
                static_cast<const std::uint8_t*>(d.bytes) + d.length);
            std::string mime = [type_to_mime[pt] UTF8String];
            on_image_paste_(std::move(bytes), std::move(mime));
            return true;
        }
    }

    // Fallback: TIFF (default screenshot format) → PNG re-encode.
    NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF];
    if (tiff.length > 0)
    {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithData:tiff];
        if (rep)
        {
            NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}];
            if (png.length > 0)
            {
                std::vector<std::uint8_t> bytes(
                    static_cast<const std::uint8_t*>(png.bytes),
                    static_cast<const std::uint8_t*>(png.bytes) + png.length);
                on_image_paste_(std::move(bytes), "image/png");
                return true;
            }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────
//  Host implementation
// ─────────────────────────────────────────────────────────────────────────

void Host::request_repaint()
{
    if (view_)
    {
        [view_ setNeedsDisplay:YES];
    }
}

void Host::post_to_ui(std::function<void()> task)
{
    __block std::function<void()> captured = std::move(task);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (captured)
        {
            captured();
        }
    });
}

void Host::post_delayed(int ms, std::function<void()> fn)
{
    __block std::function<void()> captured = std::move(fn);
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW,
                      static_cast<int64_t>(ms < 0 ? 0 : ms) * NSEC_PER_MSEC),
        dispatch_get_main_queue(), ^{
            if (captured)
            {
                captured();
            }
        });
}

std::unique_ptr<NativeTextField> Host::make_text_field()
{
    if (!view_)
    {
        return nullptr;
    }
    return std::make_unique<NSTextFieldNative>(view_);
}

std::unique_ptr<NativeTextArea> Host::make_text_area()
{
    if (!view_)
    {
        return nullptr;
    }
    return std::make_unique<NSTextViewNative>(view_);
}

// Defined in audio_macos.mm.
std::unique_ptr<tk::AudioPlayer> make_audio_player_macos();

std::unique_ptr<tk::AudioPlayer> Host::make_audio_player()
{
    return make_audio_player_macos();
}

// Defined in audio_capture_macos.mm.
std::unique_ptr<tk::AudioCapture>
make_audio_capture_macos(tk::AudioCapturePostFn post);

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return make_audio_capture_macos(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}

// Defined in video_macos.mm.
std::unique_ptr<tk::VideoPlayer> make_video_player_macos();

std::unique_ptr<tk::VideoPlayer> Host::make_video_player()
{
    return make_video_player_macos();
}

EncodedImage Host::encode_for_send(const std::uint8_t* data, std::size_t len,
                                   bool compress)
{
    EncodedImage out{};
    if (!data || len == 0)
    {
        return out;
    }

    NSData* src = [NSData dataWithBytes:data length:len];
    CGImageSourceRef src_ref =
        CGImageSourceCreateWithData((__bridge CFDataRef)src, nullptr);
    if (!src_ref)
    {
        return out;
    }

    CGImageRef cg = CGImageSourceCreateImageAtIndex(src_ref, 0, nullptr);
    CFStringRef src_type = CGImageSourceGetType(src_ref);
    if (!cg)
    {
        CFRelease(src_ref);
        return out;
    }

    const std::size_t src_w = CGImageGetWidth(cg);
    const std::size_t src_h = CGImageGetHeight(cg);

    if (!compress)
    {
        out.bytes.assign(data, data + len);
        if (src_type && UTTypeEqual(src_type, kUTTypePNG))
        {
            out.mime = "image/png";
        }
        else if (src_type && UTTypeEqual(src_type, kUTTypeJPEG))
        {
            out.mime = "image/jpeg";
        }
        else if (src_type && UTTypeEqual(src_type, kUTTypeGIF))
        {
            out.mime = "image/gif";
        }
        else
        {
            out.mime = "image/png";
        }
        out.width = static_cast<std::uint32_t>(src_w);
        out.height = static_cast<std::uint32_t>(src_h);
        CGImageRelease(cg);
        CFRelease(src_ref);
        return out;
    }

    constexpr std::size_t kMaxW = 1600;
    constexpr std::size_t kMaxH = 1200;
    std::size_t dst_w = src_w, dst_h = src_h;
    if (src_w > kMaxW || src_h > kMaxH)
    {
        double s = std::min({1.0, static_cast<double>(kMaxW) / src_w,
                             static_cast<double>(kMaxH) / src_h});
        dst_w = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::round(src_w * s)));
        dst_h = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::round(src_h * s)));
    }

    // Draw into a fresh BGRA bitmap context at the target size; CG handles
    // the downscale via the context's interpolation quality.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bm = CGBitmapContextCreate(nullptr, dst_w, dst_h, 8, 0, cs,
                                            kCGImageAlphaPremultipliedFirst |
                                                kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!bm)
    {
        CGImageRelease(cg);
        CFRelease(src_ref);
        return EncodedImage{};
    }
    CGContextSetInterpolationQuality(bm, kCGInterpolationHigh);
    CGContextDrawImage(bm, CGRectMake(0, 0, dst_w, dst_h), cg);
    CGImageRef scaled = CGBitmapContextCreateImage(bm);
    CGContextRelease(bm);
    CGImageRelease(cg);
    CFRelease(src_ref);
    if (!scaled)
    {
        return EncodedImage{};
    }

    CFMutableDataRef out_data = CFDataCreateMutable(nullptr, 0);
    CGImageDestinationRef dst =
        CGImageDestinationCreateWithData(out_data, kUTTypeJPEG, 1, nullptr);
    if (!dst)
    {
        CGImageRelease(scaled);
        CFRelease(out_data);
        return EncodedImage{};
    }
    NSDictionary* opts = @{
        (NSString*)kCGImageDestinationLossyCompressionQuality : @0.75,
    };
    CGImageDestinationAddImage(dst, scaled, (__bridge CFDictionaryRef)opts);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(scaled);
    if (!ok)
    {
        CFRelease(out_data);
        return EncodedImage{};
    }

    const std::uint8_t* b = CFDataGetBytePtr(out_data);
    CFIndex n = CFDataGetLength(out_data);
    out.bytes.assign(b, b + n);
    out.mime = "image/jpeg";
    out.width = static_cast<std::uint32_t>(dst_w);
    out.height = static_cast<std::uint32_t>(dst_h);
    CFRelease(out_data);
    return out;
}

void Host::relayout()
{
    if (!root_ || !view_)
    {
        return;
    }
    NSRect b = view_.bounds;
    LayoutCtx ctx{*factory_, *theme_};
    Rect bounds{0, 0, static_cast<float>(b.size.width),
                static_cast<float>(b.size.height)};
    root_->measure(ctx, {bounds.w, bounds.h});
    root_->arrange(ctx, bounds);
    if (on_layout_)
    {
        on_layout_();
    }
    request_repaint();
}

void Host::on_draw(CGContextRef ctx)
{
    if (!root_ || !ctx)
    {
        return;
    }
    auto canvas = cg::make_canvas(ctx);
    canvas->clear(transparent_ ? Color{0, 0, 0, 0} : theme_->palette.bg);
    PaintCtx pc{*canvas, *factory_, *theme_};
    root_->paint(pc);

    if (drag_active_ && view_)
    {
        NSRect b = view_.bounds;
        const float inset = 8.0f;
        Rect area{
            inset, inset,
            std::max(0.0f, static_cast<float>(b.size.width) - inset * 2),
            std::max(0.0f, static_cast<float>(b.size.height) - inset * 2)};
        if (area.w > 0 && area.h > 0)
        {
            Color accent = theme_->palette.accent;
            Color fill = accent;
            fill.a = 28;
            Color stroke = accent;
            stroke.a = 192;
            canvas->fill_rounded_rect(area, 12.0f, fill);
            canvas->stroke_rounded_rect(area, 12.0f, stroke, 2.0f);
            TextStyle st{};
            st.role = FontRole::Title;
            auto layout = factory_->build_text("Drop to attach", st);
            if (layout)
            {
                Size sz = layout->measure();
                canvas->draw_text(*layout,
                                  {area.x + (area.w - sz.w) * 0.5f,
                                   area.y + (area.h - sz.h) * 0.5f},
                                  accent);
            }
        }
    }
}

void Host::on_layout_changed()
{
    relayout();
}

void Host::on_pointer_down(NSPoint p)
{
    if (!root_)
    {
        return;
    }
    Point local{static_cast<float>(p.x), static_cast<float>(p.y)};
    pressed_widget_ = root_->dispatch_pointer_down(local);
    if (pressed_widget_)
    {
        request_repaint();
    }
}

void Host::on_pointer_up(NSPoint p)
{
    if (!pressed_widget_)
    {
        return;
    }
    Point world{static_cast<float>(p.x), static_cast<float>(p.y)};
    Point ws = pressed_widget_->world_to_local(world);
    bool inside =
        (ws.x >= 0 && ws.y >= 0 && ws.x < pressed_widget_->bounds().w &&
         ws.y < pressed_widget_->bounds().h);
    pressed_widget_->on_pointer_up(ws, inside);
    pressed_widget_ = nullptr;
    request_repaint();
}

void Host::on_pointer_move(NSPoint p)
{
    if (!root_)
    {
        return;
    }
    Point local{static_cast<float>(p.x), static_cast<float>(p.y)};
    if (pressed_widget_)
    {
        Point ws = pressed_widget_->world_to_local(local);
        pressed_widget_->on_pointer_drag(ws);
        request_repaint();
        return;
    }
    Widget* hit = root_->hit_test(local);
    Button* hovered = dynamic_cast<Button*>(hit);
    bool btn_changed = (hovered != hovered_btn_);
    if (btn_changed)
    {
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(false);
        }
        hovered_btn_ = hovered;
        if (hovered_btn_)
        {
            hovered_btn_->set_hovered(true);
        }
    }
    bool dirty = false;
    Widget* moved = root_->dispatch_pointer_move(local, &dirty);
    bool widget_changed = (moved != hovered_widget_);
    if (widget_changed)
    {
        if (hovered_widget_)
        {
            hovered_widget_->on_pointer_leave();
        }
        hovered_widget_ = moved;
    }
    if (btn_changed || widget_changed || dirty)
    {
        request_repaint();
    }
}

void Host::on_pointer_leave()
{
    if (hovered_btn_)
    {
        hovered_btn_->set_hovered(false);
        hovered_btn_ = nullptr;
    }
    if (hovered_widget_)
    {
        hovered_widget_->on_pointer_leave();
        hovered_widget_ = nullptr;
    }
    if (pressed_widget_)
    {
        pressed_widget_->on_pointer_up({-1, -1}, false);
        pressed_widget_ = nullptr;
    }
    request_repaint();
}

void Host::on_wheel(NSPoint p, CGFloat dx, CGFloat dy)
{
    if (!root_)
    {
        return;
    }
    if (root_->dispatch_wheel(
            {static_cast<float>(p.x), static_cast<float>(p.y)},
            static_cast<float>(dx), static_cast<float>(dy)))
    {
        request_repaint();
        on_pointer_move(p);
    }
}

void Host::on_right_click(NSPoint p)
{
    if (!on_right_click_)
    {
        return;
    }
    on_right_click_(
        tk::Point{static_cast<float>(p.x), static_cast<float>(p.y)});
}

// ─────────────────────────────────────────────────────────────────────────
//  Host — drag-and-drop helpers (pasteboard inspection + dispatch)
// ─────────────────────────────────────────────────────────────────────────

namespace
{

const char* mime_from_extension_lower(NSString* ext_lower)
{
    if ([ext_lower isEqualToString:@"png"])
    {
        return "image/png";
    }
    if ([ext_lower isEqualToString:@"jpg"] ||
        [ext_lower isEqualToString:@"jpeg"])
    {
        return "image/jpeg";
    }
    if ([ext_lower isEqualToString:@"webp"])
    {
        return "image/webp";
    }
    if ([ext_lower isEqualToString:@"bmp"])
    {
        return "image/bmp";
    }
    if ([ext_lower isEqualToString:@"gif"])
    {
        return "image/gif";
    }
    return nullptr;
}

NSArray<NSURL*>* all_file_urls(NSPasteboard* pb)
{
    return [pb readObjectsForClasses:@[ NSURL.class ]
                             options:@{
                                 NSPasteboardURLReadingFileURLsOnlyKey : @YES
                             }];
}

// Best-effort MIME from a file extension via macOS UTType (10.15+). Falls
// back to the local image table, then application/octet-stream.
std::string mime_for_url(NSURL* url)
{
    NSString* ext = url.pathExtension.lowercaseString;
    if (ext.length > 0)
    {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
        if (@available(macOS 11.0, *))
        {
            UTType* type = [UTType typeWithFilenameExtension:ext];
            if (type)
            {
                NSString* mime = type.preferredMIMEType;
                if (mime.length > 0)
                {
                    return mime.UTF8String;
                }
            }
        }
#endif
        if (const char* m = mime_from_extension_lower(ext))
        {
            return m;
        }
    }
    return "application/octet-stream";
}

} // namespace

void Host::set_drag_active(bool active)
{
    if (drag_active_ == active)
    {
        return;
    }
    drag_active_ = active;
    if (view_)
    {
        view_.needsDisplay = YES;
    }
}

bool Host::pasteboard_has_dropable(NSPasteboard* pb) const
{
    if (!pb)
    {
        return false;
    }
    NSArray<NSURL*>* urls = all_file_urls(pb);
    if (urls.count > 0)
    {
        return true;
    }
    if ([pb dataForType:NSPasteboardTypePNG] != nil)
    {
        return true;
    }
    if ([pb dataForType:@"public.jpeg"] != nil)
    {
        return true;
    }
    return false;
}

bool Host::dispatch_file_drop(NSPasteboard* pb)
{
    if (!pb || !on_file_drop_)
    {
        return false;
    }
    bool any = false;

    // 1) File URLs — one handler call per file. The shell branches on mime.
    NSArray<NSURL*>* urls = all_file_urls(pb);
    for (NSURL* url in urls)
    {
        if (!url.isFileURL)
        {
            continue;
        }
        NSString* path = url.path;
        NSError* err = nil;
        NSDictionary<NSFileAttributeKey, id>* attrs =
            [NSFileManager.defaultManager attributesOfItemAtPath:path
                                                           error:&err];
        if (!attrs)
        {
            continue;
        }
        unsigned long long sz = [attrs[NSFileSize] unsignedLongLongValue];
        if (sz == 0 || sz > kMaxDroppedFileBytes)
        {
            continue;
        }

        NSData* data = [NSData dataWithContentsOfFile:path
                                              options:0
                                                error:&err];
        if (!data || data.length == 0)
        {
            continue;
        }

        std::string mime = mime_for_url(url);
        std::vector<std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(data.bytes),
            static_cast<const std::uint8_t*>(data.bytes) + data.length);
        std::string filename =
            path.lastPathComponent.UTF8String
                ? std::string(path.lastPathComponent.UTF8String)
                : std::string{};
        on_file_drop_(std::move(bytes), std::move(mime), std::move(filename));
        any = true;
    }
    if (any)
    {
        return true;
    }

    // 2) In-app image data — try PNG then JPEG. Drop is unnamed.
    NSData* png = [pb dataForType:NSPasteboardTypePNG];
    if (png && png.length > 0 && png.length <= kMaxDroppedImageBytes)
    {
        std::vector<std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(png.bytes),
            static_cast<const std::uint8_t*>(png.bytes) + png.length);
        on_file_drop_(std::move(bytes), "image/png", std::string{});
        return true;
    }
    NSData* jpg = [pb dataForType:@"public.jpeg"];
    if (jpg && jpg.length > 0 && jpg.length <= kMaxDroppedImageBytes)
    {
        std::vector<std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(jpg.bytes),
            static_cast<const std::uint8_t*>(jpg.bytes) + jpg.length);
        on_file_drop_(std::move(bytes), "image/jpeg", std::string{});
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────
//  Surface — public glue
// ─────────────────────────────────────────────────────────────────────────

Surface::Surface(const Theme& theme, bool transparent)
{
    TKSurfaceView* view =
        [[TKSurfaceView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    host_ = std::make_unique<Host>(view, theme, transparent);
    view.hostPtr = host_.get();
    view.transparent = transparent ? YES : NO;
}

Surface::~Surface()
{
    if (host_)
    {
        TKSurfaceView* view = host_->view();
        host_->detach();
        view.hostPtr = nullptr;
    }
}

void* Surface::view_handle() const
{
    return host_ ? (__bridge void*)host_->view() : nullptr;
}

tk::Host& Surface::host()
{
    return *host_;
}
const Theme& Surface::theme() const
{
    return host_->theme();
}

void Surface::set_root(std::unique_ptr<Widget> root)
{
    host_->set_root(std::move(root));
}

Widget* Surface::root() const
{
    return host_->root();
}

void Surface::relayout()
{
    host_->relayout();
}

void Surface::set_theme(const Theme& t)
{
    host_->set_theme(t);
    relayout();
}

void Surface::set_on_layout(std::function<void()> cb)
{
    host_->set_on_layout(std::move(cb));
}

void Surface::set_on_file_drop(FileDropHandler cb)
{
    host_->set_on_file_drop(std::move(cb));
}

void Surface::set_on_right_click(std::function<void(tk::Point)> cb)
{
    host_->set_on_right_click(std::move(cb));
}

} // namespace tk::macos
