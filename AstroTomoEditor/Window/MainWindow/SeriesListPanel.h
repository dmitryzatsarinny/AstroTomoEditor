#pragma once
#ifndef SERIESLISTPANEL_h
#define SERIESLISTPANEL_h

#include <QWidget>
#include "..\..\Services\PatientInfo.h"

#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QVector>

constexpr int kRowH = 84;
constexpr int maxfiles = 8192;

struct PatientInfo;
struct SeriesScanResult;
class QListWidget;
class QListWidgetItem;

struct SeriesItem {
    QString seriesKey;
    QString studyID;
    QString description;
    QString firstFile;
    int     numImages = 0;
    QImage  thumb;
};

enum Roles { RoleSeriesKey = Qt::UserRole, RoleNumImages, RoleDescription };

struct SeriesScanResult
{
    struct QuickDicomFile
    {
        QString path;
        QString fileName;
        bool    hasZ = false;
        double  z = std::numeric_limits<double>::quiet_NaN();
        bool    hasInstance = false;
        int     instance = 0;
        QString modality;
        QString seriesDescription;
        QString studyUID;
        QString seriesNumber;

        bool hasPixelKey = false;
        int rows = 0;
        int cols = 0;
        int bitsAllocated = 0;
        int samplesPerPixel = 0;
        int pixelRepresentation = 0;
        QString photometric;
    };

    QHash<QString, QVector<QuickDicomFile>> entriesBySeries;
    QHash<QString, QVector<QString>> filesBySeries;
    QVector<SeriesItem> items;
    PatientInfo patientInfo;
    bool patientInfoValid = false;
    int totalFiles = 0;
    bool canceled = false;
};

class SeriesListPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SeriesListPanel(QWidget* parent = nullptr);
    ~SeriesListPanel() override;

    static QImage makeThumbImageFromDicom(const QString& file);
    void retranslateUi();

protected:
    void changeEvent(QEvent* e) override;

public slots:
    void scanSingleFile(const QString& filePath);
    void scan3drFile(const QString& filePath);
    void scanDicomDir(const QString& rootPath);
    void scanStudy(const QString& rootPath);
    void cancelScan();

signals:
    void patientInfoChanged(const PatientInfo& info);
    void seriesActivated(const QString& seriesUID, const QVector<QString>& files);

    void scanStarted(int totalFiles);
    void scanProgress(int processed, int totalFiles, const QString& currentPath);
    void scanFinished(int seriesCount, int totalFiles);

private:
    // helpers
    void ensureWorker();
    void abortThumbLoading();
    void enqueueThumbRequest(QListWidgetItem* item, const QString& filePath);
    void startNextThumb();
    void handleScanResult(const SeriesScanResult& result);
    void handleThumbReady();
    void populate(const QVector<SeriesItem>& items);
    void updatePatientInfoForSeries(const QString& seriesKey, const QVector<QString>& files);
private:
    QListWidget* mList = nullptr;

    // файлы серий: ключ -> список путей
    QHash<QString, QVector<QString>> mFilesBySeries;

    // сканер в отдельном потоке
    QThread* mScanThread = nullptr;
    QObject* mScanWorker = nullptr; // SeriesScanWorker*

    // генерация превью в фоне
    QFutureWatcher<QImage> mThumbWatcher;
    QQueue<QPair<QListWidgetItem*, QString>> mPendingThumbs;
    QListWidgetItem* mCurrentThumbItem = nullptr;
    QString mCurrentThumbFile;

    // флаг отмены синхронных операций
    bool mCancelScan = false;

    PatientInfo mBasePatientInfo;
    bool mHasBasePatientInfo = false;
};

#endif
