#include "SettingsView.h"

#include "views/settings/LanguageSection.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace tesseract::views
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SettingsView::SettingsView()
{
    // Back button — placed left-aligned inside the bar by arrange().
    auto back = std::make_unique<tk::Button>(tk::tr("← Back"), std::function<void()>{},
                                             tk::Button::Variant::Subtle);
    back->set_on_click(
        [this]
        {
            if (on_close)
            {
                on_close();
            }
        });
    back_btn_ = add_child(std::move(back));

    // Account section.
    auto account = std::make_unique<AccountSection>();
    account_ = account.get();

    // Appearance section.
    auto appearance = std::make_unique<AppearanceSection>();
    appearance->on_theme_changed =
        [this](tesseract::Settings::ThemePreference pref)
    {
        if (on_theme_changed)
        {
            on_theme_changed(pref);
        }
    };
    appearance->on_group_inactive_changed = [this](bool v)
    {
        if (on_group_inactive_changed) on_group_inactive_changed(v);
    };
    appearance->on_group_unread_changed = [this](bool v)
    {
        if (on_group_unread_changed) on_group_unread_changed(v);
    };
    appearance->on_inactive_period_changed = [this](int days)
    {
        if (on_inactive_period_changed) on_inactive_period_changed(days);
    };
    appearance->on_autoscroll_unread_changed = [this](bool v)
    {
        if (on_autoscroll_unread_changed) on_autoscroll_unread_changed(v);
    };
    appearance->on_show_membership_events_changed = [this](bool v)
    {
        if (on_show_membership_events_changed) on_show_membership_events_changed(v);
    };
    appearance_ = appearance.get();

    // Notifications section.
    auto notifications = std::make_unique<NotificationsSection>();
    notifications->on_notifications_changed = [this](bool enabled)
    {
        if (on_notifications_changed)
        {
            on_notifications_changed(enabled);
        }
    };
    notifications->on_hide_content_changed = [this](bool enabled)
    {
        if (on_hide_content_changed)
        {
            on_hide_content_changed(enabled);
        }
    };
    notifications->on_image_previews_changed = [this](bool enabled)
    {
        if (on_image_previews_changed)
        {
            on_image_previews_changed(enabled);
        }
    };
    notifications_ = notifications.get();

    // Media section.
    auto media = std::make_unique<MediaSection>();
    media->on_prefetch_changed = [this](bool enabled)
    {
        if (on_prefetch_changed)
        {
            on_prefetch_changed(enabled);
        }
    };
    media->on_media_previews_changed =
        [this](tesseract::Settings::MediaPreviews mode)
    {
        if (on_media_previews_changed)
            on_media_previews_changed(mode);
    };
    media->on_invite_avatars_changed = [this](bool enabled)
    {
        if (on_invite_avatars_changed)
            on_invite_avatars_changed(enabled);
    };
    media->on_audio_input_changed = [this](std::string id)
    {
        if (on_audio_input_changed) on_audio_input_changed(std::move(id));
    };
    media->on_audio_output_changed = [this](std::string id)
    {
        if (on_audio_output_changed) on_audio_output_changed(std::move(id));
    };
    media->on_camera_changed = [this](std::string id)
    {
        if (on_camera_changed) on_camera_changed(std::move(id));
    };
    media_ = media.get();

    // Privacy section.
    auto privacy = std::make_unique<PrivacySection>();
    privacy->on_send_presence_changed = [this](bool v)
    {
        if (on_send_presence_changed) on_send_presence_changed(v);
    };
    privacy->on_index_messages_changed = [this](bool v)
    {
        if (request_repaint_) request_repaint_();
        if (on_index_messages_changed) on_index_messages_changed(v);
    };
#ifdef TESSERACT_GITHUB_REPO
    privacy->on_check_for_updates_changed = [this](bool v)
    {
        if (on_check_for_updates_changed) on_check_for_updates_changed(v);
    };
#endif
    // on_export_keys / on_import_keys are wired in set_controller()
    // once the SettingsController is available.
    privacy_ = privacy.get();

    // Server section.
    auto server = std::make_unique<ServerSection>();
    server_section_ = server.get();

    // Sessions section.
    auto devices = std::make_unique<DevicesSection>();
    devices_ = devices.get();

    // About section — pinned to the bottom of the sidebar.
    auto about = std::make_unique<AboutSection>();
    about_ = about.get();

    // Advanced section — hidden bottom tab, revealed via About's "Advanced"
    // button. See kAdvancedTabIdx.
    auto advanced = std::make_unique<AdvancedSection>();
    advanced_ = advanced.get();
    advanced_->on_msc2545_legacy_compat_changed = [this](bool v)
    {
        // Reflect immediately in the Emojis & Stickers tab — the personal
        // pack disappears/reappears without waiting for the async
        // on_image_packs_updated round-trip from the Rust rebuild.
        if (image_packs_) image_packs_->set_personal_pack_enabled(v);
        if (on_msc2545_legacy_compat_changed) on_msc2545_legacy_compat_changed(v);
    };

    // Emojis & Stickers section.
    auto image_packs = std::make_unique<ImagePacksSection>();
    image_packs_ = image_packs.get();
    image_packs_->on_user_pack_pending_image_added =
        [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
              const std::string& mime)
    {
        if (on_user_pack_pending_image_added)
            on_user_pack_pending_image_added(local_id, bytes, mime);
    };

    // Language section.
    auto language = std::make_unique<LanguageSection>();
    language->on_language_changed = [this](std::string code)
    {
        if (on_language_changed)
        {
            on_language_changed(std::move(code));
        }
    };
    language_ = language.get();

    // SideTabView — owns the section widgets. "Sessions" sits next to
    // "Account" since both relate to the signed-in user's identity.
    auto tabs = std::make_unique<tk::SideTabView>();
    tabs->add_tab(tk::tr("Account"), std::move(account));
    tabs->add_tab(tk::tr("Sessions"), std::move(devices));
    tabs->add_tab(tk::tr("Appearance"), std::move(appearance));
    tabs->add_tab(tk::tr("Notifications"), std::move(notifications));
    tabs->add_tab(tk::tr("Media"), std::move(media));
    tabs->add_tab(tk::tr("Privacy"), std::move(privacy));
    tabs->add_tab(tk::tr("Server"), std::move(server));
    tabs->add_tab(tk::tr("Emojis & Stickers"), std::move(image_packs));
    tabs->add_tab(tk::tr("Language"), std::move(language));
    tabs->add_bottom_tab(tk::tr("About"), std::move(about));
    tabs->add_bottom_tab(tk::tr("Advanced"), std::move(advanced));
    // First tab is auto-selected by SideTabView::add_tab.
    tabs->on_tab_selected = [this](int idx)
    {
        // Re-hide Advanced as soon as the user navigates to any other tab —
        // it should only ever be visible while actively selected.
        if (idx != kAdvancedTabIdx)
            tabs_->set_tab_visible(kAdvancedTabIdx, false);
        if (on_tab_changed) on_tab_changed();
    };
    tabs_ = add_child(std::move(tabs));
    // Advanced is hidden until the About tab's "Advanced" button reveals it.
    tabs_->set_tab_visible(kAdvancedTabIdx, false);

    // AccountSection logout: open a confirmation dialog before firing on_logout.
    account_->on_logout = [this]
    {
        confirm_dialog_->open(
            {.title         = tk::tr("Log out?"),
             .body          = tk::tr("You will need to sign in again to access your messages."),
             .confirm_label = tk::tr("Log Out"),
             .cancel_label  = tk::tr("Cancel"),
             .destructive   = true},
            [this] { if (on_logout) on_logout(); });
    };

    // AboutSection: route "Clear all caches" through the shared ConfirmDialog.
    about_->on_clear_caches = [this]
    {
        confirm_dialog_->open(
            {.title         = tk::tr("Clear all caches?"),
             .body          = tk::tr("Downloaded media, voice waveforms, and the local event "
                              "cache will be deleted. Content reloads on demand; the "
                              "event store rebuilds on next startup."),
             .confirm_label = tk::tr("Clear"),
             .cancel_label  = tk::tr("Cancel"),
             .destructive   = true},
            [this] { if (on_clear_caches) on_clear_caches(); });
    };

    // PrivacySection: route "Reset cryptographic identity" through the shared
    // ConfirmDialog — destructive, so guard it behind an explicit confirm.
    privacy_->on_reset_identity = [this]
    {
        confirm_dialog_->open(
            {.title         = tk::tr("Reset your cryptographic identity?"),
             .body          = tk::tr("This creates a brand-new identity and replaces your "
                              "key backup. Your other sessions and the people you "
                              "chat with will need to verify you again. You'll set up "
                              "a new recovery key right after."),
             .confirm_label = tk::tr("Reset"),
             .cancel_label  = tk::tr("Cancel"),
             .destructive   = true},
            [this] { if (on_reset_identity) on_reset_identity(); });
    };

    // AboutSection: "Advanced" button reveals the hidden Advanced tab and
    // navigates straight to it.
    about_->on_advanced_clicked = [this]
    {
        tabs_->set_tab_visible(kAdvancedTabIdx, true);
        tabs_->select(kAdvancedTabIdx);
        if (on_tab_changed) on_tab_changed();
    };

    // ConfirmDialog — painted last so it overlays the entire settings view.
    auto dlg = std::make_unique<ConfirmDialog>();
    confirm_dialog_ = add_child(std::move(dlg));
}

