#include "RoomListDelegate.h"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>

namespace qt6 {

static constexpr int kRowHeight     = 62;
static constexpr int kAvatarSize    = 36;
static constexpr int kMargin        = 8;
static constexpr int kAvatarGap     = 8;
static constexpr int kBadgeHeight   = 20;
static constexpr int kBadgeMinWidth = 20;

RoomListDelegate::RoomListDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

void RoomListDelegate::paint(QPainter* painter,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());
    else if (option.state & QStyle::State_MouseOver)
        painter->fillRect(option.rect, option.palette.alternateBase());

    const int cx = option.rect.left();
    const int cy = option.rect.top();

    // ---- Avatar ----
    QRect avatarRect(cx + kMargin,
                     cy + (kRowHeight - kAvatarSize) / 2,
                     kAvatarSize, kAvatarSize);
    QPixmap avatar = index.data(Qt::DecorationRole).value<QPixmap>();

    if (!avatar.isNull()) {
        QPainterPath clip;
        clip.addEllipse(avatarRect);
        painter->setClipPath(clip);
        painter->drawPixmap(avatarRect, avatar);
        painter->setClipping(false);
    } else {
        painter->setBrush(QColor(0x8E, 0x8E, 0x93));
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(avatarRect);

        QString name = index.data(Qt::DisplayRole).toString();
        QFont f = painter->font();
        f.setPixelSize(16);
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(Qt::white);
        painter->drawText(avatarRect, Qt::AlignCenter,
                          name.isEmpty() ? "?" : QString(name[0].toUpper()));
    }

    // ---- Unread badge ----
    int unread = index.data(UnreadCountRole).toInt();
    QString badge;
    if (unread > 0)
        badge = unread > 99 ? "99+" : QString::number(unread);

    int badgeW = 0;
    if (!badge.isEmpty()) {
        QFont bf = painter->font();
        bf.setPixelSize(11);
        bf.setBold(true);
        badgeW = std::max(kBadgeMinWidth,
                          QFontMetrics(bf).horizontalAdvance(badge) + 12);
    }

    // ---- Text area bounds ----
    int textLeft  = cx + kMargin + kAvatarSize + kAvatarGap;
    int textRight = option.rect.right() - kMargin - badgeW - (badgeW ? 4 : 0);

    // ---- Room name ----
    QFont nameFont = painter->font();
    nameFont.setPixelSize(13);
    nameFont.setBold(true);
    painter->setFont(nameFont);
    painter->setPen(option.state & QStyle::State_Selected
                    ? option.palette.highlightedText().color()
                    : option.palette.text().color());
    QRect nameRect(textLeft, cy + 10, textRight - textLeft, 18);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      index.data(Qt::DisplayRole).toString());

    // ---- Last-message preview ----
    QString preview = index.data(LastMessageRole).toString();
    if (!preview.isEmpty()) {
        QFont pf = painter->font();
        pf.setPixelSize(11);
        pf.setBold(false);
        painter->setFont(pf);
        painter->setPen(QColor(0x99, 0x99, 0x99));
        QRect previewRect(textLeft, cy + 32, textRight - textLeft, 16);
        painter->drawText(previewRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(pf).elidedText(
                              preview, Qt::ElideRight, previewRect.width()));
    }

    // ---- Unread badge pill ----
    if (!badge.isEmpty()) {
        QFont bf = painter->font();
        bf.setPixelSize(11);
        bf.setBold(true);
        painter->setFont(bf);

        int bx = option.rect.right() - kMargin - badgeW;
        int by = cy + (kRowHeight - kBadgeHeight) / 2;
        QRect badgeRect(bx, by, badgeW, kBadgeHeight);

        painter->setBrush(QColor(0x00, 0x84, 0xFF));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(badgeRect, 10, 10);

        painter->setPen(Qt::white);
        painter->drawText(badgeRect, Qt::AlignCenter, badge);
    }

    painter->restore();
}

QSize RoomListDelegate::sizeHint(const QStyleOptionViewItem&,
                                  const QModelIndex&) const
{
    return { 0, kRowHeight };
}

} // namespace qt6
