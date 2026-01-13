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


vtkSmartPointer<vtkPolyData> VolumeStlExporter::BuildFromVisible(
    vtkImageData* image, vtkVolume* volumeWithTF, const VisibleExportOptions& opt)
{
    if (opt.progress) opt.progress(5, tr("Init"));

    auto mask = BuildVisibleMaskLUT(image, volumeWithTF, opt.alphaThreshold, &opt);
    if (opt.progress) opt.progress(35, tr("Classified visible voxels"));
    if (!mask) return nullptr;

    mask = CloseAndPadMask(mask, 0);
    if (opt.progress) opt.progress(50, tr("Closed mask & padded"));

    auto poly = ExtractLargestFromMask(mask, opt);
    if (opt.progress) opt.progress(98, tr("Surface extracted"));
    return poly;
}

vtkSmartPointer<vtkPolyData> VolumeStlExporter::BuildFromBinaryVoxels(
    vtkImageData* binImage, const VisibleExportOptions& opt)
{
    if (!binImage) return nullptr;
    if (opt.progress) opt.progress(5, tr("Init binary export"));

    int ext[6];
    binImage->GetExtent(ext);

    vtkNew<vtkImageConstantPad> pad;
    pad->SetInputData(binImage);
    pad->SetConstant(0);
    pad->SetOutputWholeExtent(
        ext[0] - 1, ext[1] + 1,
        ext[2] - 1, ext[3] + 1,
        ext[4] - 1, ext[5] + 1
    );
    if (opt.progress) opt.progress(10, tr("Padding"));

    // 2) Изо-поверхность по бинарному полю
    vtkNew<vtkFlyingEdges3D> fe;
    fe->SetInputConnection(pad->GetOutputPort());
    fe->SetValue(0, 127.5); // 0 vs 255
    fe->ComputeNormalsOff();
    fe->ComputeGradientsOff();
    fe->ComputeScalarsOff();
    if (opt.progress) opt.progress(35, tr("Extracting surface"));

    // 3) Треугольники
    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputConnection(fe->GetOutputPort());

    // 4) Чистка
    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputConnection(tri->GetOutputPort());
    clean->PointMergingOn();
    if (opt.progress) opt.progress(60, tr("Cleaning"));

    // 5) (опц.) Заполнить мелкие дырки, но НЕ изменять форму
    vtkNew<vtkFillHolesFilter> fill;
    fill->SetInputConnection(clean->GetOutputPort());
    fill->SetHoleSize(500.0); // маленькие дырки; не зашивает огромные пробелы
    if (opt.progress) opt.progress(75, tr("Filling small holes"));

    // 6) Взять крупнейшую компоненту
    vtkNew<vtkPolyDataConnectivityFilter> conn;
    conn->SetInputConnection(fill->GetOutputPort());
    conn->SetExtractionModeToLargestRegion();
    conn->Update();
    if (opt.progress) opt.progress(90, tr("Largest component"));

    // 7) DeepCopy → гарантированная живучесть результатов
    vtkSmartPointer<vtkPolyData> out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(conn->GetOutput());
    if (opt.progress) opt.progress(100, tr("Done"));

    return out;
}


vtkSmartPointer<vtkPolyData> VolumeStlExporter::SimplifySurface(
    vtkPolyData* in, double targetReduction, int smoothIter, double passBand)
{
    if (!in || in->GetNumberOfCells() == 0) return nullptr;

    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputData(in);

    vtkNew<vtkDecimatePro> decSafe;
    decSafe->SetInputConnection(tri->GetOutputPort());
    decSafe->SetTargetReduction(std::min(0.5, targetReduction * 0.6));
    decSafe->PreserveTopologyOn();
    decSafe->SplittingOff();
    decSafe->BoundaryVertexDeletionOff();
    decSafe->SetFeatureAngle(120.0);

    vtkNew<vtkWindowedSincPolyDataFilter> smooth;
    smooth->SetInputConnection(decSafe->GetOutputPort());
    smooth->SetNumberOfIterations(std::max(0, smoothIter));
    smooth->BoundarySmoothingOff();
    smooth->FeatureEdgeSmoothingOff();
    smooth->SetFeatureAngle(120.0);
    smooth->SetPassBand(passBand);
    smooth->NonManifoldSmoothingOn();
    smooth->NormalizeCoordinatesOff();

    vtkNew<vtkQuadricDecimation> qd;
    qd->SetInputConnection(smooth->GetOutputPort());
    qd->SetTargetReduction(std::clamp(targetReduction, 0.0, 0.95));
    qd->VolumePreservationOn();

    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputConnection(qd->GetOutputPort());
    clean->PointMergingOn();

    vtkNew<vtkPolyDataConnectivityFilter> conn;
    conn->SetInputConnection(clean->GetOutputPort());
    conn->SetExtractionModeToLargestRegion();

    vtkNew<vtkPolyDataNormals> norms;
    norms->SetInputConnection(conn->GetOutputPort());
    norms->ComputePointNormalsOn();
    norms->ComputeCellNormalsOff();
    norms->SplittingOff();
    norms->ConsistencyOn();
    norms->AutoOrientNormalsOn();
    norms->Update();

    vtkSmartPointer<vtkPolyData> out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(norms->GetOutput());
    return out;
}


bool VolumeStlExporter::SaveStl(vtkPolyData* pd, const QString& filePath, bool binary)
{
    if (!pd || filePath.isEmpty()) return false;
    vtkNew<vtkSTLWriter> w;
    w->SetFileName(filePath.toUtf8().constData());
    if (binary) w->SetFileTypeToBinary(); else w->SetFileTypeToASCII();
    w->SetInputData(pd);
    w->Write();
    return true;
}