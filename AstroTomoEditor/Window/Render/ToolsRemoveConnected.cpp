#include "ToolsRemoveConnected.h"

#include <QWidget>
#include <QMouseEvent>
#include <QApplication>
#include <QPainter>
#include <QStyleOption>
#include <QCursor>

#include <QVTKOpenGLNativeWidget.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkVolume.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageThreshold.h>
#include <vtkMatrix3x3.h>
#include <vtkSmartPointer.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>
#include <vtkType.h>

#include <array>
#include <queue>
#include <vector>
#include <cmath>
#include <cstring>

#include <vtkVolumeProperty.h>
#include <vtkPiecewiseFunction.h>
#include <type_traits>

#include <vtkExtractVOI.h>
#include <vtkOutlineFilter.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>

#include "Tools.h"

ToolsRemoveConnected::ToolsRemoveConnected(QWidget* hostParent)
    : QObject(nullptr)
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

void ToolsRemoveConnected::attach(QVTKOpenGLNativeWidget* vtk,
    vtkRenderer* renderer,
    vtkImageData* image,
    vtkVolume* volume)
{
    m_vtk = vtk;
    m_renderer = renderer;
    m_image = image;
    m_volume = volume;
    rebuildVisibilityLUT();
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

void ToolsRemoveConnected::ensureHoverPipeline()
{
    if (mHoverActor) return;
    if (!m_renderer || !m_image) return;

    mHoverVOI = vtkSmartPointer<vtkExtractVOI>::New();
    mHoverVOI->SetInputData(m_image);
    mHoverVOI->SetSampleRate(1, 1, 1);

    mHoverOutline = vtkSmartPointer<vtkOutlineFilter>::New();
    mHoverOutline->SetInputConnection(mHoverVOI->GetOutputPort());

    mHoverMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mHoverMapper->SetInputConnection(mHoverOutline->GetOutputPort());

    mHoverActor = vtkSmartPointer<vtkActor>::New();
    mHoverActor->SetMapper(mHoverMapper);
    auto* pr = mHoverActor->GetProperty();
    pr->SetColor(0.2, 1.0, 0.3);   // салатовый контур
    pr->SetLineWidth(3.0);
    pr->SetOpacity(1.0);
    pr->EdgeVisibilityOn();

    m_renderer->AddActor(mHoverActor);
    setHoverVisible(false);
}

void ToolsRemoveConnected::setHoverVisible(bool on)
{
    if (!mHoverActor) return;
    mHoverActor->SetVisibility(on ? 1 : 0);
    mHoverActor->SetPickable(on ? 1 : 0);
    mHoverActor->Modified();
}

void ToolsRemoveConnected::rebuildVisibilityLUT()
{
    mVisibleLut.assign(mLutBins, 0);
    if (!m_image || !m_volume) return;
    auto* prop = m_volume->GetProperty();
    if (!prop) return;

    auto* pwf = prop->GetScalarOpacity(0); // явный компонент
    if (!pwf) return;

    const double range = (mLutMax - mLutMin);
    for (int i = 0; i < mLutBins; ++i) 
    {
        if (i < mHistLo || i > mHistHi)
            continue;
        const double t = (mLutBins == 1) ? 0.0 : double(i) / double(mLutBins - 1);
        const double s = mLutMin + t * range;
        const double op = pwf->GetValue(s);
        mVisibleLut[i] = (op > mVisibleEps) ? 1 : 0;
    }
}

inline bool ToolsRemoveConnected::isVisible(double s) const
{
    if (mVisibleLut.empty() || mLutBins <= 0) return true; // если LUT нет — не режем
    if (s <= mLutMin) return mVisibleLut.front() != 0;
    if (s >= mLutMax) return mVisibleLut.back() != 0;
    const double t = (s - mLutMin) / (mLutMax - mLutMin);
    int idx = static_cast<int>(t * (mLutBins - 1) + 0.5);
    if (idx < 0) idx = 0; else if (idx >= mLutBins) idx = mLutBins - 1;
    return mVisibleLut[idx] != 0;
}

void ToolsRemoveConnected::forwardMouseToVtk(QEvent* e)
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

bool ToolsRemoveConnected::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj != m_overlay || m_state != State::WaitingClick)
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
            return true;
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
                return true;
            }
        }
    }

    switch (ev->type())
    {
    case QEvent::MouseMove:
    {
        auto* me = static_cast<QMouseEvent*>(ev);
        m_lastMouse = me->pos();
        updateHover(m_lastMouse);
        return true; // мы обработали hover
    }
    case QEvent::MouseButtonPress:
    {
        auto* me = static_cast<QMouseEvent*>(ev);
        // Клик по какому-нибудь UI-элементу вне VTK — передаём в UI и выходим (как в ножницах)
        const QPoint screenPt = me->globalPosition().toPoint();
        QWidget* target = QApplication::widgetAt(screenPt);
        const bool isUiTarget =
            target &&
            target != m_overlay &&
            !(m_vtk && (target == m_vtk || m_vtk->isAncestorOf(target)));
        if (isUiTarget) {
            // завершаем инструмент и пробрасываем клик
            cancel();
            // аккуратно перешлём событие в целевой виджет
            QMouseEvent copy(me->type(), target->mapFromGlobal(screenPt), target->mapFromGlobal(screenPt),
                me->globalPosition(), me->button(), me->buttons(), me->modifiers(), me->source());
            QCoreApplication::sendEvent(target, &copy);
            return true;
        }

        if (me->button() == Qt::LeftButton) {
            onLeftClick(me->pos()); // pos в координатах overlay == в координатах m_vtk (мы их синхроним)
            return true;
        }
        if (me->button() == Qt::RightButton) {
            cancel();
            return true;
        }
        break;
    }

    case QEvent::KeyPress:
    {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) { cancel(); return true; }
        break;
    }

    default: break;
    }

    return QObject::eventFilter(obj, ev);
}

