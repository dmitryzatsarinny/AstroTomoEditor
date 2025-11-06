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

#include <vtkDICOMMetaData.h>
#include <vtkDICOMParser.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMValue.h>
#include <vtkImageData.h>
#include <vtkImageShiftScale.h>
#include <vtkSmartPointer.h>
#include <vtk-9.5/vtkGDCMImageReader.h>

//static QString strOrEmpty(const char* cstr) { return cstr ? QString::fromUtf8(cstr) : QString(); }

static bool isLikelyDicomPath(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("dcm") || ext == QLatin1String("dicom") || ext == QLatin1String("ima"))
        return true;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (f.size() < 132) return false;
    if (!f.seek(128)) return false;
    char sig[4] = {};
    return f.read(sig, 4) == 4 && ::memcmp(sig, "DICM", 4) == 0;
}

struct SeriesScanResult
{
    QHash<QString, QVector<QString>> filesBySeries;
    QVector<SeriesItem> items;
    PatientInfo patientInfo;
    bool patientInfoValid = false;
    int totalFiles = 0;
    bool canceled = false;
};

Q_DECLARE_METATYPE(SeriesScanResult)

class SeriesScanWorker : public QObject
{
    Q_OBJECT
public:
    explicit SeriesScanWorker(QObject* parent = nullptr) : QObject(parent) {}

public slots:
    void startScan(const QString& rootPath, const QString& dicomdirPath);
    void cancel() { mCancelRequested.store(true, std::memory_order_relaxed); }

signals:
    void scanStarted(int totalFiles);
    void scanProgress(int processed, int totalFiles, const QString& currentPath);
    void scanCompleted(const SeriesScanResult& result);

private:
    bool shouldCancel() const { return mCancelRequested.load(std::memory_order_relaxed); }
    SeriesScanResult runScan(const QString& rootPath);

    std::atomic_bool mCancelRequested{ false };
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
        emit seriesActivated(key, mFilesBySeries.value(key));
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

static QString pickMiddleByName(const QVector<QString>& files)
{
    if (files.isEmpty()) return {};
    QVector<QString> sorted = files;
    QCollator coll; coll.setNumericMode(true); coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(sorted.begin(), sorted.end(),
        [&](const QString& a, const QString& b) { return coll.compare(a, b) < 0; });
    return sorted[(sorted.size() - 1) / 2];
}

static QString pickMiddleSliceFile(const QVector<QString>& files)
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

