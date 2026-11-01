#include "NetworkSection.h"

#include "SettingsGroup.h"
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

NetworkSection::NetworkSection(SettingBinder& binder) : binder_(&binder)
{
    auto* group = add_group(tk::tr("Proxy"));

    auto combo = tk::create_widget<tk::ComboBox>(this);
    combo->set_options({
        {tk::tr("Use system proxy"), "system"},
        {tk::tr("No proxy"), "none"},
        {tk::tr("Manual"), "manual"},
    });
    combo_ = group->add_widget(std::move(combo));
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
    group->add_widget(std::move(url_row));

    auto help = tk::create_widget<tk::Label>(
        this, tk::tr("HTTP and HTTPS proxies. Calls' audio/video can't go through an HTTP proxy."),
        tk::FontRole::Small);
    group->add_widget(std::move(help));

    auto err =
        tk::create_widget<tk::Label>(this, tk::tr("Enter a valid http:// or https:// proxy URL."));
    error_label_ = group->add_widget(std::move(err));
    error_label_->set_visible(false);

    auto note = tk::create_widget<tk::Label>(this, tk::tr("Changes take effect after restart."),
                                             tk::FontRole::Small);
    restart_note_ = group->add_widget(std::move(note));

    // HBox so the button keeps its natural width instead of stretching.
    auto row = tk::create_widget<tk::HBox>(this);
    row->add_child(tk::create_widget<tk::Button>(this, tk::tr("Restart now"),
                                                 [this]
                                                 {
                                                     if (on_restart_requested)
                                                     {
                                                         on_restart_requested();
                                                     }
                                                 }));
    row->set_visible(false);
    restart_row_ = group->add_widget(std::move(row));

    binder.bind_custom(
        [this]
        {
            load_();
        });
    load_();
}

ProxyMode NetworkSection::mode_() const
{
    const std::string& v = combo_->selected_value();
    return v == "none" ? ProxyMode::None : v == "manual" ? ProxyMode::Manual : ProxyMode::System;
}

void NetworkSection::refresh_url_state_()
{
    const bool manual = mode_() == ProxyMode::Manual;
    url_field_->set_enabled(manual);
    const std::string url = url_field_->text();
    error_label_->set_visible(manual && !url.empty() &&
                              !tesseract::Client::is_valid_proxy_url(url));
}

void NetworkSection::on_mode_changed_(const std::string&)
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

void NetworkSection::on_url_edited_()
{
    refresh_url_state_();
    commit_url_();
}

void NetworkSection::commit_url_()
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

void NetworkSection::commit_(ProxyMode mode, const std::string& url)
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

void NetworkSection::load_()
{
    // set_on_changed may fire on set_text; loading_ suppresses the commit.
    loading_ = true;
    const auto& s = tesseract::Settings::instance();
    combo_->set_selected_value(mode_value(s.proxy_mode));
    url_field_->set_text(s.proxy_url);
    loading_ = false;
    refresh_url_state_();
}

void NetworkSection::set_restart_pending(bool pending)
{
    restart_pending_ = pending;
    update_restart_row_();
    update_restart_note_();
}

void NetworkSection::set_applies_immediately(bool immediately)
{
    applies_immediately_ = immediately;
    update_restart_row_();
    update_restart_note_();
}

void NetworkSection::update_restart_row_()
{
    // Signed out (applied live) there is nothing running that needs a restart,
    // so no Restart button; the note still says calls use the new proxy after
    // one. Signed in, the button shows while the setting differs from the
    // launch-time proxy.
    if (restart_row_)
    {
        restart_row_->set_visible(restart_pending_ && !applies_immediately_);
    }
}

void NetworkSection::update_restart_note_()
{
    // Applied live (signed out): HTTP requests use the new proxy at once, but
    // call signalling still reads the launch-time one. Otherwise everything
    // waits for a restart.
    if (restart_note_)
    {
        restart_note_->set_text(
            applies_immediately_ ? tk::tr("Applied now. Calls use the new proxy after a restart.")
                                 : tk::tr("Changes take effect after restart."));
    }
}

} // namespace tesseract::views
