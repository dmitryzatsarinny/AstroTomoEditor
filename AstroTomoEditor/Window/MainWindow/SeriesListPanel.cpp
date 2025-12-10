#include "SeriesListPanel.h"
#include "..\..\Services\DicomParcer.h"
#include "..\..\Services\PatientInfo.h"
#include <Services/VolumeFix3DR.h>

#include <QtConcurrent/QtConcurrentRun>
#include <QCollator>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QIcon>
#include <QListView>
#include <QListWidget>
#include <QMetaObject>
#include <QMetaType>
#include <QPair>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QVBoxLayout>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <utility>
#include <limits>
#include <cmath>
#include <QSet>

#include <vtkDICOMMetaData.h>
#include <vtkDICOMParser.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMValue.h>
#include <vtkImageData.h>
#include <vtkImageShiftScale.h>
#include <vtkSmartPointer.h>
#include <vtk-9.5/vtkGDCMImageReader.h>

#include <QElapsedTimer>
#include <QAtomicInt>
#include <QtConcurrent/QtConcurrentFilter> 
#include <QtConcurrent/QtConcurrentMap> 
#include "PlanarView.h"

// --- Быстрая проверка: Part-10 преамбула ---
static inline bool hasDicmPreamble(const uchar* p, qsizetype n) {
    return (n >= 132 && p[128] == 'D' && p[129] == 'I' && p[130] == 'C' && p[131] == 'M');
}

// Набор допустимых VR (2 ASCII-символа)
static inline bool isVR2(char a, char b) {
    switch ((a << 8) | b) {
    case ('P' << 8 | 'N'): case ('U' << 8 | 'I'): case ('C' << 8 | 'S'):
    case ('D' << 8 | 'A'): case ('T' << 8 | 'M'): case ('S' << 8 | 'H'):
    case ('L' << 8 | 'O'): case ('S' << 8 | 'T'): case ('A' << 8 | 'E'):
    case ('A' << 8 | 'S'): case ('I' << 8 | 'S'): case ('F' << 8 | 'D'):
    case ('F' << 8 | 'L'): case ('U' << 8 | 'S'): case ('U' << 8 | 'L'):
    case ('S' << 8 | 'S'): case ('S' << 8 | 'L'): case ('O' << 8 | 'B'):
    case ('O' << 8 | 'W'): case ('O' << 8 | 'F'): case ('O' << 8 | 'D'):
    case ('U' << 8 | 'N'): case ('Q' << 8 | 'S'): return true;
    default: return false;
    }
}

// Маленький парсер первых тегов: поддержка Explicit/Implicit VR (LE)
// Этого хватает, чтобы со 100% вероятностью отличить DICOM от мусора
static bool looksLikeDicomLE(const uchar* p, qsizetype n)
{
    // Начинаем или с 0 (raw) или с 132 (после преамбулы)
    size_t off0 = hasDicmPreamble(p, n) ? 132 : 0;
    size_t off = off0;

    // Пройдём первые ~64 тега или ~8 KB, что наступит раньше
    for (int i = 0; i < 64 && off + 8 <= (size_t)n; ++i) {
        // читаем Tag (Group,Element)
        quint16 gg = p[off] | (p[off + 1] << 8);
        quint16 ee = p[off + 2] | (p[off + 3] << 8);
        off += 4;

        // теги должны быть чётной группой и не "мусорными"
        if ((gg & 1) != 0) return false; // private группы на старте встречаются редко

        // Пытаемся прочитать длину (Explicit или Implicit)
        quint32 len = 0;
        bool explicitVR = false;

        if (off + 2 <= (size_t)n && isVR2((char)p[off], (char)p[off + 1])) {
            explicitVR = true;
            char vrA = (char)p[off], vrB = (char)p[off + 1];
            off += 2;
            // Для OB/OW/OF/SQ/UT/UN длина хранится как 0x0000 + 32-бит
            if ((vrA == 'O' && (vrB == 'B' || vrB == 'W' || vrB == 'F' || vrB == 'D')) ||
                (vrA == 'U' && (vrB == 'T' || vrB == 'N')) ||
                (vrA == 'S' && vrB == 'Q')) {
                if (off + 6 > (size_t)n) break;
                // 2 зарезервированных байта
                off += 2;
                len = p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (p[off + 3] << 24);
                off += 4;
            }
            else {
                if (off + 2 > (size_t)n) break;
                len = p[off] | (p[off + 1] << 8);
                off += 2;
            }
        }
        else {
            // Implicit VR: просто 32-бит длина
            if (off + 4 > (size_t)n) break;
            len = p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (p[off + 3] << 24);
            off += 4;
        }

        // Правдоподобность: первые теги обычно из групп 0002/0008/0010
        if (gg <= 0x0010 && (ee <= 0x0020 || ee == 0x0016 || ee == 0x0000)) {
            // это очень похоже на DICOM
            return true;
        }

        // Сдвигаем на значение
        size_t skip = qMin<quint32>(len, (quint32)0x40000000u);
        if (off + skip > (size_t)n) break;
        off += skip;

        // Не уйти слишком далеко
        if (off - off0 > 16384) break;
    }
    return false;
}

