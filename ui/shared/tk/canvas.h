#pragma once

// Backend-agnostic 2D drawing surface. Each platform implements this on top
// of its native 2D + text stack (Direct2D + DirectWrite, CoreGraphics +
// CoreText, QPainter, Cairo + Pango). The shared widget tree paints into
// this interface and never sees a platform handle.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tk
{

struct Point
{
    float x = 0;
    float y = 0;
};

struct Size
{
    float w = 0;
    float h = 0;
};

struct Rect
{
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    constexpr float right() const
    {
        return x + w;
    }
    constexpr float bottom() const
    {
        return y + h;
    }
    constexpr bool empty() const
    {
        return w <= 0 || h <= 0;
    }
};

struct Color
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    static constexpr Color rgb(std::uint32_t hex)
    {
        return {std::uint8_t((hex >> 16) & 0xff),
                std::uint8_t((hex >> 8) & 0xff), std::uint8_t(hex & 0xff), 255};
    }
    static constexpr Color rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                std::uint8_t a)
    {
        return {r, g, b, a};
    }
    constexpr Color with_alpha(std::uint8_t alpha) const
    {
        return {r, g, b, alpha};
    }
};

// Maps to the tesseract::visual::kFont* sizes in visual.h. Backends pick
// the platform-native font face (Segoe UI Variable on Win32, the system
// font on macOS, the GTK theme font on GTK4, QApplication::font() on Qt6)
// and apply the role's weight + size.
enum class FontRole
{
    Small,          //  9 pt regular — hint text
    Body,           // 13 pt regular — message body
    SenderName,     // 12 pt semibold — message sender row
    Timestamp,      // 10 pt regular  — HH:MM right-side footer
    SidebarName,    // 13 pt semibold — room name in the room list
    SidebarPreview, // 11 pt regular  — last-message preview
    UnreadBadge,    // 11 pt semibold — number inside the accent pill
    Title,          // 15 pt semibold — room header
    UiSemibold,     // 11 pt semibold — button label
    BigEmoji,           // 24 pt regular — emoji-only message body (2× Body)
    EmojiPickerCell,    // 17 pt regular — emoji picker grid cells
};

// ── Shared FontRole + avatar policy (canvas_common.cpp) ────────────────────
//
// The FontRole→weight rule ("semibold for emphasis roles") and the avatar
// initials word-split are identical app policy across every backend. They
// live here so the four backends share one source of truth and keep only the
// genuinely-native bits (font face construction, locale-aware uppercasing,
// glyph drawing). See docs/UI-PARITY.md.

// True for the roles drawn semibold (DemiBold / SEMI_BOLD / emphasized UI
// font), false for the regular-weight roles. The per-role point size still
// comes from tesseract::Settings in each backend (and the family may differ
// per backend, e.g. D2D's Title uses the Display face) — only the weight
// classification is shared here.
bool font_role_is_semibold(FontRole role);

// Avatar initials disc: the glyph point size as a fraction of the circle
// diameter. Shared so the four backends can't drift (Qt previously used
// 0.36 while the others used 0.42).
inline constexpr float kAvatarInitialsFontRatio = 0.42f;

// Avatar initials policy: split `name` on whitespace and return the first
// grapheme of the first word followed by the first grapheme of the second
// word (1–2 code points total), as UTF-8. Returns "?" when `name` has no
// non-space content. UTF-8-correct: never splits a multibyte code point.
//
// This is the shared *word-split* policy only. The result is left in the
// source case; each backend applies its own locale-aware uppercasing before
// drawing (so e.g. Turkish casing stays correct). For the common ASCII case
// the result is already what gets drawn.
std::string initials_of(std::string_view name);

enum class TextHAlign
{
    Leading,
    Center,
    Trailing
};
enum class TextVAlign
{
    Top,
    Center,
    Bottom
};
enum class TextTrim
{
    None,
    Ellipsis
};

struct TextStyle
{
    FontRole role = FontRole::Body;
    TextHAlign halign = TextHAlign::Leading;
    TextVAlign valign = TextVAlign::Top;
    TextTrim trim = TextTrim::None;
    bool wrap = false;
    float max_width = -1.0f; // -1 = unbounded
    float max_height = -1.0f;
};

