#include "RoomSettingsView.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

RoomSettingsChanges compute_room_settings_changes(
    const RoomSettingsFieldValues& original,
    const RoomSettingsFieldValues& staged)
{
    RoomSettingsChanges changes;
    if (staged.name != original.name)
        changes.name = staged.name;
    if (staged.topic != original.topic)
        changes.topic = staged.topic;
    if (staged.avatar_mxc != original.avatar_mxc)
        changes.avatar_mxc = staged.avatar_mxc;
    // Encryption can only be turned ON — no disable API exists anywhere in
    // matrix-sdk (Room::enable_encryption is the only related method). Only
    // emit a change when newly true; never emit a "turn off".
    if (staged.is_encrypted && !original.is_encrypted)
        changes.is_encrypted = true;
    if (staged.join_rule != original.join_rule)
        changes.join_rule = staged.join_rule;
    if (staged.guest_access != original.guest_access)
        changes.guest_access = staged.guest_access;
    if (staged.history_visibility != original.history_visibility)
        changes.history_visibility = staged.history_visibility;
    // media_override_mode is meaningless when has_media_override is false,
    // so don't let an unrelated mode-field difference trigger a spurious
    // change while has_override is false on both sides.
    if (staged.has_media_override != original.has_media_override ||
        (staged.has_media_override &&
         staged.media_override_mode != original.media_override_mode))
    {
        changes.media_override = RoomMediaOverrideChange{
            staged.has_media_override, staged.media_override_mode};
    }
    if (staged.permissions != original.permissions)
        changes.permissions = staged.permissions;
    return changes;
}

bool would_lock_out_of_permissions(const tesseract::RoomPermissions& staged,
                                   const tesseract::RoomOwnPowerLevel& own)
{
    const int64_t effective_own_level =
        own.has_explicit_override ? own.level : staged.default_role;
    return effective_own_level < staged.change_permissions;
}

