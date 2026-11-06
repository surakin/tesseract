#include "ServerInfoPanel.h"

#include "SettingsGroup.h"

#include "tk/i18n.h"
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
constexpr float kServerInfoRowH = 22.0f;
constexpr float kServerInfoLabelW = 160.0f;
constexpr float kServerInfoLabelGap = 8.0f;

std::string spec_version_text(const tesseract::SpecVersion& v)
{
    return "v" + std::to_string(v.major) + "." + std::to_string(v.minor);
}

// Stable / Experimental / Not supported for a feature the server advertises.
std::string feature_status(bool supported, bool stable)
{
    if (!supported)
    {
        return tk::tr("Not supported");
    }
    return stable ? tk::tr("Stable") : tk::tr("Experimental");
}

std::string login_flow_label(const std::string& flow)
{
    if (flow == "password")
    {
        return tk::tr("Password");
    }
    if (flow == "sso")
    {
        return tk::tr("Single sign-on");
    }
    if (flow == "token")
    {
        return tk::tr("Token");
    }
    return {}; // not a method Tesseract knows about — not shown
}
} // namespace

// ---------------------------------------------------------------------------
// build_server_info_rows
// ---------------------------------------------------------------------------

ServerInfoRows build_server_info_rows(const tesseract::ServerInfo& info)
{
    ServerInfoRows out;

    // --- Server ---------------------------------------------------------
    out.server.push_back({tk::tr("Homeserver"), info.homeserver_url, false});

    if (!info.server_name.empty())
    {
        std::string software = info.server_name;
        if (!info.server_version.empty())
        {
            software += " " + info.server_version;
        }
        out.server.push_back({tk::tr("Software"), std::move(software), false});
    }

    if (!info.spec_versions.empty())
    {
        const auto less = [](const tesseract::SpecVersion& a, const tesseract::SpecVersion& b)
        {
            return a.major != b.major ? a.major < b.major : a.minor < b.minor;
        };
        const auto [lo, hi] =
            std::minmax_element(info.spec_versions.begin(), info.spec_versions.end(), less);
        std::string range = spec_version_text(*lo);
        if (less(*lo, *hi))
        {
            range += " – " + spec_version_text(*hi);
        }
        out.server.push_back({tk::tr("Matrix versions"), std::move(range), false});
    }

    if (info.upload_size_limit > 0)
    {
        out.server.push_back(
            {tk::tr("Maximum upload size"), tk::format_size(info.upload_size_limit), false});
    }

    std::string methods;
    for (const auto& flow : info.login_flows)
    {
        const std::string label = login_flow_label(flow);
        if (label.empty())
        {
            continue;
        }
        if (!methods.empty())
        {
            methods += ", ";
        }
        methods += label;
    }
    if (!methods.empty())
    {
        out.server.push_back({tk::tr("Login methods"), std::move(methods), false});
    }

    // --- Features -------------------------------------------------------
    const auto add_feature = [&out](std::string label, bool supported, bool stable)
    {
        out.features.push_back({std::move(label), feature_status(supported, stable), !supported});
    };

    const bool spec_1_6 = std::any_of(info.spec_versions.begin(), info.spec_versions.end(),
                                      [](const auto& v)
                                      {
                                          return v.at_least(1, 6);
                                      });
    add_feature(tk::tr("Jump to date"), info.supports_msc3030, spec_1_6);

    if (info.supports_profile_fields && !info.profile_fields_enabled)
    {
        out.features.push_back({tk::tr("Profile fields"), tk::tr("Disabled by server"), true});
    }
    else
    {
        add_feature(tk::tr("Profile fields"), info.supports_profile_fields,
                    info.profile_fields_stable);
    }

    add_feature(tk::tr("QR code login"), info.supports_qr_grant, false);
    add_feature(tk::tr("Invite reasons"), info.supports_invite_reason, false);
    add_feature(tk::tr("Voice and video calls"), info.supports_calls, false);
    out.features.push_back(
        {tk::tr("URL previews"),
         info.preview_url_enabled ? tk::tr("Enabled") : tk::tr("Disabled by server"),
         !info.preview_url_enabled});

    // --- Capabilities ---------------------------------------------------
    const auto add_capability = [&out](std::string label, bool allowed)
    {
        out.capabilities.push_back(
            {std::move(label), allowed ? tk::tr("Allowed") : tk::tr("Not allowed"), !allowed});
    };
    add_capability(tk::tr("Change password"), info.can_change_password);
    add_capability(tk::tr("Change display name"), info.can_set_displayname);
    add_capability(tk::tr("Change avatar"), info.can_set_avatar);
    if (!info.default_room_version.empty())
    {
        out.capabilities.push_back(
            {tk::tr("Default room version"), info.default_room_version, false});
    }

    return out;
}

