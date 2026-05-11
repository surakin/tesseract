#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/client.h>

// Presented as a sheet on the main window.
// Ends with NSModalResponseOK on success, NSModalResponseCancel otherwise.
@interface LoginWindowController : NSWindowController

- (instancetype)initWithClient:(tesseract::Client*)client;

@end
