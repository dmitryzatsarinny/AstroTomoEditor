#pragma once
#include <vector>
#include <array>
#include <cstdint>

#include <vtkSmartPointer.h>

class vtkImageData;
class vtkRenderer;
class vtkActor;
class vtkRenderWindow;

class ElectrodeSurfaceDetector
{
public:
    static ElectrodeSurfaceDetector& instance();

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
        double exclusionRadiusMm = 20.0;
    };

    ElectrodeSurfaceDetector();
    ~ElectrodeSurfaceDetector();

    void setOptions(const Options& opt) { opt_ = opt; }
    const Options& options() const { return opt_; }

    std::vector<std::array<double, 3>> detectAndShow(
        vtkImageData* img,
        vtkRenderer* ren,
        const std::vector<std::array<double, 3>>& excludedWorld = {});
    void clear(vtkRenderer* ren);
    void addManualSphere(vtkRenderer* ren, const std::array<double, 3>& world);
    int sphereCount() const;
    std::vector<std::array<double, 3>> sphereCenters() const;
    bool removeSphereAtDisplay(vtkRenderer* ren, int x, int y, vtkRenderWindow* rw = nullptr);
    std::vector<std::array<double, 3>> currentSphereCenters() const;
    bool closestSphereAtDisplay(vtkRenderer* ren, int x, int y,
        double maxDistPx,
        std::array<double, 3>& outWorld,
        double* outDistPx = nullptr,
        double* outRadiusMm = nullptr) const;
    bool removeSphereNearWorld(vtkRenderer* ren,
        const std::array<double, 3>& world,
        double maxDistMm);

private:
    struct SphereMarker
    {
        vtkSmartPointer<vtkActor> actor;
        std::array<double, 3> center{};
    };

    Options opt_{};
    std::vector<SphereMarker> actors_;

    static inline int idx(int x, int y, int z, int nx, int ny) noexcept
    {
        return (z * ny + y) * nx + x;
    }

    void addSphere(vtkRenderer* ren, const std::array<double, 3>& world);
    static bool worldToDisplay(vtkRenderer* ren, const std::array<double, 3>& world, double outDisplay[2]);
};