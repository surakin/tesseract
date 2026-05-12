#pragma once

// Offscreen test harness for tk::Canvas. Each platform implements a
// concrete TestSurface in tk_test_surface_<backend>.cpp and registers it
// via the TestSurface::create() factory below. Tests in
// test_tk_canvas.cpp run identical assertions against whichever backend
// the build selected.

#include "tk/canvas.h"

#include <memory>

struct TestSurface {
    virtual ~TestSurface() = default;

    virtual tk::Canvas&        canvas()  = 0;
    virtual tk::CanvasFactory& factory() = 0;

    // Sample a pixel in the offscreen target after all queued paint ops
    // have been flushed by the implementation. Returned colour is
    // non-premultiplied 8-bit.
    virtual tk::Color read_pixel(int x, int y) = 0;

    virtual int width()  const = 0;
    virtual int height() const = 0;

    static std::unique_ptr<TestSurface> create(int width, int height);
};
