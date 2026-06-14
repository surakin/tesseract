#include "InflightDotWidget.h"

#include "tk/canvas_qpainter.h"
#include "tk/inflight_dot.h"

#include <QPainter>
#include <QPaintEvent>

// Ring color: fixed text_muted value; can be sourced from the active theme
// palette in a future pass.
static constexpr tk::Color kRingColor = tk::Color::rgb(0xA0A0A6);

static constexpr float kDotR     = tk::kInflightDotR;
static constexpr float kOrbitR   = tk::kInflightOrbitR;
static constexpr float kRingDotR = tk::kInflightRingDotR;

InflightDotWidget::InflightDotWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void InflightDotWidget::update_state(uint32_t count, tk::Color dot_color)
{
    count_     = count;
    dot_color_ = dot_color;
    update();
}

void InflightDotWidget::update_phase(float phase)
{
    phase_ = phase;
}

void InflightDotWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto cv = tk::qt6::make_canvas(painter);

    const tk::Point center{width() / 2.0f, height() / 2.0f};

    tk::draw_inflight_indicator(*cv, center,
                                kDotR, kOrbitR, kRingDotR,
                                dot_color_, kRingColor,
                                phase_, count_ >= 2);
}

QSize InflightDotWidget::sizeHint() const
{
    const int sz = static_cast<int>(tk::kInflightViewSize);
    return QSize(sz, sz);
}
