#import "MacOSTrayIcon.h"
#import <AppKit/AppKit.h>

#include <utility>

@interface TesseractTrayBridge : NSObject
@property(nonatomic, strong) NSStatusItem* statusItem;
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