// Replace every Unicode mandatory line/paragraph break in a UTF-8 string with
// a space. A wrap=false layout must occupy exactly one line, but the native
// text stacks (DirectWrite, CoreText, QPainter, Pango) all honour hard breaks
// regardless of the no-wrap flag — so a "single line" style would otherwise
// spill across several lines and overflow its fixed-height container. Each
// backend folds its single-line input through this so measure and draw agree.
// Covers LF, CR, VT, FF, NEL (U+0085), LINE SEPARATOR (U+2028) and PARAGRAPH
// SEPARATOR (U+2029).
inline std::string fold_hard_breaks_utf8(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();)
    {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x0A || c == 0x0D || c == 0x0B || c == 0x0C)
        {
            out += ' ';
            i += 1;
        }
        else if (c == 0xC2 && i + 1 < in.size() &&
                 static_cast<unsigned char>(in[i + 1]) == 0x85)
        {
            out += ' '; // U+0085 NEL
            i += 2;
        }
        else if (c == 0xE2 && i + 2 < in.size() &&
                 static_cast<unsigned char>(in[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(in[i + 2]) == 0xA8 ||
                  static_cast<unsigned char>(in[i + 2]) == 0xA9))
        {
            out += ' '; // U+2028 LS / U+2029 PS
            i += 3;
        }
        else
        {
            out += in[i];
            i += 1;
        }
    }
    return out;
}

// Opaque platform-decoded image. Outlives the Canvas that drew it.
class Image
{
public:
    virtual ~Image() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Approximate resident size of the decoded pixels (and any cached
    // backend-scaled/GPU copies), in bytes. Each backend reports its true
    // allocation via its native API so the image cache's byte budget tracks
    // real memory rather than a w*h*4 guess.
    virtual std::size_t memory_bytes() const = 0;
};

// Shared handle to a decoded image. The image cache hands these out so many
// widgets can pin the same image; eviction only reclaims an image once the
// cache holds the sole reference (use_count() == 1).
using ImageRef = std::shared_ptr<Image>;

// Animated image: holds pre-decoded, pre-scaled frames with per-frame delays.
// current_frame() advances internally using steady_clock — call it every
// paint and schedule a repaint via ms_until_next_frame() to animate.
class AnimatedImage
{
public:
    AnimatedImage(std::vector<std::unique_ptr<Image>> frames,
                  std::vector<int> delays_ms)
        : frames_(std::move(frames))
        , delays_ms_(std::move(delays_ms))
        , next_tp_(std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(delays_ms_[0]))
    {
        assert(!frames_.empty());
        assert(frames_.size() == delays_ms_.size());
    }

    const Image& current_frame() const
    {
        const auto now = std::chrono::steady_clock::now();
        while (now >= next_tp_)
        {
            cur_idx_ = (cur_idx_ + 1) % frames_.size();
            next_tp_ +=
                std::chrono::milliseconds(delays_ms_[cur_idx_]);
        }
        return *frames_[cur_idx_];
    }

    int ms_until_next_frame() const
    {
        using ms = std::chrono::milliseconds;
        const auto rem = std::chrono::duration_cast<ms>(
                             next_tp_ - std::chrono::steady_clock::now())
                             .count();
        return static_cast<int>(std::max(ms::rep{1}, rem));
    }

    int width() const { return frames_[0]->width(); }
    int height() const { return frames_[0]->height(); }
    int frame_count() const { return static_cast<int>(frames_.size()); }

    std::size_t memory_bytes() const
    {
        std::size_t total = 0;
        for (const auto& f : frames_)
            total += f->memory_bytes();
        return total;
    }

private:
    std::vector<std::unique_ptr<Image>> frames_;
    std::vector<int> delays_ms_;
    mutable std::size_t cur_idx_ = 0;
    mutable std::chrono::steady_clock::time_point next_tp_;
};

// Opaque platform-laid-out text run. Owns the shaped glyphs; cheap to
// redraw across paints, expensive to construct.
class TextLayout
{
public:
    virtual ~TextLayout() = default;
    virtual Size measure() const = 0;
    virtual int line_count() const = 0;
    // Distance from the top of the layout box to the baseline (typographic
    // ascent). Use this instead of measure().h for vertically centering
    // single-line glyphs that leave an empty descender below the baseline.
    virtual float ascent() const = 0;
    // Returns the URL of the hyperlink whose glyph bounds contain `local`
    // (layout-local coordinates: origin = the Point passed to draw_text).
    // Returns "" when not over a hyperlink. Backends override when
    // build_rich_text is fully implemented; plain-text layouts always return "".
    virtual std::string link_at(Point /*local*/) const
    {
        return {};
    }

    // UTF-8 byte offset of the character nearest to `local` (layout-local
    // coords). Returns -1 when not over any character or when the backend
    // does not support hit-testing (plain-text-only layouts).
    virtual int char_index_at(Point /*local*/) const
    {
        return -1;
    }

