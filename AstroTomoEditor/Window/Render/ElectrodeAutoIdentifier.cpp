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

ElectrodeAutoIdentifier::PrecordialResult ElectrodeAutoIdentifier::SearchV1V5(ElectrodePanel* panel, vtkRenderer* ren)
{
    PrecordialResult r;
    if (!panel || !ren)
        return r;

    const auto findPanelCoord = [&](ElectrodePanel::ElectrodeId id, std::array<double, 3>& outWorld) -> bool
        {
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
        };

    std::array<double, 3> wR{}, wL{}, wF{}, wN{};
    if (!findPanelCoord(ElectrodePanel::ElectrodeId::R, wR) ||
        !findPanelCoord(ElectrodePanel::ElectrodeId::L, wL) ||
        !findPanelCoord(ElectrodePanel::ElectrodeId::F, wF) ||
        !findPanelCoord(ElectrodePanel::ElectrodeId::N, wN))
    {
        return r;
    }

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

    double xR = 0.0, yR = 0.0, xL = 0.0, yL = 0.0, xF = 0.0, yF = 0.0, xN = 0.0, yN = 0.0;
    if (!worldToDisplay(ren, wR, xR, yR) || !worldToDisplay(ren, wL, xL, yL) ||
        !worldToDisplay(ren, wF, xF, yF) || !worldToDisplay(ren, wN, xN, yN))
    {
        return r;
    }

    const double minX = std::min(std::min(xR, xL), std::min(xF, xN));
    const double maxX = std::max(std::max(xR, xL), std::max(xF, xN));
    const double minY = std::min(std::min(yR, yL), std::min(yF, yN));
    const double maxY = std::max(std::max(yR, yL), std::max(yF, yN));

    const double cx = (maxX + minX) * 0.5;
    const double cy = (maxY + minY) * 0.5;


    qDebug() << "R: (" << xR << "; " << yR << ") " << " L: (" << xL << "; " << yL << ")";
    qDebug() << "N: (" << xN << "; " << yN << ") " << " F: (" << xF << "; " << yF << ")";

    qDebug() << "Center: (" << cx << "; " << cy << ")";

    const double dx11 = -0.70710678118;
    const double dy11 = 0.70710678118;

    std::vector<bool> used(cands.size(), false);

    struct Anchor
    {
        bool valid = false;
        double x = 0.0;
        double y = 0.0;
    };

    static constexpr double kInvSqrt2 = 0.7071067811865475;
    const auto norm0_2pi = [](double a) -> double {
        const double twoPi = 2.0 * M_PI;
        while (a < 0.0) a += twoPi;
        while (a >= twoPi) a -= twoPi;
        return a;
        };

    // часы -> угол (0 часов = 12:00, по часовой стрелке)
    const auto hourToTheta = [](double hour) -> double {
        // 12.0 часов == 0.0
        // 3.0 часа == pi/2
        // 6.0 часов == pi
        // 9.0 часов == 3pi/2
        const double h = std::fmod(hour, 12.0);
        return (h / 12.0) * (2.0 * M_PI);
        };

    // проверка попадания в сектор [h0..h1], с учетом перехода через 12
    const auto inHourSector = [&](double theta, double h0, double h1) -> bool {
        const double t0 = norm0_2pi(hourToTheta(h0));
        const double t1 = norm0_2pi(hourToTheta(h1));
        theta = norm0_2pi(theta);

        if (t0 <= t1) {
            return (theta >= t0 && theta <= t1);
        }
        else {
            // сектор пересекает 12:00 (0 рад)
            return (theta >= t0 || theta <= t1);
        }
        };

    const auto pickClosestInSectorFrom = [&](double ax, double ay, double h0, double h1) -> int {
        int best = -1;
        double bestR2 = std::numeric_limits<double>::max();

        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
        {
            if (used[i]) continue;

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
        };

    // Универсальный выбор "в конусе" от точки (ox,oy) в сторону (dx,dy)
    const auto pickInCone = [&](double ox, double oy,
        double dirx, double diry,
        double minProj,
        double coneCos,          // чем ближе к 1, тем уже конус
        double kPerpPenalty,
        double kDistPenalty) -> int
        {
            int best = -1;
            double bestScore = -std::numeric_limits<double>::max();

            // нормализуем dir на всякий случай
            const double dlen = std::sqrt(dirx * dirx + diry * diry);
            if (dlen <= 1e-9)
                return -1;
            dirx /= dlen; diry /= dlen;

            for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            {
                if (used[i]) continue;

                const double vx = cands[i].x - ox;
                const double vy = cands[i].y - oy;

                const double vlen = std::sqrt(vx * vx + vy * vy);
                if (vlen <= 1e-9)
                    continue;

                // проекция на нужное направление
                const double proj = vx * dirx + vy * diry;
                if (proj < minProj)
                    continue;

                // угол: cos(theta) = (v·dir)/|v|
                const double cosAng = proj / vlen;
                if (cosAng < coneCos)
                    continue;

                // перпендикулярная составляющая (модуль в 2D)
                // perp = |v x dir| = |vx*diry - vy*dirx|
                const double perp = std::abs(vx * diry - vy * dirx);

                const double score = proj - kPerpPenalty * perp - kDistPenalty * vlen;
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
            return best;
        };

    const auto pickV1 = [&]() -> int
        {
            // 10-11 часов от центра RLFN: вверх-влево
            const double dirx = -kInvSqrt2;
            const double diry = kInvSqrt2;

            // coneCos:
            // 0.94 ~ 20 градусов, 0.90 ~ 26 градусов, 0.85 ~ 32 градуса
            // minProj - чтобы не хватать совсем близко к центру
            const double minProj = 0.10 * std::max(1.0, (maxX - minX)); // можно подправить

            return pickInCone(cx, cy, dirx, diry,
                /*minProj=*/minProj,
                /*coneCos=*/0.88,
                /*kPerpPenalty=*/0.80,
                /*kDistPenalty=*/0.05);
        };

    const auto pickV2_fromV1 = [&](const Anchor& aV1) -> int
        {
            if (!aV1.valid)
                return -1;

            // 2-4 часа от V1: в целом вправо, допускаем отклонение вверх/вниз
            // dir=(1,0), конус регулирует насколько разрешаем "вверх/вниз"
            const double dirx = 1.0;
            const double diry = 0.0;

            // минимально "вперед" по X, чтобы не брать почти на месте
            const double minProj = 0.06 * std::max(1.0, (maxX - minX)); // можно подправить

            return pickInCone(aV1.x, aV1.y, dirx, diry,
                /*minProj=*/minProj,
                /*coneCos=*/0.80,          // широкий конус: примерно +-36 градусов (то есть 2..4 часа ок)
                /*kPerpPenalty=*/1.10,     // сильнее штрафуем уход по Y
                /*kDistPenalty=*/0.03);
        };

    const auto anchorFromPanel = [&](ElectrodePanel::ElectrodeId id) -> Anchor
        {
            Anchor a;
            std::array<double, 3> w{};
            if (!findPanelCoord(id, w))
                return a;
            if (!worldToDisplay(ren, w, a.x, a.y))
                return a;
            a.valid = true;
            return a;
        };

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


    const auto pickRightOf = [&](const Anchor& a) -> int
        {
            if (!a.valid)
                return -1;

            int best = -1;
            double bestScore = -std::numeric_limits<double>::max();

            for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            {
                if (used[i])
                    continue;

                const double dx = cands[i].x - a.x;
                const double dy = cands[i].y - a.y;
                if (dx <= 0.0)
                    continue;

                const double dist = std::sqrt(dx * dx + dy * dy);
                const double score = dx - 0.9 * std::abs(dy) - 0.1 * dist;
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }

            return best;
        };

    const auto pickBelow = [&](const Anchor& a) -> int
        {
            if (!a.valid)
                return -1;

            int best = -1;
            double bestScore = -std::numeric_limits<double>::max();

            for (int i = 0; i < static_cast<int>(cands.size()); ++i)
            {
                if (used[i])
                    continue;

                const double dx = cands[i].x - a.x;
                const double dy = cands[i].y - a.y;
                if (dy >= 0.0)
                    continue;

                const double dist = std::sqrt(dx * dx + dy * dy);
                const double score = (-dy) - 0.95 * std::abs(dx) - 0.1 * dist;
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }

            return best;
        };

    Anchor aV1 = anchorFromPanel(ElectrodePanel::ElectrodeId::V1);
    if (!aV1.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V1))
        aV1 = commitByIndex(ElectrodePanel::ElectrodeId::V1,
            pickClosestInSectorFrom(cx, cy, 10.5, 12.0),
            r.placedV1, r.wV1);

    Anchor aV2 = anchorFromPanel(ElectrodePanel::ElectrodeId::V2);
    if (!aV2.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V2))
        aV2 = commitByIndex(ElectrodePanel::ElectrodeId::V2,
            pickClosestInSectorFrom(cx, cy, 12.0, 1.5),
            r.placedV2, r.wV2);

    Anchor aV3 = anchorFromPanel(ElectrodePanel::ElectrodeId::V3);
    if (!aV3.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V3))
        aV3 = commitByIndex(ElectrodePanel::ElectrodeId::V3,
            pickClosestInSectorFrom(aV2.x, aV2.y, 3.0, 6.0),
            r.placedV3, r.wV3);

    Anchor aV4 = anchorFromPanel(ElectrodePanel::ElectrodeId::V4);
    if (!aV4.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V4))
        aV4 = commitByIndex(ElectrodePanel::ElectrodeId::V4,
            pickClosestInSectorFrom(aV3.x, aV3.y, 3.0, 6.0),
            r.placedV4, r.wV4);

    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V5))
        commitByIndex(ElectrodePanel::ElectrodeId::V5,
            pickClosestInSectorFrom(aV4.x, aV4.y, 1.0, 5.0),
            r.placedV5, r.wV5);

    return r;
}

bool ElectrodeAutoIdentifier::ShouldShowSearchV1V5(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    const bool hasRLFN =
        panel->hasCoord(ElectrodePanel::ElectrodeId::R) &&
        panel->hasCoord(ElectrodePanel::ElectrodeId::L) &&
        panel->hasCoord(ElectrodePanel::ElectrodeId::F) &&
        panel->hasCoord(ElectrodePanel::ElectrodeId::N);

    if (!hasRLFN)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V1)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V2)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V3)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V4)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V5)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}