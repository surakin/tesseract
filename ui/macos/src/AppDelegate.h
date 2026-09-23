#pragma once
#import <AppKit/AppKit.h>

#ifdef __cplusplus
#include "app/Launch.h"
#endif

@interface AppDelegate : NSObject <NSApplicationDelegate>
#ifdef __cplusplus
/// The parsed command line from main() (see ui/shared/app/Launch.h). Set
/// once, before NSApplicationMain runs.
- (void)setLaunchPlan:(tesseract::LaunchPlan)plan;
#endif
@end
