#include "ToolsContour.h"

#include "Tools.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkCellPicker.h>
#include <vtkClipPolyData.h>
#include <vtkCleanPolyData.h>
#include <vtkDijkstraGraphGeodesicPath.h>
#include <vtkIdList.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSelectPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkStripper.h>
#include <vtkTriangleFilter.h>

ToolsContour::ToolsContour(QWidget* hostParent)
    : QObject(nullptr)
    , m_host(hostParent)
{
    m_overlay = new QWidget(hostParent);
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlay->setAttribute(Qt::WA_NoSystemBackground, true);
    m_overlay->setAttribute(Qt::WA_TranslucentBackground, true);
    m_overlay->setMouseTracking(true);
    m_overlay->setFocusPolicy(Qt::StrongFocus);
    m_overlay->hide();
    m_overlay->installEventFilter(this);

    m_picker = vtkSmartPointer<vtkCellPicker>::New();
    m_picker->SetTolerance(0.0008);
}

void ToolsContour::attach(QVTKOpenGLNativeWidget* vtk,
    vtkRenderer* renderer,
    vtkPolyData* mesh,
    vtkActor* meshActor)
{
    m_vtk = vtk;
    m_renderer = renderer;
    m_mesh = mesh;
    m_meshActor = meshActor;
    onViewResized();
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
    if (!m_vtk || !m_overlay)
        return;

    m_overlay->setGeometry(m_vtk->geometry());
}

void ToolsContour::cancel()
{
    const bool wasActive = (m_state != State::Off);
    m_state = State::Off;
    m_controlIds.clear();
    m_contourWorldPoints.clear();

    if (m_overlay)
    {
        m_overlay->unsetCursor();
        m_overlay->hide();
    }

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

    onViewResized();
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlay->setFocus(Qt::OtherFocusReason);
    m_overlay->show();
    redraw();
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

    const bool ok = applyContourCut();
    cancel();
    if (!ok)
        return;

    if (m_vtk && m_vtk->renderWindow())
        m_vtk->renderWindow()->Render();
}

bool ToolsContour::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_overlay || m_state != State::Collecting)
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
            redraw();
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
    case QEvent::Paint:
    {
        QPainter p(m_overlay);
        p.fillRect(m_overlay->rect(), QColor(0, 0, 0, 0));
        paintOverlay(p);
        return true;
    }
    default:
        break;
    }

    return QObject::eventFilter(obj, ev);
}

bool ToolsContour::pickPoint(const QPoint& overlayPos, vtkIdType& pointId, std::array<double, 3>& worldPoint) const
{
    if (!m_vtk || !m_renderer || !m_mesh || !m_meshActor || !m_picker)
        return false;

    const double dpr = m_vtk->devicePixelRatioF();
    int rwH = 0;
    if (auto* rw = m_vtk->renderWindow())
        rwH = rw->GetSize()[1];
    if (rwH <= 0)
        rwH = int(m_vtk->height() * dpr);

    const double xd = overlayPos.x() * dpr;
    const double yd = (rwH - 1) - overlayPos.y() * dpr;

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

    const vtkIdType begin = (m_contourWorldPoints.isEmpty() ? 0 : 1);
    for (vtkIdType i = begin; i < out->GetNumberOfPoints(); ++i)
    {
        double p[3]{};
        out->GetPoint(i, p);
        m_contourWorldPoints.push_back({ p[0], p[1], p[2] });
    }

    return true;
}

bool ToolsContour::applyContourCut()
{
    if (!m_mesh || m_contourWorldPoints.size() < 3)
        return false;

    auto loopPts = vtkSmartPointer<vtkPoints>::New();
    for (const auto& p : m_contourWorldPoints)
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

void ToolsContour::redraw()
{
    if (m_overlay)
        m_overlay->update();
}

bool ToolsContour::worldToOverlay(const std::array<double, 3>& world, QPointF& out) const
{
    if (!m_renderer || !m_vtk)
        return false;

    const double dpr = m_vtk->devicePixelRatioF();
    int rwH = 0;
    if (auto* rw = m_vtk->renderWindow())
        rwH = rw->GetSize()[1];
    if (rwH <= 0)
        rwH = int(m_vtk->height() * dpr);

    m_renderer->SetWorldPoint(world[0], world[1], world[2], 1.0);
    m_renderer->WorldToDisplay();
    double d[3]{};
    m_renderer->GetDisplayPoint(d);

    const double x = d[0] / dpr;
    const double y = ((rwH - 1) - d[1]) / dpr;
    out = QPointF(x, y);
    return true;
}

void ToolsContour::paintOverlay(QPainter& p)
{
    if (m_contourWorldPoints.isEmpty())
        return;

    p.setRenderHint(QPainter::Antialiasing, true);

    QPolygonF poly;
    for (const auto& w : m_contourWorldPoints)
    {
        QPointF q;
        if (worldToOverlay(w, q))
            poly << q;
    }

    if (poly.size() >= 2)
    {
        QPen pen(QColor(255, 220, 80));
        pen.setWidth(2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(poly);
    }

    p.setBrush(QColor(255, 220, 80));
    p.setPen(Qt::NoPen);
    for (const QPointF& pt : poly)
        p.drawEllipse(pt, 3, 3);

    if (poly.size() >= 3)
    {
        QColor fill(255, 220, 80, 45);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);

        QPainterPath path;
        path.addPolygon(poly);
        p.drawPath(path);
    }
}