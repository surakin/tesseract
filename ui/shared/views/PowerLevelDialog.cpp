#include "PowerLevelDialog.h"

#include "media_utils.h" // draw_avatar, rect_contains
#include "power_level_presets.h"

#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tesseract::views
{

namespace
{

// Largest integer canonical JSON (and so the power_levels event) allows.
constexpr std::int64_t kPowerLevelJsonMax = 9007199254740991LL; // 2^53 − 1
constexpr const char* kPowerLevelCustomValue = "custom";

} // namespace

std::optional<std::int64_t> parse_power_level(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    bool negative = false;
    if (!text.empty() && text.front() == '-')
    {
        negative = true;
        text.remove_prefix(1);
    }
    // 2^53 − 1 has 16 digits; anything longer is out of range (and capping
    // the length keeps the accumulation below from overflowing).
    if (text.empty() || text.size() > 16)
    {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    if (value > kPowerLevelJsonMax)
    {
        return std::nullopt;
    }
    return negative ? -value : value;
}

PowerLevelDialog::PowerLevelDialog()
{
    title_label_ = add_child(
        tk::create_widget<tk::Label>(this, tk::tr("Change power level"), tk::FontRole::Title));
    title_label_->set_trim(tk::TextTrim::Ellipsis);

    name_label_ = add_child(tk::create_widget<tk::Label>(this, "", tk::FontRole::Title));
    name_label_->set_trim(tk::TextTrim::Ellipsis);
    mxid_label_ = add_child(tk::create_widget<tk::Label>(this, "", tk::FontRole::Caption));
    mxid_label_->set_trim(tk::TextTrim::Ellipsis);
    room_label_ = add_child(tk::create_widget<tk::Label>(this, "", tk::FontRole::Caption));
    room_label_->set_trim(tk::TextTrim::Ellipsis);

    auto form = tk::create_widget<tk::FormLayout>(this);
    level_combo_ =
        form->add_row(tk::tr("Power level"), tk::create_widget<tk::ComboBox>(form.get()));
    level_combo_->on_changed = [this](const std::string&)
    {
        // refresh_() first: it reveals the custom field, and Host drops a
        // focus request for a widget that isn't visible yet.
        refresh_();
        if (is_custom_())
        {
            custom_field_->set_focused(true);
        }
    };
    form_ = add_child(std::move(form));

    auto field = tk::create_widget<tk::TextField>(this, kFieldH);
    field->set_placeholder(tk::tr("Custom power level"));
    field->set_accessible_name(tk::tr("Custom power level"));
    field->set_on_changed(
        [this](const std::string&)
        {
            refresh_();
        });
    field->set_on_submit(
        [this]
        {
            save_();
        });
    field->push_popup_nav(
        [this](tk::NavKey k)
        {
            if (k != tk::NavKey::Escape)
            {
                return false;
            }
            close();
            return true;
        });
    field->set_visible(false);
    custom_field_ = add_child(std::move(field));

    error_label_ = add_child(tk::create_widget<tk::Label>(this, "", tk::FontRole::Body));
    error_label_->set_tone(tk::Label::Tone::Error).set_wrap(true);
    error_label_->set_visible(false);
    warning_label_ = add_child(tk::create_widget<tk::Label>(
        this, tk::tr("You won't be able to undo this: they'll have the same power level as you."),
        tk::FontRole::Body));
    warning_label_->set_tone(tk::Label::Tone::Error).set_wrap(true);
    warning_label_->set_visible(false);

    cancel_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Cancel"),
        [this]
        {
            close();
        },
        tk::Button::Variant::Subtle));
    save_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Save"),
        [this]
        {
            save_();
        },
        tk::Button::Variant::Primary));

    // Closed by default; hidden so hit-testing walks past it when idle.
    set_visible(false);
}

void PowerLevelDialog::set_avatar_provider(ImageProvider p)
{
    avatar_provider_ = std::move(p);
}

std::string PowerLevelDialog::access_name() const
{
    return tk::tr("Change power level");
}

void PowerLevelDialog::open(Target t)
{
    const bool was_open = open_;
    target_ = std::move(t);
    open_ = true;
    press_backdrop_ = false;
    set_visible(true);

    name_label_->set_text(target_.display_name.empty() ? target_.user_id : target_.display_name);
    mxid_label_->set_text(target_.user_id);
    room_label_->set_text(tk::trf(tk::tr("in {0}"), {target_.room_name}));

    auto opts = power_level_preset_options(target_.own_level);
    opts.push_back({tk::tr("Custom"), kPowerLevelCustomValue});
    const bool preset_offered =
        std::any_of(opts.begin(), opts.end(),
                    [&](const tk::ComboBox::Option& o)
                    {
                        return o.value == std::to_string(target_.current_level);
                    });
    level_combo_->set_options(std::move(opts));
    if (is_power_level_preset(target_.current_level) && preset_offered)
    {
        level_combo_->set_selected_value(std::to_string(target_.current_level));
        custom_field_->set_text("");
    }
    else
    {
        level_combo_->set_selected_value(kPowerLevelCustomValue);
        custom_field_->set_text(std::to_string(target_.current_level));
    }
    pending_focus_ = true;
    refresh_();

    if (!was_open && on_layout_changed)
    {
        on_layout_changed();
    }
}

