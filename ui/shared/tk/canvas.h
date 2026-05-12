#pragma once

// Backend-agnostic 2D drawing surface. Each platform implements this on top
// of its native 2D + text stack (Direct2D + DirectWrite, CoreGraphics +
// CoreText, QPainter, Cairo + Pango). The shared widget tree paints into
// this interface and never sees a platform handle.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace tk {

struct Point {
    float x = 0;
    float y = 0;
};

struct Size {
    float w = 0;
    float h = 0;
};

struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    constexpr float right()  const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr bool  empty()  const { return w <= 0 || h <= 0; }
};

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    static constexpr Color rgb(std::uint32_t hex) {
        return { std::uint8_t((hex >> 16) & 0xff),
                 std::uint8_t((hex >>  8) & 0xff),
                 std::uint8_t( hex        & 0xff),
                 255 };
    }
    static constexpr Color rgba(std::uint8_t r, std::uint8_t g,
                                std::uint8_t b, std::uint8_t a) {
        return { r, g, b, a };
    }
    constexpr Color with_alpha(std::uint8_t alpha) const {
        return { r, g, b, alpha };
    }
};

// Maps to the tesseract::visual::kFont* sizes in visual.h. Backends pick
// the platform-native font face (Segoe UI Variable on Win32, the system
// font on macOS, the GTK theme font on GTK4, QApplication::font() on Qt6)
// and apply the role's weight + size.
enum class FontRole {
    Small,           //  9 pt regular — hint text
    Body,            // 13 pt regular — message body
    SenderName,      // 12 pt semibold — message sender row
    Timestamp,       // 10 pt regular  — HH:MM right-side footer
    SidebarName,     // 13 pt semibold — room name in the room list
    SidebarPreview,  // 11 pt regular  — last-message preview
    UnreadBadge,     // 11 pt semibold — number inside the accent pill
    Title,           // 15 pt semibold — room header
    UiSemibold,      // 11 pt semibold — button label
};

enum class TextHAlign { Leading, Center, Trailing };
enum class TextVAlign { Top, Center, Bottom };
enum class TextTrim   { None, Ellipsis };

struct TextStyle {
    FontRole   role   = FontRole::Body;
    TextHAlign halign = TextHAlign::Leading;
    TextVAlign valign = TextVAlign::Top;
    TextTrim   trim   = TextTrim::None;
    bool       wrap   = false;
    float      max_width  = -1.0f;  // -1 = unbounded
    float      max_height = -1.0f;
};

// Opaque platform-decoded image. Outlives the Canvas that drew it.
class Image {
public:
    virtual ~Image() = default;
    virtual int width()  const = 0;
    virtual int height() const = 0;
};

// Opaque platform-laid-out text run. Owns the shaped glyphs; cheap to
// redraw across paints, expensive to construct.
class TextLayout {
public:
    virtual ~TextLayout() = default;
    virtual Size measure()    const = 0;
    virtual int  line_count() const = 0;
};

// Per-platform factory for backend-owned resources. The platform host
// owns one of these and hands it to the shared widget tree, which uses it
// to decode avatars and build text layouts as widgets mount.
class CanvasFactory {
public:
    virtual ~CanvasFactory() = default;

    virtual std::unique_ptr<Image>
        decode_image(std::span<const std::uint8_t> bytes) = 0;

    virtual std::unique_ptr<TextLayout>
        build_text(std::string_view utf8, const TextStyle&) = 0;
};

// The drawing API the widget tree paints into. State is the current clip
// stack; everything else is per-call. Coordinates are logical pixels;
// scale_factor() reports the DPI scale of the surface for any widget that
// needs to snap to physical pixel boundaries.
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual void clear(Color) = 0;

    virtual void fill_rect(Rect, Color) = 0;
    virtual void fill_rounded_rect(Rect, float radius, Color) = 0;
    virtual void stroke_rect(Rect, Color, float width = 1.0f) = 0;
    virtual void stroke_rounded_rect(Rect, float radius, Color,
                                     float width = 1.0f) = 0;

    virtual void draw_image(const Image&, Rect dst) = 0;
    virtual void draw_image_subregion(const Image&, Rect src, Rect dst) = 0;

    // Circular clip + centre-fit, for avatars. Diameter is logical pixels.
    virtual void draw_circle_image(const Image&, Point centre,
                                   float diameter) = 0;

    // Initials-fallback avatar — backend renders the first 1–2 grapheme
    // clusters of `name` centred in a filled circle. Lives on the canvas
    // because each backend already owns the font path the initials need.
    virtual void draw_initials_circle(std::string_view name, Point centre,
                                      float diameter, Color bg, Color fg) = 0;

    virtual void draw_text(const TextLayout&, Point origin, Color) = 0;

    virtual void push_clip_rect(Rect) = 0;
    virtual void push_clip_rounded_rect(Rect, float radius) = 0;
    virtual void pop_clip() = 0;

    virtual float scale_factor() const = 0;
};

} // namespace tk
