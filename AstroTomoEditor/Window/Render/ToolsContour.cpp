#include "ToolsContour.h"

#include "Tools.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVTKOpenGLNativeWidget.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellLocator.h>
#include <vtkCellPicker.h>
#include <vtkCleanPolyData.h>
#include <vtkClipPolyData.h>
#include <vtkDijkstraGraphGeodesicPath.h>
#include <vtkGlyph3DMapper.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyLine.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSelectPolyData.h>
#include <vtkSphereSource.h>
#include <vtkTriangleFilter.h>

ToolsContour::ToolsContour(QWidget* hostParent)
    : QObject(nullptr)
    , m_host(hostParent)
{
    m_picker = vtkSmartPointer<vtkCellPicker>::New();
    m_picker->SetTolerance(0.0008);
}

void ToolsContour::attach(QVTKOpenGLNativeWidget* vtk,
    vtkRenderer* renderer,
    vtkPolyData* mesh,
    vtkActor* meshActor)
{
    if (m_vtk && m_vtk != vtk)
        m_vtk->removeEventFilter(this);

    m_vtk = vtk;
    m_renderer = renderer;
    m_mesh = mesh;
    m_meshActor = meshActor;

    if (m_vtk)
        m_vtk->installEventFilter(this);
}

bool ToolsContour::handle(Action a)
{
    if (a != Action::Contour)
        return false;

    start();
    return true;
}

void ToolsContour::onViewResized()
{
    // Оставлено специально пустым для совместимости с существующим кодом.
    // Preview теперь рисуется внутри VTK, а не поверх QWidget.
}

void ToolsContour::cancel()
{
    const bool wasActive = (m_state != State::Off);

    m_state = State::Off;
    m_controlIds.clear();
    m_contourWorldPoints.clear();

    destroyPreviewActors();
    renderNow();

    if (wasActive && m_onFinished)
        m_onFinished();
}

void ToolsContour::start()
{
    if (!m_vtk || !m_renderer || !m_mesh || !m_meshActor || m_mesh->GetNumberOfPoints() == 0)
        return;

    m_state = State::Collecting;
    m_controlIds.clear();
    m_contourWorldPoints.clear();

    createPreviewActors();
    updatePreviewGeometry();

    if (m_vtk)
        m_vtk->setFocus(Qt::OtherFocusReason);

    renderNow();
}

void ToolsContour::finish()
{
    if (m_controlIds.size() < 3)
    {
        cancel();
        return;
    }

    const vtkIdType firstId = m_controlIds.front();
    const vtkIdType lastId = m_controlIds.back();

    if (!appendGeodesicPath(lastId, firstId))
    {
        cancel();
        return;
    }

    updatePreviewGeometry();
    renderNow();

    const bool ok = applyContourCut();

    cancel();

    if (!ok)
        return;

    renderNow();
}

bool ToolsContour::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_vtk || m_state != State::Collecting)
        return QObject::eventFilter(obj, ev);

    switch (ev->type())
    {
    case QEvent::MouseButtonPress:
    {
        auto* me = static_cast<QMouseEvent*>(ev);

        if (me->button() == Qt::LeftButton)
        {
            vtkIdType pointId = -1;
            std::array<double, 3> worldPoint{};

            if (!pickPoint(me->pos(), pointId, worldPoint))
                return true;

            if (!m_controlIds.isEmpty())
            {
                if (!appendGeodesicPath(m_controlIds.back(), pointId))
                    return true;
            }
            else
            {
                m_contourWorldPoints.push_back(worldPoint);
            }

            m_controlIds.push_back(pointId);
            updatePreviewGeometry();
            renderNow();
            return true;
        }

        if (me->button() == Qt::RightButton)
        {
            finish();
            return true;
        }

        break;
    }

    case QEvent::KeyPress:
    {
        auto* ke = static_cast<QKeyEvent*>(ev);

        if (ke->key() == Qt::Key_Escape)
        {
            cancel();
            return true;
        }

        break;
    }

    default:
        break;
    }

    return QObject::eventFilter(obj, ev);
}

