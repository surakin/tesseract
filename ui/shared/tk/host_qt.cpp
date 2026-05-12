#include "host_qt.h"
#include "canvas_qpainter.h"
#include "controls.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>
#include <QtGui/QTextDocument>
#include <QtGui/QFontMetricsF>
#include <QtGui/QKeyEvent>

namespace tk::qt6 {

// ─────────────────────────────────────────────────────────────────────────
//  QtNativeTextField — QLineEdit-backed NativeTextField
// ─────────────────────────────────────────────────────────────────────────
//
// One per make_text_field() call. The QLineEdit is parented to the
// Surface so it paints on top of the canvas. We use QPointer to handle
// the case where the Surface tears down before the NativeTextField does.

class QtNativeTextField : public NativeTextField {
public:
    explicit QtNativeTextField(QWidget* parent)
        : edit_(new QLineEdit(parent)) {
        edit_->setAttribute(Qt::WA_StyledBackground, true);
        edit_->show();

        QObject::connect(edit_, &QLineEdit::textChanged,
                          edit_, [this](const QString& s) {
            if (on_changed_) on_changed_(s.toStdString());
        });
        QObject::connect(edit_, &QLineEdit::returnPressed,
                          edit_, [this]() {
            if (on_submit_) on_submit_();
        });
    }

    ~QtNativeTextField() override {
        if (edit_) edit_->deleteLater();
    }

    void set_rect(Rect r) override {
        if (!edit_) return;
        edit_->setGeometry(static_cast<int>(r.x),
                            static_cast<int>(r.y),
                            static_cast<int>(r.w),
                            static_cast<int>(r.h));
    }
    void set_text(std::string text) override {
        if (!edit_) return;
        const QSignalBlocker block(edit_);    // don't re-emit on_changed
        edit_->setText(QString::fromStdString(text));
    }
    std::string text() const override {
        return edit_ ? edit_->text().toStdString() : std::string{};
    }
    void set_placeholder(std::string text) override {
        if (edit_) edit_->setPlaceholderText(QString::fromStdString(text));
    }
    void set_focused(bool focused) override {
        if (!edit_) return;
        if (focused) edit_->setFocus();
    }
    void set_visible(bool visible) override {
        if (edit_) edit_->setVisible(visible);
    }
    void set_enabled(bool enabled) override {
        if (edit_) edit_->setEnabled(enabled);
    }
    void set_password(bool password) override {
        if (edit_) edit_->setEchoMode(password ? QLineEdit::Password
                                                  : QLineEdit::Normal);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override {
        on_submit_ = std::move(cb);
    }

private:
    QPointer<QLineEdit>                            edit_;
    std::function<void(const std::string&)>        on_changed_;
    std::function<void()>                          on_submit_;
};

// ─────────────────────────────────────────────────────────────────────────
//  QtNativeTextArea — QTextEdit-backed NativeTextArea (multi-line)
// ─────────────────────────────────────────────────────────────────────────
//
// Multi-line variant for the compose bar. Enter submits; Shift+Enter
// inserts a newline. Auto-grow is reported via on_height_changed using
// the document's `documentLayout()->documentSize()` height.

class ComposeTextEdit : public QTextEdit {
public:
    explicit ComposeTextEdit(QWidget* parent) : QTextEdit(parent) {}
    std::function<void()> on_return_;
protected:
    void keyPressEvent(QKeyEvent* e) override {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            && !(e->modifiers() & Qt::ShiftModifier)) {
            if (on_return_) on_return_();
            e->accept();
            return;
        }
        QTextEdit::keyPressEvent(e);
    }
};

class QtNativeTextArea : public NativeTextArea {
public:
    explicit QtNativeTextArea(QWidget* parent)
        : edit_(new ComposeTextEdit(parent)) {
        edit_->setAttribute(Qt::WA_StyledBackground, true);
        edit_->setFrameShape(QFrame::NoFrame);
        edit_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        edit_->setLineWrapMode(QTextEdit::WidgetWidth);
        edit_->show();

        edit_->on_return_ = [this] { if (on_submit_) on_submit_(); };

        QObject::connect(edit_, &QTextEdit::textChanged, edit_, [this]() {
            if (!edit_) return;
            if (on_changed_) on_changed_(edit_->toPlainText().toStdString());
            float h = natural_height();
            if (h != last_height_ && on_height_changed_) {
                last_height_ = h;
                on_height_changed_(h);
            }
        });
    }

