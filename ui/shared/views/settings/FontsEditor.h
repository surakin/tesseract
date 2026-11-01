#pragma once

// FontsEditor — the "Fonts" group body in Settings → Appearance. A master base
// size (moves every role together, keeping their relations), a UI and a
// monospace family, and per-category size / family overrides, with a live
// preview and "Reset to defaults". Edits are applied immediately through
// on_changed (the shell persists + re-lays-out every window).

#include "tesseract/settings.h"
#include "tk/controls.h"
#include "tk/layout.h"

#include <array>
#include <functional>

namespace tesseract::views
{

class FontFamilyPicker;
class FontPreview;

class FontsEditor : public tk::VBox
{
protected:
    FontsEditor();
    TK_WIDGET_FACTORY_FRIEND(FontsEditor)

public:
    // Silently replace the shown preferences (no on_changed).
    void set_prefs(const tesseract::Settings::FontPrefs& prefs);
    const tesseract::Settings::FontPrefs& prefs() const
    {
        return prefs_;
    }

    // Fired after every user edit with the full new preferences.
    std::function<void(const tesseract::Settings::FontPrefs&)> on_changed;

    // A font change moves the displayed sizes (and the preview).
    void on_fonts_changed() override;

    // Test hooks: press the stepper / reset buttons without hit-testing.
    void step_base_for_test(int dir)
    {
        step_base_(dir);
    }
    void step_category_for_test(std::size_t cat, int dir)
    {
        step_category_(cat, dir);
    }
    void reset_all_for_test()
    {
        reset_all_();
    }

private:
    static constexpr std::size_t kCats = tesseract::Settings::FontPrefs::kCategoryCount;

    struct CategoryRow
    {
        tk::Label* value = nullptr;
        FontFamilyPicker* family = nullptr; // null for the size-only Emoji row
        tk::Button* reset = nullptr;
    };

    void step_base_(int dir);
    void step_category_(std::size_t cat, int dir);
    void reset_category_(std::size_t cat);
    void reset_all_();
    void changed_();
    void refresh_();

    tesseract::Settings::FontPrefs prefs_;
    bool updating_ = false; // refresh_() pushing values into pickers
    tk::Label* base_value_ = nullptr;
    FontFamilyPicker* ui_family_ = nullptr;
    FontFamilyPicker* mono_family_ = nullptr;
    std::array<CategoryRow, kCats> rows_{};
    tk::Button* reset_all_btn_ = nullptr;
    FontPreview* preview_ = nullptr;
};

} // namespace tesseract::views