bool ToolsContour::pickPoint(const QPoint& viewPos, vtkIdType& pointId, std::array<double, 3>& worldPoint) const
{
    if (!m_vtk || !m_renderer || !m_mesh || !m_meshActor || !m_picker)
        return false;

    const double dpr = m_vtk->devicePixelRatioF();

    int rwH = 0;
    if (auto* rw = m_vtk->renderWindow())
        rwH = rw->GetSize()[1];

    if (rwH <= 0)
        rwH = static_cast<int>(m_vtk->height() * dpr);

    const double xd = static_cast<double>(viewPos.x()) * dpr;
    const double yd = static_cast<double>((rwH - 1)) - static_cast<double>(viewPos.y()) * dpr;

    m_picker->InitializePickList();
    m_picker->AddPickList(m_meshActor);
    m_picker->PickFromListOn();

    if (!m_picker->Pick(xd, yd, 0.0, m_renderer))
        return false;

    if (m_picker->GetActor() != m_meshActor)
        return false;

    double pickPos[3]{};
    m_picker->GetPickPosition(pickPos);

    pointId = m_mesh->FindPoint(pickPos);
    if (pointId < 0)
        return false;

    double pt[3]{};
    m_mesh->GetPoint(pointId, pt);

    worldPoint = { pt[0], pt[1], pt[2] };
    return true;
}

bool ToolsContour::appendGeodesicPath(vtkIdType fromId, vtkIdType toId)
{
    if (!m_mesh || fromId < 0 || toId < 0)
        return false;

    vtkNew<vtkDijkstraGraphGeodesicPath> dijkstra;
    dijkstra->SetInputData(m_mesh);
    dijkstra->SetStartVertex(fromId);
    dijkstra->SetEndVertex(toId);
    dijkstra->Update();

    vtkPolyData* out = dijkstra->GetOutput();
    if (!out || !out->GetPoints() || out->GetNumberOfPoints() == 0)
        return false;

    std::vector<std::array<double, 3>> path;
    path.reserve(static_cast<size_t>(out->GetNumberOfPoints()));

    for (vtkIdType i = 0; i < out->GetNumberOfPoints(); ++i)
    {
        double p[3]{};
        out->GetPoint(i, p);
        path.push_back({ p[0], p[1], p[2] });
    }

    double fromPoint[3]{};
    m_mesh->GetPoint(fromId, fromPoint);

    const auto dist2 = [](const std::array<double, 3>& a, const double* b)
        {
            const double dx = a[0] - b[0];
            const double dy = a[1] - b[1];
            const double dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        };

    if (path.size() >= 2)
    {
        const double frontDist = dist2(path.front(), fromPoint);
        const double backDist = dist2(path.back(), fromPoint);

        if (backDist < frontDist)
            std::reverse(path.begin(), path.end());
    }

    const vtkIdType begin = m_contourWorldPoints.isEmpty() ? 0 : 1;
    for (vtkIdType i = begin; i < static_cast<vtkIdType>(path.size()); ++i)
        m_contourWorldPoints.push_back(path[static_cast<int>(i)]);

    return true;
}

