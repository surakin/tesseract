#pragma once

// Image-bearing widgets. ImageView paints a rectangular image with a
// configurable content mode; Avatar paints a circular image with an
// initials fallback when no image is set.

#include "widget.h"

#include <optional>
#include <string>

namespace tk {

class ImageView : public Widget {
public:
    enum class ContentMode {
        Cover,    // fill bounds, crop overflow
        Contain,  // fit inside bounds, letterbox
        Fill,     // stretch to bounds
        Center,   // intrinsic size, centred
    };

    explicit ImageView(const Image* image = nullptr,
                        ContentMode mode = ContentMode::Cover)
        : image_(image), mode_(mode) {}

    ImageView& set_image(const Image* img) { image_ = img; return *this; }
    ImageView& set_mode (ContentMode m)    { mode_  = m;   return *this; }
    ImageView& set_size (Size s)           { explicit_size_ = s; return *this; }

    Size measure(LayoutCtx&, Size constraints) override;
    void paint  (PaintCtx&)                    override;

private:
    const Image*  image_      = nullptr;
    ContentMode   mode_       = ContentMode::Cover;
    Size          explicit_size_{ 0, 0 };
};

class Avatar : public Widget {
public:
    explicit Avatar(std::string display_name = {})
        : display_name_(std::move(display_name)) {}

    Avatar& set_image       (const Image* img)         { image_        = img;            return *this; }
    Avatar& set_display_name(std::string n)            { display_name_ = std::move(n);   return *this; }
    Avatar& set_diameter    (float d)                  { diameter_     = d;              return *this; }
    Avatar& set_initials_bg (std::optional<Color> c)   { initials_bg_  = c;              return *this; }
    Avatar& set_initials_fg (std::optional<Color> c)   { initials_fg_  = c;              return *this; }

    float diameter() const { return diameter_; }

    Size measure(LayoutCtx&, Size constraints) override;
    void paint  (PaintCtx&)                    override;

private:
    const Image*          image_   = nullptr;
    std::string           display_name_;
    float                 diameter_ = 32.0f;
    std::optional<Color>  initials_bg_;     // null = theme.avatar_initials_bg
    std::optional<Color>  initials_fg_;
};

} // namespace tk
