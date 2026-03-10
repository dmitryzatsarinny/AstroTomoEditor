#include "ElectrodeAutoIdentifier.h"
#include "ElectrodeSurfaceDetector.h"

#include <vtkProp3D.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkCamera.h>
#include <vtkLineSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkCoordinate.h>
#include <vtkRenderWindow.h>
#include <vtkRendererCollection.h>
#include <vtkNamedColors.h>
#include <vtkInformation.h>
#include <vtkInformationStringKey.h>
#include <vtkObjectFactory.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkActor2D.h>
#include <vtkProperty2D.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    static std::vector<vtkSmartPointer<vtkProp>> gSectorDebugProps;

    void ClearDebugProps(vtkRenderer* ren)
    {
        if (!ren)
            return;

        for (auto& p : gSectorDebugProps)
        {
            if (p)
                ren->RemoveViewProp(p);
        }
        gSectorDebugProps.clear();
    }

    void AddDebugProp(vtkRenderer* ren, vtkProp* prop)
    {
        if (!ren || !prop)
            return;

        ren->AddViewProp(prop);
        gSectorDebugProps.push_back(prop);
    }

    vtkSmartPointer<vtkActor> MakeLineActor(
        const std::array<double, 3>& p0,
        const std::array<double, 3>& p1,
        double r, double g, double b,
        double lineWidth = 3.0)
    {
        vtkNew<vtkLineSource> line;
        line->SetPoint1(p0[0], p0[1], p0[2]);
        line->SetPoint2(p1[0], p1[1], p1[2]);
        line->Update();

        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputConnection(line->GetOutputPort());

        vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(r, g, b);
        actor->GetProperty()->SetLineWidth(lineWidth);
        actor->PickableOff();

        return actor;
    }



    vtkSmartPointer<vtkTextActor> MakeScreenLabel(
        vtkRenderer* ren,
        const std::array<double, 3>& worldPos,
        const QString& text,
        double r, double g, double b)
    {
        if (!ren)
            return nullptr;

        double dx = 0.0, dy = 0.0;
        if (!ElectrodeAutoIdentifier::WorldToDisplay(ren, worldPos, dx, dy))
            return nullptr;

        vtkSmartPointer<vtkTextActor> txt = vtkSmartPointer<vtkTextActor>::New();
        txt->SetInput(text.toUtf8().constData());
        txt->SetPosition(dx + 8.0, dy + 8.0);
        txt->GetTextProperty()->SetFontSize(16);
        txt->GetTextProperty()->SetColor(r, g, b);
        txt->GetTextProperty()->SetBold(1);

        return txt;
    }



    static vtkSmartPointer<vtkActor2D> MakeScreenLineActor(
        double x0, double y0,
        double x1, double y1,
        double r, double g, double b,
        double lineWidth = 3.0)
    {
        vtkNew<vtkPoints> pts;
        pts->InsertNextPoint(x0, y0, 0.0);
        pts->InsertNextPoint(x1, y1, 0.0);

        vtkNew<vtkCellArray> lines;
        vtkIdType ids[2] = { 0, 1 };
        lines->InsertNextCell(2, ids);

        vtkNew<vtkPolyData> poly;
        poly->SetPoints(pts);
        poly->SetLines(lines);

        vtkNew<vtkPolyDataMapper2D> mapper;
        mapper->SetInputData(poly);

        vtkSmartPointer<vtkActor2D> actor = vtkSmartPointer<vtkActor2D>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(r, g, b);
        actor->GetProperty()->SetLineWidth(lineWidth);

        return actor;
    }
}



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

    inline std::array<double, 3> Sub3(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    }

    inline double Dot3(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    inline double Norm3(const std::array<double, 3>& v)
    {
        return std::sqrt(Dot3(v, v));
    }

    constexpr double kAutoDetectConeAngleDeg = 85.0;

    inline bool IsWorldWithinCameraCone(
        const std::array<double, 3>& volCenterW,
        const std::array<double, 3>& sphereW,
        const std::array<double, 3>& camPosW,
        double maxAngleDeg)
    {
        const auto vs = Sub3(sphereW, volCenterW);   // от центра объема к сфере
        const auto vc = Sub3(camPosW, volCenterW);   // от центра объема к камере

        const double ns = Norm3(vs);
        const double nc = Norm3(vc);

        if (ns < 1e-9 || nc < 1e-9)
            return false;

        double cosA = Dot3(vs, vc) / (ns * nc);
        cosA = std::clamp(cosA, -1.0, 1.0);

        const double angleDeg = std::acos(cosA) * 180.0 / M_PI;
        return angleDeg <= maxAngleDeg;
    }

    static std::vector<ElectrodeAutoIdentifier::Cand2D> CollectCandidatesWithinCameraCone(
        vtkRenderer* ren,
        const std::vector<std::array<double, 3>>& centers,
        const std::array<double, 3>& volCenterW,
        double maxAngleDeg = kAutoDetectConeAngleDeg)
    {
        std::vector<ElectrodeAutoIdentifier::Cand2D> out;
        if (!ren)
            return out;

        auto* cam = ren->GetActiveCamera();
        if (!cam)
            return out;

        double camPos[3]{ 0.0, 0.0, 0.0 };
        cam->GetPosition(camPos);

        const std::array<double, 3> camPosW{ camPos[0], camPos[1], camPos[2] };

        out.reserve(centers.size());

        for (const auto& w : centers)
        {
            if (!IsWorldWithinCameraCone(volCenterW, w, camPosW, maxAngleDeg))
                continue;

            ElectrodeAutoIdentifier::Cand2D c;
            c.w = w;
            if (ElectrodeAutoIdentifier::WorldToDisplay(ren, w, c.x, c.y))
                out.push_back(c);
        }

        return out;
    }

    static std::vector<ElectrodeAutoIdentifier::Cand2D> CollectRLFNCandidatesFiltered(
        vtkRenderer* ren,
        const std::vector<std::array<double, 3>>& centers,
        const std::array<double, 3>& volCenterW)
    {
        return CollectCandidatesWithinCameraCone(ren, centers, volCenterW);
    }

    static int PickTopMostSide(
        const std::vector<ElectrodeAutoIdentifier::Cand2D>& cands,
        const std::vector<bool>& used,
        bool leftSide,
        double midX)
    {
        int best = -1;
        double bestY = -std::numeric_limits<double>::max();
        double bestSideTie = leftSide ? std::numeric_limits<double>::max()
            : -std::numeric_limits<double>::max();

        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
        {
            if (used[i])
                continue;

            const bool isLeft = (cands[i].x <= midX);
            if (isLeft != leftSide)
                continue;

            // Ищем самый верхний
            if (cands[i].y > bestY + 1e-9)
            {
                bestY = cands[i].y;
                best = i;
                bestSideTie = cands[i].x;
            }
            else if (std::abs(cands[i].y - bestY) <= 1e-9)
            {
                // tie-break:
                // слева хотим еще левее
                // справа хотим еще правее
                if (leftSide)
                {
                    if (cands[i].x < bestSideTie)
                    {
                        best = i;
                        bestSideTie = cands[i].x;
                    }
                }
                else
                {
                    if (cands[i].x > bestSideTie)
                    {
                        best = i;
                        bestSideTie = cands[i].x;
                    }
                }
            }
        }

        return best;
    }

    static int PickBottomMostSide(
        const std::vector<ElectrodeAutoIdentifier::Cand2D>& cands,
        const std::vector<bool>& used,
        bool leftSide,
        double midX)
    {
        int best = -1;
        double bestY = std::numeric_limits<double>::max();
        double bestSideTie = leftSide ? std::numeric_limits<double>::max()
            : -std::numeric_limits<double>::max();

        for (int i = 0; i < static_cast<int>(cands.size()); ++i)
        {
            if (used[i])
                continue;

            const bool isLeft = (cands[i].x <= midX);
            if (isLeft != leftSide)
                continue;

            // Ищем самый нижний
            if (cands[i].y < bestY - 1e-9)
            {
                bestY = cands[i].y;
                best = i;
                bestSideTie = cands[i].x;
            }
            else if (std::abs(cands[i].y - bestY) <= 1e-9)
            {
                // tie-break:
                // слева хотим еще левее
                // справа хотим еще правее
                if (leftSide)
                {
                    if (cands[i].x < bestSideTie)
                    {
                        best = i;
                        bestSideTie = cands[i].x;
                    }
                }
                else
                {
                    if (cands[i].x > bestSideTie)
                    {
                        best = i;
                        bestSideTie = cands[i].x;
                    }
                }
            }
        }

        return best;
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

    const bool ok = std::isfinite(outX) && std::isfinite(outY);

    return ok;
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

bool ElectrodeAutoIdentifier::ComputeVolumeWorldCenter(vtkRenderer* ren, std::array<double, 3>& outC)
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
        if (!std::isfinite(b[0]) || !std::isfinite(b[1]) ||
            !std::isfinite(b[2]) || !std::isfinite(b[3]) ||
            !std::isfinite(b[4]) || !std::isfinite(b[5]))
        {
            continue;
        }

        outC = {
            0.5 * (b[0] + b[1]),
            0.5 * (b[2] + b[3]),
            0.5 * (b[4] + b[5])
        };

        return true;
    }

    return false;
}