// ---------------------------------------------------------------------------
// Public setters — forwarded to child sections
// ---------------------------------------------------------------------------

void SettingsView::set_account_info(std::string display_name,
                                    std::string user_id, std::string avatar_mxc)
{
    if (!account_)
    {
        return;
    }
    account_->set_display_name(std::move(display_name));
    account_->set_user_id(std::move(user_id));
    account_->set_avatar_url(std::move(avatar_mxc));
}

void SettingsView::set_image_provider(AccountSection::ImageProvider provider)
{
    if (account_)
    {
        account_->set_image_provider(std::move(provider));
    }
}

void SettingsView::set_theme_pref(tesseract::Settings::ThemePreference pref)
{
    if (appearance_)
    {
        appearance_->set_selected(pref);
    }
}

void SettingsView::set_group_inactive_pref(bool enabled)
{
    if (appearance_) appearance_->set_group_inactive(enabled);
}

void SettingsView::set_group_unread_pref(bool enabled)
{
    if (appearance_) appearance_->set_group_unread(enabled);
}

void SettingsView::set_inactive_period_pref(int days)
{
    if (appearance_) appearance_->set_inactive_period(days);
}

void SettingsView::set_autoscroll_unread_pref(bool enabled)
{
    if (appearance_) appearance_->set_autoscroll_unread(enabled);
}