// Быстрый доступ к префиксу файла: mmap -> read в буфер
static bool mapPrefix(const QString& path, const uchar*& data, qsizetype& n, QByteArray& storage, qsizetype need = 16384)
{
    data = nullptr; n = 0; storage.clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const qint64 size = f.size();
    if (size < 132) { return false; }

    const qsizetype take = (qsizetype)qMin<qint64>(size, need);

    uchar* mm = f.map(0, take);
    if (mm) {
        // ВНИМАНИЕ: map живёт, пока живёт QFile — storage оставляем пустым,
        // а файл пусть живёт до конца функции-обёртки (см. ниже).
        data = mm; n = take;
        // нельзя закрывать файл до unmap — поэтому используем локальную обёртку ниже
        return true;
    }

    // Fallback — быстро прочитать в память
    storage.resize(take);
    if (f.read(storage.data(), take) != take) return false;
    data = reinterpret_cast<const uchar*>(storage.constData());
    n = take;
    return true;
}

// Обёртка: true, если файл очень похож на DICOM (быстро)
static bool isDicomLikelyFast(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const qint64 size = f.size();
    if (size < 132) return false;                    // слишком мал

    const qsizetype need = 16384;                    // 16 KB префикс
    const qsizetype take = (qsizetype)qMin<qint64>(size, need);

    const uchar* p = f.map(0, take);                 // пробуем mmap
    QByteArray tmp;
    if (!p) {                                        // fallback read
        tmp.resize(take);
        if (f.read(tmp.data(), take) != take) return false;
        p = reinterpret_cast<const uchar*>(tmp.constData());
    }

    const bool ok = hasDicmPreamble(p, take) || looksLikeDicomLE(p, take);

    if (p && !tmp.size()) f.unmap(const_cast<uchar*>(p));
    return ok;
}


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
    };

    QHash<QString, QVector<QuickDicomFile>> entriesBySeries;
    QHash<QString, QVector<QString>> filesBySeries;
    QVector<SeriesItem> items;
    PatientInfo patientInfo;
    bool patientInfoValid = false;
    int totalFiles = 0;
    bool canceled = false;
};

Q_DECLARE_METATYPE(SeriesScanResult)

class SeriesScanWorker : public QObject {
    Q_OBJECT
public:
    explicit SeriesScanWorker(QObject* parent = nullptr) : QObject(parent) {}

    void setInclude3dr(bool on) { mInclude3dr = on; }

public slots:
    void startScan(const QStringList& rootPaths);
    void cancel() { mCancelRequested.store(true, std::memory_order_relaxed); }

signals:
    void scanStarted(int totalFiles);
    void scanProgress(int processed, int totalFiles, const QString& currentPath);
    void scanCompleted(const SeriesScanResult& result);

private:
    bool shouldCancel() const { return mCancelRequested.load(std::memory_order_relaxed); }
    SeriesScanResult runScan(const QStringList& rootPaths);

    std::atomic_bool mCancelRequested{ false };
    bool mInclude3dr = false;
};


SeriesListPanel::SeriesListPanel(QWidget* parent) : QWidget(parent)
{
    mList = new QListWidget(this);
    auto* lay = new QVBoxLayout(this);

    mList->setViewMode(QListView::ListMode);
    mList->setUniformItemSizes(true);
    mList->setWordWrap(false);
    mList->setTextElideMode(Qt::ElideRight);
    mList->setIconSize(QSize(64, 64));
    mList->setSpacing(6);
    mList->setGridSize(QSize(0, kRowH));

    lay->addWidget(mList);
    setLayout(lay);

    auto activate = [this](QListWidgetItem* it) {
        if (!it) return;

        const QString key = it->data(Qt::UserRole).toString();
        const QVector<QString> files = mFilesBySeries.value(key);

        emit seriesActivated(key, files);              // как и было
        updatePatientInfoForSeries(key, files);        // новая логика
        };

    connect(mList, &QListWidget::itemClicked, this, activate);
    connect(mList, &QListWidget::itemDoubleClicked, this, activate);

    qRegisterMetaType<PatientInfo>("PatientInfo");
    qRegisterMetaType<SeriesItem>("SeriesItem");

    connect(&mThumbWatcher, &QFutureWatcher<QImage>::finished,
        this, &SeriesListPanel::handleThumbReady);
}

SeriesListPanel::~SeriesListPanel()
{
    cancelScan();
    abortThumbLoading();

    if (mScanThread) {
        mScanThread->quit();
        mScanThread->wait();
        mScanThread = nullptr;
    }
}

void SeriesListPanel::cancelScan()
{
    mCancelScan = true;
    if (mScanWorker) {
        QMetaObject::invokeMethod(mScanWorker, "cancel", Qt::QueuedConnection);
    }
}

static QString pickMiddleSliceFile(const QVector<SeriesScanResult::QuickDicomFile>& files)
{
    if (files.isEmpty()) return {};

    struct Key {
        bool    hasZ = false;
        double  z = 0.0;
        bool    hasNum = false;
        int     num = 0;
        QString name;
        QString path;
    };

    QVector<Key> keys;
    keys.reserve(files.size());

    for (const auto& f : files) {
        Key k;
        k.path = f.path;
        k.name = f.fileName;
        k.hasZ = f.hasZ;
        k.z = f.z;
        k.hasNum = f.hasInstance;
        k.num = f.instance;
        keys.push_back(std::move(k));
    }

    QCollator coll; coll.setNumericMode(true); coll.setCaseSensitivity(Qt::CaseInsensitive);

    std::sort(keys.begin(), keys.end(), [&](const Key& a, const Key& b) {
        // 1) если у обоих есть Z — сортируем по Z
        if (a.hasZ && b.hasZ) return a.z < b.z;
        // 2) иначе по InstanceNumber
        if (a.hasNum && b.hasNum) return a.num < b.num;
        if (a.hasNum != b.hasNum) return a.hasNum; // у кого есть номер — тот раньше
        // 3) иначе стабильно по имени файла
        const int c = coll.compare(a.name, b.name);
        if (c != 0) return c < 0;
        // 4) и совсем на крайний случай — по полному пути
        return a.path < b.path;
        });

    const int mid = (keys.size() - 1) / 2; // середина стека
    return keys[mid].path;
}