std::vector<ElectrodeAutoIdentifier::Cand2D> ElectrodeAutoIdentifier::CollectDisplayCandidates(
    vtkRenderer* ren,
    const std::vector<std::array<double, 3>>& centers)
{
    if (!ren)
        return {};

    std::array<double, 3> volCenterW{};
    if (ComputeVolumeWorldCenter(ren, volCenterW))
        return CollectCandidatesWithinCameraCone(ren, centers, volCenterW);

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

static double Dist(const std::array<double, 3>& a,
    const std::array<double, 3>& b)
{
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void DrawSectorDebug2D(
    vtkRenderer* ren,
    double ax, double ay,
    double h0, double h1,
    double rayLen = 140.0)
{
    if (!ren)
        return;

    ClearDebugProps(ren);

    const double t0 = norm0_2pi(hourToTheta(h0));
    const double t1 = norm0_2pi(hourToTheta(h1));

    // Важно:
    // у тебя theta = atan2(dx, dy)
    // значит:
    // dx = sin(theta) * L
    // dy = cos(theta) * L
    const double x0 = ax + std::sin(t0) * rayLen;
    const double y0 = ay + std::cos(t0) * rayLen;

    const double x1 = ax + std::sin(t1) * rayLen;
    const double y1 = ay + std::cos(t1) * rayLen;

    // центральный луч сектора
    double tm = t0;
    if (t0 <= t1)
        tm = 0.5 * (t0 + t1);
    else
    {
        double a = t0;
        double b = t1 + 2.0 * M_PI;
        tm = 0.5 * (a + b);
        tm = norm0_2pi(tm);
    }

    const double xm = ax + std::sin(tm) * (rayLen * 0.9);
    const double ym = ay + std::cos(tm) * (rayLen * 0.9);

    // границы сектора
    auto l0 = MakeScreenLineActor(ax, ay, x0, y0, 0.0, 1.0, 1.0, 4.0);
    auto l1 = MakeScreenLineActor(ax, ay, x1, y1, 1.0, 0.5, 0.0, 4.0);
    auto lm = MakeScreenLineActor(ax, ay, xm, ym, 0.2, 1.0, 0.2, 3.0);

    AddDebugProp(ren, l0);
    AddDebugProp(ren, l1);
    AddDebugProp(ren, lm);

    // подписи
    std::array<double, 3> p0w{ 0.0, 0.0, 0.0 };
    std::array<double, 3> p1w{ 0.0, 0.0, 0.0 };
    std::array<double, 3> pmw{ 0.0, 0.0, 0.0 };

    auto txtAnchor = vtkSmartPointer<vtkTextActor>::New();
    txtAnchor->SetInput("ANCHOR");
    txtAnchor->SetPosition(ax + 8.0, ay + 8.0);
    txtAnchor->GetTextProperty()->SetFontSize(16);
    txtAnchor->GetTextProperty()->SetColor(0.2, 1.0, 0.2);
    txtAnchor->GetTextProperty()->SetBold(1);
    AddDebugProp(ren, txtAnchor);

    auto txt0 = vtkSmartPointer<vtkTextActor>::New();
    txt0->SetInput(QString("h=%1").arg(h0, 0, 'f', 1).toUtf8().constData());
    txt0->SetPosition(x0 + 6.0, y0 + 6.0);
    txt0->GetTextProperty()->SetFontSize(16);
    txt0->GetTextProperty()->SetColor(0.0, 1.0, 1.0);
    txt0->GetTextProperty()->SetBold(1);
    AddDebugProp(ren, txt0);

    auto txt1 = vtkSmartPointer<vtkTextActor>::New();
    txt1->SetInput(QString("h=%1").arg(h1, 0, 'f', 1).toUtf8().constData());
    txt1->SetPosition(x1 + 6.0, y1 + 6.0);
    txt1->GetTextProperty()->SetFontSize(16);
    txt1->GetTextProperty()->SetColor(1.0, 0.5, 0.0);
    txt1->GetTextProperty()->SetBold(1);
    AddDebugProp(ren, txt1);

    if (auto* rw = ren->GetRenderWindow())
        rw->Render();
}


struct SectorSearchParams
{
    double h0 = 0.0;
    double h1 = 0.0;
    double minRadialTolerance = 8.0;
    double radialToleranceFactor = 0.18;
    bool useAnchorHemisphere = true;
};

static ElectrodeAutoIdentifier::Anchor CommitByWorld(ElectrodePanel* panel,
    vtkRenderer* ren,
    ElectrodeSurfaceDetector& det,
    ElectrodePanel::ElectrodeId id,
    const std::array<double, 3>& w)
{
    ElectrodeAutoIdentifier::Anchor a;
    if (!panel->commitElectrodeFromWorld(id, w, false))
        return a;

    a.w = w;
    a.valid = ElectrodeAutoIdentifier::WorldToDisplay(ren, w, a.x, a.y);
    det.removeSphereNearWorld(ren, w, 25.0);
    return a;
}

static ElectrodeAutoIdentifier::Anchor CommitFromSector(
    ElectrodePanel* panel,
    vtkRenderer* ren,
    ElectrodeSurfaceDetector& det,
    ElectrodePanel::ElectrodeId id,
    const std::array<double, 3>& volumeCenterW,
    const std::array<double, 3>& anchorW,
    double ax,
    double ay,
    const SectorSearchParams& params)
{
    ElectrodeAutoIdentifier::Anchor a;


    const auto centersNow = det.currentSphereCenters();
    if (centersNow.empty())
        return a;

    const auto cands = ElectrodeAutoIdentifier::CollectDisplayCandidates(ren, centersNow);
    if (cands.empty())
        return a;

    const double rAnchor = Dist(volumeCenterW, anchorW);
    double minR = 0.0;
    double maxR = std::numeric_limits<double>::infinity();
    bool useAnchorHemisphere = false;

    if (rAnchor > 1e-3)
    {
        const double radialTolerance = std::max(params.minRadialTolerance, rAnchor * params.radialToleranceFactor);
        minR = std::max(0.0, rAnchor - radialTolerance);
        maxR = rAnchor + radialTolerance;
        useAnchorHemisphere = params.useAnchorHemisphere;
    }


    DrawSectorDebug2D(ren, ax, ay, params.h0, params.h1);

    int idx = ElectrodeAutoIdentifier::PickClosestInSectorFrom(
        cands,
        ax, ay, params.h0, params.h1,
        volumeCenterW, minR, maxR,
        anchorW, useAnchorHemisphere);


    if (idx < 0 && useAnchorHemisphere)
    {
        const SectorSearchParams fallback{ params.h0, params.h1, params.minRadialTolerance + 7.0, std::max(0.35, params.radialToleranceFactor), false };
        return CommitFromSector(panel, ren, det, id, volumeCenterW, anchorW, ax, ay, fallback);
    }

    if (idx < 0 || idx >= static_cast<int>(cands.size()))
        return a;

    return CommitByWorld(panel, ren, det, id, cands[idx].w);
}

static void RotateAzimuthToCenterX(ElectrodePanel* panel,
    vtkRenderer* ren,
    ElectrodePanel::ElectrodeId id,
    int iters = 2)
{
    auto* cam = ren->GetActiveCamera();
    if (!cam) return;

    auto* rw = ren->GetRenderWindow();
    if (!rw) return;

    for (int it = 0; it < iters; ++it)
    {
        ElectrodeAutoIdentifier::Anchor a0 = ElectrodeAutoIdentifier::AnchorFromPanel(panel, ren, id);
        if (!a0.valid) return;

        double cxD = 0.0, cyD = 0.0;
        if (!ElectrodeAutoIdentifier::ComputeVolumeDisplayCenter(ren, cxD, cyD))
            return;

        const double err = (a0.x - cxD);
        if (std::abs(err) < 1.0)
            return;

        constexpr double testDeg = 1.0;
        cam->Azimuth(+testDeg);
        cam->OrthogonalizeViewUp();
        ren->ResetCameraClippingRange();
        rw->Render();

        const ElectrodeAutoIdentifier::Anchor a1 = ElectrodeAutoIdentifier::AnchorFromPanel(panel, ren, id);

        cam->Azimuth(-testDeg);
        cam->OrthogonalizeViewUp();
        ren->ResetCameraClippingRange();
        rw->Render();

        if (!a1.valid) return;

        const double dxPerDeg = (a1.x - a0.x) / testDeg;
        if (std::abs(dxPerDeg) < 1e-6)
            return;

        double deltaAz = -err / dxPerDeg;
        deltaAz = std::clamp(deltaAz, -25.0, +25.0);

        cam->Azimuth(deltaAz);
        cam->OrthogonalizeViewUp();
        ren->ResetCameraClippingRange();
        rw->Render();
    }
}

static ElectrodeAutoIdentifier::Anchor PlaceSequential(
    ElectrodePanel* panel,
    vtkRenderer* ren,
    ElectrodeSurfaceDetector& det,
    ElectrodePanel::ElectrodeId id,
    ElectrodeAutoIdentifier::Anchor prevAnchor,
    const std::array<double, 3>& volumeCenterW,
    const SectorSearchParams& params)
{

    auto a = ElectrodeAutoIdentifier::AnchorFromPanel(panel, ren, id);


    if (!a.valid && !panel->hasCoord(id) && prevAnchor.valid)
    {
        a = CommitFromSector(panel, ren, det, id,
            volumeCenterW,
            prevAnchor.w, prevAnchor.x, prevAnchor.y,
            params);
    }

    RotateAzimuthToCenterX(panel, ren, id);

    a = ElectrodeAutoIdentifier::AnchorFromPanel(panel, ren, id);

    return a;
}

int ElectrodeAutoIdentifier::PickClosestInSectorFrom(
    const std::vector<Cand2D>& cands,
    double ax, double ay,
    double h0, double h1,
    const std::array<double, 3>& volumeCenterW,
    double minRadiusFromCenter,
    double maxRadiusFromCenter,
    const std::array<double, 3>& anchorWorld,
    bool useAnchorHemisphere)
{
    int best = -1;

    const std::array<double, 3> anchorVec{
        anchorWorld[0] - volumeCenterW[0],
        anchorWorld[1] - volumeCenterW[1],
        anchorWorld[2] - volumeCenterW[2]
    };

    const double anchorNorm = std::sqrt(
        anchorVec[0] * anchorVec[0] +
        anchorVec[1] * anchorVec[1] +
        anchorVec[2] * anchorVec[2]);

    const bool useAngleMode = (anchorNorm >= 1e-9);

    double bestCosAngle = -std::numeric_limits<double>::infinity();
    double bestR2 = std::numeric_limits<double>::max();
    double bestWorldDist = std::numeric_limits<double>::max();

    for (int i = 0; i < static_cast<int>(cands.size()); ++i)
    {
        const double dx = cands[i].x - ax;
        const double dy = cands[i].y - ay;

        double theta = std::atan2(dx, dy);
        theta = norm0_2pi(theta);

        const double thetaDeg = theta * 180.0 / M_PI;
        const bool passSector = inHourSector(theta, h0, h1);

        const double rFromCenter = Dist(volumeCenterW, cands[i].w);
        const bool passRadius = (rFromCenter >= minRadiusFromCenter && rFromCenter <= maxRadiusFromCenter);

        const std::array<double, 3> candVec{
            cands[i].w[0] - volumeCenterW[0],
            cands[i].w[1] - volumeCenterW[1],
            cands[i].w[2] - volumeCenterW[2]
        };

        const double candNorm = std::sqrt(
            candVec[0] * candVec[0] +
            candVec[1] * candVec[1] +
            candVec[2] * candVec[2]);

        const double dot =
            anchorVec[0] * candVec[0] +
            anchorVec[1] * candVec[1] +
            anchorVec[2] * candVec[2];

        const bool passHemisphere = (!useAnchorHemisphere || dot > 0.0);

        const double d2 = dx * dx + dy * dy;
        const double worldDist = Dist(cands[i].w, anchorWorld);

        double cosAngle = -2.0;
        double angleDeg = 999.0;

        if (useAngleMode && candNorm >= 1e-9)
        {
            cosAngle = dot / (anchorNorm * candNorm);
            cosAngle = std::clamp(cosAngle, -1.0, 1.0);
            angleDeg = std::acos(cosAngle) * 180.0 / M_PI;
        }


        if (!passSector)
        {
            continue;
        }

        if (!passRadius)
        {
            continue;
        }

        if (!passHemisphere)
        {
            continue;
        }

        if (!useAngleMode)
        {
            // Стартовая точка: опорный вектор нулевой, выбираем по экранной близости
            if (d2 < bestR2)
            {
                bestR2 = d2;
                best = i;
            }
        }
        else
        {
            // Основной режим: минимальный угол, а при равенстве - ближе по миру
            if (cosAngle > bestCosAngle + 1e-9 ||
                (std::abs(cosAngle - bestCosAngle) <= 1e-9 && worldDist < bestWorldDist))
            {
                bestCosAngle = cosAngle;
                bestWorldDist = worldDist;
                best = i;
            }
        }
    }

    return best;
}

ElectrodeAutoIdentifier::Anchor ElectrodeAutoIdentifier::AnchorFromPanel(const ElectrodePanel* panel, vtkRenderer* ren, ElectrodePanel::ElectrodeId id)
{
    Anchor a;
    if (!FindPanelCoord(panel, id, a.w))
        return a;
    if (!WorldToDisplay(ren, a.w, a.x, a.y))
        return a;
    a.valid = true;
    return a;
}

ElectrodeAutoIdentifier::Result ElectrodeAutoIdentifier::SearchRLFN(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI)
{
    Result r;
    if (!panel || !ren)
        return r;

    auto& det = ElectrodeSurfaceDetector::instance();
    const auto centers = det.currentSphereCenters();
    if (centers.empty())
        return r;

    const std::array<double, 3> volCenterW{
        DI.VolumeOriginX + DI.VolumeCenterX,
        DI.VolumeOriginY + DI.VolumeCenterY,
        DI.VolumeOriginZ + DI.VolumeCenterZ
    };

    // Берем только кандидатов на полусфере, обращенной к камере
    const auto cands = CollectRLFNCandidatesFiltered(ren, centers, volCenterW);
    if (cands.empty())
        return r;

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();

    for (const auto& c : cands)
    {
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
    }

    const double midX = 0.5 * (minX + maxX);
    std::vector<bool> used(cands.size(), false);

    const auto tryCommitIndex =
        [&](ElectrodePanel::ElectrodeId id, int idx, bool& outPlaced, std::array<double, 3>& outWorld)
        {
            if (panel->hasCoord(id))
                return;

            if (idx < 0 || idx >= static_cast<int>(cands.size()))
                return;

            if (panel->commitElectrodeFromWorld(id, cands[idx].w, false))
            {
                used[idx] = true;
                outPlaced = true;
                outWorld = cands[idx].w;
                det.removeSphereNearWorld(ren, cands[idx].w, 25.0);
            }
        };

    // AP:
    // R = левый верх
    // L = правый верх
    // F = правый низ
    // N = левый низ

    tryCommitIndex(ElectrodePanel::ElectrodeId::R,
        PickTopMostSide(cands, used, true, midX),
        r.placedR, r.wR);

    tryCommitIndex(ElectrodePanel::ElectrodeId::L,
        PickTopMostSide(cands, used, false, midX),
        r.placedL, r.wL);

    tryCommitIndex(ElectrodePanel::ElectrodeId::F,
        PickBottomMostSide(cands, used, false, midX),
        r.placedF, r.wF);

    tryCommitIndex(ElectrodePanel::ElectrodeId::N,
        PickBottomMostSide(cands, used, true, midX),
        r.placedN, r.wN);

    return r;
}

ElectrodeAutoIdentifier::CameraState ElectrodeAutoIdentifier::SaveCamera(vtkRenderer* ren)
{
    CameraState s;
    if (!ren) return s;
    auto* cam = ren->GetActiveCamera();
    if (!cam) return s;

    cam->GetPosition(s.pos);
    cam->GetFocalPoint(s.focal);
    cam->GetViewUp(s.viewUp);
    s.parallelProjection = cam->GetParallelProjection() != 0;
    s.parallelScale = cam->GetParallelScale();
    return s;
}

void ElectrodeAutoIdentifier::RestoreCamera(vtkRenderer* ren, const CameraState& s, bool render)
{
    if (!ren) return;
    auto* cam = ren->GetActiveCamera();
    if (!cam) return;

    cam->SetPosition(s.pos);
    cam->SetFocalPoint(s.focal);
    cam->SetViewUp(s.viewUp);
    cam->SetParallelProjection(s.parallelProjection ? 1 : 0);
    cam->SetParallelScale(s.parallelScale);
    cam->OrthogonalizeViewUp();
    ren->ResetCameraClippingRange();

    if (render)
        if (auto* rw = ren->GetRenderWindow()) rw->Render();
}

void ElectrodeAutoIdentifier::TurnToPA(vtkRenderer* ren, bool render)
{
    if (!ren) return;
    auto* cam = ren->GetActiveCamera();
    if (!cam) return;

    cam->Azimuth(180.0);
    cam->OrthogonalizeViewUp();
    ren->ResetCameraClippingRange();

    if (render)
        if (auto* rw = ren->GetRenderWindow()) rw->Render();
}

int ElectrodeAutoIdentifier::PickLeft(const std::vector<Cand2D>& cands)
{
    if (cands.empty()) return -1;

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

    const double rangeX = std::max(1.0, maxX - minX);
    const double rangeY = std::max(1.0, maxY - minY);

    int best = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (int i = 0; i < static_cast<int>(cands.size()); ++i)
    {
        const double nx = (cands[i].x - minX) / rangeX;       // 0 = самый левый

        if (nx < bestScore)
        {
            bestScore = nx;
            best = i;
        }
    }

    return best;
}

int ElectrodeAutoIdentifier::PickLeftBottom(const std::vector<Cand2D>& cands)
{
    if (cands.empty()) return -1;

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();

    for (const auto& c : cands)
    {
        minX = std::min(minX, c.x); maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y); maxY = std::max(maxY, c.y);
    }

    const double rangeX = std::max(1.0, maxX - minX);
    const double rangeY = std::max(1.0, maxY - minY);

    int best = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (int i = 0; i < (int)cands.size(); ++i)
    {
        const double nx = (cands[i].x - minX) / rangeX; // 0 = левее
        const double ny = (cands[i].y - minY) / rangeY; // 0 = ниже
        const double score = nx + ny;                   // “левый низ”
        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

int ElectrodeAutoIdentifier::PickRightBottom(const std::vector<Cand2D>& cands)
{
    if (cands.empty()) return -1;

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

    const double rangeX = std::max(1.0, maxX - minX);
    const double rangeY = std::max(1.0, maxY - minY);

    int best = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (int i = 0; i < (int)cands.size(); ++i)
    {
        const double nxRight = (maxX - cands[i].x) / rangeX; // 0 = самый правый
        const double ny = (cands[i].y - minY) / rangeY;      // 0 = самый нижний

        const double score = nxRight + ny;                   // “правый низ”

        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }

    return best;
}

ElectrodeAutoIdentifier::Anchor ElectrodeAutoIdentifier::CommitByIndex(
    ElectrodePanel* panel, vtkRenderer* ren, ElectrodeSurfaceDetector& det,
    ElectrodePanel::ElectrodeId id,
    const std::vector<Cand2D>& cands, int idx,
    bool& outPlaced, std::array<double, 3>& outWorld)
{
    Anchor a;
    if (!panel || !ren) return a;
    if (idx < 0 || idx >= (int)cands.size()) return a;

    const auto& w = cands[idx].w;

    if (panel->commitElectrodeFromWorld(id, w, false))
    {
        outPlaced = true;
        outWorld = w;
        a.w = w;
        a.valid = WorldToDisplay(ren, w, a.x, a.y);
        det.removeSphereNearWorld(ren, w, 25.0);
    }
    return a;
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

void ElectrodeAutoIdentifier::SearchV1V6(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI)
{
    if (!panel || !ren)
        return;

    auto& det = ElectrodeSurfaceDetector::instance();

    std::array<double, 3> volCenterW{ DI.VolumeOriginX + DI.VolumeCenterX, DI.VolumeOriginY + DI.VolumeCenterY, DI.VolumeOriginZ + DI.VolumeCenterZ };
    std::array<int, 3> centerIJK{};
    std::array<double, 3> centerFirstNonZeroW{};
    bool hasCenterFirstNonZero = panel->pickAtViewportCenter(centerIJK, centerFirstNonZeroW);

    double cx = 0.0;
    double cy = 0.0;
    if (hasCenterFirstNonZero)
    {
        if (!WorldToDisplay(ren, centerFirstNonZeroW, cx, cy))
            hasCenterFirstNonZero = false;
    }
    if (!hasCenterFirstNonZero && !ComputeVolumeDisplayCenter(ren, cx, cy))
    {
        return;
    }

    const std::array<double, 3>& seedWorld = hasCenterFirstNonZero ? centerFirstNonZeroW : volCenterW;

    Anchor aV1 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V1);
    if (!aV1.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V1))
        aV1 = CommitFromSector(panel, ren, det, ElectrodePanel::ElectrodeId::V1,
            volCenterW,
            seedWorld, cx, cy,
            SectorSearchParams{ 10.5, 12.0, 8.0, 0.18, true });

    Anchor aV2 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V2);
    if (!aV2.valid && !panel->hasCoord(ElectrodePanel::ElectrodeId::V2))
        aV2 = CommitFromSector(panel, ren, det, ElectrodePanel::ElectrodeId::V2,
            volCenterW,
            seedWorld, cx, cy,
            SectorSearchParams{ 12.0, 1.5, 8.0, 0.18, true });


    RotateAzimuthToCenterX(panel, ren, ElectrodePanel::ElectrodeId::V2);
    aV2 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V2);

    Anchor aV3 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V3, aV2, volCenterW, SectorSearchParams{ 3.0, 6.0, 8.0, 0.18, true });
    Anchor aV4 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V4, aV3, volCenterW, SectorSearchParams{ 2.0, 6.0, 8.0, 0.18, true });
    Anchor aV5 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V5, aV4, volCenterW, SectorSearchParams{ 1.0, 5.0, 8.0, 0.18, true });
    (void)PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V6, aV5, volCenterW, SectorSearchParams{ 2.0, 4.0, 8.0, 0.18, true });
    ClearDebugProps(ren);
}

