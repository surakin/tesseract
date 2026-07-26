#include "BrandView.h"

#include "tk/host.h"
#include "tk/loading_spinner.h"
#include "tk/tesseract_wireframe.h"
#include "tk/theme.h"

#include <tesseract/version.h>

#include <algorithm>
#include <cmath>

#if TESSERACT_HAS_BRAND_ICON
#include "brand_icon.h"
#endif

namespace tesseract::views
{

tk::Size BrandView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void BrandView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    if (!name_layout_)
    {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Title;
        name_layout_ = ctx.factory.build_text(tesseract::kAppName, ts);
    }
    if (!version_layout_)
    {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Small;
        version_layout_ = ctx.factory.build_text(tesseract::kVersion, ts);
    }
    if (!status_text_.empty() && !status_layout_)
    {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Small;
        status_layout_ = ctx.factory.build_text(status_text_, ts);
    }

#if TESSERACT_HAS_BRAND_ICON
    if (!icon_)
    {
        icon_ = ctx.factory.decode_image(std::span<const std::uint8_t>(
            kBrandIconPng, sizeof(kBrandIconPng)));
    }
#endif
}

void BrandView::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    ctx.canvas.fill_rect(bounds_, pal.bg);

    {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wireframe_start_)
                .count();
        const float phase = std::fmod(static_cast<float>(elapsed_ms),
                                      kWireframeRotationPeriodMs) /
                            kWireframeRotationPeriodMs;
        const float radius =
            std::min(bounds_.w, bounds_.h) * kWireframeRadiusFactor;
        const tk::Point center{bounds_.x + bounds_.w * 0.5f,
                               bounds_.y + bounds_.h * 0.5f};

        const auto edges = tk::tesseract_wireframe_edges(phase, center, radius);
        ctx.canvas.push_opacity(kWireframeOpacity);
        for (const auto& e : edges)
        {
            ctx.canvas.draw_line(e.a, e.b, pal.text_muted, 1.0f);
        }
        ctx.canvas.pop_opacity();

        if (!wireframe_repaint_pending_ && host() && visible_in_tree())
        {
            wireframe_repaint_pending_ = true;
            host()->post_delayed(kWireframeFrameIntervalMs, [this]
            {
                wireframe_repaint_pending_ = false;
                if (host() && visible_in_tree())
                {
                    host()->request_repaint();
                }
            });
        }
    }

    if (!name_layout_ || !version_layout_)
    {
        return;
    }

    const auto name_sz = name_layout_->measure();
    const auto ver_sz = version_layout_->measure();

    // Stack height: icon + gap + name + gap + version, plus (while a startup
    // status is set) a spinner + status line below that.
    float stack_h =
        kIconDiameter + kIconToName + name_sz.h + kNameToVer + ver_sz.h;
    if (status_layout_)
    {
        stack_h += kVerToSpinner + kSpinnerRadius * 2.0f + kSpinnerToStatus +
                   status_layout_->measure().h;
    }

    // Bias the stack slightly above the geometric center — feels more balanced.
    const float stack_top = bounds_.y + (bounds_.h - stack_h) * 0.45f;
    const float cx = bounds_.x + bounds_.w * 0.5f;

    // Icon — use the decoded PNG when available; fall back to initials circle.
    const float r = kIconDiameter * 0.5f;
    if (icon_)
    {
        ctx.canvas.draw_image(
            *icon_, {cx - r, stack_top, kIconDiameter, kIconDiameter});
    }
    else
    {
        ctx.canvas.draw_initials_circle(tesseract::kAppName,
                                        {cx, stack_top + r}, kIconDiameter,
                                        pal.accent, pal.text_on_accent);
    }

    // App name ("Tesseract") in Title weight.
    const float name_top = stack_top + kIconDiameter + kIconToName;
    ctx.canvas.draw_text(*name_layout_, {cx - name_sz.w * 0.5f, name_top},
                         pal.text_primary);

    // Version number in muted style.
    const float ver_top = name_top + name_sz.h + kNameToVer;
    ctx.canvas.draw_text(*version_layout_, {cx - ver_sz.w * 0.5f, ver_top},
                         pal.text_muted);

    // Startup status: a small spinner + status line, shown only while a
    // shell has an active startup phase to narrate (see set_status()).
    if (status_layout_)
    {
        const float spinner_cy =
            ver_top + ver_sz.h + kVerToSpinner + kSpinnerRadius;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - spinner_start_)
                .count();
        const float phase = static_cast<float>(elapsed_ms % 1000) / 1000.0f;
        tk::draw_spinner_dots(ctx.canvas, {cx, spinner_cy}, phase,
                              kSpinnerRadius, kSpinnerDotR, pal.accent);

        const auto status_sz = status_layout_->measure();
        const float status_top =
            spinner_cy + kSpinnerRadius + kSpinnerToStatus;
        ctx.canvas.draw_text(*status_layout_,
                             {cx - status_sz.w * 0.5f, status_top},
                             pal.text_muted);

        if (host())
        {
            host()->request_repaint(); // self-drive the spinner
        }
    }
}

void BrandView::set_status(std::string text)
{
    if (text == status_text_)
    {
        return;
    }
    const bool was_empty = status_text_.empty();
    status_text_ = std::move(text);
    status_layout_.reset();
    if (!status_text_.empty() && was_empty)
    {
        spinner_start_ = std::chrono::steady_clock::now();
    }
    if (host())
    {
        host()->request_relayout();
    }
}

} // namespace tesseract::views
