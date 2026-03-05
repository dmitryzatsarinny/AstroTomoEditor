#include "ElectrodeAutoIdentifier.h"
#include "ElectrodeSurfaceDetector.h"

#include <vtkProp3D.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    enum class Quadrant
    {
        LeftTop,
        RightTop,
        RightBottom,
        LeftBottom
    };

    inline bool inQuadrant(const ElectrodeAutoIdentifier::Cand2D& c, double midX, double midY, Quadrant q)
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

    inline int pickBestForQuadrant(const std::vector<ElectrodeAutoIdentifier::Cand2D>& cands,
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

    inline double norm0_2pi(double a)
    {
        const double twoPi = 2.0 * M_PI;
        while (a < 0.0) a += twoPi;
        while (a >= twoPi) a -= twoPi;
        return a;
    }
    
    inline double hourToTheta(double hour)
    {
        const double h = std::fmod(hour, 12.0);
        return (h / 12.0) * (2.0 * M_PI);
    }
    
    inline bool inHourSector(double theta, double h0, double h1)
    {
        const double t0 = norm0_2pi(hourToTheta(h0));
        const double t1 = norm0_2pi(hourToTheta(h1));
        theta = norm0_2pi(theta);
    
        if (t0 <= t1)
            return (theta >= t0 && theta <= t1);
    
        return (theta >= t0 || theta <= t1);
    }
}

bool ElectrodeAutoIdentifier::WorldToDisplay(vtkRenderer* ren, const std::array<double, 3>& w, double& outX, double& outY)
{
    if (!ren)
        return false;

    double p[4]{ w[0], w[1], w[2], 1.0 };
    ren->SetWorldPoint(p);
    ren->WorldToDisplay();
    double d[3]{ 0.0, 0.0, 0.0 };
    ren->GetDisplayPoint(d);
    outX = d[0];
    outY = d[1];
    return std::isfinite(outX) && std::isfinite(outY);
}

bool ElectrodeAutoIdentifier::FindPanelCoord(const ElectrodePanel* panel, ElectrodePanel::ElectrodeId id, std::array<double, 3>& outWorld)
{
    if (!panel)
        return false;

    const auto coords = panel->coordsWorld();
    for (const auto& c : coords)
    {
        if (c.id == id)
        {
            outWorld = c.world;
            return true;
        }
    }

    return false;
}

bool ElectrodeAutoIdentifier::ComputeVolumeDisplayCenter(vtkRenderer* ren, double& cx, double& cy)
{
    if (!ren)
        return false;

    auto* props = ren->GetViewProps();
    if (!props)
        return false;

    props->InitTraversal();
    vtkProp* prop = nullptr;
    while ((prop = props->GetNextProp()))
    {
        auto* prop3d = vtkProp3D::SafeDownCast(prop);
        if (!prop3d)
            continue;

        double b[6]{};
        prop3d->GetBounds(b);
        if (!std::isfinite(b[0]) || !std::isfinite(b[1]) || !std::isfinite(b[2]) ||
            !std::isfinite(b[3]) || !std::isfinite(b[4]) || !std::isfinite(b[5]))
        {
            continue;
        }

        std::array<double, 3> c{
            0.5 * (b[0] + b[1]),
            0.5 * (b[2] + b[3]),
            0.5 * (b[4] + b[5])
        };

        return WorldToDisplay(ren, c, cx, cy);
    }

    return false;
}

std::vector<ElectrodeAutoIdentifier::Cand2D> ElectrodeAutoIdentifier::CollectDisplayCandidates(vtkRenderer* ren, const std::vector<std::array<double, 3>>& centers)
{
    std::vector<Cand2D> cands;
    cands.reserve(centers.size());
    for (const auto& w : centers)
    {
        Cand2D c;
        c.w = w;
        if (WorldToDisplay(ren, w, c.x, c.y))
            cands.push_back(c);
    }
    return cands;
}

