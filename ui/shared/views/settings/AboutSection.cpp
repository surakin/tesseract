#include "AboutSection.h"

#include "ServerInfoPanel.h"

#include "views/BrandView.h"
#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/widget.h"

#include <algorithm>
#include <memory>

namespace tesseract::views
{

// BrandView that either takes all the vertical space left over by the widgets
// below it (nothing else to show), or — once the server info is on screen and
// the page may scroll — a fixed height, since a fill_main child collapses to
// nothing when the content already overflows the viewport.
class AboutSection::AboutBrand : public BrandView
{
public:
    void set_fixed_height(bool fixed)
    {
        fixed_ = fixed;
        set_layout_hints({.fill_main = !fixed});
    }

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size constraints) override
    {
        if (!fixed_)
        {
            return BrandView::measure(ctx, constraints);
        }
        return {constraints.w, std::min(constraints.h, kFixedH)};
    }

private:
    static constexpr float kFixedH = 240.0f;
    bool fixed_ = false;
};

AboutSection::AboutSection()
{
    // BrandView takes all remaining vertical space above the server info.
    auto bv = std::make_unique<AboutBrand>();
    bv->set_layout_hints({.fill_main = true});
    brand_ = add_widget(std::move(bv));

    server_info_ = add_widget(std::make_unique<ServerInfoPanel>());

    // Right-aligned "Advanced" button — reveals the hidden Advanced settings
    // tab (see SettingsView::kAdvancedTabIdx).
    auto btn_row = tk::create_widget<tk::HBox>(this);
    btn_row->set_main(tk::Main::End);
    btn_row->add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Advanced"),
        [this]
        {
            if (on_advanced_clicked)
            {
                on_advanced_clicked();
            }
        },
        tk::Button::Variant::Subtle));
    advanced_row_ = add_widget(std::move(btn_row));
}

void AboutSection::set_server_info(const tesseract::ServerInfo& info)
{
    server_info_->set_server_info(info);
    brand_->set_fixed_height(!info.homeserver_url.empty());
}

void AboutSection::set_account_actions_visible(bool visible)
{
    if (advanced_row_)
    {
        advanced_row_->set_visible(visible);
    }
}

} // namespace tesseract::views
