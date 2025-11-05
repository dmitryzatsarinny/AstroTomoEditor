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


#endif