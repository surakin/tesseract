#include "host_macos.h"
#include "canvas_cg.h"
#include "controls.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace tk::macos {
class NSTextFieldNative;
class NSTextViewNative;
} // namespace tk::macos

@class TKSurfaceView;
@class TKTextFieldBridge;
@class TKTextViewBridge;

namespace tk::macos {

// ─────────────────────────────────────────────────────────────────────────
//  Host — owns the tree, paints via CoreGraphics + CoreText
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host {
public:
    Host(TKSurfaceView* view, const Theme& theme)
        : view_(view),
          theme_(&theme),
          factory_(cg::make_factory()) {}

    void request_repaint() override;
    void post_to_ui(std::function<void()> task) override;
    std::unique_ptr<NativeTextField> make_text_field() override;
    std::unique_ptr<NativeTextArea>  make_text_area () override;

    void set_root(std::unique_ptr<Widget> root) {
        root_ = std::move(root);
        relayout();
    }
    Widget* root() const { return root_.get(); }
    const Theme& theme() const { return *theme_; }
    CanvasFactory& factory() { return *factory_; }
    TKSurfaceView* view() const { return view_; }
    void detach() { view_ = nil; }

    void relayout();
    void set_on_layout(std::function<void()> cb) { on_layout_ = std::move(cb); }

    void on_draw(CGContextRef ctx);
    void on_layout_changed();

    void on_pointer_down(NSPoint p);
    void on_pointer_up  (NSPoint p);
    void on_pointer_move(NSPoint p);
    void on_pointer_leave();
    void on_wheel       (NSPoint p, CGFloat dx, CGFloat dy);

private:
    TKSurfaceView*                       view_;
    const Theme*                         theme_;
    std::unique_ptr<CanvasFactory>       factory_;
    std::unique_ptr<Widget>              root_;
    std::function<void()>                on_layout_;
    Widget*                              pressed_widget_ = nullptr;
    Button*                              hovered_btn_    = nullptr;
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
@property (nonatomic, assign) tk::macos::Host* hostPtr;
@property (nonatomic, strong) NSTrackingArea*  trackingArea;
@end

@implementation TKSurfaceView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = NO;   // we paint via drawRect: into a fresh CG ctx
    }
    return self;
}

- (BOOL)isFlipped       { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque        { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    if (!self.hostPtr) return;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    self.hostPtr->on_draw(ctx);
}

// NSView calls -layout once per re-layout pass (after frame changes,
// constraint updates, etc.). It's the AppKit equivalent of UIKit's
// `layoutSubviews`.
- (void)layout {
    [super layout];
    if (self.hostPtr) self.hostPtr->on_layout_changed();
}

// Tracking area covers the whole view bounds; hover requires it.
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.trackingArea) {
        [self removeTrackingArea:self.trackingArea];
    }
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited
                                  | NSTrackingMouseMoved
                                  | NSTrackingActiveInKeyWindow
                                  | NSTrackingInVisibleRect;
    self.trackingArea = [[NSTrackingArea alloc]
                           initWithRect:NSZeroRect
                                  options:opts
                                    owner:self
                                 userInfo:nil];
    [self addTrackingArea:self.trackingArea];
}

// ── Mouse events ────────────────────────────────────────────────────────

