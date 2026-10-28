#include "SettingsView.h"

#include "views/settings/LanguageSection.h"
#include "views/settings/NetworkSection.h"
#include "views/settings/PronounsEditor.h"
#include "views/settings/StatusEditor.h"
#include "views/settings/TimezonePicker.h"

#include "views/launch_baseline.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <tesseract/types.h>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace tesseract::views
{

namespace
{
// JSON string escaper: produces a JSON-quoted string literal. Escapes every
// character RFC 8259 requires: quote, backslash, and the control characters
// U+0000-U+001F. A bio or status with a pasted tab or newline must stay valid
// JSON, or the profile PUT fails with no explanation.
std::string json_quote(const std::string& s)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20)
            {
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SettingsView::SettingsView()
{
    // Back button — placed left-aligned inside the bar by arrange().
    auto back = tk::create_widget<tk::Button>(this, tk::tr("← Back"), std::function<void()>{},
                                             tk::Button::Variant::Subtle);
    back->set_on_click(
        [this]
        {
            if (auto* pe = account_->pronouns_editor()) pe->flush();
            if (auto* se = account_->status_editor()) se->flush();
            hide_status_emoji_picker_();
            if (on_close)
            {
                on_close();
            }
        });
    back_btn_ = add_child(std::move(back));

    // Account section.
    auto account = tk::create_widget<AccountSection>(this);
    account_ = account.get();

    // General section.
    auto general = std::make_unique<GeneralSection>(binder_);
    general_ = general.get();

    // Appearance section.
    auto appearance = std::make_unique<AppearanceSection>(binder_);
    appearance_ = appearance.get();

    // Notifications section.
    auto notifications = std::make_unique<NotificationsSection>(binder_);
    // Unlike the three bound checkboxes above (which live in local Settings),
    // the Mentions & Keywords callbacks are push-rule-backed and wired
    // straight to SettingsController in set_controller() below — same
    // pattern as DevicesSection's on_delete_requested/on_uia_confirmed.
    notifications_ = notifications.get();

    // Media section.
    auto media = std::make_unique<MediaSection>(binder_);
    media_ = media.get();

    // Privacy section.
    auto privacy = std::make_unique<PrivacySection>(binder_);
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
    auto advanced = std::make_unique<AdvancedSection>(binder_);
    advanced_ = advanced.get();
    advanced_->on_open_activity_monitor = [this]
    {
        if (on_open_activity_monitor) on_open_activity_monitor();
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

    // Network section.
    auto network = std::make_unique<NetworkSection>(binder_);
    network->on_restart_requested = [this]
    {
        if (on_restart_requested)
            on_restart_requested();
    };
    network_ = network.get();

    // Language section.
    auto language = std::make_unique<LanguageSection>(binder_);
    language->on_restart_requested = [this]
    {
        if (on_restart_requested)
            on_restart_requested();
    };
    language_ = language.get();

    // SideTabView — owns the section widgets. "Sessions" sits next to
    // "Account" since both relate to the signed-in user's identity.
    auto tabs = tk::create_widget<tk::SideTabView>(this);
    tabs->add_tab(tk::tr("Account"), std::move(account));
    tabs->add_tab(tk::tr("Devices"), std::move(devices));
    tabs->add_tab(tk::tr("General"), std::move(general));
    tabs->add_tab(tk::tr("Appearance"), std::move(appearance));
    tabs->add_tab(tk::tr("Notifications"), std::move(notifications));
    tabs->add_tab(tk::tr("Media"), std::move(media));
    tabs->add_tab(tk::tr("Privacy"), std::move(privacy));
    tabs->add_tab(tk::tr("Network"), std::move(network));
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
        // Account's four self-owned fields position themselves from their
        // own arrange(), but an inactive tab's subtree isn't arrange()'d —
        // so a field left visible from the last time Account was shown
        // would otherwise stay stuck. Explicitly hide them here, mirroring
        // RoomSettingsView's identical tab-switch fix.
        if (idx != 0 && account_)
        {
            if (auto* f = account_->name_field())      f->set_visible(false);
            if (auto* pe = account_->pronouns_editor())
            {
                pe->flush();
                pe->set_visible(false);
            }
            if (auto* f = account_->tz_field())         f->set_visible(false);
            if (auto* f = account_->bio_field())        f->set_visible(false);
            if (auto* se = account_->status_editor())
            {
                se->flush();
                se->set_visible(false);
            }
            hide_status_emoji_picker_();
        }
        if (on_tab_changed) on_tab_changed();
        if (idx == kAboutTabIdx && on_about_tab_shown) on_about_tab_shown();
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
             .body          = tk::tr("Deletes all downloaded media and cached message "
                              "history for this account, and closes open tabs and "
                              "pop-out windows. Your rooms and messages reload from the "
                              "server afterwards. Your sign-in and message keys are "
                              "kept, so you stay logged in and confirmed."),
             .confirm_label = tk::tr("Clear"),
             .cancel_label  = tk::tr("Cancel"),
             .destructive   = true},
            [this] { if (on_clear_caches) on_clear_caches(); });
    };

    // PrivacySection: route "Reset encryption" (a cryptographic identity
    // reset) through the shared ConfirmDialog — destructive, so guard it
    // behind an explicit confirm.
    privacy_->on_reset_identity = [this]
    {
        confirm_dialog_->open(
            {.title         = tk::tr("Reset digital identity?"),
             .body          = tk::tr("Only do this if you've lost your recovery key and "
                              "have no other signed-in device. Messages you can't "
                              "read now will stay unreadable, and your other devices "
                              "and the people you chat with will need to confirm you "
                              "again. You'll get a new recovery key right after."),
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

    // MSC4426 status-emoji picker — a Host popup (registered in
    // paint_after_children / painted in paint_overlay), never a tree child,
    // mirroring RoomView's own emoji picker. Unicode-only here: no Client is
    // wired, so the Frequents tab and custom (MSC2545) packs stay empty.
    status_emoji_picker_ = tk::create_widget<EmojiPicker>(this);
    status_emoji_picker_->on_dismiss = [this] { hide_status_emoji_picker_(); };
    status_emoji_picker_->on_selected = [this](const std::string& glyph)
    {
        if (auto* se = status_editor()) se->set_emoji(glyph);
        hide_status_emoji_picker_();
    };
    status_emoji_picker_->on_skin_tone_changed = [this](tesseract::emoji::SkinTone tone)
    {
        if (on_emoji_skin_tone_changed) on_emoji_skin_tone_changed(tone);
    };

    binder_.on_changed = [this](SettingKey key)
    {
        react_locally_(key);
        if (on_setting_changed) on_setting_changed(key);
    };
    binder_.reload();
}

void SettingsView::react_locally_(SettingKey key)
{
    const auto& s = tesseract::Settings::instance();
    switch (key)
    {
    case SettingKey::IndexMessages:
        request_relayout();
        break;
    case SettingKey::Msc2545Compat:
        // Reflect immediately in the Emojis & Stickers tab — the personal
        // pack disappears/reappears without waiting for the async
        // on_image_packs_updated round-trip from the Rust rebuild.
        if (image_packs_) image_packs_->set_personal_pack_enabled(s.msc2545_legacy_compat);
        break;
    case SettingKey::Proxy:
        set_proxy_restart_pending(
            tesseract::proxy_changed_since_launch(s.proxy_mode, s.proxy_url));
        break;
    case SettingKey::Language:
        set_language_restart_pending(s.language != tesseract::launch_language());
        break;
    default:
        break;
    }
}

SettingsView::~SettingsView()
{
    // ctrl_ outlives this view (bound once at login, unaffected by Settings
    // being closed/reopened or this view being destroyed and lazily
    // rebuilt), but every one of these callbacks was set by set_controller()
    // above to capture `this` directly, with no lifetime guard — a still-
    // in-flight operation (loading image packs, saving an avatar, renaming
    // a device) completing after this view is gone would otherwise call
    // back into freed AccountSection/ImagePacksSection/DevicesSection
    // state. Clear them all so a stale completion is simply a no-op.
    if (ctrl_)
    {
        ctrl_->on_avatar_result = nullptr;
        ctrl_->on_name_result = nullptr;
        ctrl_->on_avatar_changed = nullptr;
        ctrl_->on_avatar_preview = nullptr;
        ctrl_->on_name_changed = nullptr;
        ctrl_->on_devices_loaded = nullptr;
        ctrl_->on_device_renamed = nullptr;
        ctrl_->on_device_needs_uia = nullptr;
        ctrl_->on_device_deleted = nullptr;
        ctrl_->on_mentions_settings_loaded = nullptr;
        ctrl_->on_mentions_toggle_result = nullptr;
        ctrl_->on_room_mentions_toggle_result = nullptr;
        ctrl_->on_notify_all_messages_result = nullptr;
        ctrl_->on_notify_on_keywords_result = nullptr;
        ctrl_->on_keyword_add_result = nullptr;
        ctrl_->on_keyword_remove_result = nullptr;
        ctrl_->on_image_packs_loaded = nullptr;
        ctrl_->on_user_pack_images_loaded = nullptr;
        ctrl_->on_user_pack_save_result = nullptr;
    }
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

void SettingsView::set_mentions_enabled_ui(bool enabled)
{
    if (notifications_) notifications_->set_mentions_enabled(enabled);
}

void SettingsView::set_room_mentions_enabled_ui(bool enabled)
{
    if (notifications_) notifications_->set_room_mentions_enabled(enabled);
}

void SettingsView::set_notify_all_messages_ui(bool enabled)
{
    if (notifications_) notifications_->set_notify_all_messages(enabled);
}

void SettingsView::set_notification_keywords_ui(std::vector<std::string> keywords)
{
    if (notifications_) notifications_->set_notification_keywords(std::move(keywords));
}

void SettingsView::set_mentions_loading_ui(bool loading)
{
    if (notifications_) notifications_->set_mentions_loading(loading);
}

void SettingsView::set_keyword_error_ui(std::string error)
{
    if (notifications_) notifications_->set_keyword_error(std::move(error));
}

void SettingsView::set_proxy_restart_pending(bool pending)
{
    if (network_)
    {
        network_->set_restart_pending(pending);
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

void SettingsView::set_launch_at_login_pref(bool enabled)
{
    // The persisted bool is only a cache of the OS registration; keep it in
    // step with the real state so a later user toggle isn't swallowed as
    // "unchanged".
    tesseract::Settings::instance().launch_at_login = enabled;
    if (general_)
    {
        general_->set_launch_at_login(enabled);
    }
}

void SettingsView::set_tray_available(bool available)
{
    if (general_)
    {
        general_->set_tray_available(available);
    }
}

void SettingsView::set_low_power_available(bool available)
{
    if (general_)
    {
        general_->set_low_power_available(available);
    }
}

void SettingsView::load_device_lists()
{
    auto* h = host();
    if (!h)
        return;
    set_audio_input_devices(h->enumerate_audio_inputs());
    set_audio_output_devices(h->enumerate_audio_outputs());
    set_camera_devices(h->enumerate_cameras());
    const auto& s = tesseract::Settings::instance();
    set_selected_audio_input(s.audio_input_device_id);
    set_selected_audio_output(s.audio_output_device_id);
    set_selected_camera(s.camera_device_id);
}

void SettingsView::load_persisted_settings()
{
    binder_.reload();
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
    if (privacy_)
    {
        privacy_->set_homeserver_previews_available(info.preview_url_enabled);
    }
    // Toggling profile-fields editability now also shows/hides the whole
    // ExtendedFields block, which changes AccountSection's measured height —
    // needs a repaint/relayout if server info arrives after Settings is
    // already open.
    request_relayout();
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

void SettingsView::set_language_restart_pending(bool pending)
{
    if (language_)
        language_->set_restart_pending(pending);
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
    request_relayout();
}

bool SettingsView::about_tab_selected() const
{
    return tabs_ && tabs_->selected_idx() == kAboutTabIdx;
}

void SettingsView::request_relayout()
{
    if (auto* h = host())
        h->request_relayout();
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

void SettingsView::paint_before_children(tk::PaintCtx& ctx)
{
    const bool modal_open = confirm_dialog_ && confirm_dialog_->is_open();
    if (modal_open && !modal_was_open_ && host())
    {
        // The confirm dialog just opened (logout / clear-caches /
        // reset-identity) — drop tk-level keyboard focus so its stale
        // focus ring doesn't keep showing through/around it. Mirrors
        // MainAppWidget's identical fix for the main window.
        host()->clear_focus();
    }
    modal_was_open_ = modal_open;

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

    // Children paint via the ordinary paint_children() traversal that
    // follows this call: back_btn_, tabs_, then confirm_dialog_ — already
    // add_child()'d in that order, so confirm_dialog_ naturally overlays
    // everything without needing an explicit z_order.
}

void SettingsView::paint_after_children(tk::PaintCtx& ctx)
{
    tk::Widget::paint_after_children(ctx);
    // Register the status-emoji picker as the active Host popup so input
    // routes to it first and paint_overlay() reaches it — mirrors RoomView.
    if (status_emoji_picker_visible_ && status_emoji_picker_ && ctx.host)
    {
        tk::Widget* trigger =
            status_editor() ? status_editor()->emoji_button() : nullptr;
        ctx.host->register_popup(status_emoji_picker_.get(), trigger);
    }
}

void SettingsView::paint_overlay(tk::PaintCtx& ctx)
{
    tk::Widget::paint_overlay(ctx);
    // Not a tree child, so the recursion above never reaches it.
    if (status_emoji_picker_visible_ && status_emoji_picker_)
        status_emoji_picker_->paint_overlay(ctx);
}

void SettingsView::on_theme_changed(const tk::Theme& t)
{
    tk::Widget::on_theme_changed(t);
    // The status-emoji picker isn't a tree child, so apply_theme()'s
    // recursion never reaches it — forward explicitly (mirrors RoomView).
    if (status_emoji_picker_)
        status_emoji_picker_->apply_theme(t);
}

void SettingsView::show_status_emoji_picker_(tk::Rect world_anchor)
{
    if (!status_emoji_picker_)
        return;

    if (emoji_skin_tone_provider)
        status_emoji_picker_->set_skin_tone(emoji_skin_tone_provider());
    status_emoji_picker_->refresh_frequents();
    status_emoji_picker_->set_search_query("");

    // Prefer opening above the anchor (the status row sits low in the Account
    // tab); flip below if that clips the top. Clamp to the view.
    const float w = EmojiPicker::kWidth;
    const float h = EmojiPicker::kHeight;
    float px = world_anchor.x;
    float py = world_anchor.y - h - 4.0f;
    if (py < bounds_.y)
        py = world_anchor.y + world_anchor.h + 4.0f;
    px = std::clamp(px, bounds_.x, bounds_.x + bounds_.w - w);
    py = std::clamp(py, bounds_.y, bounds_.y + bounds_.h - h);
    status_emoji_picker_->open_at({px, py, w, h});
    status_emoji_picker_->set_visible(true);
    status_emoji_picker_visible_ = true;
    request_relayout();
}

void SettingsView::hide_status_emoji_picker_()
{
    status_emoji_picker_visible_ = false;
    if (status_emoji_picker_)
        status_emoji_picker_->set_visible(false);
    request_relayout();
}

void SettingsView::set_controller(tesseract::SettingsController* ctrl)
{
    ctrl_ = ctrl;

    // Wire controller result/changed callbacks → AccountSection state. The
    // name field itself is self-owned by AccountSection (see
    // AccountSection::name_field()); its on_submit is wired below, once
    // `ctrl` is available — it can't be wired any sooner than this, since
    // AccountSection's own constructor has no controller to call.
    ctrl->on_avatar_result = [this](bool ok, std::string error)
    {
        account_->set_avatar_busy(false);
        if (!ok)
            account_->set_avatar_error(std::move(error));
        request_relayout();
    };
    ctrl->on_name_result = [this](bool ok, std::string error)
    {
        account_->set_name_busy(false);
        if (!ok)
            account_->set_name_error(std::move(error));
        request_relayout();
    };
    ctrl->on_avatar_changed = [this](std::string mxc)
    {
        account_->set_avatar_url(std::move(mxc));
        request_relayout();
    };
    ctrl->on_avatar_preview = [this](std::shared_ptr<tk::Image> preview)
    {
        account_->set_avatar_preview(std::move(preview));
        request_relayout();
    };
    ctrl->on_name_changed = [this](std::string name)
    {
        account_->set_display_name(name);
        if (auto* nf = account_->name_field())
            nf->set_text(std::move(name));
        request_relayout();
    };

    if (auto* nf = account_->name_field())
    {
        nf->set_on_submit(
            [this, ctrl]
            {
                ctrl->set_display_name(account_->name_field()->text());
                account_->set_name_busy(true);
                request_relayout();
            });
    }

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

    // Wire the scalar extended-profile field's on_submit — serialise the
    // text to the MSC-specified JSON shape and forward to the shell via
    // on_profile_field_changed, mirroring what every shell used to
    // duplicate in its own field-owning wrapper. Timezone and pronouns
    // don't fit this "one TextField -> one JSON scalar" shape (Timezone is
    // a TimezonePicker, not a TextField; MSC4247 pronouns is a list of
    // language entries) so they're wired separately, just below.
    struct ProfileFieldSpec
    {
        const char* key;
        tk::TextField* (AccountSection::*accessor)() const;
        std::string (*to_json)(const std::string&);
    };
    static const ProfileFieldSpec kSpecs[] = {
        {"gay.fomx.biography", &AccountSection::bio_field,
         [](const std::string& text) -> std::string
         {
             if (text.empty()) return "null";
             return "{\"m.text\":[{\"body\":" + json_quote(text) + "}]}";
         }},
    };
    for (const auto& spec : kSpecs)
    {
        if (auto* f = (account_->*spec.accessor)())
        {
            f->set_on_submit(
                [this, spec, f]
                {
                    account_->set_profile_field_busy(spec.key, true);
                    if (on_profile_field_changed)
                        on_profile_field_changed(spec.key,
                                                 spec.to_json(f->text()));
                    request_relayout();
                });
        }
    }

    // Timezone: on_changed only fires for a real committed pick or an
    // exact-match Enter (see TimezonePicker/tk::SearchablePicker), so unlike
    // the old free-text field this never saves a string that isn't a known
    // IANA zone id. Both the stable and unstable keys are written because
    // reads prefer m.tz: a stale m.tz left by another client (Element writes
    // both) would otherwise mask this write. Busy/error track the unstable
    // key only; an m.tz failure on a server without stable MSC4133 is benign.
    if (auto* tz = account_->tz_field())
    {
        static constexpr const char* kTzKey       = "us.cloke.msc4175.tz";
        static constexpr const char* kStableTzKey = "m.tz";
        tz->on_changed = [this](std::string value)
        {
            account_->set_profile_field_busy(kTzKey, true);
            if (on_profile_field_changed)
            {
                const std::string json =
                    value.empty() ? "null" : json_quote(value);
                on_profile_field_changed(kStableTzKey, json);
                on_profile_field_changed(kTzKey, json);
            }
            request_relayout();
        };
    }

    // Pronouns: any add/remove/language-pick/summary-submit from the editor
    // re-serialises its *entire* current entry list and PUTs it in one shot
    // (same busy/error/re-fetch flow as the scalar fields above).
    if (auto* pe = account_->pronouns_editor())
    {
        static constexpr const char* kPronounsKey = "io.fsky.nyx.pronouns";
        pe->on_changed =
            [this](std::vector<tesseract::PronounEntry> entries)
        {
            account_->set_profile_field_busy(kPronounsKey, true);
            if (on_profile_field_changed)
            {
                std::string json;
                if (entries.empty())
                {
                    json = "null";
                }
                else
                {
                    json = "[";
                    for (std::size_t i = 0; i < entries.size(); ++i)
                    {
                        if (i > 0) json += ",";
                        json += "{\"summary\":" + json_quote(entries[i].summary) +
                                ",\"language\":" + json_quote(entries[i].language);
                        if (!entries[i].grammatical_gender.empty())
                        {
                            json += ",\"grammatical_gender\":" +
                                    json_quote(entries[i].grammatical_gender);
                        }
                        json += "}";
                    }
                    json += "]";
                }
                on_profile_field_changed(kPronounsKey, json);
            }
            request_relayout();
        };
    }

    // MSC4426 status: emoji + text serialised into one {"text","emoji"} object
    // (or "null" to clear when both are empty), same busy/error/re-fetch flow.
    if (auto* se = account_->status_editor())
    {
        static constexpr const char* kStatusKey = "org.matrix.msc4426.status";
        se->on_emoji_button_clicked = [this](tk::Rect anchor)
        { show_status_emoji_picker_(anchor); };
        se->on_changed = [this](std::string emoji, std::string text)
        {
            account_->set_profile_field_busy(kStatusKey, true);
            if (on_profile_field_changed)
            {
                std::string json;
                if (emoji.empty() && text.empty())
                {
                    json = "null";
                }
                else
                {
                    json = "{\"text\":" + json_quote(text) +
                           ",\"emoji\":" + json_quote(emoji) + "}";
                }
                on_profile_field_changed(kStatusKey, json);
            }
            request_relayout();
        };
    }

    // Wire controller → DevicesSection state. Each callback that mutates
    // widget-tree state must trigger a repaint via request_repaint_ — the
    // shared layer has no other way to invalidate the surface from an
    // async callback.
    ctrl->on_devices_loaded =
        [this](std::vector<tesseract::Client::Device> devices)
    {
        if (!devices_) return;
        devices_->set_devices(std::move(devices));
        request_relayout();
    };
    ctrl->on_device_renamed =
        [this](std::string id, bool ok, std::string err)
    {
        if (!devices_) return;
        devices_->set_device_busy(id, false);
        if (!ok)
            devices_->set_device_error(id, std::move(err));
        request_relayout();
    };
    ctrl->on_device_needs_uia =
        [this](std::string id, std::string fallback_url, std::string session)
    {
        if (!devices_) return;
        devices_->enter_uia_state(id, std::move(fallback_url),
                                  std::move(session));
        request_relayout();
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
        request_relayout();
    };

    // Wire DevicesSection click callbacks → controller.
    devices_->on_delete_requested = [this, ctrl](std::string id)
    {
        devices_->set_device_busy(id, true);
        devices_->set_device_error(id, "");
        ctrl->delete_device(std::move(id));
        request_relayout();
    };
    devices_->on_uia_confirmed =
        [this, ctrl](std::string id, std::string session)
    {
        devices_->set_device_busy(id, true);
        devices_->set_device_error(id, "");
        ctrl->confirm_device_deletion(std::move(id), std::move(session));
        request_relayout();
    };
    devices_->on_uia_cancelled = [this, ctrl](std::string id)
    {
        ctrl->cancel_device_deletion(id);
        devices_->clear_uia_state(id);
        request_relayout();
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

    // Wire controller → NotificationsSection's Mentions & Keywords state.
    ctrl->on_mentions_settings_loaded =
        [this](bool mentions, bool room_mentions, bool all_messages,
              bool notify_on_keywords, std::vector<std::string> keywords)
    {
        if (!notifications_) return;
        notifications_->set_mentions_loading(false);
        notifications_->set_mentions_enabled(mentions);
        notifications_->set_room_mentions_enabled(room_mentions);
        notifications_->set_notify_all_messages(all_messages);
        notifications_->set_notify_on_keywords(notify_on_keywords);
        notifications_->set_notification_keywords(std::move(keywords));
        request_relayout();
    };
    ctrl->on_mentions_toggle_result = [this](bool ok, bool enabled)
    {
        if (!notifications_) return;
        if (!ok) notifications_->set_mentions_enabled(!enabled); // revert on failure
        request_relayout();
    };
    ctrl->on_room_mentions_toggle_result = [this](bool ok, bool enabled)
    {
        if (!notifications_) return;
        if (!ok) notifications_->set_room_mentions_enabled(!enabled);
        request_relayout();
    };
    ctrl->on_notify_all_messages_result = [this](bool ok, bool enabled)
    {
        if (!notifications_) return;
        if (!ok) notifications_->set_notify_all_messages(!enabled);
        request_relayout();
    };
    ctrl->on_notify_on_keywords_result = [this](bool ok, bool enabled)
    {
        if (!notifications_) return;
        if (!ok) notifications_->set_notify_on_keywords(!enabled);
        request_relayout();
    };
    ctrl->on_keyword_add_result = [this](bool ok, std::string keyword)
    {
        if (!notifications_) return;
        if (!ok)
        {
            notifications_->revert_keyword_add(keyword);
            notifications_->set_keyword_error(
                tk::trf(tk::tr("Could not add keyword \"{0}\""), {keyword}));
        }
        else
        {
            notifications_->set_keyword_error("");
        }
        request_relayout();
    };
    ctrl->on_keyword_remove_result = [this](bool ok, std::string keyword)
    {
        if (!notifications_) return;
        if (!ok)
        {
            notifications_->revert_keyword_remove(keyword);
            notifications_->set_keyword_error(
                tk::trf(tk::tr("Could not remove keyword \"{0}\""), {keyword}));
        }
        else
        {
            notifications_->set_keyword_error("");
        }
        request_relayout();
    };

    // Wire NotificationsSection's Mentions & Keywords callbacks → controller.
    if (notifications_)
    {
        notifications_->on_mentions_changed = [ctrl](bool v)
        { ctrl->set_mentions_enabled(v); };
        notifications_->on_room_mentions_changed = [ctrl](bool v)
        { ctrl->set_room_mentions_enabled(v); };
        notifications_->on_notify_all_messages_changed = [ctrl](bool v)
        { ctrl->set_notify_all_messages(v); };
        notifications_->on_notify_on_keywords_changed = [ctrl](bool v)
        { ctrl->set_notify_on_keywords(v); };
        notifications_->on_keyword_add_requested = [ctrl](std::string kw)
        { ctrl->add_notification_keyword(std::move(kw)); };
        notifications_->on_keyword_remove_requested = [ctrl](std::string kw)
        { ctrl->remove_notification_keyword(std::move(kw)); };
    }

    notifications_->set_mentions_loading(true);
    ctrl->load_mentions_settings();

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
        request_relayout();
    };
    ctrl->on_user_pack_images_loaded =
        [this](std::vector<tesseract::ImagePackImage> images)
    {
        if (!image_packs_) return;
        image_packs_->set_user_pack_images(std::move(images));
        request_relayout();
    };
    ctrl->on_user_pack_save_result = [this](bool ok, std::string error)
    {
        if (!image_packs_) return;
        image_packs_->set_user_pack_saving(false);
        image_packs_->set_user_pack_save_result(ok, std::move(error));
        request_relayout();
    };

    // Wire ImagePacksSection click callbacks → controller.
    image_packs_->on_user_pack_save_clicked = [this, ctrl]
    {
        if (!image_packs_ || !image_packs_->user_pack_editor()) return;
        auto* editor = image_packs_->user_pack_editor();
        if (!editor->has_changes()) return;
        image_packs_->set_user_pack_saving(true);
        request_relayout();
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
void SettingsView::set_avatar_preview(std::shared_ptr<tk::Image> img)
{ account_->set_avatar_preview(std::move(img)); }
void SettingsView::set_display_name_text(std::string n) { account_->set_display_name(std::move(n)); }

tk::Rect SettingsView::name_field_rect() const
{
    if (!account_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return account_->name_field_rect();
}

tk::TextField* SettingsView::name_field() const
{
    return account_ ? account_->name_field() : nullptr;
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

PronounsEditor* SettingsView::pronouns_editor() const
{
    return account_ ? account_->pronouns_editor() : nullptr;
}

TimezonePicker* SettingsView::tz_field() const
{
    return account_ ? account_->tz_field() : nullptr;
}

tk::TextField* SettingsView::bio_field() const
{
    return account_ ? account_->bio_field() : nullptr;
}

StatusEditor* SettingsView::status_editor() const
{
    return account_ ? account_->status_editor() : nullptr;
}

void SettingsView::show_account_section()
{
    if (tabs_)
        tabs_->select(kAccountTabIdx); // no-op + no signal if already there
}

void SettingsView::set_signed_out_mode(bool on)
{
    if (!tabs_)
        return;
    for (int i = 0; i <= kAboutTabIdx; ++i)
    {
        const bool keep =
            i == kNetworkTabIdx || i == kLanguageTabIdx || i == kAboutTabIdx;
        tabs_->set_tab_visible(i, !on || keep);
    }
    tabs_->set_tab_visible(kAdvancedTabIdx, false);
    if (about_)
        about_->set_account_actions_visible(!on);
    if (network_)
        network_->set_applies_immediately(on);
    if (on)
        tabs_->select(kNetworkTabIdx);
}

bool SettingsView::tab_visible(int idx) const
{
    return tabs_ && tabs_->tab_visible(idx);
}

void SettingsView::show_appearance_section()
{
    if (tabs_)
        tabs_->select(kAppearanceTabIdx);
}

std::string SettingsView::selected_tab_label() const
{
    return tabs_ ? tabs_->tab_label(tabs_->selected_idx()) : std::string{};
}

} // namespace tesseract::views
