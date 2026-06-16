#include "SettingsWidget.h"
#include "tk/theme.h"

#include <tesseract/paths.h>
#include <tesseract/settings.h>

#include <libintl.h>
#define _(s) gettext(s)

namespace
{

// Minimal JSON string escaper: produces a JSON-quoted string literal.
// Only escapes backslash and double-quote, which is sufficient for all
// profile field values (IANA tz IDs, pronoun summaries, bio plain text).
std::string json_quote(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s)
    {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
}

} // namespace

namespace gtk4
{

SettingsWidget::SettingsWidget()
    : surface_(std::make_unique<tk::gtk4::Surface>(tk::Theme::light()))
{
    auto view = std::make_unique<tesseract::views::SettingsView>();
    settings_view_ = view.get();

    settings_view_->on_close = [this]
    {
        if (on_close)
        {
            on_close();
        }
    };
    settings_view_->on_logout = [this]
    {
        if (on_logout)
        {
            on_logout();
        }
    };
    settings_view_->on_theme_changed = [this](auto p)
    {
        if (on_theme_changed)
        {
            on_theme_changed(p);
        }
    };
    settings_view_->on_notifications_changed = [this](bool e)
    {
        if (on_notifications_changed)
        {
            on_notifications_changed(e);
        }
    };
    settings_view_->on_send_presence_changed = [this](bool e)
    {
        if (on_send_presence_changed)
            on_send_presence_changed(e);
    };
    settings_view_->on_index_messages_changed = [this](bool e)
    {
        if (on_index_messages_changed)
            on_index_messages_changed(e);
    };
#ifdef TESSERACT_GITHUB_REPO
    settings_view_->on_check_for_updates_changed = [this](bool e)
    {
        if (on_check_for_updates_changed)
            on_check_for_updates_changed(e);
    };
#endif
    settings_view_->on_media_previews_changed =
        [this](tesseract::Settings::MediaPreviews mode)
    {
        if (on_media_previews_changed)
            on_media_previews_changed(mode);
    };
    settings_view_->on_invite_avatars_changed = [this](bool e)
    {
        if (on_invite_avatars_changed)
            on_invite_avatars_changed(e);
    };
    settings_view_->on_group_inactive_changed = [this](bool v)
    {
        if (on_group_inactive_changed)
            on_group_inactive_changed(v);
    };
    settings_view_->on_inactive_period_changed = [this](int days)
    {
        if (on_inactive_period_changed)
            on_inactive_period_changed(days);
    };
    settings_view_->on_autoscroll_unread_changed = [this](bool v)
    {
        if (on_autoscroll_unread_changed)
            on_autoscroll_unread_changed(v);
    };
    // Persisted directly here (self-contained — no extra wrapper/MainWindow
    // plumbing); the lock-screen privacy gate is always on regardless.
    settings_view_->on_hide_content_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_hide_content = e;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_image_previews_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.notification_image_previews = e;
        s.save_to_disk(tesseract::config_dir());
    };
    settings_view_->on_prefetch_changed = [](bool e)
    {
        auto& s = tesseract::Settings::instance();
        s.prefetch_full_media = e;
        s.save_to_disk(tesseract::config_dir());
    };

    settings_view_->on_tab_changed = [this] { surface_->relayout(); };

    settings_view_->on_clear_caches = [this]
    {
        if (on_clear_caches) on_clear_caches();
    };

    settings_view_->on_reset_identity = [this]
    {
        if (on_reset_identity) on_reset_identity();
    };

    settings_view_->on_show_tooltip =
        [this](std::string text, tk::Rect anchor)
    {
        GtkWidget* w = surface_->widget();
        if (!cache_tooltip_popover_)
        {
            cache_tooltip_label_ = gtk_label_new(nullptr);
            gtk_label_set_wrap(GTK_LABEL(cache_tooltip_label_), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(cache_tooltip_label_), 80);
            cache_tooltip_popover_ = gtk_popover_new();
            gtk_widget_add_css_class(cache_tooltip_popover_, "tooltip");
            gtk_popover_set_child(GTK_POPOVER(cache_tooltip_popover_),
                                  cache_tooltip_label_);
            gtk_widget_set_parent(cache_tooltip_popover_, w);
            gtk_popover_set_autohide(GTK_POPOVER(cache_tooltip_popover_),
                                     FALSE);
            gtk_popover_set_has_arrow(GTK_POPOVER(cache_tooltip_popover_),
                                      FALSE);
        }
        gtk_label_set_text(GTK_LABEL(cache_tooltip_label_), text.c_str());
        GdkRectangle rect{
            static_cast<int>(anchor.x), static_cast<int>(anchor.y),
            static_cast<int>(anchor.w), static_cast<int>(anchor.h)};
        gtk_popover_set_pointing_to(GTK_POPOVER(cache_tooltip_popover_), &rect);
        gtk_popover_popup(GTK_POPOVER(cache_tooltip_popover_));
    };
    settings_view_->on_hide_tooltip = [this]
    {
        if (cache_tooltip_popover_)
            gtk_popover_popdown(GTK_POPOVER(cache_tooltip_popover_));
    };

