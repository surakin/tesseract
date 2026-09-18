#import <Cocoa/Cocoa.h>

#import "AuxWindowController.h"

#include "tk/host_macos.h"

#include <memory>

class MacAuxWindow;

@interface AuxWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) MacAuxWindow* cppWindow;
@end

// ─────────────────────────────────────────────────────────────────────────────
// MacAuxWindow — generic secondary top-level window hosting one tk widget tree
// ─────────────────────────────────────────────────────────────────────────────

class MacAuxWindow : public tesseract::AuxWindowBase
{
public:
    MacAuxWindow(const std::string& title, std::unique_ptr<tk::Widget> root,
                 int width, int height, const tk::Theme& theme);
    ~MacAuxWindow() override;

    void bring_to_front()                override;
    void close_window()                  override;
    void apply_theme(const tk::Theme&)   override;
    void apply_scale_change(float scale) override;
    void request_relayout()              override;
    void request_repaint()               override;

    // Called by -windowWillClose: delegate method.
    void on_window_will_close()
    {
        window_closed_ = true;
        if (on_window_closed)
            on_window_closed();
    }

private:
    __strong NSWindow*         window_   = nil;
    __strong AuxWindowDelegate* delegate_ = nil;
    std::unique_ptr<tk::macos::Surface> surface_;
    bool window_closed_ = false;
};

MacAuxWindow::MacAuxWindow(const std::string& title, std::unique_ptr<tk::Widget> root,
                           int width, int height, const tk::Theme& theme)
{
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    window_ = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    // ARC owns the window via window_; closing must not also release it.
    window_.releasedWhenClosed = NO;
    [window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
    [window_ center];

    surface_ = std::make_unique<tk::macos::Surface>(theme);
    NSView* surfaceView = (__bridge NSView*)surface_->view_handle();
    [window_ setContentView:surfaceView];
    surface_->set_root(std::move(root));

    delegate_ = [[AuxWindowDelegate alloc] init];
    delegate_.cppWindow = this;
    [window_ setDelegate:delegate_];

    // Re-rasterize native-control image captures when this window's own
    // backing scale factor changes (same as MacCallWindow).
    [[NSNotificationCenter defaultCenter]
        addObserver:delegate_
           selector:@selector(_auxWindowDidChangeBackingProperties:)
               name:NSWindowDidChangeBackingPropertiesNotification
             object:window_];

    [window_ makeKeyAndOrderFront:nil];
}

MacAuxWindow::~MacAuxWindow()
{
    close_window();
    delegate_.cppWindow = nullptr;
    [window_ setDelegate:nil];
}

void MacAuxWindow::bring_to_front()
{
    if (window_)
        [window_ makeKeyAndOrderFront:nil];
}

void MacAuxWindow::close_window()
{
    if (!window_closed_ && window_)
    {
        window_closed_ = true;
        [window_ close];
    }
}

void MacAuxWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
}

void MacAuxWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

void MacAuxWindow::request_relayout()
{
    if (surface_)
        surface_->relayout();
}

void MacAuxWindow::request_repaint()
{
    if (surface_)
        surface_->host().request_repaint();
}

@implementation AuxWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (self.cppWindow)
        self.cppWindow->on_window_will_close();
}

- (void)_auxWindowDidChangeBackingProperties:(NSNotification*)note
{
    (void)note;
    if (self.cppWindow)
    {
        NSWindow* w = (NSWindow*)note.object;
        self.cppWindow->apply_scale_change((float)w.backingScaleFactor);
    }
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

@end

namespace tesseract
{

AuxWindowBase* make_mac_aux_window(const std::string& title,
                                   std::unique_ptr<tk::Widget> root,
                                   int width, int height, const tk::Theme& theme)
{
    return new MacAuxWindow(title, std::move(root), width, height, theme);
}

} // namespace tesseract
