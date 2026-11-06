#include "ProxyGroup.h"

#include "tk/i18n.h"
#include "tk/layout.h"
#include "tesseract/client.h"

namespace tesseract::views
{

using ProxyMode = tesseract::Settings::ProxyMode;

namespace
{
const char* mode_value(ProxyMode m)
{
    return m == ProxyMode::None ? "none" : m == ProxyMode::Manual ? "manual" : "system";
}
} // namespace

ProxyGroup::ProxyGroup(SettingBinder& binder) : SettingsGroup(tk::tr("Network")), binder_(&binder)
{
    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Use system proxy"), "system"},
        {tk::tr("No proxy"), "none"},
        {tk::tr("Manual"), "manual"},
    });
    combo_ = add_widget(std::move(combo));
    combo_->on_changed = [this](std::string value)
    {
        on_mode_changed_(value);
    };

    auto url_row = tk::create_widget<tk::HBox>(this);
    url_row->set_spacing(8.0f).set_cross(tk::Cross::Center);
    url_row->add_child(tk::create_widget<tk::Label>(this, tk::tr("Proxy URL:")));

    auto field = tk::create_widget<tk::TextField>(this, 32.0f);
    field->set_placeholder(tk::tr("http://proxy.example.com:8080"));
    // Typing only updates the field's state. The proxy is applied, and the
    // settings saved, once the URL is finished: on Enter or when focus leaves.
    field->set_on_changed(
        [this](const std::string&)
        {
            refresh_url_state_();
        });
    field->set_on_submit(
        [this]
        {
            commit_url_();
        });
    field->set_on_focus_changed(
        [this](bool focused)
        {
            if (!focused)
            {
                commit_url_();
            }
        });
    field->set_layout_hints({.fill_main = true});
    url_field_ = url_row->add_child(std::move(field));
    add_widget(std::move(url_row));

    auto help = tk::create_widget<tk::Label>(
        this, tk::tr("HTTP and HTTPS proxies. Calls' audio/video can't go through an HTTP proxy."),
        tk::FontRole::Small);
    add_widget(std::move(help));

    auto err =
        tk::create_widget<tk::Label>(this, tk::tr("Enter a valid http:// or https:// proxy URL."));
    error_label_ = add_widget(std::move(err));
    error_label_->set_visible(false);

    notice_ = add_widget(tk::create_widget<RestartNotice>(this));
    notice_->on_restart_requested = [this]
    {
        if (on_restart_requested)
        {
            on_restart_requested();
        }
    };

    binder.bind_custom(
        [this]
        {
            load_();
        });
    load_();
}

ProxyMode ProxyGroup::mode_() const
{
    const std::string& v = combo_->selected_value();
    return v == "none" ? ProxyMode::None : v == "manual" ? ProxyMode::Manual : ProxyMode::System;
}

void ProxyGroup::refresh_url_state_()
{
    const bool manual = mode_() == ProxyMode::Manual;
    url_field_->set_enabled(manual);
    const std::string url = url_field_->text();
    error_label_->set_visible(manual && !url.empty() &&
                              !tesseract::Client::is_valid_proxy_url(url));
}

void ProxyGroup::on_mode_changed_(const std::string&)
{
    refresh_url_state_();
    const ProxyMode m = mode_();
    if (m != ProxyMode::Manual)
    {
        commit_(m, url_field_->text());
        return;
    }
    on_url_edited_();
}

void ProxyGroup::on_url_edited_()
{
    refresh_url_state_();
    commit_url_();
}

void ProxyGroup::commit_url_()
{
    refresh_url_state_();
    if (mode_() != ProxyMode::Manual)
    {
        return;
    }
    const std::string url = url_field_->text();
    if (!tesseract::Client::is_valid_proxy_url(url))
    {
        return;
    }
    commit_(ProxyMode::Manual, url);
}

void ProxyGroup::commit_(ProxyMode mode, const std::string& url)
{
    if (loading_)
    {
        return;
    }
    auto& s = tesseract::Settings::instance();
    if (s.proxy_mode == mode && s.proxy_url == url)
    {
        return;
    }
    s.proxy_mode = mode;
    s.proxy_url = url;
    binder_->changed(SettingKey::Proxy);
}

void ProxyGroup::load_()
{
    // set_on_changed may fire on set_text; loading_ suppresses the commit.
    loading_ = true;
    const auto& s = tesseract::Settings::instance();
    combo_->set_selected_value(mode_value(s.proxy_mode));
    url_field_->set_text(s.proxy_url);
    loading_ = false;
    refresh_url_state_();
}

void ProxyGroup::set_restart_pending(bool pending)
{
    restart_pending_ = pending;
    update_notice_();
}

void ProxyGroup::set_applies_immediately(bool immediately)
{
    applies_immediately_ = immediately;
    update_notice_();
}

void ProxyGroup::update_notice_()
{
    // Signed out (applied live) there is nothing running that needs a restart,
    // so no Restart button; the note still says calls use the new proxy after
    // one. Signed in, the button shows while the setting differs from the
    // launch-time proxy.
    notice_->set_pending(restart_pending_ && !applies_immediately_);
    // Applied live (signed out): HTTP requests use the new proxy at once, but
    // call signalling still reads the launch-time one. Otherwise everything
    // waits for a restart.
    notice_->set_text(applies_immediately_
                          ? tk::tr("Applied now. Calls use the new proxy after a restart.")
                          : tk::tr("Changes take effect after restart."));
}

} // namespace tesseract::views