bool ToolsContour::applyContourCut()
{
    if (!m_mesh || m_contourWorldPoints.size() < 3)
        return false;

    auto smoothedLoop = [&]() -> QVector<std::array<double, 3>>
        {
            std::vector<std::array<double, 3>> points;
            points.reserve(static_cast<size_t>(m_contourWorldPoints.size()));
            for (const auto& p : m_contourWorldPoints)
                points.push_back(p);

            // Chaikin smoothing keeps the contour closed and visually softer.
            constexpr int smoothIterations = 2;
            for (int iteration = 0; iteration < smoothIterations && points.size() >= 3; ++iteration)
            {
                std::vector<std::array<double, 3>> refined;
                refined.reserve(points.size() * 2);

                for (size_t i = 0; i < points.size(); ++i)
                {
                    const auto& p0 = points[i];
                    const auto& p1 = points[(i + 1) % points.size()];

                    std::array<double, 3> q{};
                    std::array<double, 3> r{};
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        q[axis] = 0.75 * p0[axis] + 0.25 * p1[axis];
                        r[axis] = 0.25 * p0[axis] + 0.75 * p1[axis];
                    }

                    refined.push_back(q);
                    refined.push_back(r);
                }

                points = std::move(refined);
            }

            if (points.size() < 3)
                return m_contourWorldPoints;

            std::vector<double> lengths(points.size() + 1, 0.0);
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto& p0 = points[i];
                const auto& p1 = points[(i + 1) % points.size()];
                const double dx = p1[0] - p0[0];
                const double dy = p1[1] - p0[1];
                const double dz = p1[2] - p0[2];
                lengths[i + 1] = lengths[i] + std::sqrt(dx * dx + dy * dy + dz * dz);
            }

            const double totalLength = lengths.back();
            if (totalLength <= 0.0)
                return m_contourWorldPoints;

            const size_t targetCount = std::max<size_t>(64, points.size());
            std::vector<std::array<double, 3>> sampled;
            sampled.reserve(targetCount);

            for (size_t i = 0; i < targetCount; ++i)
            {
                const double targetLen = totalLength * static_cast<double>(i) / static_cast<double>(targetCount);
                auto upper = std::lower_bound(lengths.begin(), lengths.end(), targetLen);
                size_t seg = static_cast<size_t>(std::distance(lengths.begin(), upper));
                seg = std::clamp<size_t>(seg, 1, points.size());

                const double segStartLen = lengths[seg - 1];
                const double segEndLen = lengths[seg];
                const double segSpan = std::max(1e-12, segEndLen - segStartLen);
                const double t = (targetLen - segStartLen) / segSpan;

                const auto& p0 = points[seg - 1];
                const auto& p1 = points[seg % points.size()];

                std::array<double, 3> p{};
                for (int axis = 0; axis < 3; ++axis)
                    p[axis] = p0[axis] + (p1[axis] - p0[axis]) * t;

                sampled.push_back(p);
            }

            vtkNew<vtkCellLocator> locator;
            locator->SetDataSet(m_mesh);
            locator->BuildLocator();

            QVector<std::array<double, 3>> projected;
            projected.reserve(static_cast<qsizetype>(sampled.size()));

            for (const auto& p : sampled)
            {
                double closest[3]{};
                double dist2 = 0.0;
                vtkIdType cellId = -1;
                int subId = -1;
                locator->FindClosestPoint(p.data(), closest, cellId, subId, dist2);
                projected.push_back({ closest[0], closest[1], closest[2] });
            }

            return projected;
        }();

    vtkNew<vtkPoints> loopPts;
    for (const auto& p : smoothedLoop)
        loopPts->InsertNextPoint(p[0], p[1], p[2]);

    vtkNew<vtkSelectPolyData> select;
    select->SetInputData(m_mesh);
    select->SetLoop(loopPts);
    select->GenerateSelectionScalarsOn();
    select->SetSelectionModeToSmallestRegion();
    select->Update();

    vtkNew<vtkClipPolyData> clip;
    clip->SetInputConnection(select->GetOutputPort());
    clip->SetValue(0.0);
    clip->InsideOutOff();
    clip->GenerateClippedOutputOff();
    clip->Update();

    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputConnection(clip->GetOutputPort());
    tri->PassLinesOff();
    tri->PassVertsOff();
    tri->Update();

    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputConnection(tri->GetOutputPort());
    clean->PointMergingOn();
    clean->Update();

    vtkPolyData* out = clean->GetOutput();
    if (!out || out->GetNumberOfCells() == 0)
        return false;

    auto next = vtkSmartPointer<vtkPolyData>::New();
    next->DeepCopy(out);

    if (m_onSurfaceReplaced)
        m_onSurfaceReplaced(next);

    return true;
}