void SettingsView::set_show_membership_events_pref(bool enabled)
{
    if (appearance_) appearance_->set_show_membership_events(enabled);
}

void SettingsView::set_notifications_enabled(bool enabled)
{
    if (notifications_)
    {
        notifications_->set_checked(enabled);
    }
}

void SettingsView::set_hide_content_enabled(bool enabled)
{
    if (notifications_)
    {
        notifications_->set_hide_content_checked(enabled);
    }
}

void SettingsView::set_image_previews_enabled(bool enabled)
{
    if (notifications_)
    {
        notifications_->set_image_previews_checked(enabled);
    }
}

void SettingsView::set_prefetch_enabled(bool enabled)
{
    if (media_)
    {
        media_->set_prefetch_checked(enabled);
    }
}

void SettingsView::set_media_previews_pref(
    tesseract::Settings::MediaPreviews mode)
{
    if (media_)
    {
        media_->set_media_previews(mode);
    }
}

void SettingsView::set_invite_avatars_pref(bool enabled)
{
    if (media_)
    {
        media_->set_invite_avatars(enabled);
    }
}

void SettingsView::set_audio_input_devices(std::vector<tk::DeviceListing> devices)
{
    if (media_) media_->set_audio_input_devices(std::move(devices));
}

void SettingsView::set_audio_output_devices(std::vector<tk::DeviceListing> devices)
{
    if (media_) media_->set_audio_output_devices(std::move(devices));
}