static void TurnPA(vtkRenderer* ren)
{
    if (!ren) return;
    auto* cam = ren->GetActiveCamera();
    if (!cam) return;

    cam->Azimuth(180.0);
    cam->OrthogonalizeViewUp();
    ren->ResetCameraClippingRange();

    if (auto* rw = ren->GetRenderWindow())
        rw->Render();
}

static void TurnR(vtkRenderer* ren)
{
    if (!ren) return;
    auto* cam = ren->GetActiveCamera();
    if (!cam) return;

    cam->Azimuth(90.0);
    cam->OrthogonalizeViewUp();
    ren->ResetCameraClippingRange();

    if (auto* rw = ren->GetRenderWindow())
        rw->Render();
}

void ElectrodeAutoIdentifier::SearchV7V12(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI)
{
    if (!panel || !ren)
        return;


    auto camSaved = SaveCamera(ren);
    ClearDebugProps(ren);
    auto& det = ElectrodeSurfaceDetector::instance();

    std::array<double, 3> volCenterW{ DI.VolumeOriginX + DI.VolumeCenterX, DI.VolumeOriginY + DI.VolumeCenterY, DI.VolumeOriginZ + DI.VolumeCenterZ };

    TurnR(ren);
    Anchor aV6 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V6);
    Anchor aV7 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V7, aV6, volCenterW, SectorSearchParams{ 0.5, 5, 20.0, 0.30, true });
    Anchor aV8 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V8, aV7, volCenterW, SectorSearchParams{ 0.0, 4.0, 20.0, 0.30, true });
    Anchor aV9 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V9, aV8, volCenterW, SectorSearchParams{ 1.5, 4.5, 6.0, 0.18, true });
    Anchor aV10 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V10, aV9, volCenterW, SectorSearchParams{ 1.0, 5.0, 6.0, 0.18, true });
    Anchor aV11 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V11, aV10, volCenterW, SectorSearchParams{ 1.0, 5.0, 6.0, 0.18, true });
    Anchor aV12 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V12, aV11, volCenterW, SectorSearchParams{ 2.0, 6.0, 6.0, 0.18, true });
    ClearDebugProps(ren);
}

