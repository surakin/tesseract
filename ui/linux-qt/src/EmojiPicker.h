#pragma once
#include <QFrame>
#include <QString>

#include <functional>
#include <memory>

#include "tk/host.h"
#include "tk/host_qt.h"
#include "views/EmojiPicker.h"

namespace tesseract { class Client; }

/// Floating emoji picker. Hosts the shared tesseract::views::EmojiPicker
/// inside a tk::qt6::Surface, with a native QLineEdit overlaid on the
/// shared widget's search-field rect.
///
/// Public API preserved verbatim from the legacy native picker so
/// MainWindow.cpp doesn't need to change.
class EmojiPicker : public QFrame {
    Q_OBJECT
public:
    explicit EmojiPicker(QWidget* parent = nullptr);

    /// Borrowed pointer to the SDK client; must outlive the picker. The
    /// shared picker reads recent_emoji_top + writes recent_emoji_bump.
    void setClient(tesseract::Client* c);

    /// Position the picker so its bottom-right corner lines up just above
    /// the given anchor widget, then show it.
    void popupAt(QWidget* anchor);

    /// Position the picker so it lines up against a specific rect inside
    /// `anchor` (the rect is in `anchor`'s local widget coordinates). Used
    /// for the reaction "+" chip, where the anchor is the message surface
    /// but the visual target is a small rect within it.
    void popupAtRect(QWidget* anchor, const tk::Rect& localRect);

    /// Fired when the user picks an emoji; the QString carries the glyph
    /// as UTF-8.
    std::function<void(const QString&)> onSelected;

protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void layout_overlay();

    tk::qt6::Surface*                       surface_      = nullptr;
    tesseract::views::EmojiPicker*          shared_       = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextField>    search_field_;
};
