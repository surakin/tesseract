#include "host_qt.h"
#include "anim_image_cache.h"
#include "canvas_qpainter.h"
#include "controls.h"

#include <tesseract/settings.h>

#include <cmath>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>
#include <QtGui/QTextDocument>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextFormat>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QFontMetricsF>
#include <QtGui/QKeyEvent>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QList>
#include <QtCore/QMimeData>
#include <QtCore/QMimeDatabase>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QGuiApplication>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDropEvent>
#include <QMediaDevices>
#include <QAudioDevice>

#include "gst_hw_probe.h"
#include <gst/gst.h>

namespace tk::qt6
{

// ─────────────────────────────────────────────────────────────────────────
//  QtNativeTextField — QLineEdit-backed NativeTextField
// ─────────────────────────────────────────────────────────────────────────
//
// One per make_text_field() call. The QLineEdit is parented to the
// Surface so it paints on top of the canvas. We use QPointer to handle
// the case where the Surface tears down before the NativeTextField does.

// QLineEdit subclass that forwards Up / Down / Escape to a popup the field
// drives (the Ctrl+K quick switcher) before falling back to default handling.
class NavLineEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<bool(NavKey)> popup_nav_;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (popup_nav_)
        {
            NavKey nk{};
            bool is_nav = true;
            if (e->key() == Qt::Key_Up)
            {
                nk = NavKey::Up;
            }
            else if (e->key() == Qt::Key_Down)
            {
                nk = NavKey::Down;
            }
            else if (e->key() == Qt::Key_Escape)
            {
                nk = NavKey::Escape;
            }
            else
            {
                is_nav = false;
            }
            auto nav = popup_nav_;
            if (is_nav && nav && nav(nk))
            {
                e->accept();
                return;
            }
        }
        QLineEdit::keyPressEvent(e);
    }
};

class QtNativeTextField : public NativeTextField
{
public:
    explicit QtNativeTextField(QWidget* parent) : edit_(new NavLineEdit(parent))
    {
        edit_->setAttribute(Qt::WA_StyledBackground, true);
        edit_->setFrame(false);
        edit_->setStyleSheet("QLineEdit { background: transparent; }");
        edit_->setFocusPolicy(Qt::ClickFocus);
        edit_->show();

        QObject::connect(edit_, &QLineEdit::textChanged, edit_,
                         [this](const QString& s)
                         {
                             if (on_changed_)
                             {
                                 on_changed_(s.toStdString());
                             }
                         });
        QObject::connect(edit_, &QLineEdit::returnPressed, edit_,
                         [this]()
                         {
                             if (on_submit_)
                             {
                                 on_submit_();
                             }
                         });
    }

    ~QtNativeTextField() override
    {
        if (edit_)
        {
            edit_->deleteLater();
        }
    }

    void set_rect(Rect r) override
    {
        if (!edit_)
        {
            return;
        }
        // Use the widget's preferred height and centre it vertically within
        // the allocated rect rather than stretching to fill it.
        int h = edit_->sizeHint().height();
        int y = static_cast<int>(r.y) + (static_cast<int>(r.h) - h) / 2;
        edit_->setGeometry(static_cast<int>(r.x), y, static_cast<int>(r.w), h);
    }
    void set_text(std::string text) override
    {
        if (!edit_)
        {
            return;
        }
        const QSignalBlocker block(edit_); // don't re-emit on_changed
        edit_->setText(QString::fromStdString(text));
    }
    std::string text() const override
    {
        return edit_ ? edit_->text().toStdString() : std::string{};
    }
    void set_placeholder(std::string text) override
    {
        if (edit_)
        {
            edit_->setPlaceholderText(QString::fromStdString(text));
        }
    }
    void set_focused(bool focused) override
    {
        if (!edit_)
        {
            return;
        }
        if (focused)
        {
            edit_->setFocus();
        }
        else
        {
            edit_->clearFocus();
        }
    }
    void set_visible(bool visible) override
    {
        if (edit_)
        {
            edit_->setVisible(visible);
        }
    }
    void set_enabled(bool enabled) override
    {
        if (edit_)
        {
            edit_->setEnabled(enabled);
        }
    }
    void set_password(bool password) override
    {
        if (edit_)
        {
            edit_->setEchoMode(password ? QLineEdit::Password
                                        : QLineEdit::Normal);
        }
    }
    void set_text_color(Color c) override
    {
        if (!edit_)
            return;
        QPalette pal = edit_->palette();
        QColor qc(c.r, c.g, c.b, c.a);
        pal.setColor(QPalette::Text, qc);
        pal.setColor(QPalette::WindowText, qc);
        QColor ph = qc;
        ph.setAlpha(128);
        pal.setColor(QPalette::PlaceholderText, ph);
        edit_->setPalette(pal);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_popup_nav(std::function<bool(NavKey)> cb) override
    {
        if (edit_)
        {
            edit_->popup_nav_ = std::move(cb);
        }
    }

private:
    QPointer<NavLineEdit> edit_;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
};

// ─────────────────────────────────────────────────────────────────────────
//  QtNativeTextArea — QTextEdit-backed NativeTextArea (multi-line)
// ─────────────────────────────────────────────────────────────────────────
//
// Multi-line variant for the compose bar. Enter submits; Shift+Enter
// inserts a newline. Auto-grow is reported via on_height_changed using
// the document's `documentLayout()->documentSize()` height.

// Custom char-format properties tagging an inline mention pill image so the
// composer can recover {user_id, display_name, is_room} from the document.
enum MentionProp
{
    PropMentionUserId = QTextFormat::UserProperty + 1,
    PropMentionDisplay,
    PropMentionIsRoom,
};

class ComposeTextEdit : public QTextEdit
{
public:
    explicit ComposeTextEdit(QWidget* parent) : QTextEdit(parent)
    {
    }
    std::function<void()> on_return_;
    NativeTextArea::ImagePasteHandler on_image_paste_;
    std::function<bool(NativeTextArea::NavKey)> popup_nav_;
    std::function<bool()> on_edit_last_;

