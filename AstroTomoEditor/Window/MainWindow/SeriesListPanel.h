#pragma once
#ifndef SERIESLISTPANEL_h
#define SERIESLISTPANEL_h

#include "..\..\Services\Pool.h"
#include "..\..\Services\PatientInfo.h"

struct PatientInfo;

struct SeriesItem {
    QString seriesKey;
    QString studyID;
    QString description;
    QString firstFile;
    int     numImages = 0;
    QImage  thumb;
};

class SeriesListPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SeriesListPanel(QWidget* parent = nullptr);
    
public slots:
    void scanSingleFile(const QString& filePath);
    void scan3drFile(const QString& filePath);
    void scanDicomDir(const QString& rootPath);
    void scanStudy(const QString& rootPath);
    void cancelScan() { mCancelScan = true; } 
    
    

signals:
    void patientInfoChanged(const PatientInfo& info);
    void seriesActivated(const QString& seriesUID, const QVector<QString>& files);

    void scanStarted(int totalFiles);
    void scanProgress(int processed, int totalFiles, const QString& currentPath);
    void scanFinished(int seriesCount, int totalFiles);

private:
    bool tryParseDicomdirWithVtk(const QString& baseDir, PatientInfo& pinfo, QVector<SeriesItem>& items);
    void populate(const QVector<SeriesItem>& items);
    bool isLikelyDicom(const QString& path);

    QListWidget* mList = nullptr;
    QHash<QString, QVector<QString>> mFilesBySeries;

    static QString strOrEmpty(const char* s);
    static QImage makeThumbImageFromDicom(const QString& file);

    bool mCancelScan = false; // ← флаг отмены
};

#endif