#include "host_macos.h"
#ifdef TESSERACT_CALLS_ENABLED
#include "audio_playback.h"
#endif
#include "anim_image_cache.h"
#include "canvas_cg.h"
#include "controls.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudio.h>
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

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

class Host : public tk::Host, public tk::AnimDamageSink
{
public:
    Host(TKSurfaceView* view, const Theme& theme, bool transparent = false)
        : view_(view), theme_(&theme), factory_(cg::make_factory()),
          transparent_(transparent)
    {
    }

    void request_repaint() override;

    // Point the damage sink at the shell's animation cache so it can tell
    // animated keys (worth partial-repainting) from static ones.
    void set_anim_cache(const AnimImageCache* cache)
    {
        anim_cache_ = cache;
    }

    // AnimDamageSink: record the on-screen rect of an animated image drawn
    // during the current paint pass. Static images are ignored.
    void note_image(const std::string& key, Rect world) override
    {
        if (anim_cache_ && anim_cache_->has(key))
        {
            anim_damage_.push_back(world);
        }
    }

    // Invalidate only the regions occupied by animated images on the last
    // paint. setNeedsDisplayInRect: clips drawRect:'s CGContext to the union
    // of these rects, so the whole tree is no longer rasterized per frame.
    // Falls back to a full repaint when no animated rect was recorded.
    // Defined out-of-line (uses TKSurfaceView, only forward-declared here).
    void invalidate_anim_damage();
    void post_to_ui(std::function<void()> task) override;
    void post_delayed(int ms, std::function<void()> fn) override;
    std::unique_ptr<NativeTextField> make_text_field() override;
    std::unique_ptr<NativeTextArea> make_text_area() override;
    std::unique_ptr<AudioPlayer> make_audio_player() override;
    std::unique_ptr<AudioCapture> make_audio_capture() override;
    std::unique_ptr<VideoPlayer> make_video_player() override;
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<AudioPlayback> make_audio_playback() override;
#endif
    EncodedImage encode_for_send(const std::uint8_t* data, std::size_t len,
                                 bool compress) override;
    void set_clipboard_text(std::string_view text) override;
    bool set_clipboard_image(std::span<const std::uint8_t> encoded_bytes) override;

    std::vector<tk::DeviceListing> enumerate_audio_inputs()  const override;
    std::vector<tk::DeviceListing> enumerate_audio_outputs() const override;
    std::vector<tk::DeviceListing> enumerate_cameras()       const override;