void SettingsView::set_camera_devices(std::vector<tk::DeviceListing> devices)
{
    if (media_) media_->set_camera_devices(std::move(devices));
}

void SettingsView::set_selected_audio_input(const std::string& id)
{
    if (media_) media_->set_selected_audio_input(id);
}

void SettingsView::set_selected_audio_output(const std::string& id)
{
    if (media_) media_->set_selected_audio_output(id);
}

void SettingsView::set_selected_camera(const std::string& id)
{
    if (media_) media_->set_selected_camera(id);
}

void SettingsView::set_send_presence_pref(bool enabled)
{
    if (privacy_)
    {
        privacy_->set_send_presence(enabled);
    }
}

void SettingsView::set_index_messages_pref(bool enabled)
{
    if (privacy_)
    {
        privacy_->set_index_messages(enabled);
    }
}

#ifdef TESSERACT_GITHUB_REPO
void SettingsView::set_check_for_updates_pref(bool enabled)
{
    if (privacy_)
    {
        privacy_->set_check_for_updates(enabled);
    }
}
#endif

void SettingsView::set_msc2545_legacy_compat_pref(bool enabled)
{
    if (advanced_)
    {
        advanced_->set_msc2545_legacy_compat(enabled);
    }
    if (image_packs_)
    {
        image_packs_->set_personal_pack_enabled(enabled);
    }
}

void SettingsView::load_persisted_settings()
{
    auto& s = tesseract::Settings::instance();
    set_theme_pref(s.theme_pref);
    set_notifications_enabled(s.notifications_enabled);
    set_hide_content_enabled(s.notification_hide_content);
    set_image_previews_enabled(s.notification_image_previews);
    set_prefetch_enabled(s.prefetch_full_media);
    set_group_inactive_pref(s.group_inactive_rooms);
    set_group_unread_pref(s.group_unread_rooms);
    set_inactive_period_pref(s.inactive_room_threshold_days);
    set_autoscroll_unread_pref(s.autoscroll_unread_rooms);
    set_show_membership_events_pref(s.show_room_join_leave_events);
    set_msc2545_legacy_compat_pref(s.msc2545_legacy_compat);
    set_send_presence_pref(s.send_presence);
    set_index_messages_pref(s.index_messages_for_search);
#ifdef TESSERACT_GITHUB_REPO
    set_check_for_updates_pref(s.check_for_updates);
#endif
    set_media_previews_pref(s.media_previews);
    set_invite_avatars_pref(s.invite_avatars);
}

void SettingsView::set_search_index_stats(
    const tesseract::SearchIndexStats& stats, bool enabled)
{
    if (privacy_)
    {
        privacy_->set_search_index_stats(stats, enabled);
    }
}

void SettingsView::set_server_info(const tesseract::ServerInfo& info)
{
    if (server_section_)
    {
        server_section_->set_server_info(info);
    }
    if (account_)
    {
        account_->set_editable(info.can_set_displayname);
        account_->set_avatar_editable(info.can_set_avatar);
        account_->set_profile_fields_editable(
            info.supports_profile_fields && info.profile_fields_enabled);
    }
}

void SettingsView::set_current_device_id(std::string id)
{
    if (devices_)
        devices_->set_current_device_id(std::move(id));
}

void SettingsView::set_user_pack_images(
    std::vector<tesseract::ImagePackImage> images)
{
    if (image_packs_)
        image_packs_->set_user_pack_images(std::move(images));
}

void SettingsView::set_user_pack_image_provider(ImagePackImageProvider p)
{
    if (image_packs_)
        image_packs_->set_user_pack_image_provider(std::move(p));
}

void SettingsView::set_user_pack_tile_preview(std::uint64_t local_id,
                                              std::shared_ptr<tk::Image> image)
{
    if (image_packs_)
        image_packs_->set_user_pack_tile_preview(local_id, std::move(image));
}