    surface_->set_root(std::move(view));

    surface_->set_on_layout(
        [this]
        {
            if (name_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->name_field_rect();
                name_field_->set_visible(!r.empty());
                if (!r.empty())
                    name_field_->set_rect(r);
            }
            if (pronouns_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->pronouns_field_rect();
                pronouns_field_->set_visible(!r.empty());
                if (!r.empty())
                    pronouns_field_->set_rect(r);
            }
            if (tz_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->tz_field_rect();
                tz_field_->set_visible(!r.empty());
                if (!r.empty())
                    tz_field_->set_rect(r);
            }
            if (bio_field_ && settings_view_)
            {
                const tk::Rect r = settings_view_->bio_field_rect();
                bio_field_->set_visible(!r.empty());
                if (!r.empty())
                    bio_field_->set_rect(r);
            }
        });
}

GtkWidget* SettingsWidget::widget() const
{
    return surface_->widget();
}

void SettingsWidget::set_server_info(const tesseract::ServerInfo& info)
{
    if (settings_view_)
        settings_view_->set_server_info(info);
}

void SettingsWidget::set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                                     uint64_t memory_bytes,
                                     uint64_t mem_hits, uint64_t mem_misses,
                                     uint64_t disk_hits, uint64_t disk_misses)
{
    if (settings_view_)
        settings_view_->set_cache_sizes(local_bytes, sdk_bytes, memory_bytes,
                                        mem_hits, mem_misses,
                                        disk_hits, disk_misses);
}

void SettingsWidget::set_theme(const tk::Theme& t)
{
    surface_->set_theme(t);
    surface_->relayout();
}

void SettingsWidget::populate(
    std::string display_name, std::string user_id, std::string avatar_mxc,
    tesseract::views::AccountSection::ImageProvider provider,
    tesseract::Settings::ThemePreference theme_pref, bool notifications_enabled)
{
    settings_view_->set_account_info(std::move(display_name),
                                     std::move(user_id), std::move(avatar_mxc));
    settings_view_->set_image_provider(std::move(provider));
    settings_view_->set_theme_pref(theme_pref);
    settings_view_->set_notifications_enabled(notifications_enabled);
    settings_view_->set_hide_content_enabled(
        tesseract::Settings::instance().notification_hide_content);
    settings_view_->set_image_previews_enabled(
        tesseract::Settings::instance().notification_image_previews);
    settings_view_->set_prefetch_enabled(
        tesseract::Settings::instance().prefetch_full_media);
    settings_view_->set_send_presence_pref(
        tesseract::Settings::instance().send_presence);
    settings_view_->set_index_messages_pref(
        tesseract::Settings::instance().index_messages_for_search);
#ifdef TESSERACT_GITHUB_REPO
    settings_view_->set_check_for_updates_pref(
        tesseract::Settings::instance().check_for_updates);
#endif
    settings_view_->set_media_previews_pref(
        tesseract::Settings::instance().media_previews);
    settings_view_->set_invite_avatars_pref(
        tesseract::Settings::instance().invite_avatars);
    surface_->relayout();
}