    void set_root(std::unique_ptr<Widget> root)
    {
        root_ = std::move(root);
        root_->set_subtree_removing_cb([this](Widget* s){
            on_subtree_removing(s);
            subtree_removed_during_dispatch_ = true;
        });
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
    bool on_key_down(const KeyEvent& event);

    // Drag-and-drop. `pasteboard_has_dropable` is consulted from
    // -draggingEntered: to gate the cursor; `dispatch_file_drop` runs
    // the actual decode + handler invocation from -performDragOperation:.
    void set_on_file_drop(FileDropHandler cb)
    {
        on_file_drop_ = std::move(cb);
    }
    void set_on_file_drop_error(FileDropErrorHandler cb)
    {
        on_file_drop_error_ = std::move(cb);
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
    bool dispatch_file_drop(NSPasteboard* pb, tk::Point pos);
    void set_drag_active(bool active);
    bool drag_active() const
    {
        return drag_active_;
    }

protected:
    Widget* input_root_() const override { return root_.get(); }

private:
    TKSurfaceView* view_;
    const Theme* theme_;
    std::unique_ptr<CanvasFactory> factory_;
    bool transparent_ = false;
    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    FileDropHandler on_file_drop_;
    FileDropErrorHandler on_file_drop_error_;
    std::function<void(tk::Point)> on_right_click_;
    bool drag_active_ = false;
    const AnimImageCache* anim_cache_ = nullptr;
    std::vector<Rect> anim_damage_;
    bool subtree_removed_during_dispatch_ = false;
};

} // namespace tk::macos

namespace
{

tk::Key key_from_macos(NSEvent* event)
{
    NSString* chars = event.charactersIgnoringModifiers;
    if (chars.length == 0)
    {
        return tk::Key::Unknown;
    }

    const unichar ch = [chars characterAtIndex:0];
    switch (ch)
    {
    case 0x1B: return tk::Key::Escape;
    case '\r':
    case '\n': return tk::Key::Enter;
    case ' ': return tk::Key::Space;
    case '\t':
        return (event.modifierFlags & NSEventModifierFlagShift)
                   ? tk::Key::Backtab
                   : tk::Key::Tab;
    case NSUpArrowFunctionKey: return tk::Key::Up;
    case NSDownArrowFunctionKey: return tk::Key::Down;
    case NSLeftArrowFunctionKey: return tk::Key::Left;
    case NSRightArrowFunctionKey: return tk::Key::Right;
    case NSHomeFunctionKey: return tk::Key::Home;
    case NSEndFunctionKey: return tk::Key::End;
    case NSPageUpFunctionKey: return tk::Key::PageUp;
    case NSPageDownFunctionKey: return tk::Key::PageDown;
    case 0x7F: return tk::Key::Backspace;
    case NSDeleteFunctionKey: return tk::Key::Delete;
    default: return tk::Key::Unknown;
    }
}

std::string character_text_from_macos(NSEvent* event)
{
    NSString* chars = event.characters;
    if (chars.length == 0)
    {
        return {};
    }

    const unichar ch = [chars characterAtIndex:0];
    if ([[NSCharacterSet controlCharacterSet] characterIsMember:ch])
    {
        return {};
    }

    const char* utf8 = chars.UTF8String;
    return utf8 ? std::string(utf8) : std::string{};
}

tk::KeyEvent translate_key_event(NSEvent* event)
{
    tk::KeyEvent out{};
    out.key = key_from_macos(event);
    out.ctrl = event.modifierFlags & NSEventModifierFlagControl;
    out.shift = event.modifierFlags & NSEventModifierFlagShift;
    out.alt = event.modifierFlags & NSEventModifierFlagOption;
    out.meta = event.modifierFlags & NSEventModifierFlagCommand;
    out.repeat = event.isARepeat;
    if (out.key == tk::Key::Unknown)
    {
        out.text = character_text_from_macos(event);
        if (!out.text.empty())
        {
            out.key = tk::Key::Character;
        }
    }
    return out;
}

} // namespace

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
    [self.window makeFirstResponder:self];
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

- (void)keyDown:(NSEvent*)e
{
    if (self.hostPtr)
    {
        tk::KeyEvent event = translate_key_event(e);
        if (event.key != tk::Key::Unknown && self.hostPtr->on_key_down(event))
        {
            return;
        }
    }
    [super keyDown:e];
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
    // Mirrors -tkLocationFromEvent:'s NSEvent.locationInWindow conversion.
    // NSDraggingInfo.draggingLocation's exact coordinate space (window vs.
    // already-view-space) isn't confirmed against this SDK revision on real
    // hardware — flagging this as the one part of the drop-position
    // plumbing that needs on-device verification.
    NSPoint loc = [self convertPoint:sender.draggingLocation fromView:nil];
    tk::Point pos{static_cast<float>(loc.x), static_cast<float>(loc.y)};
    BOOL ok = self.hostPtr->dispatch_file_drop(sender.draggingPasteboard, pos)
                 ? YES
                 : NO;
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
    void set_on_popup_nav(std::function<bool(NavKey)> cb) override
    {
        popup_nav_ = std::move(cb);
    }

    void notify_changed();
    void notify_submit();
    void notify_focus_gained();
    void notify_focus_lost();

    // Public so TKTextFieldBridge's doCommandBySelector: can forward Up / Down
    // / Escape to the popup the field drives (the Ctrl+K quick switcher).
    std::function<bool(NavKey)> popup_nav_;

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

// Up / Down / Escape navigation forwarded to a popup the field drives (the
// Ctrl+K quick switcher). The field editor routes these as command selectors;
// returning YES consumes the command so the caret / field editor ignores it.
- (BOOL)control:(NSControl*)control
              textView:(NSTextView*)textView
    doCommandBySelector:(SEL)commandSelector
{
    if (!self.owner || !self.owner->popup_nav_)
    {
        return NO;
    }
    tk::NavKey nk{};
    bool is_nav = true;
    if (commandSelector == @selector(moveUp:))
    {
        nk = tk::NavKey::Up;
    }
    else if (commandSelector == @selector(moveDown:))
    {
        nk = tk::NavKey::Down;
    }
    else if (commandSelector == @selector(cancelOperation:))
    {
        nk = tk::NavKey::Escape;
    }
    else
    {
        is_nav = false;
    }
    if (is_nav && self.owner->popup_nav_(nk))
    {
        return YES;
    }
    return NO;
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
    bool visible() const override
    {
        return visible_;
    }
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
    int cursor_byte_pos() const override;
    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room) override;
    void insert_emoticon(int start, int end, const std::string& shortcode,
                         const std::string& mxc_url, const tk::Image* image) override;
    std::vector<tesseract::MentionSeg> composer_draft() const override;
    void set_mention_colors(Color bg, Color fg) override;
    void set_font_role(FontRole role) override
    {
        const int base = static_cast<int>(std::round([NSFont systemFontSize]));
        const auto pt  = static_cast<CGFloat>(font_role_pt(role, base));
        NSFont* f = font_role_is_semibold(role) ? [NSFont boldSystemFontOfSize:pt]
                                                : [NSFont systemFontOfSize:pt];
        view_.font = f;
        if (placeholder_) placeholder_.font = f;
    }
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
    ImagePasteHandler on_image_paste_;

private:
    TKSurfaceView* superview_ = nil;
    NSScrollView* scroll_ = nil;
    NSTextView* view_ = nil;
    TKTextViewBridge* bridge_ = nil;
    NSTextField* placeholder_ = nil;
    float last_height_ = 0.f;
    // Tracks the last value passed to set_visible(). Mirrors the
    // freshly-created NSScrollView's default (hidden=NO ⇒ visible).
    bool visible_ = true;
    std::string placeholder_text_;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<void(float)> on_height_changed_;
    NSColor* mention_bg_ = nil;
    NSColor* mention_fg_ = nil;
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
- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    if (item.action == @selector(paste:) || item.action == @selector(pasteAsPlainText:))
    {
        if (self.owner && self.owner->on_image_paste_)
        {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            if ([pb availableTypeFromArray:@[
                    NSPasteboardTypePNG, NSPasteboardTypeTIFF,
                    (NSPasteboardType) @"public.jpeg",
                    (NSPasteboardType) @"public.webp"]])
                return YES;
        }
    }
    return [super validateMenuItem:item];
}
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
        else if (kc == 123)
        {
            nk = tk::NativeTextArea::NavKey::Left;
        }
        else if (kc == 124)
        {
            nk = tk::NativeTextArea::NavKey::Right;
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

// Inline mention pill: an atomic NSTextAttachment carrying the mention's
// metadata, drawn as a rounded chip by its cell. AppKit treats the attachment
// as a single character (U+FFFC), so caret movement / backspace are atomic.
@interface TKMentionAttachment : NSTextAttachment
@property(nonatomic, copy) NSString* userId;
@property(nonatomic, copy) NSString* displayName;
@property(nonatomic, assign) BOOL isRoom;
@end

@implementation TKMentionAttachment
@end

@interface TKMentionCell : NSTextAttachmentCell
@property(nonatomic, copy) NSString* label;
@property(nonatomic, strong) NSColor* bgColor;
@property(nonatomic, strong) NSColor* fgColor;
@end

@implementation TKMentionCell
- (NSDictionary*)textAttrs
{
    return @{
        NSFontAttributeName : [NSFont systemFontOfSize:[NSFont systemFontSize]],
        NSForegroundColorAttributeName :
            (self.fgColor ?: [NSColor controlTextColor])
    };
}
- (NSSize)cellSize
{
    NSSize ts = [(self.label ?: @"") sizeWithAttributes:[self textAttrs]];
    return NSMakeSize(ceil(ts.width) + 16.0, ceil(ts.height) + 4.0);
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView*)controlView
{
    (void)controlView;
    NSRect r = NSInsetRect(frame, 0.5, 0.5);
    CGFloat radius = r.size.height * 0.5;
    NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:r
                                                        xRadius:radius
                                                        yRadius:radius];
    [(self.bgColor ?: [NSColor selectedControlColor]) setFill];
    [path fill];
    NSDictionary* attrs = [self textAttrs];
    NSSize ts = [(self.label ?: @"") sizeWithAttributes:attrs];
    NSPoint o = NSMakePoint(frame.origin.x + (frame.size.width - ts.width) * 0.5,
                            frame.origin.y +
                                (frame.size.height - ts.height) * 0.5);
    [(self.label ?: @"") drawAtPoint:o withAttributes:attrs];
}
@end

// Inline MSC2545 custom-emoticon pill: an atomic NSTextAttachment carrying
// the shortcode/mxc source, drawn as a bitmap by its cell. Same atomic-
// character behaviour as TKMentionAttachment (caret/backspace move over it
// as one unit).
@interface TKEmoticonAttachment : NSTextAttachment
@property(nonatomic, copy) NSString* shortcode;
@property(nonatomic, copy) NSString* mxcUrl;
@end

@implementation TKEmoticonAttachment
@end

@interface TKEmoticonCell : NSTextAttachmentCell
@end

@implementation TKEmoticonCell
- (NSSize)cellSize
{
    return self.image ? self.image.size : NSMakeSize(20, 20);
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView*)controlView
{
    (void)controlView;
    [self.image drawInRect:frame];
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
    view_.drawsBackground = NO;
    view_.richText = NO;
    view_.usesFontPanel = NO;
    view_.allowsUndo = YES;
    view_.textContainerInset = NSMakeSize(4, 6);

    scroll_.documentView = view_;
    [superview_ addSubview:scroll_];

    // Placeholder overlay — shown when text is empty and a placeholder string
    // is set. Positioned as a sibling of scroll_ above it in z-order so it
    // appears inside the text area at the first-line origin.
    placeholder_ = [NSTextField labelWithString:@""];
    // Apply FontRole::Body so both view_ and placeholder_ get the correct size
    // in one call (set_font_role sets both). placeholder_ must exist first.
    set_font_role(FontRole::Body);
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
    visible_ = visible;
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

int NSTextViewNative::cursor_byte_pos() const
{
    if (!view_)
    {
        return 0;
    }
    NSString* s = view_.string ?: @"";
    NSUInteger loc = std::min((NSUInteger)view_.selectedRange.location,
                              (NSUInteger)s.length);
    NSString* prefix = [s substringToIndex:loc];
    NSData* d = [prefix dataUsingEncoding:NSUTF8StringEncoding];
    return (int)d.length;
}

void NSTextViewNative::insert_mention(int start, int end,
                                      const std::string& user_id,
                                      const std::string& display_name,
                                      bool is_room)
{
    if (!view_)
    {
        return;
    }
    NSString* ns = view_.string;
    NSData* utf8 = [ns dataUsingEncoding:NSUTF8StringEncoding];
    int bs = std::min(start, (int)utf8.length);
    int be = std::min(end, (int)utf8.length);
    NSString* ps = [[NSString alloc]
        initWithData:[utf8 subdataWithRange:NSMakeRange(0, bs)]
            encoding:NSUTF8StringEncoding];
    NSString* pe = [[NSString alloc]
        initWithData:[utf8 subdataWithRange:NSMakeRange(0, be)]
            encoding:NSUTF8StringEncoding];
    NSRange range = NSMakeRange(ps.length, pe.length - ps.length);

    TKMentionAttachment* att = [[TKMentionAttachment alloc] init];
    att.userId = [NSString stringWithUTF8String:user_id.c_str()];
    att.displayName = [NSString stringWithUTF8String:display_name.c_str()];
    att.isRoom = is_room ? YES : NO;
    TKMentionCell* cell = [[TKMentionCell alloc] init];
    cell.label = is_room ? @"@room"
                         : [@"@" stringByAppendingString:att.displayName];
    cell.bgColor = mention_bg_;
    cell.fgColor = mention_fg_;
    att.attachmentCell = cell;

    NSMutableAttributedString* a = [[NSMutableAttributedString alloc]
        initWithAttributedString:[NSAttributedString
                                     attributedStringWithAttachment:att]];
    // Trailing space (with the view's font) so typing continues normally.
    NSDictionary* fontAttrs =
        view_.font ? @{NSFontAttributeName : view_.font} : @{};
    [a appendAttributedString:[[NSAttributedString alloc] initWithString:@" "
                                                              attributes:fontAttrs]];

    if ([view_ shouldChangeTextInRange:range replacementString:a.string])
    {
        [view_.textStorage replaceCharactersInRange:range
                               withAttributedString:a];
        NSUInteger caret = range.location + a.length;
        [view_ setSelectedRange:NSMakeRange(caret, 0)];
        [view_ didChangeText];
    }
    notify_changed();
}

void NSTextViewNative::insert_emoticon(int start, int end,
                                       const std::string& shortcode,
                                       const std::string& mxc_url,
                                       const tk::Image* image)
{
    if (!view_)
    {
        return;
    }
    if (!image)
    {
        replace_range(start, end, ":" + shortcode + ":");
        return;
    }
    NSString* ns = view_.string;
    NSData* utf8 = [ns dataUsingEncoding:NSUTF8StringEncoding];
    int bs = std::min(start, (int)utf8.length);
    int be = std::min(end, (int)utf8.length);
    NSString* ps = [[NSString alloc]
        initWithData:[utf8 subdataWithRange:NSMakeRange(0, bs)]
            encoding:NSUTF8StringEncoding];
    NSString* pe = [[NSString alloc]
        initWithData:[utf8 subdataWithRange:NSMakeRange(0, be)]
            encoding:NSUTF8StringEncoding];
    NSRange range = NSMakeRange(ps.length, pe.length - ps.length);

    constexpr CGFloat kSide = 20; // matches the reaction-chip inline image size
    CGImageRef cg = tk::cg::to_native_image(*image);
    NSImage* nsImage = [[NSImage alloc] initWithCGImage:cg
                                                    size:NSMakeSize(kSide, kSide)];

    TKEmoticonAttachment* att = [[TKEmoticonAttachment alloc] init];
    att.shortcode = [NSString stringWithUTF8String:shortcode.c_str()];
    att.mxcUrl = [NSString stringWithUTF8String:mxc_url.c_str()];
    TKEmoticonCell* cell = [[TKEmoticonCell alloc] init];
    cell.image = nsImage;
    att.attachmentCell = cell;

    NSMutableAttributedString* a = [[NSMutableAttributedString alloc]
        initWithAttributedString:[NSAttributedString
                                     attributedStringWithAttachment:att]];
    // Trailing space (with the view's font) so typing continues normally.
    NSDictionary* fontAttrs =
        view_.font ? @{NSFontAttributeName : view_.font} : @{};
    [a appendAttributedString:[[NSAttributedString alloc] initWithString:@" "
                                                              attributes:fontAttrs]];

    if ([view_ shouldChangeTextInRange:range replacementString:a.string])
    {
        [view_.textStorage replaceCharactersInRange:range
                               withAttributedString:a];
        NSUInteger caret = range.location + a.length;
        [view_ setSelectedRange:NSMakeRange(caret, 0)];
        [view_ didChangeText];
    }
    notify_changed();
}

std::vector<tesseract::MentionSeg> NSTextViewNative::composer_draft() const
{
    std::vector<tesseract::MentionSeg> segs;
    if (!view_)
    {
        return segs;
    }
    NSAttributedString* a = view_.textStorage;
    NSString* str = a.string;
    std::string pending;
    auto flush = [&]()
    {
        if (!pending.empty())
        {
            tesseract::MentionSeg s;
            s.kind = tesseract::MentionSeg::Kind::Text;
            s.text = pending;
            segs.push_back(std::move(s));
            pending.clear();
        }
    };
    NSUInteger i = 0;
    NSUInteger n = a.length;
    while (i < n)
    {
        NSRange eff;
        id att = [a attribute:NSAttachmentAttributeName
                      atIndex:i
               effectiveRange:&eff];
        if ([att isKindOfClass:[TKMentionAttachment class]])
        {
            flush();
            TKMentionAttachment* m = (TKMentionAttachment*)att;
            tesseract::MentionSeg s;
            s.kind = tesseract::MentionSeg::Kind::Mention;
            s.user_id = m.userId.UTF8String ? m.userId.UTF8String : "";
            s.display_name =
                m.displayName.UTF8String ? m.displayName.UTF8String : "";
            s.is_room = m.isRoom;
            segs.push_back(std::move(s));
        }
        else if ([att isKindOfClass:[TKEmoticonAttachment class]])
        {
            flush();
            TKEmoticonAttachment* e = (TKEmoticonAttachment*)att;
            tesseract::MentionSeg s;
            s.kind = tesseract::MentionSeg::Kind::Emoticon;
            s.shortcode = e.shortcode.UTF8String ? e.shortcode.UTF8String : "";
            s.mxc_url = e.mxcUrl.UTF8String ? e.mxcUrl.UTF8String : "";
            segs.push_back(std::move(s));
        }
        else
        {
            NSString* sub = [str substringWithRange:eff];
            if (sub.UTF8String)
            {
                pending += sub.UTF8String;
            }
        }
        i = eff.location + eff.length;
    }
    flush();
    return segs;
}

void NSTextViewNative::set_mention_colors(Color bg, Color fg)
{
    mention_bg_ = [NSColor colorWithSRGBRed:bg.r / 255.0
                                      green:bg.g / 255.0
                                       blue:bg.b / 255.0
                                      alpha:bg.a / 255.0];
    mention_fg_ = [NSColor colorWithSRGBRed:fg.r / 255.0
                                      green:fg.g / 255.0
                                       blue:fg.b / 255.0
                                      alpha:fg.a / 255.0];
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
        // Dispatch asynchronously so setNeedsDisplay:YES always runs after the
        // current drawRect: completes. Calling it synchronously from within
        // drawRect: is unreliable on macOS — the dirty flag can be swallowed
        // by the active display cycle, breaking continuous-repaint animation
        // loops (e.g. CameraWidget countdown). dispatch_async to the main
        // queue matches how ParticipantTile drives repaints: EventHandlerBase
        // posts via dispatch_get_main_queue() before calling request_repaint_.
        NSView* v = view_;
        dispatch_async(dispatch_get_main_queue(), ^{ [v setNeedsDisplay:YES]; });
    }
}

void Host::invalidate_anim_damage()
{
    if (!view_)
    {
        return;
    }
    if (anim_damage_.empty())
    {
        [view_ setNeedsDisplay:YES];
        return;
    }
    for (const auto& r : anim_damage_)
    {
        // view_ is isFlipped (top-left origin), matching canvas coords.
        // Pad by 1px so anti-aliased edges are not clipped.
        [view_ setNeedsDisplayInRect:NSMakeRect(r.x - 1.0, r.y - 1.0,
                                                r.w + 2.0, r.h + 2.0)];
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

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return tk::make_audio_capture_macos(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}

// Defined in video_macos.mm.
std::unique_ptr<tk::VideoPlayer> make_video_player_macos();

std::unique_ptr<tk::VideoPlayer> Host::make_video_player()
{
    return make_video_player_macos();
}

#ifdef TESSERACT_CALLS_ENABLED
std::unique_ptr<tk::AudioPlayback> Host::make_audio_playback()
{
    return ::tk::make_audio_playback_macos();
}
#endif

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
        NSString* srcTypeStr = src_type ? (__bridge NSString*)src_type : nil;
        if ([srcTypeStr isEqualToString:UTTypePNG.identifier])
        {
            out.mime = "image/png";
        }
        else if ([srcTypeStr isEqualToString:UTTypeJPEG.identifier])
        {
            out.mime = "image/jpeg";
        }
        else if ([srcTypeStr isEqualToString:UTTypeGIF.identifier])
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"
    CGContextRef bm = CGBitmapContextCreate(nullptr, dst_w, dst_h, 8, 0, cs,
                                            kCGImageAlphaPremultipliedFirst |
                                                kCGBitmapByteOrder32Little);
#pragma clang diagnostic pop
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
        CGImageDestinationCreateWithData(
            out_data, (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
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

void Host::set_clipboard_text(std::string_view text)
{
    NSString* str = [NSString stringWithUTF8String:std::string(text).c_str()];
    if (!str)
        return;
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str forType:NSPasteboardTypeString];
}

bool Host::set_clipboard_image(std::span<const std::uint8_t> encoded_bytes)
{
    if (encoded_bytes.empty())
        return false;
    NSData* data = [NSData dataWithBytes:encoded_bytes.data()
                                 length:encoded_bytes.size()];
    NSImage* img = [[NSImage alloc] initWithData:data];
    if (!img)
        return false;
    // writeObjects: uses NSPasteboardItem promises internally, which some apps
    // can't paste. Convert to TIFF explicitly and use the classic API instead.
    NSData* tiff = [img TIFFRepresentation];
    if (!tiff || tiff.length == 0)
        return false;
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb declareTypes:@[ NSPasteboardTypeTIFF ] owner:nil];
    return [pb setData:tiff forType:NSPasteboardTypeTIFF];
}

// ── Device enumeration ────────────────────────────────────────────────────

std::vector<tk::DeviceListing> Host::enumerate_cameras() const
{
    std::vector<tk::DeviceListing> result;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray<AVCaptureDeviceType>* types =
        @[AVCaptureDeviceTypeBuiltInWideAngleCamera,
          AVCaptureDeviceTypeExternalUnknown];
    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:types
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
#pragma clang diagnostic pop
    for (AVCaptureDevice* dev in session.devices)
    {
        tk::DeviceListing entry;
        entry.id           = dev.uniqueID.UTF8String
                                 ? std::string(dev.uniqueID.UTF8String) : "";
        entry.display_name = dev.localizedName.UTF8String
                                 ? std::string(dev.localizedName.UTF8String)
                                 : entry.id;
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<tk::DeviceListing> Host::enumerate_audio_inputs() const
{
    std::vector<tk::DeviceListing> result;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray<AVCaptureDeviceType>* types =
        @[AVCaptureDeviceTypeBuiltInMicrophone,
          AVCaptureDeviceTypeExternalUnknown];
    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:types
                                  mediaType:AVMediaTypeAudio
                                   position:AVCaptureDevicePositionUnspecified];
#pragma clang diagnostic pop
    for (AVCaptureDevice* dev in session.devices)
    {
        tk::DeviceListing entry;
        entry.id           = dev.uniqueID.UTF8String
                                 ? std::string(dev.uniqueID.UTF8String) : "";
        entry.display_name = dev.localizedName.UTF8String
                                 ? std::string(dev.localizedName.UTF8String)
                                 : entry.id;
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<tk::DeviceListing> Host::enumerate_audio_outputs() const
{
    std::vector<tk::DeviceListing> result;

    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 data_size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                       &addr, 0, nullptr, &data_size) != noErr)
        return result;

    std::vector<AudioDeviceID> device_ids(data_size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &data_size, device_ids.data()) != noErr)
        return result;

    for (AudioDeviceID dev_id : device_ids)
    {
        // Skip devices that have no output streams.
        AudioObjectPropertyAddress streams_addr{
            kAudioDevicePropertyStreams,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain};
        UInt32 streams_size = 0;
        if (AudioObjectGetPropertyDataSize(dev_id, &streams_addr, 0, nullptr,
                                           &streams_size) != noErr ||
            streams_size == 0)
            continue;

        // Retrieve the device name as a CFStringRef.
        AudioObjectPropertyAddress name_addr{
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain};
        CFStringRef name_ref = nullptr;
        UInt32 name_size = sizeof(name_ref);
        if (AudioObjectGetPropertyData(dev_id, &name_addr, 0, nullptr,
                                       &name_size, &name_ref) != noErr)
            continue;

        tk::DeviceListing entry;
        entry.id = std::to_string(dev_id);
        char name_buf[256] = {};
        if (name_ref)
        {
            CFStringGetCString(name_ref, name_buf, sizeof(name_buf),
                               kCFStringEncodingUTF8);
            CFRelease(name_ref);
        }
        entry.display_name = name_buf[0] ? name_buf : entry.id;
        result.push_back(std::move(entry));
    }
    return result;
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
    anim_damage_.clear();
    pending_popup_ = nullptr;
    PaintCtx pc{*canvas, *factory_, *theme_, this, this};
    root_->paint(pc);
    popup_ = pending_popup_;
    root_->paint_overlay(pc);
    if (view_)
    {
        NSRect b = view_.bounds;
        paint_tooltip_overlay(pc, {0, 0, static_cast<float>(b.size.width),
                                  static_cast<float>(b.size.height)});        
    }

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
    subtree_removed_during_dispatch_ = false;
    dispatch_pointer_down(
        {static_cast<float>(p.x), static_cast<float>(p.y)});
    // If a widget removed itself from the tree inside on_pointer_down (e.g.
    // CameraWidget dismissing on click), dispatch_pointer_down returns its
    // now-freed address as pressed_widget_. on_subtree_removing fired before
    // pressed_widget_ was written, so it couldn't clear it. Detect the case
    // via subtree_removed_during_dispatch_ and zero pressed_widget_ directly
    // — no on_pointer_up call, the widget is already freed.
    if (subtree_removed_during_dispatch_ && pressed_widget_)
        pressed_widget_ = nullptr;
}

void Host::on_pointer_up(NSPoint p)
{
    dispatch_pointer_up({static_cast<float>(p.x), static_cast<float>(p.y)});
}

void Host::on_pointer_move(NSPoint p)
{
    dispatch_pointer_move(
        {static_cast<float>(p.x), static_cast<float>(p.y)});
}

void Host::on_pointer_leave()
{
    dispatch_pointer_leave();
}

void Host::on_wheel(NSPoint p, CGFloat dx, CGFloat dy)
{
    fire_user_activity_();
    if (!root_)
    {
        return;
    }
    if (dispatch_wheel(
            {static_cast<float>(p.x), static_cast<float>(p.y)},
            static_cast<float>(dx), static_cast<float>(dy)))
    {
        request_repaint();
        on_pointer_move(p);
    }
}

void Host::on_right_click(NSPoint p)
{
    tk::Point pt{static_cast<float>(p.x), static_cast<float>(p.y)};
    if (root_)
        root_->dispatch_right_click(pt);
    if (on_right_click_)
        on_right_click_(pt);
}

bool Host::on_key_down(const KeyEvent& event)
{
    fire_user_activity_();
    if (popup_ && popup_->dispatch_key_down(event))
    {
        request_repaint();
        return true;
    }
    if (root_ && root_->dispatch_key_down(event))
    {
        request_repaint();
        return true;
    }
    return false;
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

bool Host::dispatch_file_drop(NSPasteboard* pb, tk::Point pos)
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
            if (on_file_drop_error_)
            {
                std::string reason =
                    err.localizedDescription.UTF8String
                        ? std::string(err.localizedDescription.UTF8String)
                        : "Could not read file";
                on_file_drop_error_(std::move(reason));
            }
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
            if (on_file_drop_error_)
            {
                std::string reason =
                    err.localizedDescription.UTF8String
                        ? std::string(err.localizedDescription.UTF8String)
                        : "Could not read file";
                on_file_drop_error_(std::move(reason));
            }
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
        on_file_drop_(std::move(bytes), std::move(mime), std::move(filename), pos);
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
        on_file_drop_(std::move(bytes), "image/png", std::string{}, pos);
        return true;
    }
    NSData* jpg = [pb dataForType:@"public.jpeg"];
    if (jpg && jpg.length > 0 && jpg.length <= kMaxDroppedImageBytes)
    {
        std::vector<std::uint8_t> bytes(
            static_cast<const std::uint8_t*>(jpg.bytes),
            static_cast<const std::uint8_t*>(jpg.bytes) + jpg.length);
        on_file_drop_(std::move(bytes), "image/jpeg", std::string{}, pos);
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

void Surface::set_anim_cache(const AnimImageCache* cache)
{
    host_->set_anim_cache(cache);
}

void Surface::update_anim_regions()
{
    host_->invalidate_anim_damage();
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

void Surface::set_on_file_drop_error(FileDropErrorHandler cb)
{
    host_->set_on_file_drop_error(std::move(cb));
}

void Surface::set_on_right_click(std::function<void(tk::Point)> cb)
{
    host_->set_on_right_click(std::move(cb));
}

} // namespace tk::macos