    bool canInsertFromMimeData(const QMimeData* source) const override
    {
        if (on_image_paste_ && source && source->hasImage())
        {
            return true;
        }
        return QTextEdit::canInsertFromMimeData(source);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (popup_nav_)
        {
            NativeTextArea::NavKey nk{};
            bool is_nav = true;
            if (e->key() == Qt::Key_Up)
            {
                nk = NativeTextArea::NavKey::Up;
            }
            else if (e->key() == Qt::Key_Down)
            {
                nk = NativeTextArea::NavKey::Down;
            }
            else if (e->key() == Qt::Key_Left)
            {
                nk = NativeTextArea::NavKey::Left;
            }
            else if (e->key() == Qt::Key_Right)
            {
                nk = NativeTextArea::NavKey::Right;
            }
            else if (e->key() == Qt::Key_Escape)
            {
                nk = NativeTextArea::NavKey::Escape;
            }
            else if (e->key() == Qt::Key_Tab)
            {
                nk = NativeTextArea::NavKey::Tab;
            }
            else if (e->key() == Qt::Key_Backtab)
            {
                nk = NativeTextArea::NavKey::ShiftTab;
            }
            else
            {
                is_nav = false;
            }
            // Copy before calling: Tab calls replace_range() → on_changed_()
            // → hide_shortcode_popup_() → set_on_popup_nav(nullptr), which
            // overwrites popup_nav_._M_functor (the live closure) with zeros.
            // The Tab lambda then calls hide again, reads a null `this` from
            // the zeroed closure storage, and segfaults.  A local copy keeps
            // the closure alive independently of popup_nav_.
            auto nav = popup_nav_;
            if (is_nav && nav && nav(nk))
            {
                e->accept();
                return;
            }
        }
        if (e->key() == Qt::Key_Up && on_edit_last_ && toPlainText().isEmpty())
        {
            if (on_edit_last_())
            {
                e->accept();
                return;
            }
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
            !(e->modifiers() & Qt::ShiftModifier))
        {
            if (on_return_)
            {
                on_return_();
            }
            e->accept();
            return;
        }
        QTextEdit::keyPressEvent(e);
    }

    void insertFromMimeData(const QMimeData* source) override
    {
        // Clipboard image route — bypasses default text/HTML paste so the
        // image lands as an attachment instead of an inline character or
        // garbled HTML.
        if (on_image_paste_ && source && source->hasImage())
        {
            // Prefer a pre-encoded PNG/JPEG payload from the clipboard
            // if one is present (avoids a decode/re-encode round-trip on
            // raw screenshots from apps that share both).
            const char* image_mimes[] = {
                "image/png",
                "image/jpeg",
                "image/webp",
                "image/bmp",
            };
            for (const char* m : image_mimes)
            {
                if (source->hasFormat(QLatin1String(m)))
                {
                    QByteArray ba = source->data(QLatin1String(m));
                    if (!ba.isEmpty())
                    {
                        std::vector<std::uint8_t> bytes(
                            reinterpret_cast<const std::uint8_t*>(
                                ba.constData()),
                            reinterpret_cast<const std::uint8_t*>(
                                ba.constData()) +
                                ba.size());
                        on_image_paste_(std::move(bytes), std::string(m));
                        return;
                    }
                }
            }
            // Fallback — re-encode the QImage variant as PNG.
            QImage img = qvariant_cast<QImage>(source->imageData());
            if (!img.isNull())
            {
                QByteArray ba;
                QBuffer buf(&ba);
                buf.open(QIODevice::WriteOnly);
                if (img.save(&buf, "PNG"))
                {
                    std::vector<std::uint8_t> bytes(
                        reinterpret_cast<const std::uint8_t*>(ba.constData()),
                        reinterpret_cast<const std::uint8_t*>(ba.constData()) +
                            ba.size());
                    on_image_paste_(std::move(bytes), "image/png");
                    return;
                }
            }
            // Fall through to default behaviour if image extraction failed.
        }
        // QTextEdit::insertFromMimeData honours text/html and would render
        // formatting in a plain-text composer. Extract plain text only.
        if (source && source->hasText())
            textCursor().insertText(source->text());
    }
};

class QtNativeTextArea : public NativeTextArea
{
public:
    explicit QtNativeTextArea(QWidget* parent)
        : edit_(new ComposeTextEdit(parent))
    {
        edit_->setAttribute(Qt::WA_StyledBackground, true);
        edit_->setFrameShape(QFrame::NoFrame);
        edit_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        edit_->setLineWrapMode(QTextEdit::WidgetWidth);
        edit_->setStyleSheet("QTextEdit { background: transparent; }");
        edit_->viewport()->setStyleSheet("background: transparent;");
        edit_->show();

        edit_->on_return_ = [this]
        {
            if (on_submit_)
            {
                on_submit_();
            }
        };

        QObject::connect(edit_, &QTextEdit::textChanged, edit_,
                         [this]()
                         {
                             if (!edit_)
                             {
                                 return;
                             }
                             if (on_changed_)
                             {
                                 on_changed_(
                                     edit_->toPlainText().toStdString());
                             }
                             float h = natural_height();
                             if (h != last_height_ && on_height_changed_)
                             {
                                 last_height_ = h;
                                 on_height_changed_(h);
                             }
                         });
    }

