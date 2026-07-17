#import "CallWindowController.h"

#include "app/ShellBase.h"
#include "tk/host_macos.h"
#include "views/CallOverlayWidget.h"

#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration
// ─────────────────────────────────────────────────────────────────────────────

class MacCallWindow;

@interface CallWindowController ()
@property(nonatomic, assign) MacCallWindow* cppWindow;
@end

// ─────────────────────────────────────────────────────────────────────────────
// MacCallWindow — C++ CallWindowBase subclass for macOS pop-out call windows
// ─────────────────────────────────────────────────────────────────────────────

class MacCallWindow : public tesseract::CallWindowBase
{
public:
    explicit MacCallWindow(tesseract::ShellBase* shell);
    ~MacCallWindow() override;

    void bring_to_front()               override;
    void close_window()                 override;
    void apply_theme(const tk::Theme&)  override;
    void request_relayout()             override;
    void request_repaint()              override;

    // Called by -windowWillClose: delegate method.
    void on_window_will_close()
    {
        window_closed_ = true;
        if (on_window_closed)
            on_window_closed();
    }

private:
    __strong CallWindowController* controller_ = nil;
    std::unique_ptr<tk::macos::Surface> surface_;
    bool window_closed_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// MacCallWindow implementation
// ─────────────────────────────────────────────────────────────────────────────

MacCallWindow::MacCallWindow(tesseract::ShellBase* shell)
    : tesseract::CallWindowBase(shell)
{
    NSRect frame = NSMakeRect(0, 0, 640, 480);
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win setTitle:@"Call"];
    [win center];

    surface_ = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    NSView* surfaceView = (__bridge NSView*)surface_->view_handle();
    [win setContentView:surfaceView];

    auto overlay = std::make_unique<tesseract::views::CallOverlayWidget>();
    call_overlay_widget_ = overlay.get();
    surface_->set_root(std::move(overlay));

    controller_ = [[CallWindowController alloc] initWithWindow:win];
    controller_.cppWindow = this;
    [win setDelegate:controller_];

    [win makeKeyAndOrderFront:nil];
}

MacCallWindow::~MacCallWindow()
{
    close_window();
}

void MacCallWindow::bring_to_front()
{
    if (controller_ && controller_.window)
        [controller_.window makeKeyAndOrderFront:nil];
}

void MacCallWindow::close_window()
{
    if (!window_closed_ && controller_ && controller_.window)
    {
        window_closed_ = true;
        [controller_.window close];
    }
}

void MacCallWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
}

void MacCallWindow::request_relayout()
{
    if (surface_)
        surface_->relayout();
}

void MacCallWindow::request_repaint()
{
    if (surface_)
        surface_->host().request_repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// CallWindowController (ObjC++) implementation
// ─────────────────────────────────────────────────────────────────────────────

@implementation CallWindowController

- (instancetype)initWithShell:(tesseract::ShellBase*)shell
{
    // Not the real construction path — MacCallWindow's ctor creates the
    // NSWindow and then calls [CallWindowController initWithWindow:]. This
    // initialiser is declared for the header interface but should not be
    // invoked directly; use make_mac_call_window() instead.
    (void)shell;
    return nil;
}

- (tesseract::CallWindowBase*)callWindowBase
{
    return self.cppWindow;
}

// NSWindowDelegate
- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (self.cppWindow)
        self.cppWindow->on_window_will_close();
}

@end

// ─────────────────────────────────────────────────────────────────────────────
// C++ factory — called by the macOS shell to open a call pop-out window.
// ─────────────────────────────────────────────────────────────────────────────

namespace tesseract
{

CallWindowBase* make_mac_call_window(ShellBase* shell)
{
    return new MacCallWindow(shell);
}

} // namespace tesseract