// ---------------------------------------------------------------------------
// InfoRow — read-only "<label> / <value>" line. Kept as a custom widget so we
// can use theme.text_secondary for the label (tk::Label only supports a
// concrete tk::Color, which is theme-independent).
// ---------------------------------------------------------------------------

class ServerInfoPanel::InfoRow : public tk::Widget
{
public:
    explicit InfoRow(ServerInfoRow row) : row_(std::move(row))
    {
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {constraints.w, kServerInfoRowH};
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

    void on_fonts_changed() override
    {
        label_layout_.reset();
        value_layout_.reset();
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;

        if (!label_layout_)
        {
            tk::TextStyle ls;
            ls.role = tk::FontRole::Body;
            ls.halign = tk::TextHAlign::Leading;
            ls.valign = tk::TextVAlign::Top;
            ls.trim = tk::TextTrim::Ellipsis;
            ls.max_width = kServerInfoLabelW;
            label_layout_ = ctx.factory.build_text(row_.label, ls);

            tk::TextStyle vs;
            vs.role = tk::FontRole::Body;
            vs.halign = tk::TextHAlign::Leading;
            vs.valign = tk::TextVAlign::Top;
            vs.trim = tk::TextTrim::Ellipsis;
            vs.max_width = std::max(0.0f, bounds_.w - kServerInfoLabelW - kServerInfoLabelGap);
            value_layout_ = ctx.factory.build_text(row_.value, vs);
        }

        ctx.canvas.draw_text(*label_layout_, {bounds_.x, bounds_.y}, pal.text_secondary);
        ctx.canvas.draw_text(*value_layout_,
                             {bounds_.x + kServerInfoLabelW + kServerInfoLabelGap, bounds_.y},
                             row_.dim ? pal.text_secondary : pal.text_primary);
    }

private:
    ServerInfoRow row_;
    tk::CachedTextLayout label_layout_;
    tk::CachedTextLayout value_layout_;
};

// ---------------------------------------------------------------------------
// ServerInfoPanel — VBox with Server / Features / Capabilities groups.
// ---------------------------------------------------------------------------

ServerInfoPanel::ServerInfoPanel()
{
    // Same inter-group gap as SettingsPage.
    set_spacing(20.0f);
    server_group_ = add_child(std::make_unique<SettingsGroup>(tk::tr("Server")));
    features_group_ = add_child(std::make_unique<SettingsGroup>(tk::tr("Features")));
    capabilities_group_ = add_child(std::make_unique<SettingsGroup>(tk::tr("Capabilities")));
    // Hidden until set_server_info() supplies a URL, so the panel reports
    // zero height when no server info has been fetched yet.
    server_group_->set_visible(false);
    features_group_->set_visible(false);
    capabilities_group_->set_visible(false);
}

ServerInfoPanel::~ServerInfoPanel() = default;

void ServerInfoPanel::fill_group_(SettingsGroup* group, const std::vector<ServerInfoRow>& rows)
{
    group->clear_children();
    for (const auto& r : rows)
    {
        group->add_widget(std::make_unique<InfoRow>(r));
    }
}

void ServerInfoPanel::set_server_info(const tesseract::ServerInfo& info)
{
    const bool have_info = !info.homeserver_url.empty();
    if (have_info)
    {
        const ServerInfoRows rows = build_server_info_rows(info);
        fill_group_(server_group_, rows.server);
        fill_group_(features_group_, rows.features);
        fill_group_(capabilities_group_, rows.capabilities);
    }
    server_group_->set_visible(have_info);
    features_group_->set_visible(have_info);
    capabilities_group_->set_visible(have_info);
}

} // namespace tesseract::views