    ~QtNativeTextArea() override
    {
        if (edit_)
        {
            edit_->deleteLater();
        }
    }

    void set_rect(Rect r) override
    {
        if (!edit_)
        {
            return;
        }
        const int rx = static_cast<int>(r.x);
        const int ry = static_cast<int>(r.y);
        const int rw = static_cast<int>(r.w);
        const int rh = static_cast<int>(r.h);
        // QTextEdit draws text top-aligned. Apply the target width first
        // so document()->size() (read by natural_height()) reflects
        // wrapping at that width, then size the widget to its content and
        // centre it within the rect — matching the emoji/sticker/send
        // buttons. When content overflows the rect, fill it so the
        // QTextEdit scrolls instead. Mirrors QtNativeTextField::set_rect.
        if (edit_->width() != rw)
        {
            edit_->setGeometry(rx, ry, rw, rh);
        }
        const int nh = static_cast<int>(natural_height());
        const int h = (nh > 0 && nh < rh) ? nh : rh;
        const int y = ry + (rh - h) / 2;
        edit_->setGeometry(rx, y, rw, h);
    }
    void set_text(std::string text) override
    {
        if (!edit_)
        {
            return;
        }
        const QSignalBlocker block(edit_);
        edit_->setPlainText(QString::fromStdString(text));
        float h = natural_height();
        if (h != last_height_ && on_height_changed_)
        {
            last_height_ = h;
            on_height_changed_(h);
        }
    }
    std::string text() const override
    {
        return edit_ ? edit_->toPlainText().toStdString() : std::string{};
    }
    void set_placeholder(std::string text) override
    {
        if (edit_)
        {
            edit_->setPlaceholderText(QString::fromStdString(text));
        }
    }
    void set_focused(bool focused) override
    {
        if (edit_ && focused)
        {
            edit_->setFocus();
        }
    }
    void set_visible(bool visible) override
    {
        visible_ = visible;
        if (edit_)
        {
            edit_->setVisible(visible);
        }
    }
    bool visible() const override
    {
        return visible_;
    }
    void set_enabled(bool enabled) override
    {
        if (edit_)
        {
            edit_->setEnabled(enabled);
        }
    }
    void set_font_role(tk::FontRole role) override
    {
        if (!edit_)
            return;
        const int base = std::max(QApplication::font().pointSize(), 8);
        QFont f; // inherits app family
        f.setPointSize(tk::font_role_pt(role, base));
        f.setWeight(tk::font_role_is_semibold(role) ? QFont::DemiBold
                                                    : QFont::Normal);
        edit_->setFont(f);
    }
    void set_text_color(Color c) override
    {
        if (!edit_)
        {
            return;
        }
        QPalette pal = edit_->palette();
        QColor qc(c.r, c.g, c.b, c.a);
        pal.setColor(QPalette::Text, qc);
        pal.setColor(QPalette::WindowText, qc);
        QColor ph = qc;
        ph.setAlpha(128);
        pal.setColor(QPalette::PlaceholderText, ph);
        edit_->setPalette(pal);
        if (auto* vp = edit_->viewport())
        {
            vp->setPalette(pal);
        }
    }

    float natural_height() const override
    {
        if (!edit_)
        {
            return 0.f;
        }
        QSizeF docSize = edit_->document()->size();
        const auto m = edit_->contentsMargins();
        return static_cast<float>(docSize.height() + m.top() + m.bottom() +
                                  4.0);
    }
    void set_on_changed(std::function<void(const std::string&)> cb) override
    {
        on_changed_ = std::move(cb);
    }
    void set_on_submit(std::function<void()> cb) override
    {
        on_submit_ = std::move(cb);
    }
    void set_on_height_changed(std::function<void(float)> cb) override
    {
        on_height_changed_ = std::move(cb);
    }
    void set_on_image_paste(ImagePasteHandler cb) override
    {
        if (edit_)
        {
            edit_->on_image_paste_ = std::move(cb);
        }
    }
    void insert_at_cursor(std::string text) override
    {
        if (!edit_)
        {
            return;
        }
        QTextCursor cursor = edit_->textCursor();
        cursor.insertText(QString::fromStdString(text));
        edit_->setTextCursor(cursor);
    }

    tk::Rect cursor_rect() const override
    {
        if (!edit_)
        {
            return {};
        }
        QRect cr = edit_->cursorRect();
        QPoint pt =
            edit_->viewport()->mapTo(edit_->parentWidget(), cr.topLeft());
        return {float(pt.x()), float(pt.y()), float(cr.width()),
                float(cr.height())};
    }

    void replace_range(int start, int end, std::string text) override
    {
        if (!edit_)
        {
            return;
        }
        const QSignalBlocker block(edit_);
        QString full = edit_->toPlainText();
        int qt_start = utf8_byte_to_qt_cursor(full, start);
        int qt_end = utf8_byte_to_qt_cursor(full, end);
        QTextCursor cursor(edit_->document());
        cursor.setPosition(qt_start);
        cursor.setPosition(qt_end, QTextCursor::KeepAnchor);
        cursor.insertText(QString::fromStdString(text));
        if (on_changed_)
        {
            on_changed_(edit_->toPlainText().toStdString());
        }
    }

    void set_on_popup_nav(std::function<bool(NavKey)> fn) override
    {
        if (edit_)
        {
            edit_->popup_nav_ = std::move(fn);
        }
    }

