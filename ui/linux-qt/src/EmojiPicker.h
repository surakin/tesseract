#pragma once
#include "tesseract/emoji.h"

#include <QFrame>
#include <QString>

#include <functional>
#include <vector>

class QLineEdit;
class QStackedWidget;
class QToolButton;
class QGridLayout;
class QScrollArea;

namespace tesseract { class Client; }

/// Floating emoji picker, mobile-keyboard style: search bar at top, a grid
/// of emoji buttons in the middle, and a category tab strip at the bottom
/// (Frequently Used + 8 Unicode categories).
///
/// The picker carries no ownership of the recents store; the caller passes
/// a pointer and the picker only reads `top()` from it. When the user
/// clicks an emoji, `onSelected` fires with the UTF-8 glyph — the caller
/// is responsible for inserting it and calling `bump()` on its recents.
class EmojiPicker : public QFrame {
    Q_OBJECT
public:
    explicit EmojiPicker(QWidget* parent = nullptr);

    /// Borrowed pointer to the SDK client; must outlive the picker. The
    /// picker calls `recent_emoji_top` to populate the Frequents tab and
    /// `recent_emoji_bump` when the user picks an emoji.
    void setClient(tesseract::Client* c) { client_ = c; }

    /// Position the picker so its bottom-right corner lines up just above
    /// the given anchor widget (typically the emoji button in the compose
    /// bar), then show it.
    void popupAt(QWidget* anchor);

    /// Fired when the user picks an emoji; the QString carries the glyph
    /// as UTF-8.
    std::function<void(const QString&)> onSelected;

protected:
    bool event(QEvent* e) override;

private slots:
    void onSearchChanged(const QString& text);
    void onTabChanged(int idx);

private:
    void  buildUi();
    void  refreshFrequents();
    void  showCategory(tesseract::emoji::Category c);
    void  showSearchResults(const QString& query);
    void  populateGrid(QGridLayout* grid,
                       const std::vector<const tesseract::emoji::Entry*>& items);
    QToolButton* makeEmojiButton(const QString& glyph);

    tesseract::Client* client_ = nullptr;

    QLineEdit*       search_      = nullptr;
    QStackedWidget*  pages_       = nullptr;   // 0 = frequents, 1..N = categories, last = search
    QWidget*         freqPage_    = nullptr;
    QGridLayout*     freqGrid_    = nullptr;
    QWidget*         searchPage_  = nullptr;
    QGridLayout*     searchGrid_  = nullptr;
    QWidget*         tabStrip_    = nullptr;
    std::vector<QToolButton*> tabButtons_;     // [0]=frequents, [1..8]=categories
    int              currentTabIdx_ = 1;       // default: Smileys & People
};