static void sortSeriesByName(QVector<SeriesItem>& items)
{
    QCollator coll;
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    coll.setNumericMode(true);        // «естественная» сортировка с числами
    coll.setIgnorePunctuation(true);

    std::sort(items.begin(), items.end(),
        [&](const SeriesItem& a, const SeriesItem& b)
        {
            const int c = coll.compare(a.description, b.description);
            if (c != 0) return c < 0;
            // тай-брейки: больше снимков — выше
            if (a.numImages != b.numImages) return a.numImages > b.numImages;
            // стабильность: по UID
            return a.seriesKey < b.seriesKey;
        });
}



static inline QString qstr(const vtkDICOMValue& v) {
    return QString::fromStdString(v.AsString());
}

void SeriesListPanel::scanSingleFile(const QString& filePath)
{
    emit scanStarted(1);

    mCancelScan = false;
    mFilesBySeries.clear();

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        emit scanStarted(0);
        emit scanFinished(0, 0);
        return;
    }

    auto r = vtkSmartPointer<vtkDICOMReader>::New();
    r->SetFileName(filePath.toUtf8().constData());

    if (r->CanReadFile(r->GetFileName()) == 0) {
        emit scanFinished(0, 1);
        return;
    }

    r->UpdateInformation();
    vtkDICOMMetaData* md = r->GetMetaData();
    if (!md) { emit scanFinished(0, 1); return; }

    const bool hasRows = md->Has(DC::Rows);
    const bool hasCols = md->Has(DC::Columns);
    const bool hasPixel = md->Has(DC::PixelData) || md->Has(DC::FloatPixelData) || md->Has(DC::DoubleFloatPixelData);
    if (!(hasRows && hasCols && hasPixel)) {
        emit scanFinished(0, 1);
        return;
    }

    // Patient info
    // Patient info
    PatientInfo pinfo;
    pinfo.patientName = DicomParcer::normalizePN(qstr(md->Get(DC::PatientName)));
    {
        QString pid = qstr(md->Get(DC::PatientID));
        if (pid.isEmpty()) pid = qstr(md->Get(DC::StudyID));
        pinfo.patientId = pid.trimmed();
    }
    pinfo.sex = DicomParcer::mapSex(qstr(md->Get(DC::PatientSex)));
    pinfo.birthDate = DicomParcer::normalizeDicomDate(qstr(md->Get(DC::PatientBirthDate)));

    // новые поля
    pinfo.Mode = qstr(md->Get(DC::Modality)).trimmed();

    QString seriesDescr = qstr(md->Get(DC::SeriesDescription)).trimmed();
    if (seriesDescr.isEmpty()) {
        const QString modality = qstr(md->Get(DC::Modality));
        const QString snum = qstr(md->Get(DC::SeriesNumber));
        seriesDescr = (modality.isEmpty() && snum.isEmpty())
            ? tr("(single image)")
            : QString("%1 %2").arg(modality, snum);
    }
    pinfo.Description = seriesDescr;

    QString seqName;
    if (md->Has(DC::SequenceName))
        seqName = qstr(md->Get(DC::SequenceName));
    else if (md->Has(DC::ProtocolName))
        seqName = qstr(md->Get(DC::ProtocolName));
    else
        seqName = qstr(md->Get(DC::SeriesNumber)); // на крайний случай

    pinfo.Sequence = seqName.trimmed();


    // Series item
    QString seriesUID = qstr(md->Get(DC::SeriesInstanceUID));
    if (seriesUID.isEmpty()) seriesUID = QStringLiteral("SINGLE_SERIES");

    QVector<QString> files{ filePath };
    mFilesBySeries[seriesUID] = files;

    SeriesItem s;
    s.seriesKey = seriesUID;
    s.firstFile = filePath;
    s.numImages = 1;
    s.studyID = qstr(md->Get(DC::StudyInstanceUID));

    QString desc = qstr(md->Get(DC::SeriesDescription));
    if (desc.isEmpty()) {
        const QString modality = qstr(md->Get(DC::Modality));
        const QString snum = qstr(md->Get(DC::SeriesNumber));
        desc = (modality.isEmpty() && snum.isEmpty())
            ? tr("(single image)")
            : QString("%1 %2").arg(modality, snum);
    }
    s.description = desc;
    s.thumb = makeThumbImageFromDicom(filePath);

    // Отрисовать
    QVector<SeriesItem> items; items.push_back(std::move(s));
    sortSeriesByName(items);
    populate(items);

    emit patientInfoChanged(pinfo);
    emit scanFinished(1, 1);
}

void SeriesListPanel::scanDicomDir(const QString& rootPath)
{
    QFileInfo fi(rootPath);
    const QString baseDir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

    QString dicomdirPath;
    if (fi.isFile()) {
        dicomdirPath = fi.absoluteFilePath();
    }
    else {
        static const char* names[] = { "DICOMDIR", "dicomdir", "DICOMDIR;1", "DIRFILE", "dirfile" };
        for (auto n : names) {
            const QString candidate = QDir(baseDir).filePath(QString::fromLatin1(n));
            if (QFileInfo::exists(candidate)) { dicomdirPath = candidate; break; }
        }
    }

    QStringList roots = DicomParcer::dicomFoldersFromDicomdir(dicomdirPath);
    if (roots.isEmpty())
        roots = { baseDir };

    ensureWorker();
    abortThumbLoading();

    mCancelScan = false;
    mFilesBySeries.clear();

    QMetaObject::invokeMethod(
        mScanWorker, 
        "startScan", 
        Qt::QueuedConnection,
        Q_ARG(QStringList, roots));
}

