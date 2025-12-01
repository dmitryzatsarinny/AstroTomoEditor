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

void ToolsRemoveConnected::removeUnconnected(const std::vector<uint8_t>& selMask)
{
    if (!m_vol.u8().valid || !m_vol.raw())
        return;
    applyKeepMask(m_vol.raw(), selMask);
}

void ToolsRemoveConnected::ensureHoverPipeline()
{
    if (!m_renderer || !m_image) return;

    // старый куб
    if (!mHoverActor) {
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
        pr->SetColor(0.2, 1.0, 0.3);
        pr->SetLineWidth(3.0);
        pr->SetOpacity(1.0);
        pr->EdgeVisibilityOn();

        m_renderer->AddActor(mHoverActor);
        mHoverActor->SetVisibility(0);
        mHoverActor->SetPickable(0);
    }

    // новая сфера
    if (!mBrushActor) {
        mBrushSphere = vtkSmartPointer<vtkSphereSource>::New();
        mBrushSphere->SetThetaResolution(24);
        mBrushSphere->SetPhiResolution(24);

        mBrushMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mBrushMapper->SetInputConnection(mBrushSphere->GetOutputPort());

        mBrushActor = vtkSmartPointer<vtkActor>::New();
        mBrushActor->SetMapper(mBrushMapper);
        auto* pr = mBrushActor->GetProperty();
        pr->SetColor(1.0, 0.3, 0.3);
        pr->SetLineWidth(1.5);
        pr->SetRepresentationToWireframe();
        pr->SetOpacity(1.0);

        m_renderer->AddActor(mBrushActor);
        mBrushActor->SetVisibility(0);
        mBrushActor->SetPickable(0);
    }
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

    if (ev->type() == QEvent::Wheel &&
        (m_mode == Action::VoxelEraser || m_mode == Action::VoxelRecovery))
    {
        auto* we = static_cast<QWheelEvent*>(ev);
        const int delta = we->angleDelta().y();
        if (delta != 0)
        {
            int steps = delta / 120;  // один щелчок колеса
            if (steps == 0)
                steps = (delta > 0 ? 1 : -1);

            int r = (m_hoverRadiusVoxels > 0 ? m_hoverRadiusVoxels : 1);
            r += steps;
            if (r < 1)  r = 1;
            if (r > 99) r = 99;       // под твой Range(1..99)

            m_hoverRadiusVoxels = r;

            // перерисовать hover-сферу под новой толщиной
            if (m_hasHover)
                updateHover(m_lastMouse);

            return true; // колесо целиком съели, без навигации
        }
    }

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
    if (a == Action::RemoveUnconnected ||
        a == Action::RemoveSelected ||
        a == Action::RemoveConnected ||
        a == Action::VoxelEraser ||
        a == Action::VoxelRecovery)
    {
        start(a);
        return true;
    }
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
    m_bin.clear();
    m_vol.clear();

    // 1) Спрятать/убрать актёр и разорвать конвейер
    if (mHoverActor) {
        mHoverActor->SetVisibility(0);
        mHoverActor->SetPickable(0);
        mHoverActor->Modified();
    }
    if (mBrushActor) { 
        mBrushActor->SetVisibility(0); 
        mBrushActor->SetPickable(0); 
        mBrushActor->Modified();
    }
    if (m_renderer && mHoverActor) m_renderer->RemoveActor(mHoverActor);
    if (m_renderer && mBrushActor) m_renderer->RemoveActor(mBrushActor);
    mBrushSphere = nullptr;
    mBrushMapper = nullptr;
    mBrushActor = nullptr;
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

    m_image = nullptr;
    m_volume = nullptr;

    if (m_onFinished) m_onFinished();
}

