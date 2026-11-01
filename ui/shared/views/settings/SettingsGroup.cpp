#include "SettingsGroup.h"

#include "tk/theme.h"

#include <cmath>

namespace tesseract::views
{

namespace
{

// Header label visuals — match the "Theme" header AppearanceSection used to
// render manually before this base class existed.
constexpr float kSettingsGroupHeaderH = 14.0f;
constexpr float kHeaderRuleGap = 5.0f; // title → rule
constexpr float kHeaderGap = 12.0f;    // rule → first row

// Sized for the default font; grown with the user's font config.
float header_h()
{
    return std::ceil(kSettingsGroupHeaderH * tk::font_height_scale(tk::FontRole::UiSemibold));
}
constexpr float kSettingsGroupRowSpacing = 6.0f;

} // namespace

SettingsGroup::SettingsGroup(std::string header) : header_text_(std::move(header))
{
    // Reserve room for the header inside the top padding; below the header,
    // child widgets stack with a small inter-row gap.
    update_header_padding_();
    set_spacing(kSettingsGroupRowSpacing);
}

void SettingsGroup::update_header_padding_()
{
    const float top = header_text_.empty() ? 0.0f : header_h() + kHeaderRuleGap + kHeaderGap;
    set_padding(tk::Edges{top, 0.0f, 0.0f, 0.0f});
}

void SettingsGroup::on_fonts_changed()
{
    update_header_padding_();
    tk::VBox::on_fonts_changed();
}

void SettingsGroup::paint_before_children(tk::PaintCtx& ctx)
{
    if (!header_text_.empty())
    {
        if (!header_layout_)
        {
            tk::TextStyle st;
            st.role = tk::FontRole::UiSemibold;
            st.halign = tk::TextHAlign::Leading;
            st.max_width = -1.0f;
            header_layout_ = ctx.factory.build_text(header_text_, st);
        }
        if (header_layout_)
        {
            ctx.canvas.draw_text(*header_layout_, {bounds_.x, bounds_.y},
                                 ctx.theme.palette.text_secondary);
        }
        // Rule under the title, full width of the group.
        const float rule_y = std::floor(bounds_.y + header_h() + kHeaderRuleGap) + 0.5f;
        ctx.canvas.draw_line({bounds_.x, rule_y}, {bounds_.x + bounds_.w, rule_y},
                             ctx.theme.palette.separator);
    }
}

} // namespace tesseract::views
