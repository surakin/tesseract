#include "EmojiPicker.h"

#include "tk/canvas_qpainter.h"
#include "tk/theme.h"

#include <tesseract/client.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QResizeEvent>
#include <QRunnable>
#include <QScreen>
#include <QShowEvent>
#include <QThreadPool>

#include <memory>
#include <utility>

namespace {

constexpr int kPickerW = 320;
constexpr int kPickerH = 360;

} // namespace

EmojiPicker::EmojiPicker(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFrameShape(QFrame::NoFrame);
    resize(kPickerW, kPickerH);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    surface_ = new tk::qt6::Surface(tk::Theme::light(), this);
    layout->addWidget(surface_);

    auto shared_owner = std::make_unique<tesseract::views::EmojiPicker>();
    shared_ = shared_owner.get();
    shared_->on_selected = [this](const std::string& glyph) {
        if (onSelected) onSelected(QString::fromStdString(glyph));
        hide();
    };
    shared_->on_emoticon_selected = [this](const tesseract::ImagePackImage& img) {
        if (onEmoticonSelected) onEmoticonSelected(img);
        hide();
    };

    shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            auto it = image_cache_.find(cache_key);
            if (it != image_cache_.end()) return it->second.get();
            const_cast<EmojiPicker*>(this)->request_image_(cache_key);
            return nullptr;
        });

    surface_->set_root(std::move(shared_owner));

    search_field_ = surface_->host().make_text_field();
    search_field_->set_placeholder("Search emoji");
    search_field_->set_on_changed(
        [this](const std::string& q) {
            shared_->set_search_query(q);
            surface_->relayout();
        });

    connect(this, &EmojiPicker::imageLoadedSignal_,
            this, &EmojiPicker::onImageLoaded_,
            Qt::QueuedConnection);
}

void EmojiPicker::setClient(tesseract::Client* c) {
    client_ = c;
    if (shared_) shared_->set_client(c);
}

void EmojiPicker::refreshEmoticonPacks() {
    if (shared_) shared_->refresh_emoticon_packs();
    if (surface_) surface_->relayout();
}

void EmojiPicker::invalidateImages() {
    if (shared_) shared_->invalidate_image_cache();
    if (surface_) surface_->update();
}

void EmojiPicker::request_image_(const std::string& cache_key) {
    if (!client_ || cache_key.empty()) return;
    if (image_cache_.count(cache_key)) return;
    if (!fetches_in_flight_.insert(cache_key).second) return;

    QString qkey = QString::fromStdString(cache_key);
    // Snapshot client_ on the GUI thread; setClient() can rebind it on an
    // account switch while this worker runs (data race + wrong account).
    auto* c = client_;
    auto* runner = QRunnable::create([c, key = cache_key, qkey, this]() {
        auto bytes = c->fetch_source_bytes(key);
        QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<int>(bytes.size()));
        emit imageLoadedSignal_(qkey, qb);
    });
    QThreadPool::globalInstance()->start(runner);
}

void EmojiPicker::onImageLoaded_(QString cache_key, QByteArray bytes) {
    std::string key = cache_key.toStdString();
    fetches_in_flight_.erase(key);
    if (bytes.isEmpty() || image_cache_.count(key)) return;

    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.constData()),
                           bytes.size()))
        return;
    QImage scaled = img.scaled(64, 64,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    image_cache_.emplace(key, tk::qt6::make_image(std::move(scaled)));
    if (shared_)  shared_->invalidate_image_cache();
    if (surface_) surface_->update();
}

void EmojiPicker::popupAt(QWidget* anchor) {
    if (!anchor) return;
    shared_->refresh_frequents();
    if (search_field_) search_field_->set_text("");
    shared_->set_search_query("");

    // Anchor the picker so its bottom edge sits just above the button,
    // then nudge so it stays inside the screen.
    QPoint anchorGlobal = anchor->mapToGlobal(QPoint(0, 0));
    QScreen* scr = QApplication::screenAt(anchorGlobal);
    if (!scr) scr = anchor->screen();
    if (!scr) scr = QApplication::primaryScreen();
    QRect screen = scr->availableGeometry();
    int x = anchorGlobal.x() + anchor->width() - width();
    int y = anchorGlobal.y() - height() - 4;
    if (x < screen.left())            x = screen.left() + 4;
    if (x + width() > screen.right()) x = screen.right() - width() - 4;
    if (y < screen.top())             y = anchorGlobal.y() + anchor->height() + 4;
    move(x, y);
    show();
    if (search_field_) search_field_->set_focused(true);
}

void EmojiPicker::popupAtRect(QWidget* anchor, const tk::Rect& localRect) {
    if (!anchor) return;
    shared_->refresh_frequents();
    if (search_field_) search_field_->set_text("");
    shared_->set_search_query("");

    // Map the anchor-local rect to global screen coordinates. tk::Rect
    // coords are widget-logical-pixels (no DPI scale on the Qt surface),
    // so they map directly to QWidget coords.
    QPoint topLeftGlobal = anchor->mapToGlobal(
        QPoint(static_cast<int>(localRect.x),
               static_cast<int>(localRect.y)));
    int rectW = static_cast<int>(localRect.w);
    int rectH = static_cast<int>(localRect.h);

    QScreen* scr = QApplication::screenAt(topLeftGlobal);
    if (!scr) scr = anchor->screen();
    if (!scr) scr = QApplication::primaryScreen();
    QRect screen = scr->availableGeometry();

    // Prefer popping above the rect, centered on it. Fall back to
    // below if there isn't room above. Clamp to the screen on x.
    int x = topLeftGlobal.x() + rectW / 2 - width() / 2;
    int y = topLeftGlobal.y() - height() - 4;
    if (y < screen.top()) y = topLeftGlobal.y() + rectH + 4;
    if (x + width() > screen.right()) x = screen.right() - width() - 4;
    if (x < screen.left())            x = screen.left() + 4;
    if (y + height() > screen.bottom()) y = screen.bottom() - height() - 4;
    (void)rectW;
    move(x, y);
    show();
    if (search_field_) search_field_->set_focused(true);
}

void EmojiPicker::showEvent(QShowEvent* e) {
    QFrame::showEvent(e);
    layout_overlay();
}

void EmojiPicker::resizeEvent(QResizeEvent* e) {
    QFrame::resizeEvent(e);
    layout_overlay();
}

void EmojiPicker::layout_overlay() {
    if (!shared_ || !search_field_) return;
    // The Surface fills the QFrame; the shared widget's search-field
    // rect is in surface-local coordinates which equal frame-local.
    search_field_->set_rect(shared_->search_field_rect());
}
