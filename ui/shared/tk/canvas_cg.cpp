#include "canvas_cg.h"

#include <tesseract/settings.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace tk::cg
{

namespace
{

// RAII helpers for the CF object families. CGImageRef, CGPathRef,
// CTFontRef, CTFramesetterRef, CTFrameRef, CTLineRef, CFAttributedString,
// CFData, CFString, CFDictionary, CFNumber — all CFTypeRef-compatible.
template <class T>
struct CFRetained
{
    T ref = nullptr;
    CFRetained() = default;
    explicit CFRetained(T r) : ref(r)
    {
    }
    ~CFRetained()
    {
        if (ref)
        {
            CFRelease(ref);
        }
    }
    CFRetained(const CFRetained&) = delete;
    CFRetained& operator=(const CFRetained&) = delete;
    CFRetained(CFRetained&& other) noexcept : ref(other.ref)
    {
        other.ref = nullptr;
    }
    CFRetained& operator=(CFRetained&& other) noexcept
    {
        if (this != &other)
        {
            if (ref)
            {
                CFRelease(ref);
            }
            ref = other.ref;
            other.ref = nullptr;
        }
        return *this;
    }
    T get() const
    {
        return ref;
    }
    operator T() const
    {
        return ref;
    }
    T release() noexcept
    {
        T r = ref;
        ref = nullptr;
        return r;
    }
};

// kCTStrikethroughStyleAttributeName is not in the 10.15 SDK and CoreText
// doesn't render strikethrough itself anyway. We tag spans with this private
// key and draw the line manually after CTFrameDraw.
static CFStringRef tk_strikethrough_key()
{
    static auto* k = CFSTR("TKStrikethrough");
    return k;
}

CGRect to_cgrect(Rect r)
{
    return CGRectMake(r.x, r.y, r.w, r.h);
}

void set_fill(CGContextRef ctx, Color c)
{
    CGContextSetRGBFillColor(ctx, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                             c.a / 255.0);
}

void set_stroke(CGContextRef ctx, Color c)
{
    CGContextSetRGBStrokeColor(ctx, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                               c.a / 255.0);
}

// Build a CGPath for an axis-aligned rounded rect (no native primitive
// in CoreGraphics — we trace four arcs).
CGPathRef rounded_rect_path(Rect r, float radius)
{
    float rr = std::min(radius, std::min(r.w, r.h) * 0.5f);
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathMoveToPoint(p, nullptr, r.x + rr, r.y);
    CGPathAddArcToPoint(p, nullptr, r.x + r.w, r.y, r.x + r.w, r.y + r.h, rr);
    CGPathAddArcToPoint(p, nullptr, r.x + r.w, r.y + r.h, r.x, r.y + r.h, rr);
    CGPathAddArcToPoint(p, nullptr, r.x, r.y + r.h, r.x, r.y, rr);
    CGPathAddArcToPoint(p, nullptr, r.x, r.y, r.x + r.w, r.y, rr);
    CGPathCloseSubpath(p);
    return p;
}

struct FontDesc
{
    CGFloat size_pt;
    bool bold; // semibold/bold use the emphasized UI font
};

FontDesc desc_for(FontRole role)
{
    const auto& s = tesseract::Settings::instance();
    // The "semibold for emphasis" rule is shared policy
    // (tk::font_role_is_semibold); only the per-role point size is native.
    const bool bold = font_role_is_semibold(role);
    switch (role)
    {
    case FontRole::Small:
        return {static_cast<CGFloat>(s.font_small), bold};
    case FontRole::Body:
        return {static_cast<CGFloat>(s.font_body), bold};
    case FontRole::SenderName:
        return {static_cast<CGFloat>(s.font_sender_name), bold};
    case FontRole::Timestamp:
        return {static_cast<CGFloat>(s.font_timestamp), bold};
    case FontRole::SidebarName:
        return {static_cast<CGFloat>(s.font_sidebar_name), bold};
    case FontRole::SidebarPreview:
        return {static_cast<CGFloat>(s.font_sidebar_preview), bold};
    case FontRole::UnreadBadge:
        return {static_cast<CGFloat>(s.font_unread_badge), bold};
    case FontRole::Title:
        return {static_cast<CGFloat>(s.font_title), bold};
    case FontRole::UiSemibold:
        return {static_cast<CGFloat>(s.font_ui_semibold), bold};
    case FontRole::BigEmoji:
        return {static_cast<CGFloat>(s.font_big_emoji), bold};
    case FontRole::EmojiPickerCell:
        return {static_cast<CGFloat>(s.font_emoji_picker_cell), bold};
    }
    return {static_cast<CGFloat>(s.font_body), bold};
}

CTFontRef create_font(FontRole role)
{
    FontDesc d = desc_for(role);
    CTFontUIFontType ui =
        d.bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem;
    CFRetained<CTFontRef> base{
        CTFontCreateUIFontForLanguage(ui, d.size_pt, nullptr)};
    if (!base.get())
        return nullptr;

    // CTFontCreateUIFontForLanguage does not guarantee Apple Color Emoji in
    // its cascade on all macOS versions/configurations.  Prepend it so emoji
    // characters always fall through to the system colour-emoji font instead
    // of reaching .LastResort and rendering as hex-codepoint boxes.
    CFRetained<CTFontDescriptorRef> emoji_fd{
        CTFontDescriptorCreateWithNameAndSize(CFSTR("Apple Color Emoji"),
                                             d.size_pt)};
    if (!emoji_fd.get())
        return base.release();

    CFRetained<CFArrayRef> existing{static_cast<CFArrayRef>(
        CTFontCopyAttribute(base.get(), kCTFontCascadeListAttribute))};

    CFRetained<CFMutableArrayRef> cascade{
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks)};
    CFArrayAppendValue(cascade.get(), emoji_fd.get());
    if (existing.get())
    {
        CFArrayAppendArray(cascade.get(), existing.get(),
                          CFRangeMake(0, CFArrayGetCount(existing.get())));
    }

    CFTypeRef k = kCTFontCascadeListAttribute;
    CFTypeRef v = cascade.get();
    CFRetained<CFDictionaryRef> attrs{
        CFDictionaryCreate(kCFAllocatorDefault, &k, &v, 1,
                          &kCFTypeDictionaryKeyCallBacks,
                          &kCFTypeDictionaryValueCallBacks)};
    CFRetained<CTFontDescriptorRef> desc{
        CTFontDescriptorCreateWithAttributes(attrs.get())};
    if (!desc.get())
        return base.release();

    CTFontRef result =
        CTFontCreateCopyWithAttributes(base.get(), d.size_pt, nullptr, desc.get());
    return result ? result : base.release();
}

CFStringRef cfstr_from_utf8(std::string_view s)
{
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(s.data()),
                                   s.size(), kCFStringEncodingUTF8, false);
}

