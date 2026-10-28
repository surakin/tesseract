#import "MainWindowController.h"
#include "MainWindow.h"
#import "tk_locale.h"

#include <memory>

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
// Screenshot builds only: lets the capture window keep its exact size when
// it's taller than the screen (AppKit otherwise shrinks it to fit below the
// menu bar, and the screenshots come out short).
@interface TesseractScreenshotWindow : NSWindow
@property(nonatomic) BOOL unconstrained;
@end

@implementation TesseractScreenshotWindow
- (NSRect)constrainFrameRect:(NSRect)frameRect toScreen:(NSScreen*)screen
{
    return self.unconstrained ? frameRect
                              : [super constrainFrameRect:frameRect toScreen:screen];
}
@end

@interface MainWindowController ()
- (BOOL)_saveScreenshotToPath:(NSString*)path;
- (void)_screenshotApplyDark:(BOOL)dark;
- (void)_screenshotRefresh;
@end

namespace
{

class MacScreenshotHost final : public tesseract::screenshot::ScreenshotHost
{
public:
    MacScreenshotHost(MainWindowController* controller, NSString* directory)
        : controller_(controller), directory_(directory)
    {
    }

    void apply_theme(tesseract::screenshot::ScreenshotTheme theme) override
    {
        [controller_ _screenshotApplyDark:
                         theme == tesseract::screenshot::ScreenshotTheme::Dark];
    }
    void refresh() override { [controller_ _screenshotRefresh]; }
    bool save_png(const std::string& filename) override
    {
        NSString* path = [directory_
            stringByAppendingPathComponent:@(filename.c_str())];
        if ([controller_ _saveScreenshotToPath:path])
            return true;
        NSLog(@"Could not save screenshot: %@", path);
        return false;
    }
    void run_after(int ms, std::function<void()> fn) override
    {
        __block std::function<void()> step = std::move(fn);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                     static_cast<int64_t>(ms) * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{ step(); });
    }
    void finish(bool ok) override
    {
        if (!ok)
            std::exit(EXIT_FAILURE);
        [NSApp terminate:nil];
    }

private:
    __weak MainWindowController* controller_;
    NSString* directory_;
};

} // namespace
#endif

// The controller is the AppKit-mandated shell around tesseract::macos::MainWindow:
// NSWindowController / NSWindowDelegate,
// UNUserNotificationCenterDelegate, KVO and the public header API that
// AppDelegate calls. Every method below is a one-line forward; the logic lives
// in MainWindow (MainWindow.mm).
@implementation MainWindowController
{
    // Account manager — owns all AccountSession objects and shared media caches.
    // For the primary window this is a value (owned). For windows spawned via
    // spawn_main_window_(), _sharedAccountManager points to the primary's manager
    // and _accountManager is unused; MainWindow is constructed from the pointer.
    // Declared before _window so the window is destroyed first.
    tesseract::AccountManager  _accountManager;
    tesseract::AccountManager* _sharedAccountManager; // non-owning

    // Owns the window state and logic; holds only a weak back-pointer to us.
    std::unique_ptr<tesseract::macos::MainWindow> _window;

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    BOOL _screenshotMode;
    std::unique_ptr<tesseract::screenshot::ScreenshotHost> _screenshotHost;
#endif
}