bool ToolsRemoveConnected::handle(Action a)
{
    if (a == Action::RemoveUnconnected) { start(a); return true; }
    if (a == Action::RemoveSelected) { start(a);  return true; }
    return false;
}

void ToolsRemoveConnected::onViewResized()
{
    if (!m_vtk || !m_overlay) return;
    const QRect r = m_vtk->geometry();
    m_overlay->setGeometry(r);
}

void ToolsRemoveConnected::cancel()
{
    const bool wasActive = (m_state != State::Off);
    if (!wasActive) return;

    m_state = State::Off;
    m_pts.clear();
    m_hasHover = false;
    mHoverMask = nullptr;

    // 1) Спрятать/убрать актёр и разорвать конвейер
    if (mHoverActor) {
        mHoverActor->SetVisibility(0);
        mHoverActor->SetPickable(0);
        mHoverActor->Modified();
    }
    if (m_renderer && mHoverActor) {
        m_renderer->RemoveActor(mHoverActor);
    }
    mHoverMapper = nullptr;
    mHoverOutline = nullptr;
    mHoverVOI = nullptr;
    mHoverActor = nullptr;

    // 2) Отключить overlay
    if (m_overlay) {
        m_overlay->unsetCursor();
        m_overlay->removeEventFilter(this);
        m_overlay->setMouseTracking(false);
        m_overlay->hide();
    }

    // 3) Форс-перерисовка, чтобы куб исчез визуально
    if (m_vtk && m_vtk->renderWindow())
        m_vtk->renderWindow()->Render();

    if (m_onFinished) m_onFinished();
}

void ToolsRemoveConnected::updateHover(const QPoint& pDevice)
{
    if (!m_image || !m_renderer || !m_vtk) return;
    if (!mHoverMask) return;

    int ijk[3]{};
    if (screenToSeedIJK(pDevice, mHoverMask, ijk))
    {
        m_hoverIJK[0] = ijk[0]; m_hoverIJK[1] = ijk[1]; m_hoverIJK[2] = ijk[2];
        m_hasHover = true;
        ensureHoverPipeline();

        int ext[6]; m_image->GetExtent(ext);
        const int R = std::max(0, m_hoverRadiusVoxels);

        const int i0 = std::max(ext[0], ijk[0] - R);
        const int i1 = std::min(ext[1], ijk[0] + R);
        const int j0 = std::max(ext[2], ijk[1] - R);
        const int j1 = std::min(ext[3], ijk[1] + R);
        const int k0 = std::max(ext[4], ijk[2] - R);
        const int k1 = std::min(ext[5], ijk[2] + R);

        if (mHoverVOI) {
            mHoverVOI->SetVOI(i0, i1, j0, j1, k0, k1);
            mHoverVOI->Modified();
        }
        if (mHoverOutline) mHoverOutline->Modified();
        if (mHoverMapper)  mHoverMapper->Modified();

        setHoverVisible(true);
        if (auto* rw = m_vtk->renderWindow()) rw->Render();
        if (m_overlay) m_overlay->setCursor(Qt::CrossCursor);
    }
    else {
        m_hasHover = false;
        setHoverVisible(false);
        if (auto* rw = m_vtk->renderWindow()) rw->Render();
        if (m_overlay) m_overlay->setCursor(Qt::ForbiddenCursor);
    }
}

