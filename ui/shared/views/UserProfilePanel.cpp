#include "UserProfilePanel.h"
#include "icons.h"
#include "media_utils.h"
#include "pronoun_utils.h"
#include "views/settings/bcp47_languages.h"

#include "tk/host.h"
#include "tk/hover_area.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

namespace tesseract::views
{

namespace {
    constexpr float kExtFieldPadTop = 8.0f;
    constexpr float kExtLabelW      = 60.0f;
    constexpr float kExtFieldGap    = 6.0f;
    constexpr float kExtRowGap      = 4.0f;  // vertical gap between ext rows
    constexpr int   kExtValueMaxLines = 4;   // cap on wrapped value lines (bio, etc.)

    constexpr float kTrustIconSz    = 18.0f; // shield beside the display name
    constexpr float kTrustIconGap   = 6.0f;
    constexpr float kRoomsHeaderH   = 22.0f;
    constexpr float kRoomsListMinH  = 120.0f;
    constexpr float kRoomRowH       = 40.0f;
    constexpr float kRoomAvatarD    = 28.0f;
    constexpr float kRoomRowPadX    = 8.0f;
    constexpr float kRoomAvatarGap  = 10.0f;
    constexpr float kCardMargin     = 8.0f;  // min gap between card and surface edge
}

// ── rooms-in-common list ──────────────────────────────────────────────────

class UserProfilePanel::CommonRoomsAdapter : public tk::ListAdapter,
                                             public tk::ListAdapterAccessibility
{
public:
    explicit CommonRoomsAdapter(UserProfilePanel& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.common_rooms_.size(); }

    float measure_row_height(std::size_t, tk::LayoutCtx&, float) override
    {
        return kRoomRowH;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                   bool /*selected*/, bool hovered) override
    {
        if (index >= owner_.common_rooms_.size())
        {
            return;
        }
        const CommonRoom& room = owner_.common_rooms_[index];
        const std::string name = room.name.empty() ? tk::tr("Unnamed room") : room.name;

        if (hovered)
        {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.sidebar_hover);
            tk::paint_hc_hover_outline(ctx.canvas, ctx.theme, bounds);
        }

        const tk::Image* avatar = nullptr;
        if (owner_.common_avatar_provider_ && !room.avatar_url.empty())
        {
            avatar = owner_.common_avatar_provider_(room.avatar_url);
            if (!avatar && owner_.on_common_room_avatar_needed &&
                owner_.common_avatar_requested_.insert(room.room_id).second)
            {
                owner_.on_common_room_avatar_needed(room.room_id);
            }
        }
        draw_avatar(ctx.canvas, avatar,
                    {bounds.x + kRoomRowPadX + kRoomAvatarD * 0.5f,
                     bounds.y + bounds.h * 0.5f},
                    kRoomAvatarD, name, ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);

        const float text_x = bounds.x + kRoomRowPadX + kRoomAvatarD + kRoomAvatarGap;
        tk::TextStyle ns{};
        ns.role      = tk::FontRole::SidebarName;
        ns.trim      = tk::TextTrim::Ellipsis;
        ns.max_width = std::max(0.0f, bounds.x + bounds.w - text_x - kRoomRowPadX);
        auto lo = ctx.factory.build_text(name, ns);
        if (lo)
        {
            const tk::Size sz = lo->measure();
            ctx.canvas.draw_text(*lo, {text_x, bounds.y + (bounds.h - sz.h) * 0.5f},
                                 ctx.theme.palette.text_primary);
        }
    }

    tk::Role access_role_for_row(std::size_t index) const override
    {
        return index < owner_.common_rooms_.size() ? tk::Role::ListItem : tk::Role::None;
    }
    std::string access_name_for_row(std::size_t index) const override
    {
        if (index >= owner_.common_rooms_.size())
        {
            return {};
        }
        const auto& n = owner_.common_rooms_[index].name;
        return n.empty() ? tk::tr("Unnamed room") : n;
    }
    bool access_activate_row(std::size_t index) override
    {
        if (index >= owner_.common_rooms_.size())
        {
            return false;
        }
        if (owner_.on_common_room_clicked)
        {
            owner_.on_common_room_clicked(owner_.common_rooms_[index].room_id);
        }
        return true;
    }

private:
    UserProfilePanel& owner_;
};

