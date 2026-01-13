#pragma once
#ifndef EXPLORERDIALOG_h
#define EXPLORERDIALOG_h

#include "..\..\Services\Pool.h"
#include "..\MainWindow\DialogShell.h"
#include "..\MainWindow\AsyncProgressBar.h"
#include <Window/MainWindow/SettingsDialog.h>

class QComboBox;                                                // Предварительные объявления (forward declarations) Qt-классов.
class QTreeView;                                                // Они уменьшают связность/время сборки: нам нужна только ссылка/указатель,
class QFileSystemModel;                                         // а полные определения будут подключены в .cpp.
class QDialogButtonBox;
class QModelIndex;
class QSortFilterProxyModel;                                    // Тоже forward-declaration: используется в сигнатуре слота.
class ContentFilterProxy;
class QCheckBox;
class TitleBar;
class AsyncProgressBar;


class ExplorerDialog : public DialogShell
{
    Q_OBJECT                                                    // Макрос Qt MOC: включает мета-информацию (RTTI Qt), поддержку сигналов/слотов и dynamic cast внутри Qt.

public:
    explicit ExplorerDialog(QWidget* parent = nullptr);         // Конструктор.
    ~ExplorerDialog() override;                                 // explicit — запрещает неявные преобразования (например, из QWidget\*).
                                                                // parent = nullptr — родитель по умолчанию отсутствует. Если parent задан,
                                                                // Qt возьмёт на себя владение виджетом (parent-child уничтожение).

                                                                // Установить корневой путь (например, "C:/")
    void setRootPath(const QString& path);                      // Метод конфигурирует модель/вид, чтобы показывать иерархию
                                                                // начиная с указанной директории. Передача по const-ссылке — без копий.

                                                                // Фильтры имён файлов (по умолчанию — DICOM)
    void setNameFilters(const QStringList& filters);            // Устанавливает маски отображаемых файлов (например, {"*.dcm", "DICOMDIR"}).
                                                                // Внутри будет передано в QFileSystemModel::setNameFilters(...).

                                                                // Возвращает выбранный файл (пусто, если ничего не выбрано)
    QString selectedFile() const;                               // Возвращает один путь — основной выбор пользователя. const в конце гарантирует, что метод не меняет состояние объекта.

                                                                // Если нужно — все выбранные (включена одиночная выборка, но метод пригодится)
    QStringList selectedFiles() const;                          // Вернёт все выбранные элементы; пригодится, если позже переключишь QTreeView на множественный выбор.


    enum class SelectionKind { None, DicomFile, DicomDir, File3DR, DicomFolder };
    SelectionKind selectedKind() const;
    QString selectedPath() const; // файл или папка, в зависимости от kind
    void retranslateUi();

private slots:                                                  // Раздел слотов Qt — методы, на которые можно "подписывать" сигналы.
    void onDriveChanged(int index);                             // Реакция на смену диска в combo: перестраиваем корень и путь.

    void onPathActivated(int index);                            // Пользователь выбрал пункт в выпадающем списке путей.

    void onPathEdited();                                        // Пользователь отредактировал текст пути вручную (по Enter).

    void onDoubleClicked(const QModelIndex& idx);               // Двойной клик по элементу в дереве: если папка — входим,                                                       // если файл — выбираем и активируем Ok.

    void onSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

    void onTypeChanged(int index);                              // смена типа

    void onDirectoryAboutToChange(const QString& path);         // вызывать при переходе

    void onDirectoryLoaded(const QString& path);                // сигнал модели

private:
    enum class LoadState { Ready, Opening };

    void populateDrives();                                      // Собрать список доступных логических дисков (C:, D:, ...).
    void navigateTo(const QString& path);                       // Переход к произвольному пути: синхронизирует модель, дерево и combo.
    void applyFiltersForType();                                 // применить фильтры по типу
    void updateOkState(const bool state);                       // Включить/выключить кнопку Ok в зависимости от валидности выбора.
    void setStatus(LoadState st, const QString& text = {});
    void showBusy(const QString& text);
    void hideBusy();
    void showSettings();

    // помощник: путь по индексу из вида (нужно мэппить через прокси)
    QString filePathFromViewIndex(const QModelIndex& viewIdx) const;

    bool dirHasDicom(const QString& dirPath, int maxProbe = 10) const;      // есть хотя бы 1 DICOM-файл
    bool dirHasDicomdir(const QString& dirPath) const;                      // есть файл DICOMDIR
    bool isDicomFile(const QString& filePath) const;                        // файл — DICOM (сигнатура/расширение)
    bool is3drFile(const QString& filePath) const;

                                                                // Указатели на дочерние виджеты/модель. Владелец — сам диалог (через parent),
                                                                // поэтому "сырые" указатели здесь — нормально: Qt удалит детей в деструкторе родителя.

    QComboBox* m_driveCombo = nullptr;                          // Combo со списком дисков.
    QComboBox* m_pathCombo = nullptr;                           // Combo/line edit текущего пути (история путей
    QComboBox* m_typeCombo = nullptr;                           // Combo со списком файлов.
    QCheckBox* m_magicCheck = nullptr;                          // Флаг глубокой проверки файлов без расширений.
    QTreeView* m_view = nullptr;                                // Дерево файловой системы.
    QFileSystemModel* m_model = nullptr;                        // Модель файловой системы (лениво читает директории).
    ContentFilterProxy* m_proxy = nullptr;
    QDialogButtonBox* m_buttons = nullptr;                      // Кнопки Ok/Cancel.

    // --- статус-бар ---
    QWidget* mStatusBar = nullptr;
    AsyncProgressBar* mBusy = nullptr;
    QTimer* mBusyDelayTimer = nullptr;
    QTimer* mOpenTimeout = nullptr;   // страховка «на всякий случай»
    LoadState     mState = LoadState::Ready;
    SettingsDialog* mSettingsDlg{ nullptr };

    QString       mPendingPath;
    QString       mCurrentRootPath;
};

#endif