#include "ElectrodeAutoIdentifier.h"
#include "ElectrodePanel.h"
#include "ElectrodeSurfaceDetector.h"

#include <vtkRenderer.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    struct Cand2D
    {
        std::array<double, 3> w{};
        double x = 0.0;
        double y = 0.0;
    };

    enum class Quadrant
    {
        LeftTop,
        RightTop,
        RightBottom,
        LeftBottom
    };

    inline bool worldToDisplay(vtkRenderer* ren, const std::array<double, 3>& w, double& outX, double& outY)
    {
        if (!ren) return false;

        double p[4]{ w[0], w[1], w[2], 1.0 };
        ren->SetWorldPoint(p);
        ren->WorldToDisplay();
        double d[3]{ 0.0, 0.0, 0.0 };
        ren->GetDisplayPoint(d);
        outX = d[0];
        outY = d[1];
        return std::isfinite(outX) && std::isfinite(outY);
    }

    inline bool inQuadrant(const Cand2D& c, double midX, double midY, Quadrant q)
    {
        switch (q)
        {
        case Quadrant::LeftTop:     return c.x <= midX && c.y >= midY;
        case Quadrant::RightTop:    return c.x >= midX && c.y >= midY;
        case Quadrant::RightBottom: return c.x >= midX && c.y <= midY;
        case Quadrant::LeftBottom:  return c.x <= midX && c.y <= midY;
        }
        return false;
    }

    inline int pickBestForQuadrant(const std::vector<Cand2D>& cands,
        const std::vector<bool>& used,
        double minX,
        double maxX,
        double minY,
        double maxY,
        double midX,
        double midY,
        Quadrant q)
    {
        double tx = midX;
        double ty = midY;
        switch (q)
        {
        case Quadrant::LeftTop:     tx = minX; ty = maxY; break;
        case Quadrant::RightTop:    tx = maxX; ty = maxY; break;
        case Quadrant::RightBottom: tx = maxX; ty = minY; break;
        case Quadrant::LeftBottom:  tx = minX; ty = minY; break;
        }

        int best = -1;
        double bestScore = std::numeric_limits<double>::max();

        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
        {
            if (used[i])
                continue;
            if (!inQuadrant(cands[i], midX, midY, q))
                continue;

            const double dx = cands[i].x - tx;
            const double dy = cands[i].y - ty;
            const double score = dx * dx + dy * dy;
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }
}

ElectrodeAutoIdentifier::Result ElectrodeAutoIdentifier::SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren)
{
    Result r;
    if (!panel || !ren)
        return r;

    auto& det = ElectrodeSurfaceDetector::instance();
    const auto centers = det.currentSphereCenters();
    if (centers.empty())
        return r;

    std::vector<Cand2D> cands;
    cands.reserve(centers.size());
    for (const auto& w : centers)
    {
        Cand2D c;
        c.w = w;
        if (worldToDisplay(ren, w, c.x, c.y))
            cands.push_back(c);
    }

    if (cands.empty())
        return r;

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    for (const auto& c : cands)
    {
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y);
        maxY = std::max(maxY, c.y);
    }

    const double midX = 0.5 * (minX + maxX);
    const double midY = 0.5 * (minY + maxY);

    std::vector<bool> used(cands.size(), false);

    const auto tryCommit = [&](ElectrodePanel::ElectrodeId id, Quadrant q, bool& outPlaced, std::array<double, 3>& outWorld)
        {
            if (panel->hasCoord(id))
                return;

            int idx = pickBestForQuadrant(cands, used, minX, maxX, minY, maxY, midX, midY, q);
            if (idx < 0)
                return;

            if (panel->commitElectrodeFromWorld(id, cands[idx].w))
            {
                used[idx] = true;
                outPlaced = true;
                outWorld = cands[idx].w;
                det.removeSphereNearWorld(ren, cands[idx].w, 25.0);
            }

        };

    // AP naming from user requirement:
    // LeftTop->R, RightTop->L, RightBottom->F, LeftBottom->N.
    tryCommit(ElectrodePanel::ElectrodeId::R, Quadrant::LeftTop, r.placedR, r.wR);
    tryCommit(ElectrodePanel::ElectrodeId::L, Quadrant::RightTop, r.placedL, r.wL);
    tryCommit(ElectrodePanel::ElectrodeId::F, Quadrant::RightBottom, r.placedF, r.wF);
    tryCommit(ElectrodePanel::ElectrodeId::N, Quadrant::LeftBottom, r.placedN, r.wN);

    return r;

}

bool ElectrodeAutoIdentifier::ShouldShowSearchRLFN(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::R)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::L)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::F)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::N)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}