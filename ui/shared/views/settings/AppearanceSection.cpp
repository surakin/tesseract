#include "AppearanceSection.h"

#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include "tk/widget.h"

#include <algorithm>
#include <memory>
#include <string>

namespace tesseract::views
{

namespace
{

// Visual constants — the picker no longer paints the "Theme" header itself
// (the enclosing SettingsGroup provides it), so kPadY / kHeaderH / kHeaderGap
// drop out compared with the pre-refactor section.
constexpr float kBtnHPad = 20.0f;      // text → button-edge horizontal inset
constexpr float kBtnVPad = 8.0f;       // text → button-edge vertical inset
constexpr float kBtnSpacing = 8.0f;    // gap between adjacent buttons
constexpr float kBtnMinHeight = 36.0f; // minimum button height
constexpr float kBtnRadius = 6.0f;     // corner radius
constexpr float kGlyphH = 16.0f;       // approximate UiSemibold glyph height

} // namespace

// ---------------------------------------------------------------------------
// ThemePicker — the three-radio row that drives theme selection.
// ---------------------------------------------------------------------------

class AppearanceSection::ThemePicker : public tk::Widget
{
public:
    static constexpr int kButtonCount = 3;

    void set_selected(tesseract::Settings::ThemePreference pref)
    {
        selected_ = pref;
    }

    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    struct RadioButton
    {
        const char* label;
        tesseract::Settings::ThemePreference pref;
        tk::Rect bounds;
        std::unique_ptr<tk::TextLayout> layout;
    };

    RadioButton buttons_[kButtonCount] = {
        {"Light", tesseract::Settings::ThemePreference::Light, {}, nullptr},
        {"Dark", tesseract::Settings::ThemePreference::Dark, {}, nullptr},
        {"System", tesseract::Settings::ThemePreference::System, {}, nullptr},
    };

    tesseract::Settings::ThemePreference selected_ =
        tesseract::Settings::instance().theme_pref;

    int hovered_idx_ = -1;
    int pressed_idx_ = -1;

    int hit_button(tk::Point local) const;
};

tk::Size AppearanceSection::ThemePicker::measure(tk::LayoutCtx&,
                                                 tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    const float btn_h = std::max(kGlyphH + kBtnVPad * 2.0f, kBtnMinHeight);
    return {w, btn_h};
}

void AppearanceSection::ThemePicker::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Measure button labels to give all three the same natural width.
    float max_text_w = 0;
    for (int i = 0; i < kButtonCount; ++i)
    {
        tk::TextStyle st;
        st.role = tk::FontRole::UiSemibold;
        st.max_width = -1.0f;
        if (auto lay = ctx.factory.build_text(buttons_[i].label, st))
        {
            max_text_w = std::max(max_text_w, lay->measure().w);
        }
    }
    const float btn_w = max_text_w + kBtnHPad * 2.0f;
    const float btn_h = std::max(kGlyphH + kBtnVPad * 2.0f, kBtnMinHeight);

    for (int i = 0; i < kButtonCount; ++i)
    {
        buttons_[i].bounds = {
            bounds.x + (btn_w + kBtnSpacing) * i,
            bounds.y,
            btn_w,
            btn_h,
        };
        buttons_[i].layout.reset();
    }
}

void AppearanceSection::ThemePicker::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    for (int i = 0; i < kButtonCount; ++i)
    {
        const auto& btn = buttons_[i];
        const bool is_selected = (btn.pref == selected_);
        const bool is_hovered = (i == hovered_idx_);
        const bool is_pressed = (i == pressed_idx_);

        tk::Color fill;
        if (is_selected)
        {
            if (is_pressed)
            {
                fill = pal.accent_pressed;
            }
            else if (is_hovered)
            {
                fill = pal.accent_hover;
            }
            else
            {
                fill = pal.accent;
            }
        }
        else
        {
            if (is_pressed)
            {
                fill = pal.subtle_pressed;
            }
            else if (is_hovered)
            {
                fill = pal.subtle_hover;
            }
            else
            {
                fill = tk::Color::rgba(0, 0, 0, 0);
            }
        }

        ctx.canvas.fill_rounded_rect(btn.bounds, kBtnRadius, fill);

        if (!is_selected)
        {
            ctx.canvas.stroke_rounded_rect(btn.bounds, kBtnRadius, pal.border);
        }

        if (!buttons_[i].layout)
        {
            tk::TextStyle st;
            st.role = tk::FontRole::UiSemibold;
            st.halign = tk::TextHAlign::Leading;
            st.valign = tk::TextVAlign::Top;
            st.trim = tk::TextTrim::None;
            st.max_width = -1.0f;
            buttons_[i].layout = ctx.factory.build_text(tk::tr(btn.label), st);
        }

        if (buttons_[i].layout)
        {
            const tk::Size sz = buttons_[i].layout->measure();
            const float tx = btn.bounds.x + (btn.bounds.w - sz.w) * 0.5f;
            const float ty = btn.bounds.y + (btn.bounds.h - sz.h) * 0.5f;
            const tk::Color ink =
                is_selected ? pal.text_on_accent : pal.text_primary;
            ctx.canvas.draw_text(*buttons_[i].layout, {tx, ty}, ink);
        }
    }
}

