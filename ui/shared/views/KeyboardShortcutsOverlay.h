#pragma once

// Read-only modal list of every keyboard shortcut, grouped (General,
// Navigation, Messages, ...). Opened with Ctrl+/ / Cmd+/ or F1, or from the
// user menu (macOS: Help menu). Its content comes entirely from
// shortcut_registry.h, so a shortcut added there shows up here without
// touching this view. Mounted on MainAppWidget's overlay stack with the
// same closed-by-default idiom as ConfirmDialog: visibility tracks the open
// state so an idle overlay never captures hit-tests.

#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <memory>

namespace tk
{
class ScrollView;
class TextLayout;
} // namespace tk

namespace tesseract::views
{

class KeyboardShortcutsOverlay : public tk::Widget
{
protected:
    KeyboardShortcutsOverlay();
    TK_WIDGET_FACTORY_FRIEND(KeyboardShortcutsOverlay)

public:
    ~KeyboardShortcutsOverlay() override;

    void open();
    void close();
    bool is_open() const
    {
        return open_;
    }

    // Fires when the overlay opens or closes, so the shells re-query rect
    // accessors and hide the native text controls it covers.
    std::function<void()> on_layout_changed;

    // The scrolled list of groups and rows (tests, accessibility).
    tk::ScrollView* scroll_view() const
    {
        return scroll_;
    }
    tk::Button* close_button() const
    {
        return close_btn_;
    }

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    // Scrolls the list (Up/Down, Page Up/Down, Home/End) while it's open.
    bool on_key_down(const tk::KeyEvent& event) override;
    // Swallows the wheel while open, so nothing behind the modal scrolls.
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;
    void on_theme_changed(const tk::Theme& t) override;

    tk::Role access_role() const override
    {
        return tk::Role::Dialog;
    }
    std::string access_name() const override;
    bool access_modal() const override
    {
        return open_ && visible();
    }

private:
    void build_rows_();

    bool open_ = false;
    bool press_backdrop_ = false;
    // Set by open(); the next paint focuses the Close button (see
    // paint_before_children).
    bool pending_focus_ = false;

    tk::ScrollView* scroll_ = nullptr; // owned via add_child
    tk::Button* close_btn_ = nullptr;  // owned via add_child

    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};
    tk::CachedTextLayout title_layout_;
    float title_w_ = -1.0f; // max width title_layout_ was built for

    static constexpr float kCardMaxW = 600.0f;
    static constexpr float kCardMaxH = 640.0f;
    static constexpr float kMargin = 24.0f;
    static constexpr float kCardPad = 20.0f;
    static constexpr float kTitleH = 28.0f;
    static constexpr float kGap = 12.0f;
    static constexpr float kBtnH = 36.0f;
};

} // namespace tesseract::views
