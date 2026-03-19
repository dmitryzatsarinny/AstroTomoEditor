#include "MainWindow.h"

#include "TitleBar.h"
#include "SeriesListPanel.h"
#include "PlanarView.h"
#include <QScopedValueRollback>
#include <Services/Save3DR.h>
#include <Services/AppConfig.h>
#include <QApplication>
#include <QEvent>
#include <QWindow>
#include <QElapsedTimer>
#include <QCloseEvent>
#include <Services/LanguageManager.h>
#include <Window/ServiceWindow/CustomMessageBox.h>
#include <Window/ServiceWindow/ShellFileDialog.h>
#include <QSettings>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFileDialog>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
#include <QDateTime>
#include <QUuid>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace 
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

    struct DicomDirRecord
    {
        enum class Type
        {
            Patient,
            Study,
            Series,
            Image
        };

        Type type = Type::Image;
        int parentIndex = -1;
        QVector<int> childIndices;
        QMap<QPair<quint16, quint16>, QByteArray> elements;
        int itemSize = 0;
        quint32 startOffset = 0;
    };

    QPair<quint16, quint16> tagKey(quint16 group, quint16 element)
    {
        return qMakePair(group, element);
    }

    QString dicomTagValue(vtkDICOMMetaData* meta, quint16 group, quint16 element)
    {
        if (!meta)
            return {};

        const vtkDICOMTag tag(group, element);
        if (!meta->Has(tag))
            return {};

        return QString::fromUtf8(meta->Get(tag).AsString()).trimmed();
    }

    bool readExportedDicomFileMeta(const QString& sourcePath, const QString& relativePath, ExportedDicomFile& out)
    {
        auto parser = vtkSmartPointer<vtkDICOMParser>::New();
        auto meta = vtkSmartPointer<vtkDICOMMetaData>::New();
        parser->SetMetaData(meta);
        parser->SetFileName(sourcePath.toUtf8().constData());
        parser->Update();

        out.sourcePath = sourcePath;
        out.relativePath = relativePath;
        out.patientName = dicomTagValue(meta, 0x0010, 0x0010);
        out.patientId = dicomTagValue(meta, 0x0010, 0x0020);
        out.studyInstanceUid = dicomTagValue(meta, 0x0020, 0x000D);
        out.studyId = dicomTagValue(meta, 0x0020, 0x0010);
        out.studyDate = dicomTagValue(meta, 0x0008, 0x0020);
        out.studyTime = dicomTagValue(meta, 0x0008, 0x0030);
        out.accessionNumber = dicomTagValue(meta, 0x0008, 0x0050);
        out.studyDescription = dicomTagValue(meta, 0x0008, 0x1030);
        out.seriesInstanceUid = dicomTagValue(meta, 0x0020, 0x000E);
        out.seriesNumber = dicomTagValue(meta, 0x0020, 0x0011);
        out.seriesDescription = dicomTagValue(meta, 0x0008, 0x103E);
        out.modality = dicomTagValue(meta, 0x0008, 0x0060);
        out.sopClassUid = dicomTagValue(meta, 0x0008, 0x0016);
        out.sopInstanceUid = dicomTagValue(meta, 0x0008, 0x0018);
        out.transferSyntaxUid = dicomTagValue(meta, 0x0002, 0x0010);
        out.instanceNumber = dicomTagValue(meta, 0x0020, 0x0013);

        return !out.relativePath.isEmpty()
            && !out.studyInstanceUid.isEmpty()
            && !out.seriesInstanceUid.isEmpty()
            && !out.sopInstanceUid.isEmpty();
    }

    QByteArray paddedDicomValue(QByteArray value, const QByteArray& vr)
    {
        const bool zeroPad = (vr == "UI") || (vr == "OB") || (vr == "OW") || (vr == "OF") || (vr == "UN") || (vr == "UT");
        if (value.size() % 2 != 0)
            value.append(zeroPad ? '\0' : ' ');

        return value;
    }

    void appendLe16(QByteArray& data, quint16 value)
    {
        data.append(char(value & 0xFF));
        data.append(char((value >> 8) & 0xFF));
    }

    void appendLe32(QByteArray& data, quint32 value)
    {
        data.append(char(value & 0xFF));
        data.append(char((value >> 8) & 0xFF));
        data.append(char((value >> 16) & 0xFF));
        data.append(char((value >> 24) & 0xFF));
    }

    QByteArray makeElement(quint16 group, quint16 element, const QByteArray& vr, QByteArray value)
    {
        value = paddedDicomValue(std::move(value), vr);

        QByteArray out;
        appendLe16(out, group);
        appendLe16(out, element);
        out.append(vr);

        if (vr == "OB" || vr == "OW" || vr == "OF" || vr == "SQ" || vr == "UT" || vr == "UN")
        {
            out.append('\0');
            out.append('\0');
            appendLe32(out, quint32(value.size()));
        }
        else
        {
            appendLe16(out, quint16(value.size()));
        }

        out.append(value);
        return out;
    }

    QByteArray makeElementU16(quint16 group, quint16 element, const QByteArray& vr, quint16 value)
    {
        QByteArray raw;
        appendLe16(raw, value);
        return makeElement(group, element, vr, raw);
    }

    QByteArray makeElementU32(quint16 group, quint16 element, const QByteArray& vr, quint32 value)
    {
        QByteArray raw;
        appendLe32(raw, value);
        return makeElement(group, element, vr, raw);
    }

    QByteArray makeTextElement(quint16 group, quint16 element, const QByteArray& vr, const QString& value)
    {
        return makeElement(group, element, vr, value.toUtf8());
    }

    QByteArray makeItem(const QByteArray& payload)
    {
        QByteArray out;
        appendLe16(out, 0xFFFE);
        appendLe16(out, 0xE000);
        appendLe32(out, quint32(payload.size()));
        out.append(payload);
        return out;
    }

    QString makeGeneratedUid()
    {
        const QString uuidDigits = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        QString decimal;
        decimal.reserve(uuidDigits.size() * 2);
        for (const QChar ch : uuidDigits)
            decimal += QString::number(ch.unicode());

        return QStringLiteral("1.2.826.0.1.3680043.10.54321.%1.%2")
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(decimal.left(40));
    }

    QByteArray buildMetaInformation(const QString& sopInstanceUid)
    {
        QByteArray meta;
        meta.append(makeElement(0x0002, 0x0001, "OB", QByteArray::fromHex("0001")));
        meta.append(makeTextElement(0x0002, 0x0002, "UI", QStringLiteral("1.2.840.10008.1.3.10")));
        meta.append(makeTextElement(0x0002, 0x0003, "UI", sopInstanceUid));
        meta.append(makeTextElement(0x0002, 0x0010, "UI", QStringLiteral("1.2.840.10008.1.2.1")));
        meta.append(makeTextElement(0x0002, 0x0012, "UI", QStringLiteral("1.2.826.0.1.3680043.10.54321.1")));
        meta.append(makeTextElement(0x0002, 0x0013, "SH", QStringLiteral("ASTROTOMO_1")));

        QByteArray out;
        out.append(makeElementU32(0x0002, 0x0000, "UL", quint32(meta.size())));
        out.append(meta);
        return out;
    }

    QByteArray buildDirectoryRecordPayload(const DicomDirRecord& record, quint32 nextOffset, quint32 lowerOffset)
    {
        QByteArray payload;
        payload.append(makeElementU32(0x0004, 0x1400, "UL", nextOffset));
        payload.append(makeElementU16(0x0004, 0x1410, "US", 0xFFFF));
        payload.append(makeElementU32(0x0004, 0x1420, "UL", lowerOffset));

        for (auto it = record.elements.cbegin(); it != record.elements.cend(); ++it)
            payload.append(it.value());

        return payload;
    }

    QByteArray buildDicomDirPrefix(quint32 firstRootOffset, quint32 lastRootOffset)
    {
        QByteArray out;
        out.append(makeTextElement(0x0004, 0x1130, "CS", QStringLiteral("ASTROCT")));
        out.append(makeElementU32(0x0004, 0x1141, "UL", firstRootOffset));
        out.append(makeElementU32(0x0004, 0x1142, "UL", lastRootOffset));
        out.append(makeElementU16(0x0004, 0x1200, "US", 0));
        return out;
    }

    QVector<DicomDirRecord> buildDicomDirRecords(const QVector<ExportedDicomFile>& files)
    {
        QVector<DicomDirRecord> records;
        QHash<QString, int> patientRecordByKey;
        QHash<QString, int> studyRecordByKey;
        QHash<QString, int> seriesRecordByKey;

        auto attachChild = [&records](int parentIndex, int childIndex)
            {
                if (parentIndex >= 0 && parentIndex < records.size())
                    records[parentIndex].childIndices.push_back(childIndex);
            };

        for (const auto& file : files)
        {
            const QString patientKey = file.patientId + "|" + file.patientName;
            int patientIndex = patientRecordByKey.value(patientKey, -1);
            if (patientIndex < 0)
            {
                DicomDirRecord patient;
                patient.type = DicomDirRecord::Type::Patient;
                patient.elements.insert(tagKey(0x0004, 0x1430), makeTextElement(0x0004, 0x1430, "CS", QStringLiteral("PATIENT")));
                if (!file.patientName.isEmpty()) patient.elements.insert(tagKey(0x0010, 0x0010), makeTextElement(0x0010, 0x0010, "PN", file.patientName));
                if (!file.patientId.isEmpty()) patient.elements.insert(tagKey(0x0010, 0x0020), makeTextElement(0x0010, 0x0020, "LO", file.patientId));
                patientIndex = records.size();
                records.push_back(patient);
                patientRecordByKey.insert(patientKey, patientIndex);
            }

            const QString studyKey = patientKey + "|" + file.studyInstanceUid;
            int studyIndex = studyRecordByKey.value(studyKey, -1);
            if (studyIndex < 0)
            {
                DicomDirRecord study;
                study.type = DicomDirRecord::Type::Study;
                study.parentIndex = patientIndex;
                study.elements.insert(tagKey(0x0004, 0x1430), makeTextElement(0x0004, 0x1430, "CS", QStringLiteral("STUDY")));
                if (!file.studyDate.isEmpty()) study.elements.insert(tagKey(0x0008, 0x0020), makeTextElement(0x0008, 0x0020, "DA", file.studyDate));
                if (!file.studyTime.isEmpty()) study.elements.insert(tagKey(0x0008, 0x0030), makeTextElement(0x0008, 0x0030, "TM", file.studyTime));
                if (!file.accessionNumber.isEmpty()) study.elements.insert(tagKey(0x0008, 0x0050), makeTextElement(0x0008, 0x0050, "SH", file.accessionNumber));
                if (!file.studyDescription.isEmpty()) study.elements.insert(tagKey(0x0008, 0x1030), makeTextElement(0x0008, 0x1030, "LO", file.studyDescription));
                if (!file.studyInstanceUid.isEmpty()) study.elements.insert(tagKey(0x0020, 0x000D), makeTextElement(0x0020, 0x000D, "UI", file.studyInstanceUid));
                if (!file.studyId.isEmpty()) study.elements.insert(tagKey(0x0020, 0x0010), makeTextElement(0x0020, 0x0010, "SH", file.studyId));
                studyIndex = records.size();
                records.push_back(study);
                studyRecordByKey.insert(studyKey, studyIndex);
                attachChild(patientIndex, studyIndex);
            }

            const QString seriesKey = studyKey + "|" + file.seriesInstanceUid;
            int seriesIndex = seriesRecordByKey.value(seriesKey, -1);
            if (seriesIndex < 0)
            {
                DicomDirRecord series;
                series.type = DicomDirRecord::Type::Series;
                series.parentIndex = studyIndex;
                series.elements.insert(tagKey(0x0004, 0x1430), makeTextElement(0x0004, 0x1430, "CS", QStringLiteral("SERIES")));
                if (!file.modality.isEmpty()) series.elements.insert(tagKey(0x0008, 0x0060), makeTextElement(0x0008, 0x0060, "CS", file.modality));
                if (!file.seriesDescription.isEmpty()) series.elements.insert(tagKey(0x0008, 0x103E), makeTextElement(0x0008, 0x103E, "LO", file.seriesDescription));
                if (!file.seriesInstanceUid.isEmpty()) series.elements.insert(tagKey(0x0020, 0x000E), makeTextElement(0x0020, 0x000E, "UI", file.seriesInstanceUid));
                if (!file.seriesNumber.isEmpty()) series.elements.insert(tagKey(0x0020, 0x0011), makeTextElement(0x0020, 0x0011, "IS", file.seriesNumber));
                seriesIndex = records.size();
                records.push_back(series);
                seriesRecordByKey.insert(seriesKey, seriesIndex);
                attachChild(studyIndex, seriesIndex);
            }

            DicomDirRecord image;
            image.type = DicomDirRecord::Type::Image;
            image.parentIndex = seriesIndex;
            image.elements.insert(tagKey(0x0004, 0x1430), makeTextElement(0x0004, 0x1430, "CS", QStringLiteral("IMAGE")));
            image.elements.insert(tagKey(0x0004, 0x1500), makeTextElement(0x0004, 0x1500, "CS", QString(file.relativePath).replace('/', '\\')));
            if (!file.sopClassUid.isEmpty()) image.elements.insert(tagKey(0x0004, 0x1510), makeTextElement(0x0004, 0x1510, "UI", file.sopClassUid));
            if (!file.sopInstanceUid.isEmpty()) image.elements.insert(tagKey(0x0004, 0x1511), makeTextElement(0x0004, 0x1511, "UI", file.sopInstanceUid));
            if (!file.transferSyntaxUid.isEmpty()) image.elements.insert(tagKey(0x0004, 0x1512), makeTextElement(0x0004, 0x1512, "UI", file.transferSyntaxUid));
            if (!file.instanceNumber.isEmpty()) image.elements.insert(tagKey(0x0020, 0x0013), makeTextElement(0x0020, 0x0013, "IS", file.instanceNumber));
            const int imageIndex = records.size();
            records.push_back(image);
            attachChild(seriesIndex, imageIndex);
        }

        for (auto& record : records)
            record.itemSize = makeItem(buildDirectoryRecordPayload(record, 0, 0)).size();

        return records;
    }

    bool writeDicomDirFile(const QString& filePath, const QVector<ExportedDicomFile>& files)
    {
        if (files.isEmpty())
            return false;

        QVector<DicomDirRecord> records = buildDicomDirRecords(files);
        if (records.isEmpty())
            return false;

        const QByteArray meta = buildMetaInformation(makeGeneratedUid());
        const QByteArray prefixTemplate = buildDicomDirPrefix(0, 0);
        const int sequenceHeaderSize = makeElement(0x0004, 0x1220, "SQ", QByteArray()).size();

        quint32 runningOffset = quint32(meta.size() + prefixTemplate.size() + sequenceHeaderSize);
        for (auto& record : records)
        {
            record.startOffset = runningOffset;
            runningOffset += quint32(record.itemSize);
        }

        auto nextSiblingOffset = [&records](int recordIndex) -> quint32
            {
                if (recordIndex < 0 || recordIndex >= records.size())
                    return 0;

                const int parentIndex = records[recordIndex].parentIndex;
                if (parentIndex < 0)
                {
                    for (int i = recordIndex + 1; i < records.size(); ++i)
                    {
                        if (records[i].parentIndex < 0)
                            return records[i].startOffset;
                    }

                    return 0;
                }

                const QVector<int>& siblings = records[parentIndex].childIndices;
                const int pos = siblings.indexOf(recordIndex);
                if (pos >= 0 && pos + 1 < siblings.size())
                    return records[siblings[pos + 1]].startOffset;

                return 0;
            };

        quint32 firstRootOffset = 0;
        quint32 lastRootOffset = 0;
        for (const auto& record : records)
        {
            if (record.parentIndex < 0)
            {
                if (firstRootOffset == 0)
                    firstRootOffset = record.startOffset;
                lastRootOffset = record.startOffset;
            }
        }

        QByteArray sequenceItems;
        for (int i = 0; i < records.size(); ++i)
        {
            const quint32 nextOffset = nextSiblingOffset(i);
            const quint32 lowerOffset = records[i].childIndices.isEmpty() ? 0 : records[records[i].childIndices.first()].startOffset;
            sequenceItems.append(makeItem(buildDirectoryRecordPayload(records[i], nextOffset, lowerOffset)));
        }

        const QByteArray datasetPrefix = buildDicomDirPrefix(firstRootOffset, lastRootOffset);
        QByteArray dataset = datasetPrefix;
        dataset.append(makeElement(0x0004, 0x1220, "SQ", sequenceItems));

        QSaveFile out(filePath);
        if (!out.open(QIODevice::WriteOnly))
            return false;

        out.write(QByteArray(128, '\0'));
        out.write("DICM", 4);
        out.write(meta);
        out.write(dataset);
        return out.commit();
    }
    QString normalizedDirPath(const QString& path)
    {
        if (path.trimmed().isEmpty())
            return {};

        return QDir(path).absolutePath();
    }

    QString patientCacheFilePath()
    {
        return QCoreApplication::applicationDirPath() + "/hdbase_patients_cache.json";
    }

    QJsonObject loadPatientCacheRoot()
    {
        QFile file(patientCacheFilePath());
        if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};

        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        return doc.isObject() ? doc.object() : QJsonObject{};
    }

    bool savePatientCacheRoot(const QJsonObject& root)
    {
        QSaveFile file(patientCacheFilePath());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;

        const QJsonDocument doc(root);
        file.write(doc.toJson(QJsonDocument::Indented));
        return file.commit();
    }

    QJsonArray loadCachedPatientsArray(const QString& basePath)
    {
        const QJsonObject root = loadPatientCacheRoot();
        const QJsonObject bases = root.value("bases").toObject();
        return bases.value(normalizedDirPath(basePath)).toArray();
    }

    void saveCachedPatientsArray(const QString& basePath, const QJsonArray& patients)
    {
        const QString normalizedBasePath = normalizedDirPath(basePath);
        if (normalizedBasePath.isEmpty())
            return;

        QJsonObject root = loadPatientCacheRoot();
        root["version"] = 1;

        QJsonObject bases = root.value("bases").toObject();
        bases[normalizedBasePath] = patients;
        root["bases"] = bases;

        savePatientCacheRoot(root);
    }
    QString decodeCp866(const QByteArray& bytes)
    {
#ifdef Q_OS_WIN
        if (bytes.isEmpty())
            return {};

        const int wideSize = MultiByteToWideChar(866, 0, bytes.constData(), static_cast<int>(bytes.size()), nullptr, 0);
        if (wideSize <= 0)
            return QString::fromLocal8Bit(bytes);

        std::wstring wide(static_cast<size_t>(wideSize), L'\0');
        MultiByteToWideChar(866, 0, bytes.constData(), static_cast<int>(bytes.size()), wide.data(), wideSize);
        return QString::fromWCharArray(wide.c_str(), wideSize);
#else
        return QString::fromLocal8Bit(bytes);
#endif
    }

    int patientNameScore(const QString& candidate)
    {
        static const QRegularExpression initialsRe(QString::fromUtf8(R"((?:^|\s)([А-ЯЁ][а-яё]{2,}(?:-[А-ЯЁ][а-яё]{2,})?\s+[А-ЯЁ]\.[А-ЯЁ]\.)(?=\s|$))"));
        static const QRegularExpression fullNameRe(QString::fromUtf8(R"((?:^|\s)([А-ЯЁ][а-яё]{2,}(?:-[А-ЯЁ][а-яё]{2,})?(?:\s+[А-ЯЁ][а-яё]{2,}){1,2})(?=\s|$))"));

        const QString simplified = candidate.simplified();
        if (simplified.size() < 6)
            return -1;

        const auto initialsMatch = initialsRe.match(simplified);
        if (initialsMatch.hasMatch())
            return 200 + initialsMatch.captured(1).size();

        const auto fullMatch = fullNameRe.match(simplified);
        if (fullMatch.hasMatch())
        {
            const QString captured = fullMatch.captured(1);
            const int wordCount = captured.split(' ', Qt::SkipEmptyParts).size();
            if (wordCount >= 2)
                return 100 + wordCount * 10 + captured.size();
        }

        return -1;
    }

    QString extractPatientNameFromStatusInf(const QString& statusPath)
    {
        QFile file(statusPath);
        if (!file.open(QIODevice::ReadOnly))
            return {};

        const QString decoded = decodeCp866(file.readAll());
        if (decoded.isEmpty())
            return {};

        QString currentChunk;
        QString bestCandidate;
        int bestScore = -1;

        const auto flushChunk = [&]()
            {
                const QString simplified = currentChunk.simplified();
                const int score = patientNameScore(simplified);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestCandidate = simplified;
                }
                currentChunk.clear();
            };

        for (const QChar ch : decoded)
        {
            const bool isAllowed = ch.isLetter() || ch == u' ' || ch == u'.' || ch == u'-';
            if (isAllowed)
            {
                currentChunk += ch;
            }
            else if (!currentChunk.isEmpty())
            {
                flushChunk();
            }
        }

        if (!currentChunk.isEmpty())
            flushChunk();

        return bestCandidate;
    }

    QVector<PatientFolderEntry> loadHdBasePatients(const QString& basePath, bool forceRefresh = false)
    {
        QVector<PatientFolderEntry> patients;
        QDir hdBase(basePath);
        if (!hdBase.exists())
            return patients;

        static const QRegularExpression folderRe(QStringLiteral(R"(^H\d{7}\.\d{3}$)"), QRegularExpression::CaseInsensitiveOption);
        const QFileInfoList entries = hdBase.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        const QJsonArray cachedEntries = forceRefresh ? QJsonArray{} : loadCachedPatientsArray(basePath);

        QHash<QString, PatientFolderEntry> cachedByFolderName;
        for (const QJsonValue& value : cachedEntries)
        {
            const QJsonObject obj = value.toObject();
            const QString folderName = obj.value("folderName").toString().trimmed();
            if (folderName.isEmpty())
                continue;

            PatientFolderEntry patient;
            patient.folderName = folderName;
            patient.folderPath = hdBase.filePath(folderName);
            patient.patientName = obj.value("patientName").toString().trimmed();
            patient.hasCt = obj.value("hasCt").toBool(false);
            cachedByFolderName.insert(folderName, patient);
        }

        QJsonArray serializedPatients;

        for (const QFileInfo& entry : entries)
        {
            if (!folderRe.match(entry.fileName()).hasMatch())
                continue;

            PatientFolderEntry patient = cachedByFolderName.value(entry.fileName());
            patient.folderName = entry.fileName();
            patient.folderPath = entry.absoluteFilePath();
            patient.hasCt = QDir(entry.absoluteFilePath()).exists("CT");

            if (forceRefresh || !cachedByFolderName.contains(entry.fileName()))
            {
                patient.patientName = extractPatientNameFromStatusInf(entry.absoluteFilePath() + "/status.inf");
            }

            patients.push_back(patient);

            QJsonObject serializedPatient;
            serializedPatient["folderName"] = patient.folderName;
            serializedPatient["patientName"] = patient.patientName;
            serializedPatient["hasCt"] = patient.hasCt;
            serializedPatients.push_back(serializedPatient);
        }
        saveCachedPatientsArray(basePath, serializedPatients);
        return patients;
    }
    void updateCachedPatientCtState(const QString& basePath, const QString& patientFolderPath, bool hasCt)
    {
        const QString normalizedBasePath = normalizedDirPath(basePath);
        const QString normalizedPatientFolderPath = normalizedDirPath(patientFolderPath);
        if (normalizedBasePath.isEmpty() || normalizedPatientFolderPath.isEmpty())
            return;

        QJsonArray patients = loadCachedPatientsArray(normalizedBasePath);
        bool found = false;

        for (int index = 0; index < patients.size(); ++index)
        {
            QJsonObject patient = patients.at(index).toObject();
            const QString folderName = patient.value("folderName").toString().trimmed();
            if (folderName.isEmpty())
                continue;

            if (normalizedDirPath(QDir(normalizedBasePath).filePath(folderName)) != normalizedPatientFolderPath)
                continue;

            patient["hasCt"] = hasCt;
            patients[index] = patient;
            found = true;
            break;
        }

        if (!found)
            return;

        saveCachedPatientsArray(normalizedBasePath, patients);
    }
}

