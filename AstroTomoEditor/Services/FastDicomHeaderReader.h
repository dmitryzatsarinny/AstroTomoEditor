#pragma once
#include <QString>
#include <QByteArray>

struct FastDicomHeader
{
    // минимум под твой SeriesScan
    QString studyUID;
    QString seriesUID;
    QString seriesDescription;
    QString modality;
    QString seriesNumber;

    int instanceNumber = 0;
    bool hasInstanceNumber = false;

    double ippZ = 0.0;
    bool hasIppZ = false;

    int rows = 0;
    int cols = 0;
    int bitsAllocated = 0;
    int samplesPerPixel = 0;
    int pixelRepresentation = 0;
    QString photometric;

    bool hasPixelKey = false;
};

class FastDicomHeaderReader
{
public:
    // maxBytes: сколько максимум читаем из файла (1..4 МБ обычно за глаза)
    // return: true если похоже на валидный DICOM dataset и нашли хоть что-то полезное
    static bool readHeader(const QString& path, FastDicomHeader& out, QString* err = nullptr,
        int maxBytes = 1024 * 1024);

private:
    struct Cursor {
        const uchar* p = nullptr;
        const uchar* e = nullptr;
    };

    enum class VrMode { Explicit, Implicit };

    static bool parseDataset(Cursor& c, VrMode vrMode, FastDicomHeader& out, QString* err);
    static bool readTag(Cursor& c, quint16& g, quint16& e, QString* err);

    static bool readU16(Cursor& c, quint16& v);
    static bool readU32(Cursor& c, quint32& v);
    static bool readBytes(Cursor& c, int n, const uchar*& v);

    static QString readStringValue(const uchar* v, int n);
    static bool parseInt(const QString& s, int& v);
    static bool parseDouble(const QString& s, double& v);

    static bool skipValue(Cursor& c, quint32 len, QString* err);

    // SQ/Items
    static bool skipSequence(Cursor& c, VrMode vrMode, quint32 len, QString* err);

    static bool isLikelyExplicitVr(const Cursor& c);
};
