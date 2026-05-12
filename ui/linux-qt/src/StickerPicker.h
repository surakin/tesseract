#pragma once
#include <QFrame>

#include <functional>
#include <memory>
#include <unordered_map>

#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/StickerPicker.h"

#include <tesseract/image_pack.h>

namespace tesseract { class Client; }

/// Floating sticker picker. Mirrors the EmojiPicker wrapper for Qt: hosts
/// the shared `tesseract::views::StickerPicker` inside a `tk::qt6::Surface`
/// with a native QLineEdit search overlay. The popup is non-modal
/// (Qt::Popup auto-closes on outside clicks).
class StickerPicker : public QFrame {
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

    /// Fired when the user picks a sticker. The host forwards to
    /// `Client::send_sticker`.
    std::function<void(const tesseract::ImagePackImage&)> onSelected;

protected:
    void showEvent  (QShowEvent*  e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void layout_overlay();

    tk::qt6::Surface*                       surface_      = nullptr;
    tesseract::views::StickerPicker*        shared_       = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>    search_field_;

    // The picker keeps its own per-instance image cache so swapping packs
    // doesn't have to re-decode bitmaps. Populated lazily on first paint
    // by `image_provider_` (decode happens on a worker via Client and
    // post_to_ui hops back to apply).
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> image_cache_;
};