void ToolsRemoveConnected::start(Action a)
{
    if (!m_vtk || !m_renderer || !m_volume || !m_image) return;

    // на всякий случай скрыть старый след
    if (mHoverActor) { mHoverActor->SetVisibility(0); mHoverActor->Modified(); }

    m_mode = a;
    m_pts.clear();
    m_state = State::WaitingClick;

    onViewResized();
    mHoverMask = makeBinaryMask(m_image);

    ensureHoverPipeline();
    setHoverVisible(false);
    m_hasHover = false;

    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlay->removeEventFilter(this);
    m_overlay->installEventFilter(this);
    m_overlay->setMouseTracking(true);
    m_overlay->setCursor(Qt::CrossCursor);
    m_overlay->setFocus(Qt::OtherFocusReason);
    m_overlay->show();

    setHoverHighlightSizeVoxels(2);
    redraw();
}

void ToolsRemoveConnected::redraw()
{
    if (m_overlay) 
        m_overlay->update();
}


void ToolsRemoveConnected::onLeftClick(const QPoint& pDevice)
{
    if (!m_image || !m_renderer || !m_vtk) return;

    // 1) Копируем текущий объём: out изначально «как m_image»
    vtkImageData* out = vtkImageData::New(); // refcount=1
    out->DeepCopy(m_image);

    // 2) Строим бинарную маску видимости под конкретную операцию
    vtkSmartPointer<vtkImageData> bin = makeBinaryMask(out);
    if (!bin)
    {
        out->Delete();               // избежать утечки
        return;
    }

    // 3) Получаем seed по лучу через bin
    int seed[3]{ 0,0,0 };
    if (!screenToSeedIJK(pDevice, bin, seed))
    {
        QApplication::beep();
        out->Delete();               // избежать утечки
        return;
    }

    // 4) Заливка по 6-соседству
    std::vector<uint8_t> mark;
    const int cnt = floodFill6(bin, seed, mark);
    if (cnt <= 0)
    {
        QApplication::beep();
        out->Delete();               // избежать утечки
        return;
    }

    if (m_mode == Action::RemoveUnconnected)
        applyKeepOnlySelected(out, mark);   // всё, что не отмечено — занулить
    else
        applyRemoveSelected(out, mark);     // всё, что отмечено — занулить


    if (m_onImageReplaced)
        m_onImageReplaced(out);

    // 8) Перерисовка
    if (m_vtk && m_vtk->renderWindow())
        m_vtk->renderWindow()->Render();

    // 9) Завершение инструмента
    cancel();
}