- (instancetype)init
{
    NSRect frame = NSMakeRect(0, 0, 1100, 768);
    NSWindowStyleMask mask =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    Class window_class = TesseractScreenshotWindow.class;
#else
    Class window_class = NSWindow.class;
#endif
    NSWindow* window = [[window_class alloc] initWithContentRect:frame
                                                       styleMask:mask
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
    window.title = @"Tesseract";
    window.minSize = NSMakeSize(tesseract::visual::kMinWindowWidth,
                                 tesseract::visual::kMinWindowHeight);
    window.titlebarAppearsTransparent = NO;
    window.releasedWhenClosed = NO;

    self = [super initWithWindow:window];
    if (!self)
    {
        return nil;
    }

    tesseract::AccountManager& mgr =
        _sharedAccountManager ? *_sharedAccountManager : _accountManager;
    _window = std::make_unique<tesseract::macos::MainWindow>(mgr, self);
    window.delegate = self;
    // Loads saved settings, restores the window geometry, builds the chrome and
    // applies the loaded theme.
    _window->initialize();

    // Re-apply when the OS switches light/dark (only in System mode).
    [NSApp addObserver:self
            forKeyPath:@"effectiveAppearance"
               options:NSKeyValueObservingOptionNew
               context:nil];

    // Backing-scale and accent-colour notification observers.
    _window->observe_system_changes();

    return self;
}

- (tesseract::macos::MainWindow*)mainWindow
{
    return _window.get();
}

// Spawned-window init: shares an existing AccountManager rather than owning one.
// Sets _sharedAccountManager before calling the designated -init so that
// the MainWindow constructor receives the shared manager reference.
- (instancetype)_initWithSharedAccountManager:(tesseract::AccountManager*)mgr
{
    _sharedAccountManager = mgr;
    return [self init];
}

- (void)dealloc
{
    if (_window)
    {
        // unregister_window was already called in windowShouldClose: so we
        // do not repeat it here.  broadcast_rebuild_tray_ is still needed to
        // refresh any remaining windows' tray menus after this window is freed.
        _window->broadcast_rebuild_tray_();
        // Drain the worker pools first: the login view's pooled discovery
        // tasks hold it by raw pointer.
        _window->stop_sync();
        // Popup/settings surfaces anchor to the main app surface's host; drop
        // them while it is still alive.
        _window->release_surfaces();
    }

    [NSApp removeObserver:self forKeyPath:@"effectiveAppearance"];
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context
{
    if ([keyPath isEqualToString:@"effectiveAppearance"] && object == NSApp)
    {
        if (_window)
            _window->on_os_appearance_changed();
    }
    else
    {
        [super observeValueForKeyPath:keyPath
                             ofObject:object
                               change:change
                              context:context];
    }
}

#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
- (BOOL)_saveScreenshotToPath:(NSString*)path
{
    NSView* view = self.window.contentView;
    [view layoutSubtreeIfNeeded];
    [view displayIfNeeded];
    NSRect bounds = view.bounds;
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:bounds];
    if (!rep)
        return NO;
    [view cacheDisplayInRect:bounds toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

- (void)_screenshotApplyDark:(BOOL)dark
{
    _window->apply_screenshot_theme(dark);
}

- (void)_screenshotRefresh
{
    _window->refresh_for_screenshot();
}

- (void)captureScreenshotsToDirectory:(NSString*)directory
{
    _screenshotMode = YES;
    NSError* error = nil;
    if (![NSFileManager.defaultManager
            createDirectoryAtPath:directory
      withIntermediateDirectories:YES
                       attributes:nil
                            error:&error])
    {
        NSLog(@"Could not create screenshot directory %@: %@", directory, error);
        std::exit(EXIT_FAILURE);
    }

    if (!_window->prepare_screenshot_view())
    {
        NSLog(@"Could not load screenshot assets");
        std::exit(EXIT_FAILURE);
    }
    // 16:10, the Mac App Store's screenshot ratio: captured at the backing
    // scale this is 1440x900 (1x) or 2880x1800 (2x), both accepted sizes.
    ((TesseractScreenshotWindow*)self.window).unconstrained = YES;
    [self.window setContentSize:NSMakeSize(1440, 900)];
    [self.window makeKeyAndOrderFront:self];

    _screenshotHost = std::make_unique<MacScreenshotHost>(self, directory);
    _window->start_screenshot_director(*_screenshotHost, "mac");
}
#endif

// ─────────────────────────────────────────────────────────────────────────
//  Public header API (AppDelegate)
// ─────────────────────────────────────────────────────────────────────────

- (void)stopSync
{
    _window->stop_sync();
}

- (void)beginLogin
{
    _window->begin_login();
}

- (void)openMatrixLink:(NSString*)uri
{
    if (uri)
        _window->open_matrix_link([uri UTF8String]);
}

- (void)dispatchLaunchAction:(tesseract::LaunchAction)action
                      roomId:(NSString*)roomId
{
    _window->dispatch_launch_action(action, roomId ? [roomId UTF8String] : "");
}

- (void)navigateToUnread
{
    _window->navigate_to_unread();
}

- (void)activateSpotlightResult:(NSString*)identifier
{
    _window->activate_spotlight_result(identifier);
}

- (void)showEmojiPicker:(id)sender
{
    _window->show_emoji_picker();
}

// ─────────────────────────────────────────────────────────────────────────
//  Application-menu-bar actions
// ─────────────────────────────────────────────────────────────────────────

- (void)openSettingsMenuAction:(id)sender { _window->menu_open_settings(); }
- (void)addRoomMenuAction:(id)sender { _window->menu_add_room(); }
- (void)showRoomInfoMenuAction:(id)sender { _window->menu_show_room_info(); }
- (void)findInConversationMenuAction:(id)sender { _window->menu_find_in_conversation(); }
- (void)searchAllMessagesMenuAction:(id)sender { _window->menu_search_all_messages(); }
- (void)goBackMenuAction:(id)sender { _window->menu_go_back(); }
- (void)goForwardMenuAction:(id)sender { _window->menu_go_forward(); }
- (void)openQuickSwitcherMenuAction:(id)sender { _window->menu_open_quick_switcher(); }
- (void)cycleRecentRoomsMenuAction:(id)sender { _window->menu_cycle_recent_rooms(+1); }
- (void)cycleRecentRoomsBackwardMenuAction:(id)sender { _window->menu_cycle_recent_rooms(-1); }
- (void)showKeyboardShortcutsMenuAction:(id)sender { _window->menu_show_keyboard_shortcuts(); }

- (BOOL)validateMenuAction:(SEL)action
{
    return _window->validate_menu_action(action);
}

// ─── Cmd+C for message-list text selection ────────────────────────────────

- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (item.action == @selector(copy:))
        return _window->can_copy_selection();
    if ([self respondsToSelector:item.action] &&
        [NSStringFromSelector(item.action) hasSuffix:@"MenuAction:"])
    {
        return [self validateMenuAction:item.action];
    }
    return YES;
}

- (void)copy:(id)sender
{
    _window->copy_selection();
}

// ─────────────────────────────────────────────────────────────────────────
//  NSWindowDelegate
// ─────────────────────────────────────────────────────────────────────────

// Save main window geometry on resize or move.
- (void)_saveWindowGeometry
{
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    if (_screenshotMode) return;
#endif
    if (_window) _window->save_window_geometry();
}

- (void)windowDidEndLiveResize:(NSNotification*)notification
{
    [self _saveWindowGeometry];
}

- (void)windowDidResize:(NSNotification*)notification
{
    if (_window) _window->window_did_resize();
}

- (void)windowDidMove:(NSNotification*)notification
{
    [self _saveWindowGeometry];
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    return _window->window_should_close(sender);
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    if (_window) _window->window_did_become_key();
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    if (_window) _window->window_did_resign_key();
}

- (void)windowDidMiniaturize:(NSNotification*)notification
{
    if (_window) _window->window_did_miniaturize();
}

- (void)windowDidDeminiaturize:(NSNotification*)notification
{
    _window->window_did_deminiaturize();
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    _window->window_did_change_occlusion_state();
}

// ─────────────────────────────────────────────────────────────────────────
//  UNUserNotificationCenterDelegate
// ─────────────────────────────────────────────────────────────────────────

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler
{
    _window->will_present_notification(notification, completionHandler);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    _window->did_receive_notification_response(response, completionHandler);
}

@end