// Build a CFAttributedString from a UTF-8 string + a CT font + a
// (optional) paragraph-style alignment + a (placeholder) text colour.
// The colour can be overridden per draw call via CGContextSetFillColor.
CFAttributedStringRef build_attr_string(std::string_view utf8, CTFontRef font,
                                        CTTextAlignment align)
{
    CFRetained<CFStringRef> text{cfstr_from_utf8(utf8)};
    if (!text.get())
    {
        return nullptr;
    }

    CTParagraphStyleSetting settings[] = {
        {kCTParagraphStyleSpecifierAlignment, sizeof(CTTextAlignment), &align},
    };
    CFRetained<CTParagraphStyleRef> style{CTParagraphStyleCreate(
        settings, sizeof(settings) / sizeof(settings[0]))};

    // kCTForegroundColorFromContextAttributeName = kCFBooleanTrue lets
    // CTFrameDraw / CTLineDraw pick up the CGContext's fill colour, so
    // the caller controls the colour at draw time rather than baking it
    // into the attributed string at build time.
    CFTypeRef keys[] = {kCTFontAttributeName, kCTParagraphStyleAttributeName,
                        kCTForegroundColorFromContextAttributeName};
    CFTypeRef values[] = {font, style.get(), kCFBooleanTrue};
    CFRetained<CFDictionaryRef> attrs{CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks)};

    return CFAttributedStringCreate(kCFAllocatorDefault, text.get(),
                                    attrs.get());
}

// The word-split policy is shared (tk::initials_of), which converges away the
// old fragile `>= 8` byte bound. Apply CoreText's locale-aware uppercasing to
// the result before drawing.
std::string initials_upper(std::string_view name)
{
    std::string base = initials_of(name);
    CFRetained<CFStringRef> s{cfstr_from_utf8(base)};
    if (!s.get())
    {
        return base;
    }
    CFRetained<CFMutableStringRef> upper{
        CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s.get())};
    CFStringUppercase(upper.get(), nullptr);

    CFIndex used = 0;
    CFIndex ulen = CFStringGetLength(upper.get());
    CFStringGetBytes(upper.get(), CFRangeMake(0, ulen), kCFStringEncodingUTF8,
                     0, false, nullptr, 0, &used);
    std::string out(static_cast<size_t>(used), '\0');
    CFStringGetBytes(upper.get(), CFRangeMake(0, ulen), kCFStringEncodingUTF8,
                     0, false, reinterpret_cast<UInt8*>(out.data()),
                     static_cast<CFIndex>(out.size()), nullptr);
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  CGImageWrapper — tk::Image
// ─────────────────────────────────────────────────────────────────────────

class CGImageWrapper : public Image
{
public:
    explicit CGImageWrapper(CGImageRef img)
        : img_(img), width_(static_cast<int>(CGImageGetWidth(img))),
          height_(static_cast<int>(CGImageGetHeight(img)))
    {
    }
    ~CGImageWrapper() override
    {
        if (img_)
        {
            CGImageRelease(img_);
        }
    }
    CGImageWrapper(const CGImageWrapper&) = delete;
    CGImageWrapper& operator=(const CGImageWrapper&) = delete;

    int width() const override
    {
        return width_;
    }
    int height() const override
    {
        return height_;
    }
    std::size_t memory_bytes() const override
    {
        if (!img_)
        {
            return 0;
        }
        return static_cast<std::size_t>(CGImageGetBytesPerRow(img_)) *
               static_cast<std::size_t>(CGImageGetHeight(img_));
    }
    CGImageRef image() const
    {
        return img_;
    }

private:
    CGImageRef img_;
    int width_;
    int height_;
};

// ─────────────────────────────────────────────────────────────────────────
//  CTLayout — tk::TextLayout
// ─────────────────────────────────────────────────────────────────────────

class CTLayout : public TextLayout
{
public:
    struct UrlRange
    {
        CFIndex start, end;
        std::string url;
    };

    CTLayout(CFAttributedStringRef attr, CGFloat max_width, CGFloat max_height,
             bool elide_single_line, std::string utf8,
             std::vector<UrlRange> url_ranges = {})
        : attr_(attr), max_width_(max_width), max_height_(max_height),
          elide_single_line_(elide_single_line), utf8_(std::move(utf8)),
          url_ranges_(std::move(url_ranges))
    {
        framesetter_ = CTFramesetterCreateWithAttributedString(attr_);
        if (framesetter_)
        {
            CFRange fit_range = CFRangeMake(0, 0);
            CGSize size = CTFramesetterSuggestFrameSizeWithConstraints(
                framesetter_, CFRangeMake(0, 0), nullptr,
                CGSizeMake(max_width_ > 0 ? max_width_ : CGFLOAT_MAX,
                           max_height_ > 0 ? max_height_ : CGFLOAT_MAX),
                &fit_range);
            measured_ = Size{static_cast<float>(size.width),
                             static_cast<float>(size.height)};
            CGFloat lh = font_line_height();
            if (lh < 1)
            {
                lh = 1;
            }
            line_count_ =
                elide_single_line_
                    ? 1
                    : std::max(1,
                               static_cast<int>(std::ceil(size.height / lh)));

            // When eliding to one line only one CTLine is drawn; clamp the
            // measured height so callers' centering calculations are correct.
            if (elide_single_line_)
            {
                measured_.h = static_cast<float>(lh);
            }

            // Pre-build the CTFrame once so draw() never allocates.
            if (!elide_single_line_)
            {
                CGFloat fw = max_width_ > 0 ? max_width_ : measured_.w;
                CGFloat fh = measured_.h;
                if (fw > 0 && fh > 0)
                {
                    CFRetained<CGMutablePathRef> path{CGPathCreateMutable()};
                    CGPathAddRect(path.get(), nullptr,
                                  CGRectMake(0, 0, fw, fh));
                    frame_ = CTFramesetterCreateFrame(
                        framesetter_, CFRangeMake(0, 0), path.get(), nullptr);
                }
            }
        }
    }