RoomSettingsView::RoomSettingsView()
{
    accept_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Accept"), std::function<void()>{},
                                     tk::Button::Variant::Primary));
    cancel_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Cancel"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    accept_btn_->set_on_click([this]() {
        if (committing_) return;
        RoomSettingsChanges changes = compute_room_settings_changes(
            RoomSettingsFieldValues{
                original_name_, original_topic_, original_avatar_mxc_,
                original_is_encrypted_, original_join_rule_,
                original_guest_access_, original_history_visibility_,
                original_media_has_override_, original_media_mode_,
                original_permissions_},
            RoomSettingsFieldValues{
                staged_name_, staged_topic_, staged_avatar_mxc_,
                staged_is_encrypted_, staged_join_rule_,
                staged_guest_access_, staged_history_visibility_,
                staged_media_has_override_, staged_media_mode_,
                staged_permissions_});
        committing_ = true;
        general_->set_committing(true);
        security_->set_committing(true);
        permissions_->set_committing(true);
        commit_error_.clear();
        commit_error_layout_.reset();
        refresh_accept_enabled_();
        cancel_btn_->set_enabled(false);
        if (on_accept) on_accept(room_id_, std::move(changes));
    });
    cancel_btn_->set_on_click([this]() {
        if (committing_) return;
        if (on_cancel) on_cancel();
    });

    auto general = std::make_unique<RoomGeneralSection>();
    general_ = general.get();
    general_->on_avatar_upload_clicked = [this]()
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
    };
    general_->on_avatar_remove_clicked = [this]()
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
    };
    general_->on_room_id_clicked = [this](std::string room_id)
    {
        if (on_copy_to_clipboard) on_copy_to_clipboard(room_id);
        toast_->show(tk::tr("Copied to clipboard"));
        if (on_layout_changed) on_layout_changed();
        if (post_delayed_)
        {
            post_delayed_(1500, [this]()
            {
                toast_->hide();
                if (on_layout_changed) on_layout_changed();
            });
        }
    };

    auto media = std::make_unique<RoomMediaSection>();
    media_ = media.get();
    media_->on_override_changed =
        [this](std::optional<tesseract::MediaPreviewConfig::Mode> mode)
    {
        staged_media_has_override_ = mode.has_value();
        staged_media_mode_ = mode.value_or(tesseract::MediaPreviewConfig::Mode::On);
    };

    auto security = std::make_unique<RoomSecuritySection>();
    security_ = security.get();
    security_->on_encryption_changed = [this](bool checked)
    {
        staged_is_encrypted_ = checked;
    };
    security_->on_join_rule_changed = [this](std::string join_rule)
    {
        staged_join_rule_ = std::move(join_rule);
    };
    security_->on_guest_access_changed = [this](bool allow)
    {
        staged_guest_access_ = allow;
    };
    security_->on_history_visibility_changed = [this](std::string visibility)
    {
        staged_history_visibility_ = std::move(visibility);
    };
    security_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };

    auto permissions = std::make_unique<RoomPermissionsSection>();
    permissions_ = permissions.get();
    permissions_->on_permissions_changed = [this](tesseract::RoomPermissions p)
    {
        staged_permissions_ = p;
        refresh_permissions_lockout_();
    };
    permissions_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };

    auto image_packs = std::make_unique<ImagePackEditorView>();
    image_packs_ = image_packs.get();
    image_packs_->on_layout_changed = [this]()
    {
        if (on_layout_changed) on_layout_changed();
    };
    // Cancelling the image-pack tab closes the whole dialog, same outward
    // effect as the main Cancel button — this tab has no separate "just
    // reset this one tab" concept, matching how Cancel discards every other
    // tab's staged edits too.
    image_packs_->on_cancel = [this]()
    {
        if (on_cancel) on_cancel();
    };
    image_packs_->on_accept = [this](ImagePackEditorResult result)
    {
        if (on_image_pack_accept) on_image_pack_accept(std::move(result));
    };
    image_packs_->on_pack_images_needed = [this](std::string pack_id)
    {
        if (on_image_pack_images_needed) on_image_pack_images_needed(std::move(pack_id));
    };
    image_packs_->on_pending_image_added =
        [this](std::uint64_t local_id, const std::vector<std::uint8_t>& bytes,
              const std::string& mime)
    {
        if (on_image_pack_pending_image_added)
            on_image_pack_pending_image_added(local_id, bytes, mime);
    };

    auto tabs = std::make_unique<tk::SideTabView>();
    tabs->add_tab(tk::tr("General"), std::move(general));
    tabs->add_tab(tk::tr("Media"), std::move(media));
    tabs->add_tab(tk::tr("Security & Privacy"), std::move(security));
    tabs->add_tab(tk::tr("Permissions"), std::move(permissions));
    tabs->add_tab(tk::tr("Emojis & Stickers"), std::move(image_packs));
    // Switching tabs must re-poll General's NativeTextField/NativeTextArea
    // overlay rects (name_field_rect()/topic_edit_rect() already collapse
    // to {} when General isn't selected) — without this, the shell never
    // learns to reposition/hide them and the overlay stays floating over
    // whichever tab the user switched to. Mirrors SettingsView.cpp's
    // identical on_tab_selected -> on_tab_changed forwarding.
    tabs->on_tab_selected = [this](int) { if (on_layout_changed) on_layout_changed(); };
    tabs_ = add_child(std::move(tabs));

    auto toast = std::make_unique<Toast>();
    toast_ = add_child(std::move(toast));

    // Closed-by-default; same idiom as RoomInfoPanel/ConfirmDialog.
    set_visible(false);
}

void RoomSettingsView::set_post_delayed(
    std::function<void(int, std::function<void()>)> f)
{
    post_delayed_ = std::move(f);
}

