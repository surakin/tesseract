#pragma once
#include <QByteArray>
#include <QFrame>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/StickerPicker.h"

#include <tesseract/image_pack.h>

class QTimer;

namespace tesseract { class Client; }

/// Floating sticker picker. Mirrors the EmojiPicker wrapper for Qt: hosts
/// the shared `tesseract::views::StickerPicker` inside a `tk::qt6::Surface`
/// with a native QLineEdit search overlay. The popup is non-modal
/// (Qt::Popup auto-closes on outside clicks).
class StickerPicker : public QFrame
{
    Q_OBJECT
public:
    explicit StickerPicker(QWidget* parent = nullptr);
    ~StickerPicker() override;

    /// Borrowed pointer to the SDK client; must outlive the picker.
    /// The shared widget calls `list_image_packs` and
    /// `list_favorite_stickers` through it.
    void setClient(tesseract::Client* c);

    /// Call from the host's `IEventHandler::on_image_packs_updated`.
    void refreshPacks();

    /// Trigger a redraw — call from the host after new sticker bitmaps
    /// land in the local image cache.
    void invalidateImages();

    /// Anchor and show against the compose-bar sticker button.
    void popupAt(QWidget* anchor);
    /// Anchor above a specific rect (surface-local coords), centered on it.
    void popupAtRect(QWidget* anchor, const tk::Rect& localRect);

    /// Fired when the user picks a sticker. The host forwards to
    /// `Client::send_sticker`.
    std::function<void(const tesseract::ImagePackImage&)> onSelected;

signals:
    /// Emitted from the worker thread when a sticker fetch finishes. The
    /// queued connection to `onImageLoaded_` brings the bytes back onto
    /// the UI thread for decode + cache + repaint.
    void imageLoadedSignal_(QString cache_key, QByteArray bytes);

protected:
    void showEvent  (QShowEvent*   e) override;
    void hideEvent  (QHideEvent*   e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onImageLoaded_(QString cache_key, QByteArray bytes);
    /// Frame-tick driver. Walks `animated_cache_`, advances any entry
    /// whose `next_advance_ms` has passed, and triggers a single
    /// surface repaint when at least one frame changed.
    void onAnimTick_();

private:
    void layout_overlay();
    /// Trigger an async download for `cache_key` (which doubles as the
    /// matrix-sdk `fetch_source_bytes` input — the picker passes `url` for
    /// both fields). Deduplicates against `fetches_in_flight_`. No-op when
    /// the bitmap is already cached or `client_` is null.
    void request_image_(const std::string& cache_key);

    tesseract::Client*                      client_       = nullptr;
    tk::qt6::Surface*                       surface_      = nullptr;
    tesseract::views::StickerPicker*        shared_       = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>    search_field_;

    // The picker keeps its own per-instance image cache so swapping packs
    // doesn't have to re-decode bitmaps. Populated lazily on first paint
    // by the provider lambda; decode happens on a worker via Client and
    // bounces back onto the UI thread through `imageLoadedSignal_`.
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> image_cache_;

    /// Animated-sticker frames (GIF / animated WebP / APNG). Each entry
    /// holds the full decoded frame list plus a per-frame delay (ms) and
    /// the current frame index + the monotonic time at which the next
    /// advance should happen. `anim_timer_` drives the advance.
    ///
    /// Stickers are bounded in size (≤ 256 px in this picker) and
    /// typically have ≤ 30 frames, so eager decode is fine; the
    /// alternative — decode-on-demand from `QImageReader` per frame —
    /// would stall the UI thread on each frame change.
    struct AnimatedEntry
    {
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int>                         delays_ms;
        std::size_t                              current        = 0;
        std::int64_t                             next_advance_ms = 0;
    };
    std::unordered_map<std::string, AnimatedEntry> animated_cache_;
    QTimer*                                         anim_timer_   = nullptr;

    /// URLs we've kicked off a fetch for but haven't yet received bytes
    /// back from. Stops every cell-paint from starting a duplicate fetch.
    std::unordered_set<std::string>                              fetches_in_flight_;
};