// ── constructor ───────────────────────────────────────────────────────────

UserProfilePanel::~UserProfilePanel() = default;

UserProfilePanel::UserProfilePanel()
    : common_adapter_(std::make_unique<CommonRoomsAdapter>(*this))
{
    // Hover ring + pointer cursor + click for the avatar; keyboard/a11y stays
    // with avatar_target_ below.
    avatar_area_ = add_child(
        tk::create_widget<tk::HoverArea>(this, tk::HoverStyle::Ring, kAvatarD * 0.5f));
    avatar_area_->set_on_click(
        [this]
        {
            if (on_avatar_clicked && !avatar_url_.empty())
                on_avatar_clicked(avatar_url_, display_name_);
        });

    avatar_target_ = add_child(tk::create_widget<tk::KeyboardTarget>(this));
    avatar_target_->set_accessible_name(tk::tr("View avatar"));
    avatar_target_->set_focus_ring_radius(kAvatarD * 0.5f);
    avatar_target_->on_activate = [this]
    {
        if (on_avatar_clicked && !avatar_url_.empty())
            on_avatar_clicked(avatar_url_, display_name_);
    };

    close_btn_ = add_child(
        tk::create_widget<tk::Button>(this, tk::tr("\xC3\x97"), std::function<void()>{},
                                     tk::Button::Variant::Icon));
    dm_btn_ = add_child(
        tk::create_widget<tk::Button>(this, tk::tr("Message"), std::function<void()>{},
                                     tk::Button::Variant::Primary));
    ignore_btn_ = add_child(
        tk::create_widget<tk::Button>(this, tk::tr("Ignore"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    verify_btn_ = add_child(
        tk::create_widget<tk::Button>(this, tk::tr("Verify"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    verify_btn_->set_visible(false);
    verify_btn_->set_on_click([this]()
    {
        if (trust_ == tesseract::UserTrust::VerificationViolation)
        {
            if (on_withdraw_verification) on_withdraw_verification(user_id_);
        }
        else if (on_verify_user)
        {
            on_verify_user(user_id_, display_name_);
        }
    });

    {
        auto list = tk::create_widget<tk::ListView>(this);
        list->set_adapter(common_adapter_.get());
        list->on_row_clicked = [this](int idx)
        {
            if (idx >= 0 && static_cast<std::size_t>(idx) < common_rooms_.size() &&
                on_common_room_clicked)
            {
                on_common_room_clicked(common_rooms_[static_cast<std::size_t>(idx)].room_id);
            }
        };
        list->on_near_bottom = [this]()
        {
            if (common_has_more_ && on_common_rooms_near_end)
            {
                on_common_rooms_near_end();
            }
        };
        list->set_visible(false);
        common_list_ = add_child(std::move(list));
    }

    close_btn_->set_accessible_name(tk::tr("Close"));
    close_btn_->set_on_click([this]() { if (on_close) on_close(); });
    dm_btn_->set_on_click([this]() { if (on_open_dm) on_open_dm(user_id_); });
    ignore_btn_->set_on_click([this]() { if (on_ignore) on_ignore(user_id_); });

    // Closed-by-default overlay. Tie widget visibility to the open state so
    // the Widget tree's hit-test walks past us entirely when closed — the
    // dm_btn_ and ignore_btn_ children are otherwise visible-by-default
    // (Widget contract) and would capture clicks at their last-arranged rects
    // even though paint() draws nothing.
    set_visible(false);
}

// ── public API ────────────────────────────────────────────────────────────

void UserProfilePanel::open(std::string user_id, std::string display_name,
                             std::string avatar_url)
{
    const bool was_open = open_;
    user_id_      = std::move(user_id);
    display_name_ = std::move(display_name);
    avatar_url_   = std::move(avatar_url);
    open_         = true;
    set_visible(true);
    name_layout_.reset();
    uid_layout_.reset();

    // Reset DM button to Normal or HasDM each time the panel opens for a
    // user, so stale Sending state from a prior open doesn't leak through.
    const bool has_dm = on_check_has_dm && on_check_has_dm(user_id_);
    set_dm_button_state(has_dm ? DmButtonState::HasDM : DmButtonState::Normal);

    // Clear extended profile from any previous open; shell will repopulate async.
    ext_profile_ = {};
    pronouns_label_layout_.reset(); pronouns_value_layout_.reset();
    tz_label_layout_.reset();       tz_value_layout_.reset();
    bio_label_layout_.reset();      bio_value_layout_.reset();
    if (on_extended_profile_requested)
        on_extended_profile_requested(user_id_);

    trust_ = tesseract::UserTrust::Unknown;
    apply_trust_(false);
    if (on_trust_requested)
        on_trust_requested(user_id_);

    // Drop the previous user's rooms; the shell repopulates async.
    common_rooms_.clear();
    common_avatar_requested_.clear();
    common_total_    = 0;
    common_has_more_ = false;
    common_list_->set_visible(false);
    common_list_->invalidate_data();
    if (on_common_rooms_requested)
        on_common_rooms_requested(user_id_);

    // Tells the shell to re-query rect accessors so the compose textarea +
    // room-search NativeTextField overlays hide while the panel is up.
    if (!was_open && on_layout_changed) on_layout_changed();
}

void UserProfilePanel::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    if (was_open && on_layout_changed) on_layout_changed();
}

void UserProfilePanel::set_avatar_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void UserProfilePanel::set_dm_button_state(DmButtonState state)
{
    if (!dm_btn_) return;
    switch (state)
    {
    case DmButtonState::Normal:
        dm_btn_->set_label(tk::tr("Message"));
        dm_btn_->set_enabled(true);
        break;
    case DmButtonState::HasDM:
        dm_btn_->set_label(tk::tr("Open DM"));
        dm_btn_->set_enabled(true);
        break;
    case DmButtonState::Sending:
        dm_btn_->set_label(tk::tr("Sending\xe2\x80\xa6")); // "Sending…"
        dm_btn_->set_enabled(false);
        break;
    }
}

std::string UserProfilePanel::dm_button_label() const
{
    return dm_btn_ ? dm_btn_->label() : std::string{};
}

bool UserProfilePanel::dm_button_enabled() const
{
    return dm_btn_ ? dm_btn_->enabled() : false;
}

void UserProfilePanel::set_extended_profile(const tesseract::ExtendedProfile& profile)
{
    if (!open_) return;
    ext_profile_ = profile;
    pronouns_label_layout_.reset(); pronouns_value_layout_.reset();
    tz_label_layout_.reset();       tz_value_layout_.reset();
    bio_label_layout_.reset();      bio_value_layout_.reset();
    status_label_layout_.reset();   status_value_layout_.reset();
    call_label_layout_.reset();     call_value_layout_.reset();
    // Trigger a repaint via the layout-changed chain — same mechanism used
    // everywhere else in this panel to invalidate the surface from shared code.
    if (on_layout_changed) on_layout_changed();
}

void UserProfilePanel::set_common_rooms_supported(bool supported)
{
    if (common_supported_ == supported)
    {
        return;
    }
    common_supported_ = supported;
    if (open_ && on_layout_changed)
    {
        on_layout_changed();
    }
}

void UserProfilePanel::set_common_rooms(std::vector<CommonRoom> rooms, int total,
                                        bool has_more)
{
    if (!open_)
    {
        return;
    }
    common_rooms_    = std::move(rooms);
    common_total_    = total;
    common_has_more_ = has_more;
    common_list_->invalidate_data(/*container_may_resize=*/true);
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

void UserProfilePanel::set_trust(tesseract::UserTrust trust, bool can_verify)
{
    if (!open_) return;
    trust_ = trust;
    apply_trust_(can_verify);
    if (on_layout_changed) on_layout_changed();
}

std::string UserProfilePanel::trust_tooltip_text_() const
{
    switch (trust_)
    {
    case tesseract::UserTrust::Verified:
        return tk::tr("Verified");
    case tesseract::UserTrust::VerificationViolation:
        return tk::tr("Verified digital identity was reset");
    case tesseract::UserTrust::NotVerified:
    case tesseract::UserTrust::Unknown:
        break;
    }
    return {};
}

std::string UserProfilePanel::verify_tooltip_text_() const
{
    if (trust_ != tesseract::UserTrust::NotVerified)
    {
        return {};
    }
    return can_verify_ ? tk::tr("Not verified")
                       : tk::tr("Not verified \xc2\xb7 confirm this device first");
}

void UserProfilePanel::apply_trust_(bool can_verify)
{
    using tesseract::UserTrust;
    can_verify_ = can_verify;
    name_layout_.reset(); // its width depends on whether the shield is shown
    bool button = false;
    switch (trust_)
    {
    case UserTrust::Verified:
        break;
    case UserTrust::NotVerified:
        verify_btn_->set_label(tk::tr("Verify"));
        verify_btn_->set_enabled(can_verify);
        button = true;
        break;
    case UserTrust::VerificationViolation:
        verify_btn_->set_label(tk::tr("Withdraw verification"));
        verify_btn_->set_enabled(true);
        button = true;
        break;
    case UserTrust::Unknown:
        break;
    }
    verify_btn_->set_visible(button);
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size UserProfilePanel::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void UserProfilePanel::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);
    backdrop_rect_ = bounds;

    constexpr float kNameH    = 20.0f; // estimated single-line title height
    constexpr float kUidH     = 16.0f; // estimated small-font line height

    // Text rows (painted directly — no Label children, just TextLayouts).
    // Invalidate caches so layout_ext_rows_()/paint() rebuild them at the
    // correct width/content.
    name_layout_.reset();
    uid_layout_.reset();
    common_header_layout_.reset();
    pronouns_label_layout_.reset(); pronouns_value_layout_.reset();
    tz_label_layout_.reset();       tz_value_layout_.reset();
    bio_label_layout_.reset();      bio_value_layout_.reset();
    status_label_layout_.reset();   status_value_layout_.reset();
    call_label_layout_.reset();     call_value_layout_.reset();

    const float left_x  = kPadX; // card-relative
    const float right_x = kPadX + kLeftW + kPadX;
    const float right_w = kCardW - right_x - kPadX;

    // Right column: extended profile fields (MSC4133/MSC4426), measured at
    // their real wrapped heights, then the rooms-in-common section.
    const float ext_max_val_w = right_w - kExtLabelW - kExtFieldGap;
    const float ext_section_h = layout_ext_rows_(lc, ext_max_val_w);
    const bool  common        = common_section_visible_();
    const float common_top    = kHeaderH + ext_section_h + (ext_section_h > 0.0f ? kPadY : 0.0f);
    const float right_h       = common ? common_top + kRoomsHeaderH + kRoomsListMinH + kPadY
                                       : kHeaderH + ext_section_h + kPadY;

    // Left column: avatar, name + shield, MXID, then the buttons.
    const bool  verify_button = verify_btn_ && verify_btn_->visible();
    const float left_h = kHeaderH
                       + kAvatarD + kPadY
                       + kNameH   + kPadY * 0.5f
                       + kUidH    + kPadY
                       + (verify_button ? kButtonH + kPadY * 0.5f : 0.0f)
                       + kButtonH + kPadY * 0.5f
                       + kButtonH + kPadY;

    const float card_h = std::min(std::max(left_h, right_h),
                                  std::max(0.0f, bounds.h - kCardMargin * 2.0f));

    // Centre the card in the available space.
    const float card_x = bounds.x + (bounds.w - kCardW) * 0.5f;
    const float card_y = bounds.y + (bounds.h - card_h) * 0.5f;
    card_rect_ = {card_x, card_y, kCardW, card_h};

    // Close button: 32×32 anchored to top-right of card with 4px margin.
    constexpr float kCloseSz = 32.0f;
    if (close_btn_)
    {
        close_btn_->arrange(lc, {card_rect_.x + card_rect_.w - kCloseSz - 4.0f,
                                  card_rect_.y + 4.0f, kCloseSz, kCloseSz});
    }

    // Avatar circle centred in the left column below the header row.
    const float av_x = card_rect_.x + left_x + (kLeftW - kAvatarD) * 0.5f;
    const float av_y = card_rect_.y + kHeaderH;
    avatar_rect_     = {av_x, av_y, kAvatarD, kAvatarD};
    const tk::Rect avatar_hit =
        !avatar_url_.empty() && on_avatar_clicked ? avatar_rect_ : tk::Rect{};
    avatar_target_->arrange(lc, avatar_hit);
    avatar_area_->arrange(lc, avatar_hit);

    // Buttons: full left-column width, stacked below the name and MXID.
    const float btn_x = card_rect_.x + left_x;
    float btn_y = av_y + kAvatarD + kPadY
                + kNameH + kPadY * 0.5f
                + kUidH  + kPadY;
    if (verify_button)
    {
        verify_btn_->arrange(lc, {btn_x, btn_y, kLeftW, kButtonH});
        btn_y += kButtonH + kPadY * 0.5f;
    }
    if (dm_btn_)
    {
        dm_btn_->arrange(lc, {btn_x, btn_y, kLeftW, kButtonH});
    }
    btn_y += kButtonH + kPadY * 0.5f;
    if (ignore_btn_)
    {
        ignore_btn_->arrange(lc, {btn_x, btn_y, kLeftW, kButtonH});
    }

    // Rooms in common: header, then a list filling the rest of the column.
    common_list_->set_visible(common);
    if (common)
    {
        const float col_x = card_rect_.x + right_x;
        const float hdr_y = card_rect_.y + common_top;
        common_header_rect_ = {col_x, hdr_y, right_w, kRoomsHeaderH};
        const float list_y = hdr_y + kRoomsHeaderH;
        const float list_h = std::max(0.0f, card_rect_.y + card_rect_.h - kPadY - list_y);
        common_list_->arrange(lc, {col_x, list_y, right_w, list_h});
    }
    else
    {
        common_header_rect_ = {};
    }
}

float UserProfilePanel::layout_ext_rows_(tk::LayoutCtx& lc, float max_val_w)
{
    // Multiple `m.pronouns` entries (MSC4247) collapse to the single entry
    // that best matches the app's current UI language for display here.
    const auto* pronoun_entry =
        select_pronoun_entry_for_locale(ext_profile_.pronouns, tk::current_locale());
    const std::string pronouns_display = pronoun_entry ? pronoun_entry->summary : std::string();

    // MSC4426: "<emoji>  <text>" (either part may be absent). Plain text — the
    // MSC asks clients not to linkify status text.
    std::string status_display = ext_profile_.status_emoji;
    if (!status_display.empty() && !ext_profile_.status_text.empty())
        status_display += "  ";
    status_display += ext_profile_.status_text;

    // MSC4426: m.call presence — shown only while the field is set.
    std::string call_display;
    if (ext_profile_.call_joined_ts != 0)
        call_display = tk::tr("In a call");

    struct ExtRow {
        std::string                      label;
        std::string                      value;
        tk::CachedTextLayout& label_layout;
        tk::CachedTextLayout& value_layout;
    };
    ExtRow ext_rows[] = {
        { tk::tr("Pronouns"), pronouns_display,      pronouns_label_layout_, pronouns_value_layout_ },
        { tk::tr("Timezone"), ext_profile_.tz,        tz_label_layout_,       tz_value_layout_       },
        { tk::tr("Bio"),      ext_profile_.biography, bio_label_layout_,      bio_value_layout_      },
        { tk::tr("Status"),   status_display,        status_label_layout_,   status_value_layout_   },
        { tk::tr("Activity"), call_display,          call_label_layout_,     call_value_layout_     },
    };
    constexpr size_t kExtRowCount = 5;
    static_assert(sizeof(ext_rows) / sizeof(ext_rows[0]) == kExtRowCount);

    float total_h = 0.0f;
    bool  any     = false;

    for (size_t i = 0; i < kExtRowCount; ++i)
    {
        auto& row  = ext_rows[i];
        auto& geom = ext_row_geom_[i];
        geom       = {};
        if (row.value.empty()) continue;

        if (!row.label_layout)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = kExtLabelW;
            row.label_layout = lc.factory.build_text(row.label, st);
        }
        if (!row.value_layout)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Body;
            st.trim      = tk::TextTrim::None;
            st.wrap      = true;
            st.max_width = max_val_w;
            row.value_layout = lc.factory.build_text(row.value, st);
        }
        if (!row.label_layout || !row.value_layout) continue;

        // Cap wrapped values at kExtValueMaxLines (a long bio scrolls off the
        // bottom of the small profile card otherwise); paint() clips to this
        // height so any further lines are silently cut, same as
        // RoomInfoPanel's topic truncation.
        const int   value_lines  = std::max(1, row.value_layout->line_count());
        const float value_line_h = row.value_layout->measure().h / static_cast<float>(value_lines);
        const int   shown_lines  = std::min(value_lines, kExtValueMaxLines);
        const float value_h      = value_line_h * static_cast<float>(shown_lines);
        const float label_h      = row.label_layout->measure().h;

        // Baseline-align the label with the value's first line instead of
        // top-aligning both — label (Small) and value (Body) have different
        // ascents, so a shared top-y otherwise looks visually misaligned.
        const float baseline_dy = std::max(row.label_layout->ascent(),
                                            row.value_layout->ascent());
        geom.h        = std::max(label_h, value_h);
        geom.label_dy = baseline_dy - row.label_layout->ascent();
        geom.value_dy = baseline_dy - row.value_layout->ascent();

        if (any) total_h += kExtRowGap;
        total_h += geom.h;
        any = true;
    }

    return any ? (kExtFieldPadTop + total_h) : 0.0f;
}

// ── paint ─────────────────────────────────────────────────────────────────

// Intentional paint() override: close_icon_.draw() (own drawing) is
// genuinely interleaved between close_btn_->paint() and dm_btn_->paint(),
// so this can't be expressed as strictly-before/strictly-after chrome.
void UserProfilePanel::paint(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }

    auto& cv         = ctx.canvas;
    const auto& pal  = ctx.theme.palette;

    // Semi-transparent backdrop over the whole surface.
    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 100});

    // Card background + border.
    constexpr float kCornerR = 8.0f;
    cv.fill_rounded_rect(card_rect_, kCornerR, pal.bg);
    cv.stroke_rounded_rect(card_rect_, kCornerR, pal.popup_border, 1.0f);

    // Avatar.
    const tk::Point av_centre{avatar_rect_.x + kAvatarD * 0.5f,
                               avatar_rect_.y + kAvatarD * 0.5f};
    const tk::Image* av_img = nullptr;
    if (!avatar_url_.empty())
    {
        if (image_provider_)
        {
            av_img = image_provider_(avatar_url_);
        }
        // image_provider_ falls back to the small shared thumbnail while the
        // full-res fetch is in flight, so it can return non-null well before
        // the actual full-res image is ready — fire once per avatar URL
        // rather than gating on av_img being null.
        if (on_avatar_needed && fullres_requested_for_ != avatar_url_)
        {
            fullres_requested_for_ = avatar_url_;
            on_avatar_needed(avatar_url_);
        }
    }
    {
        std::string_view disp =
            display_name_.empty() ? std::string_view("?")
                                  : std::string_view(display_name_);
        draw_avatar(cv, av_img, av_centre, kAvatarD, disp, pal.accent,
                    tk::accent_foreground(ctx.theme));
    }
    avatar_area_->paint(ctx); // hover ring over the avatar

    // Display name (Title, ellipsis) with the trust shield on the same line,
    // centred in the left column.
    constexpr float kNameH = 20.0f;
    constexpr float kUidH  = 16.0f;
    const float left_x     = card_rect_.x + kPadX;
    const float name_y     = avatar_rect_.y + kAvatarD + kPadY;
    const bool  shield     = trust_icon_visible_();
    const float name_max_w = kLeftW - (shield ? kTrustIconSz + kTrustIconGap : 0.0f);

    if (!name_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = name_max_w;
        name_layout_ = ctx.factory.build_text(
            display_name_.empty() ? user_id_ : display_name_, st);
    }
    trust_icon_rect_ = {};
    if (name_layout_)
    {
        const tk::Size sz = name_layout_->measure();
        const float total_w = sz.w + (shield ? kTrustIconGap + kTrustIconSz : 0.0f);
        const float tx      = left_x + (kLeftW - total_w) * 0.5f;
        cv.draw_text(*name_layout_, {tx, name_y}, pal.text_primary);
        if (shield)
        {
            trust_icon_rect_ = {tx + sz.w + kTrustIconGap,
                                name_y + (kNameH - kTrustIconSz) * 0.5f,
                                kTrustIconSz, kTrustIconSz};
            const bool ok = trust_ == tesseract::UserTrust::Verified;
            trust_icon_.draw(cv, ctx.factory,
                             ok ? std::span<const std::uint8_t>(kShieldCheckSvg)
                                : std::span<const std::uint8_t>(kShieldAlertSvg),
                             trust_icon_rect_, kTrustIconSz,
                             ok ? pal.success : pal.destructive);
        }
    }

    // User ID (Small, centred, ellipsis, muted colour).
    const float uid_y = name_y + kNameH + kPadY * 0.5f;

    if (!uid_layout_)
    {
        tk::TextStyle st{};
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = kLeftW;
        uid_layout_  = ctx.factory.build_text(user_id_, st);
    }
    if (uid_layout_)
    {
        const tk::Size sz = uid_layout_->measure();
        const float tx    = left_x + (kLeftW - sz.w) * 0.5f;
        cv.draw_text(*uid_layout_, {tx, uid_y}, pal.text_muted);
    }

    // Extended profile fields (MSC4133/MSC4426) — label+value rows at the top
    // of the right column. Geometry (heights, baseline offsets, line-wrap cap)
    // is computed once in arrange() by layout_ext_rows_() and cached in
    // ext_row_geom_; paint() just draws the cached layouts at those offsets.
    const float right_x       = card_rect_.x + kPadX + kLeftW + kPadX;
    const float right_w       = card_rect_.x + kCardW - kPadX - right_x;
    const float ext_max_val_w = right_w - kExtLabelW - kExtFieldGap;

    struct ExtRowPaint {
        tk::CachedTextLayout& label_layout;
        tk::CachedTextLayout& value_layout;
    };
    ExtRowPaint ext_rows[] = {
        { pronouns_label_layout_, pronouns_value_layout_ },
        { tz_label_layout_,       tz_value_layout_        },
        { bio_label_layout_,      bio_value_layout_       },
        { status_label_layout_,  status_value_layout_     },
        { call_label_layout_,    call_value_layout_       },
    };

    float ext_y = card_rect_.y + kHeaderH + kExtFieldPadTop;
    const float label_x = right_x;
    const float value_x = label_x + kExtLabelW + kExtFieldGap;

    pronouns_row_rect_ = {};

    for (size_t i = 0; i < 5; ++i)
    {
        auto& row  = ext_rows[i];
        auto& geom = ext_row_geom_[i];
        if (!row.value_layout || geom.h <= 0.0f) continue;

        if (i == 0) // Pronouns is always ext_rows[0]
        {
            pronouns_row_rect_ = {label_x, ext_y,
                                   kExtLabelW + kExtFieldGap + ext_max_val_w,
                                   geom.h};
        }

        if (row.label_layout)
        {
            cv.draw_text(*row.label_layout, {label_x, ext_y + geom.label_dy},
                         pal.text_secondary);
        }

        // Clip to the (possibly wrap-capped) row height so any lines beyond
        // kExtValueMaxLines are silently cut, same as RoomInfoPanel's topic.
        {
            tk::ClipScope clip(cv, {value_x, ext_y, ext_max_val_w, geom.h});
            cv.draw_text(*row.value_layout, {value_x, ext_y + geom.value_dy},
                         pal.text_primary);
        }

        ext_y += geom.h + kExtRowGap;
    }

    // Rooms in common: header, then the list.
    if (common_section_visible_())
    {
        if (!common_header_layout_)
        {
            tk::TextStyle st{};
            st.role      = tk::FontRole::Small;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = common_header_rect_.w;
            common_header_layout_ = ctx.factory.build_text(tk::tr("Rooms in common"), st);
        }
        if (common_header_layout_)
        {
            const tk::Size sz = common_header_layout_->measure();
            cv.draw_text(*common_header_layout_,
                         {common_header_rect_.x,
                          common_header_rect_.y + (common_header_rect_.h - sz.h) * 0.5f},
                         pal.text_secondary);
        }
        common_list_->paint(ctx);
    }

    // Child buttons.
    if (close_btn_)
    {
        close_btn_->paint(ctx);
        close_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg,
                         close_btn_->bounds(), 16.0f,
                         ctx.theme.palette.text_secondary);
    }
    if (verify_btn_ && verify_btn_->visible()) verify_btn_->paint(ctx);
    if (dm_btn_)     dm_btn_->paint(ctx);
    if (ignore_btn_) ignore_btn_->paint(ctx);
}

