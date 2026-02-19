#include "VolumeStlExporter.h"
#include <vtkImageConstantPad.h>
#include <vtkFlyingEdges3D.h>
#include <vtkTriangleFilter.h>
#include <vtkCleanPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkQuadricDecimation.h>  
#include <vtkWindowedSincPolyDataFilter.h>
#include <vtkPolyDataNormals.h>
#include <vtkPointData.h>
#include <algorithm>
#include <cmath>
#include <vtkImageDilateErode3D.h>
#include <vtkImageData.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkPiecewiseFunction.h>
#include <vtkDecimatePro.h>
#include <vtkFillHolesFilter.h>
#include <vtkPolyDataMapper.h>
#include <vtkSTLWriter.h>
#include <vtkQuadricClustering.h>
#include <QCoreApplication>
#include <qfile.h>
#include <qfileinfo.h>
#include <QSaveFile>
#include <QFileInfo>
#include <QtEndian>
#include <vtkPolyData.h>
#include <vtkCell.h>
#include <vtkPoints.h>
#include <vtkIdList.h>
#include <vtkMath.h>
#include <cstring>

#pragma pack(push, 1)
typedef struct MYSTLHEADER
{
    char  stl[4];        // "STL" + '\0'
    char  unused[60];
    float zero_x;
    float zero_y;
    float zero_z;
    char  unused2[4];
} MYSTLHEADER, * LPMYSTLHEADER;
#pragma pack(pop)

static_assert(sizeof(MYSTLHEADER) == 80, "MYSTLHEADER must be exactly 80 bytes");

static void InitSTLHeader(MYSTLHEADER* ph, float zx, float zy, float zz)
{
    if (!ph) return;
    std::memset(ph, 0, sizeof(MYSTLHEADER));
    ph->stl[0] = 'S';
    ph->stl[1] = 'T';
    ph->stl[2] = 'L';
    ph->stl[3] = 0;
    ph->zero_x = zx;
    ph->zero_y = zy;
    ph->zero_z = zz;
}

static bool WriteAll(QSaveFile& f, const void* data, qsizetype size)
{
    return f.write(reinterpret_cast<const char*>(data), size) == size;
}

static inline void FloatToLE(float& v)
{
    static_assert(sizeof(float) == sizeof(quint32));
    quint32 u32;
    std::memcpy(&u32, &v, sizeof(u32));
    u32 = qToLittleEndian(u32);
    std::memcpy(&v, &u32, sizeof(u32));
}

static inline float f32(double v) { return static_cast<float>(v); }

bool VolumeStlExporter::SaveStlMyBinary_NoCenter(
    vtkPolyData* pd,
    const QString& filePath,
    double VolumeOriginX, double VolumeOriginY, double VolumeOriginZ,      // world origin (обычно vtkImageData::GetOrigin())
    double VolumeCenterX, double VolumeCenterY, double VolumeCenterZ,      // ВАЖНО: local half-size (dx/2, dy/2, dz/2) в мм
    bool recomputeNormals)
{
    if (!pd || filePath.isEmpty())
        return false;

    // --- 0) Треангуляция (на всякий случай) ---
    vtkSmartPointer<vtkPolyData> triPd = pd;
    vtkNew<vtkTriangleFilter> tri;
    if (pd->GetNumberOfPolys() > 0)
    {
        tri->SetInputData(pd);
        tri->PassLinesOff();
        tri->PassVertsOff();
        tri->Update();
        triPd = tri->GetOutput();
    }

    if (!triPd || triPd->GetNumberOfCells() == 0 || !triPd->GetPoints())
        return false;

    // --- 1) Bounds меша в world (как пришло из BuildFromBinaryVoxelsNew) ---
    double b[6];
    triPd->GetBounds(b);
    if (!std::isfinite(b[0]) || !std::isfinite(b[1]) ||
        !std::isfinite(b[2]) || !std::isfinite(b[3]) ||
        !std::isfinite(b[4]) || !std::isfinite(b[5]))
        return false;

    const double meshCx = 0.5 * (b[0] + b[1]);
    const double meshCy = 0.5 * (b[2] + b[3]);
    const double meshCz = 0.5 * (b[4] + b[5]);

    const double sx = (b[1] - b[0]);
    const double sy = (b[3] - b[2]);
    const double sz = (b[5] - b[4]);

    // --- 2) Центр объёма в world: origin + localHalfSize ---
    // localHalfSize = (size/2) в мм. НЕ size.
    const double centerWorldX = VolumeOriginX + VolumeCenterX;
    const double centerWorldY = VolumeOriginY + VolumeCenterY;
    const double centerWorldZ = VolumeOriginZ + VolumeCenterZ;

    // --- 3) Диагностика ---
    qDebug() << "SaveSTL file:" << filePath;
    qDebug() << "Volume origin (world):" << VolumeOriginX << VolumeOriginY << VolumeOriginZ;
    qDebug() << "Volume center LOCAL (half-size mm):" << VolumeCenterX << VolumeCenterY << VolumeCenterZ;
    qDebug() << "Anchor / centerWorld (origin+localCenter):"
        << centerWorldX << centerWorldY << centerWorldZ;


    // --- 4) Считаем треугольники ---
    const vtkIdType cellCount = triPd->GetNumberOfCells();
    quint32 triCount = 0;
    for (vtkIdType ci = 0; ci < cellCount; ++ci)
    {
        vtkCell* c = triPd->GetCell(ci);
        if (c && c->GetNumberOfPoints() == 3)
            ++triCount;
    }
    if (triCount == 0)
        return false;

    qDebug() << "Cells total:" << cellCount
        << "Points total:" << triPd->GetNumberOfPoints()
        << "Triangles:" << triCount;

    // --- 5) Пишем файл ---
    QSaveFile out(filePath);
    if (!out.open(QIODevice::WriteOnly))
        return false;

    // В хедер пишем anchor = VolumeOrigin + VolumeCenterLocal (world-центр объёма)
    MYSTLHEADER h{};
    InitSTLHeader(&h, float(VolumeCenterX), float(VolumeCenterZ), float(VolumeCenterY));

    if (!WriteAll(out, &h, sizeof(h))) return false;

    const quint32 triCountLE = qToLittleEndian(triCount);
    if (!WriteAll(out, &triCountLE, sizeof(triCountLE))) return false;

    vtkPoints* pts = triPd->GetPoints();
    const quint16 attrLE = qToLittleEndian<quint16>(0);

    double p0[3], p1[3], p2[3];
    double n[3];

    bool firstPrinted = false;

    for (vtkIdType ci = 0; ci < cellCount; ++ci)
    {
        vtkCell* c = triPd->GetCell(ci);
        if (!c || c->GetNumberOfPoints() != 3)
            continue;

        const vtkIdType id0 = c->GetPointId(0);
        const vtkIdType id1 = c->GetPointId(1);
        const vtkIdType id2 = c->GetPointId(2);

        pts->GetPoint(id0, p0);
        pts->GetPoint(id1, p1);
        pts->GetPoint(id2, p2);

        if (!firstPrinted)
        {
            qDebug() << "First p0 world:" << p0[0] << p0[1] << p0[2];
        }

        // --- 6) Центрируем относительно centerWorld: в STL координаты станут локальными вокруг (0,0,0) ---
        const double cp0[3]{ p0[0] - centerWorldX, p0[1] - centerWorldY, p0[2] - centerWorldZ };
        const double cp1[3]{ p1[0] - centerWorldX, p1[1] - centerWorldY, p1[2] - centerWorldZ };
        const double cp2[3]{ p2[0] - centerWorldX, p2[1] - centerWorldY, p2[2] - centerWorldZ };

        if (!firstPrinted)
        {
            qDebug() << "First p0 centered:" << cp0[0] << cp0[1] << cp0[2];
            firstPrinted = true;
        }

        // --- 7) Нормаль (в локальных координатах тоже ок) ---
        if (recomputeNormals)
        {
            double u[3]{ cp1[0] - cp0[0], cp1[1] - cp0[1], cp1[2] - cp0[2] };
            double v[3]{ cp2[0] - cp0[0], cp2[1] - cp0[1], cp2[2] - cp0[2] };
            vtkMath::Cross(u, v, n);
            if (vtkMath::Normalize(n) == 0.0)
                n[0] = n[1] = n[2] = 0.0;
        }
        else
        {
            n[0] = n[1] = n[2] = 0.0;
        }

        float block[12] = {
            f32(n[0]),   f32(n[1]),   f32(n[2]),
            f32(cp0[0]), f32(cp0[1]), f32(cp0[2]),
            f32(cp1[0]), f32(cp1[1]), f32(cp1[2]),
            f32(cp2[0]), f32(cp2[1]), f32(cp2[2])
        };

        // STL binary -> little-endian
        for (int i = 0; i < 12; ++i)
        {
            quint32 u32;
            static_assert(sizeof(float) == sizeof(quint32));
            std::memcpy(&u32, &block[i], sizeof(u32));
            u32 = qToLittleEndian(u32);
            std::memcpy(&block[i], &u32, sizeof(u32));
        }

        if (!WriteAll(out, block, sizeof(block)))
            return false;

        if (!WriteAll(out, &attrLE, sizeof(attrLE)))
            return false;
    }

    return out.commit();
}