MainWindow::MainWindow(QWidget* parent,
    const QString& path,
    ExplorerDialog::SelectionKind kind)
    : QMainWindow(parent)
    , mDicomPath(path)
    , mKind(kind)
{
    setWindowTitle(tr("Astrocard DICOM Editor"));
    setMinimumSize(1069, 640);
    resize(1280, 800);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);

    setWindowIcon(QIcon(":/icons/Resources/dicom_heart.ico"));

#ifdef Q_OS_WIN
    if (auto* mb = menuBar()) mb->setNativeMenuBar(false);
#endif

    //qApp->installEventFilter(this);

    buildUi();
    mUiToDisable = mSplit;
    buildStyles();
    wireSignals();

    if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }
}

void MainWindow::buildUi()
{
    auto* frame = new QWidget(this);
    frame->setObjectName("WindowFrame");

    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(8, 8, 8, 8); // отступ от краёв окна до рамки
    outer->setSpacing(0);

    // внутренняя «карточка» со скруглёнными углами и рамкой
    auto* central = new QWidget(frame);
    central->setObjectName("CentralCard");

    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // title bar (наш кастомный)
    mTitle = new TitleBar(this);
    mTitle->setObjectName("TitleBar");          // важно для стилей
    v->addWidget(mTitle, 0);

    if (mTitle) { mTitle->set2DChecked(false); mTitle->set3DChecked(false); }

    // центральный сплиттер
    mSplit = new QSplitter(Qt::Horizontal, central);
    mSplit->setObjectName("MainSplit");
    mSplit->setHandleWidth(8);        // было 10
    mSplit->setChildrenCollapsible(false);

    // левая панель
    mSeries = new SeriesListPanel(mSplit);
    mSeries->setObjectName("SeriesPanel");
    mSeries->setMinimumWidth(100);
    mSeries->setMaximumWidth(300);
    mSeries->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    // правая область — стек просмотров
    mViewerStack = new QStackedWidget(mSplit);
    mViewerStack->setObjectName("ViewerStack");
    mViewerStack->setAutoFillBackground(false);
    mViewerStack->setAttribute(Qt::WA_TranslucentBackground);
    mViewerStack->setStyleSheet("background: transparent;");
    mViewerStack->setFrameStyle(QFrame::NoFrame);

    auto* placeholder = new QLabel(mViewerStack);
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet("background: transparent; color: rgba(255,255,255,0.55);");

    mPlanar = new PlanarView(mViewerStack);
    mPlanar->setObjectName("PlanarView");
    mViewerStack->addWidget(mPlanar);

    // вид для 3D
    mRenderView = new RenderView(mViewerStack);
    mRenderView->setObjectName("RenderView");
    mViewerStack->addWidget(mRenderView);

    // по умолчанию показываем 2D
    mViewerStack->setCurrentWidget(mPlanar);

    mSplit->setStretchFactor(0, 0);
    mSplit->setStretchFactor(1, 1);

    v->addWidget(mSplit, 1);

    auto* mFooterSep = new QFrame(central);
    mFooterSep->setObjectName("FooterSep");
    mFooterSep->setFrameShape(QFrame::HLine);
    mFooterSep->setFrameShadow(QFrame::Plain);
    mFooterSep->setFixedHeight(1);
    v->addWidget(mFooterSep);

    // сам внутренний статус-бар (прозрачный)
    mFooter = new QWidget(central);
    mFooter->setObjectName("InnerStatusBar");
    mFooter->setFixedHeight(28);
    auto* fb = new QHBoxLayout(mFooter);
    fb->setContentsMargins(20, 4, 20, 4);
    fb->setSpacing(8);

    // прогресс (справа от текста)
    mProgBox = new QWidget(mFooter);
    auto* pbLay = new QHBoxLayout(mProgBox);
    pbLay->setContentsMargins(0, 0, 0, 0);
    constexpr int kProgWidth = 180;

    mProgress = new AsyncProgressBar(mProgBox);
    mProgress->setFixedHeight(4);
    mProgress->setFixedWidth(kProgWidth);
    mProgress->hideBar();              // внутреннее состояние Hidden
    mProgress->setVisible(false);      // по умолчанию не показываем

    // немного подправим палитру, чтобы полоска была светлой
    {
        QPalette pal = mProgress->palette();
        pal.setColor(QPalette::Window, QColor(0, 0, 0, 0));
        pal.setColor(QPalette::Base, QColor(0, 0, 0, 0));
        pal.setColor(QPalette::Highlight, QColor(230, 230, 230, 190));
        mProgress->setPalette(pal);
    }

    pbLay->addWidget(mProgress);
    
    // чтобы место сохранялось даже при hide()
    auto pol = mProgBox->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mProgBox->setSizePolicy(pol);
    mProgBox->setFixedWidth(kProgWidth);
    mProgBox->setVisible(false);

    auto* leftMirror = new QWidget(mFooter);
    leftMirror->setFixedWidth(kProgWidth);
    leftMirror->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    // --- ЦЕНТР: текст в расширяющемся контейнере ---
    auto* centerWrap = new QWidget(mFooter);
    auto* cLay = new QHBoxLayout(centerWrap);
    cLay->setContentsMargins(0, 0, 0, 0);

    mStatusText = new QLabel(tr("Ready"), centerWrap);
    mStatusText->setAlignment(Qt::AlignCenter);
    mStatusText->setObjectName("StatusText");
    mStatusText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    cLay->addWidget(mStatusText);

    // --- Сборка футера: [лев.зеркало][центр][правый прогресс]
    fb->addWidget(leftMirror, 0);
    fb->addWidget(centerWrap, 1);
    fb->addWidget(mProgBox, 0, Qt::AlignRight);

    v->addWidget(mFooter, 0);

    mCornerGrip = new CornerGrip(mFooter);
    mCornerGrip->raise();

    outer->addWidget(central);
    mOuter = outer;
    mCentralCard = central;

    setCentralWidget(frame);

    auto* sb = new QStatusBar(this);
    sb->setSizeGripEnabled(false);
    sb->setFixedHeight(0);
    sb->setContentsMargins(0, 0, 0, 0);
    sb->setStyleSheet("QStatusBar{ background:transparent; border:0; margin:0; padding:0; }");
    setStatusBar(sb);
    sb->hide(); 

    if (!mPatientDlg)
        mPatientDlg = new PatientDialog(this);
    mPatientDlg->hide();

    if (!mSettingsDlg)
        mSettingsDlg = new SettingsDialog(this, true);
    mSettingsDlg->hide();

    if (!mDicomSeriesSaveDlg)
        mDicomSeriesSaveDlg = new DicomSeriesSaveDialog(this);
    mDicomSeriesSaveDlg->hide();

    connect(mDicomSeriesSaveDlg, &DicomSeriesSaveDialog::saveRequested, this,
        [this](const QVector<SeriesExportEntry>& selected)
        {
            if (selected.isEmpty())
                return;

            QSettings settings;
            const QString defDir = settings.value(
                "Paths/LastDicomExportDir",
                hdBasePath().isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                : hdBasePath()
            ).toString();

            ShellFileDialog shell(this,
                tr("Select folder to save DICOM"),
                ServiceWindow,
                defDir,
                tr("Folder"));

            auto* dlg = shell.fileDialog();
            dlg->setAcceptMode(QFileDialog::AcceptOpen);
            dlg->setFileMode(QFileDialog::Directory);
            dlg->setOption(QFileDialog::ShowDirsOnly, true);
            dlg->setFilter(QDir::AllDirs | QDir::Drives | QDir::NoDotAndDotDot);

            if (shell.exec() != QDialog::Accepted)
                return;

            const QString selectedDir = dlg->selectedFiles().isEmpty() ? QString() : dlg->selectedFiles().first();
            if (selectedDir.isEmpty())
                return;

            settings.setValue("Paths/LastDicomExportDir", selectedDir);

            if (copySelectedDicomSeries(selectedDir, selected))
            {
                CustomMessageBox::information(this, tr("Dicom Save"),
                    tr("Selected series were saved successfully."), ServiceWindow);
                mDicomSeriesSaveDlg->hide();
            }
        });

    if (mTitle)
    {
        mTitle->setSaveDicomVisible(false);
        mTitle->setSettingsVisible(false);
    }

    connect(mDicomSeriesSaveDlg, &DicomSeriesSaveDialog::saveToPatientRequested, this,
        [this](const QString& patientFolder, const QVector<SeriesExportEntry>& selected)
        {
            if (patientFolder.isEmpty() || selected.isEmpty())
                return;

            if (copySelectedDicomSeries(patientFolder, selected, true))
            {
                updateCachedPatientCtState(hdBasePath(), patientFolder, true);
                CustomMessageBox::information(this, tr("Dicom Save"),
                    tr("Selected series were saved successfully."), ServiceWindow);
                mDicomSeriesSaveDlg->hide();
            }
        });

    connect(mDicomSeriesSaveDlg, &DicomSeriesSaveDialog::refreshPatientsRequested, this,
        [this]()
        {
            const QString basePath = hdBasePath();
            if (basePath.isEmpty())
            {
                CustomMessageBox::warning(this, tr("Dicom Save"),
                    tr("HDBASE path is not configured."), ServiceWindow);
                return;
            }

            const QVector<PatientFolderEntry> patients = loadHdBasePatients(basePath, true);
            mDicomSeriesSaveDlg->setPatients(patients);
            mDicomSeriesSaveDlg->setSaveToPatientEnabled(true);

            CustomMessageBox::information(this, tr("Dicom Save"),
                tr("Patient list was rebuilt. Found %1 patient folders.").arg(patients.size()), ServiceWindow);
        });

    connect(mSettingsDlg, &SettingsDialog::languageChanged, this, [](const QString& code)
        {
            LanguageManager::instance().setLanguage(code);
        });


    connect(mSettingsDlg, &SettingsDialog::volumeInterpolationChanged,
        this, [this](int mode)
        {
            if (!mRenderView) return;

            mRenderView->setVolumeInterpolation(
                mode == 1 ? RenderView::VolumeInterpolation::Linear
                : RenderView::VolumeInterpolation::Nearest);
        });

    connect(mSettingsDlg, &SettingsDialog::samplingFactorChanged,
        this, [this](double f)
        {
            if (!mRenderView) return;
            mRenderView->setSamplingFactor(f);
        });

    retranslateUi(true);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
        this, [this] { retranslateUi(false); });
}