static QImage makeThumbImageFromVtkMidSlice(vtkImageData* img)
{
    if (!img) return {};

    int ext[6]; img->GetExtent(ext);
    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;
    if (nx <= 0 || ny <= 0 || nz <= 0) return {};

    // берём середину по Z
    const int kMid = ext[4] + (ext[5] - ext[4]) / 2;

    // убеждаемся, что тип именно U8 и 1 канал
    if (img->GetScalarType() != VTK_UNSIGNED_CHAR || img->GetNumberOfScalarComponents() != 1) {
        // если что-то не так — можно тут сделать ранний выход или безопасное преобразование
        return {};
    }

    auto sh = vtkSmartPointer<vtkImageShiftScale>::New();
    sh->SetInputData(img);
    sh->ClampOverflowOn();
    sh->SetOutputScalarTypeToUnsignedChar();
    sh->Update();

    vtkImageData* u8 = sh->GetOutput();
    u8->GetExtent(ext);
    const int w = ext[1] - ext[0] + 1;
    const int h = ext[3] - ext[2] + 1;
    if (w <= 0 || h <= 0) return {};

    bool flipX = false, flipY = true;

    auto* p00 = static_cast<char*>(u8->GetScalarPointer(ext[0], ext[2], ext[4]));
    auto* p10 = static_cast<char*>(u8->GetScalarPointer(std::min(ext[0] + 1, ext[1]), ext[2], ext[4]));
    auto* p01 = static_cast<char*>(u8->GetScalarPointer(ext[0], std::min(ext[2] + 1, ext[3]), ext[4]));

    const ptrdiff_t incXb_raw = (w > 1) ? (p10 - p00) : 1;
    const ptrdiff_t incYb_raw = (h > 1) ? (p01 - p00) : 1;

    const bool xPositive = (incXb_raw >= 0) ^ flipX;
    const bool yPositive = (incYb_raw >= 0) ^ flipY;

    const int  xStart = xPositive ? ext[0] : ext[1];
    const int  yStart = yPositive ? ext[2] : ext[3];
    const ptrdiff_t stepXb = (xPositive ? +1 : -1) * std::abs(incXb_raw);

    QImage q(w, h, QImage::Format_Grayscale8);
    for (int yy = 0; yy < h; ++yy) {
        const int ySrc = yPositive ? (yStart + yy) : (yStart - yy);
        auto* row0 = static_cast<char*>(u8->GetScalarPointer(xStart, ySrc, ext[4]));
        uchar* dst = q.scanLine(yy);
        for (int xx = 0; xx < w; ++xx)
            dst[xx] = *(reinterpret_cast<unsigned char*>(row0 + stepXb * xx));
    }
    return q;
}


void SeriesListPanel::scan3drFile(const QString& filePath)
{
    emit scanStarted(1);
    mCancelScan = false;
    mFilesBySeries.clear();

    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        emit scanStarted(0);
        emit scanFinished(0, 0);
        return;
    }

    _3Dinfo hdr{};
    vtkSmartPointer<vtkImageData> img = read3dr_asVtk(filePath, &hdr);
    if (!img) { emit scanFinished(0, 1); return; }

    int ext[6]; img->GetExtent(ext);
    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    // patient stub
    PatientInfo pinfo;
    pinfo.patientName = fi.completeBaseName();
    pinfo.patientId = fi.baseName();
    pinfo.sex.clear();
    pinfo.birthDate.clear();

    pinfo.Mode = (hdr.IsMRI == 1) ? "MRI" : "CT";
    pinfo.Description = QStringLiteral("3DR volume %1×%2×%3")
        .arg(nx).arg(ny).arg(nz);
    pinfo.Sequence = QString();   // для 3DR можно оставить пустым или "N/A"

    const QString seriesKey = QStringLiteral("3DR_%1x%2x%3_%4")
        .arg(nx).arg(ny).arg(nz).arg(fi.fileName());

    mFilesBySeries[seriesKey] = QVector<QString>{ filePath };

    SeriesItem s;
    s.seriesKey = seriesKey;
    s.firstFile = filePath;
    s.numImages = 1;
    s.studyID = QStringLiteral("3DR");
    s.description = QStringLiteral("3DR volume %1×%2×%3").arg(nx).arg(ny).arg(nz);

    _3Dinfo hdr2{};
    vtkSmartPointer<vtkImageData> img2 = read3dr_asVtk_noflip(filePath, &hdr2);
    s.thumb = makeThumbImageFromVtkMidSlice(img2);

    QVector<SeriesItem> items; items.push_back(std::move(s));
    sortSeriesByName(items);
    populate(items);

    emit patientInfoChanged(pinfo);
    emit scanFinished(1, 1);
}



