#pragma once
#import <AppKit/AppKit.h>
#include <tesseract/client.h>
#include <tesseract/types.h>

// Modal sheet driving Step 6 recovery-key entry.
// Ends with NSModalResponseOK on success, NSModalResponseCancel otherwise.
@interface RecoveryWindowController : NSWindowController

- (instancetype)initWithClient:(tesseract::Client*)client;

// Forward a backup-progress update from the MainWindow event handler. Must be
// called on the main thread.
- (void)updateProgress:(const tesseract::BackupProgress&)progress;

@end
