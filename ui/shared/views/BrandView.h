#pragma once

// Centered brand splash displayed in the chat area when no room is open.
// Renders the app icon (initials circle), the app name, and the version
// string, using only abstract canvas primitives so it works on all
// four platform backends.

#include "tk/widget.h"

#include <memory>

namespace tesseract::views {

class BrandView : public tk::Widget {
public:
    BrandView() = default;
    ~BrandView() override = default;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds)      override;
    void     paint  (tk::PaintCtx&)                        override;

private:
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> version_layout_;

    static constexpr float kIconDiameter = 80.0f;
    static constexpr float kIconToName   = 20.0f;
    static constexpr float kNameToVer    =  6.0f;
};

} // namespace tesseract::views
