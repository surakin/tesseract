#pragma once

// Cross-platform modal confirmation overlay. Mounted at MainAppWidget level
// (painted last so it sits above every other view including the lightboxes)
// and used by any widget that wants to gate a destructive action behind a
// yes/no prompt. The same pattern as RoomInfoPanel: closed-by-default,
// visibility tied to open state so the hit-test walks past it when idle.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class ConfirmDialog : public tk::Widget
{
public:
    struct Options
    {
        std::string title;
        std::string body; // optional; may be empty
        std::string confirm_label = tk::tr("Confirm");
        std::string cancel_label = tk::tr("Cancel");
        bool destructive = false; // red confirm button
        // Show a single-line free-text field under the body (e.g. a
        // kick/ban reason). Its trimmed text is handed to `on_reason`
        // immediately before the on_confirm callback fires; cancel/backdrop
        // never fire it. No-op without a Host (the field isn't created).
        bool reason_field = false;
        std::string reason_placeholder;
        std::function<void(std::string reason)> on_reason;
        // Optional third button between Cancel and Confirm (shown when
        // `secondary_label` is non-empty). Dismisses the dialog and fires
        // `on_secondary`. The card widens to fit three buttons.
        std::string secondary_label;
        std::function<void()> on_secondary;
    };

    ConfirmDialog();
    ~ConfirmDialog() override = default;

    // Show the dialog with the given options. `on_confirm` fires when the
    // user clicks the confirm button. Cancel + backdrop click dismiss the
    // dialog without firing the callback. A new open() while the dialog is
    // already up replaces the contents (mirrors RoomInfoPanel::open).
    void open(Options opts, std::function<void()> on_confirm);
    void close();
    bool is_open() const
    {
        return open_;
    }
    // Same as clicking the confirm button (Enter in the reason field, tests).
    void confirm();

    // The Options::reason_field input; null without a Host.
    tk::TextField* reason_field() const
    {
        return reason_field_;
    }

    // Fires when the dialog opens or closes. MainAppWidget routes this into
    // the shared layout-changed chain so the shells re-query rect accessors
    // (specifically: hide the native compose textarea + room-search field
    // while the dialog covers them).
    std::function<void()> on_layout_changed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    void on_theme_changed(const tk::Theme& t) override;

    // Accessibility: a modal Dialog named by its (canvas-painted) title and
    // described by its body; the buttons attach under it. While open,
    // build_access_tree hides everything behind it.
    tk::Role access_role() const override
    {
        return tk::Role::Dialog;
    }
    std::string access_name() const override
    {
        return opts_.title.empty() ? opts_.body : opts_.title;
    }
    std::string access_description() const override
    {
        return opts_.title.empty() ? std::string() : opts_.body;
    }
    bool access_modal() const override
    {
        return open_ && visible();
    }

private:
    bool open_ = false;

    Options opts_;
    std::function<void()> on_confirm_;

    // Child buttons — owned via add_child, raw pointers borrowed back.
    tk::Button* confirm_btn_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;
    tk::Button* secondary_btn_ = nullptr;
    // Options::reason_field input; null without a Host.
    tk::TextField* reason_field_ = nullptr;

    // Layout rects in world-space, updated each arrange().
    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};

    // Cached layouts so we don't rebuild every paint.
    tk::CachedTextLayout title_layout_;
    tk::CachedTextLayout body_layout_;

    bool press_backdrop_ = false;

    static constexpr float kCardW = 360.0f;
    static constexpr float kCardWWide = 520.0f; // with a secondary button
    static constexpr float kCardPad = 20.0f;
    static constexpr float kBtnH = 36.0f;
    static constexpr float kBtnGap = 8.0f;
    static constexpr float kTitleH = 22.0f;
    static constexpr float kTitleGap = 12.0f;
    static constexpr float kBodyGap = 20.0f;
    static constexpr float kFieldH = 32.0f;
};

} // namespace tesseract::views