    for (const QString& f : files) {
        auto r = vtkSmartPointer<vtkDICOMReader>::New();
        r->SetFileName(f.toUtf8().constData());
        r->UpdateInformation();
        vtkDICOMMetaData* md = r->GetMetaData();

        Key k; k.path = f; k.name = QFileInfo(f).fileName();

        // Z из ImagePositionPatient[2]
        if (md && md->Has(DC::ImagePositionPatient)) {
            const vtkDICOMValue& v = md->Get(DC::ImagePositionPatient);
            if (v.GetNumberOfValues() >= 3) {
                k.z = v.GetDouble(2);
                k.hasZ = true;
            }
        }
        // InstanceNumber
        if (md && md->Has(DC::InstanceNumber)) {
            k.num = md->Get(DC::InstanceNumber).AsInt();
            k.hasNum = true;
        }
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
    PatientInfo pinfo;
    pinfo.patientName = DicomParcer::normalizePN(qstr(md->Get(DC::PatientName)));
    {
        QString pid = qstr(md->Get(DC::PatientID));
        if (pid.isEmpty()) pid = qstr(md->Get(DC::StudyID));
        pinfo.patientId = pid.trimmed();
    }
    pinfo.sex = DicomParcer::mapSex(qstr(md->Get(DC::PatientSex)));
    pinfo.birthDate = DicomParcer::normalizeDicomDate(qstr(md->Get(DC::PatientBirthDate)));

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

    ensureWorker();
    abortThumbLoading();

    mCancelScan = false;
    mFilesBySeries.clear();

    QMetaObject::invokeMethod(mScanWorker, "startScan", Qt::QueuedConnection,
        Q_ARG(QString, baseDir),
        Q_ARG(QString, dicomdirPath));
}


//static QImage makeThumbFrom3drMidSlice(const _3Dinfo& hdr, const QByteArray& vox, bool byteswap = false)
//{
//    const int nx = int(hdr.UIheader[0]);
//    const int ny = int(hdr.UIheader[1]);
//    const int nz = int(hdr.UIheader[2]);
//    if (nx <= 0 || ny <= 0 || nz <= 0) return QImage();
//
//    const size_t sliceStride = size_t(nx) * size_t(ny);      // семплов на срез
//    const size_t samplesAvail = size_t(vox.size()) / sizeof(uint16_t);
//    if (samplesAvail < sliceStride) return QImage();         // даже одного среза нет
//
//    // сколько срезов реально помещается в буфере
//    const int slicesAvail = int(samplesAvail / sliceStride);
//    // серединный по заголовку
//    const int zMidHdr = std::clamp(nz / 2, 0, nz - 1);
//    // но ограничим реальной доступностью
//    const int zIdx = std::clamp(zMidHdr, 0, std::max(0, slicesAvail - 1));
//
//    const uint16_t* base = reinterpret_cast<const uint16_t*>(vox.constData());
//    const uint16_t* ptr = base + size_t(zIdx) * sliceStride;
//
//    // --- авто-контраст по всему срезу (учитываем нули как фон) ---
//    uint16_t vmin = 65535, vmax = 0;
//    for (size_t i = 0; i < sliceStride; ++i) {
//        const uint16_t vv = byteswap ? bswap16(ptr[i]) : ptr[i];
//        if (vv < vmin) vmin = vv;
//        if (vv > vmax) vmax = vv;
//    }
//    if (vmin == vmax) { vmin = 0; vmax = 65535; }
//
//    // --- собираем 8-битный срез, инвертируя фон при необходимости ---
//    QImage gray(nx, ny, QImage::Format_Grayscale8);
//    for (int y = 0; y < ny; ++y) {
//        uchar* dst = gray.scanLine(y);
//        const uint16_t* src = ptr + size_t(y) * size_t(nx);
//        for (int x = 0; x < nx; ++x) {
//            uint16_t vv = byteswap ? bswap16(src[x]) : src[x];
//            // линейное растяжение
//            int v = (vmax > vmin) ? int((uint32_t(vv - vmin) * 255u) / uint32_t(vmax - vmin)) : 0;
//            // фон (vv == 0) сделать черным
//            if (vv == 0) v = 0;
//            dst[x] = uchar(v);
//        }
//    }
//
//    // --- приводим к 128×128, с сохранением пропорций ---
//    constexpr int kThumb = 128;
//    QImage canvas(kThumb, kThumb, QImage::Format_ARGB32_Premultiplied);
//    canvas.fill(QColor(22, 23, 26, 255));
//
//    const qreal sx = qreal(kThumb) / qreal(nx);
//    const qreal sy = qreal(kThumb) / qreal(ny);
//    const qreal s = std::min(sx, sy);
//
//    const int w = int(nx * s);
//    const int h = int(ny * s);
//    const int x0 = (kThumb - w) / 2;
//    const int y0 = (kThumb - h) / 2;
//
//    QPainter p(&canvas);
//    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
//    p.drawImage(QRect(x0, y0, w, h), gray);
//    p.end();
//
//    return canvas;
//}

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
    sh->SetScale(255.0 / (high - low));
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


//bool SeriesListPanel::tryParseDicomdirWithVtk(const QString& baseDir,
//    PatientInfo& pinfo,
//    QVector<SeriesItem>& items)
//{
//    auto dd = vtkSmartPointer<vtkDICOMDirectory>::New();
//    dd->SetDirectoryName(baseDir.toUtf8().constData());
//    dd->SetRequirePixelData(0);
//
//    // Временное отключение глобальных ворнингов VTK
//    const int prevWarn = vtkObject::GetGlobalWarningDisplay();
//    vtkObject::GlobalWarningDisplayOff();
//
//    dd->Update();
//
//    // Вернём исходное состояние показателей
//    if (prevWarn) vtkObject::GlobalWarningDisplayOn();
//
//    const int nSeries = dd->GetNumberOfSeries();
//    if (nSeries <= 0) return false;
//
//    // Подсчёт файлов для прогресса
//    int totalFiles = 0;
//    for (int s = 0; s < nSeries; ++s) {
//        if (auto* arr = dd->GetFileNamesForSeries(s))
//            totalFiles += static_cast<int>(arr->GetNumberOfValues());
//    }
//    emit scanStarted(totalFiles);
//
//    int processed = 0;
//    bool pinfoFilled = false;
//
//    for (int s = 0; s < nSeries && !mCancelScan; ++s)
//    {
//        vtkStringArray* files = dd->GetFileNamesForSeries(s);
//        if (!files || files->GetNumberOfValues() == 0) continue;
//
//        QVector<QString> fvec;
//        fvec.reserve(static_cast<int>(files->GetNumberOfValues()));
//
//        // фильтруем до валидных изображений
//        for (vtkIdType i = 0; i < files->GetNumberOfValues() && !mCancelScan; ++i) {
//            QString rel = QString::fromUtf8(files->GetValue(i).c_str());
//            rel = QDir::fromNativeSeparators(rel);
//            if (rel.isEmpty()) { ++processed; continue; }
//
//            QString abs = QDir(baseDir).filePath(rel);
//            abs = QDir::cleanPath(abs);
//
//            auto r = vtkSmartPointer<vtkDICOMReader>::New();
//            r->SetFileName(abs.toUtf8().constData());
//            if (r->CanReadFile(r->GetFileName()) == 0) { ++processed; continue; }
//
//            r->UpdateInformation();
//            vtkDICOMMetaData* md = r->GetMetaData();
//            if (!md) { ++processed; continue; }
//
//            const bool hasRows = md->Has(DC::Rows);
//            const bool hasCols = md->Has(DC::Columns);
//            const bool hasPixel = md->Has(DC::PixelData) ||
//                md->Has(DC::FloatPixelData) ||
//                md->Has(DC::DoubleFloatPixelData);
//            if (!(hasRows && hasCols && hasPixel)) { ++processed; continue; }
//
//            fvec.push_back(abs);
//
//            emit scanProgress(processed, totalFiles, abs);
//            if ((++processed % 16) == 0)
//                qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
//        }
//
//        if (fvec.isEmpty()) continue;
//
//        const QString repFile = pickMiddleByName(fvec);
//        auto r = vtkSmartPointer<vtkDICOMReader>::New();
//        r->SetFileName(repFile.toUtf8().constData());
//        r->UpdateInformation();
//        vtkDICOMMetaData* md = r->GetMetaData();
//        if (!md) continue;
//
//        QString seriesUID = qstr(md->Get(DC::SeriesInstanceUID));
//        if (seriesUID.isEmpty()) seriesUID = QStringLiteral("SERIES_%1").arg(s);
//
//        mFilesBySeries[seriesUID] = fvec;
//
//        SeriesItem si;
//        si.seriesKey = seriesUID;
//        si.firstFile = repFile;
//        si.numImages = fvec.size();
//        si.studyID = qstr(md->Get(DC::StudyInstanceUID));
//
//        QString desc = qstr(md->Get(DC::SeriesDescription));
//        if (desc.isEmpty()) {
//            const QString modality = qstr(md->Get(DC::Modality));
//            const QString snum = qstr(md->Get(DC::SeriesNumber));
//            desc = (modality.isEmpty() && snum.isEmpty())
//                ? tr("(без названия)")
//                : QString("%1 %2").arg(modality, snum);
//        }
//        si.description = desc;
//        si.thumb = makeThumbImageFromDicom(repFile);
//        items.push_back(std::move(si));
//
//        if (!pinfoFilled) {
//            QString name = qstr(md->Get(DC::PatientName));
//            QString pid = qstr(md->Get(DC::PatientID));
//            if (pid.isEmpty()) pid = qstr(md->Get(DC::StudyID));
//            QString sex = qstr(md->Get(DC::PatientSex));
//            QString bday = qstr(md->Get(DC::PatientBirthDate));
//            pinfo.patientName = DicomParcer::normalizePN(name);
//            pinfo.patientId = pid.trimmed();
//            pinfo.sex = DicomParcer::mapSex(sex);
//            pinfo.birthDate = DicomParcer::normalizeDicomDate(bday);
//            pinfoFilled = true;
//        }
//    }
//
//    return true;
//}

//void SeriesListPanel::scanDicomDir(const QString& rootPath)
//{
//    mCancelScan = false;
//    mFilesBySeries.clear();
//
//    QFileInfo fi(rootPath);
//    const QString baseDir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
//
//    // ищем файл каталога
//    auto findCatalog = [&](const QString& dir) -> QString {
//        static const char* names[] = { "DICOMDIR", "dicomdir", "DICOMDIR;1", "DIRFILE", "dirfile" };
//        for (auto n : names) {
//            const QString p = QDir(dir).filePath(QString::fromLatin1(n));
//            if (QFileInfo::exists(p)) return p;
//        }
//        return {};
//        };
//    const QString cat = fi.isFile() ? fi.absoluteFilePath() : findCatalog(baseDir);
//    if (cat.isEmpty()) { scanStudy(baseDir); return; }
//
//    // если это не DICOMDIR по имени — проверим содержимое
//    QString dicomdirPath = cat;
//    bool removeAlias = false;
//    const QString fn = QFileInfo(cat).fileName();
//    const bool nameOk = fn.compare("DICOMDIR", Qt::CaseInsensitive) == 0
//        || fn.compare("DICOMDIR;1", Qt::CaseInsensitive) == 0;
//
//    if (!nameOk) {
//        if (!DicomParcer::looksLikeDicomDirDataset(cat)) {
//            scanStudy(baseDir); return; // проприетарный индекс → папка
//        }
//        auto [alias, created] = DicomParcer::ensureDicomdirAlias(cat, baseDir);
//        if (alias.isEmpty()) { scanStudy(baseDir); return; }
//        dicomdirPath = alias;
//        removeAlias = created;
//    }
//
//    // проприетарный или битый каталог — надёжный рекурсивный проход
//    scanStudy(baseDir);
//}

//bool SeriesListPanel::isLikelyDicom(const QString& path)
//{
//    // 1) по расширению
//    const QString ext = QFileInfo(path).suffix().toLower();
//    if (ext == "dcm") return true;
//
//    // 2) по магической подписи "DICM" на смещении 128
//    QFile f(path);
//    if (!f.open(QIODevice::ReadOnly)) return false;
//    if (f.size() < 132) return false;
//    f.seek(128);
//    char sig[4] = {};
//    return f.read(sig, 4) == 4 && ::memcmp(sig, "DICM", 4) == 0;
//}

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


//struct MiniMeta { QString seriesUID; bool ok; };
//
//static MiniMeta readMiniMeta(const QString& fp)
//{
//    // DICM — лишь подсказка; если нет — всё равно пробуем парсер
//    {
//        QFile f(fp);
//        if (!f.open(QIODevice::ReadOnly)) return { "", false };
//        if (f.size() >= 132) {
//            f.seek(128); char magic[4];
//            if (f.read(magic, 4) == 4 && ::memcmp(magic, "DICM", 4) == 0) {
//                // ок, вероятный dicom
//            }
//        }
//    }
//
//    auto p = vtkSmartPointer<vtkDICOMParser>::New();
//    auto d = vtkSmartPointer<vtkDICOMMetaData>::New();
//    p->SetMetaData(d);
//    p->SetFileName(fp.toUtf8().constData());
//    p->Update(); // парсит заголовок; пиксели не грузит
//
//    // минимум валидности: наличие размеров или PixelData-тегов (не строгая проверка)
//    const bool hasSomething =
//        d->Has(DC::Rows) || d->Has(DC::Columns) ||
//        d->Has(DC::PixelData) || d->Has(DC::FloatPixelData) || d->Has(DC::DoubleFloatPixelData);
//
//    if (!hasSomething) return { "", false };
//
//    // ключ серии (реальный или композитный)
//    const QString key = makeSeriesKey(d);
//    return { key, !key.isEmpty() };
//}

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
void SeriesScanWorker::startScan(const QString& rootPath, const QString& dicomdirPath)
{
    Q_UNUSED(dicomdirPath);
    mCancelRequested.store(false, std::memory_order_relaxed);

    SeriesScanResult result = runScan(rootPath);
    emit scanCompleted(result);
}

SeriesScanResult SeriesScanWorker::runScan(const QString& rootPath)
{
    SeriesScanResult result;

    QFileInfo fi(rootPath);
    const QString baseDir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

    QVector<QString> candidates;
    candidates.reserve(maxfiles);

    int seen = 0; // ▼ счётчик просмотренных файлов

    // Pass 1: кандидаты
    for (QDirIterator it(baseDir, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        if (shouldCancel()) { result.canceled = true; break; }

        const QString fp = it.next();
        ++seen;

        // ▼ каждые N файлов обновляем статус (totalFiles=0 -> индетерминатный режим в UI)
        if ((seen & 63) == 0) {
            emit scanProgress(seen, 0, fp);
        }

        if (isLikelyDicomPath(fp)) {
            candidates.push_back(fp);
            if (candidates.size() >= maxfiles) break;
        }
    }

    result.totalFiles = candidates.size();
    emit scanStarted(result.totalFiles);   // теперь знаем точное число

    if (shouldCancel()) {
        result.canceled = true;
        return result;
    }


    QCollator coll; coll.setNumericMode(true); coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(candidates.begin(), candidates.end(),
        [&](const QString& a, const QString& b) { return coll.compare(a, b) < 0; });

    QHash<QString, QVector<QString>> filesBySeries;
    filesBySeries.reserve(candidates.size());
    PatientInfo pinfo;
    bool pinfoFilled = false;

    auto parser = vtkSmartPointer<vtkDICOMParser>::New();

    int processed = 0;
    for (const QString& fp : candidates) {
        if (shouldCancel()) { result.canceled = true; break; }

        emit scanProgress(processed + 1, result.totalFiles, fp);

        auto meta = vtkSmartPointer<vtkDICOMMetaData>::New();
        parser->SetMetaData(meta);
        parser->SetFileName(fp.toUtf8().constData());
        parser->Update();

        const bool hasGeometry = meta->Has(DC::Rows) || meta->Has(DC::Columns);
        const bool hasPixel = meta->Has(DC::PixelData) || meta->Has(DC::FloatPixelData) || meta->Has(DC::DoubleFloatPixelData);

        if (hasGeometry || hasPixel) {
            const QString key = makeSeriesKey(meta);
            if (!key.isEmpty()) {
                filesBySeries[key].push_back(fp);
                if (!pinfoFilled && tryFillPatientInfo(fp, pinfo)) {
                    pinfoFilled = true;
                }
            }
        }

        ++processed;
    }

    if (result.canceled) {
        return result;
    }

    QVector<SeriesItem> items;
    items.reserve(filesBySeries.size());

    auto detailsParser = vtkSmartPointer<vtkDICOMParser>::New();

    for (auto it = filesBySeries.constBegin(); it != filesBySeries.constEnd(); ++it) {
        if (shouldCancel()) { result.canceled = true; break; }

        const QString seriesKey = it.key();
        QVector<QString> sorted = it.value();
        std::sort(sorted.begin(), sorted.end(),
            [&](const QString& a, const QString& b) { return coll.compare(a, b) < 0; });

        SeriesItem s;
        s.seriesKey = seriesKey;
        s.firstFile = pickMiddleByName(sorted);
        s.numImages = sorted.size();

        auto meta = vtkSmartPointer<vtkDICOMMetaData>::New();
        detailsParser->SetMetaData(meta);
        detailsParser->SetFileName(s.firstFile.toUtf8().constData());
        detailsParser->Update();

        s.studyID = QString::fromUtf8(meta->Get(DC::StudyInstanceUID).AsString());

        QString desc = QString::fromUtf8(meta->Get(DC::SeriesDescription).AsString());
        if (desc.isEmpty()) {
            const QString modality = QString::fromUtf8(meta->Get(DC::Modality).AsString());
            const QString snum = QString::fromUtf8(meta->Get(DC::SeriesNumber).AsString());
            desc = (modality.isEmpty() && snum.isEmpty())
            ? QObject::tr("(без названия)")
                : QStringLiteral("%1 %2").arg(modality, snum);
        }
        s.description = desc;

        s.thumb = SeriesListPanel::makeThumbImageFromDicom(s.firstFile);
        items.push_back(std::move(s));
    }

    if (result.canceled) {
        return result;
    }

    sortSeriesByName(items);
    result.filesBySeries = std::move(filesBySeries);
    result.items = std::move(items);
    result.patientInfo = pinfo;
    result.patientInfoValid = pinfoFilled;

    return result;
}

void SeriesListPanel::scanStudy(const QString& rootPath)
{
    ensureWorker();
    abortThumbLoading();

    mCancelScan = false;
    mFilesBySeries.clear();

    QMetaObject::invokeMethod(mScanWorker, "startScan", Qt::QueuedConnection,
        Q_ARG(QString, rootPath),
        Q_ARG(QString, QString()));
}

void SeriesListPanel::populate(const QVector<SeriesItem>& items)
{
    abortThumbLoading();
    mList->clear();

    for (const auto& s : items) {
        auto text = QString("%1\n%2 снимков").arg(s.description).arg(s.numImages);
        auto* it = new QListWidgetItem(QIcon(), text);
        it->setData(Qt::UserRole, s.seriesKey);
        it->setToolTip(s.description);
        it->setSizeHint(QSize(0, kRowH));
        mList->addItem(it);

        if (!s.thumb.isNull()) {
            QImage scaled = s.thumb.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            it->setIcon(QIcon(QPixmap::fromImage(scaled)));
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

void SeriesListPanel::handleScanResult(const SeriesScanResult& result)
{

    if (result.canceled) {
        emit scanFinished(result.filesBySeries.size(), result.totalFiles);
        return;
    }

    mFilesBySeries = result.filesBySeries;
    populate(result.items);

    if (result.patientInfoValid) {
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
            mCurrentThumbItem->setIcon(QIcon(QPixmap::fromImage(scaled)));
        }
    }

    mCurrentThumbItem = nullptr;
    mCurrentThumbFile.clear();
    startNextThumb();
}

#include "SeriesListPanel.moc"