    ~CTLayout() override
    {
        if (frame_)
        {
            CFRelease(frame_);
        }
        if (framesetter_)
        {
            CFRelease(framesetter_);
        }
        if (attr_)
        {
            CFRelease(attr_);
        }
        if (elided_line_)
        {
            CFRelease(elided_line_);
        }
    }
    CTLayout(const CTLayout&) = delete;
    CTLayout& operator=(const CTLayout&) = delete;

    Size measure() const override
    {
        return measured_;
    }
    int line_count() const override
    {
        return line_count_;
    }
    float ascent() const override
    {
        // Apple Color Emoji fills the full line box (ascent + descent), unlike
        // Noto Color Emoji on Linux which leaves the descent area empty. Return
        // the full measured height so centering formulas in views land correctly.
        return static_cast<float>(measured_.h);
    }

    std::string link_at(tk::Point local) const override
    {
        if (url_ranges_.empty())
        {
            return {};
        }
        CFIndex str_idx = kCFNotFound;
        if (frame_)
        {
            CFArrayRef lines = CTFrameGetLines(frame_);
            CFIndex n = CFArrayGetCount(lines);
            if (n == 0)
            {
                return {};
            }
            std::vector<CGPoint> origins(static_cast<std::size_t>(n));
            CTFrameGetLineOrigins(frame_, CFRangeMake(0, n), origins.data());
            // Convert layout-local Y-down to CTFrame Y-up.
            CGFloat ctframe_y = static_cast<CGFloat>(measured_.h - local.y);
            CFIndex best_line = 0;
            CGFloat best_dist = std::abs(origins[0].y - ctframe_y);
            for (CFIndex i = 1; i < n; ++i)
            {
                CGFloat d = std::abs(origins[i].y - ctframe_y);
                if (d < best_dist)
                {
                    best_dist = d;
                    best_line = i;
                }
            }
            CTLineRef line = static_cast<CTLineRef>(
                CFArrayGetValueAtIndex(lines, best_line));
            CGPoint pt = CGPointMake(
                static_cast<CGFloat>(local.x) - origins[best_line].x, 0);
            str_idx = CTLineGetStringIndexForPosition(line, pt);
        }
        else if (attr_)
        {
            // Elided single-line path — no pre-built frame.
            CFRetained<CTLineRef> line{CTLineCreateWithAttributedString(attr_)};
            if (!line.get())
            {
                return {};
            }
            CGPoint pt = CGPointMake(static_cast<CGFloat>(local.x), 0);
            str_idx = CTLineGetStringIndexForPosition(line.get(), pt);
        }
        if (str_idx == kCFNotFound)
        {
            return {};
        }
        for (const auto& r : url_ranges_)
        {
            if (str_idx >= r.start && str_idx < r.end)
            {
                return r.url;
            }
        }
        return {};
    }