QImage SeriesListPanel::makeThumbImageFromDicom(const QString& file)
{
    auto r = vtkSmartPointer<vtkDICOMReader>::New();
    vtkSmartPointer<vtkGDCMImageReader> gdcm = vtkSmartPointer<vtkGDCMImageReader>::New();

    r->SetFileName(file.toUtf8().constData());
    r->UpdateInformation();

    vtkDICOMMetaData* md = r->GetMetaData();
    if (!md) return {};

    if (!(md->Has(DC::Rows) && md->Has(DC::Columns) &&
        (md->Has(DC::PixelData) || md->Has(DC::FloatPixelData) || md->Has(DC::DoubleFloatPixelData))))
        return {};

    auto isCompressedTS = [&](vtkDICOMMetaData* m)->bool {
        if (!m || !m->Has(DC::TransferSyntaxUID)) return false;
        const std::string ts = m->Get(DC::TransferSyntaxUID).AsString();
        return ts.rfind("1.2.840.10008.1.2.4.", 0) == 0   // JPEG*, JPEG-LS, JPEG2000
            || ts == "1.2.840.10008.1.2.5";              // RLE
        };
    bool isCompressed = isCompressedTS(md);

    vtkImageData* img;

    if (isCompressed)
    {
        gdcm->SetFileName(file.toUtf8().constData());
        gdcm->Update();
        img = gdcm->GetOutput();
    }
    else
    {
        r->Update();
        img = r->GetOutput();
    }

    if (!img) return {};


    // Приводим к U8 по VOI
    double wl = 40.0, ww = 400.0;
    if (md->Has(DC::WindowCenter)) wl = md->Get(DC::WindowCenter).AsDouble();
    if (md->Has(DC::WindowWidth))  ww = md->Get(DC::WindowWidth).AsDouble();
    if (ww <= 1e-6) ww = 1.0;

    const double low = wl - ww / 2.0;
    const double high = wl + ww / 2.0;

    auto sh = vtkSmartPointer<vtkImageShiftScale>::New();
    sh->SetInputData(img);
    sh->ClampOverflowOn();
    sh->SetOutputScalarTypeToUnsignedChar();
    sh->SetShift(-low);
    sh->SetScale(static_cast<double>(HistScale) / (high - low));
    sh->Update();

    vtkImageData* u8 = sh->GetOutput();
    int ext[6]; u8->GetExtent(ext);
    const int w = ext[1] - ext[0] + 1;
    const int h = ext[3] - ext[2] + 1;
    if (w <= 0 || h <= 0) return {};

    bool flipX = false, flipY = true;

    auto* p00 = static_cast<char*>(u8->GetScalarPointer(ext[0], ext[2], ext[4]));
    auto* p10 = static_cast<char*>(u8->GetScalarPointer(std::min(ext[0] + 1, ext[1]), ext[2], ext[4]));
    auto* p01 = static_cast<char*>(u8->GetScalarPointer(ext[0], std::min(ext[2] + 1, ext[3]), ext[4]));

    const ptrdiff_t incXb_raw = (w > 1) ? (p10 - p00) : 1;
    const ptrdiff_t incYb_raw = (h > 1) ? (p01 - p00) : 1;

    const bool xPositive = (incXb_raw >= 0) ^ flipX;
    const bool yPositive = (incYb_raw >= 0) ^ flipY;

    const int  xStart = xPositive ? ext[0] : ext[1];
    const int  yStart = yPositive ? ext[2] : ext[3];
    const ptrdiff_t stepXb = (xPositive ? +1 : -1) * std::abs(incXb_raw);

    QImage q(w, h, QImage::Format_Grayscale8);
    for (int yy = 0; yy < h; ++yy) {
        const int ySrc = yPositive ? (yStart + yy) : (yStart - yy);
        auto* row0 = static_cast<char*>(u8->GetScalarPointer(xStart, ySrc, ext[4]));
        uchar* dst = q.scanLine(yy);
        for (int xx = 0; xx < w; ++xx)
            dst[xx] = *(reinterpret_cast<unsigned char*>(row0 + stepXb * xx));
    }
    return q;
}

static bool isMonoQuick(vtkDICOMMetaData* d)
{
    const bool hasRows = d->Has(DC::Rows);
    const bool hasCols = d->Has(DC::Columns);
    const bool hasPix = d->Has(DC::PixelData) || d->Has(DC::FloatPixelData) || d->Has(DC::DoubleFloatPixelData);
    if (!(hasRows && hasCols && hasPix)) return false;

    const int spp = d->Get(DC::SamplesPerPixel).AsInt();
    const QString phot = QString::fromUtf8(d->Get(DC::PhotometricInterpretation).AsString()).toUpper();
    return spp == 1 && (phot.startsWith("MONOCHROME"));
}

static QString makeSeriesKey(vtkDICOMMetaData* d)
{
    auto sUID = QString::fromUtf8(d->Get(DC::SeriesInstanceUID).AsString());
    if (!sUID.isEmpty()) return sUID;

    const QString study = QString::fromUtf8(d->Get(DC::StudyInstanceUID).AsString());
    const QString snum = QString::fromUtf8(d->Get(DC::SeriesNumber).AsString());
    const QString mod = QString::fromUtf8(d->Get(DC::Modality).AsString());
    const int rows = d->Has(DC::Rows) ? d->Get(DC::Rows).AsInt() : 0;
    const int cols = d->Has(DC::Columns) ? d->Get(DC::Columns).AsInt() : 0;

    // композитный ключ
    return QString("ST=%1|SN=%2|MD=%3|%4x%5").arg(study, snum, mod).arg(rows).arg(cols);
}


