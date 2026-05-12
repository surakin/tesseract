#include "EmojiPicker.h"

#include "tesseract/client.h"

#include <QApplication>
#include <QEvent>
#include <QGridLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

using tesseract::emoji::Category;
using tesseract::emoji::Entry;

namespace {

// Picker geometry. Sized for ~8 columns of 32×32 buttons + chrome.
constexpr int kCols       = 8;
constexpr int kBtnSize    = 34;
constexpr int kPickerW    = kCols * kBtnSize + 24;   // + scrollbar/padding
constexpr int kPickerH    = 320;
constexpr int kFreqCount  = kCols * 3;               // up to 24 frequents

// Tab strip indices.
constexpr int kTabFreq    = 0;
constexpr int kTabBaseCat = 1;
constexpr int kPageSearch = 1 + 8;   // categories occupy pages 1..8

} // namespace

EmojiPicker::EmojiPicker(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(
        "EmojiPicker { background-color: #FFFFFF; border: 1px solid #D0D3D8; "
        "border-radius: 8px; }"
        "QLineEdit { border: 1px solid #CED0D4; border-radius: 14px; "
        "padding: 4px 10px; }"
        "QToolButton { border: none; padding: 0px; background: transparent; "
        "font-family: 'Noto Color Emoji', 'Segoe UI Emoji'; font-size: 18px; }"
        "QToolButton:hover { background-color: #E4E6EB; border-radius: 4px; }"
        "QToolButton:checked { background-color: #D0E4FF; border-radius: 4px; }"
    );
    resize(kPickerW + 16, kPickerH);

    buildUi();
}

void EmojiPicker::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ── Search ────────────────────────────────────────────────────────────
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search emoji…"));
    search_->setClearButtonEnabled(true);
    root->addWidget(search_);
    connect(search_, &QLineEdit::textChanged,
            this,    &EmojiPicker::onSearchChanged);

    // ── Pages (frequents + 8 categories + search) ─────────────────────────
    pages_ = new QStackedWidget(this);
    pages_->setMinimumHeight(220);
    root->addWidget(pages_, /*stretch=*/1);

    auto makePage = [&](QGridLayout*& outGrid) -> QWidget* {
        auto* scroll = new QScrollArea(pages_);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* inner = new QWidget;
        outGrid = new QGridLayout(inner);
        outGrid->setContentsMargins(0, 0, 0, 0);
        outGrid->setSpacing(2);
        outGrid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        scroll->setWidget(inner);
        return scroll;
    };

    // Frequents page (index 0).
    freqPage_ = makePage(freqGrid_);
    pages_->addWidget(freqPage_);

    // One page per category (indices 1..8), populated lazily on first show.
    for (Category c : tesseract::emoji::kCategories) {
        QGridLayout* grid = nullptr;
        auto* page = makePage(grid);
        populateGrid(grid, tesseract::emoji::by_category(c));
        pages_->addWidget(page);
    }

    // Search results page (index kPageSearch).
    searchPage_ = makePage(searchGrid_);
    pages_->addWidget(searchPage_);

    // ── Tab strip ─────────────────────────────────────────────────────────
    tabStrip_ = new QWidget(this);
    auto* tabRow = new QHBoxLayout(tabStrip_);
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(2);

    auto addTab = [&](const QString& glyph, int pageIdx) {
        auto* btn = new QToolButton(tabStrip_);
        btn->setText(glyph);
        btn->setCheckable(true);
        btn->setFixedSize(kBtnSize, kBtnSize);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QToolButton::clicked, this, [this, pageIdx]() {
            onTabChanged(pageIdx);
        });
        tabRow->addWidget(btn);
        tabButtons_.push_back(btn);
    };

    addTab(QStringLiteral("★"), kTabFreq);
    int idx = kTabBaseCat;
    for (Category c : tesseract::emoji::kCategories) {
        addTab(QString::fromUtf8(tesseract::emoji::category_tab_glyph(c)), idx++);
    }
    root->addWidget(tabStrip_);
}