    void draw(CGContextRef ctx, Point origin, Color c) const
    {
        if (!framesetter_)
        {
            return;
        }

        if (elide_single_line_)
        {
            draw_elided_line(ctx, origin, c);
            return;
        }

        if (!frame_)
        {
            return;
        }

        // The CTFrame origin is (0,0); translate the context to `origin`
        // and flip from AppKit's top-left to CoreText's bottom-up system.
        CGFloat h = measured_.h;
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, origin.x, origin.y + h);
        CGContextScaleCTM(ctx, 1, -1);
        set_fill(ctx, c);
        CTFrameDraw(frame_, ctx);
        draw_strikethrough(ctx, c);
        CGContextRestoreGState(ctx);
    }

    // Draw strikethrough lines for any runs tagged with tk_strikethrough_key().
    // Called inside the flipped coordinate transform already set up by draw().
    void draw_strikethrough(CGContextRef ctx, Color c) const
    {
        CFArrayRef lines = CTFrameGetLines(frame_);
        CFIndex n = CFArrayGetCount(lines);
        if (n == 0)
        {
            return;
        }
        std::vector<CGPoint> origins(static_cast<std::size_t>(n));
        CTFrameGetLineOrigins(frame_, CFRangeMake(0, n), origins.data());

        set_stroke(ctx, c);
        CGContextSetLineWidth(ctx, 1.0f);

        for (CFIndex li = 0; li < n; ++li)
        {
            CTLineRef line =
                static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, li));
            CFArrayRef runs = CTLineGetGlyphRuns(line);
            CFIndex nr = CFArrayGetCount(runs);
            for (CFIndex ri = 0; ri < nr; ++ri)
            {
                CTRunRef run =
                    static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, ri));
                CFDictionaryRef attrs = CTRunGetAttributes(run);
                CFTypeRef flag =
                    CFDictionaryGetValue(attrs, tk_strikethrough_key());
                if (!flag || flag == kCFBooleanFalse)
                {
                    continue;
                }

                CGFloat asc = 0;
                CGFloat runW = CTRunGetTypographicBounds(
                    run, CFRangeMake(0, 0), &asc, nullptr, nullptr);
                CFIndex loc = CTRunGetStringRange(run).location;
                CGFloat runX =
                    CTLineGetOffsetForStringIndex(line, loc, nullptr);
                CGFloat strikeY = origins[li].y + asc * 0.4f;

                CGContextBeginPath(ctx);
                CGContextMoveToPoint(ctx, origins[li].x + runX, strikeY);
                CGContextAddLineToPoint(ctx, origins[li].x + runX + runW,
                                        strikeY);
                CGContextStrokePath(ctx);
            }
        }
    }

    int char_index_at(tk::Point local) const override
    {
        if (!frame_ && !attr_)
            return -1;
        CFIndex cf_idx = kCFNotFound;
        if (frame_)
        {
            CFArrayRef lines = CTFrameGetLines(frame_);
            CFIndex n = CFArrayGetCount(lines);
            if (n == 0)
                return -1;
            std::vector<CGPoint> origins(static_cast<std::size_t>(n));
            CTFrameGetLineOrigins(frame_, CFRangeMake(0, n), origins.data());
            // Convert tk Y-down to CTFrame Y-up.
            CGFloat ct_y = static_cast<CGFloat>(measured_.h - local.y);
            CFIndex best = 0;
            CGFloat best_dist = std::abs(origins[0].y - ct_y);
            for (CFIndex i = 1; i < n; ++i)
            {
                CGFloat d = std::abs(origins[i].y - ct_y);
                if (d < best_dist)
                {
                    best_dist = d;
                    best = i;
                }
            }
            CTLineRef line = static_cast<CTLineRef>(
                CFArrayGetValueAtIndex(lines, best));
            CGPoint pt = CGPointMake(
                static_cast<CGFloat>(local.x) - origins[best].x, 0);
            cf_idx = CTLineGetStringIndexForPosition(line, pt);
        }
        else
        {
            CFRetained<CTLineRef> line{CTLineCreateWithAttributedString(attr_)};
            if (!line.get())
                return -1;
            CGPoint pt = CGPointMake(static_cast<CGFloat>(local.x), 0);
            cf_idx = CTLineGetStringIndexForPosition(line.get(), pt);
        }
        if (cf_idx == kCFNotFound)
            return -1;
        return cf_to_utf8_byte(static_cast<int>(cf_idx));
    }

    std::vector<tk::Rect> selection_rects(int start_byte,
                                          int end_byte) const override
    {
        if (start_byte >= end_byte || !frame_)
            return {};
        CFIndex cf_start = utf8_byte_to_cf(start_byte);
        CFIndex cf_end   = utf8_byte_to_cf(end_byte);
        if (cf_start >= cf_end)
            return {};

        CFArrayRef lines = CTFrameGetLines(frame_);
        CFIndex n = CFArrayGetCount(lines);
        if (n == 0)
            return {};
        std::vector<CGPoint> origins(static_cast<std::size_t>(n));
        CTFrameGetLineOrigins(frame_, CFRangeMake(0, n), origins.data());

        std::vector<tk::Rect> out;
        for (CFIndex li = 0; li < n; ++li)
        {
            CTLineRef line = static_cast<CTLineRef>(
                CFArrayGetValueAtIndex(lines, li));
            CFRange lr = CTLineGetStringRange(line);
            CFIndex seg_start = std::max(cf_start, lr.location);
            CFIndex seg_end   = std::min(cf_end, lr.location + lr.length);
            if (seg_start >= seg_end)
                continue;
            CGFloat x1 = CTLineGetOffsetForStringIndex(line, seg_start, nullptr);
            CGFloat x2 = CTLineGetOffsetForStringIndex(line, seg_end,   nullptr);
            CGFloat asc = 0, desc = 0, lead = 0;
            CTLineGetTypographicBounds(line, &asc, &desc, &lead);
            CGFloat h = asc + desc + lead;
            // Convert from CTFrame Y-up to tk Y-down.
            CGFloat y_top = static_cast<CGFloat>(measured_.h) -
                            (origins[li].y + asc);
            out.push_back({static_cast<float>(origins[li].x + std::min(x1, x2)),
                           static_cast<float>(y_top),
                           static_cast<float>(std::abs(x2 - x1)),
                           static_cast<float>(h)});
        }
        return out;
    }

    std::string text_range(int start_byte, int end_byte) const override
    {
        int lo = std::max(0, start_byte);
        int hi = std::min(end_byte, static_cast<int>(utf8_.size()));
        if (lo >= hi)
            return {};
        return utf8_.substr(lo, hi - lo);
    }

    CGFloat font_line_height() const
    {
        if (!attr_ || CFAttributedStringGetLength(attr_) == 0)
        {
            return 13.0f;
        }
        CFDictionaryRef d = CFAttributedStringGetAttributes(attr_, 0, nullptr);
        if (!d)
        {
            return 13.0f;
        }
        CTFontRef f = static_cast<CTFontRef>(
            CFDictionaryGetValue(d, kCTFontAttributeName));
        if (!f)
        {
            return 13.0f;
        }
        return CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f);
    }

