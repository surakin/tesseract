#pragma once

// QPainter implementation of tk::Canvas + tk::Image + tk::TextLayout +
// tk::CanvasFactory. Bind one Canvas to an externally-owned QPainter for
// each paintEvent; the factory is application-lifetime.

#include "canvas.h"

class QPainter;
class QImage;

namespace tk::qt6
{

// Wrap a borrowed QPainter for the duration of one paint pass. Caller
// owns the QPainter and is responsible for begin()/end(). The Canvas
// holds a reference, so the QPainter must outlive the returned object.
std::unique_ptr<Canvas> make_canvas(QPainter& painter);

std::unique_ptr<CanvasFactory> make_factory();

// Wrap an already-decoded QImage as a tk::Image. The wrapper owns the
// QImage by value; integration code can hand this directly to an
// avatar / image provider lambda.
std::unique_ptr<Image> make_image(QImage img);

} // namespace tk::qt6