void MainWindow::retranslateUi(bool loading)
{
    setWindowTitle(tr("Astrocard DICOM Editor"));

    if (mStatusText)
        mStatusText->setText(tr("Ready"));

    if (mTitle)
        mTitle->retranslateUi();   // добавим ниже


    if (mTitle)
    {
        if (!loading)
        mTitle->setPatientInfo(mCurrentPatient);
    }

    if (mPatientDlg)
        mPatientDlg->retranslateUi();

    if (mDicomSeriesSaveDlg)
        mDicomSeriesSaveDlg->retranslateUi();

    if (mPlanar)
        mPlanar->retranslateUi();

    if (mSeries)
        mSeries->retranslateUi();

    if(mSettingsDlg)
        mSettingsDlg->retranslateUi();
}


void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    applyMaximizedUi(isWindowExpanded());
    positionCornerGrip();      // на случай показа после построения
}

void MainWindow::buildStyles()
{
    QString ss;

    // Карточка рисует ВЕСЬ внешний контур
    ss += "#CentralCard {"
        "  background:#1f2023;"
        "  border:1px solid rgba(255,255,255,0.14);"
        "  border-radius:10px;"
        "}\n";


    // Прозрачный внутренний статус-бар — никаких рамок и углов
    ss += "#InnerStatusBar {"
        "  background: transparent;"
        "  border: none;"
        "}\n";

    // Тонкая разделительная линия над футером
    ss += "#FooterSep {"
        "  background: rgba(255,255,255,0.10);"
        "  border: none;"
        "  margin: 0;"
        "}\n";

    //ss += "#ViewerStack, #ViewerStack * { background: transparent; } \n";
    ss += "#ViewerStack {"
        "  background: rgba(255,255,255,0.10);"
        "  margin: 10px;"
        "}\n";

    // Заголовок как был
    ss += "#TitleBar {"
        "  background:#1e1f22;"
        "  border-bottom:1px solid rgba(255,255,255,0.12);"
        "  border-top-left-radius:10px;"
        "  border-top-right-radius:10px;"
        "}\n";

    ss += R"(
        /* Общие настройки для элементов */
        #SeriesPanel QListWidget {
            background: rgba(255,255,255,0.02);
            border-radius: 8px;
            outline: none;
            border: none;
        }
        
        #SeriesPanel QListWidget::item {
            background: rgba(255,255,255,0.03);
            border-radius: 8px;
            margin: 2px 2px;
            padding: 6px;
            color: #ddd;
        }
        
        /* Когда курсор над элементом */
        #SeriesPanel QListWidget::item:hover {
            background: rgba(255,255,255,0.10);
            border: 1px solid rgba(255,255,255,0.14);
        }
        
        /* Активный / выделенный элемент */
        #SeriesPanel QListWidget::item:selected {
            background: rgba(255,255,255,0.15);
            border: 2px solid rgba(255,255,255,0.25);
        }
        
        /* Миниатюры в элементах */
        #SeriesPanel QListWidget::icon {
            margin: 4px;
        }
        
        /* Текст */
        #SeriesPanel QListWidget::item:selected:!active {
            color: white;
        }
        
        /* Вертикальный скролл компактный */
        #SeriesPanel QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 4px 0 4px 0;
        }
        
        #SeriesPanel QScrollBar::handle:vertical {
            background: rgba(255,255,255,0.18);
            min-height: 24px;
            border-radius: 4px;
        }
        
        #SeriesPanel QScrollBar::handle:vertical:hover {
            background: rgba(255,255,255,0.32);
        }
        
        #SeriesPanel QScrollBar::add-line:vertical,
        #SeriesPanel QScrollBar::sub-line:vertical {
            height: 0;
        }
        )";


    ss += R"(
            
                /* Аккуратный хэндл сплиттера — тонкая полоска по центру */
                #MainSplit::handle:horizontal {
                    background: qlineargradient(
                        x1:0, y1:0, x2:1, y2:0,
                        stop:0   rgba(0,0,0,0),
                        stop:0.46 rgba(255,255,255,0.09),
                        stop:0.54 rgba(255,255,255,0.09),
                        stop:1   rgba(0,0,0,0)
                    );
                    width: 8px;
                }
            
                #MainSplit::handle:horizontal:hover {
                    background: qlineargradient(
                        x1:0, y1:0, x2:1, y2:0,
                        stop:0   rgba(0,0,0,0),
                        stop:0.46 rgba(255,255,255,0.18),
                        stop:0.54 rgba(255,255,255,0.18),
                        stop:1   rgba(0,0,0,0)
                    );
                }
            
                #MainSplit::handle:horizontal:pressed {
                    background: qlineargradient(
                        x1:0, y1:0, x2:1, y2:0,
                        stop:0   rgba(0,0,0,0),
                        stop:0.46 rgba(255,255,255,0.26),
                        stop:0.54 rgba(255,255,255,0.26),
                        stop:1   rgba(0,0,0,0)
                    );
                }
            )";

    ss += "QLabel#StatusText { color: rgba(255,255,255,0.95); }\n";

    // Если окно развёрнуто — без радиусов
    ss += "#CentralCard[maxed=\"true\"] { border-radius:0; }"
        "#TitleBar[maxed=\"true\"] { border-top-left-radius:0; border-top-right-radius:0; }\n";

    qApp->setStyleSheet(ss);
}

