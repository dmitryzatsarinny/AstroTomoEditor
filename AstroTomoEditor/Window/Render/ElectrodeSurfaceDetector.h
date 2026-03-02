#pragma once
#include <vector>
#include <array>
#include <cstdint>

#include <vtkSmartPointer.h>

class vtkImageData;
class vtkRenderer;
class vtkActor;

class ElectrodeSurfaceDetector
{
public:
    struct Options
    {
        bool debug = true;

        int metalMin = 245;
        int metalMax = 255;

        int bodyThreshold = 25;
        int surfaceRadiusVox = 2;

        int minComponentVox = 5;
        int maxComponentVox = 2000;
        double maxElongation = 8.0;

        double sphereRadiusMm = 3.0;

        // NEW: фильтрация
        double minDistanceMm = 20.0;        // рядом (2 см) не должно быть другого электрода
        double outwardMaxMm = 15.0;          // за сколько мм обязаны выйти в воздух по нормали
        double inwardMinMm = 5.0;            // вглубь должны оставаться в теле хотя бы столько
        double rayStepVox = 0.5;             // шаг трассировки в вокселях
        double minNormalLen = 0.2;           // минимальная “уверенность” нормали (в вокселях на 1 грань)
    };

    ElectrodeSurfaceDetector();
    ~ElectrodeSurfaceDetector();

    void setOptions(const Options& opt) { opt_ = opt; }
    const Options& options() const { return opt_; }

    std::vector<std::array<double, 3>> detectAndShow(vtkImageData* img, vtkRenderer* ren);
    void clear(vtkRenderer* ren);

private:
    Options opt_{};
    std::vector<vtkSmartPointer<vtkActor>> actors_;

    static inline int idx(int x, int y, int z, int nx, int ny) noexcept
    {
        return (z * ny + y) * nx + x;
    }

    void addSphere(vtkRenderer* ren, const std::array<double, 3>& world);
};