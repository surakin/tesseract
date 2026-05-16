#include "BrandView.h"

#include "tk/theme.h"

#include <tesseract/version.h>

#if TESSERACT_HAS_BRAND_ICON
#include "brand_icon.h"
#endif

namespace tesseract::views {

tk::Size BrandView::measure(tk::LayoutCtx&, tk::Size constraints) {
    return constraints;
}

void BrandView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    if (!name_layout_) {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Title;
        name_layout_ = ctx.factory.build_text(tesseract::kAppName, ts);
    }
    if (!version_layout_) {
        tk::TextStyle ts;
        ts.role = tk::FontRole::Small;
        version_layout_ = ctx.factory.build_text(tesseract::kVersion, ts);
    }

#if TESSERACT_HAS_BRAND_ICON
    if (!icon_) {
        icon_ = ctx.factory.decode_image(
            std::span<const std::uint8_t>(kBrandIconPng, sizeof(kBrandIconPng)));
    }
#endif
}

void BrandView::paint(tk::PaintCtx& ctx) {
    const auto& pal = ctx.theme.palette;
    ctx.canvas.fill_rect(bounds_, pal.bg);

    if (!name_layout_ || !version_layout_) return;

    const auto name_sz = name_layout_->measure();
    const auto ver_sz  = version_layout_->measure();

    // Stack height: icon + gap + name + gap + version.
    const float stack_h = kIconDiameter + kIconToName
                        + name_sz.h + kNameToVer + ver_sz.h;

    // Bias the stack slightly above the geometric center — feels more balanced.
    const float stack_top = bounds_.y + (bounds_.h - stack_h) * 0.45f;
    const float cx = bounds_.x + bounds_.w * 0.5f;

    // Icon — use the decoded PNG when available; fall back to initials circle.
    const float r = kIconDiameter * 0.5f;
    if (icon_) {
        ctx.canvas.draw_image(*icon_, { cx - r, stack_top, kIconDiameter, kIconDiameter });
    } else {
        ctx.canvas.draw_initials_circle(
            tesseract::kAppName,
            { cx, stack_top + r },
            kIconDiameter,
            pal.accent,
            pal.text_on_accent);
    }

    // App name ("Tesseract") in Title weight.
    const float name_top = stack_top + kIconDiameter + kIconToName;
    ctx.canvas.draw_text(
        *name_layout_,
        { cx - name_sz.w * 0.5f, name_top },
        pal.text_primary);

    // Version number in muted style.
    const float ver_top = name_top + name_sz.h + kNameToVer;
    ctx.canvas.draw_text(
        *version_layout_,
        { cx - ver_sz.w * 0.5f, ver_top },
        pal.text_muted);
}

} // namespace tesseract::views