    void set_on_edit_last(std::function<bool()> fn) override
    {
        if (edit_)
        {
            edit_->on_edit_last_ = std::move(fn);
        }
    }

    int cursor_byte_pos() const override
    {
        if (!edit_)
        {
            return 0;
        }
        int pos = edit_->textCursor().position();
        QString full = edit_->toPlainText();
        return full.left(pos).toUtf8().size();
    }

    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room) override
    {
        if (!edit_)
        {
            return;
        }
        const QSignalBlocker block(edit_);
        QString full = edit_->toPlainText();
        int qs = utf8_byte_to_qt_cursor(full, start);
        int qe = utf8_byte_to_qt_cursor(full, end);
        QTextCursor cur(edit_->document());
        cur.setPosition(qs);
        cur.setPosition(qe, QTextCursor::KeepAnchor);

        QString disp = QString::fromStdString(display_name);
        QString visual =
            is_room ? QStringLiteral("@room") : (QStringLiteral("@") + disp);

        QString res = QStringLiteral("tesseract-mention://%1")
                          .arg(mention_counter_++);
        edit_->document()->addResource(QTextDocument::ImageResource, QUrl(res),
                                       QVariant(render_pill(visual)));
        QTextImageFormat fmt;
        fmt.setName(res);
        fmt.setVerticalAlignment(QTextCharFormat::AlignBaseline);
        fmt.setProperty(PropMentionUserId, QString::fromStdString(user_id));
        fmt.setProperty(PropMentionDisplay, disp);
        fmt.setProperty(PropMentionIsRoom, is_room);
        cur.insertImage(fmt);
        // Reset to a plain char format so the trailing space and any text the
        // user types next do NOT inherit the image format (which carries the
        // mention properties — otherwise mention_draft() would report the
        // mention repeatedly and the message would render it multiple times).
        QTextCharFormat plain;
        cur.insertText(QStringLiteral(" "), plain);
        edit_->setTextCursor(cur);
        edit_->setCurrentCharFormat(plain);
        if (on_changed_)
        {
            on_changed_(edit_->toPlainText().toStdString());
        }
    }

    std::vector<tesseract::MentionSeg> mention_draft() const override
    {
        std::vector<tesseract::MentionSeg> segs;
        if (!edit_)
        {
            return segs;
        }
        std::string pending;
        auto flush_text = [&]()
        {
            if (!pending.empty())
            {
                tesseract::MentionSeg s;
                s.kind = tesseract::MentionSeg::Kind::Text;
                s.text = pending;
                segs.push_back(std::move(s));
                pending.clear();
            }
        };
        bool first_block = true;
        for (QTextBlock block = edit_->document()->begin(); block.isValid();
             block = block.next())
        {
            if (!first_block)
            {
                pending += "\n";
            }
            first_block = false;
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it)
            {
                QTextFragment frag = it.fragment();
                if (!frag.isValid())
                {
                    continue;
                }
                QTextCharFormat cf = frag.charFormat();
                if (cf.hasProperty(PropMentionDisplay))
                {
                    flush_text();
                    tesseract::MentionSeg s;
                    s.kind = tesseract::MentionSeg::Kind::Mention;
                    s.user_id =
                        cf.property(PropMentionUserId).toString().toStdString();
                    s.display_name =
                        cf.property(PropMentionDisplay).toString().toStdString();
                    s.is_room = cf.property(PropMentionIsRoom).toBool();
                    segs.push_back(std::move(s));
                }
                else
                {
                    // QTextEdit inserts U+2028 LINE SEPARATOR for Shift+Enter
                    // (soft return within a block). toPlainText() normalises it
                    // to '\n', but frag.text() returns the raw codepoint.
                    // Normalise here so markdown_to_html's line-splitter (which
                    // only recognises '\n') sees proper newlines.
                    pending +=
                        frag.text()
                            .replace(QChar(0x2028), QLatin1Char('\n'))
                            .toStdString();
                }
            }
        }
        flush_text();
        return segs;
    }

    void set_mention_colors(Color bg, Color fg) override
    {
        mention_bg_ = QColor(bg.r, bg.g, bg.b, bg.a);
        mention_fg_ = QColor(fg.r, fg.g, fg.b, fg.a);
    }

private:
    static int utf8_byte_to_qt_cursor(const QString& qs, int byte_offset)
    {
        QByteArray full = qs.toUtf8();
        byte_offset = std::clamp(byte_offset, 0, (int)full.size());
        return QString::fromUtf8(full.left(byte_offset)).size();
    }

    QImage render_pill(const QString& text) const
    {
        QFont f = edit_ ? edit_->font() : QFont();
        QFontMetrics fm(f);
        const int pad_x = 8;
        const int pad_y = 2;
        const int w = fm.horizontalAdvance(text) + pad_x * 2;
        const int h = fm.height() + pad_y * 2;
        qreal dpr = edit_ ? edit_->devicePixelRatioF() : 1.0;
        QImage img(QSize(int(w * dpr), int(h * dpr)),
                   QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        QRectF pill(0.5, 0.5, w - 1.0, h - 1.0);
        const qreal r = pill.height() * 0.5;
        p.setPen(Qt::NoPen);
        p.setBrush(mention_bg_);
        p.drawRoundedRect(pill, r, r);
        p.setPen(mention_fg_);
        p.setFont(f);
        p.drawText(QRectF(0, 0, w, h), Qt::AlignCenter, text);
        p.end();
        return img;
    }

    QPointer<ComposeTextEdit> edit_;
    std::function<void(const std::string&)> on_changed_;
    std::function<void()> on_submit_;
    std::function<void(float)> on_height_changed_;
    QColor mention_bg_{0x2E, 0x3B, 0x5E};
    QColor mention_fg_{0xA8, 0xC5, 0xFF};
    int mention_counter_ = 0;
    float last_height_ = 0.f;
    // Tracks the last value passed to set_visible(). Construction-time
    // default mirrors a freshly-created QPlainTextEdit, which is visible
    // until something hides it.
    bool visible_ = true;
};