int AppearanceSection::ThemePicker::hit_button(tk::Point local) const
{
    const tk::Point world{local.x + bounds_.x, local.y + bounds_.y};
    for (int i = 0; i < kButtonCount; ++i)
    {
        const tk::Rect& r = buttons_[i].bounds;
        if (world.x >= r.x && world.x < r.x + r.w && world.y >= r.y &&
            world.y < r.y + r.h)
        {
            return i;
        }
    }
    return -1;
}

bool AppearanceSection::ThemePicker::on_pointer_down(tk::Point local)
{
    int idx = hit_button(local);
    if (idx < 0)
    {
        return false;
    }
    pressed_idx_ = idx;
    return true;
}

void AppearanceSection::ThemePicker::on_pointer_up(tk::Point local,
                                                   bool inside_self)
{
    const int was_pressed = pressed_idx_;
    pressed_idx_ = -1;

    if (!inside_self || was_pressed < 0)
    {
        return;
    }

    int idx = hit_button(local);
    if (idx != was_pressed)
    {
        return;
    }

    if (buttons_[idx].pref != selected_)
    {
        selected_ = buttons_[idx].pref;
        if (on_theme_changed)
        {
            on_theme_changed(selected_);
        }
    }
}

bool AppearanceSection::ThemePicker::on_pointer_move(tk::Point local)
{
    int prev = hovered_idx_;
    hovered_idx_ = hit_button(local);
    return hovered_idx_ != prev;
}

void AppearanceSection::ThemePicker::on_pointer_leave()
{
    hovered_idx_ = -1;
    pressed_idx_ = -1;
}

// ---------------------------------------------------------------------------
// AppearanceSection — thin SettingsPage with one "Theme" group.
// ---------------------------------------------------------------------------

AppearanceSection::AppearanceSection()
{
    auto* group = add_group("Theme");
    auto picker = std::make_unique<ThemePicker>();
    picker->on_theme_changed = [this](tesseract::Settings::ThemePreference pref)
    {
        if (on_theme_changed) on_theme_changed(pref);
    };
    picker_ = group->add_widget(std::move(picker));

    {
        const auto& s = tesseract::Settings::instance();
        auto* rl_group = add_group("Room list");

        auto cb = std::make_unique<tk::CheckButton>(
            "Group inactive rooms", s.group_inactive_rooms);
        group_inactive_cb_ = rl_group->add_widget(std::move(cb));
        group_inactive_cb_->on_change = [this](bool v)
        {
            if (on_group_inactive_changed) on_group_inactive_changed(v);
        };

        auto combo = std::make_unique<tk::ComboBox>();
        combo->set_options({
            {"1 week",   "7"},
            {"2 weeks",  "14"},
            {"1 month",  "30"},
            {"3 months", "90"},
            {"6 months", "180"},
        });
        combo->set_selected_value(
            std::to_string(s.inactive_room_threshold_days));
        period_combo_ = rl_group->add_widget(std::move(combo));
        period_combo_->on_changed = [this](std::string value)
        {
            if (on_inactive_period_changed)
            {
                on_inactive_period_changed(std::stoi(value));
            }
        };
    }
}

AppearanceSection::~AppearanceSection() = default;

void AppearanceSection::set_selected(tesseract::Settings::ThemePreference pref)
{
    picker_->set_selected(pref);
}

void AppearanceSection::set_group_inactive(bool enabled)
{
    if (group_inactive_cb_) group_inactive_cb_->set_checked(enabled);
}

void AppearanceSection::set_inactive_period(int days)
{
    if (period_combo_) period_combo_->set_selected_value(std::to_string(days));
}

void AppearanceSection::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    SettingsPage::arrange(ctx, bounds);
    // Constrain the combobox dropdown popup to the page bounds so it does not
    // paint outside the settings panel (mirrors RoomInfoPanel).
    if (period_combo_)
    {
        period_combo_->set_popup_clip(bounds);
    }
}

} // namespace tesseract::views
