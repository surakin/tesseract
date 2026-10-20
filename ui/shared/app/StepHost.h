#pragma once

#include <functional>

namespace tesseract
{

/// Native side of a scripted run (screenshot capture, CI smoke), implemented
/// by each shell. Both directors step through a script with these two calls.
struct StepHost
{
    virtual ~StepHost() = default;
    /// Run `fn` once on the UI thread after `ms`. The director never has
    /// more than one call pending, so a shell may back this with one timer.
    virtual void run_after(int ms, std::function<void()> fn) = 0;
    /// Called exactly once; quit the app with a matching exit status.
    virtual void finish(bool ok) = 0;
};

} // namespace tesseract
