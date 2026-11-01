#pragma once

// Skin-tone menu for EmojiPicker: a small card with one button per tone
// (default + the five Fitzpatrick variants) of a single emoji, opened by a
// long press / right-click / Shift+Enter on a tone-capable cell.
//
// It is a child of the picker rather than a Host popup of its own — Host has
// one popup slot and the picker already holds it — so the picker routes
// outside clicks and keys to it while it is open (see EmojiPicker).

#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/emoji.h>

#include <array>
#include <functional>
#include <string>

namespace tesseract::views
{

class SkinTonePopover : public tk::Widget
{
protected:
    SkinTonePopover();
    TK_WIDGET_FACTORY_FRIEND(SkinTonePopover)

public:
    static constexpr float kButtonSize = 34.0f;
    static constexpr float kGap = 2.0f;
    static constexpr float kPadding = 4.0f;
    static constexpr float kWidth = kButtonSize * 6 + kGap * 5 + kPadding * 2;
    static constexpr float kHeight = kButtonSize + kPadding * 2;

    /// Show the tones of `glyph` (base or toned), highlighting `current`,
    /// next to `anchor` (the pressed cell's world rect) and inside `limits`
    /// (the picker's world rect).
    void open(std::string_view glyph, tesseract::emoji::SkinTone current, tk::Rect anchor,
              tk::Rect limits);
    void close();
    bool is_open() const
    {
        return open_;
    }

    /// Keyboard while open: Left/Right move, Enter/Space pick, Escape
    /// closes. Returns true when the key was consumed.
    bool handle_key(const tk::KeyEvent& e);

    /// The user picked a tone (the popover has already closed itself).
    std::function<void(tesseract::emoji::SkinTone)> on_picked;

    /// The variant of the open emoji shown for `tone`.
    std::string glyph_for(tesseract::emoji::SkinTone tone) const;

    tk::Size measure(tk::LayoutCtx&, tk::Size) override
    {
        return {kWidth, kHeight};
    }
    void paint(tk::PaintCtx&) override;

    tk::Role access_role() const override
    {
        return tk::Role::Group;
    }
    std::string access_name() const override;

private:
    class ToneButton;

    void pick_(int index);
    void focus_(int index);
    // Keeps focused_ in step with real tk focus (Tab / clicks can move it).
    void sync_focused_from_host_();
    bool inside_owner_(const tk::Widget* w) const;

    std::array<ToneButton*, 6> buttons_{}; // borrowed, kSkinTones order
    int focused_ = 0;
    bool open_ = false;
    // Whether open() moved tk focus into the menu (only when focus was
    // already inside the picker, i.e. the keyboard path), and where from —
    // close() gives it back. Focus outside the picker is never taken or
    // restored: focusing a widget outside the registered popup dismisses it.
    bool took_focus_ = false;
    std::weak_ptr<tk::Widget> prev_focus_;
};

} // namespace tesseract::views
