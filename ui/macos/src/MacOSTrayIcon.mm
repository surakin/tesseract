#import "MacOSTrayIcon.h"
#import <AppKit/AppKit.h>

#include <utility>

@interface TesseractTrayBridge : NSObject
@property(nonatomic, strong) NSStatusItem* statusItem;
// Cached base image so set_unread can composite without re-fetching
// applicationIconImage every call.
@property(nonatomic, strong) NSImage* baseImage;
@property(nonatomic, copy) void (^onShow)(void);
@property(nonatomic, copy) void (^onQuit)(void);
- (void)showApp:(id)sender;
- (void)quitApp:(id)sender;
@end

@implementation TesseractTrayBridge
- (void)showApp:(id)sender
{
    (void)sender;
    if (self.onShow)
    {
        self.onShow();
    }
}
- (void)quitApp:(id)sender
{
    (void)sender;
    if (self.onQuit)
    {
        self.onQuit();
    }
}
@end

MacOSTrayIcon::MacOSTrayIcon(std::function<void()> on_show,
                             std::function<void()> on_quit)
{
    TesseractTrayBridge* b = [[TesseractTrayBridge alloc] init];

    NSStatusItem* item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength];
    if (!item)
    {
        bridge_ = nil;
        return;
    }

    // The app icon has a solid opaque background, so template mode (which
    // uses alpha as the mask) would render a solid white square. Display it
    // as-is instead; it stays visible on both light and dark menu bars.
    NSImage* appImage = [NSApp applicationIconImage];
    if (appImage)
    {
        NSImage* trayImg = [appImage copy];
        [trayImg setSize:NSMakeSize(18, 18)];
        item.button.image = trayImg;
        b.baseImage = trayImg;
    }
    else
    {
        item.button.title = @"T";
    }
    item.button.toolTip = @"Tesseract";

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem* showItem = [menu addItemWithTitle:@"Show App"
                                           action:@selector(showApp:)
                                    keyEquivalent:@""];
    showItem.target = b;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quitItem = [menu addItemWithTitle:@"Quit"
                                           action:@selector(quitApp:)
                                    keyEquivalent:@""];
    quitItem.target = b;

    item.menu = menu;
    b.statusItem = item;
    b.onShow = ^{
        if (on_show)
        {
            on_show();
        }
    };
    b.onQuit = ^{
        if (on_quit)
        {
            on_quit();
        }
    };
    bridge_ = b;
}

MacOSTrayIcon::~MacOSTrayIcon()
{
    if (bridge_ && bridge_.statusItem)
    {
        [[NSStatusBar systemStatusBar] removeStatusItem:bridge_.statusItem];
        bridge_.statusItem = nil;
    }
    bridge_ = nil;
}

void MacOSTrayIcon::set_tooltip(const std::string& text)
{
    if (bridge_ && bridge_.statusItem)
    {
        bridge_.statusItem.button.toolTip =
            [NSString stringWithUTF8String:text.c_str()];
    }
}

void MacOSTrayIcon::set_unread(bool has_unread, bool has_highlight)
{
    if (!bridge_ || !bridge_.statusItem || !bridge_.baseImage)
    {
        return;
    }

    if (!has_unread && !has_highlight)
    {
        // Restore the plain base image. Hand back a copy so AppKit owns its
        // own ref and the cached baseImage isn't mutated by status-bar code.
        bridge_.statusItem.button.image = [bridge_.baseImage copy];
        return;
    }

    NSSize size = bridge_.baseImage.size;
    NSImage* out = [[NSImage alloc] initWithSize:size];
    [out lockFocus];
    [bridge_.baseImage drawInRect:NSMakeRect(0, 0, size.width, size.height)
                         fromRect:NSZeroRect
                        operation:NSCompositingOperationSourceOver
                         fraction:1.0];

    // Dot at ~38% of the icon edge, anchored bottom-right with a small inset.
    CGFloat side  = MIN(size.width, size.height);
    CGFloat dot   = MAX(6.0, side * 0.38);
    CGFloat inset = MAX(1.0, side / 32.0);
    NSRect dotRect = NSMakeRect(size.width - dot - inset, inset, dot, dot);

    // 0xD93636 (destructive/red) for mentions, 0x0084FF (accent/blue) for
    // plain unread. Hex values mirror the light palette in ui/shared/tk/theme.cpp.
    NSColor* fill = has_highlight
        ? [NSColor colorWithSRGBRed:0xD9/255.0
                              green:0x36/255.0
                               blue:0x36/255.0
                              alpha:1.0]
        : [NSColor colorWithSRGBRed:0x00/255.0
                              green:0x84/255.0
                               blue:0xFF/255.0
                              alpha:1.0];

    NSBezierPath* path = [NSBezierPath bezierPathWithOvalInRect:dotRect];
    [fill setFill];
    [path fill];

    // White outline so the dot stays legible on both light and dark menu bars.
    [[NSColor whiteColor] setStroke];
    path.lineWidth = MAX(1.0, side / 32.0);
    [path stroke];
    [out unlockFocus];

    // The composited image carries colour, not a monochrome mask — disable
    // template mode so macOS doesn't strip the dot's colour.
    [out setTemplate:NO];
    bridge_.statusItem.button.image = out;
}
