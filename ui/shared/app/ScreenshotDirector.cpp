#include "app/ScreenshotDirector.h"

#include <utility>

namespace tesseract::screenshot
{

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
