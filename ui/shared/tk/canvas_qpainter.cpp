#include "canvas_qpainter.h"

#include <tesseract/settings.h>

#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtGui/QStaticText>
#include <QtGui/QTextLayout>
#include <QtGui/QTextOption>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QPalette>
#include <QtCore/QRectF>
#include <QtCore/QSizeF>
#include <QtCore/QString>

#include <QtCore/QBuffer>
#include <QtGui/QImageReader>

#include <array>
#include <cmath>
#include <list>
#include <unordered_map>
#include <utility>

namespace tk::qt6
{

namespace
{

QColor to_qcolor(Color c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

QRectF to_qrect(Rect r)
{
    return QRectF(r.x, r.y, r.w, r.h);
}

// Map FontRole → QFont. Pulls the application default family (whatever
// QApplication::font() resolved to via the platform theme) and modifies
// the size + weight per role. Sizes come from tesseract::Settings.
QFont font_for(FontRole role)
{
    const auto& s = tesseract::Settings::instance();
    QFont f;
    int pt = s.font_body;
    switch (role)
    {
    case FontRole::Small:
        pt = s.font_small;
        break;
    case FontRole::Body:
        pt = s.font_body;
        break;
    case FontRole::SenderName:
        pt = s.font_sender_name;
        break;
    case FontRole::Timestamp:
        pt = s.font_timestamp;
        break;
    case FontRole::SidebarName:
        pt = s.font_sidebar_name;
        break;
    case FontRole::SidebarPreview:
        pt = s.font_sidebar_preview;
        break;
    case FontRole::UnreadBadge:
        pt = s.font_unread_badge;
        break;
    case FontRole::Title:
        pt = s.font_title;
        break;
    case FontRole::UiSemibold:
        pt = s.font_ui_semibold;
        break;
    case FontRole::BigEmoji:
        pt = s.font_big_emoji;
        break;
    case FontRole::EmojiPickerCell:
        pt = s.font_emoji_picker_cell;
        break;
    }
    f.setPointSize(pt);
    f.setWeight(font_role_is_semibold(role) ? QFont::DemiBold : QFont::Normal);
    return f;
}

// The word-split policy is shared (tk::initials_of); apply Qt's locale-aware
// uppercasing to the result before drawing.
QString initials_upper(std::string_view name)
{
    std::string base = initials_of(name);
    return QString::fromUtf8(base.data(), static_cast<int>(base.size()))
        .toUpper();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  QtImage — tk::Image
// ─────────────────────────────────────────────────────────────────────────

class QtImage : public Image
{
public:
    explicit QtImage(QImage image) : image_(std::move(image))
    {
    }
    int width() const override
    {
        return image_.width();
    }
    int height() const override
    {
        return image_.height();
    }
    std::size_t memory_bytes() const override
    {
        std::size_t bytes = static_cast<std::size_t>(image_.sizeInBytes());
        for (const auto& e : cache_)
        {
            bytes += static_cast<std::size_t>(e.img.sizeInBytes());
        }
        return bytes;
    }
    const QImage& image() const
    {
        return image_;
    }

    // Return a copy of `image_` pre-scaled to (target_w × target_h) DEVICE
    // pixels via Qt::SmoothTransformation, with devicePixelRatio set to
    // `dpr`. Drawn at (target_w/dpr × target_h/dpr) logical pixels, QPainter
    // sees a deviceIndependentSize match and skips its own (bilinear-only)
    // resampling — the result is the multi-step area filter Qt applies in
    // QImage::scaled, which approaches cubic quality on >2× downscales.
    //
    // The result is memoised on the QtImage with a tiny LRU; sticker grids
    // and avatar lists redraw the same image at the same size every frame,
    // so a 4-entry cap is plenty and keeps per-image memory bounded.
    const QImage& scaled_for(int target_w, int target_h, qreal dpr) const
    {
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
        {
            if (it->w == target_w && it->h == target_h)
            {
                if (it != cache_.begin())
                {
                    cache_.splice(cache_.begin(), cache_, it);
                }
                return cache_.front().img;
            }
        }
        QImage scaled = image_.scaled(target_w, target_h, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        cache_.push_front(Entry{target_w, target_h, std::move(scaled)});
        if (cache_.size() > kCacheLimit)
        {
            cache_.pop_back();
        }
        return cache_.front().img;
    }

private:
    static constexpr std::size_t kCacheLimit = 4;
    struct Entry
    {
        int w;
        int h;
        QImage img;
    };

    QImage image_;
    mutable std::list<Entry> cache_;
};

// ─────────────────────────────────────────────────────────────────────────
//  QtTextLayoutBase — shared dispatch base for plain and rich layouts
// ─────────────────────────────────────────────────────────────────────────

class QtTextLayoutBase : public TextLayout
{
public:
    virtual void draw(QPainter&, Point origin, Color) const = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  QtTextLayout — plain-text tk::TextLayout
// ─────────────────────────────────────────────────────────────────────────
//
// Holds the immutable inputs needed to draw the text inside a rect:
// the string, the QFont (sized + weighted by FontRole), and a
// QTextOption (alignment + wrap). measure() returns the size the text
// occupies inside max_width; draw goes through QPainter::drawText with
// the same rect + option so what we measured is what we render.

class QtTextLayout : public QtTextLayoutBase
{
public:
    QtTextLayout(QString text, QFont font, QTextOption option, QSizeF measured,
                 int line_count, qreal max_width, qreal max_height,
                 bool elide_single_line, qreal ascent, qreal explicit_max_height)
        : text_(std::move(text)), font_(std::move(font)), option_(option),
          measured_(measured), line_count_(line_count), max_width_(max_width),
          max_height_(max_height), elide_single_line_(elide_single_line),
          ascent_(ascent), explicit_max_height_(explicit_max_height)
    {
        // Pre-build QStaticText for single-line (non-wrap) layouts so draw()
        // submits pre-shaped glyph data instead of re-shaping on every frame.
        if (option_.wrapMode() != QTextOption::WordWrap)
        {
            QString drawn;
            if (elide_single_line_)
            {
                QFontMetricsF fm(font_);
                const qreal draw_w =
                    max_width_ > 0 ? max_width_ : measured_.width();
                drawn = fm.elidedText(text_, Qt::ElideRight, draw_w);
            }
            else
            {
                drawn = text_;
            }
            static_text_ = QStaticText(drawn);
            static_text_.setTextFormat(Qt::PlainText);
            static_text_.setPerformanceHint(QStaticText::AggressiveCaching);
        }
    }

    Size measure() const override
    {
        return Size{static_cast<float>(measured_.width()),
                    static_cast<float>(measured_.height())};
    }
    int line_count() const override
    {
        return line_count_;
    }
    float ascent() const override
    {
        return static_cast<float>(ascent_);
    }

    void draw(QPainter& p, Point origin, Color c) const override
    {
        p.save();
        p.setFont(font_);
        p.setPen(to_qcolor(c));

        if (!static_text_.text().isNull())
        {
            // When an explicit bounding rect is available and vertical centering
            // is requested, let Qt handle both axes natively via drawText(rect).
            // drawStaticText ignores QTextOption alignment for the vertical axis,
            // so this is the only reliable path for VCenter.
            const Qt::Alignment valign =
                option_.alignment() & Qt::AlignVertical_Mask;
            if (valign == Qt::AlignVCenter && explicit_max_height_ > 0)
            {
                p.drawText(
                    QRectF(origin.x, origin.y, max_width_, explicit_max_height_),
                    static_cast<int>(option_.alignment()), text_);
            }
            else
            {
                // Non-wrap path: submit pre-shaped glyph data via QStaticText.
                // Adjust x for non-leading horizontal alignment.
                qreal draw_x = origin.x;
                const Qt::Alignment halign =
                    option_.alignment() & Qt::AlignHorizontal_Mask;
                if (halign == Qt::AlignHCenter || halign == Qt::AlignRight)
                {
                    const qreal avail =
                        max_width_ > 0 ? max_width_ : measured_.width();
                    const qreal tw = measured_.width();
                    draw_x += (halign == Qt::AlignHCenter) ? (avail - tw) * 0.5
                                                           : (avail - tw);
                }
                p.drawStaticText(QPointF(draw_x, origin.y), static_text_);
            }
        }
        else
        {
            // Wrap (multi-line) path: use the pre-built QTextLayout so line
            // positions are not recomputed on every paint.
            ensure_ql_();
            ql_->draw(&p, QPointF(origin.x, origin.y));
        }
        p.restore();
    }

    int char_index_at(tk::Point local) const override
    {
        if (elide_single_line_)
            return -1;
        ensure_ql_();
        if (!ql_ || ql_->lineCount() == 0)
            return -1;
        int target = ql_->lineCount() - 1;
        for (int i = 0; i < ql_->lineCount(); ++i)
        {
            QTextLine ln = ql_->lineAt(i);
            if (local.y < ln.position().y() + ln.height())
            {
                target = i;
                break;
            }
        }
        QTextLine ln = ql_->lineAt(target);
        int qt16 = ln.xToCursor(static_cast<qreal>(local.x),
                                 QTextLine::CursorBetweenCharacters);
        return qt16_to_utf8_(qt16);
    }

    std::vector<tk::Rect> selection_rects(int start_byte,
                                          int end_byte) const override
    {
        if (start_byte >= end_byte || elide_single_line_)
            return {};
        ensure_ql_();
        if (!ql_)
            return {};
        int qs = utf8_to_qt16_(start_byte);
        int qe = utf8_to_qt16_(end_byte);
        if (qs >= qe)
            return {};
        std::vector<tk::Rect> out;
        for (int i = 0; i < ql_->lineCount(); ++i)
        {
            QTextLine ln = ql_->lineAt(i);
            int ls = ln.textStart();
            int le = ls + ln.textLength();
            int cs = std::max(qs, ls);
            int ce = std::min(qe, le);
            if (cs >= ce)
                continue;
            qreal x1 = ln.cursorToX(cs, QTextLine::Leading);
            qreal x2 = ln.cursorToX(ce, QTextLine::Leading);
            if (x1 > x2)
                std::swap(x1, x2);
            out.push_back({static_cast<float>(x1),
                           static_cast<float>(ln.position().y()),
                           static_cast<float>(x2 - x1),
                           static_cast<float>(ln.height())});
        }
        return out;
    }

    std::string text_range(int start_byte, int end_byte) const override
    {
        if (start_byte >= end_byte)
            return {};
        QByteArray utf8 = text_.toUtf8();
        int lo = std::max(0, start_byte);
        int hi = std::min(end_byte, static_cast<int>(utf8.size()));
        if (lo >= hi)
            return {};
        return std::string(utf8.constData() + lo,
                           static_cast<std::size_t>(hi - lo));
    }

private:
    void ensure_ql_() const
    {
        if (ql_)
            return;
        // QTextLayout does not break on '\n'; it only honors
        // QChar::LineSeparator (U+2028). Substitute so hard breaks render.
        // text_ keeps real '\n' so byte/QChar index mapping and clipboard copy
        // remain correct (both are a single QChar at identical indices).
        QString laid = text_;
        laid.replace(QLatin1Char('\n'), QChar(QChar::LineSeparator));
        ql_ = std::make_unique<QTextLayout>(laid, font_);
        ql_->setTextOption(option_);
        ql_->beginLayout();
        qreal y = 0;
        qreal w = max_width_ > 0 ? max_width_ : measured_.width();
        while (true)
        {
            QTextLine ln = ql_->createLine();
            if (!ln.isValid())
                break;
            ln.setLineWidth(w);
            ln.setPosition(QPointF(0, y));
            y += ln.height();
        }
        ql_->endLayout();
    }

    int qt16_to_utf8_(int qt16) const
    {
        if (qt16 <= 0)
            return 0;
        int clamped = std::min(qt16, static_cast<int>(text_.size()));
        return static_cast<int>(text_.left(clamped).toUtf8().size());
    }

    int utf8_to_qt16_(int utf8_byte) const
    {
        if (utf8_byte <= 0)
            return 0;
        QByteArray ba = text_.toUtf8();
        int clamped = std::min(utf8_byte, static_cast<int>(ba.size()));
        return static_cast<int>(
            QString::fromUtf8(ba.constData(), clamped).size());
    }

    QString text_;
    QFont font_;
    QTextOption option_;
    QSizeF measured_;
    int line_count_ = 0;
    qreal max_width_ = -1;
    qreal max_height_ = -1;
    bool elide_single_line_ = false;
    qreal ascent_ = 0;
    qreal explicit_max_height_ = -1;
    mutable std::unique_ptr<QTextLayout> ql_;
    QStaticText static_text_;
};

// ─────────────────────────────────────────────────────────────────────────
//  QtCanvas — tk::Canvas
// ─────────────────────────────────────────────────────────────────────────

class QtCanvas : public Canvas
{
public:
    explicit QtCanvas(QPainter& p) : p_(p)
    {
        p_.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                          QPainter::SmoothPixmapTransform);
    }

    void clear(Color c) override
    {
        QRectF r(0, 0, p_.device()->width() / p_.device()->devicePixelRatioF(),
                 p_.device()->height() / p_.device()->devicePixelRatioF());
        const auto prev = p_.compositionMode();
        p_.setCompositionMode(QPainter::CompositionMode_Source);
        p_.fillRect(r, to_qcolor(c));
        p_.setCompositionMode(prev);
    }

    void fill_rect(Rect r, Color c) override
    {
        p_.fillRect(to_qrect(r), to_qcolor(c));
    }

    void fill_rounded_rect(Rect r, float radius, Color c) override
    {
        p_.save();
        p_.setBrush(to_qcolor(c));
        p_.setPen(Qt::NoPen);
        p_.drawRoundedRect(to_qrect(r), radius, radius);
        p_.restore();
    }

    void stroke_rect(Rect r, Color c, float width) override
    {
        p_.save();
        p_.setBrush(Qt::NoBrush);
        p_.setPen(QPen(to_qcolor(c), width));
        // Inset by half to mirror D2D's "centred on path" stroke semantics
        // so 1 px lines land at integer pixel boundaries when origin is integer.
        qreal half = width * 0.5;
        p_.drawRect(QRectF(r.x + half, r.y + half, r.w - width, r.h - width));
        p_.restore();
    }

    void stroke_rounded_rect(Rect r, float radius, Color c,
                             float width) override
    {
        p_.save();
        p_.setBrush(Qt::NoBrush);
        p_.setPen(QPen(to_qcolor(c), width));
        qreal half = width * 0.5;
        p_.drawRoundedRect(
            QRectF(r.x + half, r.y + half, r.w - width, r.h - width), radius,
            radius);
        p_.restore();
    }

    void draw_image(const Image& image, Rect dst) override
    {
        const auto& qi = static_cast<const QtImage&>(image);
        p_.drawImage(to_qrect(dst), pick_image_(qi, dst.w, dst.h));
    }

    void draw_image_subregion(const Image& image, Rect src, Rect dst) override
    {
        // Subregion blits go through the unscaled source — the LRU keys
        // on whole-image dimensions, so caching pre-clipped variants would
        // explode the working set. Relies on SmoothPixmapTransform (bilinear).
        const auto& qi = static_cast<const QtImage&>(image);
        p_.drawImage(to_qrect(dst), qi.image(), to_qrect(src));
    }

    void draw_circle_image(const Image& image, Point centre,
                           float diameter) override
    {
        const auto& qi = static_cast<const QtImage&>(image);
        static std::unordered_map<int, QPainterPath> s_paths;
        // Key on half-pixel units so distinct rendered sizes get distinct paths.
        const int key =
            static_cast<int>(std::round(static_cast<qreal>(diameter) * 2.0));
        auto& path = s_paths[key];
        if (path.isEmpty())
        {
            const qreal r = key * 0.25;
            path.addEllipse(QPointF(0.0, 0.0), r, r);
        }
        p_.save();
        p_.translate(static_cast<qreal>(centre.x),
                     static_cast<qreal>(centre.y));
        p_.setClipPath(path, Qt::IntersectClip);
        p_.drawImage(
            QRectF(-diameter * 0.5, -diameter * 0.5, diameter, diameter),
            pick_image_(qi, diameter, diameter));
        p_.restore();
    }

    void draw_initials_circle(std::string_view name, Point centre,
                              float diameter, Color bg, Color fg) override
    {
        p_.save();
        p_.setBrush(to_qcolor(bg));
        p_.setPen(Qt::NoPen);
        p_.drawEllipse(QPointF(centre.x, centre.y), diameter * 0.5,
                       diameter * 0.5);

        QFont f;
        f.setPointSizeF(diameter * kAvatarInitialsFontRatio);
        f.setWeight(QFont::DemiBold);
        p_.setFont(f);
        p_.setPen(to_qcolor(fg));
        QString s = initials_upper(name);
        QRectF box(centre.x - diameter * 0.5, centre.y - diameter * 0.5,
                   diameter, diameter);
        p_.drawText(box, Qt::AlignCenter, s);
        p_.restore();
    }

    void draw_text(const TextLayout& layout, Point origin, Color c) override
    {
        static_cast<const QtTextLayoutBase&>(layout).draw(p_, origin, c);
    }

    void push_clip_rect(Rect r) override
    {
        p_.save();
        p_.setClipRect(to_qrect(r), Qt::IntersectClip);
    }

    void push_clip_rounded_rect(Rect r, float radius) override
    {
        p_.save();
        QPainterPath path;
        path.addRoundedRect(to_qrect(r), radius, radius);
        p_.setClipPath(path, Qt::IntersectClip);
    }

    void pop_clip() override
    {
        p_.restore();
    }

    Rect clip_rect() const override
    {
        if (!p_.hasClipping())
        {
            return {0.f, 0.f, 1e9f, 1e9f};
        }
        const QRectF r = p_.clipBoundingRect();
        if (r.isEmpty())
        {
            return {0.f, 0.f, 1e9f, 1e9f};
        }
        return {float(r.x()), float(r.y()), float(r.width()), float(r.height())};
    }

    float scale_factor() const override
    {
        return static_cast<float>(p_.device()->devicePixelRatioF());
    }

private:
    // Pick the source QImage for `qi` at logical destination size
    // (dst_w × dst_h): the cached pre-scaled variant when its device-pixel
    // size differs meaningfully from the source, otherwise the original.
    //
    // The cache is bypassed for identity / single-pixel-mismatch blits so we
    // don't churn entries on subpixel-rounded paint rects.
    const QImage& pick_image_(const QtImage& qi, float dst_w, float dst_h) const
    {
        if (dst_w <= 0.0f || dst_h <= 0.0f)
        {
            return qi.image();
        }
        const qreal dpr = p_.device()->devicePixelRatioF();
        const int tw = static_cast<int>(std::lround(dst_w * dpr));
        const int th = static_cast<int>(std::lround(dst_h * dpr));
        if (tw <= 0 || th <= 0)
        {
            return qi.image();
        }
        const int sw = qi.image().width();
        const int sh = qi.image().height();
        if (std::abs(tw - sw) <= 1 && std::abs(th - sh) <= 1)
        {
            return qi.image();
        }
        return qi.scaled_for(tw, th, dpr);
    }

    QPainter& p_;
};

std::unique_ptr<Canvas> make_canvas(QPainter& painter)
{
    return std::make_unique<QtCanvas>(painter);
}

// ─────────────────────────────────────────────────────────────────────────
//  QtRichTextLayout — rich-text tk::TextLayout backed by QTextDocument
// ─────────────────────────────────────────────────────────────────────────

class QtRichTextLayout : public QtTextLayoutBase
{
public:
    explicit QtRichTextLayout(std::unique_ptr<QTextDocument> doc)
        : doc_(std::move(doc))
    {
        sz_ = doc_->size();
        lines_ = doc_->blockCount();
    }

    Size measure() const override
    {
        return {static_cast<float>(sz_.width()),
                static_cast<float>(sz_.height())};
    }
    int line_count() const override
    {
        return lines_;
    }
    float ascent() const override
    {
        return static_cast<float>(QFontMetricsF(doc_->defaultFont()).ascent());
    }

    void draw(QPainter& p, Point origin, Color c) const override
    {
        p.save();
        p.translate(static_cast<qreal>(origin.x), static_cast<qreal>(origin.y));
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, QColor(c.r, c.g, c.b, c.a));
        doc_->documentLayout()->draw(&p, ctx);
        p.restore();
    }

    std::string link_at(tk::Point local) const override
    {
        int pos = doc_->documentLayout()->hitTest(
            QPointF(static_cast<qreal>(local.x), static_cast<qreal>(local.y)),
            Qt::FuzzyHit);
        if (pos < 0)
        {
            return {};
        }
        QTextCursor cur(doc_.get());
        cur.setPosition(pos);
        QTextCharFormat fmt = cur.charFormat();
        return fmt.isAnchor() ? fmt.anchorHref().toStdString() : "";
    }

    int char_index_at(tk::Point local) const override
    {
        int qt_pos = doc_->documentLayout()->hitTest(
            QPointF(static_cast<qreal>(local.x), static_cast<qreal>(local.y)),
            Qt::FuzzyHit);
        if (qt_pos < 0)
            return -1;
        return qt_pos_to_utf8_byte(qt_pos);
    }

    std::vector<tk::Rect> selection_rects(int start_byte,
                                          int end_byte) const override
    {
        if (start_byte >= end_byte)
            return {};
        int qt_start = utf8_byte_to_qt_pos(start_byte);
        int qt_end   = utf8_byte_to_qt_pos(end_byte);
        if (qt_start >= qt_end)
            return {};

        std::vector<tk::Rect> out;
        // Walk each block's QTextLayout and collect line-level selection rects.
        for (QTextBlock blk = doc_->begin(); blk != doc_->end(); blk = blk.next())
        {
            int blk_start = blk.position();
            int blk_end   = blk_start + blk.length();
            if (blk_end <= qt_start || blk_start >= qt_end)
                continue;
            int sel_start = std::max(qt_start, blk_start) - blk_start;
            int sel_end   = std::min(qt_end,   blk_end)   - blk_start;
            const QTextLayout* tl = blk.layout();
            if (!tl)
                continue;
            for (int li = 0; li < tl->lineCount(); ++li)
            {
                QTextLine line = tl->lineAt(li);
                int line_start = line.textStart();
                int line_end   = line_start + line.textLength();
                if (line_end < sel_start || line_start > sel_end)
                    continue;
                int clamped_start = std::max(sel_start, line_start);
                int clamped_end   = std::min(sel_end,   line_end);
                qreal x1 = line.cursorToX(clamped_start, QTextLine::Leading);
                qreal x2 = line.cursorToX(clamped_end,   QTextLine::Leading);
                if (x1 > x2)
                    std::swap(x1, x2);
                QRectF lr = line.naturalTextRect();
                // naturalTextRect y is block-local; add block's visual y
                qreal block_y =
                    doc_->documentLayout()->blockBoundingRect(blk).top();
                out.push_back({static_cast<float>(x1),
                               static_cast<float>(block_y + lr.top()),
                               static_cast<float>(x2 - x1),
                               static_cast<float>(lr.height())});
            }
        }
        return out;
    }

    std::string text_range(int start_byte, int end_byte) const override
    {
        if (start_byte >= end_byte)
            return {};
        int qt_start = utf8_byte_to_qt_pos(start_byte);
        int qt_end   = utf8_byte_to_qt_pos(end_byte);
        if (qt_start >= qt_end)
            return {};
        QTextCursor cur(doc_.get());
        cur.setPosition(qt_start);
        cur.setPosition(qt_end, QTextCursor::KeepAnchor);
        return cur.selectedText().replace(QChar(0x2029), '\n').toStdString();
    }

private:
    // QTextDocument uses UTF-16 positions internally (one code unit per
    // BMP character, two for supplementary). Convert to/from UTF-8 byte offset.
    int qt_pos_to_utf8_byte(int qt_pos) const
    {
        QString plain = doc_->toPlainText();
        if (qt_pos <= 0)
            return 0;
        int clamped = std::min(qt_pos, static_cast<int>(plain.size()));
        return plain.left(clamped).toUtf8().size();
    }

    int utf8_byte_to_qt_pos(int byte_offset) const
    {
        if (byte_offset <= 0)
            return 0;
        QByteArray utf8 = doc_->toPlainText().toUtf8();
        int clamped = std::min(byte_offset, static_cast<int>(utf8.size()));
        return QString::fromUtf8(utf8.constData(), clamped).size();
    }

    std::unique_ptr<QTextDocument> doc_;
    QSizeF sz_;
    int lines_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────
//  QtFactory — tk::CanvasFactory
// ─────────────────────────────────────────────────────────────────────────

class QtFactory : public CanvasFactory
{
    static constexpr std::size_t kNumRoles =
        static_cast<std::size_t>(FontRole::EmojiPickerCell) + 1;
    std::array<QFont, kNumRoles> font_cache_;

public:
    QtFactory()
    {
        static_assert(
            static_cast<int>(FontRole::EmojiPickerCell) == 10,
            "FontRole layout changed — verify font_cache_ index mapping");
        for (std::size_t i = 0; i < kNumRoles; ++i)
        {
            font_cache_[i] = font_for(static_cast<FontRole>(i));
        }
    }

    std::unique_ptr<Image>
    decode_image(std::span<const std::uint8_t> bytes) override
    {
        if (bytes.empty())
        {
            return nullptr;
        }
        QImage img;
        if (!img.loadFromData(bytes.data(), static_cast<int>(bytes.size())))
        {
            return nullptr;
        }
        return std::make_unique<QtImage>(std::move(img));
    }

    std::unique_ptr<Image>
    create_image_rgba(const std::uint8_t* pixels, int w, int h) override
    {
        if (!pixels || w <= 0 || h <= 0)
            return nullptr;
        // QImage::Format_RGBA8888 matches nanosvg's RGBA output byte order.
        // .copy() detaches from the caller's buffer before it's freed.
        QImage img(pixels, w, h, w * 4, QImage::Format_RGBA8888);
        return std::make_unique<QtImage>(img.copy());
    }

    std::unique_ptr<Image>
    scale_image(const Image& src, int max_w, int max_h) override
    {
        const auto& qi = static_cast<const QtImage&>(src);
        const QImage& orig = qi.image();
        if (orig.width() <= max_w && orig.height() <= max_h)
        {
            return nullptr; // already fits — caller keeps original
        }
        QImage scaled = orig.scaled(max_w, max_h, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        return std::make_unique<QtImage>(std::move(scaled));
    }

    std::unique_ptr<AnimatedImage>
    decode_animated_image(std::span<const std::uint8_t> bytes,
                          int max_px) override
    {
        if (bytes.empty())
            return nullptr;

        QByteArray qba(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<qsizetype>(bytes.size()));
        QBuffer buf(&qba);
        buf.open(QIODevice::ReadOnly);

        QImageReader reader(&buf);
        const int count = reader.imageCount();
        if (count <= 1)
            return nullptr;

        std::vector<std::unique_ptr<Image>> frames;
        std::vector<int> delays;
        frames.reserve(static_cast<std::size_t>(count));
        delays.reserve(static_cast<std::size_t>(count));

        for (int i = 0; i < count; ++i)
        {
            // jumpToImage() returns false for sequential-only formats (e.g. GIF)
            // even when the read succeeds — ignore its return value and rely on
            // read() returning a null image to detect real failures.
            if (i > 0)
                reader.jumpToNextImage();
            QImage img = reader.read();
            if (img.isNull())
                break;

            const int delay = qMax(reader.nextImageDelay(), 20);

            if (img.width() > max_px || img.height() > max_px)
                img = img.scaled(max_px, max_px, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);

            frames.push_back(std::make_unique<QtImage>(std::move(img)));
            delays.push_back(delay);
        }

        if (frames.size() < 2)
            return nullptr;
        return std::make_unique<AnimatedImage>(std::move(frames),
                                              std::move(delays));
    }

    std::unique_ptr<TextLayout> build_text(std::string_view utf8,
                                           const TextStyle& s) override
    {
        QFont f = font_cache_[static_cast<std::size_t>(s.role)];
        // A wrap=false layout must stay on one line; QPainter::drawText honours
        // hard breaks even under NoWrap, so fold them out first (see
        // tk::fold_hard_breaks_utf8).
        const std::string folded =
            s.wrap ? std::string() : fold_hard_breaks_utf8(utf8);
        const std::string_view src = s.wrap ? utf8 : std::string_view(folded);
        QString text =
            QString::fromUtf8(src.data(), static_cast<int>(src.size()));

        QTextOption opt;
        Qt::Alignment a = Qt::AlignTop;
        switch (s.halign)
        {
        case TextHAlign::Leading:
            a |= Qt::AlignLeft;
            break;
        case TextHAlign::Center:
            a |= Qt::AlignHCenter;
            break;
        case TextHAlign::Trailing:
            a |= Qt::AlignRight;
            break;
        }
        switch (s.valign)
        {
        case TextVAlign::Top:
            a = (a & ~Qt::AlignVertical_Mask) | Qt::AlignTop;
            break;
        case TextVAlign::Center:
            a = (a & ~Qt::AlignVertical_Mask) | Qt::AlignVCenter;
            break;
        case TextVAlign::Bottom:
            a = (a & ~Qt::AlignVertical_Mask) | Qt::AlignBottom;
            break;
        }
        opt.setAlignment(a);
        opt.setWrapMode(s.wrap ? QTextOption::WordWrap : QTextOption::NoWrap);

        qreal max_w = s.max_width > 0 ? s.max_width : 8192.0;
        qreal max_h = s.max_height > 0 ? s.max_height : 8192.0;

        bool elide_single_line = (s.trim == TextTrim::Ellipsis);

        QFontMetricsF fm(f);
        QSizeF measured;
        int line_count = 1;
        if (elide_single_line)
        {
            QString shown = fm.elidedText(text, Qt::ElideRight, max_w);
            QRectF br = fm.boundingRect(shown);
            measured = QSizeF(qMin(br.width(), max_w), fm.height());
        }
        else if (s.wrap)
        {
            QRectF br =
                fm.boundingRect(QRectF(0, 0, max_w, max_h),
                                static_cast<int>(a) | Qt::TextWordWrap, text);
            measured = br.size();
            line_count = qMax(1, qRound(br.height() / fm.height()));
        }
        else
        {
            // Use the cursor-advance width, not the ink-bounding-rect:
            // `fm.boundingRect(text)` returns the tight box around the
            // painted pixels, which omits trailing whitespace advance
            // and per-glyph right side bearing. drawText() lays glyphs
            // out using advance widths, so an advance-based measure is
            // what the chip / button background needs in order to fully
            // contain the rendered text (e.g. reaction "👍 5" — the space
            // contributes zero ink but real advance).
            measured = QSizeF(fm.horizontalAdvance(text), fm.height());
        }

        return std::make_unique<QtTextLayout>(
            std::move(text), std::move(f), opt, measured, line_count, max_w,
            max_h, elide_single_line, fm.ascent(), s.max_height);
    }

    std::unique_ptr<TextLayout> build_rich_text(std::span<const TextSpan> spans,
                                                const TextStyle& s) override
    {
        QFont base = font_cache_[static_cast<std::size_t>(s.role)];

        QString html;
        html.reserve(256);
        for (const auto& sp : spans)
        {
            QString t = QString::fromUtf8(sp.text.data(),
                                          static_cast<int>(sp.text.size()))
                            .toHtmlEscaped();
            t.replace(QLatin1Char('\n'), QLatin1String("<br>"));
            if (sp.has_color)
            {
                t = QLatin1String("<span style=\"color:") +
                    to_qcolor(sp.color).name() + QLatin1String(";\">") + t +
                    QLatin1String("</span>");
            }
            if (sp.code)
            {
                // white-space:pre-wrap keeps significant whitespace — leading
                // tabs/spaces and internal runs — that QTextDocument's HTML
                // parser would otherwise collapse, while still wrapping long
                // lines inside the bubble. (pre-wrap, not pre, to avoid
                // horizontal overflow.)
                t = QLatin1String("<code style=\"white-space:pre-wrap;\">") + t +
                    QLatin1String("</code>");
            }
            if (sp.strikethrough)
            {
                t = QLatin1String("<s>") + t + QLatin1String("</s>");
            }
            if (sp.italic)
            {
                t = QLatin1String("<i>") + t + QLatin1String("</i>");
            }
            if (sp.bold)
            {
                t = QLatin1String("<b>") + t + QLatin1String("</b>");
            }
            if (sp.semibold)
            {
                t = QLatin1String("<span style=\"font-weight:600\">") + t + QLatin1String("</span>");
            }
            if (!sp.url.empty())
            {
                QString href =
                    QString::fromUtf8(sp.url.data(),
                                      static_cast<int>(sp.url.size()))
                        .toHtmlEscaped();
                if (sp.is_mention)
                {
                    // Keep the anchor for hit-testing but drop the link
                    // underline; the accent colour comes from has_color and the
                    // pill background is drawn by the view.
                    QString col = sp.has_color ? to_qcolor(sp.color).name()
                                               : QStringLiteral("#1B4AC2");
                    t = QLatin1String("<a href=\"") + href +
                        QLatin1String("\" style=\"text-decoration:none;color:") +
                        col + QLatin1String(";\">") + t + QLatin1String("</a>");
                }
                else
                {
                    t = QLatin1String("<a href=\"") + href +
                        QLatin1String("\">") + t + QLatin1String("</a>");
                }
            }
            html += t;
        }

        auto doc = std::make_unique<QTextDocument>();
        doc->setDefaultFont(base);
        doc->setDocumentMargin(0.0);
        doc->setHtml(QLatin1String("<body>") + html + QLatin1String("</body>"));
        if (s.max_width > 0)
        {
            doc->setTextWidth(static_cast<qreal>(s.max_width));
        }

        return std::make_unique<QtRichTextLayout>(std::move(doc));
    }
};

std::unique_ptr<CanvasFactory> make_factory()
{
    return std::make_unique<QtFactory>();
}

std::unique_ptr<Image> make_image(QImage img)
{
    return std::make_unique<QtImage>(std::move(img));
}

} // namespace tk::qt6
