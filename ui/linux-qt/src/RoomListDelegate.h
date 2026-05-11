#pragma once
#include <QStyledItemDelegate>

namespace qt6 {

enum RoomItemRole {
    LastMessageRole = Qt::UserRole + 1,
    UnreadCountRole = Qt::UserRole + 2,
    RoomIdRole      = Qt::UserRole + 3,
};

class RoomListDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit RoomListDelegate(QObject* parent = nullptr);

    void  paint(QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

} // namespace qt6