// ─────────────────────────────────────────────────────────────────────────
//  Host — tk::Host impl + glue between Surface QWidget and the tree
// ─────────────────────────────────────────────────────────────────────────

class Host : public tk::Host, public tk::AnimDamageSink
{
public:
    Host(Surface* surface, const Theme& theme, bool transparent = false)
        : surface_(surface), theme_(&theme), factory_(make_factory()),
          transparent_(transparent)
    {
    }

    void request_repaint() override
    {
        if (surface_)
        {
            surface_->update();
        }
    }

    // Point the damage sink at the shell's animation cache so it can tell
    // animated keys (worth partial-repainting) from static ones.
    void set_anim_cache(const AnimImageCache* cache)
    {
        anim_cache_ = cache;
    }

    // AnimDamageSink: record the on-screen rect of an animated image drawn
    // during the current paint pass. Static images (not in the anim cache)
    // are ignored — only animated content needs per-frame invalidation.
    void note_image(const std::string& key, Rect world) override
    {
        if (anim_cache_ && anim_cache_->has(key))
        {
            anim_damage_.push_back(world);
        }
    }

    // Invalidate just the regions occupied by animated images on the last
    // paint, instead of the whole surface. Falls back to a full repaint when
    // no animated rects were recorded (e.g. the first frame after a GIF is
    // decoded, before it has been painted once).
    void invalidate_anim_damage()
    {
        if (!surface_)
        {
            return;
        }
        if (anim_damage_.empty())
        {
            surface_->update();
            return;
        }
        for (const auto& r : anim_damage_)
        {
            const int x = static_cast<int>(std::floor(r.x));
            const int y = static_cast<int>(std::floor(r.y));
            const int w = static_cast<int>(std::ceil(r.x + r.w)) - x;
            const int h = static_cast<int>(std::ceil(r.y + r.h)) - y;
            // Pad by 1px so anti-aliased edges are not clipped.
            surface_->update(QRect(x - 1, y - 1, w + 2, h + 2));
        }
    }

    void post_to_ui(std::function<void()> task) override
    {
        // QMetaObject::invokeMethod with a queued connection lands `task`
        // on the QObject's thread (the GUI thread, for surface_).
        if (!surface_)
        {
            return;
        }
        QMetaObject::invokeMethod(surface_, std::move(task),
                                  Qt::QueuedConnection);
    }

    void post_delayed(int ms, std::function<void()> fn) override
    {
        // surface_ is the context object: if it's destroyed before the
        // timer fires, Qt cancels the callback automatically.
        if (!surface_)
        {
            return;
        }
        QTimer::singleShot(ms, surface_, std::move(fn));
    }

    std::unique_ptr<NativeTextField> make_text_field() override
    {
        if (!surface_)
        {
            return nullptr;
        }
        return std::make_unique<QtNativeTextField>(surface_);
    }

    std::unique_ptr<NativeTextArea> make_text_area() override
    {
        if (!surface_)
        {
            return nullptr;
        }
        return std::make_unique<QtNativeTextArea>(surface_);
    }

    std::unique_ptr<AudioPlayer> make_audio_player() override;
    std::unique_ptr<AudioCapture> make_audio_capture() override;
    std::unique_ptr<VideoPlayer> make_video_player() override;
#ifdef TESSERACT_CALLS_ENABLED
    std::unique_ptr<AudioPlayback> make_audio_playback() override;
#endif

    std::vector<tk::DeviceListing> enumerate_audio_inputs()  const override;
    std::vector<tk::DeviceListing> enumerate_audio_outputs() const override;
    std::vector<tk::DeviceListing> enumerate_cameras()       const override;

