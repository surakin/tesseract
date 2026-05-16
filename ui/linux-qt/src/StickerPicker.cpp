#include "StickerPicker.h"

#include "tk/canvas_qpainter.h"
#include "tk/theme.h"

#include <tesseract/client.h>

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QResizeEvent>
#include <QRunnable>
#include <QScreen>
#include <QShowEvent>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
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
    // cache. On miss we kick off a background fetch via QThreadPool — the
    // SDK's matrix-sdk-sqlite media cache makes repeat hits effectively
    // free, but the first fetch of an unseen sticker can take a network
    // round-trip and must not block the UI thread.
    shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            // Animated entries take priority — `onAnimTick_` keeps
            // `current` valid; static cache is the second hop.
            auto ait = animated_cache_.find(cache_key);
            if (ait != animated_cache_.end() && !ait->second.frames.empty()) {
                return ait->second.frames[ait->second.current].get();
            }
            auto sit = image_cache_.find(cache_key);
            if (sit != image_cache_.end()) return sit->second.get();
            // Fire-and-forget request — the worker emits
            // `imageLoadedSignal_` when it lands, which queues onto the
            // UI thread for decode + cache + repaint.
            const_cast<StickerPicker*>(this)->request_image_(cache_key);
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

    // Marshal worker-thread fetch completions back onto the UI thread.
    connect(this, &StickerPicker::imageLoadedSignal_,
            this, &StickerPicker::onImageLoaded_,
            Qt::QueuedConnection);

    // Animation frame-tick. 60 Hz is enough for GIF / WebP / APNG; the
    // timer only fires while the picker is visible to keep idle cost
    // bounded (showEvent / hideEvent toggle it via `setActive` below).
    anim_timer_ = new QTimer(this);
    anim_timer_->setInterval(16);
    connect(anim_timer_, &QTimer::timeout,
            this, &StickerPicker::onAnimTick_);
}

StickerPicker::~StickerPicker() = default;

void StickerPicker::setClient(tesseract::Client* c) {
    client_ = c;
    if (shared_) shared_->set_client(c);
}

void StickerPicker::request_image_(const std::string& cache_key) {
    if (!client_ || cache_key.empty()) return;
    if (image_cache_.count(cache_key)) return;
    if (!fetches_in_flight_.insert(cache_key).second) return;

    // Capture client_ + key by value; the picker outlives any in-flight
    // fetch (auto-cleanup on dtor isn't needed because the picker is
    // owned by the QMainWindow for the app's lifetime).
    QString qkey = QString::fromStdString(cache_key);
    // Snapshot client_ now (GUI thread): setClient() can rebind it on an
    // account switch while this worker runs, which would be a data race and
    // could fetch against the wrong account.
    auto* c = client_;
    if (!c) { fetches_in_flight_.erase(cache_key); return; }
    auto* runner = QRunnable::create([c, key = cache_key, qkey, this]() {
        auto bytes = c->fetch_source_bytes(key);
        QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<int>(bytes.size()));
        emit imageLoadedSignal_(qkey, qb);
    });
    QThreadPool::globalInstance()->start(runner);
}

