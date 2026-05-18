#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Thin facade over DirectWrite + Direct2D for text drawing.
//
// Why: GDI+ Graphics::DrawString cannot read OpenType color font tables
// (COLR/CPAL) in Segoe UI Emoji, so emoji rendered through GDI+ are
// monochrome outlines. D2D's DrawTextLayout with
// D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT renders the colored layers.
//
// All call sites pass HDC, RECT, COLORREF and wstring; D2D/DWrite stay
// fully encapsulated inside the .cpp.
//
// THREADING: all functions must be called on the UI thread. The D2D factory
// is created with D2D1_FACTORY_TYPE_SINGLE_THREADED.
struct IDWriteFontFallback;

namespace win32::text
{

enum class Weight
{
    Regular,
    Semibold,
    Bold
};
enum class Slant
{
    Roman,
    Italic
};
enum class HAlign
{
    Leading,
    Center,
    Trailing
};
enum class VAlign
{
    Top,
    Center,
    Bottom
};
enum class Trim
{
    None,
    EllipsisChar
};
enum class Wrap
{
    NoWrap,
    Word
};
enum class SizeUnit
{
    Point,
    Pixel
};

struct Style
{
    const wchar_t* family = L"Segoe UI";
    float size = 10.0f;
    SizeUnit unit = SizeUnit::Point;
    Weight weight = Weight::Regular;
    Slant slant = Slant::Roman;
    COLORREF color = 0;
    BYTE alpha = 255;
    HAlign halign = HAlign::Leading;
    VAlign valign = VAlign::Top;
    Trim trim = Trim::None;
    Wrap wrap = Wrap::NoWrap;
};

void draw(HDC hdc, const RECT& bounds, const wchar_t* text, int len,
          const Style& s);

struct Metrics
{
    int width;
    int height;
    int line_count;
};

Metrics measure(const wchar_t* text, int len, const Style& s,
                int max_width = -1);

bool init();
void shutdown();
void on_dpi_changed(UINT dpi);

// Override the font fallback used for all subsequent draw/measure calls.
// Call after the D2D backend initializes to share its Twemoji-first
// fallback chain so flag emoji render the same as in the tk surfaces.
void set_font_fallback(IDWriteFontFallback* fallback);

} // namespace win32::text