void PowerLevelDialog::close()
{
    if (!open_)
    {
        return;
    }
    open_ = false;
    pending_focus_ = false;
    press_backdrop_ = false;
    level_combo_->collapse();
    custom_field_->set_focused(false);
    custom_field_->set_visible(false);
    set_visible(false);
    if (on_layout_changed)
    {
        on_layout_changed();
    }
    if (on_close)
    {
        on_close();
    }
}

// ── State ──────────────────────────────────────────────────────────────────

bool PowerLevelDialog::is_custom_() const
{
    return level_combo_->selected_value() == kPowerLevelCustomValue;
}

std::optional<std::int64_t> PowerLevelDialog::chosen_level() const
{
    if (is_custom_())
    {
        return parse_power_level(custom_field_->text());
    }
    return parse_power_level(level_combo_->selected_value());
}

bool PowerLevelDialog::error_shown() const
{
    const auto level = chosen_level();
    return level && *level > target_.own_level;
}

bool PowerLevelDialog::save_enabled() const
{
    const auto level = chosen_level();
    return open_ && level && *level <= target_.own_level && *level != target_.current_level;
}

bool PowerLevelDialog::same_level_warning_shown() const
{
    const auto level = chosen_level();
    return target_.own_level != kCreatorLevel && level && *level == target_.own_level;
}

void PowerLevelDialog::refresh_()
{
    const bool custom = is_custom_();
    const bool error = error_shown();
    const bool warn = same_level_warning_shown();
    const bool geometry_changed = custom_field_->own_visible() != custom ||
                                  error_label_->own_visible() != error ||
                                  warning_label_->own_visible() != warn;

    custom_field_->set_visible(open_ && custom);
    if (error)
    {
        error_label_->set_text(tk::trf(tk::tr("Can't be higher than your own power level ({0})."),
                                       {std::to_string(target_.own_level)}));
    }
    error_label_->set_visible(error);
    warning_label_->set_visible(warn);
    save_btn_->set_enabled(save_enabled());

    if (open_ && geometry_changed && on_layout_changed)
    {
        on_layout_changed();
    }
}

void PowerLevelDialog::save_()
{
    if (!save_enabled())
    {
        return;
    }
    const std::int64_t level = *chosen_level();
    Target t = target_;
    auto cb = on_save; // copy: a re-entrant open() from on_close must not lose it
    close();
    if (cb)
    {
        cb(std::move(t.room_id), std::move(t.user_id), std::move(t.display_name), level);
    }
}

// ── Layout ─────────────────────────────────────────────────────────────────

tk::Size PowerLevelDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the surface (backdrop)
}