static int pickLeftBottomPA(const std::vector<ElectrodeAutoIdentifier::Cand2D>& cands)
{
    if (cands.empty())
        return -1;

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

    const double rangeX = std::max(1.0, maxX - minX);
    const double rangeY = std::max(1.0, maxY - minY);

    int best = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (int i = 0; i < (int)cands.size(); ++i)
    {
        // нормируем в [0..1]
        const double nx = (cands[i].x - minX) / rangeX; // 0 = самый левый
        const double ny = (cands[i].y - minY) / rangeY; // 0 = самый нижний

        // “левый-низ” => минимизируем nx + ny
        const double score = nx + ny;

        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }

    return best;
}

void ElectrodeAutoIdentifier::SearchV13V19(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI)
{
    if (!panel || !ren)
        return;


    auto camSaved = SaveCamera(ren);
    ClearDebugProps(ren);
    auto& det = ElectrodeSurfaceDetector::instance();

    std::array<double, 3> volCenterW{ DI.VolumeOriginX + DI.VolumeCenterX, DI.VolumeOriginY + DI.VolumeCenterY, DI.VolumeOriginZ + DI.VolumeCenterZ };

    RotateAzimuthToCenterX(panel, ren, ElectrodePanel::ElectrodeId::V1);
    Anchor aV1 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V1);


    Anchor aV15 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V15, aV1, volCenterW, SectorSearchParams{ 6.0, 9.0, 20.0, 0.30, true });
    Anchor aV14 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V14, aV15, volCenterW, SectorSearchParams{ 6.0, 9.0, 20.0, 0.30, true });
    Anchor aV13 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V13, aV14, volCenterW, SectorSearchParams{ 7.5, 10.5, 6.0, 0.18, true });
    Anchor aV19 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V19, aV13, volCenterW, SectorSearchParams{ 4.5, 7.5, 6.0, 0.18, true });

    RotateAzimuthToCenterX(panel, ren, ElectrodePanel::ElectrodeId::V1);
    aV1 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V1);

    Anchor aV16 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V16, aV1, volCenterW, SectorSearchParams{ 0.0, 3.0, 6.0, 0.18, true });
    Anchor aV17 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V17, aV16, volCenterW, SectorSearchParams{ 1.5, 4.5, 6.0, 0.18, true });
    Anchor aV18 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V18, aV17, volCenterW, SectorSearchParams{ 1.5, 4.5, 6.0, 0.18, true });
    ClearDebugProps(ren);
}