// ---- ядро ----
vtkSmartPointer<vtkImageData> ToolsRemoveConnected::makeBinaryMask(vtkImageData* src) const
{
    vtkImageData* BinMask = vtkImageData::New();
    BinMask->DeepCopy(src);

    if (!BinMask || !m_volume) return nullptr;

    auto* prop = m_volume->GetProperty();
    auto* otf = prop ? prop->GetScalarOpacity(0) : nullptr;
    const bool haveOTF = (otf && otf->GetSize() >= 2);

    int ext[6];
    BinMask->GetExtent(ext);

    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    // стартовый адрес (xmin, ymin, zmin)
    auto* p0 = static_cast<uint8_t*>(
        BinMask->GetScalarPointer(ext[0], ext[2], ext[4]));

    const int step = 1;

    for (int i = 0; i < nx * ny * nz; i++)
    {
        if (!*(p0 + i))
            continue;

        *(p0 + i) = isVisible(*(p0 + i)) ? 1 : 0;
    }

    return BinMask;
    //int ext[6]; src->GetExtent(ext);
    //vtkNew<vtkImageData> mask;
    //mask->SetExtent(ext);
    //mask->SetSpacing(src->GetSpacing());
    //mask->SetOrigin(src->GetOrigin());
    //if (auto* dm = src->GetDirectionMatrix())
    //    mask->SetDirectionMatrix(dm);   // если есть direction
    //mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    //// ----- подготовка к быстрому проходу -----
    //const int stype = src->GetScalarType();
    //unsigned char* dstBase = static_cast<unsigned char*>(mask->GetScalarPointer());
    //vtkIdType di[3]; mask->GetIncrements(di); // В БАЙТАХ

    //unsigned char* srcBase = static_cast<unsigned char*>(src->GetScalarPointer());
    //vtkIdType si[3]; src->GetIncrements(si);  // В БАЙТАХ

    //auto toBin = [&](double s)->int {
    //    if (mLutMax <= mLutMin) return 0;
    //    double t = (s - mLutMin) / (mLutMax - mLutMin);   // [0..1]
    //    int b = int(std::lround(std::clamp(t, 0.0, 1.0) * 255.0));
    //    return b;
    //    };

    //// Быстрый путь для U8 (необязателен, но ещё ускоряет)
    //unsigned char lutU8[256];
    //const bool useU8Lut = (stype == VTK_UNSIGNED_CHAR) && !mVisibleLut.empty();
    //if (useU8Lut) {
    //    for (int v = 0; v < 256; ++v) {
    //        // видимость по текущей OTF через LUT или напрямую через pwf->GetValue
    //        lutU8[v] = isVisible(double(v)) ? 1u : 0u;
    //    }
    //}

    //// хелпер: маппинг скаляр -> видимость по TF через предрасчитанный LUT
    //auto visibleTF = [&](double s)->int {
    //    return mVisibleLut.empty() ? 1 : (isVisible(s) ? 1 : 0);
    //    };

    //// основной проход
    //for (int k = ext[4]; k <= ext[5]; ++k) {
    //    for (int j = ext[2]; j <= ext[3]; ++j) {
    //        for (int i = ext[0]; i <= ext[1]; ++i) {
    //            unsigned char* sp = srcBase
    //                + (i - ext[0]) * si[0]
    //                + (j - ext[2]) * si[1]
    //                + (k - ext[4]) * si[2];

    //            double v; // читаем РОВНО ОДИН раз
    //            if (useU8Lut) {
    //                v = static_cast<double>(*reinterpret_cast<unsigned char*>(sp));
    //            }
    //            else {
    //                switch (stype) {
    //                case VTK_SHORT:          v = *reinterpret_cast<short*>(sp); break;
    //                case VTK_UNSIGNED_SHORT: v = *reinterpret_cast<unsigned short*>(sp); break;
    //                case VTK_INT:            v = *reinterpret_cast<int*>(sp); break;
    //                case VTK_FLOAT:          v = *reinterpret_cast<float*>(sp); break;
    //                case VTK_DOUBLE:         v = *reinterpret_cast<double*>(sp); break;
    //                case VTK_CHAR:           v = *reinterpret_cast<char*>(sp); break;
    //                default:                 v = *reinterpret_cast<double*>(sp); break;
    //                }
    //            }

    //            // видимость по TF
    //            int visTF = useU8Lut ? int(lutU8[static_cast<unsigned char>(v)] != 0)
    //                : visibleTF(v);

    //            // видимость по Hist (HU): одно сравнение, без повторного чтения
    //            const int visHist = (v >= mHistLo && v <= mHistHi) ? 1 : 0;

    //            const unsigned char vis = (visTF + visHist > 1) ? 1u : 0u;

    //            unsigned char* dp = dstBase
    //                + (i - ext[0]) * di[0]
    //                + (j - ext[2]) * di[1]
    //                + (k - ext[4]) * di[2];
    //            *dp = vis;
    //        }
    //    }
    //}

    /*return mask;*/
}

void ToolsRemoveConnected::displayToWorld(double xd, double yd, double z01, double out[3]) const
{
    m_renderer->SetDisplayPoint(xd, yd, z01);
    m_renderer->DisplayToWorld();
    double w[4]{};
    m_renderer->GetWorldPoint(w);
    const double W = (std::abs(w[3]) > 1e-12) ? w[3] : 1.0;
    out[0] = w[0] / W;
    out[1] = w[1] / W;
    out[2] = w[2] / W;
}

