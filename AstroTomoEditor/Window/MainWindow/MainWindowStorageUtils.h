#pragma once

#include "DicomSeriesSaveDialog.h"

#include <QString>
#include <QVector>

namespace MainWindowStorageUtils
{
    struct ExportedDicomFile
    {
        QString sourcePath;
        QString relativePath;
        QString patientName;
        QString patientId;
        QString studyInstanceUid;
        QString studyId;
        QString studyDate;
        QString studyTime;
        QString accessionNumber;
        QString studyDescription;
        QString seriesInstanceUid;
        QString seriesNumber;
        QString seriesDescription;
        QString modality;
        QString sopClassUid;
        QString sopInstanceUid;
        QString transferSyntaxUid;
        QString instanceNumber;
    };

    bool readExportedDicomFileMeta(const QString& sourcePath, const QString& relativePath, ExportedDicomFile& out);
    bool writeDicomDirFile(const QString& filePath, const QVector<ExportedDicomFile>& files);
    QVector<PatientFolderEntry> loadHdBasePatients(const QString& basePath, bool forceRefresh = false);
    void updateCachedPatientCtState(const QString& basePath, const QString& patientFolderPath, bool hasCt);
    QString sanitizeSeriesFolderName(const QString& description, const QString& fallbackSeriesKey);
}