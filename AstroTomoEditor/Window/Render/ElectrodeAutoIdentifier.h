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

    static Result SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren);
    static bool ShouldShowSearchRLFN(const ElectrodePanel* panel);
};