bool ToolsRemoveConnected::worldToIJK(const double world[3], int ijk[3]) const
{
    if (!m_image) return false;

    vtkNew<vtkMatrix3x3> M;
    if (auto* dm = m_image->GetDirectionMatrix()) M->DeepCopy(dm);
    else M->Identity();

    // D^T (world->IJK поворот)
    const double DT[3][3] = {
        { M->GetElement(0,0), M->GetElement(1,0), M->GetElement(2,0) },
        { M->GetElement(0,1), M->GetElement(1,1), M->GetElement(2,1) },
        { M->GetElement(0,2), M->GetElement(1,2), M->GetElement(2,2) },
    };

    double org[3]; m_image->GetOrigin(org);
    double sp[3]; m_image->GetSpacing(sp);

    double rel[3]{ world[0] - org[0], world[1] - org[1], world[2] - org[2] };
    double iF = (DT[0][0] * rel[0] + DT[0][1] * rel[1] + DT[0][2] * rel[2]) / sp[0];
    double jF = (DT[1][0] * rel[0] + DT[1][1] * rel[1] + DT[1][2] * rel[2]) / sp[1];
    double kF = (DT[2][0] * rel[0] + DT[2][1] * rel[1] + DT[2][2] * rel[2]) / sp[2];

    ijk[0] = int(std::floor(iF));
    ijk[1] = int(std::floor(jF));
    ijk[2] = int(std::floor(kF));
    return true;
}

