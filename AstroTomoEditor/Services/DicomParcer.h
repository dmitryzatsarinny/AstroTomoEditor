#pragma once
#ifndef DICOMPARCER_h
#define DICOMPARCER_h

#include "Pool.h"

namespace DicomParcer
{
    QString normalizePN(QString pn)
    {
        QStringList parts = pn.split('^', Qt::KeepEmptyParts);
        for (QString& p : parts) p = p.trimmed();
        parts.erase(std::remove_if(parts.begin(), parts.end(),
            [](const QString& s) { return s.isEmpty(); }),
            parts.end());
        return parts.join(' ').trimmed();
    }

    QString normalizeDicomDate(QString s)
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

    QString mapSex(QString sx)
    {
        sx = sx.trimmed().toUpper();
        if (sx == "M") return "М";
        if (sx == "F") return "Ж";
        if (sx == "O") return "Др.";
        if (sx == "U") return "Неизв.";
        return sx;
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

} // namespace

#endif