void RoomSettingsView::open(const tesseract::RoomInfo& info)
{
    const bool was_open = open_;

    room_id_ = info.id;
    original_name_        = info.name;
    original_topic_       = info.topic;
    original_avatar_mxc_  = info.avatar_url;
    staged_name_          = original_name_;
    staged_topic_         = original_topic_;
    staged_avatar_mxc_    = original_avatar_mxc_;

    general_->set_name(staged_name_);
    general_->set_topic(staged_topic_);
    general_->set_avatar_url(staged_avatar_mxc_);
    general_->set_room_id(room_id_);
    general_->set_canonical_alias(info.canonical_alias);
    general_->set_avatar_busy(false);
    general_->set_avatar_error("");
    general_->set_field_permissions(false, false, false);
    general_->set_committing(false);
    general_->reset();

    original_is_encrypted_       = info.is_encrypted;
    original_join_rule_          = info.join_rule;
    original_guest_access_       = info.guest_access;
    original_history_visibility_ = info.history_visibility;
    staged_is_encrypted_         = original_is_encrypted_;
    staged_join_rule_            = original_join_rule_;
    staged_guest_access_         = original_guest_access_;
    staged_history_visibility_   = original_history_visibility_;

    security_->set_encryption(staged_is_encrypted_);
    security_->set_join_rule(staged_join_rule_);
    security_->set_guest_access(staged_guest_access_);
    security_->set_history_visibility(staged_history_visibility_);
    security_->set_field_permissions(false, false, false, false);
    security_->set_committing(false);

    // Placeholder — ShellBase's on_room_settings_opened handler calls
    // set_permissions_state() synchronously right after open() (Client::
    // room_power_levels is a cached local read, unlike the async fetch
    // Security & Privacy needs), so this placeholder is corrected within
    // the same call that opens the dialog rather than moments later.
    original_permissions_ = tesseract::RoomPermissions{};
    staged_permissions_   = tesseract::RoomPermissions{};
    own_power_level_      = tesseract::RoomOwnPowerLevel{};
    can_edit_permissions_ = false;
    permissions_->set_permissions(staged_permissions_);
    permissions_->set_field_permissions(false);
    permissions_->set_committing(false);

    // Placeholder — ShellBase::seed_room_media_section_ (called right after
    // open() by each shell's on_room_settings_opened handler) pushes the
    // real cached/fetched per-room override immediately after this, via
    // set_media_override(), which seeds both original_media_*_ and
    // staged_media_*_ together.
    media_->set_override(false, tesseract::MediaPreviewConfig::Mode::On);
    original_media_has_override_ = false;
    original_media_mode_         = tesseract::MediaPreviewConfig::Mode::On;
    staged_media_has_override_   = false;
    staged_media_mode_           = tesseract::MediaPreviewConfig::Mode::On;

    committing_ = false;
    commit_error_.clear();
    commit_error_layout_.reset();
    // Not a real computation against placeholder data (would_lock_out_of_
    // permissions(RoomPermissions{}, RoomOwnPowerLevel{}) actually reads as
    // locked out: default_role 0 < change_permissions 50) — just a safe
    // "not locked out" reset, corrected for real once set_permissions_state
    // and set_own_power_level are called (synchronously, right after open(),
    // by ShellBase's on_room_settings_opened handler).
    would_lock_out_self_ = false;
    permissions_->set_would_lock_out_self(false);
    refresh_accept_enabled_();
    cancel_btn_->set_enabled(true);

    // ShellBase's on_room_settings_opened handler pushes the room's actual
    // packs/images right after this via set_image_pack_available_packs (see
    // seed_image_pack_tab_) — this just resets the tab's own staged state,
    // mirroring every other tab's placeholder-then-corrected seeding.
    image_packs_->open(room_id_);

    title_layout_.reset();

    open_ = true;
    set_visible(true);

    if (!was_open && on_layout_changed) on_layout_changed();
}

