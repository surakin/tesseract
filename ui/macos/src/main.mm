#import <AppKit/AppKit.h>
#import "AppDelegate.h"
#import "MacRelaunchHandoff.h"

#include "app/Launch.h"
#include "app/UpdateChecker.h"
#include "tk/single_instance.h"

#include <algorithm>
#include <string>
#include <vector>

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        // Shared startup pipeline (ui/shared/app/Launch.h): parses argv,
        // selects --profile, loads settings + locale, and handles --help /
        // --version / --logoutall before AppKit starts, so those print and
        // exit without a Dock icon ever appearing. argv is already UTF-8 on
        // macOS; AppKit's own -NS…/-Apple…/-psn_ arguments are skipped by
        // the parser.
        tesseract::LaunchHooks hooks;
        hooks.detect_system_lang = []
        {
            NSString* os_lang = NSLocale.preferredLanguages.firstObject ?: @"en";
            os_lang = [os_lang stringByReplacingOccurrencesOfString:@"-"
                                                         withString:@"_"];
            return std::string([os_lang UTF8String]);
        };
        hooks.i18n_dir = []
        {
            NSString* resDir = NSBundle.mainBundle.resourcePath;
            return (resDir ? std::string([resDir UTF8String]) : std::string{}) +
                   "/i18n";
        };
        hooks.acquire_instance_lock = []
        { return tk::acquire_single_instance_lock().acquired; };
        std::vector<std::string> args(argv + 1, argv + argc);
#ifdef TESSERACT_MAC_APP_STORE
        // A sandboxed relaunch can't pass arguments; the old instance left
        // them in a handoff file instead (see MacRelaunchHandoff.h).
        auto handoff = mac::take_relaunch_handoff();
        if (std::find(args.begin(), args.end(), "--relaunch") == args.end())
            args.insert(args.end(), handoff.begin(), handoff.end());
#endif
        tesseract::LaunchPlan plan = tesseract::prepare_launch(args, hooks);
        if (plan.exit_code)
        {
            return *plan.exit_code;
        }

#ifdef TESSERACT_MAC_APP_STORE
        // The App Store delivers updates; the GitHub release check would point
        // at the DMG. Must precede any UI (Settings builds "Updates" lazily).
        tesseract::disable_update_checks();
#endif

        NSApplication* app = NSApplication.sharedApplication;
        AppDelegate* del = [[AppDelegate alloc] init];
        [del setLaunchPlan:std::move(plan)];
        app.delegate = del;
        return NSApplicationMain(argc, argv);
    }
}