void ToolsRemoveConnected::updateHover(const QPoint& pDevice)
{
    if (!m_image || !m_renderer || !m_vtk) return;
    if (!m_bin) return;

    int ijk[3]{};
    if (screenToSeedIJK(pDevice, ijk))
    {
        m_hoverIJK[0] = ijk[0];
        m_hoverIJK[1] = ijk[1];
        m_hoverIJK[2] = ijk[2];
        m_hasHover = true;

        ensureHoverPipeline();

        // общий радиус в вокселях
        int Rvox = 0;
        if (m_mode == Action::VoxelEraser || m_mode == Action::VoxelRecovery)
            Rvox = std::max(1, m_hoverRadiusVoxels);
        else
            Rvox = 2;

        // --- режим кисти: показываем сферу ---
        if (m_mode == Action::VoxelEraser || m_mode == Action::VoxelRecovery)
        {
            if (mHoverActor) { mHoverActor->SetVisibility(0); }

            if (mBrushActor && mBrushSphere)
            {
                double sp[3]; m_image->GetSpacing(sp);
                const double minSp = std::min({ sp[0], sp[1], sp[2] });
                const double radiusMm = Rvox * minSp;

                double centerW[3];
                ijkToWorld(ijk, centerW);

                mBrushSphere->SetCenter(centerW);
                mBrushSphere->SetRadius(radiusMm);
                mBrushSphere->Modified();

                auto* pr = mBrushActor->GetProperty();
                if (m_mode == Action::VoxelEraser)
                    pr->SetColor(1.0, 0.3, 0.3);
                else
                    pr->SetColor(0.3, 0.9, 1.0);

                mBrushActor->SetVisibility(1);
                mBrushActor->SetPickable(0);
            }
        }
        else
        {
            // --- старые режимы: куб VOI ---
            if (mBrushActor) mBrushActor->SetVisibility(0);

            int ext[6]; m_image->GetExtent(ext);
            const int i0 = std::max(ext[0], ijk[0] - Rvox);
            const int i1 = std::min(ext[1], ijk[0] + Rvox);
            const int j0 = std::max(ext[2], ijk[1] - Rvox);
            const int j1 = std::min(ext[3], ijk[1] + Rvox);
            const int k0 = std::max(ext[4], ijk[2] - Rvox);
            const int k1 = std::min(ext[5], ijk[2] + Rvox);

            if (mHoverVOI) {
                mHoverVOI->SetVOI(i0, i1, j0, j1, k0, k1);
                mHoverVOI->Modified();
            }
            if (mHoverOutline) mHoverOutline->Modified();
            if (mHoverMapper)  mHoverMapper->Modified();

            if (mHoverActor)
            {
                auto* pr = mHoverActor->GetProperty();
                pr->SetColor(0.2, 1.0, 0.3);
                mHoverActor->SetVisibility(1);
                mHoverActor->SetPickable(0);
            }
        }

        if (auto* rw = m_vtk->renderWindow()) rw->Render();
        if (m_overlay) m_overlay->setCursor(Qt::CrossCursor);
    }
    else
    {
        m_hasHover = false;
        if (mHoverActor)  mHoverActor->SetVisibility(0);
        if (mBrushActor)  mBrushActor->SetVisibility(0);
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
    makeBinaryMask(m_image);
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

    m_vol.clear();
    m_vol.copy(m_image);

    m_bin.clear();
    onViewResized();
    makeBinaryMask(m_image);

    int seed[3]{ 0,0,0 };
    if (!screenToSeedIJK(pDevice, seed))
    {
        QApplication::beep();
        return;
    }

    // === кисти работают без floodFill ===
    if (m_mode == Action::VoxelEraser)
    {
        applyVoxelErase(seed);
    }
    else if (m_mode == Action::VoxelRecovery)
    {
        applyVoxelRecover(seed);
    }
    else
    {
        // == старая логика для Remove* ==
        std::vector<uint8_t> mark;
        const int cnt = floodFill6(m_bin, seed, mark);
        if (cnt <= 0)
        {
            QApplication::beep();
            return;
        }

        switch (m_mode)
        {
        case Action::RemoveUnconnected:
            applyKeepOnlySelected(mark);
            break;
        case Action::RemoveSelected:
            applyRemoveSelected(mark);
            break;
        case Action::RemoveConnected:
            RemoveConnectedRegions(mark, seed);
            break;
        default:
            break;
        }
    }

    if (m_vol.raw())
        m_vol.raw()->Modified();
    if (m_onImageReplaced)
        m_onImageReplaced(m_vol.raw());

    if (m_vtk && m_vtk->renderWindow())
        m_vtk->renderWindow()->Render();

    if (m_mode != Action::VoxelEraser && m_mode != Action::VoxelRecovery)
        cancel();
}


static inline size_t linearIdx(int i, int j, int k, const int ext[6], int nx, int ny) {
    return size_t(k - ext[4]) * (nx * ny) + size_t(j - ext[2]) * nx + size_t(i - ext[0]);
}

void ToolsRemoveConnected::ClearOriginalSnapshot()
{
    if (m_hasOrig)
    {
        m_orig.clear();
        m_hasOrig = false;
    }
}


void ToolsRemoveConnected::EnsureOriginalSnapshot(vtkImageData* _image)
{
    if (m_hasOrig)
        return;
    if (!_image)
        return;

    m_orig.clear();
    m_orig.copy(_image);
    m_hasOrig = true;
}

void ToolsRemoveConnected::applyVoxelErase(const int seed[3])
{
    vtkImageData* im = m_vol.raw();
    if (!im) return;

    int ext[6]; im->GetExtent(ext);

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    int R = m_hoverRadiusVoxels > 0 ? m_hoverRadiusVoxels : 1;
    int R2 = R * R;

    const int i0 = std::max(ext[0], seed[0] - R);
    const int i1 = std::min(ext[1], seed[0] + R);
    const int j0 = std::max(ext[2], seed[1] - R);
    const int j1 = std::min(ext[3], seed[1] + R);
    const int k0 = std::max(ext[4], seed[2] - R);
    const int k1 = std::min(ext[5], seed[2] + R);

    auto* p0 = static_cast<unsigned char*>(
        im->GetScalarPointer(ext[0], ext[2], ext[4]));
    vtkIdType incX, incY, incZ;
    im->GetIncrements(incX, incY, incZ);

    for (int k = k0; k <= k1; ++k)
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
            {
                if (!inExt(i, j, k)) continue;

                const int di = i - seed[0];
                const int dj = j - seed[1];
                const int dk = k - seed[2];
                if (di * di + dj * dj + dk * dk > R2)
                    continue;

                unsigned char* p = p0
                    + (i - ext[0]) * incX
                    + (j - ext[2]) * incY
                    + (k - ext[4]) * incZ;

                const unsigned char v = *p;
                if (!v) continue;
                if (!isVisible(double(v))) continue;

                *p = 0u;
            }
}


void ToolsRemoveConnected::applyVoxelRecover(const int seed[3])
{
    vtkImageData* curIm = m_vol.raw();
    vtkImageData* origIm = m_orig.raw();
    if (!curIm || !origIm) return;

    int ext[6]; curIm->GetExtent(ext);

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    int R = m_hoverRadiusVoxels > 0 ? m_hoverRadiusVoxels : 1;
    int R2 = R * R;

    const int i0 = std::max(ext[0], seed[0] - R);
    const int i1 = std::min(ext[1], seed[0] + R);
    const int j0 = std::max(ext[2], seed[1] - R);
    const int j1 = std::min(ext[3], seed[1] + R);
    const int k0 = std::max(ext[4], seed[2] - R);
    const int k1 = std::min(ext[5], seed[2] + R);

    auto* baseCur = static_cast<unsigned char*>(
        curIm->GetScalarPointer(ext[0], ext[2], ext[4]));
    auto* baseOrig = static_cast<unsigned char*>(
        origIm->GetScalarPointer(ext[0], ext[2], ext[4]));

    vtkIdType incXC, incYC, incZC;
    vtkIdType incXO, incYO, incZO;
    curIm->GetIncrements(incXC, incYC, incZC);
    origIm->GetIncrements(incXO, incYO, incZO);

    for (int k = k0; k <= k1; ++k)
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
            {
                if (!inExt(i, j, k)) continue;

                const int di = i - seed[0];
                const int dj = j - seed[1];
                const int dk = k - seed[2];
                if (di * di + dj * dj + dk * dk > R2)
                    continue;

                unsigned char* pCur = baseCur
                    + (i - ext[0]) * incXC
                    + (j - ext[2]) * incYC
                    + (k - ext[4]) * incZC;

                const unsigned char* pOrig = baseOrig
                    + (i - ext[0]) * incXO
                    + (j - ext[2]) * incYO
                    + (k - ext[4]) * incZO;

                const unsigned char ov = *pOrig;
                if (!ov) continue;
                if (!isVisible(double(ov))) continue; // восстанавливаем только видимое

                *pCur = ov;
            }
}


void ToolsRemoveConnected::RemoveConnectedRegions(const std::vector<uint8_t>& mark,
    const int seedIn[3])
{
    // 1) проверяем объём
    const auto& S = m_vol.u8();
    if (!S.valid || !S.p0 || !m_vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    // размер mark должен соответствовать объёму
    if (mark.size() < total)
        return;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    if (!inExt(seedIn[0], seedIn[1], seedIn[2]))
        return;

    int seed[3]{ seedIn[0], seedIn[1], seedIn[2] };
    const size_t seedIdx = linearIdx(seed[0], seed[1], seed[2], ext, nx, ny);
    if (seedIdx >= total || mark[seedIdx] == 0)
        return; // seed не попал в выбранную компоненту

    // 2) геодезический радиус вокруг seed в вокселях
    //    задаём в мм, превращаем в воксели по минимальному шагу
    double sp[3]{ 1.0, 1.0, 1.0 };
    if (m_image)
        m_image->GetSpacing(sp);
    const double minSp = std::min({ sp[0], sp[1], sp[2] });

    const double radiusMM = 70.0;          // можно подкрутить на вкус
    const int maxSteps = std::max(1, int(radiusMM / std::max(minSp, 1e-6) + 0.5));

    // 3) BFS от seed по 6-соседству внутри mark с ограничением по dist
    std::vector<uint8_t> core(total, 0);   // что оставить в компоненте
    std::vector<uint8_t> visited(total, 0);

    struct Node { int i, j, k, d; };
    std::queue<Node> q;

    q.push({ seed[0], seed[1], seed[2], 0 });
    visited[seedIdx] = 1;
    core[seedIdx] = 1;

    static const int N6[6][3] = {
        {+1,0,0},{-1,0,0},
        {0,+1,0},{0,-1,0},
        {0,0,+1},{0,0,-1}
    };

    while (!q.empty())
    {
        Node v = q.front(); q.pop();
        if (v.d >= maxSteps)
            continue;

        for (const auto& d : N6)
        {
            const int ni = v.i + d[0];
            const int nj = v.j + d[1];
            const int nk = v.k + d[2];
            if (!inExt(ni, nj, nk))
                continue;

            const size_t w = linearIdx(ni, nj, nk, ext, nx, ny);
            if (w >= total)
                continue;

            if (!mark[w])          // вне исходной компоненты
                continue;
            if (visited[w])        // уже были
                continue;

            visited[w] = 1;
            core[w] = 1;
            q.push({ ni, nj, nk, v.d + 1 });
        }
    }

    // 4) Вычёркиваем из тома всё, что в выбранной компоненте, но не в core
    for (size_t n = 0; n < total; ++n)
    {
        if (mark[n] && !core[n])
            m_vol.at(n) = 0u;
    }
}

// ---- ядро ----
void ToolsRemoveConnected::makeBinaryMask(vtkImageData* image)
{
    if (!image) {
        m_bin.clear();
        return;
    }

    const bool brushMode =
        (m_mode == Action::VoxelEraser || m_mode == Action::VoxelRecovery);

    if (brushMode) 
    {
        m_bin.set(image, [](uint8_t v) {
            return v != 0;
            });
    }
    else 
    {
        m_bin.set(image, [&](uint8_t v) {
            return v != 0 && isVisible(double(v));
            });
    }
}

// Ищет ближайшего непустого соседа вокруг seed на image (>0).
// Порядок: 6-соседей → 18-соседей → 26-соседей. Ранний выход.
bool ToolsRemoveConnected::findNearestNonEmptyConnectedVoxel(vtkImageData* image,
    const int seed[3],
    int outIJK[3]) const
{
    if (!image) return false;

    int ext[6]; image->GetExtent(ext);
    auto inExt = [&](int i, int j, int k)->bool {
        return (i >= ext[0] && i <= ext[1] && j >= ext[2] && j <= ext[3] && k >= ext[4] && k <= ext[5]);
        };

    // быстрый доступ к байту
    auto* base = static_cast<unsigned char*>(image->GetScalarPointer(ext[0], ext[2], ext[4]));
    if (!base) return false;

    vtkIdType incX, incY, incZ; image->GetIncrements(incX, incY, incZ);
    auto at = [&](int i, int j, int k)->unsigned char {
        unsigned char* p = base
            + (i - ext[0]) * incX
            + (j - ext[2]) * incY
            + (k - ext[4]) * incZ;
        return *p;
        };

    // 0) сам seed
    if (inExt(seed[0], seed[1], seed[2]) && at(seed[0], seed[1], seed[2]) > 0) {
        outIJK[0] = seed[0]; outIJK[1] = seed[1]; outIJK[2] = seed[2];
        return true;
    }

    // Наборы смещений: 6, 18 (плоско-диагональные), 26 (пространственные диагонали)
    static const int N6[][3] = {
        {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1}
    };
    static const int N18[][3] = {
        // сначала рёбра в плоскостях (|di|+|dj|==2 или |di|+|dk|==2 или |dj|+|dk|==2)
        {+1,+1,0},{+1,-1,0},{-1,+1,0},{-1,-1,0},
        {+1,0,+1},{+1,0,-1},{-1,0,+1},{-1,0,-1},
        {0,+1,+1},{0,+1,-1},{0,-1,+1},{0,-1,-1}
    };
    static const int N26_diag[][3] = {
        // пространственные диагонали (три координаты ненулевые)
        {+1,+1,+1},{+1,+1,-1},{+1,-1,+1},{+1,-1,-1},
        {-1,+1,+1},{-1,+1,-1},{-1,-1,+1},{-1,-1,-1}
    };

    const int si = seed[0], sj = seed[1], sk = seed[2];

    auto tryList = [&](const int (*offs)[3], int count)->bool {
        for (int t = 0; t < count; ++t) {
            const int i = si + offs[t][0];
            const int j = sj + offs[t][1];
            const int k = sk + offs[t][2];
            if (!inExt(i, j, k)) continue;
            if (at(i, j, k) > 0) { outIJK[0] = i; outIJK[1] = j; outIJK[2] = k; return true; }
        }
        return false;
        };

    if (tryList(N6, int(std::size(N6))))       return true;
    if (tryList(N18, int(std::size(N18))))      return true;
    if (tryList(N26_diag, int(std::size(N26_diag)))) return true;

    return false;
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

void ToolsRemoveConnected::ijkToWorld(const int ijk[3], double world[3]) const
{
    if (!m_image) {
        world[0] = world[1] = world[2] = 0.0;
        return;
    }

    vtkNew<vtkMatrix3x3> M;
    if (auto* dm = m_image->GetDirectionMatrix()) M->DeepCopy(dm);
    else M->Identity();

    double org[3]; m_image->GetOrigin(org);
    double sp[3];  m_image->GetSpacing(sp);

    // index -> physical
    double r[3] = {
        ijk[0] * sp[0],
        ijk[1] * sp[1],
        ijk[2] * sp[2]
    };

    // world = origin + D * r
    world[0] = org[0]
        + M->GetElement(0, 0) * r[0]
        + M->GetElement(0, 1) * r[1]
        + M->GetElement(0, 2) * r[2];
    world[1] = org[1]
        + M->GetElement(1, 0) * r[0]
        + M->GetElement(1, 1) * r[1]
        + M->GetElement(1, 2) * r[2];
    world[2] = org[2]
        + M->GetElement(2, 0) * r[0]
        + M->GetElement(2, 1) * r[1]
        + M->GetElement(2, 2) * r[2];
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

bool ToolsRemoveConnected::screenToSeedIJK(const QPoint& pDevice, int ijk[3]) const
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
    int ext[6]; m_bin.raw()->GetExtent(ext);

    // Функция доступа к маске
    auto* scal = static_cast<unsigned char*>(m_bin.raw()->GetScalarPointer());
    vtkIdType inc[3]; m_bin.raw()->GetIncrements(inc);
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


//#include <vtkMatrix3x3.h>
//
//bool ToolsRemoveConnected::worldToIJK(const double world[3], int ijk[3]) const
//{
//    if (!m_image) return false;
//
//    // Direction (D), origin, spacing
//    vtkNew<vtkMatrix3x3> M;
//    if (auto* dm = m_image->GetDirectionMatrix()) M->DeepCopy(dm);
//    else M->Identity();
//
//    double org[3]; m_image->GetOrigin(org);
//    double sp[3]; m_image->GetSpacing(sp);
//
//    // D^T: world -> IJK (без учёта смещения/масштаба)
//    const double DT00 = M->GetElement(0, 0), DT01 = M->GetElement(1, 0), DT02 = M->GetElement(2, 0);
//    const double DT10 = M->GetElement(0, 1), DT11 = M->GetElement(1, 1), DT12 = M->GetElement(2, 1);
//    const double DT20 = M->GetElement(0, 2), DT21 = M->GetElement(1, 2), DT22 = M->GetElement(2, 2);
//
//    const double rx = world[0] - org[0];
//    const double ry = world[1] - org[1];
//    const double rz = world[2] - org[2];
//
//    const double iF = (DT00 * rx + DT01 * ry + DT02 * rz) / sp[0];
//    const double jF = (DT10 * rx + DT11 * ry + DT12 * rz) / sp[1];
//    const double kF = (DT20 * rx + DT21 * ry + DT22 * rz) / sp[2];
//
//    ijk[0] = static_cast<int>(std::floor(iF));
//    ijk[1] = static_cast<int>(std::floor(jF));
//    ijk[2] = static_cast<int>(std::floor(kF));
//
//    // проверка на выход за экстенты тома
//    const int* ext = input->u8().ext;
//    ijk[0] = std::clamp(ijk[0], ext[0], ext[1]);
//    ijk[1] = std::clamp(ijk[1], ext[2], ext[3]);
//    ijk[2] = std::clamp(ijk[2], ext[4], ext[5]);
//    return true;
//}
//
//
//bool ToolsRemoveConnected::screenToSeedIJK(const QPoint& pDevice, int ijk[3]) const
//{
//    if (!m_vtk || !m_renderer || !m_image)
//        return false;
//
//    const auto& S = input->u8();
//    const int* ext = S.ext;
//
//    auto* rw = m_vtk->renderWindow();
//    if (!rw) return false;
//
//    const double dpr = m_vtk->devicePixelRatioF();
//    const QPoint pGlobal = m_overlay ? m_overlay->mapToGlobal(pDevice)
//        : m_vtk->mapToGlobal(pDevice);
//    const QPoint pVtkLogical = m_vtk->mapFromGlobal(pGlobal);
//
//    int vpW = 0, vpH = 0;
//    if (int* asz = rw->GetActualSize()) { vpW = asz[0]; vpH = asz[1]; }
//
//    const double xd = pVtkLogical.x() * dpr;
//    const double yd = (vpH - 1) - pVtkLogical.y() * dpr;
//
//    // ---- экран → мир (near/far) ----
//    double nearW[3], farW[3];
//    displayToWorld(xd, yd, 0.0, nearW);
//    displayToWorld(xd, yd, 1.0, farW);
//
//    // ---- мир → IJK (вещественные) ----
//    vtkNew<vtkMatrix3x3> M;
//    if (auto* dm = m_image->GetDirectionMatrix()) M->DeepCopy(dm);
//    else M->Identity();
//
//    const double DT00 = M->GetElement(0, 0), DT01 = M->GetElement(1, 0), DT02 = M->GetElement(2, 0);
//    const double DT10 = M->GetElement(0, 1), DT11 = M->GetElement(1, 1), DT12 = M->GetElement(2, 1);
//    const double DT20 = M->GetElement(0, 2), DT21 = M->GetElement(1, 2), DT22 = M->GetElement(2, 2);
//
//    double org[3]; input->raw()->GetOrigin(org);
//    double sp[3]; input->raw()->GetSpacing(sp);
//
//    auto w2ijk = [&](const double w[3], double o[3]) {
//        const double rx = w[0] - org[0], ry = w[1] - org[1], rz = w[2] - org[2];
//        o[0] = (DT00 * rx + DT01 * ry + DT02 * rz) / sp[0];
//        o[1] = (DT10 * rx + DT11 * ry + DT12 * rz) / sp[1];
//        o[2] = (DT20 * rx + DT21 * ry + DT22 * rz) / sp[2];
//        };
//
//    double aF[3], bF[3];
//    w2ijk(nearW, aF);
//    w2ijk(farW, bF);
//
//    auto inExt = [&](int i, int j, int k)->bool {
//        return (i >= ext[0] && i <= ext[1] &&
//            j >= ext[2] && j <= ext[3] &&
//            k >= ext[4] && k <= ext[5]);
//        };
//
//    // ---- DDA по вокселям ----
//    double sx = aF[0], sy = aF[1], sz = aF[2];
//    double ex = bF[0], ey = bF[1], ez = bF[2];
//    double dx = ex - sx, dy = ey - sy, dz = ez - sz;
//
//    const double tiny = 1e-12;
//    if (std::abs(dx) < tiny) dx = (dx >= 0 ? tiny : -tiny);
//    if (std::abs(dy) < tiny) dy = (dy >= 0 ? tiny : -tiny);
//    if (std::abs(dz) < tiny) dz = (dz >= 0 ? tiny : -tiny);
//
//    int ix = static_cast<int>(std::floor(sx));
//    int iy = static_cast<int>(std::floor(sy));
//    int iz = static_cast<int>(std::floor(sz));
//
//    const int stepx = (dx > 0) ? +1 : -1;
//    const int stepy = (dy > 0) ? +1 : -1;
//    const int stepz = (dz > 0) ? +1 : -1;
//
//    auto nextBoundary = [&](double s, int step)->double {
//        const double cell = std::floor(s);
//        return (step > 0) ? (cell + 1.0) : cell;
//        };
//
//    double txMax = (nextBoundary(sx, stepx) - sx) / dx;
//    double tyMax = (nextBoundary(sy, stepy) - sy) / dy;
//    double tzMax = (nextBoundary(sz, stepz) - sz) / dz;
//
//    const double txDelta = 1.0 / std::abs(dx);
//    const double tyDelta = 1.0 / std::abs(dy);
//    const double tzDelta = 1.0 / std::abs(dz);
//
//    // быстрый старт: если уже в видимой ячейке
//    if (inExt(ix, iy, iz) && m_bin.at(ix, iy, iz) != 0) {
//        ijk[0] = ix; ijk[1] = iy; ijk[2] = iz; return true;
//    }
//
//    int maxIters = (ext[1] - ext[0] + 1) + (ext[3] - ext[2] + 1) + (ext[5] - ext[4] + 1);
//    double t = 0.0;
//
//    while (t <= 1.0 && maxIters-- > 0) {
//        if (txMax < tyMax) {
//            if (txMax < tzMax) { ix += stepx; t = txMax; txMax += txDelta; }
//            else { iz += stepz; t = tzMax; tzMax += tzDelta; }
//        }
//        else {
//            if (tyMax < tzMax) { iy += stepy; t = tyMax; tyMax += tyDelta; }
//            else { iz += stepz; t = tzMax; tzMax += tzDelta; }
//        }
//        if (!inExt(ix, iy, iz)) continue;
//        if (m_bin.at(ix, iy, iz) != 0) 
//        {
//            ijk[0] = ix; ijk[1] = iy; ijk[2] = iz; return true;
//        }
//    }
//    return false;
//}


int ToolsRemoveConnected::floodFill6(const Volume& bin,
    const int seed[3],
    std::vector<uint8_t>& mark) const
{
    const auto& S = bin.u8();
    if (!S.valid || !S.p0) return 0;

    const int* ext = S.ext;
    const int nx = S.nx, ny = S.ny, nz = S.nz;

    auto idx = [&](int i, int j, int k) -> size_t {
        return size_t(k - ext[4]) * size_t(nx * ny)
            + size_t(j - ext[2]) * size_t(nx)
            + size_t(i - ext[0]);
        };

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    // Чтение 0/1 из бинарника по инкрементам (байты)
    const unsigned char* base = reinterpret_cast<const unsigned char*>(S.p0);
    const vtkIdType incx = S.incX, incy = S.incY, incz = S.incZ;

    auto at = [&](int i, int j, int k) -> unsigned char {
        const unsigned char* p = base
            + (i - ext[0]) * incx
            + (j - ext[2]) * incy
            + (k - ext[4]) * incz;
        return *p;
        };

    if (!inExt(seed[0], seed[1], seed[2]) || at(seed[0], seed[1], seed[2]) == 0)
        return 0;

    mark.assign(size_t(nx) * ny * nz, 0);

    struct V { int i, j, k; };
    std::queue<V> q;
    q.push({ seed[0], seed[1], seed[2] });
    mark[idx(seed[0], seed[1], seed[2])] = 1;

    static const int N6[6][3] = {
        {+1,0,0},{-1,0,0},
        {0,+1,0},{0,-1,0},
        {0,0,+1},{0,0,-1}
    };

    int visited = 0;
    while (!q.empty()) {
        auto v = q.front(); q.pop();
        ++visited;

        for (const auto& d : N6) {
            const int ni = v.i + d[0];
            const int nj = v.j + d[1];
            const int nk = v.k + d[2];
            if (!inExt(ni, nj, nk)) continue;

            const size_t w = idx(ni, nj, nk);
            if (mark[w]) continue;
            if (at(ni, nj, nk) == 0) continue;

            mark[w] = 1;
            q.push({ ni, nj, nk });
        }
    }
    return visited;
}


void ToolsRemoveConnected::applyKeepOnlySelected(const std::vector<uint8_t>& mark)
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();
    if (mark.size() < total) return; // подстраховка

    for (size_t n = 0; n < total; ++n)
    {
        if (!mark[n])               // если маска 0 → обнуляем воксель
            m_vol.at(n) = 0u;
    }
}


bool ToolsRemoveConnected::buildSelectedComponentMask(vtkImageData* image,
    const int seed[3],
    std::vector<uint8_t>& outMask,
    int& outCount) const
{
////    if (!image) return false;
//
////    m_bin = makeBinaryMask(image);
// //   if (!bin) return false;
//
// //   int ext[6]; bin->GetExtent(ext);
//    const int nx = ext[1] - ext[0] + 1, ny = ext[3] - ext[2] + 1, nz = ext[5] - ext[4] + 1;
//    outMask.clear();
//    outMask.reserve(size_t(nx) * ny * nz);
//
// //   outCount = floodFill6(bin, seed, outMask);
//    return outCount > 0;
    return true;
}

void ToolsRemoveConnected::applyRemoveSelected(const std::vector<uint8_t>& selMask)
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();
    if (selMask.size() < total) return;

    for (size_t n = 0; n < total; ++n)
    {
        if (selMask[n])
            m_vol.at(n) = 0u;
    }
}



int ToolsRemoveConnected::peelOnce(const std::vector<uint8_t>& inMask,
    std::vector<uint8_t>& outMask,
    const int ext[6]) const
{
    const int nx = ext[1] - ext[0] + 1, ny = ext[3] - ext[2] + 1, nz = ext[5] - ext[4] + 1;
    auto inExt = [&](int i, int j, int k)->bool {
        return (i >= ext[0] && i <= ext[1] && j >= ext[2] && j <= ext[3] && k >= ext[4] && k <= ext[5]);
        };
    static const int N6[6][3] = { {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1} };

    outMask = inMask; // копия, будем занулять границу
    int kept = 0;

    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
            for (int i = ext[0]; i <= ext[1]; ++i) {
                const size_t w = linearIdx(i, j, k, ext, nx, ny);
                if (!inMask[w]) continue;

                bool boundary = false;
                for (auto& d : N6) {
                    const int ni = i + d[0], nj = j + d[1], nk = k + d[2];
                    if (!inExt(ni, nj, nk) || !inMask[linearIdx(ni, nj, nk, ext, nx, ny)]) { boundary = true; break; }
                }
                if (boundary) outMask[w] = 0;
            }

    for (auto v : outMask) if (v) ++kept;
    return kept;
}

int ToolsRemoveConnected::keepOnlyConnectedFromSeed(std::vector<uint8_t>& mask,
    const int ext[6],
    const int seed[3]) const
{
    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    if (!inExt(seed[0], seed[1], seed[2])) {
        std::fill(mask.begin(), mask.end(), 0);
        return 0;
    }

    const size_t total = mask.size();
    if (total == 0)
        return 0;

    const size_t seedIdx = linearIdx(seed[0], seed[1], seed[2], ext, nx, ny);
    if (seedIdx >= total || mask[seedIdx] == 0) {
        // seed попал в пустую ячейку данной маски
        std::fill(mask.begin(), mask.end(), 0);
        return 0;
    }

    std::vector<uint8_t> out(total, 0);

    struct V { int i, j, k; };
    std::queue<V> q;
    q.push({ seed[0], seed[1], seed[2] });
    out[seedIdx] = 1;

    static const int N6[6][3] = {
        {+1,0,0},{-1,0,0},
        {0,+1,0},{0,-1,0},
        {0,0,+1},{0,0,-1}
    };

    int visited = 0;

    while (!q.empty()) {
        V v = q.front(); q.pop();
        ++visited;

        for (const auto& d : N6) {
            const int ni = v.i + d[0];
            const int nj = v.j + d[1];
            const int nk = v.k + d[2];
            if (!inExt(ni, nj, nk))
                continue;

            const size_t w = linearIdx(ni, nj, nk, ext, nx, ny);
            if (w >= total)
                continue;

            if (!mask[w])       // вне исходной маски
                continue;
            if (out[w])         // уже посещён
                continue;

            out[w] = 1;
            q.push({ ni, nj, nk });
        }
    }

    if (visited > 0)
        mask.swap(out);
    else
        std::fill(mask.begin(), mask.end(), 0);

    return visited;
}

int ToolsRemoveConnected::smartPeel(std::vector<uint8_t>& mask,
    const std::vector<uint8_t>& /*selMask*/,
    const int ext[6],
    const int seed[3],
    double dropFrac,
    int maxIters) const
{
    // держим connectivity к seed после каждого шага
    int prevCnt = keepOnlyConnectedFromSeed(mask, ext, seed);
    if (prevCnt <= 0) return 0;

    int did = 0;
    for (int it = 0; it < maxIters; ++it) {
        std::vector<uint8_t> peeled;
        int afterPeel = peelOnce(mask, peeled, ext);
        if (afterPeel == 0) break;

        // оставляем только связную с seed часть
        int selCnt = keepOnlyConnectedFromSeed(peeled, ext, seed);
        if (selCnt <= 0) break;

        const double ratio = (prevCnt > 0) ? double(selCnt) / double(prevCnt) : 0.0;
        ++did;
        mask.swap(peeled);
        prevCnt = selCnt;

        if (ratio < (1.0 - dropFrac)) break; // резкое падение — хватит
    }
    return did;
}

void ToolsRemoveConnected::applyKeepMask(vtkImageData* image,
    const std::vector<uint8_t>& keepMask) const
{
    if (!image) return;
    int ext[6]; image->GetExtent(ext);
    const int nx = ext[1] - ext[0] + 1, ny = ext[3] - ext[2] + 1, nz = ext[5] - ext[4] + 1;

    auto* p0 = static_cast<unsigned char*>(image->GetScalarPointer(ext[0], ext[2], ext[4]));
    vtkIdType incX, incY, incZ; image->GetIncrements(incX, incY, incZ);

    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
            for (int i = ext[0]; i <= ext[1]; ++i) {
                const size_t w = linearIdx(i, j, k, ext, nx, ny);
                if (!keepMask[w]) {
                    unsigned char* p = p0 + (i - ext[0]) * incX + (j - ext[2]) * incY + (k - ext[4]) * incZ;
                    *p = 0u;
                }
            }
}

void ToolsRemoveConnected::recoveryDilate(std::vector<uint8_t>& mask,
    const std::vector<uint8_t>& selMask,
    const int ext[6],
    int iters) const
{
    const int nx = ext[1] - ext[0] + 1, ny = ext[3] - ext[2] + 1, nz = ext[5] - ext[4] + 1;
    static const int N6[6][3] = { {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1} };
    auto inExt = [&](int i, int j, int k)->bool {
        return (i >= ext[0] && i <= ext[1] && j >= ext[2] && j <= ext[3] && k >= ext[4] && k <= ext[5]);
        };

    for (int r = 0; r < iters; ++r) {
        std::vector<size_t> toOne;
        for (int k = ext[4]; k <= ext[5]; ++k)
            for (int j = ext[2]; j <= ext[3]; ++j)
                for (int i = ext[0]; i <= ext[1]; ++i) {
                    const size_t w = linearIdx(i, j, k, ext, nx, ny);
                    if (mask[w]) continue;
                    if (!selMask[w]) continue; // расти только внутри исходной выбранной компоненты

                    for (auto& d : N6) {
                        const int ni = i + d[0], nj = j + d[1], nk = k + d[2];
                        if (!inExt(ni, nj, nk)) continue;
                        if (mask[linearIdx(ni, nj, nk, ext, nx, ny)]) { toOne.push_back(w); break; }
                    }
                }
        if (toOne.empty()) break;
        for (auto w : toOne) mask[w] = 1;
    }
}