private:
    // Build the elided CTLine once and cache it. CTLayout is UI-thread-only
    // and immutable after construction, so lazy mutable init is safe.
    bool ensure_elided_line() const
    {
        if (elided_line_)
            return true;
        if (!attr_ || CFAttributedStringGetLength(attr_) == 0)
            return false;
        CFRetained<CTLineRef> raw{CTLineCreateWithAttributedString(attr_)};
        if (!raw.get())
            return false;
        CFDictionaryRef attrs0 =
            CFAttributedStringGetAttributes(attr_, 0, nullptr);
        CFRetained<CFAttributedStringRef> token_attr;
        {
            CFStringRef ell = CFSTR("…");
            token_attr = CFRetained<CFAttributedStringRef>{
                CFAttributedStringCreate(kCFAllocatorDefault, ell, attrs0)};
        }
        CFRetained<CTLineRef> token{
            token_attr.get()
                ? CTLineCreateWithAttributedString(token_attr.get())
                : nullptr};
        CFRetained<CTLineRef> elided{CTLineCreateTruncatedLine(
            raw.get(), max_width_ > 0 ? max_width_ : measured_.w,
            kCTLineTruncationEnd, token.get())};
        CTLineRef out = elided.get() ? elided.get() : raw.get();
        CFRetain(out);
        elided_line_ = out;
        CTLineGetTypographicBounds(elided_line_, &elided_ascent_, nullptr,
                                   nullptr);
        return true;
    }

    void draw_elided_line(CGContextRef ctx, Point origin, Color c) const
    {
        if (!ensure_elided_line())
            return;
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, origin.x, origin.y + elided_ascent_);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextSetTextPosition(ctx, 0, 0);
        set_fill(ctx, c);
        CTLineDraw(elided_line_, ctx);
        CGContextRestoreGState(ctx);
    }

    // Convert a Unicode code-point's UTF-8 byte length to the number of
    // UTF-16 code units it occupies (1 for BMP, 2 for supplementary).
    static int cp_utf16_units(uint32_t cp)
    {
        return (cp >= 0x10000u) ? 2 : 1;
    }

    // Decode one UTF-8 code point starting at `p`; advance `p` past it.
    // Returns U+FFFD on invalid input.
    static uint32_t next_cp(const char*& p, const char* end)
    {
        if (p >= end) return 0xFFFDu;
        unsigned char c = static_cast<unsigned char>(*p);
        uint32_t cp; int extra;
        if      (c < 0x80) { cp = c; extra = 0; }
        else if (c < 0xE0) { cp = c & 0x1Fu; extra = 1; }
        else if (c < 0xF0) { cp = c & 0x0Fu; extra = 2; }
        else                { cp = c & 0x07u; extra = 3; }
        ++p;
        for (int i = 0; i < extra && p < end; ++i, ++p)
            cp = (cp << 6) | (static_cast<unsigned char>(*p) & 0x3Fu);
        return cp;
    }

    // UTF-8 byte offset → CF (UTF-16 code-unit) index.
    CFIndex utf8_byte_to_cf(int byte_offset) const
    {
        const char* p   = utf8_.c_str();
        const char* end = p + std::min(byte_offset, static_cast<int>(utf8_.size()));
        const char* src = utf8_.c_str();
        CFIndex cf = 0;
        while (src < end)
            cf += cp_utf16_units(next_cp(src, end));
        return cf;
    }

    // CF (UTF-16 code-unit) index → UTF-8 byte offset.
    int cf_to_utf8_byte(int cf_idx) const
    {
        const char* p   = utf8_.c_str();
        const char* end = p + utf8_.size();
        int consumed = 0;
        while (p < end && consumed < cf_idx)
        {
            const char* before = p;
            uint32_t cp = next_cp(p, end);
            consumed += cp_utf16_units(cp);
            if (consumed > cf_idx)
            {
                // cf_idx landed inside a surrogate pair — return byte before.
                return static_cast<int>(before - utf8_.c_str());
            }
        }
        return static_cast<int>(p - utf8_.c_str());
    }

    CFAttributedStringRef attr_ = nullptr;
    CTFramesetterRef framesetter_ = nullptr;
    CTFrameRef frame_ = nullptr;
    Size measured_{};
    int line_count_ = 0;
    CGFloat max_width_ = -1;
    CGFloat max_height_ = -1;
    bool elide_single_line_ = false;
    mutable CTLineRef elided_line_ = nullptr;
    mutable CGFloat   elided_ascent_ = 0;
    std::string utf8_;
    std::vector<UrlRange> url_ranges_;
};

// ─────────────────────────────────────────────────────────────────────────
//  CGCanvas — tk::Canvas
// ─────────────────────────────────────────────────────────────────────────

class CGCanvas : public Canvas
{
public:
    explicit CGCanvas(CGContextRef ctx) : ctx_(ctx)
    {
        CGContextSetShouldAntialias(ctx_, true);
        CGContextSetAllowsAntialiasing(ctx_, true);
        // kCGInterpolationHigh → cubic resampling for CGContextDrawImage,
        // matching the D2D backend's HIGH_QUALITY_CUBIC. The context default
        // (kCGInterpolationDefault) is bilinear-equivalent.
        CGContextSetInterpolationQuality(ctx_, kCGInterpolationHigh);
    }

    void clear(Color c) override
    {
        CGRect bbox = CGContextGetClipBoundingBox(ctx_);
        CGContextSaveGState(ctx_);
        CGContextSetBlendMode(ctx_, kCGBlendModeCopy);
        set_fill(ctx_, c);
        CGContextFillRect(ctx_, bbox);
        CGContextRestoreGState(ctx_);
    }

    void fill_rect(Rect r, Color c) override
    {
        set_fill(ctx_, c);
        CGContextFillRect(ctx_, to_cgrect(r));
    }

    void fill_rounded_rect(Rect r, float radius, Color c) override
    {
        CFRetained<CGPathRef> path{rounded_rect_path(r, radius)};
        set_fill(ctx_, c);
        CGContextAddPath(ctx_, path.get());
        CGContextFillPath(ctx_);
    }

    void stroke_rect(Rect r, Color c, float width) override
    {
        set_stroke(ctx_, c);
        CGContextSetLineWidth(ctx_, width);
        float h = width * 0.5f;
        CGContextStrokeRect(
            ctx_, CGRectMake(r.x + h, r.y + h, r.w - width, r.h - width));
    }

    void stroke_rounded_rect(Rect r, float radius, Color c,
                             float width) override
    {
        float h = width * 0.5f;
        CFRetained<CGPathRef> path{rounded_rect_path(
            {r.x + h, r.y + h, r.w - width, r.h - width}, radius)};
        set_stroke(ctx_, c);
        CGContextSetLineWidth(ctx_, width);
        CGContextAddPath(ctx_, path.get());
        CGContextStrokePath(ctx_);
    }

    void draw_image(const Image& image, Rect dst) override
    {
        const auto& wi = static_cast<const CGImageWrapper&>(image);
        draw_cg_image(wi.image(), dst);
    }