bool ToolsRemoveConnected::screenToSeedIJK(const QPoint& pDevice, vtkImageData* binMask, int ijk[3]) const
{
    if (!m_vtk || !m_renderer || !m_image) return false;
    auto* rw = m_vtk->renderWindow();
    if (!rw) return false;

    const double dpr = m_vtk->devicePixelRatioF();

    QPoint pGlobal = m_overlay ? m_overlay->mapToGlobal(pDevice)
        : m_vtk->mapToGlobal(pDevice);
    QPoint pVtkLogical = m_vtk->mapFromGlobal(pGlobal);

    int vpW = 0, vpH = 0;
    if (int* asz = rw->GetActualSize())
    {
        vpW = asz[0];
        vpH = asz[1];
    }

    const double xd = pVtkLogical.x() * dpr;
    const double yd = (vpH - 1) - pVtkLogical.y() * dpr;

    // Строим near/far → world
    double nearW[3], farW[3];
    displayToWorld(xd, yd, 0.0, nearW);
    displayToWorld(xd, yd, 1.0, farW);

    // world → IJK (float)
    vtkNew<vtkMatrix3x3> M;
    if (auto* dm = m_image->GetDirectionMatrix()) M->DeepCopy(dm);
    else M->Identity();
    const double DT[3][3] = {
        { M->GetElement(0,0), M->GetElement(1,0), M->GetElement(2,0) },
        { M->GetElement(0,1), M->GetElement(1,1), M->GetElement(2,1) },
        { M->GetElement(0,2), M->GetElement(1,2), M->GetElement(2,2) },
    };
    double org[3]; m_image->GetOrigin(org);
    double sp[3]; m_image->GetSpacing(sp);

    auto w2ijk = [&](const double w[3], double o[3]) {
        double rel[3]{ w[0] - org[0], w[1] - org[1], w[2] - org[2] };
        o[0] = (DT[0][0] * rel[0] + DT[0][1] * rel[1] + DT[0][2] * rel[2]) / sp[0];
        o[1] = (DT[1][0] * rel[0] + DT[1][1] * rel[1] + DT[1][2] * rel[2]) / sp[1];
        o[2] = (DT[2][0] * rel[0] + DT[2][1] * rel[1] + DT[2][2] * rel[2]) / sp[2];
        };

    double aF[3], bF[3];
    w2ijk(nearW, aF);
    w2ijk(farW, bF);

    // ===== 3D DDA по IJK =====
    int ext[6]; binMask->GetExtent(ext);

    // Функция доступа к маске
    auto* scal = static_cast<unsigned char*>(binMask->GetScalarPointer());
    vtkIdType inc[3]; binMask->GetIncrements(inc);
    auto atBin = [&](int i, int j, int k)->unsigned char {
        unsigned char* p = scal
            + (i - ext[0]) * inc[0]
            + (j - ext[2]) * inc[1]
            + (k - ext[4]) * inc[2];
        return *p;
        };

    // Старт/конец луча в IJK (double)
    const double sx = aF[0], sy = aF[1], sz = aF[2];
    const double ex = bF[0], ey = bF[1], ez = bF[2];
    double dx = ex - sx, dy = ey - sy, dz = ez - sz;

    // Нормализуем направление так, чтобы шагающий параметр t увеличивался равномерно
    const double tiny = 1e-12;
    if (std::abs(dx) < tiny) dx = (dx >= 0 ? tiny : -tiny);
    if (std::abs(dy) < tiny) dy = (dy >= 0 ? tiny : -tiny);
    if (std::abs(dz) < tiny) dz = (dz >= 0 ? tiny : -tiny);

    // Текущий воксель — по floor (а не round!)
    int ix = static_cast<int>(std::floor(sx + 0.5)); // можно без +0.5, если надо жёстче
    int iy = static_cast<int>(std::floor(sy + 0.5));
    int iz = static_cast<int>(std::floor(sz + 0.5));

    // Направление шага по каждой оси
    int stepx = (dx > 0) ? +1 : -1;
    int stepy = (dy > 0) ? +1 : -1;
    int stepz = (dz > 0) ? +1 : -1;

    // Границы «следующей» плоскости сетки по каждой оси
    auto nextBoundary = [&](double s, int step)->double {
        double cell = std::floor(s);
        return step > 0 ? (cell + 1.0) : cell; // если идём +, то правая грань; если -, то левая
        };

    double txMax = (nextBoundary(sx, stepx) - sx) / dx;
    double tyMax = (nextBoundary(sy, stepy) - sy) / dy;
    double tzMax = (nextBoundary(sz, stepz) - sz) / dz;

    // Сколько t нужно, чтобы пересечь одну ячейку по оси
    const double txDelta = 1.0 / std::abs(dx);
    const double tyDelta = 1.0 / std::abs(dy);
    const double tzDelta = 1.0 / std::abs(dz);

    // Бросаем луч, пока внутри экстента и не прошли конец (t>1)
    auto inExt = [&](int i, int j, int k)->bool {
        return (i >= ext[0] && i <= ext[1] && j >= ext[2] && j <= ext[3] && k >= ext[4] && k <= ext[5]);
        };

    int maxIters = (ext[1] - ext[0] + 1) + (ext[3] - ext[2] + 1) + (ext[5] - ext[4] + 1); // верхняя оценка
    double t = 0.0;

    // Перед DDA — если старт внутри и воксель видимый, берём сразу
    if (inExt(ix, iy, iz) && atBin(ix, iy, iz) != 0) {
        ijk[0] = ix; ijk[1] = iy; ijk[2] = iz; return true;
    }

    while (t <= 1.0 && maxIters-- > 0) {
        // выбираем по какой оси пересекаем ближайшую грань
        if (txMax < tyMax) {
            if (txMax < tzMax) {
                ix += stepx; t = txMax; txMax += txDelta;
            }
            else {
                iz += stepz; t = tzMax; tzMax += tzDelta;
            }
        }
        else {
            if (tyMax < tzMax) {
                iy += stepy; t = tyMax; tyMax += tyDelta;
            }
            else {
                iz += stepz; t = tzMax; tzMax += tzDelta;
            }
        }

        if (!inExt(ix, iy, iz)) continue;
        if (atBin(ix, iy, iz) != 0) {
            ijk[0] = ix; ijk[1] = iy; ijk[2] = iz;
            return true;
        }
    }
    return false;
}