void StickerPicker::onImageLoaded_(QString cache_key, QByteArray bytes) {
    std::string key = cache_key.toStdString();
    fetches_in_flight_.erase(key);
    if (bytes.isEmpty()) return;
    if (image_cache_.count(key) || animated_cache_.count(key)) return;

    // Probe via QImageReader so we can distinguish single-frame from
    // animated GIF / WebP / APNG without paying for a full decode twice.
    QBuffer buf(&bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    const bool animated =
        reader.supportsAnimation() && reader.imageCount() > 1;

    if (animated) {
        AnimatedEntry entry;
        entry.frames.reserve(reader.imageCount());
        entry.delays_ms.reserve(reader.imageCount());
        QImage frame;
        while (reader.read(&frame)) {
            // `nextImageDelay()` reads the *trailing* delay for the
            // frame we just read — i.e. how long this frame should be
            // displayed before advancing. Clamp to the same 20 ms floor
            // browsers use; some old GIFs encode 0 ms which would burn
            // CPU.
            int delay = reader.nextImageDelay();
            if (delay <= 0)   delay = 100;
            if (delay < 20)   delay = 20;
            QImage scaled = frame.scaled(256, 256,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
            entry.frames.push_back(tk::qt6::make_image(std::move(scaled)));
            entry.delays_ms.push_back(delay);
        }
        if (!entry.frames.empty()) {
            entry.current         = 0;
            entry.next_advance_ms = QDateTime::currentMSecsSinceEpoch()
                                  + entry.delays_ms[0];
            animated_cache_.emplace(key, std::move(entry));
            if (anim_timer_ && !anim_timer_->isActive()
                && isVisible())
                anim_timer_->start();
            if (shared_)  shared_->invalidate_image_cache();
            if (surface_) surface_->update();
            return;
        }
        // Decoder claimed animation but yielded no frames — fall through
        // to the static path with `bytes` so the user sees something.
        buf.seek(0);
    }

    // Static (or animated-decode-failed) path.
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                            bytes.size()))
        return;
    // Scale down to a sensible upper bound for picker cells (96 px) and
    // pack-tab avatars (40 px). 256×256 keeps the bitmap usable for both;
    // canvas_qpainter.cpp's draw_image letterboxes from there.
    QImage scaled = img.scaled(256, 256,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    image_cache_.emplace(key, tk::qt6::make_image(std::move(scaled)));
    if (shared_)  shared_->invalidate_image_cache();
    if (surface_) surface_->update();
}

void StickerPicker::onAnimTick_() {
    if (animated_cache_.empty()) {
        if (anim_timer_) anim_timer_->stop();
        return;
    }
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
    bool any_changed = false;
    for (auto& [_, entry] : animated_cache_) {
        if (entry.frames.size() <= 1) continue;
        // Advance through every elapsed delay so we don't drift on slow
        // ticks (e.g. when the picker is briefly occluded and we miss a
        // few timer fires). Cap at one full loop to avoid pathological
        // catch-up on long pauses.
        std::size_t steps = 0;
        while (now >= entry.next_advance_ms
                && steps < entry.frames.size())
        {
            entry.current = (entry.current + 1) % entry.frames.size();
            entry.next_advance_ms +=
                entry.delays_ms[entry.current];
            ++steps;
        }
        if (steps > 0) any_changed = true;
    }
    if (any_changed) {
        if (shared_)  shared_->invalidate_image_cache();
        if (surface_) surface_->update();
    }
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

void StickerPicker::popupAtRect(QWidget* anchor, const tk::Rect& localRect) {
    if (!anchor) return;
    refreshPacks();
    if (search_field_) search_field_->set_text("");
    shared_->set_search_query("");

    QPoint topLeftGlobal = anchor->mapToGlobal(
        QPoint(static_cast<int>(localRect.x), static_cast<int>(localRect.y)));
    int rectW = static_cast<int>(localRect.w);
    int rectH = static_cast<int>(localRect.h);
    QScreen* scr = QApplication::screenAt(topLeftGlobal);
    if (!scr) scr = anchor->screen();
    if (!scr) scr = QApplication::primaryScreen();
    QRect screen = scr->availableGeometry();

    int x = topLeftGlobal.x() + rectW / 2 - width() / 2;   // centered over button
    int y = topLeftGlobal.y() - height() - 4;               // above button
    if (y < screen.top()) y = topLeftGlobal.y() + rectH + 4;
    if (x + width() > screen.right()) x = screen.right() - width() - 4;
    if (x < screen.left())            x = screen.left() + 4;
    if (y + height() > screen.bottom()) y = screen.bottom() - height() - 4;
    move(x, y);
    show();
    if (search_field_) search_field_->set_focused(true);
}

void StickerPicker::showEvent(QShowEvent* e) {
    QFrame::showEvent(e);
    layout_overlay();
    // Resume animation when the picker becomes visible. The timer
    // auto-stops in `onAnimTick_` once `animated_cache_` empties; we
    // restart it here so revisiting the picker after a hide-reshow
    // resumes any in-flight animations.
    if (anim_timer_ && !animated_cache_.empty() && !anim_timer_->isActive()) {
        // Re-base the schedule so frames don't all advance at once on
        // resume (would look like a jump-cut).
        const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
        for (auto& [_, entry] : animated_cache_) {
            if (entry.frames.empty()) continue;
            entry.next_advance_ms = now
                + entry.delays_ms[entry.current];
        }
        anim_timer_->start();
    }
}

void StickerPicker::hideEvent(QHideEvent* e) {
    QFrame::hideEvent(e);
    if (anim_timer_ && anim_timer_->isActive()) anim_timer_->stop();
}

void StickerPicker::resizeEvent(QResizeEvent* e) {
    QFrame::resizeEvent(e);
    layout_overlay();
}

void StickerPicker::layout_overlay() {
    if (!shared_ || !search_field_) return;
    search_field_->set_rect(shared_->search_field_rect());
}
