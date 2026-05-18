#include "tk_test_surface.h"
#include "tk/canvas_qpainter.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <memory>

namespace
{

// Catch2WithMain owns main(), so we lazily construct a QGuiApplication on
// the first TestSurface::create() call. QGuiApplication is needed by
// QFont resolution; we never enter its event loop.
QGuiApplication* g_app()
{
    static int argc = 1;
    static char name[] = "tesseract_tests";
    static char* argv[] = {name, nullptr};
    static QGuiApplication* app =
        QCoreApplication::instance()
            ? static_cast<QGuiApplication*>(QCoreApplication::instance())
            : new QGuiApplication(argc, argv);
    return app;
}

class QtTestSurface : public TestSurface
{
public:
    QtTestSurface(int w, int h)
        : img_(w, h, QImage::Format_ARGB32_Premultiplied), painter_(&img_),
          factory_(tk::qt6::make_factory()),
          canvas_(tk::qt6::make_canvas(painter_))
    {
        img_.fill(Qt::white);
    }
    ~QtTestSurface() override
    {
        if (painter_.isActive())
        {
            painter_.end();
        }
    }

    tk::Canvas& canvas() override
    {
        return *canvas_;
    }
    tk::CanvasFactory& factory() override
    {
        return *factory_;
    }

    tk::Color read_pixel(int x, int y) override
    {
        if (painter_.isActive())
        {
            painter_.end();
        }
        QImage unpremul = img_.convertToFormat(QImage::Format_ARGB32);
        QRgb px = unpremul.pixel(x, y);
        return tk::Color::rgba(static_cast<std::uint8_t>(qRed(px)),
                               static_cast<std::uint8_t>(qGreen(px)),
                               static_cast<std::uint8_t>(qBlue(px)),
                               static_cast<std::uint8_t>(qAlpha(px)));
    }

    int width() const override
    {
        return img_.width();
    }
    int height() const override
    {
        return img_.height();
    }

private:
    QImage img_;
    QPainter painter_;
    std::unique_ptr<tk::CanvasFactory> factory_;
    std::unique_ptr<tk::Canvas> canvas_;
};

} // namespace

std::unique_ptr<TestSurface> TestSurface::create(int w, int h)
{
    g_app();
    return std::make_unique<QtTestSurface>(w, h);
}