    EncodedImage encode_for_send(const std::uint8_t* data, std::size_t len,
                                 bool compress) override
    {
        EncodedImage out{};
        if (!data || len == 0)
        {
            return out;
        }

        // Decode once so we can read the size + mime even on the
        // pass-through path. QImageReader sniffs format from the bytes.
        QByteArray src(reinterpret_cast<const char*>(data),
                       static_cast<int>(len));
        QBuffer src_buf(&src);
        src_buf.open(QIODevice::ReadOnly);
        QImageReader reader(&src_buf);
        QImage img = reader.read();
        if (img.isNull())
        {
            return out;
        }

        const int src_w = img.width();
        const int src_h = img.height();
        QByteArray fmt = reader.format();

        if (!compress)
        {
            out.bytes.assign(
                reinterpret_cast<const std::uint8_t*>(src.constData()),
                reinterpret_cast<const std::uint8_t*>(src.constData()) +
                    src.size());
            if (fmt == "png")
            {
                out.mime = "image/png";
            }
            else if (fmt == "jpeg" || fmt == "jpg")
            {
                out.mime = "image/jpeg";
            }
            else if (fmt == "webp")
            {
                out.mime = "image/webp";
            }
            else if (fmt == "bmp")
            {
                out.mime = "image/bmp";
            }
            else if (fmt == "gif")
            {
                out.mime = "image/gif";
            }
            else
            {
                out.mime = "image/" +
                           std::string(fmt.constData(),
                                       static_cast<std::size_t>(fmt.size()));
            }
            out.width = static_cast<std::uint32_t>(src_w);
            out.height = static_cast<std::uint32_t>(src_h);
            return out;
        }

        // Cap to 1600×1200, preserving aspect ratio.
        constexpr int kMaxW = 1600;
        constexpr int kMaxH = 1200;
        if (src_w > kMaxW || src_h > kMaxH)
        {
            img = img.scaled(kMaxW, kMaxH, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        }

        QByteArray dst;
        QBuffer dst_buf(&dst);
        dst_buf.open(QIODevice::WriteOnly);
        QImageWriter writer(&dst_buf, "JPEG");
        writer.setQuality(75);
        if (!writer.write(img))
        {
            return EncodedImage{};
        }

        out.bytes.assign(
            reinterpret_cast<const std::uint8_t*>(dst.constData()),
            reinterpret_cast<const std::uint8_t*>(dst.constData()) +
                dst.size());
        out.mime = "image/jpeg";
        out.width = static_cast<std::uint32_t>(img.width());
        out.height = static_cast<std::uint32_t>(img.height());
        return out;
    }

    void set_clipboard_text(std::string_view text) override
    {
        QGuiApplication::clipboard()->setText(
            QString::fromUtf8(text.data(), static_cast<int>(text.size())));
    }

    // ── Internal accessors used by Surface ────────────────────────────
    void set_root(std::unique_ptr<Widget> root)
    {
        root_ = std::move(root);
        root_->set_subtree_removing_cb([this](Widget* s){ on_subtree_removing(s); });
        relayout();
    }
    Widget* root() const
    {
        return root_.get();
    }
    const Theme& theme() const
    {
        return *theme_;
    }
    void set_theme(const Theme& t)
    {
        theme_ = &t;
    }
    CanvasFactory& factory()
    {
        return *factory_;
    }

    void relayout()
    {
        if (!root_ || !surface_)
        {
            return;
        }
        LayoutCtx ctx{*factory_, *theme_};
        Rect bounds{0, 0, static_cast<float>(surface_->width()),
                    static_cast<float>(surface_->height())};
        root_->measure(ctx, {bounds.w, bounds.h});
        root_->arrange(ctx, bounds);
        if (on_layout_)
        {
            on_layout_();
        }
    }

    void set_on_layout(std::function<void()> cb)
    {
        on_layout_ = std::move(cb);
    }

    void paint(QPainter& painter)
    {
        if (!root_)
        {
            return;
        }
        auto canvas = make_canvas(painter);
        canvas->clear(transparent_ ? Color{0, 0, 0, 0} : theme_->palette.bg);
        anim_damage_.clear();
        pending_popup_ = nullptr;
        PaintCtx ctx{*canvas, *factory_, *theme_, this, this};
        root_->paint(ctx);
        popup_ = pending_popup_;
        root_->paint_overlay(ctx);
    }

    // Pointer-event entry points. Each translates the native event to a
    // world-space Point (already done by the Surface) and forwards to the
    // shared Host::dispatch_pointer_* state machine.
    void on_pointer_down(Point local) { dispatch_pointer_down(local); }

    void on_right_click(Point local)
    {
        if (root_)
            root_->dispatch_right_click(local);
    }

    void on_pointer_up(Point local) { dispatch_pointer_up(local); }

    void on_pointer_move(Point local) { dispatch_pointer_move(local); }

    void on_pointer_leave() { dispatch_pointer_leave(); }

    void on_wheel(Point local, float dx, float dy)
    {
        fire_user_activity_();
        if (!root_)
        {
            return;
        }
        if (dispatch_wheel(local, dx, dy))
        {
            request_repaint();
            on_pointer_move(local);
        }
    }

    void detach_surface()
    {
        surface_ = nullptr;
    }

protected:
    Widget* input_root_() const override { return root_.get(); }

private:
    Surface* surface_;
    const Theme* theme_;
    std::unique_ptr<CanvasFactory> factory_;
    bool transparent_ = false;
    std::unique_ptr<Widget> root_;
    std::function<void()> on_layout_;
    const AnimImageCache* anim_cache_ = nullptr;
    std::vector<Rect> anim_damage_;
};

// ─────────────────────────────────────────────────────────────────────────
//  Surface — QWidget that owns the host + dispatches events
// ─────────────────────────────────────────────────────────────────────────

Surface::Surface(const Theme& theme, QWidget* parent, bool transparent)
    : QWidget(parent), host_(std::make_unique<Host>(this, theme, transparent))
{
    if (transparent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
    }
    else
    {
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }
    setMouseTracking(true);
}

Surface::~Surface()
{
    if (host_)
    {
        host_->detach_surface();
    }
}

tk::Host& Surface::host()
{
    return *host_;
}
const Theme& Surface::theme() const
{
    return host_->theme();
}

void Surface::set_root(std::unique_ptr<Widget> root)
{
    host_->set_root(std::move(root));
    update();
}

Widget* Surface::root() const
{
    return host_->root();
}

void Surface::relayout()
{
    host_->relayout();
    update();
}

void Surface::set_theme(const Theme& t)
{
    host_->set_theme(t);
    relayout();
}

void Surface::set_anim_cache(const AnimImageCache* cache)
{
    host_->set_anim_cache(cache);
}

void Surface::update_anim_regions()
{
    host_->invalidate_anim_damage();
}

void Surface::set_on_layout(std::function<void()> cb)
{
    host_->set_on_layout(std::move(cb));
}

void Surface::set_on_file_drop(FileDropHandler cb)
{
    on_file_drop_ = std::move(cb);
    setAcceptDrops(static_cast<bool>(on_file_drop_));
}

void Surface::set_on_file_drop_error(FileDropErrorHandler cb)
{
    on_file_drop_error_ = std::move(cb);
}

namespace
{

bool drop_is_acceptable(const QMimeData* md)
{
    if (!md)
    {
        return false;
    }
    if (md->hasUrls())
    {
        for (const QUrl& u : md->urls())
        {
            if (u.isLocalFile())
            {
                return true;
            }
        }
    }
    return md->hasImage();
}

} // namespace

void Surface::paintEvent(QPaintEvent* ev)
{
    QPainter painter(this);
    const QRect dirty = ev->rect();
    if (!dirty.isEmpty())
    {
        painter.setClipRect(dirty, Qt::IntersectClip);
    }
    host_->paint(painter);

    if (drag_active_)
    {
        painter.save();
        painter.setClipping(false);
        // Translucent accent fill + dashed border + centred "Drop to
        // attach" label. Painted last so it sits above the widget tree.
        const QPalette& pal = palette();
        QColor accent = pal.color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlpha(28);
        QColor stroke = accent;
        stroke.setAlpha(192);
        const qreal inset = 8.0;
        const QRectF area = rect().adjusted(inset, inset, -inset, -inset);
        if (area.width() > 0 && area.height() > 0)
        {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.fillRect(area, fill);
            QPen pen(stroke, 2.0, Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(area, 12.0, 12.0);

            QFont f = painter.font();
            f.setBold(true);
            painter.setFont(f);
            painter.setPen(pal.color(QPalette::HighlightedText).alpha() != 0
                               ? pal.color(QPalette::HighlightedText)
                               : accent);
            painter.drawText(area, Qt::AlignCenter,
                             QStringLiteral("Drop to attach"));
        }
        painter.restore();
    }
}

void Surface::resizeEvent(QResizeEvent*)
{
    host_->relayout();
}

void Surface::mousePressEvent(QMouseEvent* e)
{
    tk::Point pt{static_cast<float>(e->position().x()),
                 static_cast<float>(e->position().y())};
    if (e->button() == Qt::LeftButton)
    {
        host_->on_pointer_down(pt);
        // Accept so Qt sets the implicit mouse grab on this surface: without
        // accepting, Qt propagates the press to the parent and the implicit
        // grab is not assigned here, meaning the corresponding release event
        // goes to a different widget and on_pointer_up is never called.
        e->accept();
    }
    else if (e->button() == Qt::RightButton)
    {
        host_->on_right_click(pt);
        e->accept();
    }
    else
    {
        QWidget::mousePressEvent(e);
    }
}

void Surface::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
    {
        host_->on_pointer_up({static_cast<float>(e->position().x()),
                              static_cast<float>(e->position().y())});
    }
    QWidget::mouseReleaseEvent(e);
}

void Surface::mouseMoveEvent(QMouseEvent* e)
{
    host_->on_pointer_move({static_cast<float>(e->position().x()),
                            static_cast<float>(e->position().y())});
    QWidget::mouseMoveEvent(e);
}

void Surface::wheelEvent(QWheelEvent* e)
{
    // Toolkit convention: positive dy = scroll content *down*.
    // Qt's positive angleDelta.y = scroll *up*, so we invert.
    QPointF pos = e->position();
    float dx, dy;
    QPoint pd = e->pixelDelta();
    if (!pd.isNull())
    {
        // Smooth-scroll device (trackpad): pixel delta is already in px.
        dx = static_cast<float>(pd.x());
        dy = -static_cast<float>(pd.y());
    }
    else
    {
        // Physical mouse wheel: angleDelta in 1/8-degree units, 120 per notch.
        // Scale 0.75 px/unit → 90 px per standard notch, matching Win32.
        QPoint ad = e->angleDelta();
        dx = static_cast<float>(ad.x()) * 0.75f;
        dy = -static_cast<float>(ad.y()) * 0.75f;
    }
    host_->on_wheel({static_cast<float>(pos.x()), static_cast<float>(pos.y())},
                    dx, dy);
    e->accept();
}

void Surface::leaveEvent(QEvent* e)
{
    host_->on_pointer_leave();
    QWidget::leaveEvent(e);
}

void Surface::dragEnterEvent(QDragEnterEvent* e)
{
    if (on_file_drop_ && drop_is_acceptable(e->mimeData()))
    {
        e->setDropAction(Qt::CopyAction);
        e->acceptProposedAction();
        if (!drag_active_)
        {
            drag_active_ = true;
            update();
        }
    }
    else
    {
        e->ignore();
    }
}

void Surface::dragMoveEvent(QDragMoveEvent* e)
{
    if (on_file_drop_ && drop_is_acceptable(e->mimeData()))
    {
        e->setDropAction(Qt::CopyAction);
        e->acceptProposedAction();
    }
    else
    {
        e->ignore();
    }
}

void Surface::dragLeaveEvent(QDragLeaveEvent*)
{
    if (drag_active_)
    {
        drag_active_ = false;
        update();
    }
}

void Surface::dropEvent(QDropEvent* e)
{
    const bool was_active = drag_active_;
    drag_active_ = false;
    if (was_active)
    {
        update();
    }

    if (!on_file_drop_)
    {
        e->ignore();
        return;
    }
    const QMimeData* md = e->mimeData();
    if (!md)
    {
        e->ignore();
        return;
    }

    bool handled = false;

    // 1) File drops — iterate over every dropped local file. Each fires
    // the handler separately; the shell dispatches by mime.
    if (md->hasUrls())
    {
        QMimeDatabase db;
        for (const QUrl& u : md->urls())
        {
            if (!u.isLocalFile())
            {
                continue;
            }
            const QString path = u.toLocalFile();
            const QFileInfo fi(path);
            if (!fi.isFile())
            {
                continue;
            }
            const qint64 size = fi.size();
            if (size <= 0 ||
                static_cast<std::size_t>(size) > kMaxDroppedFileBytes)
            {
                continue;
            }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
            {
                if (on_file_drop_error_)
                    on_file_drop_error_(f.errorString().toStdString());
                continue;
            }
            const QByteArray ba = f.readAll();
            if (ba.isEmpty())
            {
                continue;
            }

            // Sniff mime by content + name. Fall back to
            // application/octet-stream so the handler still fires.
            QString mime = db.mimeTypeForFileNameAndData(path, ba).name();
            if (mime.isEmpty())
            {
                mime = QStringLiteral("application/octet-stream");
            }

            std::vector<std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(ba.constData()),
                reinterpret_cast<const std::uint8_t*>(ba.constData()) +
                    ba.size());
            on_file_drop_(std::move(bytes), mime.toStdString(),
                          fi.fileName().toStdString());
            handled = true;
        }
    }