void SettingsView::set_user_pack_saving(bool saving)
{
    if (image_packs_)
        image_packs_->set_user_pack_saving(saving);
}

void SettingsView::set_user_pack_save_result(bool ok, std::string error)
{
    if (image_packs_)
        image_packs_->set_user_pack_save_result(ok, std::move(error));
}

void SettingsView::set_known_packs(std::vector<tesseract::ImagePack> all_room_packs)
{
    if (image_packs_)
        image_packs_->set_known_packs(std::move(all_room_packs));
}

UserPackEditor* SettingsView::user_pack_editor() const
{
    return image_packs_ ? image_packs_->user_pack_editor() : nullptr;
}

void SettingsView::set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                                   uint64_t memory_bytes,
                                   uint64_t mem_hits, uint64_t mem_misses,
                                   uint64_t disk_hits, uint64_t disk_misses)
{
    if (about_)
    {
        about_->set_memory_cache_size(memory_bytes);
        about_->set_local_cache_size(local_bytes);
        about_->set_sdk_store_size(sdk_bytes);
        about_->set_memory_cache_stats(mem_hits, mem_misses);
        about_->set_local_cache_stats(disk_hits, disk_misses);
    }
    if (request_repaint_) request_repaint_();
}

void SettingsView::set_request_repaint(std::function<void()> cb)
{
    request_repaint_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// tk::Widget overrides
// ---------------------------------------------------------------------------

tk::Size SettingsView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever the host gives us.
    return constraints;
}

void SettingsView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Top bar: full width, fixed height.
    const tk::Rect bar_rect = {bounds.x, bounds.y, bounds.w, kBarHeight};

    // Place the back button left-aligned with a small horizontal margin.
    // Button measures its own natural size; we let it determine its width but
    // fix the height to the bar and add a 12 px left inset.
    constexpr float kBtnHInset = 12.0f;
    if (back_btn_)
    {
        tk::Size btn_size = back_btn_->measure(ctx, {bounds.w, kBarHeight});
        const float btn_w = btn_size.w;
        const float btn_h = std::min(btn_size.h, kBarHeight);
        const float btn_y = bar_rect.y + (kBarHeight - btn_h) * 0.5f;
        back_btn_->arrange(ctx, {bar_rect.x + kBtnHInset, btn_y, btn_w, btn_h});
    }

    // SideTabView: remaining height below the bar + 1 px separator.
    const float tabs_y = bounds.y + kBarHeight + 1.0f;
    const float tabs_h = std::max(0.0f, bounds.h - kBarHeight - 1.0f);
    if (tabs_)
    {
        tabs_->arrange(ctx, {bounds.x, tabs_y, bounds.w, tabs_h});
    }

    // ConfirmDialog overlays the full view.
    if (confirm_dialog_)
    {
        confirm_dialog_->arrange(ctx, bounds);
    }
}

void SettingsView::paint(tk::PaintCtx& ctx)
{
    const tk::Palette& pal = ctx.theme.palette;

    // Overall background.
    ctx.canvas.fill_rect(bounds_, pal.bg);

    // Back bar background (sidebar tone so it feels like chrome).
    const tk::Rect bar_rect = {bounds_.x, bounds_.y, bounds_.w, kBarHeight};
    ctx.canvas.fill_rect(bar_rect, pal.sidebar_bg);

    // 1 px separator between the back bar and the tab view.
    const tk::Rect sep_rect = {bounds_.x, bounds_.y + kBarHeight, bounds_.w,
                               1.0f};
    ctx.canvas.fill_rect(sep_rect, pal.separator);

    // Paint children (back button + SideTabView).
    if (back_btn_ && back_btn_->visible())
    {
        back_btn_->paint(ctx);
    }
    if (tabs_ && tabs_->visible())
    {
        tabs_->paint(ctx);
    }

    // ConfirmDialog paints last so it overlays everything.
    if (confirm_dialog_ && confirm_dialog_->visible())
    {
        confirm_dialog_->paint(ctx);
    }
}

