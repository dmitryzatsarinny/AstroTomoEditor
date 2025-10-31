#include "ToolsScissors.h"
#include "Tools.h"
#include <QVTKOpenGLNativeWidget.h>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>

// VTK
#include <vtkRenderer.h>
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkVolume.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkPoints.h>
#include <vtkPolygon.h>
#include <vtkCellArray.h>
#include <vtkPolyData.h>
#include <vtkLinearExtrusionFilter.h>
#include <vtkPolyDataToImageStencil.h>
#include <vtkImageStencil.h>
#include <vtkSmartPointer.h>
#include <vtkRenderWindow.h>
#include <QPainterPath.h>

#include <cmath>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTransform.h>
#include <vtkTriangleFilter.h>
#include <vtkCleanPolyData.h>

#include <QApplication>
#include <QWheelEvent>
#include <QWidget>

ToolsScissors::ToolsScissors(QWidget* hostParent)
    : QObject(nullptr)
    , m_host(hostParent)
{
    m_overlay = new QWidget(hostParent);
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlay->setAttribute(Qt::WA_NoSystemBackground, true);
    m_overlay->setAttribute(Qt::WA_TranslucentBackground, true);
    m_overlay->hide();
    m_overlay->installEventFilter(this);
    m_overlay->setMouseTracking(true);
    m_overlay->setFocusPolicy(Qt::StrongFocus);
}

void ToolsScissors::attach(QVTKOpenGLNativeWidget* vtk,
    vtkRenderer* renderer,
    vtkImageData* image,
    vtkVolume* volume)
{
    m_vtk = vtk;
    m_renderer = renderer;
    m_image = image;
    m_volume = volume;
    onViewResized();
}

static void forwardMouseToWidget(QWidget* target, QMouseEvent* me)
{
    if (!target || !me) return;
    const QPointF screenPos = me->globalPosition();
    const QPointF localPos = target->mapFromGlobal(screenPos.toPoint());
    const QPointF windowPos = localPos;

    QMouseEvent copy(
        me->type(),
        localPos, windowPos, screenPos,
        me->button(),
        me->buttons(),
        me->modifiers(),
        me->source()
    );
    QCoreApplication::sendEvent(target, &copy);
}

void ToolsScissors::forwardMouseToVtk(QEvent* e)
{
    if (!m_vtk) return;

    switch (e->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(e);
        const QPointF screenPos = me->globalPosition();
        const QPointF localPos = m_vtk->mapFromGlobal(screenPos.toPoint());
        const QPointF windowPos = localPos;

        QMouseEvent copy(
            me->type(),
            localPos,   // pos в координатах m_vtk
            windowPos,  // scene/window pos
            screenPos,  // global
            me->button(),
            me->buttons(),
            me->modifiers(),
            me->source()
        );
        QCoreApplication::sendEvent(m_vtk, &copy);
        break;
    }
    case QEvent::Wheel: {
        auto* we = static_cast<QWheelEvent*>(e);
        const QPointF screenPos = we->globalPosition();
        const QPointF localPos = m_vtk->mapFromGlobal(screenPos.toPoint());

        QWheelEvent copy(
            localPos,        // pos в координатах m_vtk
            screenPos,       // global
            we->pixelDelta(),
            we->angleDelta(),
            we->buttons(),
            we->modifiers(),
            we->phase(),
            we->inverted(),
            we->source()
        );
        QCoreApplication::sendEvent(m_vtk, &copy);
        break;
    }
    default: break;
    }
}