void ElectrodeAutoIdentifier::SearchV20V25(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI)
{
    if (!panel || !ren)
        return;


    auto camSaved = SaveCamera(ren);
    ClearDebugProps(ren);
    auto& det = ElectrodeSurfaceDetector::instance();

    std::array<double, 3> volCenterW{ DI.VolumeOriginX + DI.VolumeCenterX, DI.VolumeOriginY + DI.VolumeCenterY, DI.VolumeOriginZ + DI.VolumeCenterZ };


    RotateAzimuthToCenterX(panel, ren, ElectrodePanel::ElectrodeId::V1);
    Anchor aV1 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V1);


    Anchor aV25 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V25, aV1, volCenterW, SectorSearchParams{ 9.0, 0.0, 20.0, 0.30, true });
    Anchor aV24 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V24, aV25, volCenterW, SectorSearchParams{ 6.0, 10.5, 20.0, 0.30, true });

    RestoreCamera(ren, camSaved); // вернулись в AP
    TurnPA(ren);                  // повернули на PA

    bool placedV20 = false;
    std::array<double, 3> wV20{};
    Anchor aV20;

    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V20))
    {
        const auto centers = det.currentSphereCenters();
        const auto cands = CollectDisplayCandidates(ren, centers);

        const int idx = PickLeft(cands);

        aV20 = CommitByIndex(panel, ren, det,
            ElectrodePanel::ElectrodeId::V20,
            cands, idx,
            placedV20, wV20);
    }
    else
    {
        aV20 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V20);
    }

    Anchor aV21 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V21, aV20, volCenterW, SectorSearchParams{ 1.0, 4.5, 6.0, 0.18, true });
    Anchor aV22 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V22, aV21, volCenterW, SectorSearchParams{ 1.5, 4.5, 6.0, 0.18, true });
    Anchor aV23 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V23, aV22, volCenterW, SectorSearchParams{ 1.5, 4.5, 6.0, 0.18, true });
    ClearDebugProps(ren);
}

