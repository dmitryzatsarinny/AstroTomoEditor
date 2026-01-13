#pragma once
#ifndef DICOMPARCER_h
#define DICOMPARCER_h

#include "Pool.h"

namespace DicomParcer
{
    inline QString normalizePN(QString pn)
    {
        QStringList parts = pn.split('^', Qt::KeepEmptyParts);
        for (QString& p : parts) p = p.trimmed();
        parts.erase(std::remove_if(parts.begin(), parts.end(),
            [](const QString& s) { return s.isEmpty(); }),
            parts.end());
        return parts.join(' ').trimmed();
    }

    inline QString normalizeDicomDate(QString s)
    {
        s = s.trimmed();
        if (s.isEmpty()) return s;

        // выбросить всё, кроме цифр — часто помогает при "YYYY.MM.DD", "YYYY-MM-DD"
        QString digits;
        digits.reserve(s.size());
        for (QChar ch : s) if (ch.isDigit()) digits.push_back(ch);

        // пусто или все нули — считаем неизвестной
        if (digits.isEmpty() || digits == "00000000") return QString();

        // классика: YYYYMMDD
        if (digits.size() >= 8) {
            const QString yyyy = digits.mid(0, 4);
            const QString mm = digits.mid(4, 2);
            const QString dd = digits.mid(6, 2);
            const QDate d = QDate::fromString(yyyy + mm + dd, "yyyyMMdd");
            if (d.isValid()) return d.toString("dd.MM.yyyy");
        }

        // пробуем «как есть» в популярных форматах
        QDate d = QDate::fromString(s, "yyyy.MM.dd");
        if (!d.isValid()) d = QDate::fromString(s, "yyyy-MM-dd");
        if (d.isValid()) return d.toString("dd.MM.yyyy");

        // иначе — возвращаем исходное, вдруг там уже человекочитаемый формат
        return s;
    }

    inline QString mapSex(QString sx)
    {
        sx = sx.trimmed().toUpper();
        if (sx == "M") return QObject::tr("Male");
        if (sx == "F") return QObject::tr("Female");
        if (sx == "O") return QObject::tr("Other");
        if (sx == "U") return QObject::tr("Unknown"); // лучше так, чем "No"
        return sx;
    }

    inline QString mapSex(quint16 uix)
    {
        if (uix == 1) return QObject::tr("Male");
        if (uix == 2) return QObject::tr("Female");
        return QObject::tr("Unknown");
    }

    static bool looksLikeDicomDirDataset(const QString& file)
    {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return false;

        if (f.size() >= 132) {
            f.seek(128);
            char sig[4] = {};
            if (f.read(sig, 4) == 4 && sig[0] == 'D' && sig[1] == 'I' && sig[2] == 'C' && sig[3] == 'M')
                return true;
        }
        f.seek(0);
        QByteArray head = f.read(8192);
        return head.contains("1.2.840.10008.1.3.10"); // UID Media Storage Directory Storage
    }

    static std::pair<QString, bool> ensureDicomdirAlias(const QString& src, const QString& baseDir)
    {
        const QString dicomdir = QDir(baseDir).filePath(QStringLiteral("DICOMDIR"));
        if (QFileInfo::exists(dicomdir)) return { dicomdir, false };
        if (QFile::link(src, dicomdir))  return { dicomdir, true };   // быстрый хардлинк
        if (QFile::copy(src, dicomdir))  return { dicomdir, true };   // если линк нельзя
        return { QString(), false };
    }

    static bool isMonochromeImage(vtkDICOMMetaData* md)
    {
        if (!md) return false;
        int spp = 1;
        if (md->Has(DC::SamplesPerPixel)) spp = md->Get(DC::SamplesPerPixel).AsInt();
        QString phot = QString::fromStdString(md->Get(DC::PhotometricInterpretation).AsString()).toUpper();
        // допускаем отсутствие тега — тогда ориентируемся по spp
        const bool monoPi = phot.startsWith("MONOCHROME");
        return (spp == 1) && (phot.isEmpty() || monoPi);
    }

    static inline quint16 rd16(const uchar* p)
    {
        return quint16(p[0]) | (quint16(p[1]) << 8);
    }

    static inline quint32 rd32(const uchar* p)
    {
        return quint32(p[0]) |
            (quint32(p[1]) << 8) |
            (quint32(p[2]) << 16) |
            (quint32(p[3]) << 24);
    }

    inline QStringList dicomFoldersFromDicomdir(const QString& dicomdirPath)
    {
        QStringList result;
        QSet<QString> dirs;

        QFile f(dicomdirPath);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "dicomFoldersFromDicomdir: cannot open" << dicomdirPath;
            return result;
        }

        const QByteArray data = f.readAll();
        if (data.size() < 256) {
            qWarning() << "dicomFoldersFromDicomdir: file too small";
            return result;
        }

        const uchar* p = reinterpret_cast<const uchar*>(data.constData());
        const int    len = data.size();

        int pos = 0;

        // пропускаем 128 байт + "DICM", если есть
        if (len >= 132 && memcmp(p + 128, "DICM", 4) == 0)
            pos = 132;

        // интересующие теги
        const quint16 GR_DIRREC = 0x0004;
        const quint16 EL_DIRSEQ = 0x1220;