// ── pointer events ────────────────────────────────────────────────────────

bool UserProfilePanel::on_pointer_down(tk::Point local)
{
    if (!open_)
    {
        return false;
    }

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    if (rect_contains(card_rect_, w))
    {
        // Let the child dispatch (buttons) handle events inside the card.
        return false;
    }

    // Backdrop click: consume and remember for on_pointer_up.
    press_backdrop_ = true;
    return true;
}

void UserProfilePanel::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
    {
        return;
    }
    press_backdrop_ = false;

    if (inside_self)
    {
        const tk::Point w{local.x + bounds().x, local.y + bounds().y};
        if (!rect_contains(card_rect_, w))
        {
            if (on_close)
            {
                on_close();
            }
        }
    }
}

std::string UserProfilePanel::pronouns_tooltip_text_() const
{
    std::string out;
    for (const auto& e : ext_profile_.pronouns)
    {
        if (!out.empty()) out += '\n';
        out += bcp47_language_name(e.language);
        out += ": ";
        out += e.summary;
    }
    return out;
}

bool UserProfilePanel::on_pointer_move(tk::Point local)
{
    if (!open_) return false;

    const tk::Point w{local.x + bounds().x, local.y + bounds().y};

    // Pronouns tooltip: list every configured language/pronoun pair when
    // there's more than one (the row itself only shows the one matching the
    // app's current language). Mirrors RoomInfoPanel's topic tooltip.
    const bool over_pronouns = ext_profile_.pronouns.size() > 1 &&
                                rect_contains(pronouns_row_rect_, w);
    if (over_pronouns && !hover_pronouns_)
    {
        hover_pronouns_ = true;
        if (host()) host()->show_tooltip(this, pronouns_tooltip_text_(), pronouns_row_rect_);
    }
    else if (!over_pronouns && hover_pronouns_)
    {
        hover_pronouns_ = false;
        if (host()) host()->hide_tooltip(this);
    }

    // Trust shield tooltip (the shield is the only trust indicator).
    const bool over_trust = trust_icon_visible_() && rect_contains(trust_icon_rect_, w);
    if (over_trust && !hover_trust_)
    {
        hover_trust_ = true;
        if (host()) host()->show_tooltip(this, trust_tooltip_text_(), trust_icon_rect_);
    }
    else if (!over_trust && hover_trust_)
    {
        hover_trust_ = false;
        if (host()) host()->hide_tooltip(this);
    }

    // Verify button tooltip: why it is offered / disabled.
    const bool over_verify = verify_btn_ && verify_btn_->visible() &&
                             trust_ == tesseract::UserTrust::NotVerified &&
                             rect_contains(verify_btn_->bounds(), w);
    if (over_verify && !hover_verify_)
    {
        hover_verify_ = true;
        if (host()) host()->show_tooltip(this, verify_tooltip_text_(), verify_btn_->bounds());
    }
    else if (!over_verify && hover_verify_)
    {
        hover_verify_ = false;
        if (host()) host()->hide_tooltip(this);
    }

    return false;
}

void UserProfilePanel::on_pointer_leave()
{
    press_backdrop_ = false;
    if ((hover_pronouns_ || hover_trust_ || hover_verify_) && host())
        host()->hide_tooltip(this);
    hover_pronouns_ = false;
    hover_trust_    = false;
    hover_verify_   = false;
}

} // namespace tesseract::views