    void draw_image_subregion(const Image& image, Rect src, Rect dst) override
    {
        const auto& wi = static_cast<const CGImageWrapper&>(image);
        CFRetained<CGImageRef> sub{CGImageCreateWithImageInRect(
            wi.image(), CGRectMake(src.x, src.y, src.w, src.h))};
        if (!sub.get())
        {
            return;
        }
        draw_cg_image(sub.get(), dst);
    }

    void draw_circle_image(const Image& image, Point centre,
                           float diameter) override
    {
        const auto& wi = static_cast<const CGImageWrapper&>(image);
        CGContextSaveGState(ctx_);
        CGContextAddEllipseInRect(ctx_, CGRectMake(centre.x - diameter * 0.5f,
                                                   centre.y - diameter * 0.5f,
                                                   diameter, diameter));
        CGContextClip(ctx_);
        draw_cg_image(wi.image(),
                      {centre.x - diameter * 0.5f, centre.y - diameter * 0.5f,
                       diameter, diameter});
        CGContextRestoreGState(ctx_);
    }

    void draw_initials_circle(std::string_view name, Point centre,
                              float diameter, Color bg, Color fg) override
    {
        set_fill(ctx_, bg);
        CGContextFillEllipseInRect(ctx_, CGRectMake(centre.x - diameter * 0.5f,
                                                    centre.y - diameter * 0.5f,
                                                    diameter, diameter));

        std::string initials = initials_upper(name);
        CFRetained<CTFontRef> font{CTFontCreateUIFontForLanguage(
            kCTFontUIFontEmphasizedSystem,
            diameter * kAvatarInitialsFontRatio, nullptr)};
        CFRetained<CFAttributedStringRef> attr{
            build_attr_string(initials, font.get(), kCTTextAlignmentCenter)};
        if (!attr.get())
        {
            return;
        }
        CFRetained<CTLineRef> line{
            CTLineCreateWithAttributedString(attr.get())};

        CGFloat ascent = 0;
        CGFloat descent = 0;
        CGFloat leading = 0;
        double w =
            CTLineGetTypographicBounds(line.get(), &ascent, &descent, &leading);
        double tx = centre.x - w * 0.5;
        // Baseline lands halfway between centre and descent.
        double baseline = centre.y + (ascent - descent) * 0.5;

        CGContextSaveGState(ctx_);
        CGContextTranslateCTM(ctx_, tx, baseline);
        CGContextScaleCTM(ctx_, 1, -1);
        CGContextSetTextPosition(ctx_, 0, 0);
        set_fill(ctx_, fg);
        CTLineDraw(line.get(), ctx_);
        CGContextRestoreGState(ctx_);
    }

    void draw_text(const TextLayout& layout, Point origin, Color c) override
    {
        static_cast<const CTLayout&>(layout).draw(ctx_, origin, c);
    }

    void push_clip_rect(Rect r) override
    {
        CGContextSaveGState(ctx_);
        CGContextClipToRect(ctx_, to_cgrect(r));
    }

    void push_clip_rounded_rect(Rect r, float radius) override
    {
        CGContextSaveGState(ctx_);
        CFRetained<CGPathRef> path{rounded_rect_path(r, radius)};
        CGContextAddPath(ctx_, path.get());
        CGContextClip(ctx_);
    }

    void pop_clip() override
    {
        CGContextRestoreGState(ctx_);
    }

    Rect clip_rect() const override
    {
        const CGRect r = CGContextGetClipBoundingBox(ctx_);
        if (CGRectIsEmpty(r) || CGRectIsInfinite(r))
        {
            return {0.f, 0.f, 1e9f, 1e9f};
        }
        return {float(r.origin.x), float(r.origin.y),
                float(r.size.width), float(r.size.height)};
    }

    float scale_factor() const override
    {
        // The CTM scales logical → device. Read it back and assume the
        // x/y scales are equal (true under AppKit isFlipped=YES views).
        CGAffineTransform t = CGContextGetCTM(ctx_);
        return static_cast<float>(std::fabs(t.a));
    }

private:
    void draw_cg_image(CGImageRef img, Rect dst)
    {
        // CoreText framesetters need a bottom-up context; CGImage drawing
        // is the same — CGContextDrawImage paints with origin at the
        // bottom-left of the destination rect, then the host NSView's
        // isFlipped=YES inverts it. We mirror locally so the toolkit's
        // top-left semantics hold either way.
        CGContextSaveGState(ctx_);
        CGContextTranslateCTM(ctx_, dst.x, dst.y + dst.h);
        CGContextScaleCTM(ctx_, 1, -1);
        CGContextDrawImage(ctx_, CGRectMake(0, 0, dst.w, dst.h), img);
        CGContextRestoreGState(ctx_);
    }

    CGContextRef ctx_;
};

std::unique_ptr<Canvas> make_canvas(CGContextRef ctx)
{
    return std::make_unique<CGCanvas>(ctx);
}

// ─────────────────────────────────────────────────────────────────────────
//  CGFactory — tk::CanvasFactory
// ─────────────────────────────────────────────────────────────────────────

class CGFactory : public CanvasFactory
{
public:
    std::unique_ptr<Image>
    decode_image(std::span<const std::uint8_t> bytes) override
    {
        if (bytes.empty())
        {
            return nullptr;
        }
        CFRetained<CFDataRef> data{
            CFDataCreate(kCFAllocatorDefault, bytes.data(),
                         static_cast<CFIndex>(bytes.size()))};
        if (!data.get())
        {
            return nullptr;
        }
        CFRetained<CGImageSourceRef> src{
            CGImageSourceCreateWithData(data.get(), nullptr)};
        if (!src.get())
        {
            return nullptr;
        }
        CGImageRef img = CGImageSourceCreateImageAtIndex(src.get(), 0, nullptr);
        if (!img)
        {
            return nullptr;
        }
        return std::make_unique<CGImageWrapper>(img);
    }

