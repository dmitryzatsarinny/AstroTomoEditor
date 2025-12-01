#pragma once
#include <QColor.h>
#include <QStyle.h>
#include <QStyledItemDelegate.h>
#include <QPainter.h>

class HoverDelegate : public QStyledItemDelegate
{
public:
    explicit HoverDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
        const QModelIndex& idx) const override
    {
        QStyleOptionViewItem o(opt);

        // Подсветка строки при наведении
        if (o.state & QStyle::State_MouseOver)
        {
            p->save();
            p->fillRect(o.rect, QColor(120, 120, 120, 80));
            p->restore();
        }

        // Обычная отрисовка поверх
        QStyledItemDelegate::paint(p, o, idx);
    }
};