void SettingsView::set_controller(tesseract::SettingsController* ctrl)
{
    // Wire controller result/changed callbacks → AccountSection state.
    ctrl->on_avatar_result = [this](bool ok, std::string error)
    {
        account_->set_avatar_busy(false);
        if (!ok)
            account_->set_avatar_error(std::move(error));
    };
    ctrl->on_name_result = [this](bool ok, std::string error)
    {
        account_->set_name_busy(false);
        if (!ok)
            account_->set_name_error(std::move(error));
    };
    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        account_->set_avatar_url(std::move(mxc));
    };
    ctrl->on_name_changed = [this](std::string name)
    {
        account_->set_display_name(std::move(name));
    };

    // Wire AccountSection click callbacks → SettingsView output callbacks.
    account_->on_avatar_upload_clicked = [this]
    {
        if (on_avatar_upload_requested)
            on_avatar_upload_requested();
    };
    account_->on_avatar_remove_clicked = [this]
    {
        if (on_avatar_remove_requested)
            on_avatar_remove_requested();
    };

    // Re-expose the extended-profile field change callback so the shell can
    // wire it in one place on SettingsView rather than reaching into account_.
    account_->on_profile_field_changed =
        [this](std::string key, std::string value_json)
    {
        if (on_profile_field_changed)
            on_profile_field_changed(std::move(key), std::move(value_json));
    };

    // Wire controller → DevicesSection state. Each callback that mutates
    // widget-tree state must trigger a repaint via request_repaint_ — the
    // shared layer has no other way to invalidate the surface from an
    // async callback.
    ctrl->on_devices_loaded =
        [this](std::vector<tesseract::Client::Device> devices)
    {
        if (!devices_) return;
        devices_->set_devices(std::move(devices));
        if (request_repaint_) request_repaint_();
    };
    ctrl->on_device_renamed =
        [this](std::string id, bool ok, std::string err)
    {
        if (!devices_) return;
        devices_->set_device_busy(id, false);
        if (!ok)
            devices_->set_device_error(id, std::move(err));
        if (request_repaint_) request_repaint_();
    };
    ctrl->on_device_needs_uia =
        [this](std::string id, std::string fallback_url, std::string session)
    {
        if (!devices_) return;
        devices_->enter_uia_state(id, std::move(fallback_url),
                                  std::move(session));
        if (request_repaint_) request_repaint_();
    };
    ctrl->on_device_deleted =
        [this, ctrl](std::string id, bool ok, std::string err)
    {
        if (!devices_) return;
        devices_->set_device_busy(id, false);
        if (ok)
        {
            // Refresh the list — the row is gone on the server now.
            devices_->set_loading(true);
            ctrl->load_devices();
        }
        else
        {
            devices_->clear_uia_state(id);
            devices_->set_device_error(id, std::move(err));
        }
        if (request_repaint_) request_repaint_();
    };

    // Wire DevicesSection click callbacks → controller.
    devices_->on_delete_requested = [this, ctrl](std::string id)
    {
        devices_->set_device_busy(id, true);
        devices_->set_device_error(id, "");
        ctrl->delete_device(std::move(id));
        if (request_repaint_) request_repaint_();
    };
    devices_->on_uia_confirmed =
        [this, ctrl](std::string id, std::string session)
    {
        devices_->set_device_busy(id, true);
        devices_->set_device_error(id, "");
        ctrl->confirm_device_deletion(std::move(id), std::move(session));
        if (request_repaint_) request_repaint_();
    };
    devices_->on_uia_cancelled = [this, ctrl](std::string id)
    {
        ctrl->cancel_device_deletion(id);
        devices_->clear_uia_state(id);
        if (request_repaint_) request_repaint_();
    };

    // Kick off the device list fetch so the Sessions tab is populated by
    // the time the user navigates to it. The current device id is read off
    // the underlying Client so we don't need every shell to wire it.
    if (auto* c = ctrl->client())
    {
        devices_->set_current_device_id(c->get_device_id());
    }
    devices_->set_loading(true);
    ctrl->load_devices();

    // Wire PrivacySection buttons → controller key export/import flows.
    if (privacy_)
    {
        privacy_->on_export_keys = [ctrl] { ctrl->export_room_keys(); };
        privacy_->on_import_keys = [ctrl] { ctrl->import_room_keys(); };
    }

    // Wire controller → ImagePacksSection state.
    ctrl->on_image_packs_loaded =
        [this](std::vector<tesseract::ImagePack> packs)
    {
        if (!image_packs_) return;
        image_packs_->set_known_packs(std::move(packs));
        if (request_repaint_) request_repaint_();
    };
    ctrl->on_user_pack_images_loaded =
        [this](std::vector<tesseract::ImagePackImage> images)
    {
        if (!image_packs_) return;
        image_packs_->set_user_pack_images(std::move(images));
        if (request_repaint_) request_repaint_();
    };
    ctrl->on_user_pack_save_result = [this](bool ok, std::string error)
    {
        if (!image_packs_) return;
        image_packs_->set_user_pack_saving(false);
        image_packs_->set_user_pack_save_result(ok, std::move(error));
        if (request_repaint_) request_repaint_();
    };

    // Wire ImagePacksSection click callbacks → controller.
    image_packs_->on_user_pack_save_clicked = [this, ctrl]
    {
        if (!image_packs_ || !image_packs_->user_pack_editor()) return;
        auto* editor = image_packs_->user_pack_editor();
        if (!editor->has_changes()) return;
        image_packs_->set_user_pack_saving(true);
        if (request_repaint_) request_repaint_();
        ctrl->save_user_pack_changes(editor->build_result());
    };
    image_packs_->on_pack_subscription_toggled =
        [ctrl](std::string room_id, std::string state_key, bool subscribed)
    {
        ctrl->set_pack_subscribed(std::move(room_id), std::move(state_key),
                                  subscribed);
    };

    // Kick off the initial image-pack load, exactly like the devices load
    // above — set_controller is itself only called once per shell at
    // startup, not on every settings-open, so this is loaded once up front
    // and kept current afterwards via on_image_packs_updated (see
    // ShellBase::handle_image_packs_updated_ui_).
    ctrl->load_image_packs();
}