void RoomSettingsView::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    image_packs_->close();
    if (was_open && on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_avatar_provider(ImageProvider p)
{
    general_->set_avatar_provider(std::move(p));
}

void RoomSettingsView::set_field_permissions(bool can_name, bool can_topic,
                                             bool can_avatar)
{
    general_->set_field_permissions(can_name, can_topic, can_avatar);
    if (on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_security_field_permissions(
    bool can_encryption, bool can_join_rule, bool can_guest_access,
    bool can_history_visibility)
{
    security_->set_field_permissions(can_encryption, can_join_rule,
                                     can_guest_access, can_history_visibility);
    if (on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_security_state(bool is_encrypted, std::string join_rule,
                                          bool guest_access,
                                          std::string history_visibility)
{
    original_is_encrypted_       = is_encrypted;
    original_join_rule_          = join_rule;
    original_guest_access_       = guest_access;
    original_history_visibility_ = history_visibility;
    staged_is_encrypted_         = original_is_encrypted_;
    staged_join_rule_            = original_join_rule_;
    staged_guest_access_         = original_guest_access_;
    staged_history_visibility_   = original_history_visibility_;

    security_->set_encryption(staged_is_encrypted_);
    security_->set_join_rule(staged_join_rule_);
    security_->set_guest_access(staged_guest_access_);
    security_->set_history_visibility(staged_history_visibility_);
}

void RoomSettingsView::set_permissions_field_permissions(bool can_edit)
{
    can_edit_permissions_ = can_edit;
    permissions_->set_field_permissions(can_edit);
    refresh_permissions_lockout_();
    if (on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_permissions_state(
    const tesseract::RoomPermissions& permissions)
{
    original_permissions_ = permissions;
    staged_permissions_   = permissions;
    permissions_->set_permissions(staged_permissions_);
    refresh_permissions_lockout_();
}

void RoomSettingsView::set_own_power_level(const tesseract::RoomOwnPowerLevel& own)
{
    own_power_level_ = own;
    refresh_permissions_lockout_();
}

void RoomSettingsView::refresh_permissions_lockout_()
{
    would_lock_out_self_ = can_edit_permissions_ &&
        would_lock_out_of_permissions(staged_permissions_, own_power_level_);
    permissions_->set_would_lock_out_self(would_lock_out_self_);
    refresh_accept_enabled_();
}

void RoomSettingsView::refresh_accept_enabled_()
{
    accept_btn_->set_enabled(!committing_ && !would_lock_out_self_);
}

tk::Rect RoomSettingsView::name_field_rect() const
{
    if (!open_ || !general_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return general_->name_field_rect();
}

void RoomSettingsView::set_name_edit_text(std::string t)
{
    staged_name_ = std::move(t);
    general_->set_name(staged_name_);
}

tk::Rect RoomSettingsView::topic_edit_rect() const
{
    if (!open_ || !general_ || !tabs_ || tabs_->selected_idx() != 0)
        return {};
    return general_->topic_edit_rect();
}

void RoomSettingsView::set_topic_edit_text(std::string t)
{
    staged_topic_ = std::move(t);
}

void RoomSettingsView::set_topic_area_natural_height(float h)
{
    general_->set_topic_area_natural_height(h);
    if (on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_staged_avatar(std::string mxc)
{
    staged_avatar_mxc_ = std::move(mxc);
    general_->set_avatar_url(staged_avatar_mxc_);
}

void RoomSettingsView::set_avatar_busy(bool busy)
{
    general_->set_avatar_busy(busy);
}

void RoomSettingsView::set_avatar_error(std::string error)
{
    general_->set_avatar_error(std::move(error));
}

void RoomSettingsView::set_commit_result(bool ok, std::string error)
{
    committing_ = false;
    general_->set_committing(false);
    security_->set_committing(false);
    permissions_->set_committing(false);
    if (ok)
    {
        close();
        return;
    }
    commit_error_ = std::move(error);
    commit_error_layout_.reset();
    refresh_accept_enabled_();
    cancel_btn_->set_enabled(true);
    if (on_layout_changed) on_layout_changed();
}

void RoomSettingsView::set_media_override(bool has_override,
                                          tesseract::MediaPreviewConfig::Mode mode)
{
    // Establishes the baseline for both original and staged together — like
    // every other tab, nothing is sent to the server until Accept, so
    // "no change" must mean "matches what was seeded here."
    original_media_has_override_ = has_override;
    original_media_mode_         = mode;
    staged_media_has_override_   = has_override;
    staged_media_mode_           = mode;
    media_->set_override(has_override, mode);
}

void RoomSettingsView::set_image_pack_available_packs(
    std::vector<tesseract::ImagePack> packs)
{
    image_packs_->set_available_packs(std::move(packs));
}

void RoomSettingsView::set_image_pack_images(
    std::string pack_id, std::vector<tesseract::ImagePackImage> images)
{
    image_packs_->set_pack_images(std::move(pack_id), std::move(images));
}

void RoomSettingsView::set_image_pack_provider(ImagePackImageProvider p)
{
    image_packs_->set_image_provider(std::move(p));
}

void RoomSettingsView::set_image_pack_tile_preview(
    std::uint64_t local_id, std::shared_ptr<tk::Image> image)
{
    image_packs_->set_tile_preview(local_id, std::move(image));
}

void RoomSettingsView::set_image_pack_new_pack_name_text(std::string text)
{
    image_packs_->set_new_pack_name_text(std::move(text));
}

void RoomSettingsView::set_image_pack_editing_shortcode_text(std::string text)
{
    image_packs_->set_editing_shortcode_text(std::move(text));
}

void RoomSettingsView::commit_image_pack_editing_shortcode()
{
    image_packs_->commit_editing_shortcode();
}

void RoomSettingsView::add_image_pack_pasted_image(
    std::vector<std::uint8_t> bytes, std::string mime)
{
    image_packs_->add_pending_image_to_active(std::move(bytes), std::move(mime));
}

void RoomSettingsView::add_image_pack_dropped_image(
    tk::Point pos, std::vector<std::uint8_t> bytes, std::string mime)
{
    image_packs_->add_pending_image_at(pos, std::move(bytes), std::move(mime));
}

bool RoomSettingsView::image_pack_tab_selected_() const
{
    return open_ && tabs_ && tabs_->selected_idx() == kImagePackTabIndex;
}

tk::Rect RoomSettingsView::image_pack_new_pack_name_field_rect() const
{
    if (!image_pack_tab_selected_())
        return {};
    return image_packs_->new_pack_name_field_rect();
}

std::uint64_t RoomSettingsView::image_pack_new_pack_name_reset_generation() const
{
    return image_packs_->new_pack_name_reset_generation();
}

tk::Rect RoomSettingsView::image_pack_shortcode_edit_rect() const
{
    if (!image_pack_tab_selected_())
        return {};
    return image_packs_->shortcode_edit_rect();
}

tk::Rect RoomSettingsView::image_pack_list_rect() const
{
    if (!image_pack_tab_selected_())
        return {};
    return image_packs_->list_rect();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size RoomSettingsView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire replaced content area
}

void RoomSettingsView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    // The image-pack tab manages its own independent Accept/Cancel footer
    // (see ImagePackEditorView) — give it the full remaining height instead
    // of reserving space for this view's own footer, and hide/skip that
    // footer's buttons entirely so there's no double footer or click-through
    // to a hidden button underneath the tab's own one.
    const bool image_pack_tab =
        tabs_ && tabs_->selected_idx() == kImagePackTabIndex;

    // Title bar, tabs, and footer bar are chrome sandwiched top/bottom
    // around the tab content — same idiom as SettingsView's back-bar, so
    // the footer reads as part of the same surface rather than a
    // disconnected strip below the tab widget.
    const float tabs_y   = bounds.y + kBarHeight + 1.0f;
    const float footer_y = image_pack_tab ? bounds.y + bounds.h
                                          : bounds.y + bounds.h - kFooterH;
    const float tabs_h   = std::max(0.0f, footer_y - tabs_y);
    if (tabs_)
        tabs_->arrange(lc, {bounds.x, tabs_y, bounds.w, tabs_h});

    if (accept_btn_) accept_btn_->set_visible(!image_pack_tab);
    if (cancel_btn_) cancel_btn_->set_visible(!image_pack_tab);

    if (!image_pack_tab)
    {
        // Accept/Cancel — right-aligned within the footer bar, same right
        // margin (kPadX) the title bar uses.
        const float btn_w_min = 88.0f;
        tk::Size accept_sz = accept_btn_ ? accept_btn_->measure(lc, {-1.0f, kBtnH})
                                        : tk::Size{btn_w_min, kBtnH};
        tk::Size cancel_sz = cancel_btn_ ? cancel_btn_->measure(lc, {-1.0f, kBtnH})
                                        : tk::Size{btn_w_min, kBtnH};
        const float accept_w = std::max(accept_sz.w, btn_w_min);
        const float cancel_w = std::max(cancel_sz.w, btn_w_min);
        const float btns_y   = footer_y + (kFooterH - kBtnH) * 0.5f;
        const float accept_x = bounds.x + bounds.w - kPadX - accept_w;
        const float cancel_x = accept_x - kBtnGap - cancel_w;

        if (cancel_btn_)
            cancel_btn_->arrange(lc, {cancel_x, btns_y, cancel_w, kBtnH});
        if (accept_btn_)
            accept_btn_->arrange(lc, {accept_x, btns_y, accept_w, kBtnH});
    }

    if (toast_)
        toast_->arrange(lc, bounds);
}

// ── paint ─────────────────────────────────────────────────────────────────

void RoomSettingsView::paint(tk::PaintCtx& ctx)
{
    if (!open_) return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Opaque background — this view fully replaces the header/timeline/
    // composer, so (unlike RoomInfoPanel/ConfirmDialog) there is nothing
    // underneath to dim.
    cv.fill_rect(bounds_, pal.bg);

    // Title bar — same chrome (sidebar_bg + separator) as SettingsView's
    // back-bar, so this reads as one continuous surface with the footer.
    const tk::Rect bar_rect = {bounds_.x, bounds_.y, bounds_.w, kBarHeight};
    cv.fill_rect(bar_rect, pal.sidebar_bg);
    cv.fill_rect({bounds_.x, bounds_.y + kBarHeight, bounds_.w, 1.0f}, pal.separator);

    if (!title_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::UiSemibold;
        st.halign    = tk::TextHAlign::Leading;
        st.max_width = std::max(0.0f, bounds_.w - 2.0f * kPadX);
        title_layout_ = ctx.factory.build_text(tk::tr("Room Settings"), st);
    }
    if (title_layout_)
    {
        const float title_y =
            bounds_.y + (kBarHeight - title_layout_->measure().h) * 0.5f;
        cv.draw_text(*title_layout_, {bounds_.x + kPadX, title_y}, pal.text_primary);
    }

    if (tabs_ && tabs_->visible())
        tabs_->paint(ctx);

    // The image-pack tab paints its own independent footer inside its own
    // bounds (see arrange()'s image_pack_tab branch) — skip this view's own
    // footer bar/buttons entirely rather than drawing a redundant one.
    const bool image_pack_tab =
        tabs_ && tabs_->selected_idx() == kImagePackTabIndex;
    if (!image_pack_tab)
    {
        // Footer bar — same chrome as the title bar, so Accept/Cancel read as
        // part of the settings surface rather than a detached bar underneath
        // the tab widget.
        const float footer_y = bounds_.y + bounds_.h - kFooterH;
        const tk::Rect footer_rect = {bounds_.x, footer_y, bounds_.w, kFooterH};
        cv.fill_rect({bounds_.x, footer_y, bounds_.w, 1.0f}, pal.separator);
        cv.fill_rect({bounds_.x, footer_y + 1.0f, bounds_.w, kFooterH - 1.0f}, pal.sidebar_bg);

        // Commit error, left-aligned within the footer bar.
        if (!commit_error_.empty())
        {
            if (!commit_error_layout_)
            {
                tk::TextStyle st{};
                st.role      = tk::FontRole::Small;
                st.halign    = tk::TextHAlign::Leading;
                st.max_width = std::max(0.0f, bounds_.w - 2.0f * kPadX);
                commit_error_layout_ = ctx.factory.build_text(commit_error_, st);
            }
            if (commit_error_layout_)
            {
                const float err_y = footer_rect.y +
                                    (kFooterH - commit_error_layout_->measure().h) * 0.5f;
                cv.draw_text(*commit_error_layout_, {bounds_.x + kPadX, err_y},
                             tk::Color::rgb(0xcc3333));
            }
        }

        if (cancel_btn_) cancel_btn_->paint(ctx);
        if (accept_btn_) accept_btn_->paint(ctx);
    }

    if (toast_ && toast_->visible()) toast_->paint(ctx);
}

} // namespace tesseract::views
