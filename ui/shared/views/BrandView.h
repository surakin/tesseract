#pragma once

// Centered brand splash displayed in the chat area when no room is open.
// Renders the app icon (PNG decoded from the embedded brand_icon.h byte array,
// with an initials-circle fallback), the app name, and the version string,
// using only abstract canvas primitives so it works on all four backends.

#include "tk/widget.h"

#include <chrono>
#include <memory>
#include <string>

namespace tesseract::views
{

class BrandView : public tk::Widget
{
public:
    BrandView() = default;
    ~BrandView() override = default;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;

    // Set (or, with an empty string, clear) a status line drawn below the
    // version string, with a small self-animating spinner above it. Used by
    // shells to narrate startup account-restore progress on the one surface
    // visible during that window. Safe to call before/after mount; triggers
    // a relayout since the line changes the widget's stack height.
    void set_status(std::string text);

private:
    std::unique_ptr<tk::Image> icon_;
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> version_layout_;
    std::unique_ptr<tk::TextLayout> status_layout_;
    std::string status_text_;
    std::chrono::steady_clock::time_point spinner_start_{};

    // Very subtle rotating tesseract wireframe drawn behind everything else,
    // for as long as this view is on-screen (idle "no room open" splash,
    // startup splash, About section). Starts ticking from construction, not
    // lazily, since it animates unconditionally.
    std::chrono::steady_clock::time_point wireframe_start_ =
        std::chrono::steady_clock::now();
    // Guards against scheduling overlapping post_delayed timers if paint()
    // fires more often than the throttle interval for unrelated reasons.
    bool wireframe_repaint_pending_ = false;

    static constexpr float kIconDiameter = 80.0f;
    static constexpr float kIconToName = 20.0f;
    static constexpr float kNameToVer = 6.0f;
    static constexpr float kVerToSpinner = 20.0f;
    static constexpr float kSpinnerToStatus = 10.0f;
    static constexpr float kSpinnerRadius = 10.0f;
    static constexpr float kSpinnerDotR = 2.0f;

    static constexpr float kWireframeOpacity = 0.22f;
    static constexpr float kWireframeRotationPeriodMs = 24000.0f;
    static constexpr float kWireframeRadiusFactor = 0.48f;
    static constexpr int kWireframeFrameIntervalMs = 70; // ~14 fps
};

} // namespace tesseract::views
