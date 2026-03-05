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

    // Ищет R/L (если их нет) и F/N (если их нет), используя текущие “желтые” сферы.
    // Логика:
    //  - берём текущие sphere-centers из ElectrodeSurfaceDetector
    //  - проецируем в AP (экранные координаты)
    //  - самая дальняя пара -> R/L
    //  - из оставшихся самая дальняя пара -> F/N
    //
    // Важно: назначение R vs L и F vs N делаем по экранным осям:
    //  - R = левее на экране (как на рентгене AP: правая сторона пациента слева)
    //  - L = правее
    //  - F = ниже на экране
    //  - N = выше
    //
    static Result SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren);
};