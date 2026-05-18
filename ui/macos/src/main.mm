#import <AppKit/AppKit.h>
#import "AppDelegate.h"

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        NSApplication* app = NSApplication.sharedApplication;
        AppDelegate* del = [[AppDelegate alloc] init];
        app.delegate = del;
        return NSApplicationMain(argc, argv);
    }
}
