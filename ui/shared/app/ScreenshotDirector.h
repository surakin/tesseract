#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::screenshot
{

enum class ScreenshotTheme
{
    Light,
    Dark,
};

/// Native side of screenshot capture, implemented by each shell.
struct ScreenshotHost
{
    virtual ~ScreenshotHost() = default;
    virtual void apply_theme(ScreenshotTheme theme) = 0;
    /// Relayout + schedule a repaint after a scene or theme change.
    virtual void refresh() = 0;
    virtual bool save_png(const std::string& filename) = 0;
    /// Run `fn` once on the UI thread after `ms`. The director never has
    /// more than one call pending, so a shell may back this with one timer.
    virtual void run_after(int ms, std::function<void()> fn) = 0;
    /// Called exactly once; quit the app with a matching exit status.
    virtual void finish(bool ok) = 0;
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