void SettingsView::set_name_busy(bool busy)        { account_->set_name_busy(busy); }
void SettingsView::set_name_error(std::string e)   { account_->set_name_error(std::move(e)); }
void SettingsView::set_avatar_busy(bool busy)      { account_->set_avatar_busy(busy); }
void SettingsView::set_avatar_error(std::string e) { account_->set_avatar_error(std::move(e)); }
void SettingsView::set_avatar_url(std::string m)   { account_->set_avatar_url(std::move(m)); }
void SettingsView::set_display_name_text(std::string n) { account_->set_display_name(std::move(n)); }

tk::Rect SettingsView::name_field_rect() const
{
    if (!account_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return account_->name_field_rect();
}

// ---------------------------------------------------------------------------
// Extended profile (MSC4133) — thin delegation to AccountSection
// ---------------------------------------------------------------------------

void SettingsView::set_extended_profile(const tesseract::ExtendedProfile& profile)
{
    if (account_)
        account_->set_extended_profile(profile);
}

void SettingsView::set_profile_fields_editable(bool editable)
{
    if (account_)
        account_->set_profile_fields_editable(editable);
}

void SettingsView::set_profile_field_busy(const std::string& key, bool busy)
{
    if (account_)
        account_->set_profile_field_busy(key, busy);
}

void SettingsView::set_profile_field_error(const std::string& key, std::string error)
{
    if (account_)
        account_->set_profile_field_error(key, std::move(error));
}

tk::Rect SettingsView::pronouns_field_rect() const
{
    if (!account_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return account_->pronouns_field_rect();
}

tk::Rect SettingsView::tz_field_rect() const
{
    if (!account_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return account_->tz_field_rect();
}

tk::Rect SettingsView::bio_field_rect() const
{
    if (!account_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return account_->bio_field_rect();
}

} // namespace tesseract::views
