#pragma once
#ifdef TESSERACT_CALLS_ENABLED

#import <Cocoa/Cocoa.h>
#include "app/CallWindowBase.h"
#include "app/ShellBase.h"

@interface CallWindowController : NSWindowController <NSWindowDelegate>
// Not the real construction path: use make_mac_call_window() from C++.
// Declared to satisfy the header interface; MacCallWindow's ctor calls
// initWithWindow: directly after creating the NSWindow.
- (instancetype)initWithShell:(tesseract::ShellBase*)shell;
- (tesseract::CallWindowBase*)callWindowBase;
@end

// C++ factory — allocates a MacCallWindow (which creates its own NSWindow and
// CallWindowController). The caller takes ownership of the returned pointer.
namespace tesseract
{
CallWindowBase* make_mac_call_window(ShellBase* shell);
} // namespace tesseract

#endif // TESSERACT_CALLS_ENABLED
