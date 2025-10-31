#include "DicomSniffer.h"
#include <QFileInfo>
#include <QFile>
#include <QIODevice>

namespace DicomSniffer {
    bool isDicomdirName(const QString& fileName) noexcept {
        return (fileName.compare(QStringLiteral("DICOMDIR"), Qt::CaseInsensitive) == 0) || (fileName.compare(QStringLiteral("DIRFILE"), Qt::CaseInsensitive) == 0);
    }

    bool looksLikeDicomFile(const QString& filePath) noexcept {
        const QFileInfo fi(filePath);
        if (!fi.isFile() || !fi.exists()) return false;
        if (fi.fileName().contains(QStringLiteral("DICOMDIR"), Qt::CaseInsensitive) || fi.fileName().contains(QStringLiteral("DIRFILE"), Qt::CaseInsensitive)) return false;

        const QString lower = fi.fileName().toLower();
        if (lower.endsWith(".dcm") || lower.endsWith(".dicom") || lower.endsWith(".ima"))
            return true;

        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly) || f.size() < 132) return false;

        if (!f.seek(128)) return false;
        char magic[4];
        if (f.read(magic, 4) != 4) return false;
        return magic[0] == 'D' && magic[1] == 'I' && magic[2] == 'C' && magic[3] == 'M';
    }
}