    std::unique_ptr<Image>
    create_image_rgba(const std::uint8_t* pixels, int w, int h) override
    {
        if (!pixels || w <= 0 || h <= 0)
            return nullptr;
        // Copy into CFData so the provider owns the buffer lifetime.
        CFRetained<CFDataRef> data{
            CFDataCreate(kCFAllocatorDefault, pixels,
                         static_cast<CFIndex>(w * h * 4))};
        if (!data.get())
            return nullptr;
        CFRetained<CGDataProviderRef> provider{
            CGDataProviderCreateWithCFData(data.get())};
        if (!provider.get())
            return nullptr;
        CFRetained<CGColorSpaceRef> cs{CGColorSpaceCreateDeviceRGB()};
        CGImageRef img = CGImageCreate(
            static_cast<size_t>(w), static_cast<size_t>(h),
            8,                  // bits per component
            32,                 // bits per pixel
            static_cast<size_t>(w * 4),
            cs.get(),
            static_cast<CGBitmapInfo>(kCGImageAlphaLast) | kCGBitmapByteOrderDefault,
            provider.get(),
            nullptr, false, kCGRenderingIntentDefault);
        if (!img)
            return nullptr;
        return std::make_unique<CGImageWrapper>(img);
    }

    std::unique_ptr<Image>
    scale_image(const Image& src, int max_w, int max_h) override
    {
        int sw = src.width(), sh = src.height();
        if (sw <= 0 || sh <= 0 || (sw <= max_w && sh <= max_h))
            return nullptr;
        float scale = std::min(static_cast<float>(max_w) / sw,
                               static_cast<float>(max_h) / sh);
        int tw = static_cast<int>(std::ceil(sw * scale));
        int th = static_cast<int>(std::ceil(sh * scale));
        CFRetained<CGColorSpaceRef> cs{CGColorSpaceCreateDeviceRGB()};
        CGContextRef bctx = CGBitmapContextCreate(
            nullptr, tw, th, 8, 0, cs.get(),
            kCGBitmapByteOrder32Host | kCGImageAlphaPremultipliedFirst);
        if (!bctx)
            return nullptr;
        CGContextSetInterpolationQuality(bctx, kCGInterpolationHigh);
        const auto& wi = static_cast<const CGImageWrapper&>(src);
        CGContextDrawImage(bctx, CGRectMake(0, 0, tw, th), wi.image());
        CGImageRef scaled = CGBitmapContextCreateImage(bctx);
        CGContextRelease(bctx);
        if (!scaled)
            return nullptr;
        return std::make_unique<CGImageWrapper>(scaled);
    }

    std::unique_ptr<AnimatedImage>
    decode_animated_image(std::span<const std::uint8_t> bytes,
                          int max_px) override
    {
        if (bytes.empty())
            return nullptr;

        CFRetained<CFDataRef> data{
            CFDataCreate(kCFAllocatorDefault, bytes.data(),
                         static_cast<CFIndex>(bytes.size()))};
        if (!data.get())
            return nullptr;

        CFRetained<CGImageSourceRef> src{
            CGImageSourceCreateWithData(data.get(), nullptr)};
        if (!src.get())
            return nullptr;

        const std::size_t count = CGImageSourceGetCount(src.get());
        if (count <= 1)
            return nullptr;

        std::vector<std::unique_ptr<Image>> frames;
        std::vector<int> delays;
        frames.reserve(count);
        delays.reserve(count);

        for (std::size_t i = 0; i < count; ++i)
        {
            int delay_ms = 100;

            CFRetained<CFDictionaryRef> props{
                CGImageSourceCopyPropertiesAtIndex(src.get(), i, nullptr)};
            if (props.get())
            {
                // GIF delay
                auto* gif_dict = static_cast<CFDictionaryRef>(
                    CFDictionaryGetValue(props.get(),
                                        kCGImagePropertyGIFDictionary));
                if (gif_dict)
                {
                    auto* num = static_cast<CFNumberRef>(CFDictionaryGetValue(
                        gif_dict, kCGImagePropertyGIFUnclampedDelayTime));
                    if (!num)
                        num = static_cast<CFNumberRef>(CFDictionaryGetValue(
                            gif_dict, kCGImagePropertyGIFDelayTime));
                    if (num)
                    {
                        double secs = 0;
                        CFNumberGetValue(num, kCFNumberDoubleType, &secs);
                        delay_ms =
                            std::max(20, static_cast<int>(secs * 1000));
                    }
                }
            }

            CGImageRef cg =
                CGImageSourceCreateImageAtIndex(src.get(), i, nullptr);
            if (!cg)
                continue;

            std::unique_ptr<Image> img = std::make_unique<CGImageWrapper>(cg);
            if (auto scaled = scale_image(*img, max_px, max_px))
                img = std::move(scaled);

            frames.push_back(std::move(img));
            delays.push_back(delay_ms);
        }

        if (frames.size() < 2)
            return nullptr;
        return std::make_unique<AnimatedImage>(std::move(frames),
                                              std::move(delays));
    }

    std::unique_ptr<TextLayout> build_text(std::string_view utf8,
                                           const TextStyle& s) override
    {
        CFRetained<CTFontRef> font{create_font(s.role)};
        if (!font.get())
        {
            return nullptr;
        }

        CTTextAlignment align = kCTTextAlignmentLeft;
        switch (s.halign)
        {
        case TextHAlign::Leading:
            align = kCTTextAlignmentLeft;
            break;
        case TextHAlign::Center:
            align = kCTTextAlignmentCenter;
            break;
        case TextHAlign::Trailing:
            align = kCTTextAlignmentRight;
            break;
        }
        // A wrap=false layout must stay on one line; CoreText honours hard
        // breaks regardless, so fold them out first (see
        // tk::fold_hard_breaks_utf8).
        const std::string folded =
            s.wrap ? std::string() : fold_hard_breaks_utf8(utf8);
        const std::string_view src = s.wrap ? utf8 : std::string_view(folded);
        CFAttributedStringRef attr = build_attr_string(src, font.get(), align);
        if (!attr)
        {
            return nullptr;
        }

        bool elide = (s.trim == TextTrim::Ellipsis);
        CGFloat max_w = s.max_width > 0 ? s.max_width : -1;
        CGFloat max_h = s.max_height > 0 ? s.max_height : -1;
        return std::make_unique<CTLayout>(attr, max_w, max_h, elide,
                                          std::string(src));
    }

