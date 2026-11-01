#pragma once

// Cross-platform "please wait" overlay. Like AlertDialog it is mounted at
// container level and painted last, but it has no buttons and cannot be
// dismissed by the user: backdrop clicks and every key (Escape included) are
// consumed until the owner calls close(). Used while the SDK clients are
// being rebuilt in place.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class BusyDialog : public tk::Widget
{
public:
    BusyDialog();
    ~BusyDialog() override = default;

    // Show the dialog. A new open() while already shown replaces the text.
    void open(std::string title, std::string body = {});
    void close();
    bool is_open() const
    {
        return open_;
    }

    // Fires when the dialog opens or closes so the host can re-query native
    // overlay rects (e.g. hide the compose textarea while the dialog is up).
    std::function<void()> on_layout_changed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;

    // Consume input so nothing behind the dialog is reachable.
    bool on_pointer_down(tk::Point local) override;
    bool on_key_down(const tk::KeyEvent&) override;
    bool dispatch_key_down(const tk::KeyEvent&) override;

    bool focusable() const override
    {
        return false;
    }

    tk::Role access_role() const override
    {
        return tk::Role::Dialog;
    }
    std::string access_name() const override
    {
        return title_.empty() ? body_ : title_;
    }
    std::string access_description() const override
    {
        return title_.empty() ? std::string() : body_;
    }
    bool access_modal() const override
    {
        return open_ && visible();
    }

private:
    bool open_ = false;

    std::string title_;
    std::string body_;

    tk::ProgressBar* progress_ = nullptr;

    tk::Rect backdrop_rect_{};
    tk::Rect card_rect_{};

    tk::CachedTextLayout title_layout_;
    tk::CachedTextLayout body_layout_;

    static constexpr float kCardW = 360.0f;
    static constexpr float kCardPad = 20.0f;
    static constexpr float kTitleH = 22.0f;
    static constexpr float kTitleGap = 12.0f;
    static constexpr float kBodyH = 40.0f;
    static constexpr float kBarGap = 16.0f;
    static constexpr float kBarH = 6.0f;
};

} // namespace tesseract::views
