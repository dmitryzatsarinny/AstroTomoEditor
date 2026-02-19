#pragma once
// ShellFileDialog.cpp
#include "ShellFileDialog.h"

#include <QFileDialog>
#include <QVBoxLayout>
#include <QApplication>
#include <qabstractscrollarea.h>
#include <qcombobox.h>
#include <qabstractitemview.h>

#include <QSplitter>
#include <QListView>
#include <QTreeView>
#include <QTimer>
#include <QTreeView>
#include <QHeaderView>

static void forceDarkPalette(QWidget* w)
{
    if (!w) return;

    QPalette p = w->palette();

    const QColor win(0x26, 0x28, 0x2c);
    const QColor base(0x26, 0x28, 0x2c);
    const QColor alt(0x2a, 0x2b, 0x2f);
    const QColor txt(0xea, 0xea, 0xea);
    const QColor btn(0x2a, 0x2c, 0x30);
    const QColor hi(255, 255, 255, 50);

    p.setColor(QPalette::Window, win);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, txt);
    p.setColor(QPalette::WindowText, txt);
    p.setColor(QPalette::Button, btn);
    p.setColor(QPalette::ButtonText, txt);
    p.setColor(QPalette::Highlight, hi);
    p.setColor(QPalette::HighlightedText, Qt::white);

    w->setPalette(p);

    // важно: иначе часть виджетов не заливает фон и остаются "светлые островки"
    w->setAutoFillBackground(true);

    // у scroll-area фон рисуется на viewport, а не на самом контейнере
    if (auto* area = qobject_cast<QAbstractScrollArea*>(w))
    {
        if (auto* vp = area->viewport())
        {
            vp->setPalette(p);
            vp->setAutoFillBackground(true);
        }
    }
}



