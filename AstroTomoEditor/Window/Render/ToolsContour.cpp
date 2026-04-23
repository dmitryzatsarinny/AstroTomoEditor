#include "ToolsContour.h"

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
#include <vtkLinearSubdivisionFilter.h>
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
    // Ничего не нужно: превью рисуется VTK-акторами.
}

void ToolsContour::start()
{
    if (!m_vtk || !m_renderer || !m_mesh || !m_meshActor || m_mesh->GetNumberOfPoints() == 0)
        return;

    m_state = State::Collecting;
    m_controlIds.clear();
    m_controlWorldPoints.clear();
    m_rawContourWorldPoints.clear();
    m_displayContourWorldPoints.clear();

    createPreviewActors();
    updatePreviewGeometry();

    if (m_vtk)
        m_vtk->setFocus(Qt::OtherFocusReason);

    renderNow();
}

void ToolsContour::cancel()
{
    const bool wasActive = (m_state != State::Off);

    m_state = State::Off;
    m_controlIds.clear();
    m_controlWorldPoints.clear();
    m_rawContourWorldPoints.clear();
    m_displayContourWorldPoints.clear();

    destroyPreviewActors();
    renderNow();

    if (wasActive && m_onFinished)
        m_onFinished();
}

void ToolsContour::finish()
{
    if (m_finishing || m_state != State::Collecting)
        return;

    if (m_controlIds.size() < 3)
        return;

    m_finishing = true;

    const bool ok = applyContourCut();
    if (!ok)
    {
        qDebug() << "Contour cut failed";
        m_finishing = false;
        renderNow();
        return;
    }

    qDebug() << "Contour cut applied";
    cancel();
    m_finishing = false;
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
            WorldPoint worldPoint{};

            if (!pickPoint(me->pos(), pointId, worldPoint))
                return true;

            m_controlIds.push_back(pointId);
            m_controlWorldPoints.push_back(worldPoint);

            rebuildContourFromControls();
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

    case QEvent::ContextMenu:
        return true;

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

bool ToolsContour::pickPoint(const QPoint& viewPos, vtkIdType& pointId, WorldPoint& worldPoint) const
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
    const double yd = static_cast<double>(rwH - 1) - static_cast<double>(viewPos.y()) * dpr;

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

bool ToolsContour::computeGeodesicSegment(vtkIdType fromId,
    vtkIdType toId,
    std::vector<WorldPoint>& outPath) const
{
    outPath.clear();

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

    outPath.reserve(static_cast<size_t>(out->GetNumberOfPoints()));
    for (vtkIdType i = 0; i < out->GetNumberOfPoints(); ++i)
    {
        double p[3]{};
        out->GetPoint(i, p);
        outPath.push_back({ p[0], p[1], p[2] });
    }

    double fromPoint[3]{};
    m_mesh->GetPoint(fromId, fromPoint);

    const auto dist2ToFrom = [&](const WorldPoint& p)
        {
            const double dx = p[0] - fromPoint[0];
            const double dy = p[1] - fromPoint[1];
            const double dz = p[2] - fromPoint[2];
            return dx * dx + dy * dy + dz * dz;
        };

    if (outPath.size() >= 2)
    {
        const double frontDist = dist2ToFrom(outPath.front());
        const double backDist = dist2ToFrom(outPath.back());

        if (backDist < frontDist)
            std::reverse(outPath.begin(), outPath.end());
    }

    return !outPath.empty();
}

void ToolsContour::rebuildContourFromControls()
{
    m_rawContourWorldPoints.clear();

    if (m_controlIds.isEmpty() || !m_mesh)
    {
        updateDisplayContour();
        return;
    }

    if (m_controlIds.size() == 1)
    {
        m_rawContourWorldPoints.push_back(m_controlWorldPoints.front());
        updateDisplayContour();
        return;
    }

    std::vector<WorldPoint> segment;

    auto appendSegment = [&](vtkIdType fromId, vtkIdType toId, bool skipFirstPoint)
        {
            if (!computeGeodesicSegment(fromId, toId, segment))
                return false;

            const int beginIndex = skipFirstPoint ? 1 : 0;
            for (int i = beginIndex; i < static_cast<int>(segment.size()); ++i)
                m_rawContourWorldPoints.push_back(segment[static_cast<size_t>(i)]);

            return true;
        };

    bool firstSegment = true;

    for (int i = 1; i < m_controlIds.size(); ++i)
    {
        if (!appendSegment(m_controlIds[i - 1], m_controlIds[i], !firstSegment))
        {
            updateDisplayContour();
            return;
        }
        firstSegment = false;
    }

    // После третьего клика и дальше сразу держим контур замкнутым.
    if (m_controlIds.size() >= 3)
    {
        appendSegment(m_controlIds.back(), m_controlIds.front(), !m_rawContourWorldPoints.isEmpty());
    }

    updateDisplayContour();
}

void ToolsContour::updateDisplayContour()
{
    m_displayContourWorldPoints.clear();

    if (m_rawContourWorldPoints.isEmpty())
        return;

    if (m_controlIds.size() < 3)
    {
        m_displayContourWorldPoints = m_rawContourWorldPoints;
        return;
    }

    m_displayContourWorldPoints = buildSmoothedClosedLoop(m_rawContourWorldPoints);
}

QVector<ToolsContour::WorldPoint> ToolsContour::buildSmoothedClosedLoop(const QVector<WorldPoint>& closedLoop) const
{
    if (closedLoop.size() < 3 || !m_mesh)
        return closedLoop;

    QVector<WorldPoint> loop = closedLoop;

    // Если последний элемент совпадает с первым, убираем дубль для внутренних вычислений.
    if (loop.size() >= 2 && almostEqual(loop.front(), loop.back()))
        loop.removeLast();

    if (loop.size() < 3)
        return closedLoop;

    const int targetCount = std::max(m_previewMinSampleCount, (int)loop.size());
    QVector<WorldPoint> resampled = resampleClosedLoop(loop, targetCount);
    QVector<WorldPoint> smoothed = smoothClosedLoopOnSurface(
        resampled,
        m_previewSmoothIterations,
        m_previewSmoothFactor);

    return smoothed;
}

QVector<ToolsContour::WorldPoint> ToolsContour::resampleClosedLoop(const QVector<WorldPoint>& closedLoop,
    int targetCount) const
{
    QVector<WorldPoint> result;

    if (closedLoop.size() < 3 || targetCount < 3)
        return closedLoop;

    std::vector<double> cumulative(static_cast<size_t>(closedLoop.size()) + 1, 0.0);

    for (int i = 0; i < closedLoop.size(); ++i)
    {
        const WorldPoint& a = closedLoop[i];
        const WorldPoint& b = closedLoop[(i + 1) % closedLoop.size()];

        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double dz = b[2] - a[2];

        cumulative[static_cast<size_t>(i + 1)] =
            cumulative[static_cast<size_t>(i)] + std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    const double totalLength = cumulative.back();
    if (totalLength <= 1e-12)
        return closedLoop;

    result.reserve(targetCount);

    for (int i = 0; i < targetCount; ++i)
    {
        const double targetLen = totalLength * static_cast<double>(i) / static_cast<double>(targetCount);

        auto upper = std::lower_bound(cumulative.begin(), cumulative.end(), targetLen);
        size_t seg = static_cast<size_t>(std::distance(cumulative.begin(), upper));
        seg = std::clamp<size_t>(seg, 1, static_cast<size_t>(closedLoop.size()));

        const double segStart = cumulative[seg - 1];
        const double segEnd = cumulative[seg];
        const double span = std::max(1e-12, segEnd - segStart);
        const double t = (targetLen - segStart) / span;

        const WorldPoint& p0 = closedLoop[static_cast<int>(seg - 1)];
        const WorldPoint& p1 = closedLoop[static_cast<int>(seg % static_cast<size_t>(closedLoop.size()))];

        WorldPoint p{};
        for (int axis = 0; axis < 3; ++axis)
            p[axis] = p0[axis] + (p1[axis] - p0[axis]) * t;

        result.push_back(p);
    }

    return result;
}

QVector<ToolsContour::WorldPoint> ToolsContour::smoothClosedLoopOnSurface(const QVector<WorldPoint>& closedLoop,
    int iterations,
    double factor) const
{
    if (!m_mesh || closedLoop.size() < 3)
        return closedLoop;

    vtkNew<vtkCellLocator> locator;
    locator->SetDataSet(m_mesh);
    locator->BuildLocator();

    QVector<WorldPoint> current = closedLoop;
    QVector<WorldPoint> next = current;

    factor = std::clamp(factor, 0.0, 1.0);

    for (int iter = 0; iter < iterations; ++iter)
    {
        for (int i = 0; i < current.size(); ++i)
        {
            const WorldPoint& prev = current[(i - 1 + current.size()) % current.size()];
            const WorldPoint& cur = current[i];
            const WorldPoint& nxt = current[(i + 1) % current.size()];

            WorldPoint blended{};
            for (int axis = 0; axis < 3; ++axis)
            {
                const double neighborMean = 0.5 * (prev[axis] + nxt[axis]);
                blended[axis] = (1.0 - factor) * cur[axis] + factor * neighborMean;
            }

            double closest[3]{};
            double dist2 = 0.0;
            vtkIdType cellId = -1;
            int subId = -1;

            locator->FindClosestPoint(blended.data(), closest, cellId, subId, dist2);
            next[i] = { closest[0], closest[1], closest[2] };
        }

        current.swap(next);
    }

    return current;
}

bool ToolsContour::applyContourCut()
{
    if (!m_mesh || m_controlIds.size() < 3)
    {
        qDebug() << "applyContourCut: invalid input";
        return false;
    }

    QVector<WorldPoint> smoothLoop = m_displayContourWorldPoints;
    if (smoothLoop.size() < 3)
        smoothLoop = buildSmoothedClosedLoop(m_rawContourWorldPoints);

    if (smoothLoop.size() < 3)
    {
        qDebug() << "applyContourCut: smooth loop too small";
        return false;
    }

    vtkNew<vtkPoints> loopPts;
    for (const auto& p : smoothLoop)
        loopPts->InsertNextPoint(p[0], p[1], p[2]);

    qDebug() << "applyContourCut: loop pts =" << loopPts->GetNumberOfPoints();

    // 1. Готовим исходную сетку.
    vtkNew<vtkTriangleFilter> triInput;
    triInput->SetInputData(m_mesh);
    triInput->PassLinesOff();
    triInput->PassVertsOff();
    triInput->Update();

    qDebug() << "triInput cells =" << triInput->GetOutput()->GetNumberOfCells();

    vtkNew<vtkCleanPolyData> cleanInput;
    cleanInput->SetInputConnection(triInput->GetOutputPort());
    cleanInput->PointMergingOn();
    cleanInput->Update();

    vtkPolyData* cleanInputOut = cleanInput->GetOutput();
    if (!cleanInputOut || cleanInputOut->GetNumberOfCells() == 0)
    {
        qDebug() << "cleanInput empty";
        return false;
    }

    qDebug() << "cleanInput cells =" << cleanInputOut->GetNumberOfCells();

    vtkSmartPointer<vtkPolyData> selectInput = cleanInputOut;

    // 1.1 Лёгкая subdivision ПЕРЕД select/clip:
    // это добавляет локально больше треугольников, и линия выреза становится более гладкой.
    if (m_preCutSubdivisionIterations > 0)
    {
        const vtkIdType baseCells = cleanInputOut->GetNumberOfCells();

        vtkNew<vtkLinearSubdivisionFilter> preSubdiv;
        preSubdiv->SetInputData(cleanInputOut);
        preSubdiv->SetNumberOfSubdivisions(m_preCutSubdivisionIterations);
        preSubdiv->Update();

        vtkPolyData* preSubdivOut = preSubdiv->GetOutput();
        const vtkIdType subdivCells = preSubdivOut ? preSubdivOut->GetNumberOfCells() : 0;
        qDebug() << "pre-cut subdiv cells =" << subdivCells;

        if (preSubdivOut && subdivCells > 0)
        {
            const double growthRatio = (baseCells > 0)
                ? static_cast<double>(subdivCells) / static_cast<double>(baseCells)
                : 0.0;

            if (growthRatio <= m_maxPreCutSubdivisionGrowthRatio)
            {
                vtkNew<vtkCleanPolyData> preSubdivClean;
                preSubdivClean->SetInputConnection(preSubdiv->GetOutputPort());
                preSubdivClean->PointMergingOn();
                preSubdivClean->Update();

                vtkPolyData* preSubdivCleanOut = preSubdivClean->GetOutput();
                if (preSubdivCleanOut && preSubdivCleanOut->GetNumberOfCells() > 0)
                {
                    selectInput = preSubdivCleanOut;
                    qDebug() << "pre-cut subdiv accepted, cells =" << selectInput->GetNumberOfCells();
                }
                else
                {
                    qDebug() << "pre-cut subdiv clean failed, keep base mesh";
                }
            }
            else
            {
                qDebug() << "pre-cut subdiv skipped, growth ratio =" << growthRatio
                    << "limit =" << m_maxPreCutSubdivisionGrowthRatio;
            }
        }
        else
        {
            qDebug() << "pre-cut subdiv failed, keep base mesh";
        }
    }


    // 2. Строим выделение по ИСХОДНОЙ поверхности
    vtkNew<vtkSelectPolyData> select;
    select->SetInputData(selectInput);
    select->SetLoop(loopPts);
    select->GenerateSelectionScalarsOn();
    select->SetSelectionModeToSmallestRegion();
    select->Update();

    vtkPolyData* selectOut = select->GetOutput();
    const vtkIdType selectCells = selectOut ? selectOut->GetNumberOfCells() : 0;
    qDebug() << "select output cells =" << selectCells;

    if (!selectOut || selectCells == 0)
    {
        qDebug() << "applyContourCut: select output empty";
        return false;
    }

    // 3. Вырезаем меньшую область
    vtkNew<vtkClipPolyData> clip;
    clip->SetInputConnection(select->GetOutputPort());
    clip->SetValue(0.0);
    clip->InsideOutOff();
    clip->GenerateClippedOutputOff();
    clip->Update();

    vtkPolyData* clipOut = clip->GetOutput();
    const vtkIdType clipCells = clipOut ? clipOut->GetNumberOfCells() : 0;
    qDebug() << "clip output cells =" << clipCells;

    if (!clipOut || clipCells == 0)
    {
        qDebug() << "applyContourCut: clip output empty";
        return false;
    }

    // 4. Финальная триангуляция и очистка после выреза
    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputConnection(clip->GetOutputPort());
    tri->PassLinesOff();
    tri->PassVertsOff();
    tri->Update();

    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputConnection(tri->GetOutputPort());
    clean->PointMergingOn();
    clean->Update();

    vtkSmartPointer<vtkPolyData> result = vtkSmartPointer<vtkPolyData>::New();
    result->DeepCopy(clean->GetOutput());

    if (!result || result->GetNumberOfCells() == 0)
    {
        qDebug() << "clean output empty";
        return false;
    }

    qDebug() << "post-clip clean cells =" << result->GetNumberOfCells();

    // 5. И только теперь optional subdivision
    if (m_cutSubdivisionIterations > 0)
    {
        const vtkIdType baseCells = result->GetNumberOfCells();
        vtkNew<vtkLinearSubdivisionFilter> subdiv;
        subdiv->SetInputData(result);
        subdiv->SetNumberOfSubdivisions(m_cutSubdivisionIterations);
        subdiv->Update();

        vtkPolyData* subdivOut = subdiv->GetOutput();
        const vtkIdType subdivCells = subdivOut ? subdivOut->GetNumberOfCells() : 0;
        qDebug() << "post-clip subdiv cells =" << subdivCells;

        if (subdivOut && subdivCells > 0)
        {
            const double growthRatio = (baseCells > 0)
                ? static_cast<double>(subdivCells) / static_cast<double>(baseCells)
                : 0.0;

            if (growthRatio <= m_maxSubdivisionGrowthRatio)
            {
                vtkNew<vtkCleanPolyData> cleanSubdiv;
                cleanSubdiv->SetInputConnection(subdiv->GetOutputPort());
                cleanSubdiv->PointMergingOn();
                cleanSubdiv->Update();

                vtkPolyData* cleanSubdivOut = cleanSubdiv->GetOutput();
                if (cleanSubdivOut && cleanSubdivOut->GetNumberOfCells() > 0)
                {
                    result->DeepCopy(cleanSubdivOut);
                }
                else
                {
                    qDebug() << "post-clip subdivision clean failed, keep unclipped result";
                }
            }
            else
            {
                qDebug() << "post-clip subdivision skipped, growth ratio =" << growthRatio
                    << "limit =" << m_maxSubdivisionGrowthRatio;
            }
        }
        else
        {
            qDebug() << "post-clip subdivision failed, keep unclipped result";
        }
    }

    qDebug() << "final output cells =" << result->GetNumberOfCells();

    if (m_onSurfaceReplaced)
    {
        qDebug() << "calling m_onSurfaceReplaced";
        m_onSurfaceReplaced(result, smoothLoop);
    }
    else
    {
        qDebug() << "m_onSurfaceReplaced is not set";
    }

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
        m_previewSphereSource->SetThetaResolution(14);
        m_previewSphereSource->SetPhiResolution(14);

        m_previewPointsMapper = vtkSmartPointer<vtkGlyph3DMapper>::New();
        m_previewPointsMapper->SetInputData(m_previewPointsData);
        m_previewPointsMapper->SetSourceConnection(m_previewSphereSource->GetOutputPort());
        m_previewPointsMapper->ScalingOff();
        m_previewPointsMapper->ScalarVisibilityOff();

        m_previewPointsActor = vtkSmartPointer<vtkActor>::New();
        m_previewPointsActor->SetMapper(m_previewPointsMapper);
        m_previewPointsActor->PickableOff();
        m_previewPointsActor->GetProperty()->SetColor(1.0, 0.62, 0.12);
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
    if (!m_mesh || !m_previewLineData || !m_previewPointsData)
        return;

    // ---------- ЛИНИЯ ----------
    vtkNew<vtkPoints> linePts;

    const bool closedPreview = (m_controlIds.size() >= 3);
    const QVector<WorldPoint>& lineSource =
        m_displayContourWorldPoints.isEmpty() ? m_rawContourWorldPoints : m_displayContourWorldPoints;

    for (const auto& p : lineSource)
        linePts->InsertNextPoint(p[0], p[1], p[2]);

    if (closedPreview && linePts->GetNumberOfPoints() >= 3)
    {
        const WorldPoint& first = lineSource.front();
        linePts->InsertNextPoint(first[0], first[1], first[2]);
    }

    vtkNew<vtkCellArray> lines;
    if (linePts->GetNumberOfPoints() >= 2)
    {
        vtkNew<vtkPolyLine> polyLine;
        polyLine->GetPointIds()->SetNumberOfIds(linePts->GetNumberOfPoints());

        for (vtkIdType i = 0; i < linePts->GetNumberOfPoints(); ++i)
            polyLine->GetPointIds()->SetId(i, i);

        lines->InsertNextCell(polyLine);
    }

    m_previewLineData->SetPoints(linePts);
    m_previewLineData->SetLines(lines);
    m_previewLineData->Modified();

    // ---------- КОНТРОЛЬНЫЕ ТОЧКИ ----------
    vtkNew<vtkPoints> controlPts;
    for (const auto& p : m_controlWorldPoints)
        controlPts->InsertNextPoint(p[0], p[1], p[2]);

    vtkNew<vtkCellArray> verts;
    for (vtkIdType i = 0; i < controlPts->GetNumberOfPoints(); ++i)
    {
        verts->InsertNextCell(1);
        verts->InsertCellPoint(i);
    }

    m_previewPointsData->SetPoints(controlPts);
    m_previewPointsData->SetVerts(verts);
    m_previewPointsData->Modified();

    if (m_previewSphereSource && m_mesh->GetNumberOfPoints() > 0)
    {
        double bounds[6]{};
        m_mesh->GetBounds(bounds);

        const double dx = bounds[1] - bounds[0];
        const double dy = bounds[3] - bounds[2];
        const double dz = bounds[5] - bounds[4];
        const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);

        const double radius = std::max(0.1, diag * 0.0040);
        m_previewSphereSource->SetRadius(radius);
        m_previewSphereSource->Update();
    }
}

void ToolsContour::renderNow()
{
    if (m_vtk && m_vtk->renderWindow())
        m_vtk->renderWindow()->Render();
}

double ToolsContour::distanceSquared(const WorldPoint& a, const WorldPoint& b)
{
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

bool ToolsContour::almostEqual(const WorldPoint& a, const WorldPoint& b, double eps2)
{
    return distanceSquared(a, b) <= eps2;
}