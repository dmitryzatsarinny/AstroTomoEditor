#pragma once
#include "ElectrodePanel.h"

#include <array>
#include <vector>

class vtkRenderer;

class ElectrodeAutoIdentifier
{
public:
    struct Cand2D
    {
        std::array<double, 3> w{};
        double x = 0.0;
        double y = 0.0;
    };

    struct Anchor
    {
        bool valid = false;
        double x = 0.0;
        double y = 0.0;
        std::array<double, 3> w{};
    };

    struct Result
    {
        bool placedR = false;
        bool placedL = false;
        bool placedF = false;
        bool placedN = false;

        std::array<double, 3> wR{};
        std::array<double, 3> wL{};
        std::array<double, 3> wF{};
        std::array<double, 3> wN{};
    };

    struct PrecordialResult
    {
        bool placedV1 = false;
        bool placedV2 = false;
        bool placedV3 = false;
        bool placedV4 = false;
        bool placedV5 = false;
        bool placedV6 = false;
        bool placedV7 = false;
        bool placedV8 = false;
        bool placedV9 = false;
        bool placedV10 = false;
        bool placedV11 = false;
        bool placedV12 = false;

        std::array<double, 3> wV1{};
        std::array<double, 3> wV2{};
        std::array<double, 3> wV3{};
        std::array<double, 3> wV4{};
        std::array<double, 3> wV5{};
        std::array<double, 3> wV6{};
        std::array<double, 3> wV7{};
        std::array<double, 3> wV8{};
        std::array<double, 3> wV9{};
        std::array<double, 3> wV10{};
        std::array<double, 3> wV11{};
        std::array<double, 3> wV12{};
    };

    static Result SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren);
    static bool ShouldShowSearchRLFN(const ElectrodePanel* panel);

    static PrecordialResult SearchV1V6(ElectrodePanel* panel, vtkRenderer* ren);
    static bool ShouldShowSearchV1V6(const ElectrodePanel* panel);
    static PrecordialResult SearchV7V12(ElectrodePanel* panel, vtkRenderer* ren);
    static bool ShouldShowSearchV7V12(const ElectrodePanel* panel);
    static bool WorldToDisplay(vtkRenderer* ren, const std::array<double, 3>& w, double& outX, double& outY);
    static bool FindPanelCoord(const ElectrodePanel* panel, ElectrodePanel::ElectrodeId id, std::array<double, 3>& outWorld);
    static bool ComputeVolumeDisplayCenter(vtkRenderer* ren, double& cx, double& cy);
    static bool ComputeVolumeWorldCenter(vtkRenderer* ren, std::array<double, 3>& outC);
    static std::vector<Cand2D> CollectDisplayCandidates(vtkRenderer* ren, const std::vector<std::array<double, 3>>& centers);
    static int PickClosestInSectorFrom(
        const std::vector<Cand2D>& cands,
        double ax, double ay,
        double h0, double h1,
        const std::array<double, 3>& volumeCenterW,
        double minRadiusFromCenter,
        double maxRadiusFromCenter,
        const std::array<double, 3>& anchorWorld,
        bool useAnchorHemisphere);
    static Anchor AnchorFromPanel(const ElectrodePanel* panel, vtkRenderer* ren, ElectrodePanel::ElectrodeId id);
};