bool ToolsScissors::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_overlay || m_state != State::Collecting)
        return QObject::eventFilter(obj, ev);

    if (ev->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(ev);
        const QPoint  screenPt = me->globalPosition().toPoint();
        QWidget* target = QApplication::widgetAt(screenPt);

        // Целимся в любой виджет, который НЕ vtk-виджет и не его потомок
        const bool isUiTarget =
            target &&
            target != m_overlay &&
            !(m_vtk && (target == m_vtk || m_vtk->isAncestorOf(target)));

        if (isUiTarget) 
        {
            cancel();
            return false;
        }
    }

    if (m_allowNav) {
        if (ev->type() == QEvent::Wheel) {
            forwardMouseToVtk(ev);
            return true; // событие обработали (передали в VTK)
        }
        if (ev->type() == QEvent::MouseMove ||
            ev->type() == QEvent::MouseButtonPress ||
            ev->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(ev);
            const bool isMMB = (me->buttons() & Qt::MiddleButton) ||
                (me->button() == Qt::MiddleButton);
            const bool ctrlL = (me->modifiers() & Qt::ControlModifier) &&
                ((me->buttons() & Qt::LeftButton) || me->button() == Qt::LeftButton);

            if (isMMB || ctrlL) {
                forwardMouseToVtk(ev);
                return true; // навигацию отдали VTK
            }
        }
    }

    switch (ev->type())
    {
    case QEvent::MouseMove:
    {
        auto* me = static_cast<QMouseEvent*>(ev);
        m_cursorPos = me->pos();
        m_hasCursor = true;
        redraw();
        return true;
    }
    case QEvent::MouseButtonPress:
    {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            m_pts.push_back(me->pos());
            redraw();
            return true;
        }
        else if (me->button() == Qt::RightButton)
        {
            m_pts.push_back(me->pos());
            if (m_pts.size() >= 3) {
                finish();
            }
            else
                cancel();
            return true;
        }

        break;
    }

    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) { finish(); return true; }
        if (ke->key() == Qt::Key_Escape) { cancel(); return true; }
        if (ke->key() == Qt::Key_Backspace && !m_pts.isEmpty()) { m_pts.removeLast(); redraw(); return true; }
        break;
    }
    case QEvent::Paint: {
        QPainter p(m_overlay);
        p.fillRect(m_overlay->rect(), QColor(0, 0, 0, 0));
        paintOverlay(p);
        return true;
    }
    default: break;
    }

    // Остальное обрабатываем обычной логикой ножниц
    return QObject::eventFilter(obj, ev);
}

bool ToolsScissors::handle(Action a)
{
    if (a == Action::Scissors) { start(true);  return true; }
    if (a == Action::InverseScissors) { start(false); return true; }
    return false;
}

void ToolsScissors::onViewResized()
{
    if (!m_vtk || !m_overlay) return;
    // Геометрия оверлея = геометрия области VTK внутри host
    const QRect r = m_vtk->geometry();
    m_overlay->setGeometry(r);
}

void ToolsScissors::cancel()
{
    const bool wasActive = (m_state != State::Off);
    if (wasActive)
    {
        m_pts.clear();
        m_state = State::Off;
        if (m_overlay) {
            m_overlay->unsetCursor();   // по желанию, чтобы курсор вернулся
            m_overlay->hide();
        }
        if (m_onFinished) m_onFinished();
    }
}


void ToolsScissors::start(bool cutInside)
{
    if (!m_vtk || !m_renderer || !m_volume || !m_image)
        return;

    m_cutInside = cutInside;
    m_pts.clear();
    m_state = State::Collecting;

    onViewResized();                // подгони размер overlay под VTK

    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlay->removeEventFilter(this);
    m_overlay->installEventFilter(this);
    m_overlay->setMouseTracking(true);
    //m_overlay->raise();
    m_overlay->setFocus(Qt::OtherFocusReason);
    m_overlay->show();

    redraw();
}

void ToolsScissors::repeat()
{
    m_pts.clear();
}

void ToolsScissors::finish()
{
    if (m_pts.size() < 3) 
    { 
       return; 
    }

    vtkImageData* out = applyPolygonCut(m_pts, m_cutInside);
    repeat();
    if (!out) 
        return;

    // Подменяем вход маппера
    if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(m_volume->GetMapper()))
        gm->SetInputData(out);
    else
        m_volume->GetMapper()->SetInputDataObject(out);

    // Передаём наружу, чтобы RenderView обновил свой mImage
    if (m_onImageReplaced) 
        m_onImageReplaced(out);

    if (m_vtk && m_vtk->renderWindow()) 
        m_vtk->renderWindow()->Render();
}

void ToolsScissors::redraw()
{
    if (m_overlay) m_overlay->update();
}

