#pragma once
#include <array>
#include <vector>

class vtkRenderer;
class vtkCamera;

class ElectrodePanel;

class ElectrodeAutoIdentifier
{
public:
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

        std::array<double, 3> wV1{};
        std::array<double, 3> wV2{};
        std::array<double, 3> wV3{};
        std::array<double, 3> wV4{};
        std::array<double, 3> wV5{};
    };

    static Result SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren);
    static bool ShouldShowSearchRLFN(const ElectrodePanel* panel);

    static PrecordialResult SearchV1V5(ElectrodePanel* panel, vtkRenderer* ren);
    static bool ShouldShowSearchV1V5(const ElectrodePanel* panel);
};