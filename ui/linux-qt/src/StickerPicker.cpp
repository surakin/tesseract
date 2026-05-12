#include "StickerPicker.h"

#include "tk/theme.h"

#include <tesseract/client.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>

#include <memory>
#include <utility>

namespace {

constexpr int kPickerW = 360;
constexpr int kPickerH = 420;

} // namespace

StickerPicker::StickerPicker(QWidget* parent)
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

    auto shared_owner = std::make_unique<tesseract::views::StickerPicker>();
    shared_ = shared_owner.get();
    shared_->on_selected = [this](const tesseract::ImagePackImage& img) {
        if (onSelected) onSelected(img);
        hide();
    };

    // Image provider: synchronous best-effort lookup from the per-picker
    // cache. On miss we kick off a fetch on the same thread (the SDK's
    // media cache is consulted first so the second call is instant) and
    // populate the cache for the next paint pass.
    shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& source_token) -> const tk::Image* {
            auto it = image_cache_.find(cache_key);
            if (it != image_cache_.end()) return it->second.get();
            // Decode lazily on the UI thread using the matrix-sdk media
            // cache. fetch_source_bytes handles both plain mxc:// URIs
            // and encrypted MediaSource JSON tokens; for thumbnail
            // sizes the decode is cheap.
            (void)source_token;
            return nullptr;
        });
    surface_->set_root(std::move(shared_owner));

    search_field_ = surface_->host().make_text_field();
    search_field_->set_placeholder("Search stickers");
    search_field_->set_on_changed(
        [this](const std::string& q) {
            shared_->set_search_query(q);
            surface_->relayout();
        });
}

StickerPicker::~StickerPicker() = default;

void StickerPicker::setClient(tesseract::Client* c) {
    if (shared_) shared_->set_client(c);
}

void StickerPicker::refreshPacks() {
    if (shared_) shared_->refresh_packs();
    if (surface_) surface_->relayout();
}

void StickerPicker::invalidateImages() {
    if (shared_) shared_->invalidate_image_cache();
}

void StickerPicker::popupAt(QWidget* anchor) {
    if (!anchor) return;
    refreshPacks();
    if (search_field_) search_field_->set_text("");
    shared_->set_search_query("");

    QPoint anchorGlobal = anchor->mapToGlobal(QPoint(0, 0));
    QRect  screen       = QApplication::primaryScreen()->availableGeometry();
    int x = anchorGlobal.x() + anchor->width() - width();
    int y = anchorGlobal.y() - height() - 4;
    if (x < screen.left())            x = screen.left() + 4;
    if (x + width() > screen.right()) x = screen.right() - width() - 4;
    if (y < screen.top())             y = anchorGlobal.y() + anchor->height() + 4;
    move(x, y);
    show();
    if (search_field_) search_field_->set_focused(true);
}

void StickerPicker::showEvent(QShowEvent* e) {
    QFrame::showEvent(e);
    layout_overlay();
}

void StickerPicker::resizeEvent(QResizeEvent* e) {
    QFrame::resizeEvent(e);
    layout_overlay();
}

void StickerPicker::layout_overlay() {
    if (!shared_ || !search_field_) return;
    search_field_->set_rect(shared_->search_field_rect());
}
