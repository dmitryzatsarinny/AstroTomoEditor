#include "ElectrodeAutoIdentifier.h"
#include "ElectrodePanel.h"
#include "ElectrodeSurfaceDetector.h"

#include <vtkRenderer.h>
#include <vtkCamera.h>

#include <algorithm>
#include <cmath>

namespace
{
    struct Cand2D
    {
        std::array<double, 3> w{};
        double x = 0.0; // display
        double y = 0.0; // display
    };

    inline double dist2_2d(const Cand2D& a, const Cand2D& b)
    {
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    // world -> display (VTK display coords)
    inline bool worldToDisplay(vtkRenderer* ren, const std::array<double, 3>& w, double& outX, double& outY)
    {
        if (!ren) return false;

        double p[4]{ w[0], w[1], w[2], 1.0 };
        ren->SetWorldPoint(p);
        ren->WorldToDisplay();
        double d[3]{ 0,0,0 };
        ren->GetDisplayPoint(d);
        outX = d[0];
        outY = d[1];
        return std::isfinite(outX) && std::isfinite(outY);
    }

    inline bool pickFarthestPair(const std::vector<Cand2D>& v, int& outI, int& outJ)
    {
        if (v.size() < 2) return false;
        double best = -1.0;
        int bi = -1, bj = -1;

        for (int i = 0; i < (int)v.size(); ++i)
            for (int j = i + 1; j < (int)v.size(); ++j)
            {
                const double d2 = dist2_2d(v[i], v[j]);
                if (d2 > best)
                {
                    best = d2;
                    bi = i; bj = j;
                }
            }

        if (bi < 0 || bj < 0) return false;
        outI = bi; outJ = bj;
        return true;
    }

    inline void eraseTwo(std::vector<Cand2D>& v, int i, int j)
    {
        if (i > j) std::swap(i, j);
        v.erase(v.begin() + j);
        v.erase(v.begin() + i);
    }
}

ElectrodeAutoIdentifier::Result ElectrodeAutoIdentifier::SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren)
{
    Result r;
    if (!panel || !ren) return r;

    auto& det = ElectrodeSurfaceDetector::instance();
    const auto centers = det.currentSphereCenters();
    if (centers.empty()) return r;

    // Собираем кандидатов в display-space
    std::vector<Cand2D> c;
    c.reserve(centers.size());
    for (const auto& w : centers)
    {
        Cand2D cd;
        cd.w = w;
        if (worldToDisplay(ren, w, cd.x, cd.y))
            c.push_back(cd);
    }
    if (c.size() < 2) return r;

    const bool hasR = panel->hasCoord(ElectrodePanel::ElectrodeId::R);
    const bool hasL = panel->hasCoord(ElectrodePanel::ElectrodeId::L);
    const bool hasF = panel->hasCoord(ElectrodePanel::ElectrodeId::F);
    const bool hasN = panel->hasCoord(ElectrodePanel::ElectrodeId::N);

    // ---------- 1) RL ----------
    if (!(hasR && hasL))
    {
        int i = -1, j = -1;
        if (pickFarthestPair(c, i, j))
        {
            // Кто левее/правее на экране
            const Cand2D& a = c[i];
            const Cand2D& b = c[j];

            const Cand2D* left = &a;
            const Cand2D* right = &b;
            if (a.x > b.x) { left = &b; right = &a; }

            // На AP обычно “R” справа на экране, “L” слева
            if (!hasR && !hasL)
            {
                if (panel->commitElectrodeFromWorld(ElectrodePanel::ElectrodeId::R, right->w))
                {
                    r.placedR = true;
                    r.wR = right->w;
                    det.removeSphereNearWorld(ren, right->w, /*maxDistMm*/ 25.0);
                }
                if (panel->commitElectrodeFromWorld(ElectrodePanel::ElectrodeId::L, left->w))
                {
                    r.placedL = true;
                    r.wL = left->w;
                    det.removeSphereNearWorld(ren, left->w, /*maxDistMm*/ 25.0);
                }
            }
            else if (!hasR && hasL)
            {
                if (panel->commitElectrodeFromWorld(ElectrodePanel::ElectrodeId::R, left->w))
                {
                    r.placedR = true;
                    r.wR = left->w;
                    det.removeSphereNearWorld(ren, left->w, 25.0);
                }
            }
            else if (hasR && !hasL)
            {
                if (panel->commitElectrodeFromWorld(ElectrodePanel::ElectrodeId::L, right->w))
                {
                    r.placedL = true;
                    r.wL = right->w;
                    det.removeSphereNearWorld(ren, right->w, 25.0);
                }
            }

            // Чтобы FN не схватил эти же точки - убираем их из локального списка
            // (даже если commit не сработал, всё равно лучше их убрать из FN-кандидатов)
            eraseTwo(c, i, j);
        }
    }

    // ---------- 2) FN ----------
    if (!(hasF && hasN) && c.size() >= 2)
    {
        int i = -1, j = -1;
        if (pickFarthestPair(c, i, j))
        {
            const Cand2D& a = c[i];
            const Cand2D& b = c[j];

            // F ниже на экране, N выше
            const Cand2D* low = &a;
            const Cand2D* high = &b;
            if (a.y > b.y) { low = &b; high = &a; }

            if (!hasF)
            {
                if (panel->commitElectrodeFromWorld(ElectrodePanel::ElectrodeId::F, low->w))
                {
                    r.placedF = true;
                    r.wF = low->w;
                    det.removeSphereNearWorld(ren, low->w, 25.0);
                }
            }
            if (!hasN)
            {
                if (panel->commitElectrodeFromWorld(ElectrodePanel::ElectrodeId::N, high->w))
                {
                    r.placedN = true;
                    r.wN = high->w;
                    det.removeSphereNearWorld(ren, high->w, 25.0);
                }
            }
        }
    }

    return r;
}