static bool tryFillPatientInfo(const QString& fp, PatientInfo& out)
{
    auto p = vtkSmartPointer<vtkDICOMParser>::New();
    auto d = vtkSmartPointer<vtkDICOMMetaData>::New();
    p->SetMetaData(d);
    p->SetFileName(fp.toUtf8().constData());
    p->Update();

    if (!isMonoQuick(d)) return false;

    QString name = QString::fromUtf8(d->Get(DC::PatientName).AsString());
    QString pid = QString::fromUtf8(d->Get(DC::PatientID).AsString());
    if (pid.isEmpty()) pid = QString::fromUtf8(d->Get(DC::StudyID).AsString());
    QString sex = QString::fromUtf8(d->Get(DC::PatientSex).AsString());
    QString bday = QString::fromUtf8(d->Get(DC::PatientBirthDate).AsString());

    out.patientName = DicomParcer::normalizePN(name);
    out.patientId = pid.trimmed();
    out.sex = DicomParcer::mapSex(sex);
    out.birthDate = DicomParcer::normalizeDicomDate(bday);

    return true;
}



// естественная сортировка имён
void SeriesScanWorker::startScan(const QStringList& rootPaths)
{
    mCancelRequested.store(false, std::memory_order_relaxed);

    SeriesScanResult result = runScan(rootPaths);
    emit scanCompleted(result);
}

SeriesScanResult SeriesScanWorker::runScan(const QStringList& rootPaths)
{
    SeriesScanResult result;

    if (rootPaths.isEmpty()) {
        result.totalFiles = 0;
        return result;
    }

    // ---------- PASS 1: обход нескольких каталогов и сбор кандидатов ----------
    QVector<QString> candidates;
    candidates.reserve(1 << 15);

    QElapsedTimer tick;
    tick.start();

    int seen = 0;

    for (const QString& rootPath : rootPaths)
    {
        if (shouldCancel()) { result.canceled = true; return result; }

        QFileInfo fi(rootPath);
        const QString baseDir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

        QDirIterator it(
            baseDir,
            QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories
        );

        while (it.hasNext())
        {
            if (shouldCancel()) { result.canceled = true; return result; }

            it.next();
            const QFileInfo fi = it.fileInfo();

            const QString ext = fi.suffix().toLower();
            if (ext == "3dr" && !mInclude3dr)
                continue;

            qint64 sz = fi.size();
            if (sz < 1024 || sz >(1ll << 33))
                continue;

            ++seen;
            if (tick.hasExpired(120)) {
                emit scanProgress(seen, 0, fi.filePath());
                tick.restart();
            }

            candidates.push_back(fi.filePath());
            if (candidates.size() >= maxfiles)
                break;
        }

        if (candidates.size() >= maxfiles)
            break;
    }

    if (shouldCancel()) {
        result.canceled = true;
        return result;
    }

    if (candidates.isEmpty()) {
        emit scanStarted(0);
        result.totalFiles = 0;
        return result;
    }

    // ---------- PASS 2: sniff + metadata parsing + группировка ----------
    emit scanStarted(candidates.size());

    struct MapOut {
        bool ok = false;
        QString seriesKey;
        SeriesScanResult::QuickDicomFile meta;
        PatientInfo pinfo;
        bool pinfoValid = false;
    };

    QAtomicInt progressed = 0;

    auto mapFn = [&](const QString& p) -> MapOut {
        MapOut out;
        out.meta.path = p;
        if (shouldCancel()) return out;

        if (!isDicomLikelyFast(p))
            return out;

        // Парсим метаданные
        auto meta = vtkSmartPointer<vtkDICOMMetaData>::New();
        auto parser = vtkSmartPointer<vtkDICOMParser>::New();
        parser->SetMetaData(meta);
        parser->SetFileName(p.toUtf8().constData());
        parser->Update();

        const bool hasGeometry =
            meta->Has(DC::Rows) || meta->Has(DC::Columns);
        const bool hasPixel =
            meta->Has(DC::PixelData) ||
            meta->Has(DC::FloatPixelData) ||
            meta->Has(DC::DoubleFloatPixelData);

        if (!(hasGeometry || hasPixel))
            return out;

        out.seriesKey = makeSeriesKey(meta);
        out.ok = !out.seriesKey.isEmpty();

        if (out.ok)
        {
            out.meta.fileName = QFileInfo(p).fileName();

            if (meta->Has(DC::ImagePositionPatient)) {
                const vtkDICOMValue& v = meta->Get(DC::ImagePositionPatient);
                if (v.GetNumberOfValues() >= 3) {
                    out.meta.z = v.GetDouble(2);
                    out.meta.hasZ = true;
                }
            }

            if (meta->Has(DC::InstanceNumber)) {
                out.meta.instance = meta->Get(DC::InstanceNumber).AsInt();
                out.meta.hasInstance = true;
            }

            out.meta.modality =
                QString::fromUtf8(meta->Get(DC::Modality).AsString());
            out.meta.seriesDescription =
                QString::fromUtf8(meta->Get(DC::SeriesDescription).AsString());
            out.meta.studyUID =
                QString::fromUtf8(meta->Get(DC::StudyInstanceUID).AsString());
            out.meta.seriesNumber =
                QString::fromUtf8(meta->Get(DC::SeriesNumber).AsString());
        }

        if (out.ok) {
            PatientInfo pi;
            if (tryFillPatientInfo(p, pi)) {
                out.pinfo = pi;
                out.pinfoValid = true;
            }
        }

        int k = ++progressed;
        if ((k & 0x0F) == 0)
            emit scanProgress(k, candidates.size(), p);

        return out;
        };

    SeriesScanResult acc;

    auto reduceFn = [&](SeriesScanResult& a, const MapOut& m) {
        if (!m.ok) return;
        a.entriesBySeries[m.seriesKey].push_back(m.meta);
        if (!a.patientInfoValid && m.pinfoValid) {
            a.patientInfo = m.pinfo;
            a.patientInfoValid = true;
        }
        };

    acc = QtConcurrent::blockingMappedReduced<SeriesScanResult>(
        candidates, mapFn, reduceFn, QtConcurrent::SequentialReduce);

    if (shouldCancel()) {
        result.canceled = true;
        return result;
    }

    if (acc.entriesBySeries.isEmpty()) {
        result.totalFiles = 0;
        return result;
    }

    // ---------- PASS 3: группировка, сортировка, превью ----------
    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);

    QVector<SeriesItem> items;
    items.reserve(acc.entriesBySeries.size());

    const auto keys = acc.entriesBySeries.keys();
    items.reserve(keys.size());

    for (const QString& seriesKey : keys)
    {
        if (shouldCancel()) { result.canceled = true; break; }

        QVector<SeriesScanResult::QuickDicomFile> entries =
            acc.entriesBySeries.value(seriesKey);

        std::sort(entries.begin(), entries.end(),
            [&](const auto& a, const auto& b) {
                return coll.compare(a.fileName, b.fileName) < 0;
            });

        QVector<QString> sortedPaths;
        sortedPaths.reserve(entries.size());
        QHash<QString, SeriesScanResult::QuickDicomFile> entryByPath;
        entryByPath.reserve(entries.size());

        for (const auto& e : entries) {
            sortedPaths.push_back(e.path);
            entryByPath.insert(e.path, e);
        }

        QVector<QString> valid = sortedPaths;
        auto rep = PlanarView::filterSeriesByConsistency(sortedPaths);
        if (rep.good.isEmpty()) {
            QString mid = pickMiddleSliceFile(entries);
            valid = mid.isEmpty() ? QVector<QString>{} : QVector<QString>{ mid };
        }
        else {
            valid = rep.good;
        }

        auto zForPath = [&](const QString& p) {
            auto it = entryByPath.find(p);
            if (it == entryByPath.end() || !it->hasZ)
                return std::numeric_limits<double>::quiet_NaN();
            return it->z;
            };

        if (!valid.isEmpty()) {
            double firstZ = zForPath(valid.first());
            double lastZ = zForPath(valid.last());
            if (std::isfinite(firstZ) && std::isfinite(lastZ) && firstZ == lastZ) {
                valid = { valid.first() };
            }
        }

        acc.filesBySeries[seriesKey] = valid;

        QVector<SeriesScanResult::QuickDicomFile> validEntries;
        for (const auto& p : valid)
            validEntries.push_back(entryByPath[p]);

        SeriesItem s;
        s.seriesKey = seriesKey;
        s.firstFile =
            pickMiddleSliceFile(validEntries.isEmpty() ? entries : validEntries);
        s.numImages = valid.size();

        const auto pickMeta = [&]() -> SeriesScanResult::QuickDicomFile {
            for (const auto& x : validEntries) return x;
            if (!entries.isEmpty()) return entries.first();
            return {};
            }();

        s.studyID = pickMeta.studyUID;
        QString desc = pickMeta.seriesDescription;
        if (desc.isEmpty()) {
            QString mod = pickMeta.modality;
            QString sn = pickMeta.seriesNumber;
            desc = (mod.isEmpty() && sn.isEmpty())
                ? QObject::tr("No Name")
                : QStringLiteral("%1 %2").arg(mod, sn);
        }
        s.description = desc;

        s.thumb = SeriesListPanel::makeThumbImageFromDicom(s.firstFile);
        items.push_back(std::move(s));
    }

    if (result.canceled) return result;

    sortSeriesByName(items);

    // переносим данные в результат
    result.filesBySeries = std::move(acc.filesBySeries);
    result.items = std::move(items);
    result.patientInfo = acc.patientInfo;
    result.patientInfoValid = acc.patientInfoValid;

    int dicomCount = 0;
    for (const auto& v : result.filesBySeries)
        dicomCount += v.size();
    result.totalFiles = dicomCount;

    return result;
}