        const quint16 GR_ITEM = 0xFFFE;
        const quint16 EL_ITEM = 0xE000;
        const quint16 EL_ITEMDL = 0xE0DD;

        const quint16 GR_TYPE = 0x0004;
        const quint16 EL_TYPE = 0x1430;
        const quint16 EL_FILEID = 0x1500;

        // Ищем DirectoryRecordSequence (0004,1220)
        int seqStart = -1;
        int seqEnd = len;

        while (pos + 8 <= len) {
            quint16 g = rd16(p + pos);
            quint16 e = rd16(p + pos + 2);
            char vrA = char(p[pos + 4]);
            char vrB = char(p[pos + 5]);

            quint32 vlen = 0;

            if (isalpha(uchar(vrA)) && isalpha(uchar(vrB))) {
                QByteArray vr = QByteArray::fromRawData(reinterpret_cast<const char*>(p + pos + 4), 2);
                if (vr == "OB" || vr == "OW" || vr == "OF" ||
                    vr == "SQ" || vr == "UT" || vr == "UN") {
                    // 2 резерва + 4 длина
                    vlen = rd32(p + pos + 8);
                    pos += 12;
                }
                else {
                    vlen = rd16(p + pos + 6);
                    pos += 8;
                }
            }
            else {
                // на всякий случай implicit VR
                vlen = rd32(p + pos + 4);
                pos += 8;
            }

            if (g == GR_DIRREC && e == EL_DIRSEQ) {
                seqStart = pos;
                if (vlen != 0xFFFFFFFF)
                    seqEnd = qMin(len, int(pos + vlen));
                else
                    seqEnd = len;   // undefined length — до Sequence Delimitation
                break;
            }

            if (vlen == 0xFFFFFFFF) {
                // простая защита: не умеем нормально с undefined length
                break;
            }

            pos += int(vlen);
        }

        if (seqStart < 0)
            return result;

        const QString rootDir = QFileInfo(dicomdirPath).absolutePath();

        // Парсим ITEMS внутри DirectoryRecordSequence
        pos = seqStart;
        while (pos + 8 <= seqEnd) {
            quint16 g = rd16(p + pos);
            quint16 e = rd16(p + pos + 2);

            if (g == GR_ITEM && e == EL_ITEMDL) {
                // Sequence Delimitation Item
                break;
            }

            if (g != GR_ITEM || e != EL_ITEM) {
                // что-то пошло не так, выходим
                break;
            }

            quint32 itemLen = rd32(p + pos + 4);
            pos += 8;
            if (itemLen == 0xFFFFFFFF) {
                // для простоты не разбираем undefined length у самих ITEM
                break;
            }

            const int itemEnd = qMin(len, int(pos + itemLen));

            QString recordType;
            QString fileId;

            int ipos = pos;
            while (ipos + 8 <= itemEnd) {
                quint16 ig = rd16(p + ipos);
                quint16 ie = rd16(p + ipos + 2);
                char vrA = char(p[ipos + 4]);
                char vrB = char(p[ipos + 5]);

                quint32 ilen = 0;
                int     header = 0;

                if (isalpha(uchar(vrA)) && isalpha(uchar(vrB))) {
                    QByteArray vr = QByteArray::fromRawData(reinterpret_cast<const char*>(p + ipos + 4), 2);
                    if (vr == "OB" || vr == "OW" || vr == "OF" ||
                        vr == "SQ" || vr == "UT" || vr == "UN") {
                        ilen = rd32(p + ipos + 8);
                        header = 12;
                    }
                    else {
                        ilen = rd16(p + ipos + 6);
                        header = 8;
                    }
                }
                else {
                    ilen = rd32(p + ipos + 4);
                    header = 8;
                }

                const int valuePos = ipos + header;
                const int nextTag = valuePos + int(ilen);
                if (nextTag > itemEnd)
                    break;

                if (ig == GR_TYPE && ie == EL_TYPE && ilen > 0) {
                    QByteArray v = QByteArray::fromRawData(
                        reinterpret_cast<const char*>(p + valuePos), int(ilen));
                    recordType = QString::fromLatin1(v).trimmed();
                }
                else if (ig == GR_TYPE && ie == EL_FILEID && ilen > 0) {
                    QByteArray v = QByteArray::fromRawData(
                        reinterpret_cast<const char*>(p + valuePos), int(ilen));
                    QString raw = QString::fromLatin1(v).trimmed();
                    QStringList parts = raw.split("\\", Qt::SkipEmptyParts);
                    fileId = parts.join("/");
                }

                ipos = nextTag;
            }

            // нас интересуют только записи IMAGE с валидным FILEID
            if (!recordType.isEmpty() &&
                recordType.compare("IMAGE", Qt::CaseInsensitive) == 0 &&
                !fileId.isEmpty())
            {
                const QString fullPath = QDir(rootDir).absoluteFilePath(fileId);
                const QString dirPath = QFileInfo(fullPath).absolutePath();
                dirs.insert(dirPath);
            }

            pos = itemEnd;
        }

        result = dirs.values();
        // при желании можно отсортировать
        std::sort(result.begin(), result.end());
        return result;
    }

} // namespace

#endif