void ElectrodeAutoIdentifier::SearchV26V30(ElectrodePanel* panel, vtkRenderer* ren, DicomInfo DI)
{
    if (!panel || !ren)
        return;

    auto camSaved = SaveCamera(ren);

    auto& det = ElectrodeSurfaceDetector::instance();

    std::array<double, 3> volCenterW{ DI.VolumeOriginX + DI.VolumeCenterX, DI.VolumeOriginY + DI.VolumeCenterY, DI.VolumeOriginZ + DI.VolumeCenterZ };

    RotateAzimuthToCenterX(panel, ren, ElectrodePanel::ElectrodeId::V1);
    Anchor aV1 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V1);

    Anchor aV26 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V26, aV1, volCenterW, SectorSearchParams{ 4.5, 6, 8.0, 0.18, true });
    RestoreCamera(ren, camSaved);
    Anchor aV27 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V27, aV26, volCenterW, SectorSearchParams{3.0, 6.0, 8.0, 0.18, true});

    RotateAzimuthToCenterX(panel, ren, ElectrodePanel::ElectrodeId::V5);
    Anchor aV5 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V5);

    RestoreCamera(ren, camSaved); // вернулись в AP

    bool placedV28 = false;
    std::array<double, 3> wV28{};
    Anchor aV28;

    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V28))
    {
        const auto centers = det.currentSphereCenters();
        const auto cands = CollectDisplayCandidates(ren, centers);

        const int idx = PickRightBottom(cands);

        aV28 = CommitByIndex(panel, ren, det,
            ElectrodePanel::ElectrodeId::V28,
            cands, idx,
            placedV28, wV28);
    }
    
    RestoreCamera(ren, camSaved); // вернулись в AP
    TurnPA(ren);                  // повернули на PA

    bool placedV29 = false;
    std::array<double, 3> wV29{};
    Anchor aV29;

    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V29))
    {
        const auto centers = det.currentSphereCenters();
        const auto cands = CollectDisplayCandidates(ren, centers);

        const int idx = PickLeftBottom(cands);

        aV29 = CommitByIndex(panel, ren, det,
            ElectrodePanel::ElectrodeId::V29,
            cands, idx,
            placedV29, wV29);
    }
    else
    {
        aV29 = AnchorFromPanel(panel, ren, ElectrodePanel::ElectrodeId::V29);
    }

    Anchor aV30 = PlaceSequential(panel, ren, det, ElectrodePanel::ElectrodeId::V30, aV29, volCenterW, SectorSearchParams{ 1.5, 4.5, 8.0, 0.18, true });
    ClearDebugProps(ren);
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

