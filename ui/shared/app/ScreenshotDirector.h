#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/StepHost.h"

namespace tesseract::screenshot
{

/// Every capture is the whole window, title bar and frame included, at this
/// size (pixels at 1x, points on macOS). 16:10 is the Mac App Store's
/// screenshot ratio.
inline constexpr int kCaptureWidth = 1440;
inline constexpr int kCaptureHeight = 900;

/// The size a shell gives its window's content: TESSERACT_SCREENSHOT_SIZE
/// ("WxH") when set, else the capture size. CI sets it when a compositor
/// draws the frame, so frame plus content come out at the capture size.
std::pair<int, int> capture_content_size();

/// When TESSERACT_SCREENSHOT_CAPTURE names a command, runs it with the output
/// path as its only argument and returns whether it succeeded; returns
/// nullopt when the variable is unset (or on Windows). Lets CI capture the
/// window with the compositor's frame, which the app can't render itself.
std::optional<bool> capture_externally(const std::string& path);

enum class ScreenshotTheme
{
    Light,
    Dark,
};

/// Native side of screenshot capture, implemented by each shell.
struct ScreenshotHost : StepHost
{
    virtual void apply_theme(ScreenshotTheme theme) = 0;
    /// Relayout + schedule a repaint after a scene or theme change.
    virtual void refresh() = 0;
    virtual bool save_png(const std::string& filename) = 0;
};

struct Scene
{
    std::string name;
    std::function<void()> setup;
    std::function<void()> teardown;
};

/// "main" keeps the pre-scene names (`qt6-light.png`) so README links hold.
std::string screenshot_filename(std::string_view prefix, std::string_view scene,
                                ScreenshotTheme theme);

/// Steps scenes × {light, dark}: setup, then per theme apply/refresh/wait/
/// save, then teardown. Stops at the first failed save.
class ScreenshotDirector
{
public:
    ScreenshotDirector(ScreenshotHost& host, std::vector<Scene> scenes, std::string prefix,
                       int settle_ms = 300);

    void start();

private:
    void step_();

    ScreenshotHost& host_;
    std::vector<Scene> scenes_;
    std::string prefix_;
    int settle_ms_;
    std::size_t index_ = 0; // capture index: scene = index_/2, theme = index_%2
};

} // namespace tesseract::screenshot