void SeriesListPanel::scanStudy(const QString& rootPath)
{
    QStringList roots;
    roots.push_back(rootPath);

    ensureWorker();
    abortThumbLoading();

    mCancelScan = false;
    mFilesBySeries.clear();

    QMetaObject::invokeMethod(
        mScanWorker,
        "startScan",
        Qt::QueuedConnection,
        Q_ARG(QStringList, roots)
    );
}

void SeriesListPanel::populate(const QVector<SeriesItem>& items)
{
    abortThumbLoading();
    mList->clear();

    for (const auto& s : items)
    {
        // --- корректный нейминг ---
        QString suffix;
        if (s.numImages == 1)
            suffix = tr("slice");
        else
            suffix = tr("slices");

        auto text = QString("%1\n%2 %3")
            .arg(s.description)
            .arg(s.numImages)
            .arg(suffix);

        auto* it = new QListWidgetItem(QIcon(), text);
        it->setData(Qt::UserRole, s.seriesKey);
        it->setToolTip(s.description);
        it->setSizeHint(QSize(0, kRowH));
        mList->addItem(it);

        // превью
        if (!s.thumb.isNull()) {
            QImage scaled = s.thumb.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap pm = QPixmap::fromImage(scaled);

            QIcon icon;
            icon.addPixmap(pm, QIcon::Normal);
            icon.addPixmap(pm, QIcon::Active);
            icon.addPixmap(pm, QIcon::Selected);

            it->setIcon(icon);
        }
        else {
            enqueueThumbRequest(it, s.firstFile);
        }

    }

    startNextThumb();
}