- (NSPoint)tkLocationFromEvent:(NSEvent*)e {
    return [self convertPoint:e.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent*)e {
    if (self.hostPtr) self.hostPtr->on_pointer_down([self tkLocationFromEvent:e]);
}

- (void)mouseUp:(NSEvent*)e {
    if (self.hostPtr) self.hostPtr->on_pointer_up([self tkLocationFromEvent:e]);
}

- (void)mouseMoved:(NSEvent*)e {
    if (self.hostPtr) self.hostPtr->on_pointer_move([self tkLocationFromEvent:e]);
}

- (void)mouseDragged:(NSEvent*)e {
    if (self.hostPtr) self.hostPtr->on_pointer_move([self tkLocationFromEvent:e]);
}

- (void)mouseExited:(NSEvent*)e {
    if (self.hostPtr) self.hostPtr->on_pointer_leave();
}

// Scroll wheel + trackpad scroll. AppKit reports scroll deltas as pixels
// (with scrollingDeltaX/Y when hasPreciseScrollingDeltas == YES, or
// device units otherwise). Positive deltaY scrolls content downward in
// AppKit's flipped view space, matching the toolkit convention.
- (void)scrollWheel:(NSEvent*)e {
    if (!self.hostPtr) return;
    NSPoint loc = [self tkLocationFromEvent:e];
    CGFloat dx, dy;
    if (e.hasPreciseScrollingDeltas) {
        dx = -e.scrollingDeltaX;
        dy = -e.scrollingDeltaY;
    } else {
        // 1 detent ≈ 30 pixels — matches the Win32 default.
        dx = -e.scrollingDeltaX * 10.0;
        dy = -e.scrollingDeltaY * 10.0;
    }
    self.hostPtr->on_wheel(loc, dx, dy);
}

@end

// ─────────────────────────────────────────────────────────────────────────
//  TKTextFieldBridge — NSTextFieldDelegate helper
// ─────────────────────────────────────────────────────────────────────────

namespace tk::macos {

class NSTextFieldNative : public NativeTextField {
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
    void set_on_changed(std::function<void(const std::string&)> cb) override {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override {
        on_submit_ = std::move(cb);
    }

    void notify_changed();
    void notify_submit ();

private:
    TKSurfaceView*                          superview_ = nil;
    NSTextField*                            field_     = nil;
    TKTextFieldBridge*                      bridge_    = nil;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()>                   on_submit_;
};

} // namespace tk::macos

@interface TKTextFieldBridge : NSObject <NSTextFieldDelegate>
@property (nonatomic, assign) tk::macos::NSTextFieldNative* owner;
@end

@implementation TKTextFieldBridge

// Fires on every keystroke.
- (void)controlTextDidChange:(NSNotification*)note {
    if (self.owner) self.owner->notify_changed();
}

// Fires when the field ends editing (return key, focus loss). The Return
// key sets userInfo[@"NSTextMovement"] to NSReturnTextMovement, which is
// how we distinguish "submit" from "tab away".
- (void)controlTextDidEndEditing:(NSNotification*)note {
    NSNumber* movement = note.userInfo[@"NSTextMovement"];
    if (movement && movement.intValue == NSReturnTextMovement) {
        if (self.owner) self.owner->notify_submit();
    }
}

@end

namespace tk::macos {

NSTextFieldNative::NSTextFieldNative(TKSurfaceView* superview)
    : superview_(superview) {
    field_ = [[NSTextField alloc] initWithFrame:NSZeroRect];
    field_.bezeled         = YES;
    field_.bezelStyle      = NSTextFieldRoundedBezel;
    field_.bordered        = YES;
    field_.editable        = YES;
    field_.selectable      = YES;
    field_.usesSingleLineMode = YES;
    field_.translatesAutoresizingMaskIntoConstraints = YES;
    [superview_ addSubview:field_];

    bridge_       = [[TKTextFieldBridge alloc] init];
    bridge_.owner = this;
    field_.delegate = bridge_;
}

NSTextFieldNative::~NSTextFieldNative() {
    if (bridge_) bridge_.owner = nullptr;
    field_.delegate = nil;
    [field_ removeFromSuperview];
    field_  = nil;
    bridge_ = nil;
}

void NSTextFieldNative::set_rect(Rect r) {
    // Superview reports `isFlipped == YES`, so y grows downward —
    // matches the toolkit convention. AppKit positions subviews by
    // their `frame.origin` in the superview's coordinate system.
    field_.frame = NSMakeRect(std::floor(r.x), std::floor(r.y),
                                std::round(r.w), std::round(r.h));
}

void NSTextFieldNative::set_text(std::string text) {
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    field_.stringValue = s ?: @"";
}

std::string NSTextFieldNative::text() const {
    NSString* s = field_.stringValue ?: @"";
    return [s UTF8String] ? std::string([s UTF8String]) : std::string{};
}

void NSTextFieldNative::set_placeholder(std::string text) {
    field_.placeholderString = [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

void NSTextFieldNative::set_focused(bool focused) {
    if (focused) {
        [field_.window makeFirstResponder:field_];
    }
}

void NSTextFieldNative::set_visible(bool visible) { field_.hidden = !visible; }
void NSTextFieldNative::set_enabled(bool enabled) { field_.enabled = enabled;  }

// Password mode is a no-op on AppKit for now: NSSecureTextField is a
// different class from NSTextField, so toggling the mode requires
// swapping the view. The recovery-banner key field on macOS will show
// plaintext until the SecureTextField path lands.
void NSTextFieldNative::set_password(bool /*password*/) {}

void NSTextFieldNative::notify_changed() {
    if (on_changed_) on_changed_(text());
}
void NSTextFieldNative::notify_submit() {
    if (on_submit_) on_submit_();
}

// ─────────────────────────────────────────────────────────────────────────
//  NSTextViewNative — multi-line NSTextView in an NSScrollView overlay
// ─────────────────────────────────────────────────────────────────────────

class NSTextViewNative : public NativeTextArea {
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
    void set_on_changed(std::function<void(const std::string&)> cb) override {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override {
        on_height_changed_ = std::move(cb);
    }

    void notify_changed();
    void notify_submit ();

private:
    TKSurfaceView*      superview_ = nil;
    NSScrollView*       scroll_    = nil;
    NSTextView*         view_      = nil;
    TKTextViewBridge*   bridge_    = nil;
    float                                    last_height_ = 0.f;
    std::function<void(const std::string&)>  on_changed_;
    std::function<void()>                    on_submit_;
    std::function<void(float)>               on_height_changed_;
};

} // namespace tk::macos

// `NSTextViewDelegate` gives us textDidChange + the Return-key trap via
// `textView:doCommandBySelector:`. Shift+Return falls through to
// insertNewline:, so we only swallow plain Return.
@interface TKTextViewBridge : NSObject <NSTextViewDelegate>
@property (nonatomic, assign) tk::macos::NSTextViewNative* owner;
@end

@implementation TKTextViewBridge

- (void)textDidChange:(NSNotification*)note {
    if (self.owner) self.owner->notify_changed();
}

- (BOOL)textView:(NSTextView*)tv
    doCommandBySelector:(SEL)sel
{
    if (sel == @selector(insertNewline:)) {
        if (self.owner) self.owner->notify_submit();
        return YES;     // swallowed — caller doesn't insert the newline
    }
    return NO;
}

@end

namespace tk::macos {

NSTextViewNative::NSTextViewNative(TKSurfaceView* superview)
    : superview_(superview) {
    scroll_ = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll_.borderType = NSBezelBorder;
    scroll_.hasVerticalScroller = YES;
    scroll_.hasHorizontalScroller = NO;
    scroll_.autohidesScrollers = YES;
    scroll_.drawsBackground = NO;

    view_ = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 200, 40)];
    view_.minSize = NSMakeSize(0, 0);
    view_.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view_.verticallyResizable   = YES;
    view_.horizontallyResizable = NO;
    view_.autoresizingMask      = NSViewWidthSizable;
    view_.textContainer.widthTracksTextView = YES;
    view_.textContainer.containerSize =
        NSMakeSize(scroll_.contentSize.width, FLT_MAX);
    view_.richText = NO;
    view_.usesFontPanel = NO;
    view_.allowsUndo    = YES;
    view_.font          = [NSFont systemFontOfSize:13];
    view_.textContainerInset = NSMakeSize(4, 6);

    scroll_.documentView = view_;
    [superview_ addSubview:scroll_];

    bridge_ = [[TKTextViewBridge alloc] init];
    bridge_.owner = this;
    view_.delegate = bridge_;
}

NSTextViewNative::~NSTextViewNative() {
    if (bridge_) bridge_.owner = nullptr;
    view_.delegate = nil;
    [scroll_ removeFromSuperview];
    scroll_ = nil;
    view_   = nil;
    bridge_ = nil;
}

void NSTextViewNative::set_rect(Rect r) {
    scroll_.frame = NSMakeRect(std::floor(r.x), std::floor(r.y),
                                 std::round(r.w), std::round(r.h));
}

void NSTextViewNative::set_text(std::string t) {
    NSString* s = [NSString stringWithUTF8String:t.c_str()];
    [view_.textStorage.mutableString setString:(s ?: @"")];
}

std::string NSTextViewNative::text() const {
    NSString* s = view_.textStorage.string ?: @"";
    return [s UTF8String] ? std::string([s UTF8String]) : std::string{};
}

void NSTextViewNative::set_placeholder(std::string /*text*/) {
    // NSTextView has no built-in placeholder; shared ComposeBar paints
    // its own placeholder underneath when text() is empty.
}

void NSTextViewNative::set_focused(bool focused) {
    if (focused) [view_.window makeFirstResponder:view_];
}
void NSTextViewNative::set_visible(bool visible) { scroll_.hidden = !visible; }
void NSTextViewNative::set_enabled(bool enabled) {
    view_.editable   = enabled;
    view_.selectable = enabled;
}

float NSTextViewNative::natural_height() const {
    if (!view_) return 0.f;
    [view_.layoutManager ensureLayoutForTextContainer:view_.textContainer];
    NSRect used = [view_.layoutManager usedRectForTextContainer:view_.textContainer];
    return static_cast<float>(used.size.height + view_.textContainerInset.height * 2);
}

void NSTextViewNative::notify_changed() {
    if (on_changed_) on_changed_(text());
    float h = natural_height();
    if (h != last_height_ && on_height_changed_) {
        last_height_ = h;
        on_height_changed_(h);
    }
}
void NSTextViewNative::notify_submit() {
    if (on_submit_) on_submit_();
}

// ─────────────────────────────────────────────────────────────────────────
//  Host implementation
// ─────────────────────────────────────────────────────────────────────────

void Host::request_repaint() {
    if (view_) [view_ setNeedsDisplay:YES];
}

void Host::post_to_ui(std::function<void()> task) {
    __block std::function<void()> captured = std::move(task);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (captured) captured();
    });
}

std::unique_ptr<NativeTextField> Host::make_text_field() {
    if (!view_) return nullptr;
    return std::make_unique<NSTextFieldNative>(view_);
}

std::unique_ptr<NativeTextArea> Host::make_text_area() {
    if (!view_) return nullptr;
    return std::make_unique<NSTextViewNative>(view_);
}

void Host::relayout() {
    if (!root_ || !view_) return;
    NSRect b = view_.bounds;
    LayoutCtx ctx{ *factory_, *theme_ };
    Rect bounds{ 0, 0,
                  static_cast<float>(b.size.width),
                  static_cast<float>(b.size.height) };
    root_->measure(ctx, { bounds.w, bounds.h });
    root_->arrange(ctx, bounds);
    if (on_layout_) on_layout_();
    request_repaint();
}

void Host::on_draw(CGContextRef ctx) {
    if (!root_ || !ctx) return;
    auto canvas = cg::make_canvas(ctx);
    canvas->clear(theme_->palette.bg);
    PaintCtx pc{ *canvas, *factory_, *theme_ };
    root_->paint(pc);
}

void Host::on_layout_changed() { relayout(); }

void Host::on_pointer_down(NSPoint p) {
    if (!root_) return;
    Point local{ static_cast<float>(p.x), static_cast<float>(p.y) };
    pressed_widget_ = root_->dispatch_pointer_down(local);
    if (pressed_widget_) request_repaint();
}

void Host::on_pointer_up(NSPoint p) {
    if (!pressed_widget_) return;
    Point world{ static_cast<float>(p.x), static_cast<float>(p.y) };
    Point ws = pressed_widget_->world_to_local(world);
    bool inside = (ws.x >= 0 && ws.y >= 0 &&
                    ws.x < pressed_widget_->bounds().w &&
                    ws.y < pressed_widget_->bounds().h);
    pressed_widget_->on_pointer_up(ws, inside);
    pressed_widget_ = nullptr;
    request_repaint();
}

void Host::on_pointer_move(NSPoint p) {
    if (!root_) return;
    Point local{ static_cast<float>(p.x), static_cast<float>(p.y) };
    Widget* hit = root_->hit_test(local);
    Button* hovered = dynamic_cast<Button*>(hit);
    if (hovered == hovered_btn_) return;
    if (hovered_btn_) hovered_btn_->set_hovered(false);
    hovered_btn_ = hovered;
    if (hovered_btn_) hovered_btn_->set_hovered(true);
    request_repaint();
}

void Host::on_pointer_leave() {
    if (hovered_btn_) { hovered_btn_->set_hovered(false); hovered_btn_ = nullptr; }
    if (pressed_widget_) {
        pressed_widget_->on_pointer_up({-1, -1}, false);
        pressed_widget_ = nullptr;
    }
    request_repaint();
}

void Host::on_wheel(NSPoint p, CGFloat dx, CGFloat dy) {
    if (!root_) return;
    root_->dispatch_wheel({ static_cast<float>(p.x),
                              static_cast<float>(p.y) },
                            static_cast<float>(dx),
                            static_cast<float>(dy));
}

// ─────────────────────────────────────────────────────────────────────────
//  Surface — public glue
// ─────────────────────────────────────────────────────────────────────────

Surface::Surface(const Theme& theme) {
    TKSurfaceView* view = [[TKSurfaceView alloc]
                            initWithFrame:NSMakeRect(0, 0, 100, 100)];
    host_         = std::make_unique<Host>(view, theme);
    view.hostPtr  = host_.get();
}

Surface::~Surface() {
    if (host_) {
        TKSurfaceView* view = host_->view();
        host_->detach();
        view.hostPtr = nullptr;
    }
}

void* Surface::view_handle() const {
    return host_ ? (__bridge void*)host_->view() : nullptr;
}

tk::Host& Surface::host() { return *host_; }
const Theme& Surface::theme() const { return host_->theme(); }

void Surface::set_root(std::unique_ptr<Widget> root) {
    host_->set_root(std::move(root));
}

Widget* Surface::root() const { return host_->root(); }

void Surface::relayout() { host_->relayout(); }

void Surface::set_on_layout(std::function<void()> cb) {
    host_->set_on_layout(std::move(cb));
}

} // namespace tk::macos