    // Highlight rects for the UTF-8 byte range [start_byte, end_byte).
    // May return multiple rects for multi-line selections. Returns empty
    // when the backend does not implement this or the range is empty.
    virtual std::vector<Rect> selection_rects(int /*start_byte*/,
                                              int /*end_byte*/) const
    {
        return {};
    }

    // Plain UTF-8 text for the range [start_byte, end_byte) as rendered.
    // Used to put the selected substring onto the clipboard.
    virtual std::string text_range(int /*start_byte*/,
                                   int /*end_byte*/) const
    {
        return {};
    }
};

// One formatting run for rich text. Bold, italic, code, and strikethrough
// may be combined; `text` is plain UTF-8 (no markup, newlines as '\n').
// `url` is non-empty iff this run is a hyperlink — backends render it with
// underline + link colour.
// `spoiler` is true for MSC2010 spoiler spans; `spoiler_reason` is the
// optional reason string from data-mx-spoiler="reason".
struct TextSpan
{
    std::string text;
    std::string url;
    std::string spoiler_reason;
    bool bold = false;
    bool italic = false;
    bool code = false; // render in monospace (inline `code` or fenced block)
    bool code_block = false; // part of a fenced ```block``` (vs inline code)
    bool strikethrough = false;
    bool spoiler = false;
    // Explicit foreground colour for this run (syntax-highlighted code, or a
    // mention's accent text). When has_color is false, backends paint with the
    // draw_text() colour.
    bool  has_color = false;
    Color color{};
    // Mention pill: an intentional mention (matrix.to user link or @room). The
    // run keeps its `url` for hit-testing but is drawn without the link
    // underline; `background` is filled as a rounded pill behind the text.
    bool  is_mention = false;
    bool  has_background = false;
    Color background{};
};

// Per-platform factory for backend-owned resources. The platform host
// owns one of these and hands it to the shared widget tree, which uses it
// to decode avatars and build text layouts as widgets mount.
class CanvasFactory
{
public:
    virtual ~CanvasFactory() = default;

    virtual std::unique_ptr<Image>
    decode_image(std::span<const std::uint8_t> bytes) = 0;

    // Create an Image from a raw RGBA8888 pixel buffer (stride = w * 4).
    // Returns nullptr on backends that don't implement this (default).
    virtual std::unique_ptr<Image>
    create_image_rgba(const std::uint8_t* /*pixels*/, int /*w*/, int /*h*/)
    {
        return nullptr;
    }

    // Downscale `src` to fit within max_w × max_h, preserving aspect ratio,
    // using a high-quality multi-pass filter (box/bicubic). Returns nullptr
    // when the image already fits or the backend doesn't override this
    // (caller should keep the original in that case).
    virtual std::unique_ptr<Image>
    scale_image(const Image& /*src*/, int /*max_w*/, int /*max_h*/)
    {
        return nullptr;
    }

    // Decode all frames of an animated GIF or WebP from `bytes`, pre-scaling
    // each frame to fit within max_px × max_px (aspect-ratio-preserving).
    // Returns nullptr for static (single-frame) images, on decode failure,
    // or when the backend does not override this method.
    virtual std::unique_ptr<AnimatedImage>
    decode_animated_image(std::span<const std::uint8_t> /*bytes*/,
                          int /*max_px*/)
    {
        return nullptr;
    }

    virtual std::unique_ptr<TextLayout> build_text(std::string_view utf8,
                                                   const TextStyle&) = 0;

    // Rich-text variant. All spans share the same base TextStyle (size,
    // wrap, max_width). Falls back to concatenated plain text on backends
    // that have not yet implemented inline formatting.
    virtual std::unique_ptr<TextLayout>
    build_rich_text(std::span<const TextSpan> spans, const TextStyle&) = 0;
};

// The drawing API the widget tree paints into. State is the current clip
// stack; everything else is per-call. Coordinates are logical pixels;
// scale_factor() reports the DPI scale of the surface for any widget that
// needs to snap to physical pixel boundaries.
class Canvas
{
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

    // Bounding rect of the current accumulated clip in surface coordinates.
    // ListView uses this to skip paint_row() for rows that lie entirely
    // outside the repainted region (e.g. a small partial-update from an
    // animated-image tick). Default returns a full-coverage sentinel so
    // backends that do not track clip state never cull rows incorrectly.
    virtual Rect clip_rect() const
    {
        return {0.f, 0.f, 1e9f, 1e9f};
    }

    virtual float scale_factor() const = 0;
};

} // namespace tk
