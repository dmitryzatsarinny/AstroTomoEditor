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

        int metalMin = 254;
        int metalMax = 255;

        // если хочешь отсечь воздух/стол
        bool useBodyMask = false;
        int bodyThreshold = 25;

        int minComponentVox = 25;
        int maxComponentVox = 1000;

        // “сферичность” по ковариации в voxel-space
        double maxAxisRatioVox = 5;     // 1.0 идеал, 1.6..2.2 обычно норм

        // доп. критерий “похож на шар” по разбросу радиуса
        bool useRadiusConsistency = false;
        double maxRadiusStdRel = 0.99;    // std(r)/mean(r)

        double sphereRadiusMm = 5.0;      // как рисовать маркер
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