QString VolumeStlExporter::prettyBytes(quint64 bytes)
{
    const double b = double(bytes);
    if (b < 1024.0) return QString::number(bytes) + " B";
    if (b < 1024.0 * 1024.0) return QString::number(b / 1024.0, 'f', 2) + " KB";
    if (b < 1024.0 * 1024.0 * 1024.0) return QString::number(b / (1024.0 * 1024.0), 'f', 2) + " MB";
    return QString::number(b / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
}

// Быстрый путь: если mesh после TriangleFilter — polys == triangles.
// Если не уверен, можно сделать "медленный" подсчет, но он может быть дорогим.
quint64 VolumeStlExporter::estimateBinaryStlBytesFast(vtkPolyData* pd)
{
    if (!pd) return 0;
    const quint64 tri = quint64(pd->GetNumberOfPolys());
    return 84ull + 50ull * tri;
}

QString VolumeStlExporter::makeStlSizeText(vtkPolyData* pd)
{
    if (!pd) return QString();
    const quint64 tri = quint64(pd->GetNumberOfPolys());
    const quint64 bytes = 84ull + 50ull * tri;
    return QString("~%1\n(%2 tris)").arg(prettyBytes(bytes)).arg(tri);
}


static vtkSmartPointer<vtkImageData> CloseAndPadMask(vtkImageData* mask, int radius = 0)
{
    if (!mask) return nullptr;

    // 1) Закрытие: dilate(255) -> erode(0)
    vtkNew<vtkImageDilateErode3D> dil;
    dil->SetInputData(mask);
    dil->SetKernelSize(2 * radius + 1, 2 * radius + 1, 2 * radius + 1);
    dil->SetDilateValue(255);
    dil->SetErodeValue(0);

    vtkNew<vtkImageDilateErode3D> ero;
    ero->SetInputConnection(dil->GetOutputPort());
    ero->SetKernelSize(2 * radius + 1, 2 * radius + 1, 2 * radius + 1);
    ero->SetDilateValue(255);
    ero->SetErodeValue(0);

    // 2) Паддинг нулями по 1 вокселу
    int ext[6]; mask->GetExtent(ext);
    vtkNew<vtkImageConstantPad> pad;
    pad->SetInputConnection(ero->GetOutputPort());
    pad->SetConstant(0);
    pad->SetOutputWholeExtent(ext[0] - 1, ext[1] + 1,
        ext[2] - 1, ext[3] + 1,
        ext[4] - 1, ext[5] + 1);

    pad->Update();
    return pad->GetOutput();
}

static inline std::int64_t EstimateBinaryStlSizeBytesTriangulated(vtkPolyData* pd)
{
    if (!pd) return 0;

    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputData(pd);
    tri->PassLinesOff();
    tri->PassVertsOff();
    tri->Update();

    auto* out = tri->GetOutput();
    const std::int64_t tris = static_cast<std::int64_t>(out ? out->GetNumberOfPolys() : 0);
    return 84 + 50 * tris;
}


static inline double Clamp01(double v) { return std::clamp(v, 0.0, 0.99); }

// рассчитывает targetReduction так, чтобы попасть в targetBytes (примерно)
static inline double ComputeReductionForTargetBytes(vtkPolyData* pd, std::int64_t targetBytes)
{
    if (!pd) return 0.0;
    const std::int64_t curCells = static_cast<std::int64_t>(pd->GetNumberOfCells());
    if (curCells <= 0) return 0.0;

    const std::int64_t curBytes = EstimateBinaryStlSizeBytesTriangulated(pd);
    if (curBytes <= targetBytes) return 0.0; // уже меньше цели

    // сколько треугольников хотим получить
    const std::int64_t targetTris = std::max<std::int64_t>(1, (targetBytes - 84) / 50);
    const double keep = static_cast<double>(targetTris) / static_cast<double>(curCells);
    const double reduction = 1.0 - keep;

    // чуть ограничим, чтобы DecimatePro не сходил с ума за один шаг
    return Clamp01(reduction);
}

vtkSmartPointer<vtkPolyData> VolumeStlExporter::NormalizeSurface(vtkPolyData* in)
{
    if (!in || in->GetNumberOfCells() == 0) return nullptr;

    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputData(in);
    tri->PassLinesOff();
    tri->PassVertsOff();
    tri->Update();

    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputConnection(tri->GetOutputPort());
    clean->PointMergingOn();
    clean->ConvertLinesToPointsOff();
    clean->ConvertPolysToLinesOff();
    clean->ConvertStripsToPolysOff();
    clean->Update();

    vtkNew<vtkPolyDataConnectivityFilter> conn;
    conn->SetInputConnection(clean->GetOutputPort());
    conn->SetExtractionModeToLargestRegion();
    conn->Update();

    // нормали тут НЕ обязательны, можно не считать
    // но если хочешь, чтобы сразу в превью было красиво:
    vtkNew<vtkPolyDataNormals> nrm;
    nrm->SetInputConnection(conn->GetOutputPort());
    nrm->ComputePointNormalsOn();
    nrm->ComputeCellNormalsOff();
    nrm->SplittingOff();
    nrm->ConsistencyOn();
    nrm->AutoOrientNormalsOn();
    nrm->Update();

    vtkSmartPointer<vtkPolyData> out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(nrm->GetOutput());
    return out;
}


// “попасть в цель” за 1-3 шага (PreserveTopology иногда не дает ровно дойти)
vtkSmartPointer<vtkPolyData> VolumeStlExporter::SimplifyToTargetBytes(
    vtkPolyData* in,
    std::int64_t targetBytes,
    int smoothIter,
    double passBand)
{
    if (!in || in->GetNumberOfCells() == 0)
        return nullptr;

    vtkSmartPointer<vtkPolyData> cur = in;

    // максимум 3 попытки поджаться ближе к цели
    for (int i = 0; i < 3; ++i)
    {
        const std::int64_t curBytes = EstimateBinaryStlSizeBytesTriangulated(cur);
        if (curBytes <= targetBytes)
            break;

        double red = ComputeReductionForTargetBytes(cur, targetBytes);

        // если расчет дал почти 0, но размер все еще больше цели, значит уперлись в preserve topology
        if (red < 0.01)
            red = 0.10; // легкий пинок

        // чтобы не убить форму: не делаем супер-агрессивно за раз
        red = std::clamp(red, 0.05, 0.85);

        cur = SimplifySurface(cur, red, smoothIter, passBand);
        if (!cur || cur->GetNumberOfCells() == 0)
            return nullptr;
    }

    return cur;
}


static inline QString tr(const char* s)
{
    return QCoreApplication::translate("VolumeStlExporter", s);
}

// --- Быстрая бинарная маска: alpha(v) > thr ? 255 : 0 (через LUT) ---
static vtkSmartPointer<vtkImageData> BuildVisibleMaskLUT(
    vtkImageData* image, vtkVolume* volumeWithTF, double alphaThr, const VisibleExportOptions* popt = nullptr)
{
    if (!image || !volumeWithTF) return nullptr;

    auto* prop = volumeWithTF->GetProperty();
    auto* otf = prop ? prop->GetScalarOpacity(0) : nullptr;
    if (!otf) return nullptr;

    int ext[6]; image->GetExtent(ext);

    vtkNew<vtkImageData> mask;
    mask->SetExtent(ext);
    mask->SetSpacing(image->GetSpacing());
    mask->SetOrigin(image->GetOrigin());
#if VTK_MAJOR_VERSION >= 9
    mask->SetDirectionMatrix(image->GetDirectionMatrix());
#endif
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    void* inPtr = image->GetScalarPointer();
    auto* out = static_cast<unsigned char*>(mask->GetScalarPointer());
    if (!inPtr || !out) return nullptr;

    double range[2]{ 0, 255 };
    if (popt && popt->progress) popt->progress(10, tr("Computing range"));

    constexpr int BINS = 1024;
    std::vector<unsigned char> lut(BINS);
    const double thr = std::clamp(alphaThr, 0.0, 1.0);
    for (int i = 0; i < BINS; ++i) {
        const double v = range[0] + (range[1] - range[0]) * (double(i) / (BINS - 1));
        lut[i] = (otf->GetValue(v) > thr) ? 255 : 0;
    }
    if (popt && popt->progress) popt->progress(15, tr("Preparing LUT"));

    const double scale = (range[1] > range[0]) ? double(BINS - 1) / (range[1] - range[0]) : 0.0;

    const vtkIdType N = static_cast<vtkIdType>(
        (ext[1] - ext[0] + 1) * vtkIdType(ext[3] - ext[2] + 1) * vtkIdType(ext[5] - ext[4] + 1));

    auto classify = [&](auto* src)
        {
            using T = std::remove_pointer_t<decltype(src)>;
#pragma omp parallel for schedule(static)
            for (vtkIdType i = 0; i < N; ++i) {
                const double val = static_cast<double>(src[i]);
                int bin = static_cast<int>((val - range[0]) * scale);
                if (bin < 0) bin = 0; else if (bin >= BINS) bin = BINS - 1;
                out[i] = lut[bin];
            }
        };

    switch (image->GetScalarType()) {
    case VTK_UNSIGNED_CHAR: classify(static_cast<const unsigned char*>(inPtr)); break;
    case VTK_CHAR:          classify(static_cast<const signed char*>(inPtr));   break;
    case VTK_UNSIGNED_SHORT:classify(static_cast<const unsigned short*>(inPtr)); break;
    case VTK_SHORT:         classify(static_cast<const short*>(inPtr));         break;
    case VTK_INT:           classify(static_cast<const int*>(inPtr));           break;
    case VTK_UNSIGNED_INT:  classify(static_cast<const unsigned int*>(inPtr));  break;
    case VTK_FLOAT:         classify(static_cast<const float*>(inPtr));         break;
    case VTK_DOUBLE:        classify(static_cast<const double*>(inPtr));        break;
    default:
        if (auto* arr = image->GetPointData()->GetScalars()) {
#pragma omp parallel for schedule(static)
            for (vtkIdType i = 0; i < arr->GetNumberOfTuples(); ++i) {
                const double val = arr->GetComponent(i, 0);
                int bin = static_cast<int>((val - range[0]) * scale);
                if (bin < 0) bin = 0; else if (bin >= BINS) bin = BINS - 1;
                out[i] = lut[bin];
            }
        }
        break;
    }

    if (popt && popt->progress) popt->progress(30, tr("Mask classified"));
    return mask;
}


// --- Извлечь ОДНУ внешнюю оболочку из бинарной маски ---
static vtkSmartPointer<vtkPolyData> ExtractLargestFromMask(
    vtkImageData* mask, const VisibleExportOptions& opt)
{
    if (!mask) return nullptr;

    const double isoMask = 127.5; // порог между 0 и 255

    // 1) Изо-поверхность
    vtkNew<vtkFlyingEdges3D> fe;
    fe->SetInputData(mask);
    fe->SetValue(0, isoMask);
    fe->ComputeNormalsOff();
    fe->ComputeGradientsOff();
    fe->ComputeScalarsOff();
    if (opt.progress) opt.progress(57, tr("Marching cubes"));

    // 2) Треугольники + чистка
    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputConnection(fe->GetOutputPort());
    tri->PassLinesOff();
    tri->PassVertsOff();

    vtkAlgorithmOutput* current = tri->GetOutputPort();

    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputConnection(current);
    clean->PointMergingOn();
    current = clean->GetOutputPort();
    if (opt.progress) opt.progress(70, tr("Clean triangles"));

    // 3) (опц.) сглаживание
    vtkAlgorithmOutput* afterSmooth = current;
    vtkNew<vtkWindowedSincPolyDataFilter> smooth;
    if (opt.smoothIterations > 0) {
        smooth->SetInputConnection(current);
        smooth->SetNumberOfIterations(std::max(0, opt.smoothIterations));
        smooth->BoundarySmoothingOff();
        smooth->FeatureEdgeSmoothingOff();
        smooth->SetFeatureAngle(120.0);
        smooth->SetPassBand(std::max(0.0, opt.smoothPassBand));
        smooth->NonManifoldSmoothingOn();
        smooth->NormalizeCoordinatesOn();
        afterSmooth = smooth->GetOutputPort();
        if (opt.progress) opt.progress(78, tr("Smoothing"));
    }

    // 4) (опц.) упрощение
    vtkAlgorithmOutput* afterDec = afterSmooth;
    vtkNew<vtkDecimatePro> dec;
    if (opt.decimate > 0.0) {
        dec->SetInputConnection(afterSmooth);
        dec->SetTargetReduction(std::clamp(opt.decimate, 0.0, 0.9));
        dec->PreserveTopologyOn();
        dec->SplittingOff();
        dec->BoundaryVertexDeletionOff();
        dec->SetFeatureAngle(120.0);
        afterDec = dec->GetOutputPort();
        if (opt.progress) opt.progress(85, tr("Decimation"));
    }

    // 5) Закрыть отверстия (умеренно, без тяжёлой валидации)
    vtkAlgorithmOutput* afterDecOrFill = afterDec;
    vtkNew<vtkFillHolesFilter> fill;
    fill->SetInputConnection(afterDec);
    fill->SetHoleSize(1e6);
    afterDecOrFill = fill->GetOutputPort();
    if (opt.progress) opt.progress(88, tr("Fill holes"));

    // 6) Взять крупнейшую компоненту
    vtkNew<vtkPolyDataConnectivityFilter> conn;
    conn->SetInputConnection(afterDecOrFill);
    conn->SetExtractionModeToLargestRegion();
    conn->Update();
    if (opt.progress) opt.progress(92, tr("Largest component"));

    // 7) Возврат без дополнительной проверки FeatureEdges и без нормалей
    // (нормали считаются при визуализации в MakeSurfaceActor)
    return conn->GetOutput();
}

#include <vtkImageCast.h>
#include <vtkExtractVOI.h>
#include <vtkImageThreshold.h>
#include <vtkImageEuclideanDistance.h>
#include <vtkImageMathematics.h>
#include <vtkImageGaussianSmooth.h>
#include <limits>
#include <vtkBox.h>
#include <vtkClipPolyData.h>
#include <vtkImageShrink3D.h>

namespace
{
    // Возвращает false, если маска пустая.
    static bool ComputeNonZeroBBoxU8_Safe(vtkImageData* imgU8, int bbox[6])
    {
        if (!imgU8) return false;

        // На всякий: приводим к 1 компоненте ожидание
        const int comps = imgU8->GetNumberOfScalarComponents();
        if (comps < 1) return false;

        int ext[6];
        imgU8->GetExtent(ext);

        int minI = ext[1], minJ = ext[3], minK = ext[5];
        int maxI = ext[0], maxJ = ext[2], maxK = ext[4];
        bool any = false;

        for (int k = ext[4]; k <= ext[5]; ++k)
            for (int j = ext[2]; j <= ext[3]; ++j)
                for (int i = ext[0]; i <= ext[1]; ++i)
                {
                    unsigned char* p = static_cast<unsigned char*>(imgU8->GetScalarPointer(i, j, k));
                    if (!p) continue;

                    // считаем "внутри", если любой компонент > 0
                    bool nonZero = false;
                    for (int c = 0; c < comps; ++c)
                        if (p[c] != 0) { nonZero = true; break; }

                    if (!nonZero) continue;

                    any = true;
                    minI = std::min(minI, i);  minJ = std::min(minJ, j);  minK = std::min(minK, k);
                    maxI = std::max(maxI, i);  maxJ = std::max(maxJ, j);  maxK = std::max(maxK, k);
                }

        if (!any) return false;

        bbox[0] = minI; bbox[1] = maxI;
        bbox[2] = minJ; bbox[3] = maxJ;
        bbox[4] = minK; bbox[5] = maxK;
        return true;
    }


    static inline void ExpandClampBBox(int bbox[6], const int ext[6], int margin)
    {
        bbox[0] = std::max(ext[0], bbox[0] - margin);
        bbox[1] = std::min(ext[1], bbox[1] + margin);
        bbox[2] = std::max(ext[2], bbox[2] - margin);
        bbox[3] = std::min(ext[3], bbox[3] + margin);
        bbox[4] = std::max(ext[4], bbox[4] - margin);
        bbox[5] = std::min(ext[5], bbox[5] + margin);
    }
}

#include <vtkReverseSense.h>
#include <vtkMassProperties.h>


vtkSmartPointer<vtkPolyData> VolumeStlExporter::BuildFromBinaryVoxelsNew(
    vtkImageData* binImage, const VisibleExportOptions& opt)
{
    if (!binImage) return nullptr;
    if (opt.progress) opt.progress(5, tr("Init binary export"));

    qDebug() << "[STL] BuildFromBinaryVoxelsNew: start";

    enum class Mode { FullSdf, ShrinkSdf, FastIso };

    auto extentToDims = [](const int e[6], int& nx, int& ny, int& nz)
        {
            nx = e[1] - e[0] + 1;
            ny = e[3] - e[2] + 1;
            nz = e[5] - e[4] + 1;
        };

    auto estimatePeakSdfGB = [](quint64 vox, double kFloatVolumes) -> double
        {
            const double bytes = double(vox) * kFloatVolumes * 4.0;
            return bytes / (1024.0 * 1024.0 * 1024.0);
        };

    auto deepCopyOut = [](vtkPolyData* pd) -> vtkSmartPointer<vtkPolyData>
        {
            if (!pd) return nullptr;
            vtkSmartPointer<vtkPolyData> out = vtkSmartPointer<vtkPolyData>::New();
            out->DeepCopy(pd);
            return out;
        };


    // =========================================================
    // 0) Cast input -> u8
    // =========================================================
    vtkNew<vtkImageCast> castU8;
    castU8->SetInputData(binImage);
    castU8->SetOutputScalarTypeToUnsignedChar();
    castU8->Update();


    vtkImageData* u8 = castU8->GetOutput();
    if (!u8) return nullptr;

    // =========================================================
    // 1) ROI bbox + margin
    // =========================================================
    int ext[6];
    u8->GetExtent(ext);

    int bbox[6];
    if (!ComputeNonZeroBBoxU8_Safe(u8, bbox))
    {
        qDebug() << "[STL] ROI bbox is empty (no non-zero voxels)";
        return nullptr;
    }

    constexpr int ROI_MARGIN = 5;
    ExpandClampBBox(bbox, ext, ROI_MARGIN);

    qDebug() << "[STL] input extent"
        << ext[0] << ext[1] << ext[2] << ext[3] << ext[4] << ext[5];
    qDebug() << "[STL] ROI bbox (with margin)"
        << bbox[0] << bbox[1] << bbox[2] << bbox[3] << bbox[4] << bbox[5];

    vtkNew<vtkExtractVOI> roi;
    roi->SetInputData(u8);
    roi->SetVOI(bbox);
    roi->Update();
    if (opt.progress) opt.progress(12, tr("ROI crop"));

    {
        double o[3]; u8->GetOrigin(o);
        double s[3]; u8->GetSpacing(s);
        qDebug() << "[STL] ROI bbox ijk"
            << bbox[0] << bbox[1] << bbox[2] << bbox[3] << bbox[4] << bbox[5];

        qDebug() << "[STL] ROI min mm = origin + ijk*spacing"
            << (o[0] + bbox[0] * s[0])
            << (o[1] + bbox[2] * s[1])
            << (o[2] + bbox[4] * s[2]);
    }


    // =========================================================
    // 2) bin01 u8: 0/255 -> 0/1
    // =========================================================
    vtkNew<vtkImageThreshold> to01;
    to01->SetInputConnection(roi->GetOutputPort());
    to01->ThresholdByLower(1);
    to01->SetInValue(1);
    to01->SetOutValue(0);
    to01->ReplaceInOn();
    to01->ReplaceOutOn();
    to01->SetOutputScalarTypeToUnsignedChar();
    to01->Update();
    if (opt.progress) opt.progress(18, tr("Binarize 0/1"));
    // =========================================================
    // 3) pad zeros
    // =========================================================
    vtkNew<vtkImageConstantPad> pad;
    pad->SetInputConnection(to01->GetOutputPort());
    pad->SetConstant(0);

    int ext01[6];
    to01->GetOutput()->GetExtent(ext01);

    constexpr int PAD = 2;
    pad->SetOutputWholeExtent(
        ext01[0] - PAD, ext01[1] + PAD,
        ext01[2] - PAD, ext01[3] + PAD,
        ext01[4] - PAD, ext01[5] + PAD
    );
    pad->Update();
    if (opt.progress) opt.progress(25, tr("Padding"));
    // =========================================================
    // Diagnostics + mode selection
    // =========================================================
    int ePad[6];
    pad->GetOutput()->GetExtent(ePad);

    int nx = 0, ny = 0, nz = 0;
    extentToDims(ePad, nx, ny, nz);

    const quint64 vox = quint64(nx) * quint64(ny) * quint64(nz);

    double spPad[3];
    pad->GetOutput()->GetSpacing(spPad);

    qDebug() << "[STL] padded extent"
        << ePad[0] << ePad[1] << ePad[2] << ePad[3] << ePad[4] << ePad[5];
    qDebug() << "[STL] dims" << nx << ny << nz << "vox" << vox;
    qDebug() << "[STL] spacing" << spPad[0] << spPad[1] << spPad[2];

    const double estGB = estimatePeakSdfGB(vox, 5.5);
    qDebug() << "[STL] estimated peak SDF GB" << estGB;

    constexpr quint64 VOX_FORCE_SHRINK = 20ull * 1000ull * 1000ull; // 20M
    constexpr double  SDF_GB_LIMIT = 0.6; // более консервативно

    Mode mode = Mode::FullSdf;
    if (vox >= VOX_FORCE_SHRINK) mode = Mode::ShrinkSdf;
    else if (estGB > SDF_GB_LIMIT) mode = Mode::ShrinkSdf;

    const char* modeStr =
        (mode == Mode::FullSdf) ? "FullSdf" :
        (mode == Mode::ShrinkSdf) ? "ShrinkSdf" : "FastIso";
    qDebug() << "[STL] mode" << modeStr;

    // =========================================================
    // FAST ISO: изо 0.5 по u8 0/1
    // =========================================================
    if (mode == Mode::FastIso)
    {
        if (opt.progress) opt.progress(35, tr("Fast surface"));

        vtkNew<vtkFlyingEdges3D> fe;
        fe->SetInputConnection(pad->GetOutputPort());
        fe->SetValue(0, 0.5);
        fe->ComputeNormalsOff();
        fe->ComputeGradientsOff();
        fe->ComputeScalarsOff();
        fe->Update();
        if (opt.progress) opt.progress(70, tr("Extracting surface"));

        vtkNew<vtkTriangleFilter> tri;
        tri->SetInputConnection(fe->GetOutputPort());
        tri->PassLinesOff();
        tri->PassVertsOff();
        tri->Update();

        vtkNew<vtkCleanPolyData> clean;
        clean->PointMergingOn();
        clean->SetInputConnection(tri->GetOutputPort());
        clean->Update();
        if (opt.progress) opt.progress(85, tr("Cleaning"));

        vtkNew<vtkFillHolesFilter> fill;
        fill->SetInputConnection(clean->GetOutputPort());
        fill->SetHoleSize(500.0);

        vtkNew<vtkPolyDataConnectivityFilter> conn;
        conn->SetInputConnection(fill->GetOutputPort());
        conn->SetExtractionModeToLargestRegion();
        conn->Update();
        if (opt.progress) opt.progress(92, tr("Largest component"));

        vtkNew<vtkPolyDataNormals> nrm;
        nrm->SetInputConnection(conn->GetOutputPort());
        nrm->ComputePointNormalsOn();
        nrm->ComputeCellNormalsOff();
        nrm->SplittingOff();
        nrm->ConsistencyOn();
        nrm->AutoOrientNormalsOn();
        nrm->Update();

        auto out = deepCopyOut(nrm->GetOutput());
        if (!out) return nullptr;

        qDebug() << "[STL] output points" << out->GetNumberOfPoints();
        qDebug() << "[STL] output polys" << out->GetNumberOfPolys();
        qDebug() << "[STL] BuildFromBinaryVoxelsNew: done (FastIso)";

        if (opt.progress) opt.progress(100, tr("Done"));
        return out;
    }

    // =========================================================
    // SDF PATH
    // =========================================================

    vtkAlgorithmOutput* sdfMaskU8 = pad->GetOutputPort();

    vtkNew<vtkExtractVOI> shrinkVoi;
    int shrinkFactor = 1;
    bool usedShrink = false;

    if (mode == Mode::ShrinkSdf)
    {
        shrinkFactor = (estGB > 1.6) ? 3 : 2;
        qDebug() << "[STL] shrinkFactor" << shrinkFactor;

        int ePad2[6];
        pad->GetOutput()->GetExtent(ePad2);

        shrinkVoi->SetInputConnection(pad->GetOutputPort());
        shrinkVoi->SetVOI(ePad2);
        shrinkVoi->SetSampleRate(shrinkFactor, shrinkFactor, shrinkFactor);
        shrinkVoi->Update();

        sdfMaskU8 = shrinkVoi->GetOutputPort();
        usedShrink = true;

        int eS[6];
        shrinkVoi->GetOutput()->GetExtent(eS);
        int sx = 0, sy = 0, sz = 0; extentToDims(eS, sx, sy, sz);
        quint64 svox = quint64(sx) * quint64(sy) * quint64(sz);
        double sps[3]; shrinkVoi->GetOutput()->GetSpacing(sps);

        qDebug() << "[STL] shrink dims" << sx << sy << sz << "vox" << svox;
        qDebug() << "[STL] shrink spacing" << sps[0] << sps[1] << sps[2];
    }

    // --- EDT считаем по u8 0/1 ---
    vtkNew<vtkImageCast> binU8;
    binU8->SetInputConnection(sdfMaskU8);
    binU8->SetOutputScalarTypeToUnsignedChar();
    binU8->Update();

    vtkNew<vtkImageMathematics> invU8;
    invU8->SetOperationToMultiplyByK();
    invU8->SetInputConnection(0, binU8->GetOutputPort());
    invU8->SetConstantK(-1.0);
    invU8->Update();

    vtkNew<vtkImageMathematics> invU8b;
    invU8b->SetOperationToAddConstant();
    invU8b->SetInputConnection(0, invU8->GetOutputPort());
    invU8b->SetConstantC(1.0);
    invU8b->Update();

    vtkNew<vtkImageEuclideanDistance> distOut;
    distOut->SetInputConnection(binU8->GetOutputPort());
    distOut->SetConsiderAnisotropy(true);
    distOut->Update();
    if (opt.progress) opt.progress(40, tr("Distance (outside)"));
    qDebug() << "[STL] EDT outside done";

    vtkNew<vtkImageEuclideanDistance> distIn;
    distIn->SetInputConnection(invU8b->GetOutputPort());
    distIn->SetConsiderAnisotropy(true);
    distIn->Update();
    if (opt.progress) opt.progress(55, tr("Distance (inside)"));
    qDebug() << "[STL] EDT inside done";

    // SDF = distOut - distIn
    vtkNew<vtkImageMathematics> sdf;
    sdf->SetOperationToSubtract();
    sdf->SetInputConnection(0, distOut->GetOutputPort());
    sdf->SetInputConnection(1, distIn->GetOutputPort());
    sdf->Update();

    double rSdf[2];
    sdf->GetOutput()->GetScalarRange(rSdf);
    qDebug() << "[STL] SDF range" << rSdf[0] << rSdf[1];

    // Если знак вдруг перевёрнут
    if (!(rSdf[0] <= 0.0 && rSdf[1] >= 0.0))
    {
        qDebug() << "[STL] SDF sign inverted, fixing";
        vtkNew<vtkImageMathematics> neg;
        neg->SetOperationToMultiplyByK();
        neg->SetInputConnection(0, sdf->GetOutputPort());
        neg->SetConstantK(-1.0);
        neg->Update();

        neg->GetOutput()->GetScalarRange(rSdf);
        qDebug() << "[STL] SDF range after fix" << rSdf[0] << rSdf[1];

        if (!(rSdf[0] <= 0.0 && rSdf[1] >= 0.0))
        {
            qDebug() << "[STL] SDF still wrong, abort";
            return nullptr;
        }

        sdf->SetOperationToMultiplyByK();
        sdf->SetInputConnection(0, neg->GetOutputPort());
        sdf->SetConstantK(1.0);
        sdf->Update();
    }
    if (opt.progress) opt.progress(62, tr("Signed distance"));

    // Gaussian smooth SDF
    vtkNew<vtkImageGaussianSmooth> gs;
    gs->SetInputConnection(sdf->GetOutputPort());
    gs->SetDimensionality(3);

    double spS[3];
    if (usedShrink) shrinkVoi->GetOutput()->GetSpacing(spS);
    else           pad->GetOutput()->GetSpacing(spS);

    double sigmaXY = 0.9;     // было 0.8
    double sigmaZ = 1.4;     // усилить вдоль срезов

    gs->SetStandardDeviations(
        sigmaXY / std::max(1e-9, spS[0]),
        sigmaXY / std::max(1e-9, spS[1]),
        sigmaZ / std::max(1e-9, spS[2])
    );
    gs->SetRadiusFactors(3.0, 3.0, 3.0);
    gs->Update();

    if (opt.progress) opt.progress(72, tr("Smooth SDF"));


    // iso-surface at 0
    vtkNew<vtkFlyingEdges3D> fe;
    fe->SetInputConnection(gs->GetOutputPort());
    fe->SetValue(0, 0.0);
    fe->ComputeNormalsOff();
    fe->ComputeGradientsOff();
    fe->ComputeScalarsOff();
    fe->Update();
    if (opt.progress) opt.progress(80, tr("Extracting surface"));
    qDebug() << "[STL] FlyingEdges iso=0 done";

    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputConnection(fe->GetOutputPort());
    tri->PassLinesOff();
    tri->PassVertsOff();
    tri->Update();

    // bounds/spacing берём с актуального объёма
    double b[6];
    double sp2[3];
    if (usedShrink)
    {
        shrinkVoi->GetOutput()->GetBounds(b);
        shrinkVoi->GetOutput()->GetSpacing(sp2);
    }
    else
    {
        pad->GetOutput()->GetBounds(b);
        pad->GetOutput()->GetSpacing(sp2);
    }

    const double mX = 2.0 * sp2[0];
    const double mY = 2.0 * sp2[1];
    const double mZ = 2.0 * sp2[2];

    vtkAlgorithmOutput* geomPort = tri->GetOutputPort();

    vtkNew<vtkBox> innerBox;
    vtkNew<vtkClipPolyData> clip;

    // небольшой inner-clip, чтобы гарантированно отрезать “рамку паддинга”
    if ((b[1] - b[0]) > 2.5 * mX && (b[3] - b[2]) > 2.5 * mY && (b[5] - b[4]) > 2.5 * mZ)
    {
        innerBox->SetBounds(
            b[0] + mX, b[1] - mX,
            b[2] + mY, b[3] - mY,
            b[4] + mZ, b[5] - mZ
        );

        clip->SetInputConnection(geomPort);
        clip->SetClipFunction(innerBox);
        clip->InsideOutOn();
        clip->GenerateClippedOutputOff();
        clip->Update();

        geomPort = clip->GetOutputPort();
        qDebug() << "[STL] clip enabled";
    }
    else
    {
        qDebug() << "[STL] clip disabled (bounds too small)";
    }

    vtkNew<vtkCleanPolyData> clean;
    clean->PointMergingOn();
    clean->SetInputConnection(geomPort);
    clean->Update();
    if (opt.progress) opt.progress(88, tr("Cleaning"));

    vtkNew<vtkFillHolesFilter> fill;
    fill->SetInputConnection(clean->GetOutputPort());
    fill->SetHoleSize(500.0);

    vtkNew<vtkPolyDataConnectivityFilter> conn;
    conn->SetInputConnection(fill->GetOutputPort());
    conn->SetExtractionModeToLargestRegion();
    conn->Update();
    if (opt.progress) opt.progress(95, tr("Largest component"));

    vtkNew<vtkPolyDataNormals> nrm;
    nrm->SetInputConnection(conn->GetOutputPort());
    nrm->ComputePointNormalsOn();
    nrm->ComputeCellNormalsOff();
    nrm->SplittingOff();
    nrm->ConsistencyOn();
    nrm->AutoOrientNormalsOn();
    nrm->Update();

    // ---- страховка ориентации: если объём отрицательный, разворачиваем ----
    vtkSmartPointer<vtkPolyData> finalPd = deepCopyOut(nrm->GetOutput());
    if (!finalPd || finalPd->GetNumberOfCells() == 0) return nullptr;

    {
        vtkNew<vtkMassProperties> mp;
        mp->SetInputData(finalPd);
        mp->Update();
        const double vol = mp->GetVolume();

        // Если получилось совсем криво/нулевой объём, не дёргаем.
        // Если отрицательный, значит ориентация треугольников “внутрь”.
        if (std::isfinite(vol) && vol < 0.0)
        {
            qDebug() << "[STL] volume negative, reversing sense";

            vtkNew<vtkReverseSense> rev;
            rev->SetInputData(finalPd);
            rev->ReverseCellsOn();
            rev->ReverseNormalsOn();
            rev->Update();

            finalPd = deepCopyOut(rev->GetOutput());
        }
    }

    qDebug() << "[STL] output points" << finalPd->GetNumberOfPoints();
    qDebug() << "[STL] output polys" << finalPd->GetNumberOfPolys();
    qDebug() << "[STL] BuildFromBinaryVoxelsNew: done (SDF)";

    if (opt.progress) opt.progress(100, tr("Done"));
    return finalPd;
}

#include <vtkSmartPointer.h>
#include <vtkSmoothPolyDataFilter.h>

vtkSmartPointer<vtkPolyData> VolumeStlExporter::SimplifySurface(
    vtkPolyData* in,
    double targetReduction,
    int smoothIter,
    double passBand)
{
    if (!in || in->GetNumberOfCells() == 0)
        return nullptr;

    targetReduction = std::clamp(targetReduction, 0.0, 0.99);
    smoothIter = std::max(0, smoothIter);
    passBand = std::clamp(passBand, 0.01, 0.5);

    // 1) Треугольники + clean + largest
    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputData(in);
    tri->PassLinesOff();
    tri->PassVertsOff();
    tri->Update();

    vtkNew<vtkCleanPolyData> clean0;
    clean0->SetInputConnection(tri->GetOutputPort());
    clean0->PointMergingOn();
    clean0->Update();

    vtkNew<vtkPolyDataConnectivityFilter> conn0;
    conn0->SetInputConnection(clean0->GetOutputPort());
    conn0->SetExtractionModeToLargestRegion();
    conn0->Update();

    vtkAlgorithmOutput* cur = conn0->GetOutputPort();

    // 2) Легкое сглаживание ДО децимации (убрать “шахматку/рябь”)
    // Важно: без normalize coordinates.
    vtkAlgorithmOutput* pre = cur;
    vtkNew<vtkWindowedSincPolyDataFilter> preSmooth;
    {
        // preSmooth всегда небольшой, иначе начнет “мылить”
        const int preIter = (smoothIter > 0) ? std::min(12, std::max(6, smoothIter / 2)) : 8;
        const double preBand = std::clamp(passBand * 1.3, 0.08, 0.25);

        preSmooth->SetInputConnection(cur);
        preSmooth->SetNumberOfIterations(preIter);
        preSmooth->SetPassBand(preBand);

        preSmooth->BoundarySmoothingOff();
        preSmooth->FeatureEdgeSmoothingOff();
        preSmooth->NonManifoldSmoothingOn();
        preSmooth->NormalizeCoordinatesOff();     // ключ!
        preSmooth->GenerateErrorScalarsOff();
        preSmooth->GenerateErrorVectorsOff();
        preSmooth->Update();

        pre = preSmooth->GetOutputPort();
    }

    // 3) Децимация: QuadricDecimation обычно держит форму лучше, чем DecimatePro,
    // особенно когда нужно сильно ужимать (как у тебя 9MB -> 3MB).
    vtkAlgorithmOutput* decOut = pre;

    vtkNew<vtkQuadricDecimation> qdec;
    vtkNew<vtkCleanPolyData> postDecClean;
    vtkNew<vtkTriangleFilter> postDecTri;

    if (targetReduction > 0.0)
    {
        qdec->SetInputConnection(pre);
        qdec->SetTargetReduction(targetReduction);

        qdec->AttributeErrorMetricOff();
        qdec->ScalarsAttributeOff();
        qdec->VectorsAttributeOff();
        qdec->NormalsAttributeOff();
        qdec->TCoordsAttributeOff();
        qdec->TensorsAttributeOff();
        qdec->Update();

        postDecClean->SetInputConnection(qdec->GetOutputPort());
        postDecClean->PointMergingOn();
        postDecClean->Update();

        postDecTri->SetInputConnection(postDecClean->GetOutputPort());
        postDecTri->PassLinesOff();
        postDecTri->PassVertsOff();
        postDecTri->Update();

        decOut = postDecTri->GetOutputPort();
    }


    vtkNew<vtkSmoothPolyDataFilter> lap;
    lap->SetInputConnection(decOut);
    lap->SetNumberOfIterations(std::min(60, std::max(10, smoothIter * 3)));
    lap->SetRelaxationFactor(0.01);        // маленький, чтобы не “усаживало”
    lap->FeatureEdgeSmoothingOff();
    lap->BoundarySmoothingOn();            // ключ для среза
    lap->Update();

    // 4) Post-smooth: уже мягко, чтобы скрыть триангуляцию после децимации
    vtkAlgorithmOutput* post = lap->GetOutputPort();
    vtkNew<vtkWindowedSincPolyDataFilter> postSmooth;
    if (smoothIter > 0)
    {
        const int postIter = std::max(0, smoothIter);
        const double postBand = passBand; // как просили параметрами

        postSmooth->SetInputConnection(post);
        postSmooth->SetNumberOfIterations(postIter);
        postSmooth->SetPassBand(postBand);

        postSmooth->BoundarySmoothingOn();        // было Off
        postSmooth->SetFeatureAngle(120.0);       // можно оставить, не мешает
        postSmooth->FeatureEdgeSmoothingOff();
        postSmooth->NonManifoldSmoothingOn();
        postSmooth->NormalizeCoordinatesOff();    // тоже ключ!
        postSmooth->GenerateErrorScalarsOff();
        postSmooth->GenerateErrorVectorsOff();
        postSmooth->Update();

        post = postSmooth->GetOutputPort();
    }

    // 5) Largest component еще раз (после decimation бывает мусор)
    vtkNew<vtkPolyDataConnectivityFilter> conn1;
    conn1->SetInputConnection(post);
    conn1->SetExtractionModeToLargestRegion();
    conn1->Update();

    // 6) Нормали
    vtkNew<vtkPolyDataNormals> nrm;
    nrm->SetInputConnection(conn1->GetOutputPort());
    nrm->ComputePointNormalsOn();
    nrm->ComputeCellNormalsOff();
    nrm->SplittingOff();
    nrm->ConsistencyOn();
    nrm->AutoOrientNormalsOn();
    nrm->Update();

    // 7) Финальный clean
    vtkNew<vtkCleanPolyData> clean1;
    clean1->SetInputConnection(nrm->GetOutputPort());
    clean1->PointMergingOn();
    clean1->Update();

    vtkSmartPointer<vtkPolyData> out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(clean1->GetOutput());
    return out;
}