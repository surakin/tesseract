#import <AppKit/AppKit.h>
#import "AppDelegate.h"

#include "app/Launch.h"
#include "tk/single_instance.h"

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
        tesseract::LaunchPlan plan = tesseract::prepare_launch(
            std::vector<std::string>(argv + 1, argv + argc), hooks);
        if (plan.exit_code)
        {
            return *plan.exit_code;
        }

        NSApplication* app = NSApplication.sharedApplication;
        AppDelegate* del = [[AppDelegate alloc] init];
        [del setLaunchPlan:std::move(plan)];
        app.delegate = del;
        return NSApplicationMain(argc, argv);
    }
}