void SeriesListPanel::ensureWorker()
{
    if (mScanWorker) return;

    qRegisterMetaType<SeriesScanResult>("SeriesScanResult");

    mScanThread = new QThread(this);
    mScanWorker = new SeriesScanWorker();
    mScanWorker->moveToThread(mScanThread);

    connect(mScanThread, &QThread::finished,
        mScanWorker, &QObject::deleteLater);

    connect(mScanWorker, &QObject::destroyed, this, [this]() {
        mScanWorker = nullptr;
        });

    auto* w = static_cast<SeriesScanWorker*>(mScanWorker);

    connect(w, &SeriesScanWorker::scanStarted,
        this, &SeriesListPanel::scanStarted);

    connect(w, &SeriesScanWorker::scanProgress,
        this, [this](int processed, int totalFiles, const QString& currentPath) {
            emit scanProgress(processed, totalFiles, currentPath);
        });

    connect(w, &SeriesScanWorker::scanCompleted,
        this, &SeriesListPanel::handleScanResult);

    mScanThread->start();
}

void SeriesListPanel::abortThumbLoading()
{
    mPendingThumbs.clear();
    mCurrentThumbItem = nullptr;
    mCurrentThumbFile.clear();

    if (mThumbWatcher.isRunning()) {
        mThumbWatcher.cancel();
        mThumbWatcher.waitForFinished();
    }
}

void SeriesListPanel::enqueueThumbRequest(QListWidgetItem* item, const QString& filePath)
{
    if (!item || filePath.isEmpty()) return;
    mPendingThumbs.enqueue({ item, filePath });
}

void SeriesListPanel::startNextThumb()
{
    if (mThumbWatcher.isRunning() || mPendingThumbs.isEmpty()) return;

    const auto next = mPendingThumbs.dequeue();
    mCurrentThumbItem = next.first;
    mCurrentThumbFile = next.second;

    if (mCurrentThumbFile.isEmpty()) {
        mCurrentThumbItem = nullptr;
        mCurrentThumbFile.clear();
        startNextThumb();
        return;
    }

    auto future = QtConcurrent::run([file = mCurrentThumbFile]() { return SeriesListPanel::makeThumbImageFromDicom(file); });
    mThumbWatcher.setFuture(future);
}

void SeriesListPanel::updatePatientInfoForSeries(const QString& seriesKey,
    const QVector<QString>& files)
{
    Q_UNUSED(seriesKey);

    if (!mHasBasePatientInfo || files.isEmpty())
        return;

    const QString firstFile = files.first();

    auto r = vtkSmartPointer<vtkDICOMReader>::New();
    r->SetFileName(firstFile.toUtf8().constData());
    r->UpdateInformation();
    vtkDICOMMetaData* md = r->GetMetaData();
    if (!md) return;

    PatientInfo info = mBasePatientInfo; // копия демографии

    // --- тип (Modality) ---
    if (md->Has(DC::Modality))
        info.Mode = QString::fromUtf8(md->Get(DC::Modality).AsString()).trimmed();
    else
        info.Mode.clear();

    // --- описание серии ---
    QString desc;
    if (md->Has(DC::SeriesDescription))
        desc = QString::fromUtf8(md->Get(DC::SeriesDescription).AsString()).trimmed();

    if (desc.isEmpty()) {
        QString modality = md->Has(DC::Modality)
            ? QString::fromUtf8(md->Get(DC::Modality).AsString())
            : QString();
        QString snum = md->Has(DC::SeriesNumber)
            ? QString::fromUtf8(md->Get(DC::SeriesNumber).AsString())
            : QString();
        desc = (modality.isEmpty() && snum.isEmpty())
            ? tr("(unnamed series)")
            : QStringLiteral("%1 %2").arg(modality, snum);
    }
    info.Description = desc;

    // --- sequence ---
    QString seq;
    if (md->Has(DC::SequenceName))
        seq = QString::fromUtf8(md->Get(DC::SequenceName).AsString());
    else if (md->Has(DC::ProtocolName))
        seq = QString::fromUtf8(md->Get(DC::ProtocolName).AsString());
    else if (md->Has(DC::SeriesNumber))
        seq = QString::fromUtf8(md->Get(DC::SeriesNumber).AsString());

    info.Sequence = seq.trimmed();

    emit patientInfoChanged(info);
}

void SeriesListPanel::handleScanResult(const SeriesScanResult& result)
{

    if (result.canceled) {
        emit scanFinished(result.filesBySeries.size(), result.totalFiles);
        return;
    }

    mFilesBySeries = result.filesBySeries;
    populate(result.items);

    if (result.patientInfoValid) {
        mBasePatientInfo = result.patientInfo;
        mHasBasePatientInfo = true;
        emit patientInfoChanged(result.patientInfo);
    }

    emit scanFinished(mFilesBySeries.size(), result.totalFiles);
}

void SeriesListPanel::handleThumbReady()
{
    if (mThumbWatcher.future().isCanceled()) {
        mCurrentThumbItem = nullptr;
        mCurrentThumbFile.clear();
        startNextThumb();
        return;
    }

    if (mCurrentThumbItem && mList->indexFromItem(mCurrentThumbItem).isValid()) {
        const QImage image = mThumbWatcher.result();
        if (!image.isNull()) {
            QImage scaled = image.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap pm = QPixmap::fromImage(scaled);

            QIcon icon;
            icon.addPixmap(pm, QIcon::Normal);
            icon.addPixmap(pm, QIcon::Active);
            icon.addPixmap(pm, QIcon::Selected);

            mCurrentThumbItem->setIcon(icon);
        }
    }


    mCurrentThumbItem = nullptr;
    mCurrentThumbFile.clear();
    startNextThumb();
}

#include "SeriesListPanel.moc"

