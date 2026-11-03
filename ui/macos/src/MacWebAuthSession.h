#pragma once

#import <AppKit/AppKit.h>

#include <functional>
#include <string>

#include "views/web_auth.h"

namespace tesseract::macos
{

// Presents `auth_url` in a system ASWebAuthenticationSession sheet over `anchor`
// and reports the outcome through `done` on the main thread. Returns a cancel
// function; once it has run, `done` is never invoked.
std::function<void()> start_web_auth_session(NSWindow* anchor, const std::string& auth_url,
                                             const std::string& callback_scheme,
                                             views::WebAuthDone done);

} // namespace tesseract::macos