int ElectrodeAutoIdentifier::PickClosestInSectorFrom(const std::vector<Cand2D>& cands,
    const std::vector<bool>& used,
    double ax,
    double ay,
    double h0,
    double h1)
{
    int best = -1;
    double bestR2 = std::numeric_limits<double>::max();

    for (int i = 0; i < static_cast<int>(cands.size()); ++i)
    {
        if (used[i])
            continue;

        const double dx = cands[i].x - ax;
        const double dy = cands[i].y - ay;

        double theta = std::atan2(dx, dy);
        theta = norm0_2pi(theta);

        if (!inHourSector(theta, h0, h1))
            continue;

        const double r2 = dx * dx + dy * dy;
        if (r2 < bestR2)
        {
            bestR2 = r2;
            best = i;
        }
    }

    return best;
}

ElectrodeAutoIdentifier::Anchor ElectrodeAutoIdentifier::AnchorFromPanel(const ElectrodePanel* panel, vtkRenderer* ren, ElectrodePanel::ElectrodeId id)
{
    Anchor a;
    std::array<double, 3> w{};
    if (!FindPanelCoord(panel, id, w))
        return a;
    if (!WorldToDisplay(ren, w, a.x, a.y))
        return a;
    a.valid = true;
    return a;
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

    const auto cands = CollectDisplayCandidates(ren, centers);
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

ElectrodeAutoIdentifier::PrecordialResult ElectrodeAutoIdentifier::SearchV1V6(ElectrodePanel* panel, vtkRenderer* ren)
{
    PrecordialResult r;
    if (!panel || !ren)
        return r;

    auto& det = ElectrodeSurfaceDetector::instance();
    const auto centers = det.currentSphereCenters();
    if (centers.empty())
        return r;

    const auto cands = CollectDisplayCandidates(ren, centers);

    if (cands.empty())
        return r;

    double cx = 0.0;
    double cy = 0.0;
    if (!ComputeVolumeDisplayCenter(ren, cx, cy))
        return r;

    std::vector<bool> used(cands.size(), false);

    const auto commitByIndex = [&](ElectrodePanel::ElectrodeId id, int idx, bool& outPlaced, std::array<double, 3>& outWorld) -> Anchor
        {
            Anchor a;
            if (idx < 0 || idx >= static_cast<int>(cands.size()))
                return a;

            if (panel->commitElectrodeFromWorld(id, cands[idx].w))
            {
                used[idx] = true;
                outPlaced = true;
                outWorld = cands[idx].w;
                a.valid = true;
                a.x = cands[idx].x;
                a.y = cands[idx].y;
                det.removeSphereNearWorld(ren, cands[idx].w, 25.0);
            }

            return a;
        };


    Anchor aV1 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V1);
    if (!aV1.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V1))
        aV1 = commitByIndex(ElectrodePanel::ElectrodeId::V1,
            PickClosestInSectorFrom(cands, used, cx, cy, 10.5, 12.0),
            r.placedV1, r.wV1);

    Anchor aV2 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V2);
    if (!aV2.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V2))
        aV2 = commitByIndex(ElectrodePanel::ElectrodeId::V2,
            PickClosestInSectorFrom(cands, used, cx, cy, 12.0, 1.5),
            r.placedV2, r.wV2);

    Anchor aV3 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V3);
    if (!aV3.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V3) && aV2.valid)
        aV3 = commitByIndex(ElectrodePanel::ElectrodeId::V3,
            PickClosestInSectorFrom(cands, used, aV2.x, aV2.y, 3.0, 6.0),
            r.placedV3, r.wV3);

    Anchor aV4 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V4);
    if (!aV4.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V4) && aV3.valid)
        aV4 = commitByIndex(ElectrodePanel::ElectrodeId::V4,
            PickClosestInSectorFrom(cands, used, aV3.x, aV3.y, 3.0, 6.0),
            r.placedV4, r.wV4);

    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V5) && aV4.valid)
    {
        commitByIndex(ElectrodePanel::ElectrodeId::V5,
            PickClosestInSectorFrom(cands, used, aV4.x, aV4.y, 1.0, 5.0),
            r.placedV5, r.wV5);
    }
    return r;
}

bool ElectrodeAutoIdentifier::ShouldShowSearchV1V6(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V1)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V2)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V3)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V4)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V5)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V6)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}