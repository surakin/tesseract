#pragma once
#include <QFrame>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/StickerPicker.h"

#include <tesseract/image_pack.h>

namespace tesseract
{
class Client;
}

/// Floating sticker picker. Mirrors the EmojiPicker wrapper for Qt: hosts
/// the shared `tesseract::views::StickerPicker` inside a `tk::qt6::Surface`;
/// the search row's native field is self-owned by the shared widget. The
/// popup is non-modal (Qt::Popup auto-closes on outside clicks).
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

    /// Which room this picker is currently being shown for — forwarded to
    /// the wrapped shared picker; call before refreshPacks() so the room's
    /// own pack sorts right after the personal pack.
    void setCurrentRoomId(const std::string& room_id);

    /// Every Space (direct and ancestor) that the current room is in —
    /// forwarded to the wrapped shared picker; call before refreshPacks()
    /// so those spaces' own packs sort right after the current room's pack.
    void setCurrentRoomParentSpaces(const std::vector<std::string>& space_ids);

    /// Trigger a redraw — call from the host after new sticker bitmaps
    /// land in the local image cache.
    void invalidateImages();

    /// Install the image provider on the wrapped shared picker. Called by
    /// MainWindow so the provider can capture the ShellBase shared caches.
    void setImageProvider(tesseract::views::StickerPicker::ImageProvider p);

    /// Re-skin the picker surface when the theme preference changes.
    void set_theme(const tk::Theme& t);

    /// Anchor and show against the compose-bar sticker button.
    void popupAt(QWidget* anchor);
    /// Anchor above a specific rect (surface-local coords), centered on it.
    void popupAtRect(QWidget* anchor, const tk::Rect& localRect);

    /// Fired when the user picks a sticker. The host forwards to
    /// `Client::send_sticker`.
    std::function<void(const tesseract::ImagePackImage&)> onSelected;

    /// Fired whenever the picker is dismissed (outside click or programmatic
    /// hide), regardless of whether a selection was made. Mirrors
    /// EmojiPicker::onDismiss.
    std::function<void()> onDismiss;

protected:
    void hideEvent(QHideEvent* e) override;

private:
    tesseract::Client* client_ = nullptr;
    tk::qt6::Surface* surface_ = nullptr;
    tesseract::views::StickerPicker* shared_ = nullptr; // borrowed
};