ShellFileDialog::ShellFileDialog(QWidget* parent,
    const QString& title,
    int typeofwindow,
    const QString& directory,
    const QString& filter)
    : DialogShell(parent, title, typeofwindow)
{
    resize(860, 520);

    auto* root = contentWidget();
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(12, 10, 12, 12);
    lay->setSpacing(0);

    dlg_ = new QFileDialog(root, title, directory, filter);

    // обязательно: иначе будет нативный и не встроится
    dlg_->setOption(QFileDialog::DontUseNativeDialog, true);

    // чтобы не было своих кнопок "в отдельном окне", встраиваем как виджет
    dlg_->setWindowFlags(Qt::Widget);
    dlg_->setSizeGripEnabled(false);

    connect(dlg_, &QFileDialog::fileSelected, this, [this](const QString&) { accept(); });
    connect(dlg_, &QFileDialog::rejected, this, &QDialog::reject);
    connect(dlg_, &QFileDialog::accepted, this, &QDialog::accept);

    QTimer::singleShot(0, dlg_, [this] {
        QSplitter* target = nullptr;
        int sidebarIndex = -1;
        int mainIndex = -1;

        const auto splitters = dlg_->findChildren<QSplitter*>();
        for (auto* sp : splitters)
        {
            if (!sp || sp->count() < 2) continue;

            QWidget* w0 = sp->widget(0);
            QWidget* w1 = sp->widget(1);

            auto hasListView = [](QWidget* w) {
                return w && (qobject_cast<QListView*>(w) || !w->findChildren<QListView*>().isEmpty());
                };
            auto hasTreeView = [](QWidget* w) {
                return w && (qobject_cast<QTreeView*>(w) || !w->findChildren<QTreeView*>().isEmpty());
                };

            // классическая компоновка: слева sidebar (QListView), справа main (QTreeView)
            if (hasListView(w0) && hasTreeView(w1)) {
                target = sp; sidebarIndex = 0; mainIndex = 1; break;
            }
            // иногда бывает наоборот
            if (hasTreeView(w0) && hasListView(w1)) {
                target = sp; sidebarIndex = 1; mainIndex = 0; break;
            }
        }

        if (!target || sidebarIndex < 0 || mainIndex < 0)
            return;

        // Сжимаем и прячем sidebar
        target->setCollapsible(sidebarIndex, true);
        target->setStretchFactor(mainIndex, 1);
        target->setStretchFactor(sidebarIndex, 0);

        auto sizes = target->sizes();
        if (sizes.size() >= 2) {
            sizes[sidebarIndex] = 0;
            sizes[mainIndex] = 10000;
            target->setSizes(sizes);
        }

        if (auto* sb = target->widget(sidebarIndex)) {
            sb->setMinimumWidth(0);
            sb->setMaximumWidth(0);
            sb->hide();
        }
        });

    lay->addWidget(dlg_, 1);

    QTimer::singleShot(0, dlg_, [this]
        {
            const auto trees = dlg_->findChildren<QTreeView*>();
            for (auto* tree : trees)
            {
                if (!tree || !tree->model())
                    continue;

                // основной список файлов обычно имеет 4 колонки
                if (tree->model()->columnCount() >= 3)
                {
                    auto* header = tree->header();
                    if (!header) continue;

                    header->setStretchLastSection(false);

                    // Имя — растягиваем
                    header->setSectionResizeMode(0, QHeaderView::Stretch);

                    // Остальные — по содержимому
                    for (int i = 1; i < tree->model()->columnCount(); ++i)
                        header->setSectionResizeMode(i, QHeaderView::ResizeToContents);

                    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                    break;
                }
            }
        });

    // если хочешь, подтягиваем общий стиль
    dlg_->setStyleSheet(qApp->styleSheet() + QStringLiteral(R"(
    /* базовые цвета */
    QFileDialog {
        background: #1f2023;
        color: #eaeaea;
    }
    
    /* любые панели/рамки внутри диалога */
    QFileDialog QWidget {
        color: #eaeaea;
    }
    QFileDialog QFrame,
    QFileDialog QAbstractScrollArea,
    QFileDialog QAbstractItemView,
    QFileDialog QScrollArea {
        background: #2a2b2f;
        color: #ffffff;
        border: none;
    }
    
    /* белая полоса часто бывает у QSplitter handle или у пустого QFrame */
    QFileDialog QSplitter::handle {
        background: #1f2023;
    }
    
    /* левые списки "My Computer" и дерево */
    QFileDialog QTreeView,
    QFileDialog QListView {
        background: #2a2b2f;
        color: #ffffff;
        alternate-background-color: #26272b;
        selection-background-color: #3a6ea5;
        selection-color: #ffffff;
    }
    
    /* заголовки таблицы */
    QFileDialog QHeaderView::section {
        background: #1f2023;
        color: #d8d8d8;
        padding: 4px 6px;
        border: none;
    }
    
    /* тулбар-кнопки справа (вверх/назад/новая папка и т.п.) */
    QFileDialog QToolButton {
        background: rgba(255,255,255,0.08);
        color: #ffffff;
        border: 1px solid rgba(255,255,255,0.12);
        border-radius: 6px;
        padding: 2px;
    }
    QFileDialog QToolButton:hover {
        background: rgba(255,255,255,0.14);
    }
    
    /* обычные кнопки Save/Cancel */
    QFileDialog QPushButton {
        background: rgba(255,255,255,0.10);
        color: #ffffff;
        border: 1px solid rgba(255,255,255,0.14);
        border-radius: 6px;
        padding: 6px 12px;
    }
    QFileDialog QPushButton:hover {
        background: rgba(255,255,255,0.16);
    }
    
    /* скроллбары часто остаются светлыми */
    QFileDialog QScrollBar:vertical,
    QFileDialog QScrollBar:horizontal {
        background: #1f2023;
    }
    QFileDialog QScrollBar::handle:vertical,
    QFileDialog QScrollBar::handle:horizontal {
        background: rgba(255,255,255,0.18);
        border-radius: 6px;
    }
    QFileDialog QScrollBar::add-line,
    QFileDialog QScrollBar::sub-line {
        background: transparent;
        border: none;
    }
/* --- combobox как в ExplorerDialog --- */
    QFileDialog QComboBox {
        background:#26282c;
        color:#f0f0f0;
        border:1px solid rgba(255,255,255,0.25);
        border-radius:4px;
        padding:2px 22px 2px 6px;
    }
    
    QFileDialog QComboBox:hover {
        background:#2c2f34;
    }
    
    QFileDialog QComboBox::drop-down {
        border:none;
        width:18px;
    }
    
    QFileDialog QComboBox::down-arrow {
        image:none;
    }
    
    /* --- самое важное: выпадающий список --- */
    QFileDialog QComboBox QAbstractItemView {
        background:#26282c;
        color:#f0f0f0;
        border:1px solid rgba(255,255,255,0.25);
        outline:0;
        selection-background-color:rgba(255,255,255,0.20);
        selection-color:#ffffff;
    }
    
    QFileDialog QComboBox QAbstractItemView::item {
        height:22px;
        padding:2px 6px;
    }
    
    QFileDialog QComboBox QAbstractItemView::item:hover {
        background:rgba(255,255,255,0.10);
    }
    
    QFileDialog QComboBox QAbstractItemView::item:selected {
        background:rgba(255,255,255,0.20);
    }
    
    /* --- иногда всплывает меню (контекст, кнопки тулбара) --- */
    QFileDialog QMenu {
        background:#26282c;
        color:#f0f0f0;
        border:1px solid rgba(255,255,255,0.25);
    }
    
    QFileDialog QMenu::item:selected {
        background:rgba(255,255,255,0.20);
        color:#ffffff;
    }
    
    QFileDialog QToolBar {
        background: #1f2023;
        border: none;
    }
    QFileDialog QToolBar QWidget {
        background: #1f2023;
    }
    
    /* верхняя строка "Перейти к" и нижняя зона с полями */
    QFileDialog QToolBar,
    QFileDialog QToolBar QWidget {
        background: #1f2023;
    }
    
    QFileDialog QDialogButtonBox,
    QFileDialog QDialogButtonBox QWidget {
        background: #1f2023;
    }
    
    /* нижняя панель QFileDialog часто собирается из QWidget/QFrame */
    QFileDialog > QWidget {
        background: #1f2023;
    }
    
    /* если вдруг полосы идут от групп/рамок */
    QFileDialog QGroupBox,
    QFileDialog QGroupBox QWidget,
    QFileDialog QFrame[frameShape="4"] { /* StyledPanel */
        background: #26282c;
    }

    )"));

    forceDarkPalette(dlg_);
    for (auto* w : dlg_->findChildren<QWidget*>())
        forceDarkPalette(w);

    // отдельно добиваем попапы комбобоксов (их view часто живет своей жизнью)
    for (auto* cb : dlg_->findChildren<QComboBox*>())
    {
        if (auto* v = cb->view())
        {
            forceDarkPalette(v);
            v->setAutoFillBackground(true);
        }
    }
}