void PowerLevelDialog::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    const float card_w = std::min(kCardW, bounds.w);
    const float inner_w = std::max(0.0f, card_w - kCardPad * 2.0f);
    const float text_w = std::max(0.0f, inner_w - kAvatar - kAvatarGap);

    const float title_h = title_label_->measure(lc, {inner_w, 0}).h;
    const float name_h = name_label_->measure(lc, {text_w, 0}).h;
    const float mxid_h = mxid_label_->measure(lc, {text_w, 0}).h;
    const float room_h = room_label_->measure(lc, {text_w, 0}).h;
    const float header_h = std::max(kAvatar, name_h + mxid_h + room_h);
    const float form_h = form_->measure(lc, {inner_w, 0}).h;
    const bool custom = custom_field_->own_visible();
    const float error_h =
        error_label_->own_visible() ? error_label_->measure(lc, {inner_w, 0}).h : 0.0f;
    const float warn_h =
        warning_label_->own_visible() ? warning_label_->measure(lc, {inner_w, 0}).h : 0.0f;

    float card_h = kCardPad + title_h + kSectionGap + header_h + kSectionGap + form_h;
    if (custom)
    {
        card_h += kRowGap + kFieldH;
    }
    if (error_h > 0.0f)
    {
        card_h += kRowGap + error_h;
    }
    if (warn_h > 0.0f)
    {
        card_h += kRowGap + warn_h;
    }
    card_h += kSectionGap + kBtnH + kCardPad;
    card_h = std::min(card_h, bounds.h);

    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f, bounds.y + (bounds.h - card_h) * 0.5f,
                  card_w, card_h};

    const float x = card_rect_.x + kCardPad;
    float y = card_rect_.y + kCardPad;

    title_label_->arrange(lc, {x, y, inner_w, title_h});
    y += title_h + kSectionGap;

    avatar_rect_ = {x, y + (header_h - kAvatar) * 0.5f, kAvatar, kAvatar};
    const float tx = x + kAvatar + kAvatarGap;
    float ty = y + (header_h - (name_h + mxid_h + room_h)) * 0.5f;
    name_label_->arrange(lc, {tx, ty, text_w, name_h});
    ty += name_h;
    mxid_label_->arrange(lc, {tx, ty, text_w, mxid_h});
    ty += mxid_h;
    room_label_->arrange(lc, {tx, ty, text_w, room_h});
    y += header_h + kSectionGap;

    form_->arrange(lc, {x, y, inner_w, form_h});
    y += form_h;

    if (custom)
    {
        // Under the combo, in its column.
        const tk::Rect cb = level_combo_->bounds();
        const float fx = cb.w > 0.0f ? cb.x : x;
        const float fw = cb.w > 0.0f ? cb.w : inner_w;
        y += kRowGap;
        custom_field_->arrange(lc, {fx, y, fw, kFieldH});
        y += kFieldH;
    }
    if (error_h > 0.0f)
    {
        y += kRowGap;
        error_label_->arrange(lc, {x, y, inner_w, error_h});
        y += error_h;
    }
    if (warn_h > 0.0f)
    {
        y += kRowGap;
        warning_label_->arrange(lc, {x, y, inner_w, warn_h});
    }

    // Buttons bottom-right: Cancel, Save.
    const float btn_y = card_rect_.y + card_rect_.h - kCardPad - kBtnH;
    const float save_w = std::max(save_btn_->measure(lc, {-1.0f, kBtnH}).w, kBtnMinW);
    const float cancel_w = std::max(cancel_btn_->measure(lc, {-1.0f, kBtnH}).w, kBtnMinW);
    const float save_x = card_rect_.x + card_rect_.w - kCardPad - save_w;
    save_btn_->arrange(lc, {save_x, btn_y, save_w, kBtnH});
    cancel_btn_->arrange(lc, {save_x - kBtnGap - cancel_w, btn_y, cancel_w, kBtnH});
}

void PowerLevelDialog::on_theme_changed(const tk::Theme& t)
{
    tk::Widget::on_theme_changed(t);
    mxid_label_->set_colour(t.palette.text_secondary);
    room_label_->set_colour(t.palette.text_secondary);
    custom_field_->set_text_color(t.palette.text_primary);
}

// ── Paint ──────────────────────────────────────────────────────────────────

void PowerLevelDialog::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }

    if (pending_focus_ && host())
    {
        // Deferred until arranged so a native field overlay has its rect.
        pending_focus_ = false;
        if (is_custom_())
        {
            custom_field_->set_focused(true);
        }
        else
        {
            host()->request_focus(level_combo_);
        }
    }

    auto& cv = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(bounds(), tk::Color{0, 0, 0, 120});
    cv.fill_rounded_rect(card_rect_, 8.0f, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, 8.0f, pal.border, 1.0f);

    const tk::Image* img = nullptr;
    if (avatar_provider_ && !target_.avatar_url.empty())
    {
        img = avatar_provider_(target_.avatar_url);
    }
    const std::string& name = target_.display_name.empty() ? target_.user_id : target_.display_name;
    draw_avatar(cv, img,
                {avatar_rect_.x + avatar_rect_.w * 0.5f, avatar_rect_.y + avatar_rect_.h * 0.5f},
                kAvatar, name, pal.avatar_initials_bg, pal.avatar_initials_text);
}

// ── Input ──────────────────────────────────────────────────────────────────

bool PowerLevelDialog::on_pointer_down(tk::Point local)
{
    if (!open_)
    {
        return false;
    }
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (rect_contains(card_rect_, w))
    {
        return false; // clicks on the card fall through to the child widgets
    }
    press_backdrop_ = true;
    return true;
}

void PowerLevelDialog::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
    {
        return;
    }
    press_backdrop_ = false;
    if (!inside_self)
    {
        return;
    }
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(card_rect_, w))
    {
        close();
    }
}

bool PowerLevelDialog::on_key_down(const tk::KeyEvent& e)
{
    if (!open_)
    {
        return false;
    }
    if (e.key == tk::Key::Escape)
    {
        close();
        return true;
    }
    return false;
}

} // namespace tesseract::views
