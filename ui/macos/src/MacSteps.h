#pragma once

// Timer + exit plumbing shared by the screenshot and smoke runs. Included by
// the .mm files that build a StepHost; empty unless one of the modes is on.
#if defined(TESSERACT_SCREENSHOT_MODE_ENABLED) || defined(TESSERACT_SMOKE_MODE_ENABLED)

#import <Cocoa/Cocoa.h>

#include "app/StepHost.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <utility>

namespace tesseract::macos
{

/// `Base` is StepHost itself (smoke) or a host interface derived from it
/// (screenshots).
template <class Base>
class MacSteps : public Base
{
public:
    void run_after(int ms, std::function<void()> fn) override
    {
        __block std::function<void()> step = std::move(fn);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ms) * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
                           step();
                       });
    }
    void finish(bool ok) override
    {
        // A failed run must still fail the CI step.
        if (!ok)
        {
            std::exit(EXIT_FAILURE);
        }
        [NSApp terminate:nil];
    }
};

} // namespace tesseract::macos

#endif
