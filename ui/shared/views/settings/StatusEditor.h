#pragma once

// StatusEditor — the field-column half of the MSC4426 `m.status` editor in
// Settings › Account: an emoji button (opens the shared EmojiPicker, hosted by
// SettingsView) followed by a free-text TextField. The "Status" label itself
// is drawn by the parent (AccountSection::ExtendedFields), mirroring how the
// "Pronouns" label sits outside PronounsEditor.
//
// Emitting: `on_changed(emoji, text)` fires when the text field is submitted
// (Enter) or loses focus, or when the emoji is picked/cleared, with a value
// that differs from what was last sent — the parent serialises it to the
// `{ "text": ..., "emoji": ... }` wire shape (or a clear when both are empty).
//
// The EmojiPicker is a Host popup and can only live on a widget with overlay
// access, so this widget doesn't own it: an emoji-button click fires
// `on_emoji_button_clicked(world_anchor)` and SettingsView opens the picker,
// pushing the pick back in via `set_emoji()`.

#include "tk/controls.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class StatusEditor : public tk::Widget
{
protected:
    // host() is nullable — mirrors PronounsEditor / ExtendedFields: with no
    // Host (unit tests) no controls are created and the widget renders nothing.
    StatusEditor();
    TK_WIDGET_FACTORY_FRIEND(StatusEditor)

public:
    // Baseline load — pushes values into the controls without firing on_changed.
    void set_status(std::string emoji, std::string text);

    // Apply a just-picked (or cleared, with "") emoji — fires on_changed.
    void set_emoji(std::string glyph);

    std::string status_emoji() const { return emoji_; }
    std::string status_text() const;

    // Trigger widget for Host::register_popup() (so reclicking the button
    // while the picker is open doesn't dismiss-then-reopen it). Null without
    // a Host.
    tk::Widget* emoji_button() const;

    void set_editable(bool editable);
    void set_busy(bool busy);
    void set_error(std::string error);

    // Re-syncs the live text-field value and fires on_changed if the pair
    // differs from the last sent one. Called on the text field losing focus
    // and when the Account tab is switched away / settings closed (see
    // SettingsView wiring), matching PronounsEditor::flush().
    void flush();

    // Shadows the non-virtual Widget::set_visible so hiding this widget also
    // hides the native TextField overlay — same reason as PronounsEditor's
    // override.
    void set_visible(bool v);

    // world_anchor = the emoji button's bounds; SettingsView opens the picker
    // relative to it.
    std::function<void(tk::Rect world_anchor)> on_emoji_button_clicked;
    std::function<void(std::string emoji, std::string text)> on_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint_after_children(tk::PaintCtx&) override;
    void     on_theme_changed(const tk::Theme& t) override;

    static constexpr float kRowH     = 32.0f;
    static constexpr float kEmojiW   = 32.0f;
    static constexpr float kClearW   = 22.0f;
    static constexpr float kFieldGap = 6.0f;
    static constexpr float kTextW    = 260.0f; // capped, not stretched
    static constexpr float kErrorH   = 14.0f;
    static constexpr float kErrorGap = 4.0f;

private:
    void notify_changed_();
    void refresh_emoji_button_();

    tk::Button*    emoji_btn_  = nullptr; // owned via add_child
    tk::Button*    clear_btn_  = nullptr; // owned via add_child; hidden when no emoji
    tk::TextField* text_field_ = nullptr; // owned via add_child

    std::string emoji_;

    // Last (emoji, text) pair actually sent through on_changed — lets flush()
    // and repeated picks no-op when nothing changed.
    std::string last_emoji_;
    std::string last_text_;
    bool        have_last_ = false;

    bool        editable_ = false;
    bool        busy_     = false;
    std::string error_;
    mutable std::unique_ptr<tk::TextLayout> error_layout_;
    // The chosen emoji is hand-drawn over the button at emoji-cell size — the
    // button's own label render is tiny body-font text. Empty emoji falls back
    // to the button's own Lucide icon.
    mutable std::unique_ptr<tk::TextLayout> emoji_glyph_layout_;
};

} // namespace tesseract::views
