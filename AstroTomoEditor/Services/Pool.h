#pragma once
#ifndef Pool_h
#define Pool_h

#pragma warning(disable: 4996)
#pragma warning(disable:4244)

#include <algorithm>
#include <vector>

#include <QApplication>
#include <QVBoxLayout>                        // Вертикальный лэйаут (контейнер для виджетов).
#include <QHBoxLayout>                        // Горизонтальный лэйаут (верхняя панель).
#include <QComboBox>                          // Выпадающие списки (диски и путь).
#include <QTreeView>                          // Древовидное/табличное представление файлов.
#include <QFileSystemModel>                   // Модель файловой системы (ленивая подгрузка).
#include <QDialogButtonBox>                   // Стандартные кнопки Ok/Cancel.
#include <QHeaderView>                        // Настройка колонок заголовка таблицы.
#include <QLineEdit>                          // Доступ к lineEdit() у QComboBox.
#include <QDir>                               // Работа с путями, фильтрами, списком дисков.
#include <QFileInfo>                          // Информация о файле/папке.
#include <QItemSelectionModel>                // Слежение за выбором в представлении.
#include <QDialog>
#include <QStringList>
#include <QPushButton>
#include <QString>
#include <QFile>
#include <QSortFilterProxyModel>
#include <QHash>
#include <QDateTime>
#include <QDirIterator>
#include <QPointer>
#include <QMessageBox>
#include <QProgressBar>
#include <QLabel>
#include <QTimer>
#include <QPixmap>
#include <QObject>
#include <QThread>
#include <QFutureWatcher>
#include <QTableView>
#include <QSignalBlocker>
#include <QMainWindow>
#include <QDockWidget>
#include <QSplitter>
#include <QToolBar>
#include <QStatusBar>
#include <QShortcut>
#include <QKeySequence>
#include <QWidget>
#include <QImage>
#include <QVector>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QListWidget>
#include <QBuffer>
#include <QMap>
#include <QCollator>
#include <QProgressDialog>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QStackedWidget>
#include <QKeyEvent>
#include <QSlider>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QEventLoop>
#include <QtMath>
#include <QMouseEvent>
#include <QToolButton>
#include <QSizeGrip>
#include <QFormLayout>
#include <QButtonGroup>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QMatrix3x3>

// VTK 
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkDICOMImageReader.h>
#include <vtkImageShiftScale.h>
#include <vtkImageExtractComponents.h>
#include <vtkPointData.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMMetaData.h>
#include <vtkDICOMDictionary.h>
#include <vtkStringArray.h>
#include <vtkExtractVOI.h>
#include <vtkImageMapToWindowLevelColors.h>
#include <vtkImageExport.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkDICOMParser.h>
#include <vtkDICOMTag.h>
#include <vtkFileOutputWindow.h>
#include <vtkImageCast.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif


#include <QScrollBar>
#include <qstyleditemdelegate.h>

namespace {
    class FixedDownComboBox : public QComboBox {
    public:
        using QComboBox::QComboBox;

    protected:
        void showPopup() override {
            QComboBox::showPopup();

            QAbstractItemView* v = view();
            if (!v) return;

            QWidget* popup = v->window();
            if (!popup) popup = v;

            // ---------- палитра попапа + списка ----------
            QPalette pal = popup->palette();
            pal.setColor(QPalette::Window, QColor("#1f2023"));           // фон окна
            pal.setColor(QPalette::Base, QColor("#1f2023"));          // фон списка
            pal.setColor(QPalette::Text, QColor("#f0f0f0"));          // текст
            pal.setColor(QPalette::Highlight, QColor(80, 150, 255));    // фон выделения
            pal.setColor(QPalette::HighlightedText, Qt::white);       // текст в выделении

            popup->setPalette(pal);
            popup->setAutoFillBackground(true);

            v->setPalette(pal);
            if (auto* vp = v->viewport()) {
                vp->setPalette(pal);
                vp->setAutoFillBackground(true);
            }

            // на всякий — убираем рамку у контейнера
            if (auto* fr = qobject_cast<QFrame*>(popup))
                fr->setFrameStyle(QFrame::NoFrame);

            // высота строки
            int rowH = v->sizeHintForRow(0);
            if (rowH <= 0)
                rowH = v->fontMetrics().height() + 6;

            const int visibleCount = qMax(1, count());
            const int totalH = rowH * visibleCount;
            const int totalW = width();

            // точка ПОД комбобоксом
            const QPoint globalPos = mapToGlobal(QPoint(0, height()));

            // ставим контейнер попапа
            popup->setGeometry(QRect(globalPos, QSize(totalW, totalH)));
            if (auto* lay = popup->layout())
                lay->setContentsMargins(0, 0, 0, 0);

            // растягиваем список на весь popup
            v->setGeometry(popup->rect());
            v->setMinimumHeight(totalH);
            v->setMaximumHeight(totalH);

            // рубим скроллбары
            v->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            v->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            if (auto* sb = v->verticalScrollBar()) {
                sb->setVisible(false);
                sb->setEnabled(false);
                sb->setMaximum(0);
            }
            if (auto* sb = v->horizontalScrollBar()) {
                sb->setVisible(false);
                sb->setEnabled(false);
                sb->setMaximum(0);
            }

            v->setMouseTracking(true);


        }
    };

    class FlatTreeView : public QTreeView
    {
    public:
        using QTreeView::QTreeView;

    protected:
        void drawRow(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override
        {
            QStyleOptionViewItem opt(option);
            opt.state &= ~QStyle::State_HasFocus;
            QTreeView::drawRow(painter, opt, index);
        }
    };

    class NoFocusDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override
        {
            QStyleOptionViewItem opt(option);
            // убираем фокус с отдельной ячейки
            opt.state &= ~QStyle::State_HasFocus;
            QStyledItemDelegate::paint(painter, opt, index);
        }
    };
}

#endif