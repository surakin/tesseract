#include "app/ScreenshotDirector.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace tesseract::screenshot
{

std::pair<int, int> capture_content_size()
{
    int width = 0;
    int height = 0;
    const char* size = std::getenv("TESSERACT_SCREENSHOT_SIZE");
    if (size && std::sscanf(size, "%dx%d", &width, &height) == 2 && width > 0 && height > 0)
    {
        return {width, height};
    }
    return {kCaptureWidth, kCaptureHeight};
}

std::optional<bool> capture_externally(const std::string& path)
{
#ifdef _WIN32
    (void)path;
    return std::nullopt;
#else
    const char* command = std::getenv("TESSERACT_SCREENSHOT_CAPTURE");
    if (!command || !*command)
    {
        return std::nullopt;
    }
    char* argv[] = {const_cast<char*>(command), const_cast<char*>(path.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, command, nullptr, nullptr, argv, environ) != 0)
    {
        std::fprintf(stderr, "Could not run %s\n", command);
        return false;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
    {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::string screenshot_filename(std::string_view prefix, std::string_view scene,
                                ScreenshotTheme theme)
{
    std::string out{prefix};
    if (scene != "main")
    {
        out += '-';
        out += scene;
    }
    out += theme == ScreenshotTheme::Light ? "-light.png" : "-dark.png";
    return out;
}

ScreenshotDirector::ScreenshotDirector(ScreenshotHost& host, std::vector<Scene> scenes,
                                       std::string prefix, int settle_ms)
    : host_(host), scenes_(std::move(scenes)), prefix_(std::move(prefix)), settle_ms_(settle_ms)
{
}

void ScreenshotDirector::start()
{
    index_ = 0;
    step_();
}

void ScreenshotDirector::step_()
{
    const std::size_t scene = index_ / 2;
    const bool dark = index_ % 2 != 0;

    if (scene >= scenes_.size())
    {
        host_.finish(true);
        return;
    }

    if (!dark && scenes_[scene].setup)
    {
        scenes_[scene].setup();
    }

    const auto theme = dark ? ScreenshotTheme::Dark : ScreenshotTheme::Light;
    host_.apply_theme(theme);
    host_.refresh();
    host_.run_after(
        settle_ms_,
        [this, scene, dark, theme]
        {
            if (!host_.save_png(screenshot_filename(prefix_, scenes_[scene].name, theme)))
            {
                host_.finish(false);
                return;
            }
            if (dark && scenes_[scene].teardown)
            {
                scenes_[scene].teardown();
            }
            ++index_;
            step_();
        });
}

} // namespace tesseract::screenshot
