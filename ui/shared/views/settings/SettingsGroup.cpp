#include "SettingsGroup.h"

#include "tk/theme.h"

namespace tesseract::views
{

namespace
{

// Header label visuals — match the "Theme" header AppearanceSection used to
// render manually before this base class existed.
constexpr float kHeaderH = 14.0f;
constexpr float kHeaderGap = 10.0f;
constexpr float kRowSpacing = 6.0f;

} // namespace

SettingsGroup::SettingsGroup(std::string header)
    : header_text_(std::move(header))
{
    // Reserve room for the header inside the top padding; below the header,
    // child widgets stack with a small inter-row gap.
    set_padding(tk::Edges{kHeaderH + kHeaderGap, 0.0f, 0.0f, 0.0f});
    set_spacing(kRowSpacing);
}

void SettingsGroup::paint(tk::PaintCtx& ctx)
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
    }
    tk::VBox::paint(ctx);
}

} // namespace tesseract::views
