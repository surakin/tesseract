#pragma once

// Settings panel section: language selection.
// Renders three mutually exclusive radio-style buttons — "Auto", "English",
// "Spanish" — and fires on_language_changed when the selection changes.
// A note below the buttons informs the user that changes take effect after
// restart.

#include "tk/widget.h"
#include "tesseract/settings.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class LanguageSection : public tk::Widget
{
public:
    LanguageSection();
    ~LanguageSection() override = default;

    // Silently update the displayed selection without firing on_language_changed.
    void set_selected(const std::string& lang);

    // Fires with the newly selected language code when the user picks a button.
    std::function<void(std::string)> on_language_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    static constexpr int kButtonCount = 3;

    struct RadioButton
    {
        const char* label;
        const char* lang_code;
        tk::Rect bounds;
        std::unique_ptr<tk::TextLayout> layout;
    };

    RadioButton buttons_[kButtonCount] = {
        {"Auto", "auto", {}, nullptr},
        {"English", "en", {}, nullptr},
        {"Spanish", "es", {}, nullptr},
    };

    std::string selected_ = tesseract::Settings::instance().language;
    std::unique_ptr<tk::TextLayout> note_layout_;

    int hovered_idx_ = -1; // index of the button under the pointer, or -1
    int pressed_idx_ = -1; // index of the button being pressed, or -1

    // Return the index of the button whose bounds contain `local`, or -1.
    int hit_button(tk::Point local) const;
};

} // namespace tesseract::views