int ToolsRemoveConnected::floodFill6(vtkImageData* binMask, const int seed[3], std::vector<uint8_t>& mark) const
{
    if (!binMask) return 0;

    int ext[6]; binMask->GetExtent(ext);
    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;
    const auto idx = [&](int i, int j, int k) {
        return (k - ext[4]) * (nx * ny) + (j - ext[2]) * nx + (i - ext[0]);
        };

    auto inExt = [&](int i, int j, int k)->bool {
        return (i >= ext[0] && i <= ext[1] && j >= ext[2] && j <= ext[3] && k >= ext[4] && k <= ext[5]);
        };

    auto* scal = static_cast<unsigned char*>(binMask->GetScalarPointer());
    vtkIdType inc[3]; binMask->GetIncrements(inc);

    auto at = [&](int i, int j, int k)->unsigned char {
        unsigned char* p = scal
            + (i - ext[0]) * inc[0]
            + (j - ext[2]) * inc[1]
            + (k - ext[4]) * inc[2];
        return *p;
        };

    if (!inExt(seed[0], seed[1], seed[2]) || at(seed[0], seed[1], seed[2]) == 0)
        return 0;

    mark.assign(size_t(nx * ny * nz), 0);

    struct V { int i, j, k; };
    std::queue<V> q;
    q.push({ seed[0],seed[1],seed[2] });
    mark[size_t(idx(seed[0], seed[1], seed[2]))] = 1;

    static const int N6[6][3] = { {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1} };

    int visited = 0;
    while (!q.empty()) {
        auto v = q.front(); q.pop();
        ++visited;

        for (auto& d : N6) {
            const int ni = v.i + d[0], nj = v.j + d[1], nk = v.k + d[2];
            if (!inExt(ni, nj, nk)) continue;
            const size_t w = size_t(idx(ni, nj, nk));
            if (mark[w]) continue;
            if (at(ni, nj, nk) == 0) continue;
            mark[w] = 1;
            q.push({ ni,nj,nk });
        }
    }
    return visited;
}

void ToolsRemoveConnected::applyKeepOnlySelected(vtkImageData* image, const std::vector<uint8_t>& mark) const
{
    if (!image) return;

    // проверим тип
    if (image->GetScalarType() != VTK_UNSIGNED_CHAR ||
        image->GetNumberOfScalarComponents() != 1)
    {
        return;
    }

    int ext[6];
    image->GetExtent(ext);

    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    vtkIdType incX, incY, incZ;  // ВНИМАНИЕ: ИНКРЕМЕНТЫ В БАЙТАХ
    image->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    uint8_t* p0 = static_cast<uint8_t*>(image->GetScalarPointer(ext[0], ext[2], ext[4]));
    if (!p0) return;

    const int step = 1; // без субсэмплинга

    auto idx = [&](int i, int j, int k)->size_t {
        // здесь i,j,k — относительные (0..nx-1/ny-1/nz-1)
        return size_t(k) * (nx * ny) + size_t(j) * nx + size_t(i);
        };

    // Проход с записью: если mark[w]==0 -> зануляем байт вокселя
    uint8_t* pz = p0;
    for (int k = 0; k < nz; ++k) {
        uint8_t* py = pz;
        for (int j = 0; j < ny; ++j) {
            uint8_t* px = py;
            for (int i = 0; i < nx; ++i) {
                const size_t w = idx(i, j, k);
                if (!mark[w])
                    *px = 0u;
                px += incX;
            }
            py += incY;
        }
        pz += incZ;
    }
}

void ToolsRemoveConnected::applyRemoveSelected(vtkImageData* image, const std::vector<uint8_t>& mark) const
{
    if (!image) return;

    // проверим тип
    if (image->GetScalarType() != VTK_UNSIGNED_CHAR ||
        image->GetNumberOfScalarComponents() != 1)
    {
        return;
    }

    int ext[6];
    image->GetExtent(ext);

    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    vtkIdType incX, incY, incZ;  // ВНИМАНИЕ: ИНКРЕМЕНТЫ В БАЙТАХ
    image->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    uint8_t* p0 = static_cast<uint8_t*>(image->GetScalarPointer(ext[0], ext[2], ext[4]));
    if (!p0) return;

    const int step = 1; // без субсэмплинга

    auto idx = [&](int i, int j, int k)->size_t {
        // здесь i,j,k — относительные (0..nx-1/ny-1/nz-1)
        return size_t(k) * (nx * ny) + size_t(j) * nx + size_t(i);
        };

    // Проход с записью: если mark[w]==0 -> зануляем байт вокселя
    uint8_t* pz = p0;
    for (int k = 0; k < nz; ++k) {
        uint8_t* py = pz;
        for (int j = 0; j < ny; ++j) {
            uint8_t* px = py;
            for (int i = 0; i < nx; ++i) {
                const size_t w = idx(i, j, k);
                if (mark[w]) 
                    *px = 0u;   
                px += incX;               // шаг по X в байтах (для UCHAR,1с = 1)
            }
            py += incY;                   // шаг по Y
        }
        pz += incZ;                       // шаг по Z
    }
}