    std::unique_ptr<TextLayout> build_rich_text(std::span<const TextSpan> spans,
                                                const TextStyle& s) override
    {
        if (spans.empty())
        {
            return nullptr;
        }

        CTTextAlignment align = kCTTextAlignmentLeft;
        switch (s.halign)
        {
        case TextHAlign::Leading:
            align = kCTTextAlignmentLeft;
            break;
        case TextHAlign::Center:
            align = kCTTextAlignmentCenter;
            break;
        case TextHAlign::Trailing:
            align = kCTTextAlignmentRight;
            break;
        }
        CTParagraphStyleSetting ps[] = {{kCTParagraphStyleSpecifierAlignment,
                                         sizeof(CTTextAlignment), &align}};
        CFRetained<CTParagraphStyleRef> para{CTParagraphStyleCreate(ps, 1)};

        CFRetained<CFMutableAttributedStringRef> mattr{
            CFAttributedStringCreateMutable(kCFAllocatorDefault, 0)};
        if (!mattr.get())
        {
            return nullptr;
        }

        FontDesc base_desc = desc_for(s.role);
        std::vector<CTLayout::UrlRange> url_ranges;
        CFIndex char_offset = 0;
        std::string plain_utf8;
        for (const auto& sp : spans) plain_utf8 += sp.text;

        // Reused for per-span syntax-highlight colors (created lazily below).
        CFRetained<CGColorSpaceRef> rgb_cs{CGColorSpaceCreateDeviceRGB()};

        for (const auto& span : spans)
        {
            if (span.text.empty())
            {
                continue;
            }
            CFRetained<CFStringRef> text{cfstr_from_utf8(span.text)};
            if (!text.get() || CFStringGetLength(text.get()) == 0)
            {
                continue;
            }
            CFIndex span_len = CFStringGetLength(text.get());

            CFRetained<CTFontRef> span_font;
            if (span.code)
            {
                span_font = CFRetained<CTFontRef>{CTFontCreateWithName(
                    CFSTR("Menlo"), base_desc.size_pt, nullptr)};
                if (!span_font.get())
                {
                    span_font = CFRetained<CTFontRef>{create_font(s.role)};
                }
            }
            else
            {
                CFRetained<CTFontRef> base{create_font(s.role)};
                CTFontSymbolicTraits need = 0;
                if (span.bold || span.semibold)
                {
                    need |= kCTFontTraitBold;
                }
                if (span.italic)
                {
                    need |= kCTFontTraitItalic;
                }
                if (need)
                {
                    CFRetained<CTFontRef> styled{
                        CTFontCreateCopyWithSymbolicTraits(
                            base.get(), 0.0, nullptr, need, need)};
                    span_font =
                        styled.get() ? std::move(styled) : std::move(base);
                }
                else
                {
                    span_font = std::move(base);
                }
            }

            CFTypeRef st_flag =
                span.strikethrough ? kCFBooleanTrue : kCFBooleanFalse;

            // Syntax-highlighted runs carry an explicit foreground color;
            // everything else inherits the context color set by draw_text().
            CFTypeRef               fg_key = kCTForegroundColorFromContextAttributeName;
            CFTypeRef               fg_val = kCFBooleanTrue;
            CFRetained<CGColorRef>  fg_color;
            if (span.has_color)
            {
                CGFloat comps[4] = {span.color.r / 255.0, span.color.g / 255.0,
                                    span.color.b / 255.0, span.color.a / 255.0};
                fg_color = CFRetained<CGColorRef>{
                    CGColorCreate(rgb_cs.get(), comps)};
                if (fg_color.get())
                {
                    fg_key = kCTForegroundColorAttributeName;
                    fg_val = fg_color.get();
                }
            }

            CFTypeRef keys[] = {
                kCTFontAttributeName,
                kCTParagraphStyleAttributeName,
                fg_key,
                tk_strikethrough_key(),
            };
            CFTypeRef vals[] = {span_font.get(), para.get(), fg_val, st_flag};
            CFRetained<CFDictionaryRef> attrs{
                CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 4,
                                   &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks)};
            CFRetained<CFAttributedStringRef> aspan{CFAttributedStringCreate(
                kCFAllocatorDefault, text.get(), attrs.get())};
            if (!aspan.get())
            {
                continue;
            }
            CFAttributedStringReplaceAttributedString(
                mattr.get(),
                CFRangeMake(CFAttributedStringGetLength(mattr.get()), 0),
                aspan.get());

            if (!span.url.empty())
            {
                url_ranges.push_back(
                    {char_offset, char_offset + span_len, span.url});
            }
            char_offset += span_len;
        }

        if (CFAttributedStringGetLength(mattr.get()) == 0)
        {
            return nullptr;
        }

        CFRetained<CFAttributedStringRef> iattr{
            CFAttributedStringCreateCopy(kCFAllocatorDefault, mattr.get())};
        if (!iattr.get())
        {
            return nullptr;
        }

        bool elide = (s.trim == TextTrim::Ellipsis);
        CGFloat max_w = s.max_width > 0 ? s.max_width : -1;
        CGFloat max_h = s.max_height > 0 ? s.max_height : -1;
        return std::make_unique<CTLayout>(iattr.release(), max_w, max_h, elide,
                                          std::move(plain_utf8),
                                          std::move(url_ranges));
    }
};

std::unique_ptr<CanvasFactory> make_factory()
{
    return std::make_unique<CGFactory>();
}

std::unique_ptr<Image> make_image(CGImageRef img)
{
    if (!img)
    {
        return nullptr;
    }
    CGImageRetain(img); // CGImageWrapper releases in its destructor
    return std::make_unique<CGImageWrapper>(img);
}

} // namespace tk::cg
