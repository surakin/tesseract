#include "ServerSection.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kPadX   = 24.0f;
constexpr float kPadY   = 16.0f;
constexpr float kRowH   = 22.0f;
constexpr float kLabelW = 160.0f;
} // namespace

void ServerSection::set_server_info(const tesseract::ServerInfo& info)
{
    homeserver_url_ = info.homeserver_url;
    label_layout_   = nullptr;
    value_layout_   = nullptr;
}

tk::Size ServerSection::measure(tk::LayoutCtx& /*ctx*/, tk::Size constraints)
{
    if (homeserver_url_.empty())
        return {constraints.w, 0.0f};
    return {constraints.w, kPadY * 2.0f + kRowH};
}

void ServerSection::arrange(tk::LayoutCtx& /*ctx*/, tk::Rect bounds)
{
    if (bounds.w != bounds_.w)
    {
        label_layout_.reset();
        value_layout_.reset();
    }
    bounds_ = bounds;
}

void ServerSection::paint(tk::PaintCtx& ctx)
{
    if (homeserver_url_.empty())
        return;

    const auto& pal = ctx.theme.palette;

    if (!label_layout_)
    {
        tk::TextStyle ls;
        ls.role      = tk::FontRole::Body;
        ls.halign    = tk::TextHAlign::Leading;
        ls.valign    = tk::TextVAlign::Top;
        ls.trim      = tk::TextTrim::Ellipsis;
        ls.max_width = kLabelW;
        label_layout_ = ctx.factory.build_text("Homeserver", ls);

        tk::TextStyle vs;
        vs.role      = tk::FontRole::Body;
        vs.halign    = tk::TextHAlign::Leading;
        vs.valign    = tk::TextVAlign::Top;
        vs.trim      = tk::TextTrim::Ellipsis;
        vs.max_width = std::max(0.0f, bounds_.w - kPadX * 2.0f - kLabelW - 8.0f);
        value_layout_ = ctx.factory.build_text(homeserver_url_, vs);
    }

    const float y = bounds_.y + kPadY;
    ctx.canvas.draw_text(*label_layout_, {bounds_.x + kPadX, y},
                         pal.text_secondary);
    ctx.canvas.draw_text(*value_layout_,
                         {bounds_.x + kPadX + kLabelW + 8.0f, y},
                         pal.text_primary);
}

} // namespace tesseract::views