    ~QtNativeTextArea() override {
        if (edit_) edit_->deleteLater();
    }

    void set_rect(Rect r) override {
        if (!edit_) return;
        edit_->setGeometry(static_cast<int>(r.x),
                            static_cast<int>(r.y),
                            static_cast<int>(r.w),
                            static_cast<int>(r.h));
    }
    void set_text(std::string text) override {
        if (!edit_) return;
        const QSignalBlocker block(edit_);
        edit_->setPlainText(QString::fromStdString(text));
    }
    std::string text() const override {
        return edit_ ? edit_->toPlainText().toStdString() : std::string{};
    }
    void set_placeholder(std::string text) override {
        if (edit_) edit_->setPlaceholderText(QString::fromStdString(text));
    }
    void set_focused(bool focused) override {
        if (edit_ && focused) edit_->setFocus();
    }
    void set_visible(bool visible) override {
        if (edit_) edit_->setVisible(visible);
    }
    void set_enabled(bool enabled) override {
        if (edit_) edit_->setEnabled(enabled);
    }
    float natural_height() const override {
        if (!edit_) return 0.f;
        QSizeF docSize = edit_->document()->size();
        const auto m = edit_->contentsMargins();
        return static_cast<float>(docSize.height()
                                    + m.top() + m.bottom() + 4.0);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override {
        on_height_changed_ = std::move(cb);
    }

private:
    QPointer<ComposeTextEdit>                edit_;
    std::function<void(const std::string&)>  on_changed_;
    std::function<void()>                    on_submit_;
    std::function<void(float)>               on_height_changed_;
    float                                    last_height_ = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────
//  Host — tk::Host impl + glue between Surface QWidget and the tree
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host {
public:
    Host(Surface* surface, const Theme& theme)
        : surface_(surface),
          theme_(&theme),
          factory_(make_factory()) {}

    void request_repaint() override {
        if (surface_) surface_->update();
    }

    void post_to_ui(std::function<void()> task) override {
        // QMetaObject::invokeMethod with a queued connection lands `task`
        // on the QObject's thread (the GUI thread, for surface_).
        if (!surface_) return;
        QMetaObject::invokeMethod(surface_, std::move(task),
                                   Qt::QueuedConnection);
    }

    std::unique_ptr<NativeTextField> make_text_field() override {
        if (!surface_) return nullptr;
        return std::make_unique<QtNativeTextField>(surface_);
    }

    std::unique_ptr<NativeTextArea> make_text_area() override {
        if (!surface_) return nullptr;
        return std::make_unique<QtNativeTextArea>(surface_);
    }

    // ── Internal accessors used by Surface ────────────────────────────
    void set_root(std::unique_ptr<Widget> root) {
        root_ = std::move(root);
        relayout();
    }
    Widget* root() const { return root_.get(); }
    const Theme& theme() const { return *theme_; }
    CanvasFactory& factory() { return *factory_; }

    void relayout() {
        if (!root_ || !surface_) return;
        LayoutCtx ctx{ *factory_, *theme_ };
        Rect bounds{ 0, 0,
                      static_cast<float>(surface_->width()),
                      static_cast<float>(surface_->height()) };
        root_->measure(ctx, { bounds.w, bounds.h });
        root_->arrange(ctx, bounds);
        if (on_layout_) on_layout_();
    }

    void set_on_layout(std::function<void()> cb) {
        on_layout_ = std::move(cb);
    }

    void paint(QPainter& painter) {
        if (!root_) return;
        auto canvas = make_canvas(painter);
        PaintCtx ctx{ *canvas, *factory_, *theme_ };
        root_->paint(ctx);
    }

    // Pointer-event dispatch. We keep simple capture semantics: a
    // pointer-down on a Button stamps it as the captured widget; the
    // matching up-on-the-same-button fires its click. This isn't a
    // generic capture protocol — it's the minimum LoginView needs.
    void on_pointer_down(Point local) {
        if (!root_) return;
        pressed_widget_ = root_->dispatch_pointer_down(local);
        if (pressed_widget_) request_repaint();
    }

    void on_pointer_up(Point local) {
        if (!pressed_widget_) return;
        Point ws = pressed_widget_->world_to_local(local);
        bool inside = (ws.x >= 0 && ws.y >= 0 &&
                        ws.x < pressed_widget_->bounds().w &&
                        ws.y < pressed_widget_->bounds().h);
        pressed_widget_->on_pointer_up(ws, inside);
        pressed_widget_ = nullptr;
        request_repaint();
    }

    void on_pointer_move(Point local) {
        if (!root_) return;
        // If a widget claimed the last pointer-down, all subsequent
        // moves go to it as a drag (this is how ListView's scrollbar
        // thumb tracks a held mouse). Hover updates are suspended for
        // the duration of the drag.
        if (pressed_widget_) {
            Point ws = pressed_widget_->world_to_local(local);
            pressed_widget_->on_pointer_drag(ws);
            request_repaint();
            return;
        }
        Widget* hit = root_->hit_test(local);
        Button* hovered = dynamic_cast<Button*>(hit);
        if (hovered == hovered_btn_) return;
        if (hovered_btn_) hovered_btn_->set_hovered(false);
        hovered_btn_ = hovered;
        if (hovered_btn_) hovered_btn_->set_hovered(true);
        request_repaint();
    }

    void on_pointer_leave() {
        if (hovered_btn_) {
            hovered_btn_->set_hovered(false);
            hovered_btn_ = nullptr;
        }
        if (pressed_widget_) {
            // Synthetic pointer-up outside any widget so the captured
            // widget gets a chance to clean up its pressed state.
            pressed_widget_->on_pointer_up({-1, -1}, false);
            pressed_widget_ = nullptr;
        }
        request_repaint();
    }

    void on_wheel(Point local, float dx, float dy) {
        if (!root_) return;
        if (root_->dispatch_wheel(local, dx, dy)) request_repaint();
    }

    void detach_surface() { surface_ = nullptr; }

private:
    Surface*                            surface_;
    const Theme*                        theme_;
    std::unique_ptr<CanvasFactory>      factory_;
    std::unique_ptr<Widget>             root_;
    std::function<void()>               on_layout_;
    Widget*                             pressed_widget_ = nullptr;
    Button*                             hovered_btn_    = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface — QWidget that owns the host + dispatches events
// ─────────────────────────────────────────────────────────────────────────

Surface::Surface(const Theme& theme, QWidget* parent)
    : QWidget(parent),
      host_(std::make_unique<Host>(this, theme)) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMouseTracking(true);
}

Surface::~Surface() {
    if (host_) host_->detach_surface();
}

tk::Host& Surface::host() { return *host_; }
const Theme& Surface::theme() const { return host_->theme(); }

void Surface::set_root(std::unique_ptr<Widget> root) {
    host_->set_root(std::move(root));
    update();
}

Widget* Surface::root() const { return host_->root(); }

void Surface::relayout() {
    host_->relayout();
    update();
}

void Surface::set_on_layout(std::function<void()> cb) {
    host_->set_on_layout(std::move(cb));
}

void Surface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    host_->paint(painter);
}

void Surface::resizeEvent(QResizeEvent*) {
    host_->relayout();
}

void Surface::mousePressEvent(QMouseEvent* e) {
    host_->on_pointer_down({ static_cast<float>(e->position().x()),
                               static_cast<float>(e->position().y()) });
    QWidget::mousePressEvent(e);
}

void Surface::mouseReleaseEvent(QMouseEvent* e) {
    host_->on_pointer_up({ static_cast<float>(e->position().x()),
                             static_cast<float>(e->position().y()) });
    QWidget::mouseReleaseEvent(e);
}

void Surface::mouseMoveEvent(QMouseEvent* e) {
    host_->on_pointer_move({ static_cast<float>(e->position().x()),
                                static_cast<float>(e->position().y()) });
    QWidget::mouseMoveEvent(e);
}

void Surface::wheelEvent(QWheelEvent* e) {
    // Qt reports angleDelta in 1/8 degree units; positive y = scroll
    // away from the user (content scrolls *up*). The toolkit convention
    // is positive dy = scroll content *down*, so invert.
    QPointF pos = e->position();
    QPoint  ad  = e->angleDelta();
    float   dx  =  static_cast<float>(ad.x()) / 8.0f;
    float   dy  = -static_cast<float>(ad.y()) / 8.0f;
    host_->on_wheel({ static_cast<float>(pos.x()),
                       static_cast<float>(pos.y()) }, dx, dy);
    e->accept();
}

void Surface::leaveEvent(QEvent* e) {
    host_->on_pointer_leave();
    QWidget::leaveEvent(e);
}

} // namespace tk::qt6
