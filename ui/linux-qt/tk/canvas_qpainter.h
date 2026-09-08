#pragma once

// QPainter implementation of tk::Canvas + tk::Image + tk::TextLayout +
// tk::CanvasFactory. Bind one Canvas to an externally-owned QPainter for
// each paintEvent; the factory is application-lifetime.

#include "canvas.h"

class QPainter;
class QImage;

namespace tk::qt6
{

// Qt's colour-emoji stock-rendering compensation. Noto Color Emoji on Linux
// is a CBDT bitmap font whose one strike FreeType reports as bitmap height
// 128px at 109 ppem — the glyphs are authored ~1.17x taller than the em box.
// QFreetypeFace::computeSize (qtbase qfontengine_ft.cpp) scales such a glyph
// by `pixelSize / bitmap.height`, squishing that overshoot away, so Qt draws
// emoji at exactly the point size. Cairo/Pango (the GTK4 backend) scales by
// `pixelSize / strike_ppem` instead and keeps the overshoot. Qt multiplies
// its emoji-run point size by this (128/109) so both backends land at the
// same visual size. Combined with the shared tk::kEmojiSizeAdjust knob.
inline constexpr double kQtEmojiStockComp = 128.0 / 109.0;

// Wrap a borrowed QPainter for the duration of one paint pass. Caller
// owns the QPainter and is responsible for begin()/end(). The Canvas
// holds a reference, so the QPainter must outlive the returned object.
std::unique_ptr<Canvas> make_canvas(QPainter& painter);

std::unique_ptr<CanvasFactory> make_factory();

// Wrap an already-decoded QImage as a tk::Image. The wrapper owns the
// QImage by value; integration code can hand this directly to an
// avatar / image provider lambda.
std::unique_ptr<Image> make_image(QImage img);

// The reverse of make_image() — extract the underlying native bitmap from
// a tk::Image so it can be embedded into a platform-native rich-text
// control (e.g. QTextDocument::addResource for an inline composer
// emoticon pill). Every backend exposes the same name,
// tk::<backend>::to_native_image, returning its own concretely-typed
// NativeImageHandle — host_qt.cpp is the only caller, and only ever
// compiles this one backend's header.
using NativeImageHandle = QImage;
NativeImageHandle to_native_image(const Image& img);

} // namespace tk::qt6