    // 2) In-app image data (e.g. drag from Firefox / a Qt app). Encode
    // as PNG — mirrors the clipboard-paste fallback. Only fires when no
    // url-bearing files were forwarded.
    if (!handled && md->hasImage())
    {
        const QImage img = qvariant_cast<QImage>(md->imageData());
        if (!img.isNull())
        {
            QByteArray out;
            QBuffer buf(&out);
            buf.open(QIODevice::WriteOnly);
            if (img.save(&buf, "PNG"))
            {
                std::vector<std::uint8_t> bytes(
                    reinterpret_cast<const std::uint8_t*>(out.constData()),
                    reinterpret_cast<const std::uint8_t*>(out.constData()) +
                        out.size());
                on_file_drop_(std::move(bytes), "image/png", std::string{});
                handled = true;
            }
        }
    }

    if (handled)
    {
        e->setDropAction(Qt::CopyAction);
        e->acceptProposedAction();
    }
    else
    {
        e->ignore();
    }
}

// Defined in audio_qt.cpp; wired here so Host::make_audio_player() lives
// next to the rest of the host implementation.
std::unique_ptr<tk::AudioPlayer> make_audio_player_qt();

std::unique_ptr<tk::AudioPlayer> Host::make_audio_player()
{
    return make_audio_player_qt();
}

