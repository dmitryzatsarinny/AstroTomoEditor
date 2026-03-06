#pragma once
#include "ElectrodePanel.h"

#include <array>
#include <vector>

class vtkRenderer;
class ElectrodeSurfaceDetector;

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

    static Result SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI);
    static bool ShouldShowSearchRLFN(const ElectrodePanel* panel);

    static void SearchV1V6(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI);
    static bool ShouldShowSearchV1V6(const ElectrodePanel* panel);
    static void SearchV7V12(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI);
    static bool ShouldShowSearchV7V12(const ElectrodePanel* panel);
    static void SearchV13V19(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI);
    static bool ShouldShowSearchV13V19(const ElectrodePanel* panel);
    static void SearchV20V25(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI);
    static bool ShouldShowSearchV20V25(const ElectrodePanel* panel);
    static void SearchV26V30(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI);
    static bool ShouldShowSearchV26V30(const ElectrodePanel* panel);
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

    struct CameraState
    {
        double pos[3]{};
        double focal[3]{};
        double viewUp[3]{};
        double parallelScale = 1.0;
        bool parallelProjection = false;
    };

    static CameraState SaveCamera(vtkRenderer* ren);
    static void RestoreCamera(vtkRenderer* ren, const CameraState& s, bool render = true);

    static void TurnToPA(vtkRenderer* ren, bool render = true); // разворот на 180 от AP
    static int PickLeftBottom(const std::vector<Cand2D>& cands);
    static int PickRightBottom(const std::vector<Cand2D>& cands);
    static int PickLeft(const std::vector<Cand2D>& cands);
    static Anchor CommitByIndex(ElectrodePanel* panel, vtkRenderer* ren, ElectrodeSurfaceDetector& det,
        ElectrodePanel::ElectrodeId id,
        const std::vector<Cand2D>& cands, int idx,
        bool& outPlaced, std::array<double, 3>& outWorld);
};

namespace
{
    QString WToStr(const std::array<double, 3>& w)
    {
        return QString("(%1, %2, %3)")
            .arg(w[0], 0, 'f', 2)
            .arg(w[1], 0, 'f', 2)
            .arg(w[2], 0, 'f', 2);
    }

    QString DToStr(double x, double y)
    {
        return QString("(%1, %2)")
            .arg(x, 0, 'f', 2)
            .arg(y, 0, 'f', 2);
    }

    const char* ElectrodeIdToString(ElectrodePanel::ElectrodeId id)
    {
        using Id = ElectrodePanel::ElectrodeId;
        switch (id)
        {
        case Id::R: return "R";
        case Id::L: return "L";
        case Id::F: return "F";
        case Id::N: return "N";
        case Id::V1: return "V1";
        case Id::V2: return "V2";
        case Id::V3: return "V3";
        case Id::V4: return "V4";
        case Id::V5: return "V5";
        case Id::V6: return "V6";
        case Id::V7: return "V7";
        case Id::V8: return "V8";
        case Id::V9: return "V9";
        case Id::V10: return "V10";
        case Id::V11: return "V11";
        case Id::V12: return "V12";
        case Id::V13: return "V13";
        case Id::V14: return "V14";
        case Id::V15: return "V15";
        case Id::V16: return "V16";
        case Id::V17: return "V17";
        case Id::V18: return "V18";
        case Id::V19: return "V19";
        case Id::V20: return "V20";
        case Id::V21: return "V21";
        case Id::V22: return "V22";
        case Id::V23: return "V23";
        case Id::V24: return "V24";
        case Id::V25: return "V25";
        case Id::V26: return "V26";
        case Id::V27: return "V27";
        case Id::V28: return "V28";
        case Id::V29: return "V29";
        case Id::V30: return "V30";
        default: return "Unknown";
        }
    }
}