void ToolsContour::createPreviewActors()
{
    if (!m_renderer)
        return;

    if (!m_previewLineActor)
    {
        m_previewLineData = vtkSmartPointer<vtkPolyData>::New();

        m_previewLineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        m_previewLineMapper->SetInputData(m_previewLineData);

        m_previewLineActor = vtkSmartPointer<vtkActor>::New();
        m_previewLineActor->SetMapper(m_previewLineMapper);
        m_previewLineActor->PickableOff();
        m_previewLineActor->GetProperty()->SetColor(1.0, 0.86, 0.31);
        m_previewLineActor->GetProperty()->SetLineWidth(3.0);
        m_previewLineActor->GetProperty()->SetLighting(false);
        m_previewLineActor->GetProperty()->RenderLinesAsTubesOn();

        m_renderer->AddActor(m_previewLineActor);
    }

    if (!m_previewPointsActor)
    {
        m_previewPointsData = vtkSmartPointer<vtkPolyData>::New();

        m_previewSphereSource = vtkSmartPointer<vtkSphereSource>::New();
        m_previewSphereSource->SetRadius(1.0);
        m_previewSphereSource->SetThetaResolution(12);
        m_previewSphereSource->SetPhiResolution(12);

        m_previewPointsMapper = vtkSmartPointer<vtkGlyph3DMapper>::New();
        m_previewPointsMapper->SetInputData(m_previewPointsData);
        m_previewPointsMapper->SetSourceConnection(m_previewSphereSource->GetOutputPort());
        m_previewPointsMapper->ScalingOff();
        m_previewPointsMapper->ScalarVisibilityOff();

        m_previewPointsActor = vtkSmartPointer<vtkActor>::New();
        m_previewPointsActor->SetMapper(m_previewPointsMapper);
        m_previewPointsActor->PickableOff();
        m_previewPointsActor->GetProperty()->SetColor(1.0, 0.86, 0.31);
        m_previewPointsActor->GetProperty()->SetLighting(false);

        m_renderer->AddActor(m_previewPointsActor);
    }
}

void ToolsContour::destroyPreviewActors()
{
    if (m_renderer)
    {
        if (m_previewLineActor)
            m_renderer->RemoveActor(m_previewLineActor);

        if (m_previewPointsActor)
            m_renderer->RemoveActor(m_previewPointsActor);
    }

    m_previewLineActor = nullptr;
    m_previewLineMapper = nullptr;
    m_previewLineData = nullptr;

    m_previewPointsActor = nullptr;
    m_previewPointsMapper = nullptr;
    m_previewSphereSource = nullptr;
    m_previewPointsData = nullptr;
}

void ToolsContour::updatePreviewGeometry()
{
    if (!m_mesh)
        return;

    if (!m_previewLineData || !m_previewPointsData)
        return;

    vtkNew<vtkPoints> pts;
    for (const auto& p : m_contourWorldPoints)
        pts->InsertNextPoint(p[0], p[1], p[2]);

    // Линия
    vtkNew<vtkCellArray> lines;
    if (pts->GetNumberOfPoints() >= 2)
    {
        vtkNew<vtkPolyLine> polyLine;
        polyLine->GetPointIds()->SetNumberOfIds(pts->GetNumberOfPoints());

        for (vtkIdType i = 0; i < pts->GetNumberOfPoints(); ++i)
            polyLine->GetPointIds()->SetId(i, i);

        lines->InsertNextCell(polyLine);
    }

    m_previewLineData->SetPoints(pts);
    m_previewLineData->SetLines(lines);
    m_previewLineData->Modified();

    // Точки
    vtkNew<vtkCellArray> verts;
    for (vtkIdType i = 0; i < pts->GetNumberOfPoints(); ++i)
    {
        verts->InsertNextCell(1);
        verts->InsertCellPoint(i);
    }

    m_previewPointsData->SetPoints(pts);
    m_previewPointsData->SetVerts(verts);
    m_previewPointsData->Modified();

    // Подбираем размер сфер под масштаб модели
    if (m_previewSphereSource && m_mesh->GetNumberOfPoints() > 0)
    {
        double bounds[6]{};
        m_mesh->GetBounds(bounds);

        const double dx = bounds[1] - bounds[0];
        const double dy = bounds[3] - bounds[2];
        const double dz = bounds[5] - bounds[4];
        const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Можно подправить коэффициент под свою сцену
        const double radius = std::max(0.1, diag * 0.0035);
        m_previewSphereSource->SetRadius(radius);
        m_previewSphereSource->Update();
    }
}

void ToolsContour::renderNow()
{
    if (m_vtk && m_vtk->renderWindow())
        m_vtk->renderWindow()->Render();
}