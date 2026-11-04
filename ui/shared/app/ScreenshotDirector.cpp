#include "app/ScreenshotDirector.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
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

namespace
{

bool same_contents(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb)
    {
        return false;
    }
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb), std::istreambuf_iterator<char>()) &&
           fb.peek() == std::ifstream::traits_type::eof();
}

} // namespace

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
    host_.run_after(settle_ms_,
                    [this, scene, dark, theme]
                    {
                        const auto name = screenshot_filename(prefix_, scenes_[scene].name, theme);
                        if (!host_.save_png(name))
                        {
                            host_.finish(false);
                            return;
                        }
                        check_stable_(name, 1, scene, dark);
                    });
}

void ScreenshotDirector::check_stable_(std::string name, int attempt, std::size_t scene, bool dark)
{
    const auto dir = host_.output_dir();
    if (dir.empty())
    {
        advance_(scene, dark);
        return;
    }
    host_.run_after(kStableCheckMs,
                    [this, name = std::move(name), attempt, scene, dark, dir]
                    {
                        const std::string check = "check-" + name;
                        if (!host_.save_png(check))
                        {
                            host_.finish(false);
                            return;
                        }
                        const auto saved = dir / name;
                        const auto checked = dir / check;
                        std::error_code ec;
                        if (same_contents(saved, checked))
                        {
                            std::filesystem::remove(checked, ec);
                            if (attempt > 1)
                            {
                                std::fprintf(stderr, "screenshot %s settled after %d checks\n",
                                             name.c_str(), attempt);
                            }
                            advance_(scene, dark);
                            return;
                        }
                        if (attempt >= kMaxStableChecks)
                        {
                            std::fprintf(stderr, "screenshot %s still changing after %d checks\n",
                                         name.c_str(), attempt);
                            std::filesystem::remove(checked, ec);
                            host_.finish(false);
                            return;
                        }
                        // Keep the newer frame as the one the next check compares against.
                        std::filesystem::rename(checked, saved, ec);
                        if (ec)
                        {
                            std::fprintf(stderr,
                                         "screenshot %s: could not replace the saved frame: %s\n",
                                         name.c_str(), ec.message().c_str());
                            host_.finish(false);
                            return;
                        }
                        check_stable_(name, attempt + 1, scene, dark);
                    });
}

void ScreenshotDirector::advance_(std::size_t scene, bool dark)
{
    if (dark && scenes_[scene].teardown)
    {
        scenes_[scene].teardown();
    }
    ++index_;
    step_();
}

} // namespace tesseract::screenshot
