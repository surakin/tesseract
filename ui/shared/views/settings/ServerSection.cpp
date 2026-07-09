#include "ServerSection.h"

#include "SettingsGroup.h"

#include "tk/theme.h"
#include "tk/widget.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace tesseract::views
{

namespace
{
constexpr float kServerSectionRowH   = 22.0f;
constexpr float kServerSectionLabelW = 160.0f;
constexpr float kServerSectionLabelGap = 8.0f;
} // namespace

// ---------------------------------------------------------------------------
// HomeserverRow — read-only "Homeserver / <url>" line. Kept as a custom widget
// so we can use theme.text_secondary for the label (tk::Label only supports a
// concrete tk::Color, which is theme-independent).
// ---------------------------------------------------------------------------

class ServerSection::HomeserverRow : public tk::Widget
{
public:
    void set_homeserver_url(std::string url)
    {
        homeserver_url_ = std::move(url);
        label_layout_.reset();
        value_layout_.reset();
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        if (homeserver_url_.empty())
        {
            return {constraints.w, 0.0f};
        }
        return {constraints.w, kServerSectionRowH};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        if (bounds.w != bounds_.w)
        {
            label_layout_.reset();
            value_layout_.reset();
        }
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        if (homeserver_url_.empty())
        {
            return;
        }

        const auto& pal = ctx.theme.palette;

        if (!label_layout_)
        {
            tk::TextStyle ls;
            ls.role      = tk::FontRole::Body;
            ls.halign    = tk::TextHAlign::Leading;
            ls.valign    = tk::TextVAlign::Top;
            ls.trim      = tk::TextTrim::Ellipsis;
            ls.max_width = kServerSectionLabelW;
            label_layout_ = ctx.factory.build_text("Homeserver", ls);

            tk::TextStyle vs;
            vs.role      = tk::FontRole::Body;
            vs.halign    = tk::TextHAlign::Leading;
            vs.valign    = tk::TextVAlign::Top;
            vs.trim      = tk::TextTrim::Ellipsis;
            vs.max_width = std::max(0.0f, bounds_.w - kServerSectionLabelW - kServerSectionLabelGap);
            value_layout_ = ctx.factory.build_text(homeserver_url_, vs);
        }

        ctx.canvas.draw_text(*label_layout_, {bounds_.x, bounds_.y},
                             pal.text_secondary);
        ctx.canvas.draw_text(*value_layout_,
                             {bounds_.x + kServerSectionLabelW + kServerSectionLabelGap, bounds_.y},
                             pal.text_primary);
    }

private:
    std::string homeserver_url_;
    std::unique_ptr<tk::TextLayout> label_layout_;
    std::unique_ptr<tk::TextLayout> value_layout_;
};

// ---------------------------------------------------------------------------
// ServerSection — SettingsPage with one "Server" group containing the row.
// ---------------------------------------------------------------------------

ServerSection::ServerSection()
{
    group_ = add_group("Server");
    row_ = group_->add_widget(std::make_unique<HomeserverRow>());
    // Hidden until set_server_info() supplies a URL, so the page reports
    // zero height when no server info has been fetched yet.
    group_->set_visible(false);
}

ServerSection::~ServerSection() = default;

void ServerSection::set_server_info(const tesseract::ServerInfo& info)
{
    row_->set_homeserver_url(info.homeserver_url);
    group_->set_visible(!info.homeserver_url.empty());
}

tk::Size ServerSection::measure(tk::LayoutCtx& ctx, tk::Size constraints)
{
    if (!group_->visible())
    {
        return {constraints.w, 0.0f};
    }
    return SettingsPage::measure(ctx, constraints);
}

} // namespace tesseract::views
