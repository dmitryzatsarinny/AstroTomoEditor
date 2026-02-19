#pragma once
#include <QString>
#include <vtkSmartPointer.h>
#include <functional>

class vtkImageData;
class vtkVolume;
class vtkPolyData;

struct VisibleExportOptions
{
    // что считать «видимым»: alpha(value) > threshold
    double alphaThreshold = 0.05;

    // постобработка сетки
    bool   computeNormals = true;
    int    smoothIterations = 0;     // 0 = без сглаживания
    double smoothPassBand = 0.1;     // 0..1
    double decimate = 0.0;           // 0..0.9

    double clusterCellMM = 0.0;

    // Либо можно задать фиксированные деления по осям (если > 0 — имеют приоритет).
    int clusterDivX = 0;
    int clusterDivY = 0;
    int clusterDivZ = 0;

    bool   binaryStl = true;

    std::function<void(int, const QString&)> progress;
};

class VolumeStlExporter
{

public:
    static vtkSmartPointer<vtkPolyData> BuildFromBinaryVoxelsNew(
        vtkImageData* binImage, const VisibleExportOptions& opt);

    static vtkSmartPointer<vtkPolyData> SimplifySurface(
        vtkPolyData* in, double targetReduction = 0.25, int smoothIter = 10, double passBand = 0.15);
    
    static bool SaveStlMyBinary_NoCenter(vtkPolyData* pd, const QString& filePath, double VolumeOriginX, double VolumeOriginY, double VolumeOriginZ,
        double VolumeCenterX, double VolumeCenterY, double VolumeCenterZ, bool recomputeNormals /*= true*/);

    static vtkSmartPointer<vtkPolyData> SimplifyToTargetBytes(vtkPolyData* in, std::int64_t targetBytes, int smoothIter, double passBand);

    static vtkSmartPointer<vtkPolyData> NormalizeSurface(vtkPolyData* in);

    static QString prettyBytes(quint64 bytes);

    static quint64 estimateBinaryStlBytesFast(vtkPolyData* pd);

    static QString makeStlSizeText(vtkPolyData* pd);
};
