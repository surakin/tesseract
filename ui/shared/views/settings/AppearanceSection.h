#pragma once

// Settings panel section: theme selection.
// Renders three mutually exclusive radio-style buttons — "Light", "Dark",
// "System" — and fires on_theme_changed when the selection changes.

#include "tk/widget.h"
#include "tesseract/settings.h"

#include <functional>
#include <memory>

namespace tesseract::views
{

class AppearanceSection : public tk::Widget
{
public:
    AppearanceSection();
    ~AppearanceSection() override = default;

    // Silently update the displayed selection without firing on_theme_changed.
    void set_selected(tesseract::Settings::ThemePreference pref);

    // Fires with the newly selected preference when the user picks a button.
    std::function<void(tesseract::Settings::ThemePreference)> on_theme_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    // The three selectable options in display order.
    static constexpr int kButtonCount = 3;

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

    int hovered_idx_ = -1; // index of the button under the pointer, or -1
    int pressed_idx_ = -1; // index of the button being pressed, or -1

    // Return the index of the button whose bounds contain `local`, or -1.
    int hit_button(tk::Point local) const;
};

} // namespace tesseract::views