// Defined in audio_capture_qt.cpp.
std::unique_ptr<tk::AudioCapture>
make_audio_capture_qt(tk::AudioCapturePostFn post);

std::unique_ptr<tk::AudioCapture> Host::make_audio_capture()
{
    return make_audio_capture_qt(
        [this](std::function<void()> fn) { post_to_ui(std::move(fn)); });
}

#ifdef TESSERACT_CALLS_ENABLED
// Defined in audio_playback_qt.cpp (in namespace tk::qt6).
std::unique_ptr<::tk::AudioPlayback> make_audio_playback_qt();

std::unique_ptr<::tk::AudioPlayback> Host::make_audio_playback()
{
    return make_audio_playback_qt();
}
#endif

// Defined in video_qt.cpp.
std::unique_ptr<tk::VideoPlayer> make_video_player_qt();

std::unique_ptr<tk::VideoPlayer> Host::make_video_player()
{
    return make_video_player_qt();
}

std::vector<tk::DeviceListing> Host::enumerate_audio_inputs() const
{
    std::vector<tk::DeviceListing> result;
    for (const QAudioDevice& dev : QMediaDevices::audioInputs())
    {
        tk::DeviceListing entry;
        entry.id           = dev.id().toStdString();
        entry.display_name = dev.description().toStdString();
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<tk::DeviceListing> Host::enumerate_audio_outputs() const
{
    std::vector<tk::DeviceListing> result;
    for (const QAudioDevice& dev : QMediaDevices::audioOutputs())
    {
        tk::DeviceListing entry;
        entry.id           = dev.id().toStdString();
        entry.display_name = dev.description().toStdString();
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<tk::DeviceListing> Host::enumerate_cameras() const
{
    tk::gst::ensure_gst_init();

    std::vector<tk::DeviceListing> result;
    GstDeviceMonitor* monitor = gst_device_monitor_new();
    GstCaps* caps = gst_caps_new_empty_simple("video/x-raw");
    gst_device_monitor_add_filter(monitor, "Video/Source", caps);
    gst_caps_unref(caps);

    if (!gst_device_monitor_start(monitor))
    {
        gst_object_unref(monitor);
        return result;
    }

    GList* devices = gst_device_monitor_get_devices(monitor);
    for (GList* l = devices; l; l = l->next)
    {
        GstDevice* dev = GST_DEVICE(l->data);

        // Create a temporary element to read the "device" property
        // (the /dev/videoN path suitable for injection into the pipeline).
        GstElement* elem = gst_device_create_element(dev, nullptr);
        if (!elem) { gst_object_unref(dev); continue; }

        gchar* dev_path = nullptr;
        g_object_get(elem, "device", &dev_path, nullptr);
        gst_object_unref(elem);

        if (!dev_path) { gst_object_unref(dev); continue; }

        gchar* display = gst_device_get_display_name(dev);
        tk::DeviceListing entry;
        entry.id           = dev_path;
        entry.display_name = display ? display : dev_path;
        result.push_back(std::move(entry));

        g_free(dev_path);
        g_free(display);
        gst_object_unref(dev);
    }
    g_list_free(devices);

    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return result;
}

} // namespace tk::qt6