void ToolsScissors::paintOverlay(QPainter& p)
{
    if (m_pts.isEmpty()) return;

    p.setRenderHint(QPainter::Antialiasing, true);

    // Собираем «текущий» полигон: вершины + (опционально) курсор
    QPolygon poly;
    poly.reserve(m_pts.size() + 1);
    for (const auto& q : m_pts) poly << q;
    if (m_hasCursor) poly << m_cursorPos;

    // Заливка замкнутой области — чтобы было «замкнутое пространство»
    if (poly.size() >= 3) {
        QPainterPath path;
        path.addPolygon(poly);
        path.closeSubpath();

        QColor fill = m_cutInside ? QColor(0, 180, 100, 60)   // внутри вырежем — зелёный оттенок
            : QColor(220, 70, 70, 60);  // снаружи вырежем — красный оттенок
        p.setBrush(fill);
        p.setPen(Qt::NoPen);
        p.drawPath(path);
    }

    // Контур: линии между вершинами + «живое» ребро к курсору + замыкание к первой точке
    QPen pen(Qt::white);
    pen.setWidth(2);
    p.setPen(pen);
    for (int i = 1; i < m_pts.size(); ++i)
        p.drawLine(m_pts[i - 1], m_pts[i]);
    if (m_hasCursor && !m_pts.isEmpty())
        p.drawLine(m_pts.back(), m_cursorPos);
    if (m_pts.size() >= 2)                // показать «замыкание»
        p.drawLine(m_hasCursor ? m_cursorPos : m_pts.back(), m_pts.front());

    // Узлы
    p.setBrush(Qt::white);
    for (const auto& q : m_pts)
        p.drawEllipse(q, 3, 3);
    if (m_hasCursor)
        p.drawEllipse(m_cursorPos, 2, 2);
}

