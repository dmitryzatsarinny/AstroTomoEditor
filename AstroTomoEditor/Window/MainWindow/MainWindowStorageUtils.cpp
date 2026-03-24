#include "MainWindowStorageUtils.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <vtkDICOMMetaData.h>
#include <vtkDICOMParser.h>
#include <vtkDICOMTag.h>
#include <vtkSmartPointer.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <string>

namespace MainWindowStorageUtils
{
    namespace
    {
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

    QVector<PatientFolderEntry> loadHdBasePatients(const QString& basePath, bool forceRefresh)
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

    QString sanitizeSeriesFolderName(const QString& description, const QString& fallbackSeriesKey)
    {
        const QString safeName = description.simplified().isEmpty()
            ? fallbackSeriesKey
            : description.simplified();

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
        return folderName;
    }
}