void MainWindow::wireSignals()
{
    // TitleBar
    connect(mTitle, &TitleBar::patientClicked,
        this, &MainWindow::showPatientDetails);

    connect(mTitle, &TitleBar::settingsClicked,
        this, &MainWindow::showSettings);

    // Series panel → header / viewer / scan progress
    connect(mSeries, &SeriesListPanel::patientInfoChanged,
        this, &MainWindow::onSeriesPatientInfoChanged);

    // при активации серии: переключить вид и обновить статус
    connect(mSeries, &SeriesListPanel::seriesActivated,
        this, &MainWindow::onSeriesActivated);

    // напрямую прокинем файлы в PlanarView (если у него есть соответствующий слот — оставим через лямбду)
    connect(mSeries, &SeriesListPanel::seriesActivated,
        this, [this](const QString&, const QVector<QString>& files) 
        {
            if (mPlanar && !files.isEmpty())
                if (!mPlanar->IsLoading())
                {
                    StartLoading();
                    mPlanar->StartLoading();
                    mPlanar->loadSeriesFiles(files);
                }
        });

    // прогресс сканирования (левая панель)
    connect(mRenderView, &RenderView::renderStarted, this,
        [this]() {
            mStatusText->setText(tr("Render 0%"));
            StartLoading();
            mProgBox->setVisible(true);
            if (mProgress) {
                mProgress->setVisible(true);
                mProgress->startFill();                     // детерминированный режим
                mProgress->setRange(0, 100);
                mProgress->setValue(0);
            }
        });

    connect(mRenderView, &RenderView::renderProgress, this,
        [this](int processed) {
            mStatusText->setText(tr("Render progress: %1").arg(processed));
            if (mProgress) {
                mProgress->setRange(0, std::max(1, 100));
                mProgress->setValue(std::clamp(processed, 0, 100));
            }
        });

    connect(mRenderView, &RenderView::Progress, this,
        [this](int processed) {
            if (processed == 0)
            {
                StartLoading();
                mProgBox->setVisible(true);
                if (mProgress) 
                {
                    mProgress->setVisible(true);
                    mProgress->startLoading();
                    mProgress->setRange(0, 100);
                    mProgress->setValue(0);
                }
            }
            else if (processed < 100)
            {
                if (mProgress)
                {
                    mProgress->setRange(0, 100);
                    mProgress->setValue(processed);
                }
            }
            else
            {
                if (mProgress) 
                {
                    mProgress->setValue(mProgress->maximum());
                    mProgress->hideBar();
                    mProgress->setVisible(false);
                }
                StopLoading();
            }
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        });

    connect(mRenderView, &RenderView::renderFinished, this,
        [this]() {
            mStatusText->setText(tr("Render success"));
            if (mProgress) {
                mProgress->setValue(mProgress->maximum());
                mProgress->hideBar();
                mProgress->setVisible(false);
            }
            StopLoading();
            QTimer::singleShot(800, this, [this] {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });

    connect(mRenderView, &RenderView::showInfo,
        this, &MainWindow::showInfo);

    connect(mSeries, &SeriesListPanel::scanStarted, this,
        [this](int total) {
            mStatusText->setText(tr("DICOM files detection 0%"));
            StartLoading();
            mProgBox->setVisible(true);
            if (mProgress) {
                mProgress->setVisible(true);
                if (total > 0) {
                    mProgress->startFill();
                    mProgress->setRange(0, std::max(1, total));
                    mProgress->setValue(0);
                }
                else {
                    mProgress->startLoading();    // пока не знаем total
                }
            }
        });

    connect(mSeries, &SeriesListPanel::scanProgress, this,
        [this](int processed, int total, const QString& path) {

            if (!mProgress)
                return;

            if (total <= 0) {
                mStatusText->setText(tr("Searching… %1 files checked: %2")
                    .arg(processed)
                    .arg(QFileInfo(path).fileName()));
                mProgress->startLoading();         // чистый indeterminate
                return;
            }

            const int pct = (total > 0) ? (processed * 100 / total) : 0;
            mStatusText->setText(tr("Header reading: %1 (%2%)")
                .arg(QFileInfo(path).fileName())
                .arg(pct));

            mProgress->startFill();
            mProgress->setRange(0, std::max(1, total));
            mProgress->setValue(std::min(processed, total));
        });

    connect(mSeries, &SeriesListPanel::scanFinished, this,
        [this](int seriesCount, int /*total*/) {
            mStatusText->setText(tr("Ready. Series: %1").arg(seriesCount));
            if (mProgress) {
                mProgress->setValue(mProgress->maximum());
                mProgress->hideBar();
                mProgress->setVisible(false);
            }
            StopLoading();
            QTimer::singleShot(800, this, [this] {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });

    // прогресс загрузки в PlanarView (правый просмотр)
    connect(mPlanar, &PlanarView::loadStarted, this,
        [this](int total) {
            mViewerStack->setCurrentWidget(mPlanar);
            mStatusText->setText(tr("Series loading… 0/%1").arg(total));
            mProgBox->setVisible(true);
            if (mProgress) {
                mProgress->setVisible(true);
                mProgress->startFill();
                mProgress->setRange(0, std::max(1, total));
                mProgress->setValue(0);
            }
        });

    connect(mPlanar, &PlanarView::loadProgress, this,
        [this](int processed, int total) {
            mStatusText->setText(tr("Series loading… %1/%2").arg(processed).arg(total));
            if (mProgress) {
                mProgress->setRange(0, std::max(1, total));
                mProgress->setValue(std::min(processed, total));
            }
        });

    connect(mPlanar, &PlanarView::loadFinished, this,
        [this](int total) {
            mStatusText->setText(tr("Loaded: %1 slices").arg(total));
            if (mProgress) {
                mProgress->setValue(mProgress->maximum());
                mProgress->hideBar();
                mProgress->setVisible(false);
            }
            mPlanar->StopLoading();
            StopLoading();
            onShowPlanar2D();
            QTimer::singleShot(1200, this, [this] {
                mProgBox->setVisible(false);
                mStatusText->setText(tr("Ready"));
                });
        });

    connect(mSettingsDlg, &SettingsDialog::gradientOpacityChanged,
        mRenderView, &RenderView::setGradientOpacityEnabled);

    connect(mRenderView, &RenderView::gradientOpacityChanged,
        mSettingsDlg, &SettingsDialog::syncGradientOpacityUi);

    connect(mTitle, &TitleBar::volumeClicked, this, &MainWindow::onShowVolume3D);
    connect(mTitle, &TitleBar::planarClicked, this, &MainWindow::onShowPlanar2D);

    // Подписка на клик из заголовка
    connect(mTitle, &TitleBar::save3DRRequested, this, &MainWindow::onSave3DR);
    connect(mTitle, &TitleBar::saveDicomRequested, this, &MainWindow::onSaveDicom);

    // Горячая клавиша Ctrl+S
    auto* actSave = new QAction(tr("Save 3DR"), this);
    actSave->setShortcut(QKeySequence::Save);
    connect(actSave, &QAction::triggered, this, &MainWindow::onSave3DR);
    addAction(actSave);

    // Горячая клавиша Ctrl+D
    auto* actSave2 = new QAction(tr("Save Dicom"), this);
    actSave2->setShortcut(QKeySequence::Save);
    connect(actSave2, &QAction::triggered, this, &MainWindow::onSaveDicom);
    addAction(actSave2);

    auto sc2 = new QShortcut(QKeySequence(Qt::Key_2), this);
    connect(sc2, &QShortcut::activated, this, [this]
        {
            if (!mPlanar)
                return;

            if (mTitle)
                if (!mTitle->is2DVisible())
                    return;

            onShowPlanar2D();
        });

    auto sc3 = new QShortcut(QKeySequence(Qt::Key_3), this);
    connect(sc3, &QShortcut::activated, this, [this]    // ← sc3 здесь
        {
            if (!mPlanar)
                return;

            if (mTitle)
                if (!mTitle->is3DVisible())
                    return;

            onShowVolume3D();
        });
}

void MainWindow::onSave3DR()
{
    if (!mRenderView) return;

    auto vol = mRenderView->image();
    if (!vol) 
    {
        CustomMessageBox::warning(this, tr("Save 3DR"), tr("No volume to save"), ServiceWindow);
        return;
    }

    QString filename = "NULL";
    DicomInfo di = mRenderView->GetDicomInfo();
    if (Save3DR::saveWithDialog(this, mRenderView->image(), &di, filename))
        mRenderView->saveTemplates(filename);
}

void MainWindow::onSaveDicom()
{
    if (!mSeries)
        return;

    if (!mDicomSeriesSaveDlg)
        mDicomSeriesSaveDlg = new DicomSeriesSaveDialog(this);

    const auto series = mSeries->seriesForExport();
    if (series.isEmpty())
    {
        CustomMessageBox::warning(this, tr("Dicom Save"), tr("No series available for saving"), ServiceWindow);
        return;
    }

    const QString basePath = hdBasePath();
    const QVector<PatientFolderEntry> patients = basePath.isEmpty() ? QVector<PatientFolderEntry>{} : loadHdBasePatients(basePath);

    mDicomSeriesSaveDlg->setSeries(series);
    mDicomSeriesSaveDlg->setPatients(patients);
    mDicomSeriesSaveDlg->setSaveToPatientEnabled(!basePath.isEmpty());

    mDicomSeriesSaveDlg->show();
    mDicomSeriesSaveDlg->raise();
    mDicomSeriesSaveDlg->activateWindow();

    // Центрируем относительно главного окна
    const QRect r = geometry();
    const QSize s = mDicomSeriesSaveDlg->size();
    mDicomSeriesSaveDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}

QString MainWindow::hdBasePath() const
{
    return AppConfig::loadCurrent().hdBasePath.trimmed();
}

bool MainWindow::copySelectedDicomSeries(const QString& targetRoot, const QVector<SeriesExportEntry>& selected, bool replaceExistingCt)
{
    if (targetRoot.isEmpty() || selected.isEmpty())
        return false;

    QDir root(targetRoot);
    if (!root.exists() && !root.mkpath("."))
    {
        CustomMessageBox::critical(this, tr("Dicom Save"),
            tr("Failed to create destination folder."), ServiceWindow);
        return false;
    }

    const QString ctPath = root.filePath("CT");
    QDir ctDir(ctPath);
    if(replaceExistingCt && ctDir.exists() && !ctDir.removeRecursively())
    {
        CustomMessageBox::critical(this, tr("Dicom Save"),
            tr("Failed to replace existing CT folder."), ServiceWindow);
        return false;
    }

    if (!root.mkpath("CT"))
    {
        CustomMessageBox::critical(this, tr("Dicom Save"),
            tr("Failed to create CT folder."), ServiceWindow);
        return false;
    }

    QVector<ExportedDicomFile> exportedFiles;

    for (const auto& entry : selected)
    {
        const QString safeName = entry.description.simplified().isEmpty()
            ? entry.seriesKey
            : entry.description.simplified();

        QString folderName = safeName;
        folderName.replace('\\', '_');
        folderName.replace('/', '_');
        folderName.replace(':', '_');
        folderName.replace('*', '_');
        folderName.replace('?', '_');
        folderName.replace('"', '_');
        folderName.replace('<', '_');
        folderName.replace('>', '_');
        folderName.replace('|', '_');

        const QString seriesFolder = ctDir.filePath(folderName);
        if (!ctDir.mkpath(folderName))
        {
            CustomMessageBox::critical(this, tr("Dicom Save"),
                tr("Failed to create series folder: %1").arg(folderName), ServiceWindow);
            return false;
        }

        QSet<QString> usedNames;
        for (const auto& fp : entry.files)
        {
            QFileInfo fi(fp);
            QString fileName = fi.fileName();
            if (fileName.isEmpty())
                continue;

            if (usedNames.contains(fileName))
            {
                fileName = fi.completeBaseName() + "_" + QString::number(qHash(fp)) + "." + fi.suffix();
            }
            usedNames.insert(fileName);

            const QString dst = QDir(seriesFolder).filePath(fileName);
            QFile::remove(dst);
            if (!QFile::copy(fp, dst))
            {
                CustomMessageBox::critical(this, tr("Dicom Save"),
                    tr("Failed to copy file:\n%1").arg(fp), ServiceWindow);
                return false;
            }

            ExportedDicomFile exported;
            if (!readExportedDicomFileMeta(fp, QStringLiteral("%1/%2").arg(folderName, fileName), exported))
            {
                CustomMessageBox::critical(this, tr("Dicom Save"),
                    tr("Failed to read DICOM metadata for file:\n%1").arg(fp), ServiceWindow);
                return false;
            }

            exportedFiles.push_back(std::move(exported));
        }
    }

    const QString dstDicomDir = ctDir.filePath("DICOMDIR");
    QFile::remove(dstDicomDir);
    if (!writeDicomDirFile(dstDicomDir, exportedFiles))
    {
        CustomMessageBox::critical(this, tr("Dicom Save"),
            tr("Failed to create DICOMDIR for exported series."), ServiceWindow);
        return false;
    }

    return true;
}

void MainWindow::StartLoading()
{
    if (mLoading)
        return;

    mLoading = true;

    QObject* src = sender();
    std::optional<QSignalBlocker> blocker;
    if (src) blocker.emplace(src);

    if (mUiToDisable) mUiToDisable->setEnabled(false);
    
    QApplication::setOverrideCursor(Qt::BusyCursor);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::StopLoading()
{
    // Снять override-курсор(ы) со стека
    while (QApplication::overrideCursor())
        QApplication::restoreOverrideCursor();

    // Пока идёт загрузка, чуть подчистим очередь событий,
    // чтобы накопленные клики/скроллы не выстрелили сразу после разблокировки.
    QElapsedTimer flushTimer;
    flushTimer.start();

    // крутимся не дольше 50 мс, просто обрабатывая все события
    while (mLoading && flushTimer.elapsed() < 50)
    {
        qApp->processEvents(QEventLoop::AllEvents);
    }

    if (mUiToDisable)
        mUiToDisable->setEnabled(true);

    if (mTitle)
    {
        mTitle->setSettingsVisible(true);
        mTitle->setSaveDicomVisible(true);
    }

    mLoading = false;

    // Ещё раз обновим UI, но без пользовательского ввода
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}


void MainWindow::onOpenStudy()
{
    if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }

    mPlanar->sethidescroll();
    mProgBox->setVisible(true);
    if (mProgress) {
        mProgress->setVisible(true);
        mProgress->startLoading();  // чистый indeterminate
    }
    mStatusText->setText(tr("Searching DICOM files…"));
    StartLoading();

    qApp->processEvents(QEventLoop::AllEvents);
    QTimer::singleShot(0, this, &MainWindow::startScan);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (mLoading)
    {
        switch (event->type())
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::DragMove:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::ContextMenu:
            return true; // игнорируем пользовательский ввод пока идёт загрузка
        default:
            break;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}


void MainWindow::showInfo(const QString& text)
{
    if (mStatusText) mStatusText->setText(text);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::startScan()
{
    switch (mKind)
    {
    case ExplorerDialog::SelectionKind::DicomDir:
        mSeries->scanDicomDir(mDicomPath);
        break;
    case ExplorerDialog::SelectionKind::DicomFile:
        mSeries->scanSingleFile(mDicomPath);
        break;
    case ExplorerDialog::SelectionKind::File3DR:
        mSeries->scan3drFile(mDicomPath);
        break;
    case ExplorerDialog::SelectionKind::DicomFolder:
    default:
        mSeries->scanStudy(mDicomPath);
        break;
    }
    emit studyOpened(mDicomPath);
}

void MainWindow::changeEvent(QEvent* e)
{
    QMainWindow::changeEvent(e);

    if (e->type() == QEvent::LanguageChange)
    {
        retranslateUi(false);
        return;
    }

    if (e->type() == QEvent::WindowStateChange)
    {
        applyMaximizedUi(isWindowExpanded());
        positionCornerGrip();
        QTimer::singleShot(0, this, [this] {
            applyMaximizedUi(isWindowExpanded());
            positionCornerGrip();
            });
    }
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    applyMaximizedUi(isWindowExpanded());
    positionCornerGrip();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (mSeries)
        mSeries->stopBackgroundWork();

    QMainWindow::closeEvent(e);

    QCoreApplication::quit();
}

void MainWindow::positionCornerGrip()
{
    if (!mFooter || !mCornerGrip)
        return;

    const int insetX = 2;
    const int insetY = 2;
    const int x = mFooter->width() - mCornerGrip->width() - insetX;
    const int y = mFooter->height() - mCornerGrip->height() - insetY;
    mCornerGrip->move(x, y);
}

bool MainWindow::isWindowExpanded() const
{
    const auto st = windowState();
    if (st.testFlag(Qt::WindowMaximized) || st.testFlag(Qt::WindowFullScreen))
        return true;

    const QScreen* screen = nullptr;
    if (windowHandle())
        screen = windowHandle()->screen();
    if (!screen)
        screen = QApplication::primaryScreen();
    if (!screen)
        return false;

    const QRect available = screen->availableGeometry();
    const QRect current = frameGeometry();

    // Для frameless-окна snap/expand иногда не ставит WindowMaximized,
    // поэтому считаем «развёрнутым» окно, которое фактически заняло рабочую область экрана.
    constexpr int kTolerance = 1;
    return std::abs(current.left() - available.left()) <= kTolerance
        && std::abs(current.top() - available.top()) <= kTolerance
        && std::abs(current.right() - available.right()) <= kTolerance
        && std::abs(current.bottom() - available.bottom()) <= kTolerance;
}

void MainWindow::applyMaximizedUi(bool maxed)
{
    if (mOuter) mOuter->setContentsMargins(maxed ? 0 : 8, maxed ? 0 : 8,
        maxed ? 0 : 8, maxed ? 0 : 8);

    // Проставим property, чтобы стиль подхватил разные радиусы
    if (mCentralCard) { mCentralCard->setProperty("maxed", maxed); mCentralCard->style()->unpolish(mCentralCard); mCentralCard->style()->polish(mCentralCard); }
    if (mTitle) { mTitle->setProperty("maxed", maxed);       mTitle->style()->unpolish(mTitle);             mTitle->style()->polish(mTitle); }
    if (mCornerGrip) mCornerGrip->setVisible(!maxed);
}

void MainWindow::onExitRequested()
{
    close();
}

// ===== Reactions from panels ===============================================

void MainWindow::onSeriesPatientInfoChanged(const PatientInfo& info)
{
    mCurrentPatient = info;
    if (mTitle) mTitle->setPatientInfo(mCurrentPatient);
    if (mPatientDlg) mPatientDlg->setInfo(mCurrentPatient);
}

void MainWindow::onSeriesActivated(const QString& /*seriesUID*/, const QVector<QString>& files)
{
    StartLoading();

    if (!mPlanar || files.isEmpty())
        return;

    if (mPlanar->IsLoading())
        return;

    mPlanar->sethidescroll();

    if (mRenderView)
        mRenderView->hideOverlays();

    if (mTitle) { mTitle->set2DVisible(false); mTitle->set3DVisible(false); mTitle->set3DChecked(false); mTitle->set2DChecked(false); mTitle->setSaveVisible(false);}

    mViewerStack->setCurrentWidget(mPlanar);
    mStatusText->setText(tr("Series loading…"));
}

void MainWindow::onShowVolume3D()
{
    if (!mPlanar) return;

    auto vtkVol = mPlanar->makeVtkVolume();
    if (!vtkVol) {
        mStatusText->setText(tr("Warning"));
        return;
    }

    mCurrentPatient.DicomDirPath = mDicomPath;
    mRenderView->setVolume(vtkVol, mPlanar->GetDicomInfo(), mCurrentPatient);
    mViewerStack->setCurrentWidget(mRenderView);

    if (mTitle) { mTitle->set3DChecked(true); mTitle->set2DChecked(false); }
    mStatusText->setText(tr("Ready volume"));

    mTitle->setSaveVisible(true);
}

void MainWindow::onShowPlanar2D()
{
    if (mPlanar) 
    {
        mViewerStack->setCurrentWidget(mPlanar);

        if (mRenderView)
            mRenderView->hideOverlays();

        // <<< здесь обновляем состояние кнопок TitleBar
        if (mTitle) { mTitle->set2DChecked(true); mTitle->set3DChecked(false); }

        mTitle->setSaveVisible(false);

        mTitle->set2DVisible(true);
        if (mPlanar->IsAvalibleToReconstruct())
        mTitle->set3DVisible(true);
    }
}

void MainWindow::showPatientDetails()
{
    if (mCurrentPatient.patientName.isEmpty())
        return;

    if (!mPatientDlg)
        mPatientDlg = new PatientDialog(this);

    mPatientDlg->setInfo(mCurrentPatient);

    mPatientDlg->show();
    mPatientDlg->raise();
    mPatientDlg->activateWindow();

    // Центрируем относительно главного окна
    const QRect r = geometry();
    const QSize s = mPatientDlg->size();
    mPatientDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}

void MainWindow::showSettings()
{
    if (!mSettingsDlg)
        mSettingsDlg = new SettingsDialog(this, true);


    mSettingsDlg->show();
    mSettingsDlg->raise();
    mSettingsDlg->activateWindow();

    // Центрируем относительно главного окна
    const QRect r = geometry();
    const QSize s = mSettingsDlg->size();
    mSettingsDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}