#pragma once

// Settings panel section: one labelled checkbox row —
//   "Pre-load full images while scrolling"
// Reads initial state from Settings::instance() and fires on_prefetch_changed
// when the row is toggled.

#include "tk/controls.h"

#include <functional>

namespace tesseract::views
{

class MediaSection : public tk::Widget
{
public:
    MediaSection();
    ~MediaSection() override = default;

    // Silently update checkbox state without firing the callback.
    void set_prefetch_checked(bool enabled);

    // Fired with the new boolean state when the row is toggled.
    std::function<void(bool)> on_prefetch_changed;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    tk::CheckButton* prefetch_cb_ = nullptr;
};

} // namespace tesseract::views