vtkImageData* ToolsScissors::applyPolygonCut(const QVector<QPoint>& pts2D, bool cutInside)
{
    if (!m_renderer || !m_vtk || !m_image) return nullptr;

    // 0) Вспомогалка: взять DirectionMatrix (или I)
    vtkNew<vtkMatrix3x3> M;
    if (auto* dm = m_image->GetDirectionMatrix()) M->DeepCopy(dm);
    else M->Identity();

    // построим D^T (world->IJK поворот)
    double DT[3][3]{
        { M->GetElement(0,0), M->GetElement(1,0), M->GetElement(2,0) },
        { M->GetElement(0,1), M->GetElement(1,1), M->GetElement(2,1) },
        { M->GetElement(0,2), M->GetElement(1,2), M->GetElement(2,2) },
    };

    // origin и spacing изображения
    double org[3]; m_image->GetOrigin(org);
    double sp[3]; m_image->GetSpacing(sp);

    // --- 1) POV камеры (лучи) ---
    auto* cam = m_renderer->GetActiveCamera();
    if (!cam) return nullptr;

    // Нормаль к плоскости экрана — это и есть направление "от камеры вглубь"
    double vpn[3]; cam->GetViewPlaneNormal(vpn); // направлена ОТ камеры к сцене
    // Приведём её к единичному масштабу и переведём в IJK
    auto mulDT = [&](const double v[3], double out[3]) {
        out[0] = DT[0][0] * v[0] + DT[0][1] * v[1] + DT[0][2] * v[2];
        out[1] = DT[1][0] * v[0] + DT[1][1] * v[1] + DT[1][2] * v[2];
        out[2] = DT[2][0] * v[0] + DT[2][1] * v[1] + DT[2][2] * v[2];
        };
    double dirW[3]{ vpn[0], vpn[1], vpn[2] }, dirIJK[3]{};
    double nrm = std::sqrt(dirW[0] * dirW[0] + dirW[1] * dirW[1] + dirW[2] * dirW[2]);
    if (nrm < 1e-12) nrm = 1.0;
    for (int i = 0; i < 3; ++i) dirW[i] /= nrm;
    mulDT(dirW, dirIJK);

    double pos[3]; cam->GetPosition(pos);
    double foc[3]; cam->GetFocalPoint(foc);

    double dir[3]{ foc[0] - pos[0], foc[1] - pos[1], foc[2] - pos[2] };
    double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-12) len = 1.0;
    for (int i = 0; i < 3; ++i) dir[i] /= len;

    // z-слой на дисплее берём по текущему фокусу
    double focDisp[3]{ 0,0,0 };
    m_renderer->SetWorldPoint(foc[0], foc[1], foc[2], 1.0);
    m_renderer->WorldToDisplay();
    m_renderer->GetDisplayPoint(focDisp);

    // --- 2) Qt->VTK: инвертируем Y и учитываем DPR/размер окна ---
    const double dpr = m_vtk ? m_vtk->devicePixelRatioF() : 1.0;
    int rwW = 0, rwH = 0;
    if (auto* rw = m_vtk ? m_vtk->renderWindow() : nullptr) {
        const int* sz = rw->GetSize(); rwW = sz[0]; rwH = sz[1];
    }
    else {
        rwW = int(m_vtk->width() * dpr);
        rwH = int(m_vtk->height() * dpr);
    }

    vtkNew<vtkPoints> worldPts;
    worldPts->SetNumberOfPoints(pts2D.size());
    for (int i = 0; i < pts2D.size(); ++i) {
        const double xd = pts2D[i].x() * dpr;
        const double yd = (rwH - 1) - pts2D[i].y() * dpr; // ВАЖНО: инверсия Y
        m_renderer->SetDisplayPoint(xd, yd, focDisp[2]);
        m_renderer->DisplayToWorld();
        double w[4]; m_renderer->GetWorldPoint(w);
        if (std::abs(w[3]) > 1e-12) { w[0] /= w[3]; w[1] /= w[3]; w[2] /= w[3]; }
        worldPts->SetPoint(i, w[0], w[1], w[2]);
    }

    // --- 3) World→IJK: поворот D^T и сдвиг на origin ---
    vtkNew<vtkPoints> ijkPts;
    ijkPts->SetNumberOfPoints(worldPts->GetNumberOfPoints());
    for (vtkIdType i = 0; i < worldPts->GetNumberOfPoints(); ++i) {
        double w[3]; worldPts->GetPoint(i, w);
        // world -> (world - origin)
        double rel[3]{ w[0] - org[0], w[1] - org[1], w[2] - org[2] };
        // повернуть D^T
        double p[3]; mulDT(rel, p);
        // НЕ делим на spacing здесь — мы зададим spacing в p2s
        ijkPts->SetPoint(i, p[0], p[1], p[2]);
    }

    // Собираем полигон в IJK-пространстве
    vtkNew<vtkPolygon> poly;
    poly->GetPointIds()->SetNumberOfIds(ijkPts->GetNumberOfPoints());
    for (vtkIdType i = 0; i < ijkPts->GetNumberOfPoints(); ++i)
        poly->GetPointIds()->SetId(i, i);

    // --- 4) FRUSTUM: near/far per-vertex, сборка замкнутого полиэдра в IJK ---
    vtkNew<vtkPoints> nearW, farW;
    nearW->SetNumberOfPoints(pts2D.size());
    farW->SetNumberOfPoints(pts2D.size());

    for (int i = 0; i < pts2D.size(); ++i) {
        const double xd = pts2D[i].x() * dpr;
        const double yd = (rwH - 1) - pts2D[i].y() * dpr; // инверсия Y

        // near (z = 0)
        m_renderer->SetDisplayPoint(xd, yd, 0.0);
        m_renderer->DisplayToWorld();
        double nw[4]; m_renderer->GetWorldPoint(nw);
        if (std::abs(nw[3]) > 1e-12) { nw[0] /= nw[3]; nw[1] /= nw[3]; nw[2] /= nw[3]; }
        nearW->SetPoint(i, nw[0], nw[1], nw[2]);

        // far (z = 1)
        m_renderer->SetDisplayPoint(xd, yd, 1.0);
        m_renderer->DisplayToWorld();
        double fw[4]; m_renderer->GetWorldPoint(fw);
        if (std::abs(fw[3]) > 1e-12) { fw[0] /= fw[3]; fw[1] /= fw[3]; fw[2] /= fw[3]; }
        farW->SetPoint(i, fw[0], fw[1], fw[2]);
    }

    // World -> IJK: (w - origin) * D^T
    auto toIJK = [&](vtkPoints* srcW, vtkPoints* dstIJK) 
        {
            dstIJK->SetNumberOfPoints(srcW->GetNumberOfPoints());
            for (vtkIdType i = 0; i < srcW->GetNumberOfPoints(); ++i) 
            {
                double w[3]; srcW->GetPoint(i, w);
                double rel[3]{ w[0] - org[0], w[1] - org[1], w[2] - org[2] };
                double p[3];
                p[0] = DT[0][0] * rel[0] + DT[0][1] * rel[1] + DT[0][2] * rel[2];
                p[1] = DT[1][0] * rel[0] + DT[1][1] * rel[1] + DT[1][2] * rel[2];
                p[2] = DT[2][0] * rel[0] + DT[2][1] * rel[1] + DT[2][2] * rel[2];
                dstIJK->SetPoint(i, p);
            }
        };

    vtkNew<vtkPoints> nearIJK, farIJK;
    toIJK(nearW, nearIJK);
    toIJK(farW, farIJK);

    // Общая таблица точек: [ near(0..N-1), far(N..2N-1) ]
    const vtkIdType N = static_cast<vtkIdType>(pts2D.size());
    vtkNew<vtkPoints> allPts; allPts->SetNumberOfPoints(2 * N);
    for (vtkIdType i = 0; i < N; ++i) {
        double p[3];
        nearIJK->GetPoint(i, p); allPts->SetPoint(i, p);
        farIJK->GetPoint(i, p); allPts->SetPoint(N + i, p);
    }

    vtkNew<vtkCellArray> faces;

    // Near cap: тот же порядок, что рисовали
    {
        vtkNew<vtkPolygon> cap;
        cap->GetPointIds()->SetNumberOfIds(N);
        for (vtkIdType i = 0; i < N; ++i) cap->GetPointIds()->SetId(i, i);
        faces->InsertNextCell(cap);
    }

    // Far cap: обратный порядок, индексы смещены на N
    {
        vtkNew<vtkPolygon> cap;
        cap->GetPointIds()->SetNumberOfIds(N);
        for (vtkIdType i = 0; i < N; ++i) cap->GetPointIds()->SetId(i, N + (N - 1 - i));
        faces->InsertNextCell(cap);
    }

    // Боковые панели: квадраты → два треугольника
    for (vtkIdType i = 0; i < N; ++i) {
        const vtkIdType i0 = i;
        const vtkIdType i1 = (i + 1) % N;
        const vtkIdType j0 = N + i0;
        const vtkIdType j1 = N + i1;

        // tri1: near(i0), near(i1), far(i1)
        faces->InsertNextCell(3);
        faces->InsertCellPoint(i0);
        faces->InsertCellPoint(i1);
        faces->InsertCellPoint(j1);

        // tri2: near(i0), far(i1), far(i0)
        faces->InsertNextCell(3);
        faces->InsertCellPoint(i0);
        faces->InsertCellPoint(j1);
        faces->InsertCellPoint(j0);
    }

    vtkNew<vtkPolyData> frustumIJK;
    frustumIJK->SetPoints(allPts);
    frustumIJK->SetPolys(faces);

    // Немного подчистим и триангулируем (надёжность)
    vtkNew<vtkCleanPolyData> clean;
    clean->SetInputData(frustumIJK);
    clean->ConvertPolysToLinesOff();
    clean->Update();

    vtkNew<vtkTriangleFilter> tri;
    tri->SetInputConnection(clean->GetOutputPort());
    tri->Update();

    // --- 5) Растеризация в сетку изображения ---
    int   ext[6]; m_image->GetExtent(ext);

    vtkNew<vtkPolyDataToImageStencil> p2s;
    p2s->SetInputConnection(tri->GetOutputPort());
    p2s->SetOutputOrigin(0.0, 0.0, 0.0);   // важно: точки уже в IJK*spacing
    p2s->SetOutputSpacing(sp);
    p2s->SetOutputWholeExtent(ext);
    p2s->Update();

    vtkNew<vtkImageStencil> sten;
    sten->SetInputData(m_image);
    sten->SetStencilConnection(p2s->GetOutputPort());
    // семантика меню: Scissors = оставить внутри, Inverse = снаружи
    sten->SetReverseStencil(cutInside);
    sten->SetBackgroundValue(0);
    sten->Update();

    // Возвращаем новый vtkImageData во владение VTK (жить будет, пока держим ссылку в маппере и RenderView)
    vtkImageData* out = vtkImageData::New();
    out->DeepCopy(sten->GetOutput());
    return out;
}
