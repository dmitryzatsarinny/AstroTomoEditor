#pragma once
#ifndef MAINWINDOW_h
#define MAINWINDOW_h

#include "..\..\Services\Pool.h"
#include "..\..\Services\PatientInfo.h"
#include <Window/Explorer/ExplorerDialog.h>
#include <Window/Render/RenderView.h>
#include <QElapsedTimer>
#include <Services/CornerGrip.h>
#include "PatientDialog.h"
#include "AsyncProgressBar.h"
#include "SettingsDialog.h"

class QSplitter;
class QStackedWidget;
class QLabel;
class QProgressBar;
class QVBoxLayout;
class QWidget;
class QEvent;
class QSizeGrip;

class TitleBar;
class SeriesListPanel;
class PlanarView;
class RenderView;
class PatientDialog;
class AsyncProgressBar;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr,
        const QString& path = QString(),
        ExplorerDialog::SelectionKind kind = ExplorerDialog::SelectionKind::DicomFile);

    void onOpenStudy();
    void StartLoading();
    void StopLoading();

signals:
    void studyOpened(const QString& path);

private slots:
    // верхнеуровневые действия
    void onExitRequested();

    // переключение 2D <-> 3D
    void onShowPlanar2D();
    void onShowVolume3D();

    // реакции на сигналы SeriesListPanel
    void onSeriesPatientInfoChanged(const PatientInfo& info);
    void onSeriesActivated(const QString& seriesUID, const QVector<QString>& files);

    // окно с данными пациента
    void showPatientDetails();
    void showSettings();
    void onSave3DR();

protected:
    void changeEvent(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent*) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    // построение и настройка UI
    void buildUi();
    void buildStyles();
    void wireSignals();
    void startScan(); // запускает сканирование по mDicomPath/mKind
    void applyMaximizedUi(bool maximized);
    void showInfo(const QString& text);
    void positionCornerGrip();
    void retranslateUi();

private:
    // --- данные контекста ---
    QString                         mDicomPath;
    ExplorerDialog::SelectionKind   mKind{ ExplorerDialog::SelectionKind::DicomFile };
    PatientInfo                     mCurrentPatient;

    // --- верхняя панель ---
    TitleBar* mTitle{ nullptr };

    // --- центральная область ---
    QVBoxLayout* mOuter{ nullptr };
    QWidget* mCentralCard{ nullptr };
    QSplitter* mSplit{ nullptr };
    SeriesListPanel* mSeries{ nullptr };  // левая панель со списком серий
    QStackedWidget* mViewerStack{ nullptr };  // справа: 2D и 3D стеки
    PlanarView* mPlanar{ nullptr };  // 2D просмотр
    RenderView* mRenderView{ nullptr };  // 3D просмотр (лениво создаём)
    PatientDialog* mPatientDlg{ nullptr };
    SettingsDialog* mSettingsDlg{ nullptr };

    // --- нижняя панель / статус ---
    QWidget* mFooter{ nullptr };
    QLabel* mStatusText{ nullptr };
    QWidget* mProgBox{ nullptr };
    AsyncProgressBar* mProgress{ nullptr };
    CornerGrip* mCornerGrip{ nullptr };

    bool mLoading = false;
    QWidget* mUiToDisable{ nullptr };
};

#endif // MAINWINDOW_h