bool ElectrodeAutoIdentifier::ShouldShowSearchV7V12(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    const bool hasV1V6 = panel->hasCoord(ElectrodePanel::ElectrodeId::V1)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V2)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V3)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V4)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V5)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V6);

    if (!hasV1V6)
        return false;

    const bool hasV26V30 = panel->hasCoord(ElectrodePanel::ElectrodeId::V26)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V27)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V28)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V29)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V30);

    if (!hasV26V30)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V7)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V8)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V9)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V10)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V11)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V12)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}

bool ElectrodeAutoIdentifier::ShouldShowSearchV13V19(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    const bool hasV1V6 = panel->hasCoord(ElectrodePanel::ElectrodeId::V1)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V2)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V3)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V4)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V5)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V6);

    if (!hasV1V6)
        return false;

    const bool hasV26V30 = panel->hasCoord(ElectrodePanel::ElectrodeId::V26)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V27)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V28)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V29)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V30);

    if (!hasV26V30)
        return false;

    const bool hasV7V12 = panel->hasCoord(ElectrodePanel::ElectrodeId::V7)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V8)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V9)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V10)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V11)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V12);

    if (!hasV7V12)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V13)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V14)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V15)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V16)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V17)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V18)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V19)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}

bool ElectrodeAutoIdentifier::ShouldShowSearchV20V25(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    const bool hasV1V6 = panel->hasCoord(ElectrodePanel::ElectrodeId::V1)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V2)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V3)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V4)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V5)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V6);

    if (!hasV1V6)
        return false;

    const bool hasV26V30 = panel->hasCoord(ElectrodePanel::ElectrodeId::V26)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V27)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V28)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V29)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V30);

    if (!hasV26V30)
        return false;

    const bool hasV7V12 = panel->hasCoord(ElectrodePanel::ElectrodeId::V7)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V8)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V9)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V10)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V11)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V12);

    if (!hasV7V12)
        return false;

    const bool hasV13V19 = panel->hasCoord(ElectrodePanel::ElectrodeId::V13)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V14)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V15)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V16)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V17)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V18)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V19);

    if (!hasV13V19)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V20)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V21)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V22)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V23)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V24)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V25)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}

bool ElectrodeAutoIdentifier::ShouldShowSearchV26V30(const ElectrodePanel* panel)
{
    if (!panel)
        return false;

    const bool hasV1V6 = panel->hasCoord(ElectrodePanel::ElectrodeId::V1)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V2)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V3)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V4)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V5)
        && panel->hasCoord(ElectrodePanel::ElectrodeId::V6);

    if (!hasV1V6)
        return false;

    int missing = 0;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V26)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V27)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V28)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V29)) ++missing;
    if (!panel->hasCoord(ElectrodePanel::ElectrodeId::V30)) ++missing;

    if (missing <= 0)
        return false;

    const int availableSpheres = ElectrodeSurfaceDetector::instance().sphereCount();
    return availableSpheres >= missing;
}