QToolButton* EmojiPicker::makeEmojiButton(const QString& glyph) {
    auto* b = new QToolButton;
    b->setText(glyph);
    b->setFixedSize(kBtnSize, kBtnSize);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(glyph);
    connect(b, &QToolButton::clicked, this, [this, glyph]() {
        if (onSelected) onSelected(glyph);
        // Don't close — let users pick several in a row (mobile-keyboard feel).
        // Re-render the frequents page on the next open via refreshFrequents().
    });
    return b;
}

void EmojiPicker::populateGrid(QGridLayout* grid,
                               const std::vector<const Entry*>& items)
{
    // Clear existing buttons.
    while (auto* item = grid->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    int row = 0, col = 0;
    for (const auto* e : items) {
        auto* btn = makeEmojiButton(QString::fromUtf8(
            e->glyph.data(), static_cast<int>(e->glyph.size())));
        grid->addWidget(btn, row, col);
        if (++col >= kCols) { col = 0; ++row; }
    }
}

void EmojiPicker::refreshFrequents() {
    // Clear current frequents.
    while (auto* item = freqGrid_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    if (!client_) return;
    auto top = client_->recent_emoji_top(kFreqCount);
    int row = 0, col = 0;
    for (const auto& glyph : top) {
        auto* btn = makeEmojiButton(QString::fromUtf8(glyph.c_str()));
        freqGrid_->addWidget(btn, row, col);
        if (++col >= kCols) { col = 0; ++row; }
    }
}

void EmojiPicker::onSearchChanged(const QString& text) {
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        // Restore the previously selected category tab.
        pages_->setCurrentIndex(currentTabIdx_);
        for (int i = 0; i < (int)tabButtons_.size(); ++i)
            tabButtons_[i]->setChecked(i == currentTabIdx_);
        return;
    }
    showSearchResults(trimmed);
}

void EmojiPicker::onTabChanged(int idx) {
    currentTabIdx_ = idx;
    for (int i = 0; i < (int)tabButtons_.size(); ++i)
        tabButtons_[i]->setChecked(i == idx);
    if (idx == kTabFreq) refreshFrequents();
    pages_->setCurrentIndex(idx);
    if (!search_->text().isEmpty()) search_->clear();
}

void EmojiPicker::showSearchResults(const QString& query) {
    auto results = tesseract::emoji::filter(query.toStdString());
    populateGrid(searchGrid_, results);
    pages_->setCurrentIndex(kPageSearch);
    for (auto* tb : tabButtons_) tb->setChecked(false);
}

void EmojiPicker::popupAt(QWidget* anchor) {
    refreshFrequents();
    // If frequents are empty on first open, default to Smileys & People
    // (page index kTabBaseCat). Otherwise restore the user's last tab.
    int targetIdx = currentTabIdx_;
    const bool freq_empty = !client_ || client_->recent_emoji_top(1).empty();
    if (currentTabIdx_ == kTabFreq && freq_empty)
        targetIdx = kTabBaseCat;
    search_->clear();
    onTabChanged(targetIdx);
    search_->setFocus();

    // Position above the anchor, right-aligned to its right edge.
    QPoint anchorTopLeft = anchor->mapToGlobal(QPoint(0, 0));
    int x = anchorTopLeft.x() + anchor->width() - width();
    int y = anchorTopLeft.y() - height() - 6;
    // Keep on-screen.
    QScreen* scr = anchor->screen();
    if (scr) {
        QRect avail = scr->availableGeometry();
        if (x < avail.left())   x = avail.left();
        if (y < avail.top())    y = avail.top();
        if (x + width()  > avail.right())  x = avail.right()  - width();
        if (y + height() > avail.bottom()) y = avail.bottom() - height();
    }
    move(x, y);
    show();
    raise();
    activateWindow();
}

bool EmojiPicker::event(QEvent* e) {
    // Qt::Popup auto-closes on outside click; we just chain through.
    return QFrame::event(e);
}