void SettingsWidget::set_controller(tesseract::SettingsController* ctrl,
                                    const std::string& current_display_name)
{
    controller_ = ctrl;

    // Plug in the surface-relayout callback so DevicesSection's async
    // callbacks can invalidate the surface after mutating widgets.
    settings_view_->set_request_repaint([this]
    {
        if (surface_) surface_->relayout();
    });

    // Wire SettingsView (which wires AccountSection + DevicesSection).
    settings_view_->set_controller(ctrl);

    // Wire SettingsView avatar callbacks to controller.
    settings_view_->on_avatar_upload_requested = [this]
    {
        if (controller_) controller_->upload_avatar();
    };
    settings_view_->on_avatar_remove_requested = [this]
    {
        if (controller_) controller_->remove_avatar();
    };

    // Create (or recreate) the NativeTextField for name editing.
    name_field_ = surface_->host().make_text_field();
    name_field_->set_compact(true);
    name_field_->set_text(current_display_name);
    name_field_->set_placeholder("Display name");
    name_field_->set_visible(false);

    name_field_->set_on_submit(
        [this]
        {
            if (!controller_) return;
            const std::string text = name_field_->text();
            controller_->set_display_name(text);
            settings_view_->set_name_busy(true);
            surface_->relayout();
        });

    // Overwrite on_name_changed / on_name_result to also update the NativeTextField.
    ctrl->on_name_changed = [this](std::string name)
    {
        settings_view_->set_display_name_text(name);
        if (name_field_) name_field_->set_text(name);
        surface_->relayout();
    };

    ctrl->on_name_result = [this](bool ok, std::string error)
    {
        settings_view_->set_name_busy(false);
        if (!ok) settings_view_->set_name_error(std::move(error));
        surface_->relayout();
    };

    // Overwrite on_avatar_changed so the sidebar UserInfo strip can refresh.
    // The shared SettingsView lambda only updates the AccountSection chip.
    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        settings_view_->set_avatar_url(mxc);
        surface_->relayout();
        if (on_local_avatar_changed) on_local_avatar_changed(std::move(mxc));
    };

    // Create NativeTextField overlays for the three extended profile fields.
    // The on_submit handlers serialise the text to the MSC-specified JSON
    // shape and forward to the shell via on_profile_field_changed.
    static constexpr char kKeyPronouns[] = "io.fsky.nyx.pronouns";
    static constexpr char kKeyTz[]       = "us.cloke.msc4175.tz";
    static constexpr char kKeyBio[]      = "gay.fomx.biography";

    pronouns_field_ = surface_->host().make_text_field();
    pronouns_field_->set_compact(true);
    pronouns_field_->set_placeholder(_("Pronouns"));
    pronouns_field_->set_visible(false);
    pronouns_field_->set_on_submit(
        [this]
        {
            const std::string text = pronouns_field_->text();
            std::string value_json;
            if (text.empty())
                value_json = "null";
            else
                value_json = "[{\"summary\":" + json_quote(text) +
                             ",\"language\":\"en\"}]";
            settings_view_->set_profile_field_busy(kKeyPronouns, true);
            if (on_profile_field_changed)
                on_profile_field_changed(kKeyPronouns, std::move(value_json));
            surface_->relayout();
        });

    tz_field_ = surface_->host().make_text_field();
    tz_field_->set_compact(true);
    tz_field_->set_placeholder(_("Timezone (e.g. Europe/London)"));
    tz_field_->set_visible(false);
    tz_field_->set_on_submit(
        [this]
        {
            const std::string text = tz_field_->text();
            std::string value_json = text.empty() ? "null" : json_quote(text);
            settings_view_->set_profile_field_busy(kKeyTz, true);
            if (on_profile_field_changed)
                on_profile_field_changed(kKeyTz, std::move(value_json));
            surface_->relayout();
        });

    bio_field_ = surface_->host().make_text_field();
    bio_field_->set_compact(true);
    bio_field_->set_placeholder(_("Short biography"));
    bio_field_->set_visible(false);
    bio_field_->set_on_submit(
        [this]
        {
            const std::string text = bio_field_->text();
            std::string value_json;
            if (text.empty())
                value_json = "null";
            else
                value_json = "{\"m.text\":[{\"body\":" + json_quote(text) + "}]}";
            settings_view_->set_profile_field_busy(kKeyBio, true);
            if (on_profile_field_changed)
                on_profile_field_changed(kKeyBio, std::move(value_json));
            surface_->relayout();
        });

    // Wire SettingsView's re-exported on_profile_field_changed so the shell
    // only needs to wire one callback (on this wrapper) rather than reaching
    // into settings_view_ directly.
    settings_view_->on_profile_field_changed =
        [this](std::string key, std::string value_json)
    {
        if (on_profile_field_changed)
            on_profile_field_changed(std::move(key), std::move(value_json));
    };

    surface_->relayout();
}

void SettingsWidget::set_extended_profile(const tesseract::ExtendedProfile& profile)
{
    if (settings_view_)
        settings_view_->set_extended_profile(profile);
    // Seed the NativeTextField overlays with the current values so the
    // user sees what is stored when they open the Account tab.
    if (pronouns_field_) pronouns_field_->set_text(profile.pronouns);
    if (tz_field_)       tz_field_->set_text(profile.tz);
    if (bio_field_)      bio_field_->set_text(profile.biography);
    if (surface_)        surface_->relayout();
}

void SettingsWidget::set_profile_field_busy(const std::string& key, bool busy)
{
    if (settings_view_)
        settings_view_->set_profile_field_busy(key, busy);
    if (surface_)
        surface_->relayout();
}

void SettingsWidget::set_profile_field_error(const std::string& key,
                                              std::string error)
{
    if (settings_view_)
        settings_view_->set_profile_field_error(key, std::move(error));
    if (surface_)
        surface_->relayout();
}

void SettingsWidget::set_group_inactive_pref(bool enabled)
{
    settings_view_->set_group_inactive_pref(enabled);
}

void SettingsWidget::set_inactive_period_pref(int days)
{
    settings_view_->set_inactive_period_pref(days);
}

void SettingsWidget::set_autoscroll_unread_pref(bool enabled)
{
    settings_view_->set_autoscroll_unread_pref(enabled);
}

} // namespace gtk4
