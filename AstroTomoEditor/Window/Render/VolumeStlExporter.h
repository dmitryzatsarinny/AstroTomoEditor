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

    std::function<void(int, const char*)> progress;
};

class VolumeStlExporter
{
public:
    static vtkSmartPointer<vtkPolyData> BuildFromVisible(
        vtkImageData* image,
        vtkVolume* volumeWithTF,
        const VisibleExportOptions& opt = {});

    static vtkSmartPointer<vtkPolyData> BuildFromBinaryVoxels(
        vtkImageData* binImage, const VisibleExportOptions& opt);

    static vtkSmartPointer<vtkPolyData> SimplifySurface(
        vtkPolyData* in, double targetReduction = 0.25, int smoothIter = 10, double passBand = 0.15);

    static bool SaveStl(vtkPolyData* pd, const QString& filePath, bool binary = true);
};
