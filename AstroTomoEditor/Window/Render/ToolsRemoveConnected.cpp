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
#include <vtkFlyingEdges3D.h>
#include <vtkImageMask.h>


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

    mHistLo = HistMin;
    mHistHi = HistMax;

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
    mVisibleLut.clear();

    if (!m_volume) return;
    auto* prop = m_volume->GetProperty();
    if (!prop) return;
    auto* pwf = prop->GetScalarOpacity(0);
    if (!pwf) return;

    // работаем по целым HU
    const int lo = HistMin;
    const int hi = HistMax;
    const int bins = hi - lo + 1;
    if (bins <= 0) return;

    mLutMin = lo;
    mLutMax = hi;
    mLutBins = bins;
    mVisibleLut.assign(bins, 0);

    for (int v = lo; v <= hi; ++v) {
        const double op = pwf->GetValue(static_cast<double>(v));
        mVisibleLut[v - lo] = (op > mVisibleEps) ? 1 : 0;
    }
}

uint8_t ToolsRemoveConnected::GetAverageVisibleValue()
{
    const double mid = 0.5 * (mHistLo + mHistHi);
    double s = std::clamp(mid, mLutMin, mLutMax);

    // если вдруг невидим — попробовать сдвинуться вверх/вниз в окне
    if (!isVisible(s)) {
        const int steps = 16;
        const double step = (mHistHi - mHistLo) / double(steps + 1);
        double x = s;
        for (int k = 0; k < steps && !isVisible(x); ++k)
            x += step;
        if (!isVisible(x)) {
            x = s;
            for (int k = 0; k < steps && !isVisible(x); ++k)
                x -= step;
        }
        s = x;
    }

    return static_cast<uint8_t>(std::clamp(std::lround(s), 0L, 255L));
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
        return true;
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
            onLeftClick(me->pos());
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
    switch (a)
    {
    case Action::RemoveUnconnected:
    case Action::RemoveSelected:
    case Action::RemoveConnected:
    case Action::SmartDeleting:
    case Action::VoxelEraser:
    case Action::VoxelRecovery:
    case Action::AddBase:
    case Action::FillEmpty:
        start(a, HoverMode::Default);
        return true;

    case Action::Minus:
    case Action::Plus:
    case Action::TotalSmoothing:
    case Action::PeelRecovery:
    case Action::SurfaceMapping:
        start(a, HoverMode::None);
        return true;

    default:
        return false;
    }
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


void ToolsRemoveConnected::start(Action a, HoverMode hm)
{

    if (!m_vtk || !m_renderer || !m_volume || !m_image) return;

    // на всякий случай скрыть старый след
    if (mHoverActor) { mHoverActor->SetVisibility(0); mHoverActor->Modified(); }
    
    m_mode = a;
    m_hm = hm;
    m_vol.clear();
    m_vol.copy(m_image);
    m_bin.clear();
    makeBinaryMask(m_image);

    switch (m_hm)
    {
    case HoverMode::None:
    {
        switch (m_mode)
        {
        case Action::Plus:
            PlusVoxels();
            break;
        case Action::Minus:
            MinusVoxels();
            break;
        case Action::TotalSmoothing:
            TotalSmoothingVolume();
            break;
        case Action::PeelRecovery:
            PeelRecoveryVolume();
            break;
        default:
            break;
        }

        if (m_vol.raw())
            m_vol.raw()->Modified();
        if (m_onImageReplaced)
            m_onImageReplaced(m_vol.raw());

        if (m_vtk && m_vtk->renderWindow())
            m_vtk->renderWindow()->Render();

        const bool wasActive = (m_state != State::Off);
        if (!wasActive) return;

        m_state = State::Off;
        m_bin.clear();
        m_vol.clear();

        if (m_onFinished) m_onFinished();
    }
    break;
    case HoverMode::Default:
        m_pts.clear();
        m_state = State::WaitingClick;

        onViewResized();
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
        break;
    }
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
        std::vector<uint8_t> mark;
        const int cnt = floodFill6(m_bin, seed, mark);
        if (cnt <= 0)
        {
            QApplication::beep();
            return;
        }

        AverageVisibleValue = GetAverageVisibleValue();

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
        case Action::SmartDeleting:
            RemoveConnectedRegions(mark, seed);
            SmartDeleting(seed);
            break;
        case Action::AddBase:
            AddBaseToBounds(mark, seed);
            break;
        case Action::FillEmpty:
            FillEmptyRegions(mark, seed);
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

static inline void ijkFromLinear(size_t idx,
    const int ext[6],
    int nx, int ny,
    int& i, int& j, int& k)
{
    const size_t planeSize = static_cast<size_t>(nx) * ny;
    const int kk = static_cast<int>(idx / planeSize);
    const size_t rem = idx % planeSize;
    const int jj = static_cast<int>(rem / nx);
    const int ii = static_cast<int>(rem % nx);

    i = ii + ext[0];
    j = jj + ext[2];
    k = kk + ext[4];
}

static size_t CountNonZero(const Volume& vol)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw()) return 0;
    const size_t total = vol.u8().size();
    size_t cnt = 0;
    for (size_t idx = 0; idx < total; ++idx)
        if (vol.at(idx) != 0u)
            ++cnt;
    return cnt;
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
                
                if (v > 240) qDebug() << "BIG V =" << int(v)
                    << "at" << i << j << k;

                if (!isVisible(v)) continue; // стираем только видимое

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
                if (!isVisible(ov)) continue; // восстанавливаем только видимое

                *pCur = ov;
            }
}

// Восстанавливает не более maxExtraVoxels вокселей вокруг оболочки shell.
// Восстанавливаем те, которые в volume == 0, а в refVolume != 0.
// Один и тот же соседний воксель не проверяем больше одного раза.
static void RecoverFromShellLimited(
    Volume& volume,
    const Volume& refVolume,
    const std::vector<size_t>& shell,
    size_t maxExtraVoxels)
{
    const auto& S = volume.u8();
    const auto& R = refVolume.u8();

    if (!S.valid || !R.valid || !volume.raw() || !refVolume.raw())
        return;

    if (S.nx != R.nx || S.ny != R.ny || S.nz != R.nz)
        return;
    for (int a = 0; a < 6; ++a)
        if (S.ext[a] != R.ext[a])
            return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    if (maxExtraVoxels == 0)
        return;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    static const int N6[6][3] = {
        {+1, 0, 0},
        {-1, 0, 0},
        {0, +1, 0},
        {0, -1, 0},
        {0, 0, +1},
        {0, 0, -1}
    };

    auto ijkFromIdx = [&](size_t idx, int& i, int& j, int& k) {
        const size_t plane = static_cast<size_t>(nx) * ny;
        const size_t relK = idx / plane;
        const size_t rem = idx % plane;
        const size_t relJ = rem / nx;
        const size_t relI = rem % nx;

        i = ext[0] + static_cast<int>(relI);
        j = ext[2] + static_cast<int>(relJ);
        k = ext[4] + static_cast<int>(relK);
        };


    size_t restored = 0;
    bool stop = false;

    // visited — этот воксель уже рассматривали как кандидата-соседа
    std::vector<uint8_t> visited(total, 0);
    // inQueue — этот воксель уже стоит в очереди фронта роста
    std::vector<uint8_t> inQueue(total, 0);

    std::queue<size_t> q;

    // стартовый фронт — сама оболочка (ненулевые воксели)
    for (size_t idx : shell)
    {
        q.push(idx);
        inQueue[idx] = 1;
    }

    while (!q.empty() && restored < maxExtraVoxels)
    {
        const size_t idxCenter = q.front();
        q.pop();

        int ci, cj, ck;
        ijkFromIdx(idxCenter, ci, cj, ck);

        for (const auto& d : N6)
        {
            const int ni = ci + d[0];
            const int nj = cj + d[1];
            const int nk = ck + d[2];
            if (!inExt(ni, nj, nk))
                continue;

            const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
            if (nIdx >= total)
                continue;

            // этот воксель-кандидат уже рассматривали — пропускаем
            if (visited[nIdx])
                continue;
            visited[nIdx] = 1;

            // интересен только фон, где в ref есть объект
            if (volume.at(nIdx) == 0u && refVolume.at(nIdx) != 0u)
            {
                volume.at(nIdx) = refVolume.at(nIdx);
                ++restored;

                // новый воксель становится частью фронта роста
                if (!inQueue[nIdx])
                {
                    q.push(nIdx);
                    inQueue[nIdx] = 1;
                }

                if (restored >= maxExtraVoxels)
                    break;
            }
        }
    }
}


// Восстанавливает countOfPeels "слоёв" вокруг текущего объекта в volume,
// используя refVolume как эталон (исходный том).
// Требование: volume и refVolume должны иметь одинаковую геометрию U8.

static void RecoveryPeel(Volume& volume,
    const Volume& refVolume,
    int countOfPeels)
{
    const auto& S = volume.u8();
    const auto& R = refVolume.u8();

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    // 6-соседство
    static const int N6[6][3] = {
        {+1, 0, 0},
        {-1, 0, 0},
        {0, +1, 0},
        {0, -1, 0},
        {0, 0, +1},
        {0, 0, -1}
    };

    // Делаем несколько итераций "наращивания"
    for (int iter = 0; iter < countOfPeels; ++iter)
    {
        std::vector<uint8_t> toRestore(total, 0);

        // 1. Находим, какие воксели надо вернуть на этом шаге
        for (int k = ext[4]; k <= ext[5]; ++k)
        {
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                for (int i = ext[0]; i <= ext[1]; ++i)
                {
                    const size_t idx = linearIdx(i, j, k, ext, nx, ny);

                    // Берём только текущий объект: от нулевых вокселей смысла смотреть соседей нет
                    if (volume.at(idx) == 0u)
                        continue;

                    // Смотрим соседей этого ненулевого вокселя
                    for (const auto& d : N6)
                    {
                        const int ni = i + d[0];
                        const int nj = j + d[1];
                        const int nk = k + d[2];
                        if (!inExt(ni, nj, nk))
                            continue;

                        const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);

                        // Если в текущем томе там 0, а в эталонном - не 0, помечаем к восстановлению
                        if (volume.at(nIdx) == 0u && refVolume.at(nIdx) != 0u)
                        {
                            toRestore[nIdx] = 1;
                        }
                    }
                }
            }
        }

        // 2. Применяем восстановление: добавляем один слой
        bool anyRestored = false;
        for (size_t n = 0; n < total; ++n)
        {
            if (toRestore[n])
            {
                volume.at(n) = refVolume.at(n);
                anyRestored = true;
            }
        }

        // Если на этой итерации ничего не восстановили - дальше расти нечему
        if (!anyRestored)
            return;
    }
}

static void FilterShellWithGrowableNeighbor(
    const Volume& volume,
    const Volume& refVolume,
    std::vector<size_t>& shell)
{
    const auto& S = volume.u8();
    const auto& R = refVolume.u8();

    if (!S.valid || !R.valid || !volume.raw() || !refVolume.raw())
        return;

    // проверяем совместимость геометрии
    if (S.nx != R.nx || S.ny != R.ny || S.nz != R.nz)
        return;
    for (int a = 0; a < 6; ++a)
        if (S.ext[a] != R.ext[a])
            return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    static const int N6[6][3] = {
        {+1, 0, 0},
        {-1, 0, 0},
        {0, +1, 0},
        {0, -1, 0},
        {0, 0, +1},
        {0, 0, -1}
    };

    auto ijkFromIdx = [&](size_t idx, int& i, int& j, int& k) {
        const size_t plane = static_cast<size_t>(nx) * ny;
        const size_t relK = idx / plane;
        const size_t rem = idx % plane;
        const size_t relJ = rem / nx;
        const size_t relI = rem % nx;

        i = ext[0] + static_cast<int>(relI);
        j = ext[2] + static_cast<int>(relJ);
        k = ext[4] + static_cast<int>(relK);
        };

    std::vector<size_t> newShell;
    newShell.reserve(shell.size());

    for (size_t idx : shell)
    {
        if (idx >= total)
            continue;

        // если сам воксель уже нулевой в volume – нам он не нужен
        if (volume.at(idx) == 0u)
            continue;

        int i, j, k;
        ijkFromIdx(idx, i, j, k);

        bool hasGrowableNeighbor = false;

        for (const auto& d : N6)
        {
            const int ni = i + d[0];
            const int nj = j + d[1];
            const int nk = k + d[2];
            if (!inExt(ni, nj, nk))
                continue;

            const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
            if (nIdx >= total)
                continue;

            // сосед сейчас пустой, а в эталоне есть объект
            if (volume.at(nIdx) == 0u && refVolume.at(nIdx) != 0u)
            {
                hasGrowableNeighbor = true;
                break;
            }
        }

        if (hasGrowableNeighbor)
            newShell.push_back(idx);
    }

    shell.swap(newShell);
}


void ToolsRemoveConnected::RemoveConnectedRegions(const std::vector<uint8_t>& mark,
    const int seedIn[3])
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();
    if (mark.size() < total) return;

    for (size_t n = 0; n < total; ++n)
    {
        if (!mark[n])
            m_vol.at(n) = 0u;
    }

    Volume volNew;
    volNew.copy(m_vol.raw());

    std::vector<size_t> shell;
    CollectShellVoxels(volNew, shell);

    std::vector<uint8_t> isShell(total, 0);
    for (size_t w : shell)
        if (w < total)
            isShell[w] = 1;

    for (size_t n = 0; n < total; ++n)
        if (isShell[n])
            volNew.at(n) = 0u;


    int newSeed[3] = { seedIn[0], seedIn[1], seedIn[2] };
    if (!findNearestNonEmptyConnectedVoxel(volNew.raw(), seedIn, newSeed))
        return;

    std::vector<uint8_t> newmark;
    const int cnt = floodFill6(volNew, newSeed, newmark);
    if (cnt <= 0)
        return;

    for (size_t n = 0; n < total; ++n)
        if (!newmark[n])
            volNew.at(n) = 0u;




    RecoveryPeel(volNew, m_vol, 1);

    m_vol = volNew;
}



void ToolsRemoveConnected::SmartDeleting(const int seedIn[3])
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();

    Volume volNew;
    volNew.copy(m_vol.raw());

    const double ClearPersent = 0.20;
    double DistMm = ClearingVolume(volNew, seedIn, ClearPersent);

    size_t numofnonzerovox = CountNonZero(volNew);

    std::vector<size_t> shell;
    CollectShellVoxels(volNew, shell);

    FilterShellWithGrowableNeighbor(volNew, m_vol, shell);
    
    const size_t maxExtra = numofnonzerovox * ( 0.5 - ClearPersent);
    RecoverFromShellLimited(volNew, m_vol, shell, maxExtra);
    m_vol = volNew;
}

// ---- ядро ----
void ToolsRemoveConnected::makeBinaryMask(vtkImageData* image)
{
    m_bin.set(image, [&](uint8_t v) -> uint8_t
        {
            if (v <= 0u)
                return 0u;

            if (mVisibleLut.empty() || mLutBins < 255 || v >= 255u)
                return 1u;

            return mVisibleLut[v];
        });
}


double ToolsRemoveConnected::ClearingVolume(Volume& vol,
    const int seedIn[3],
    double percent)
{
    // подстрахуемся, если кто-то передал ерунду
    if (percent <= 0.0) percent = 0.25;
    if (percent > 1.0)  percent = 1.0;

    const auto& S = vol.u8();
    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    const size_t seedIdx = linearIdx(seedIn[0], seedIn[1], seedIn[2], ext, nx, ny);

    // --- 2) считаем физический радиус в мм по percent от размера тома ---

    double sp[3]{ 1.0, 1.0, 1.0 };
    if (m_image)
        m_image->GetSpacing(sp);
    else if (vol.raw())
        vol.raw()->GetSpacing(sp);

    const double minSp = std::min({ sp[0], sp[1], sp[2] });

    auto distMM = [&](double i, double j, double k) {
        return std::sqrt(i * i + j * j + k * k);
        };

    // физические размеры по осям в мм
    const double sizeXmm = sp[0] * (ext[1] - ext[0] + 1);
    const double sizeYmm = sp[1] * (ext[3] - ext[2] + 1);
    const double sizeZmm = sp[2] * (ext[5] - ext[4] + 1);
    const double minDimMm = distMM(sizeXmm, sizeYmm, sizeZmm);

    // радиус = percent * минимальная физическая длина
    const double radiusMM = percent * minDimMm;

    // шаги BFS по самому мелкому spacing
    const int maxSteps =
        std::max(1, int(radiusMM / std::max(minSp, 1e-6) + 0.5));

    // --- 3) BFS от seed по НЕнулевым вокселям vol, ограниченный maxSteps ---

    std::vector<uint8_t> core(total, 0);
    std::vector<uint8_t> visited(total, 0);

    struct Node { int i, j, k, d; };
    std::queue<Node> q;

    q.push({ seedIn[0], seedIn[1], seedIn[2], 0 });
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

            if (vol.at(w) == 0u)
                continue;   // фон
            if (visited[w])
                continue;

            visited[w] = 1;
            core[w] = 1;
            q.push({ ni, nj, nk, v.d + 1 });
        }
    }

    // --- 4) Очищаем всё, что не попало в ядро core ---

    for (size_t n = 0; n < total; ++n)
    {
        if (vol.at(n) != 0u && !core[n])
            vol.at(n) = 0u;
    }

    return radiusMM;
}



void ToolsRemoveConnected::CollectShellVoxels(const Volume& vol,
    std::vector<size_t>& shell) const
{
    shell.clear();

    const auto& S = vol.u8();
    if (!S.valid || !S.p0)
        return;

    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;

    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    static const int N6[6][3] = {
        {+1, 0, 0}, {-1, 0, 0},
        {0, +1, 0}, {0, -1, 0},
        {0, 0, +1}, {0, 0, -1}
    };

    const size_t total = S.size();
    shell.reserve(total / 10);    // чисто чтобы меньше реаллокаций было

    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                const size_t w = S.idxRel(i, j, k);
                if (vol.at(w) == 0u)
                    continue; // фон, не интересует

                bool boundary = false;
                for (const auto& d : N6)
                {
                    const int ni = i + d[0];
                    const int nj = j + d[1];
                    const int nk = k + d[2];

                    // сосед вне объёма -> граница
                    if (ni < 0 || ni >= nx ||
                        nj < 0 || nj >= ny ||
                        nk < 0 || nk >= nz)
                    {
                        boundary = true;
                        break;
                    }

                    const size_t wn = S.idxRel(ni, nj, nk);
                    if (vol.at(wn) == 0u) {
                        boundary = true;
                        break;
                    }
                }

                if (boundary)
                    shell.push_back(w);
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

// Возвращает список индексов вокселей (linearIdx),
// у которых среди N соседей (6 / 18 / 26) не менее frac * N нулевых.
// Центральный воксель считается только если сам != 0.
static void CollectVoxelsWithZeroNeighbors(
    const Volume& volume,
    int neighborhood,          // 6, 18 или 26
    double fracZero,           // например 1.0 / 6.0
    std::vector<size_t>& out)
{
    out.clear();

    const auto& S = volume.u8();
    if (!S.valid || !S.p0 || !volume.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    // доступ к значению вокселя (0/..255)
    const unsigned char* base = reinterpret_cast<const unsigned char*>(S.p0);
    const vtkIdType incx = S.incX;
    const vtkIdType incy = S.incY;
    const vtkIdType incz = S.incZ;

    auto at = [&](int i, int j, int k) -> unsigned char {
        const unsigned char* p = base
            + (i - ext[0]) * incx
            + (j - ext[2]) * incy
            + (k - ext[4]) * incz;
        return *p;
        };

    // задаём шаблоны соседей
    static const int N6[][3] = {
        {+1,0,0},{-1,0,0},
        {0,+1,0},{0,-1,0},
        {0,0,+1},{0,0,-1}
    };

    // faces + corners (6 + 12 = 18)
    static const int N18[][3] = {
        // 6 граней
        {+1,0,0},{-1,0,0},
        {0,+1,0},{0,-1,0},
        {0,0,+1},{0,0,-1},
        // 12 рёбер
        {+1,+1,0},{+1,-1,0},{-1,+1,0},{-1,-1,0},
        {+1,0,+1},{+1,0,-1},{-1,0,+1},{-1,0,-1},
        {0,+1,+1},{0,+1,-1},{0,-1,+1},{0,-1,-1},
    };

    // все 26 соседей (faces + edges + corners)
    static const int N26[][3] = {
        // 6 граней
        {+1,0,0},{-1,0,0},
        {0,+1,0},{0,-1,0},
        {0,0,+1},{0,0,-1},
        // 12 рёбер
        {+1,+1,0},{+1,-1,0},{-1,+1,0},{-1,-1,0},
        {+1,0,+1},{+1,0,-1},{-1,0,+1},{-1,0,-1},
        {0,+1,+1},{0,+1,-1},{0,-1,+1},{0,-1,-1},
        // 8 углов
        {+1,+1,+1},{+1,+1,-1},{+1,-1,+1},{+1,-1,-1},
        {-1,+1,+1},{-1,+1,-1},{-1,-1,+1},{-1,-1,-1}
    };

    const int (*offs)[3] = nullptr;
    int nCount = 0;

    switch (neighborhood) {
    case 6:
        offs = N6;
        nCount = static_cast<int>(std::size(N6));
        break;
    case 18:
        offs = N18;
        nCount = static_cast<int>(std::size(N18));
        break;
    case 26:
        offs = N26;
        nCount = static_cast<int>(std::size(N26));
        break;
    default:
        return; // некорректный режим
    }

    // минимум нулевых соседей (округляем вверх)
    const int minZero = std::max(1, int(std::ceil(fracZero * nCount)));

    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const unsigned char v = at(i, j, k);
                if (v == 0)
                    continue; // интересуют только непустые воксели

                int zeroNeigh = 0;

                for (int t = 0; t < nCount; ++t) {
                    const int ni = i + offs[t][0];
                    const int nj = j + offs[t][1];
                    const int nk = k + offs[t][2];

                    // за пределами тома считаем как "ноль" — это тоже граница
                    if (!inExt(ni, nj, nk)) {
                        ++zeroNeigh;
                    }
                    else {
                        if (at(ni, nj, nk) == 0)
                            ++zeroNeigh;
                    }

                    // можно ранний выход
                    if (zeroNeigh >= minZero)
                        break;
                }

                if (zeroNeigh >= minZero) {
                    const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                    if (idx < total)
                        out.push_back(idx);
                }
            }
}

// Обнуляет границы объёма и затем делает 6-соседнюю эрозию.
void ToolsRemoveConnected::ErodeBy6Neighbors(Volume& volume)
{
    const auto& S = volume.u8();
    if (!S.valid || !volume.raw())
        return;

    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;

    const size_t total = size_t(nx) * ny * nz;
    const size_t slice = size_t(nx) * ny;

    if (total == 0)
        return;

    // слой Z = 0 и Z = nz-1
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
        {
            volume.at(size_t(j) * nx + i) = 0u;
            volume.at(slice * (nz - 1) + size_t(j) * nx + i) = 0u;
        }

    // слой Y = 0 и Y = ny-1
    for (int k = 0; k < nz; k++)
        for (int i = 0; i < nx; i++)
        {
            volume.at(size_t(k) * slice + i) = 0u;
            volume.at(size_t(k) * slice + size_t(ny - 1) * nx + i) = 0u;
        }

    // слой X = 0 и X = nx-1
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
        {
            volume.at(size_t(k) * slice + size_t(j) * nx + 0) = 0u;
            volume.at(size_t(k) * slice + size_t(j) * nx + nx - 1) = 0u;
        }

    std::vector<uint8_t> toZero(total, 0);

    for (size_t idx = 0; idx < total; idx++)
    {
        if (volume.at(idx) == 0u)
            continue;

        // 6 индексов
        const size_t n0 = idx - 1;
        const size_t n1 = idx + 1;
        const size_t n2 = idx - nx;
        const size_t n3 = idx + nx;
        const size_t n4 = idx - slice;
        const size_t n5 = idx + slice;

        if (volume.at(n0) == 0u ||
            volume.at(n1) == 0u ||
            volume.at(n2) == 0u ||
            volume.at(n3) == 0u ||
            volume.at(n4) == 0u ||
            volume.at(n5) == 0u)
            toZero[idx] = 1;
    }

    // === 3. Второй проход — обнуляем ===
    for (size_t idx = 0; idx < total; idx++)
        if (toZero[idx])
            volume.at(idx) = 0u;
}

bool ToolsRemoveConnected::isVisible(const short v) const
{
    if (mVisibleLut.empty()) return true;

    const int idx = int(v) - int(mLutMin);
    if (idx < 0 || idx >= (int)mVisibleLut.size())
        return false;

    return mVisibleLut[size_t(idx)] != 0;
}

void ToolsRemoveConnected::RecoveryNonVisibleVoxels(Volume& volume)
{
    Volume refvol;
    refvol.clear();
    refvol.copy(m_image);

    const auto& S = volume.u8();
    if (!S.valid || !volume.raw())
        return;

    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;

    const size_t slice = size_t(nx) * ny;
    const size_t total = slice * nz;
    if (total == 0)
        return;

    // 1. Обнуляем границы (X/Y/Z)
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
        {
            volume.at(size_t(j) * nx + i) = 0u;  // Z = 0
            volume.at(slice * (nz - 1) + size_t(j) * nx + i) = 0u; // Z = nz-1
        }

    for (int k = 0; k < nz; ++k)
        for (int i = 0; i < nx; ++i)
        {
            volume.at(size_t(k) * slice + i) = 0u; // Y = 0
            volume.at(size_t(k) * slice + size_t(ny - 1) * nx + i) = 0u; // Y = ny-1
        }

    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
        {
            volume.at(size_t(k) * slice + size_t(j) * nx + 0) = 0u;      // X = 0
            volume.at(size_t(k) * slice + size_t(j) * nx + nx - 1) = 0u;      // X = nx-1
        }

    // 2. Отмечаем НУЛИ, которые рядом с ненулевыми (6-соседство)
    std::vector<uint8_t> toFill(total, 0);

    for (int k = 1; k < nz - 1; ++k)
    {
        const size_t zOff = size_t(k) * slice;
        for (int j = 1; j < ny - 1; ++j)
        {
            const size_t yOff = zOff + size_t(j) * nx;
            for (int i = 1; i < nx - 1; ++i)
            {
                const size_t idx = yOff + size_t(i);

                if (volume.at(idx) != 0u)
                    continue; // нас интересуют только нули

                const size_t n0 = idx - 1;
                const size_t n1 = idx + 1;
                const size_t n2 = idx - nx;
                const size_t n3 = idx + nx;
                const size_t n4 = idx - slice;
                const size_t n5 = idx + slice;
                const size_t n6 = idx - nx - 1;
                const size_t n7 = idx - nx + 1;
                const size_t n8 = idx + nx - 1;
                const size_t n9 = idx + nx + 1;

                const size_t n10 = idx - slice - 1;
                const size_t n11 = idx - slice + 1;
                const size_t n12 = idx + slice - 1;
                const size_t n13 = idx + slice + 1;

                const size_t n14 = idx - slice - nx;
                const size_t n15 = idx - slice + nx;
                const size_t n16 = idx + slice - nx;
                const size_t n17 = idx + slice + nx;

                if (volume.at(n0) ||
                    volume.at(n1) ||
                    volume.at(n2) ||
                    volume.at(n3) ||
                    volume.at(n4) ||
                    volume.at(n5) ||
                    volume.at(n6) ||
                    volume.at(n7) ||
                    volume.at(n8) ||
                    volume.at(n9) ||
                    volume.at(n10) ||
                    volume.at(n11) ||
                    volume.at(n12) ||
                    volume.at(n13) ||
                    volume.at(n14) ||
                    volume.at(n15) ||
                    volume.at(n16) ||
                    volume.at(n17))
                {
                    toFill[idx] = 1;
                }
            }
        }
    }

    for (size_t idx = 0; idx < total; ++idx)
        if (toFill[idx] && !volume.at(idx) && refvol.at(idx))
            volume.at(idx) = refvol.at(idx);

    for (size_t i = 0; i < total; i++)
        if (!volume.at(i))
            refvol.at(i) = 0;

    volume = refvol;
}

bool ToolsRemoveConnected::AddBy6Neighbors(Volume& volume, uint8_t fillVal)
{
    const auto& S = volume.u8();
    if (!S.valid || !volume.raw())
        return false;

    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;

    const size_t slice = size_t(nx) * ny;
    const size_t total = slice * nz;
    if (total == 0)
        return false;

    // 2. Отмечаем НУЛИ, которые рядом с ненулевыми (6-соседство)
    std::vector<uint8_t> toFill(total, 0);

    for (int k = 1; k < nz - 1; ++k)
    {
        const size_t zOff = size_t(k) * slice;
        for (int j = 1; j < ny - 1; ++j)
        {
            const size_t yOff = zOff + size_t(j) * nx;
            for (int i = 1; i < nx - 1; ++i)
            {
                const size_t idx = yOff + size_t(i);

                if (!volume.at(idx))
                    continue;

                const size_t n0 = idx - 1;
                const size_t n1 = idx + 1;
                const size_t n2 = idx - nx;
                const size_t n3 = idx + nx;
                const size_t n4 = idx - slice;
                const size_t n5 = idx + slice;

                toFill[n0] = 1;
                toFill[n1] = 1;
                toFill[n2] = 1;
                toFill[n3] = 1;
                toFill[n4] = 1;
                toFill[n5] = 1;
            }
        }
    }


    for (size_t idx = 0; idx < total; ++idx)
        if (toFill[idx] == 1 && !volume.at(idx))
            volume.at(idx) = fillVal;

    return true;
}

void ToolsRemoveConnected::MinusVoxels()
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;
    if (CountNonZero(m_bin) == 0) return; // нет видимых вокселей — ничего не делать

    const size_t total = m_vol.u8().size();

    Volume volNew;
    volNew.copy(m_vol.raw());

    for (size_t n = 0; n < total; ++n)
        if (!m_bin.at(n))
            volNew.at(n) = 0;

    ErodeBy6Neighbors(volNew);

    m_vol = volNew;
}

void ToolsRemoveConnected::PlusVoxels()
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;
    if (CountNonZero(m_bin) == 0) return;

    const size_t total = m_vol.u8().size();

    // расширяем МАСКУ строго единицами
    if (!AddBy6Neighbors(m_bin, 1u))
        return;

    const uint8_t fillVal = GetAverageVisibleValue();

    for (size_t n = 0; n < total; ++n)
        if (m_bin.at(n) && !m_vol.at(n))
            m_vol.at(n) = fillVal;
}

void ToolsRemoveConnected::AddBaseLeftX(Volume& vol, uint8_t shift, uint8_t fillVal)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    // 1. Ищем первый по X слой слева, где есть ненулевой воксель
    int firstObjI = -1;
    for (int i = ext[0]; i <= ext[1] && firstObjI < 0; ++i)
        for (int k = ext[4]; k <= ext[5] && firstObjI < 0; ++k)
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                {
                    firstObjI = i;
                    break;
                }
            }

    // ничего не нашли или нет "внутреннего" слоя справа
    if (firstObjI < 0 || firstObjI >= ext[1])
        return;

    // 2. Заполняем весь найденный X-слой значением fillVal
    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            const size_t idxFill = linearIdx(firstObjI, j, k, ext, nx, ny);
            vol.at(idxFill) = fillVal;
        }

    // 3. Проверка: есть ли "опора" по X внутрь (в сторону больших i)
    auto hasSupportInside = [&](int j, int k) -> bool
        {
            const int iMax = std::min(ext[1], firstObjI + static_cast<int>(shift));
            for (int i = firstObjI + 1; i <= iMax; ++i)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                    return true;
            }
            return false;
        };

    // 4. Срезаем свесы без опоры: сначала с "верхне-заднего" угла (max Z, max Y)
    for (int k = ext[5]; k >= ext[4]; --k)
        for (int j = ext[3]; j >= ext[2]; --j)
        {
            if (!hasSupportInside(j, k))
            {
                const size_t idxFill = linearIdx(firstObjI, j, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    // потом с противоположного края (min Z, min Y)
    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            if (!hasSupportInside(j, k))
            {
                const size_t idxFill = linearIdx(firstObjI, j, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    // 5. Копируем основание на слой "внутрь" (по X вправо)
    const int iInside = firstObjI + 1;
    if (iInside <= ext[1])
        for (int k = ext[4]; k <= ext[5]; ++k)
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                const size_t idxSide = linearIdx(firstObjI, j, k, ext, nx, ny);
                const size_t idxInside = linearIdx(iInside, j, k, ext, nx, ny);

                if (vol.at(idxSide) != 0u)
                    vol.at(idxInside) = vol.at(idxSide);
            }
}

void ToolsRemoveConnected::AddBaseRightX(Volume& vol, uint8_t shift, uint8_t fillVal)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    // 1. Ищем первый по X слой справа, где есть ненулевой воксель
    int firstObjI = -1;
    for (int i = ext[1]; i >= ext[0] && firstObjI < 0; --i)
        for (int k = ext[4]; k <= ext[5] && firstObjI < 0; ++k)
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                {
                    firstObjI = i;
                    break;
                }
            }

    // ничего не нашли или нет "внутреннего" слоя слева
    if (firstObjI < 0 || firstObjI <= ext[0])
        return;

    // 2. Заполняем весь найденный X-слой значением fillVal
    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            const size_t idxFill = linearIdx(firstObjI, j, k, ext, nx, ny);
            vol.at(idxFill) = fillVal;
        }

    // 3. Проверка: есть ли "опора" по X внутрь (в сторону меньших i)
    auto hasSupportInside = [&](int j, int k) -> bool
        {
            const int iMin = std::max(ext[0], firstObjI - static_cast<int>(shift));
            for (int i = firstObjI - 1; i >= iMin; --i)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                    return true;
            }
            return false;
        };

    // 4. Срезаем свесы без опоры — два прохода по плоскости YZ
    for (int k = ext[5]; k >= ext[4]; --k)
        for (int j = ext[3]; j >= ext[2]; --j)
        {
            if (!hasSupportInside(j, k))
            {
                const size_t idxFill = linearIdx(firstObjI, j, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            if (!hasSupportInside(j, k))
            {
                const size_t idxFill = linearIdx(firstObjI, j, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    // 5. Копируем основание на слой "внутрь" (по X влево)
    const int iInside = firstObjI - 1;
    if (iInside >= ext[0])
        for (int k = ext[4]; k <= ext[5]; ++k)
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                const size_t idxSide = linearIdx(firstObjI, j, k, ext, nx, ny);
                const size_t idxInside = linearIdx(iInside, j, k, ext, nx, ny);

                if (vol.at(idxSide) != 0u)
                    vol.at(idxInside) = vol.at(idxSide);
            }
}

void ToolsRemoveConnected::AddBaseTopZ(Volume& vol, uint8_t shift, uint8_t fillVal)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    // 1. Ищем первый по Z слой, где есть ненулевой воксель
    int firstObjK = -1;
    for (int k = ext[5]; k >= ext[4] && firstObjK < 0; --k)
        for (int i = ext[0]; i <= ext[1] && firstObjK < 0; ++i)
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                {
                    firstObjK = k;
                    break;
                }
            }

    // ничего не нашли или слой упёрся в нижнюю границу
    if (firstObjK < 0 || firstObjK <= ext[4])
        return;

    // 2. Заполняем весь найденный слой значением fillVal
    for (int j = ext[2]; j <= ext[3]; ++j)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            const size_t idxFill = linearIdx(i, j, firstObjK, ext, nx, ny);
            vol.at(idxFill) = fillVal;
        }

    // Вспомогательная функция: есть ли "опора" под точкой (i, j)
    auto hasSupportBelow = [&](int i, int j) -> bool
        {
            const int kMin = std::max(ext[4], firstObjK - shift);
            for (int k = firstObjK - 1; k >= kMin; --k)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                    return true;
            }
            return false;
        };

    // 3. Убираем те заполняемые воксели слоя firstObjK,
    //    под которыми в исходном томе нет ничего в пределах shift
    for (int j = ext[3]; j >= ext[2]; --j)
        for (int i = ext[1]; i >= ext[0]; --i)
        {
            if (!hasSupportBelow(i, j))
            {
                const size_t idxFill = linearIdx(i, j, firstObjK, ext, nx, ny);
                vol.at(idxFill) = 0;
            }
            else
                break;
        }

    for (int j = ext[2]; j <= ext[3]; ++j)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            if (!hasSupportBelow(i, j))
            {
                const size_t idxFill = linearIdx(i, j, firstObjK, ext, nx, ny);
                vol.at(idxFill) = 0;
            }
            else
                break;
        }

    // 4. Копируем основание на слой ниже (если он существует)
    const int kBelow = firstObjK + 1;
    if (kBelow <= ext[5])
        for (int j = ext[2]; j <= ext[3]; ++j)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idxTop = linearIdx(i, j, firstObjK, ext, nx, ny);
                const size_t idxBelow = linearIdx(i, j, kBelow, ext, nx, ny);

                if (vol.at(idxTop) != 0u)
                    vol.at(idxBelow) = vol.at(idxTop);
            }
}

void ToolsRemoveConnected::AddBaseBottomZ(Volume& vol, uint8_t shift, uint8_t fillVal)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    auto inExt = [&](int i, int j, int k) -> bool {
        return (i >= ext[0] && i <= ext[1] &&
            j >= ext[2] && j <= ext[3] &&
            k >= ext[4] && k <= ext[5]);
        };

    // 1. Ищем первый по Z слой, где есть ненулевой воксель
    int firstObjK = -1;
    for (int k = ext[4]; k <= ext[5] && firstObjK < 0; ++k)
        for (int i = ext[0]; i <= ext[1] && firstObjK < 0; ++i)
            for (int j = ext[2]; j <= ext[3]; ++j)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                {
                    firstObjK = k;
                    break;
                }
            }
    
    // ничего не нашли или слой упёрся в нижнюю границу
    if (firstObjK < 0 || firstObjK <= ext[4])
        return;
    
    // 2. Заполняем весь найденный слой значением fillVal
    for (int j = ext[2]; j <= ext[3]; ++j)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            const size_t idxFill = linearIdx(i, j, firstObjK, ext, nx, ny);
            vol.at(idxFill) = fillVal;
        }
    
    // Вспомогательная функция: есть ли "опора" под точкой (i, j)
    auto hasSupportBelow = [&](int i, int j) -> bool
        {
            const int kMax = std::min(ext[5], firstObjK + shift);
            for (int k = firstObjK + 1; k <= kMax; ++k)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                    return true;
            }
            return false;
        };
    
    // 3. Убираем те заполняемые воксели слоя firstObjK,
    //    под которыми в исходном томе нет ничего в пределах shift
    for (int j = ext[3]; j >= ext[2]; --j)
        for (int i = ext[1]; i >= ext[0]; --i) 
        { 
            if (!hasSupportBelow(i, j))
            {
                const size_t idxFill = linearIdx(i, j, firstObjK, ext, nx, ny);
                vol.at(idxFill) = 0;
            }
            else
                break;
        }

    for (int j = ext[2]; j <= ext[3]; ++j) 
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            if (!hasSupportBelow(i, j))
            {
                const size_t idxFill = linearIdx(i, j, firstObjK, ext, nx, ny);
                vol.at(idxFill) = 0;
            }
            else
                break;
        }
    
    // 4. Копируем основание на слой ниже (если он существует)
    const int kBelow = firstObjK - 1;
    if (kBelow >= ext[4])
        for (int j = ext[2]; j <= ext[3]; ++j)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idxTop = linearIdx(i, j, firstObjK, ext, nx, ny);
                const size_t idxBelow = linearIdx(i, j, kBelow, ext, nx, ny);
    
                if (vol.at(idxTop) != 0u)
                    vol.at(idxBelow) = vol.at(idxTop);
            }
}

void ToolsRemoveConnected::AddBaseFrontY(Volume& vol, uint8_t shift, uint8_t fillVal)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    // 1. Ищем первый по Y слой спереди (минимальный j), где есть ненулевой воксель
    int firstObjJ = -1;
    for (int j = ext[2]; j <= ext[3] && firstObjJ < 0; ++j)
        for (int k = ext[4]; k <= ext[5] && firstObjJ < 0; ++k)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                {
                    firstObjJ = j;
                    break;
                }
            }

    if (firstObjJ < 0 || firstObjJ >= ext[3])
        return;

    // 2. Заполняем найденный Y-слой
    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            const size_t idxFill = linearIdx(i, firstObjJ, k, ext, nx, ny);
            vol.at(idxFill) = fillVal;
        }

    // 3. Опора внутрь по Y (к большим j)
    auto hasSupportInside = [&](int i, int k) -> bool
        {
            const int jMax = std::min(ext[3], firstObjJ + static_cast<int>(shift));
            for (int j = firstObjJ + 1; j <= jMax; ++j)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                    return true;
            }
            return false;
        };

    // 4. Срезаем свесы по плоскости XZ
    for (int k = ext[5]; k >= ext[4]; --k)
        for (int i = ext[1]; i >= ext[0]; --i)
        {
            if (!hasSupportInside(i, k))
            {
                const size_t idxFill = linearIdx(i, firstObjJ, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            if (!hasSupportInside(i, k))
            {
                const size_t idxFill = linearIdx(i, firstObjJ, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    // 5. Копируем основание внутрь (к большим j)
    const int jInside = firstObjJ + 1;
    if (jInside <= ext[3])
        for (int k = ext[4]; k <= ext[5]; ++k)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idxSide = linearIdx(i, firstObjJ, k, ext, nx, ny);
                const size_t idxInside = linearIdx(i, jInside, k, ext, nx, ny);

                if (vol.at(idxSide) != 0u)
                    vol.at(idxInside) = vol.at(idxSide);
            }
}

void ToolsRemoveConnected::AddBaseBackY(Volume& vol, uint8_t shift, uint8_t fillVal)
{
    const auto& S = vol.u8();
    if (!S.valid || !vol.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    // 1. Ищем первый по Y слой сзади (максимальный j), где есть ненулевой воксель
    int firstObjJ = -1;
    for (int j = ext[3]; j >= ext[2] && firstObjJ < 0; --j)
        for (int k = ext[4]; k <= ext[5] && firstObjJ < 0; ++k)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                {
                    firstObjJ = j;
                    break;
                }
            }

    if (firstObjJ < 0 || firstObjJ <= ext[2])
        return;

    // 2. Заполняем найденный Y-слой
    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            const size_t idxFill = linearIdx(i, firstObjJ, k, ext, nx, ny);
            vol.at(idxFill) = fillVal;
        }

    // 3. Опора внутрь по Y (к меньшим j)
    auto hasSupportInside = [&](int i, int k) -> bool
        {
            const int jMin = std::max(ext[2], firstObjJ - static_cast<int>(shift));
            for (int j = firstObjJ - 1; j >= jMin; --j)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (vol.at(idx) != 0u)
                    return true;
            }
            return false;
        };

    // 4. Срезаем свесы по плоскости XZ
    for (int k = ext[5]; k >= ext[4]; --k)
        for (int i = ext[1]; i >= ext[0]; --i)
        {
            if (!hasSupportInside(i, k))
            {
                const size_t idxFill = linearIdx(i, firstObjJ, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    for (int k = ext[4]; k <= ext[5]; ++k)
        for (int i = ext[0]; i <= ext[1]; ++i)
        {
            if (!hasSupportInside(i, k))
            {
                const size_t idxFill = linearIdx(i, firstObjJ, k, ext, nx, ny);
                vol.at(idxFill) = 0u;
            }
            else
                break;
        }

    // 5. Копируем основание внутрь (к меньшим j)
    const int jInside = firstObjJ - 1;
    if (jInside >= ext[2])
        for (int k = ext[4]; k <= ext[5]; ++k)
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idxSide = linearIdx(i, firstObjJ, k, ext, nx, ny);
                const size_t idxInside = linearIdx(i, jInside, k, ext, nx, ny);

                if (vol.at(idxSide) != 0u)
                    vol.at(idxInside) = vol.at(idxSide);
            }
}

void ToolsRemoveConnected::AddBaseToBounds(const std::vector<uint8_t>& mark, const int seedIn[3])
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();
    if (mark.size() < total) return; // подстраховка

    for (size_t n = 0; n < total; ++n)
    {
        if (!mark[n])               // если маска 0 → обнуляем воксель
            m_vol.at(n) = 0u;
    }

    Volume volNew;
    volNew.copy(m_vol.raw());

    const auto& S = volNew.u8();
    if (!S.valid || !volNew.raw())
        return;

    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    const size_t slice = size_t(nx) * ny;

    const int si = seedIn[0];
    const int sj = seedIn[1];
    const int sk = seedIn[2];

    const int dXMin = (si - ext[0]);
    const int dXMax = (ext[1] - si);
    const int dYMin = (sj - ext[2]);
    const int dYMax = (ext[3] - sj);
    const int dZMin = (sk - ext[4]);
    const int dZMax = (ext[5] - sk);

    int dist[6] = { dXMin, dXMax, dYMin, dYMax, dZMin, dZMax };

    int nearestFace = 0;
    for (int idx = 1; idx < 6; ++idx)
        if (dist[idx] < dist[nearestFace])
            nearestFace = idx;

    const uint8_t fillVal = GetAverageVisibleValue();
    constexpr int baseMargin = 3;   // тот самый N

    switch (nearestFace)
    {
    case 0:
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                volNew.at(size_t(k) * slice + size_t(j) * nx + 0) = 0u;      // X = 0
        AddBaseLeftX(volNew, baseMargin, fillVal);
        break;
    case 1:
        for (int k = 0; k < nz; ++k)
            for (int j = 0; j < ny; ++j)
                volNew.at(size_t(k) * slice + size_t(j) * nx + nx - 1) = 0u;      // X = nx-1
        AddBaseRightX(volNew, baseMargin, fillVal);
        break;
    case 2:
        for (int k = 0; k < nz; ++k)
            for (int i = 0; i < nx; ++i)
                volNew.at(size_t(k) * slice + i) = 0u;
        AddBaseFrontY(volNew, baseMargin, fillVal);
        break;
    case 3:
        for (int k = 0; k < nz; ++k)
            for (int i = 0; i < nx; ++i)
                volNew.at(size_t(k) * slice + size_t(ny - 1) * nx + i) = 0u;
        AddBaseBackY(volNew, baseMargin, fillVal);
        break;
    case 4:
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                volNew.at(size_t(j) * nx + i) = 0u;  // Z = 0
        AddBaseBottomZ(volNew, baseMargin, fillVal);
        break;
    case 5:
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                volNew.at(slice * (nz - 1) + size_t(j) * nx + i) = 0u; // Z = nz-1
        AddBaseTopZ(volNew, baseMargin, fillVal);
        break;
    }

    m_vol = volNew;
}

void ToolsRemoveConnected::FillEmptyRegions(const std::vector<uint8_t>& mark, const int seedIn[3])
{
    Q_UNUSED(seedIn); // пока не нужен

    if (!m_vol.u8().valid || !m_vol.raw())
        return;

    const size_t total = m_vol.u8().size();
    if (mark.size() < total)
        return; // подстраховка

    // 1) применяем маску: оставляем только выбранную компоненту
    for (size_t n = 0; n < total; ++n)
    {
        if (!mark[n])
            m_vol.at(n) = 0u;
    }

    Volume volNew;
    volNew.copy(m_vol.raw());

    const auto& S = volNew.u8();
    if (!S.valid || !S.p0)
    {
        m_vol = volNew;
        return;
    }

    const int* ext = S.ext; // [xmin,xmax, ymin,ymax, zmin,zmax]
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
    {
        m_vol = volNew;
        return;
    }

    const size_t totalLocal = static_cast<size_t>(nx) * ny * nz;
    const uint8_t fillVal = GetAverageVisibleValue();

    // ===============================
    // 2) Шаг 1: заполняем ТОЛЬКО внутренние полости
    // ===============================

    std::vector<uint8_t> outside(totalLocal, 0);
    std::queue<size_t> q;

    auto markOutside = [&](int i, int j, int k)
        {
            if (i < ext[0] || i > ext[1] ||
                j < ext[2] || j > ext[3] ||
                k < ext[4] || k > ext[5])
                return;

            const size_t idx = linearIdx(i, j, k, ext, nx, ny);
            if (outside[idx])
                return;

            if (volNew.at(idx) != 0u)
                return; // объект

            outside[idx] = 1;
            q.push(idx);
        };

    // старт по нулям на внешней границе
    for (int k = ext[4]; k <= ext[5]; ++k)
    {
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const bool onBoundary =
                    (i == ext[0] || i == ext[1] ||
                        j == ext[2] || j == ext[3] ||
                        k == ext[4] || k == ext[5]);
                if (!onBoundary)
                    continue;

                markOutside(i, j, k);
            }
        }
    }

    static const int N6[6][3] = {
        {+1, 0, 0}, {-1, 0, 0},
        {0, +1, 0}, {0, -1, 0},
        {0, 0, +1}, {0, 0, -1}
    };

    while (!q.empty())
    {
        const size_t w = q.front();
        q.pop();

        int i, j, k;
        ijkFromLinear(w, ext, nx, ny, i, j, k);

        for (const auto& d : N6)
        {
            const int ni = i + d[0];
            const int nj = j + d[1];
            const int nk = k + d[2];

            if (ni < ext[0] || ni > ext[1] ||
                nj < ext[2] || nj > ext[3] ||
                nk < ext[4] || nk > ext[5])
                continue;

            const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
            if (outside[nIdx])
                continue;
            if (volNew.at(nIdx) != 0u)
                continue;

            outside[nIdx] = 1;
            q.push(nIdx);
        }
    }

    // заливаем внутренние полости (нули, не помеченные как outside)
    for (size_t idx = 0; idx < totalLocal; ++idx)
    {
        if (volNew.at(idx) == 0u && !outside[idx])
            volNew.at(idx) = fillVal;
    }

    // ===============================
    // 3) Шаг 2: зашпаклевать мелкие порезы на поверхности
    // ===============================

    // делаем копию, чтобы не портить соседний подсчёт
    std::vector<uint8_t> dataCopy(totalLocal);
    for (size_t idx = 0; idx < totalLocal; ++idx)
        dataCopy[idx] = volNew.at(idx);

    auto countNonZeroNeighbors = [&](int i, int j, int k) -> int
        {
            int cnt = 0;
            for (const auto& d : N6)
            {
                const int ni = i + d[0];
                const int nj = j + d[1];
                const int nk = k + d[2];

                if (ni < ext[0] || ni > ext[1] ||
                    nj < ext[2] || nj > ext[3] ||
                    nk < ext[4] || nk > ext[5])
                    continue;

                const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
                if (dataCopy[nIdx] != 0u)
                    ++cnt;
            }
            return cnt;
        };

    // порог "соседей-объектов", при котором считаем нулевой воксель мелким разрезом
    const int minNeighborsToFill = 4; // 4–5 из 6 обычно хорошо сглаживает, не раздувая сильно

    for (int k = ext[4]; k <= ext[5]; ++k)
    {
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (dataCopy[idx] != 0u)
                    continue; // это уже объект

                const int nzNeigh = countNonZeroNeighbors(i, j, k);
                if (nzNeigh >= minNeighborsToFill)
                {
                    // этот ноль лежит в "щербинке" между вокселями объекта → заполняем
                    volNew.at(idx) = fillVal;
                }
            }
        }
    }

    m_vol = volNew;
}


int ToolsRemoveConnected::FillAndFindSurf(Volume& volNew, std::vector<uint8_t>& mark)
{
    if (!volNew.u8().valid || !volNew.raw())
        return -1;

    const size_t total = volNew.u8().size();
    if (mark.size() < total)
        return -1; // подстраховка

    const auto& S = volNew.u8();
    if (!S.valid || !S.p0)
        return -1;

    const int* ext = S.ext; // [xmin,xmax, ymin,ymax, zmin,zmax]
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return -1;

    const size_t totalLocal = static_cast<size_t>(nx) * ny * nz;
    const uint8_t fillVal = GetAverageVisibleValue();

    const size_t slice = size_t(nx) * ny;

    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
        {
            volNew.at(size_t(j) * nx + i) = 0u;  // Z = 0
            volNew.at(slice * (nz - 1) + size_t(j) * nx + i) = 0u; // Z = nz-1
        }

    for (int k = 0; k < nz; ++k)
        for (int i = 0; i < nx; ++i)
        {
            volNew.at(size_t(k) * slice + i) = 0u; // Y = 0
            volNew.at(size_t(k) * slice + size_t(ny - 1) * nx + i) = 0u; // Y = ny-1
        }

    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
        {
            volNew.at(size_t(k) * slice + size_t(j) * nx + 0) = 0u;      // X = 0
            volNew.at(size_t(k) * slice + size_t(j) * nx + nx - 1) = 0u;      // X = nx-1
        }

    // ===============================
    // 2) Шаг 1: заполняем ТОЛЬКО внутренние полости
    // ===============================

    std::vector<uint8_t> outside(totalLocal, 0);
    std::queue<size_t> q;

    auto markOutside = [&](int i, int j, int k)
        {
            if (i < ext[0] || i > ext[1] ||
                j < ext[2] || j > ext[3] ||
                k < ext[4] || k > ext[5])
                return;

            const size_t idx = linearIdx(i, j, k, ext, nx, ny);
            if (outside[idx])
                return;

            if (volNew.at(idx) != 0u)
                return; // объект

            outside[idx] = 1;
            q.push(idx);
        };

    // старт по нулям на внешней границе
    for (int k = ext[4]; k <= ext[5]; ++k)
    {
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const bool onBoundary =
                    (i == ext[0] || i == ext[1] ||
                        j == ext[2] || j == ext[3] ||
                        k == ext[4] || k == ext[5]);
                if (!onBoundary)
                    continue;

                markOutside(i, j, k);
            }
        }
    }

    static const int N6[6][3] = {
        {+1, 0, 0}, {-1, 0, 0},
        {0, +1, 0}, {0, -1, 0},
        {0, 0, +1}, {0, 0, -1}
    };

    while (!q.empty())
    {
        const size_t w = q.front();
        q.pop();

        int i, j, k;
        ijkFromLinear(w, ext, nx, ny, i, j, k);

        for (const auto& d : N6)
        {
            const int ni = i + d[0];
            const int nj = j + d[1];
            const int nk = k + d[2];

            if (ni < ext[0] || ni > ext[1] ||
                nj < ext[2] || nj > ext[3] ||
                nk < ext[4] || nk > ext[5])
                continue;

            const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
            if (outside[nIdx])
                continue;
            if (volNew.at(nIdx) != 0u)
                continue;

            outside[nIdx] = 1;
            q.push(nIdx);
        }
    }

    // заливаем внутренние полости (нули, не помеченные как outside)
    for (size_t idx = 0; idx < totalLocal; ++idx)
        if (volNew.at(idx) == 0u && !outside[idx])
            volNew.at(idx) = fillVal;

    // ===============================
    // 3) Шаг 2: зашпаклевать мелкие порезы на поверхности
    // ===============================

    // делаем копию, чтобы не портить соседний подсчёт
    std::vector<uint8_t> dataCopy(totalLocal);
    for (size_t idx = 0; idx < totalLocal; ++idx)
        dataCopy[idx] = volNew.at(idx);

    auto countNonZeroNeighbors = [&](int i, int j, int k) -> int
        {
            int cnt = 0;
            for (const auto& d : N6)
            {
                const int ni = i + d[0];
                const int nj = j + d[1];
                const int nk = k + d[2];

                if (ni < ext[0] || ni > ext[1] ||
                    nj < ext[2] || nj > ext[3] ||
                    nk < ext[4] || nk > ext[5])
                    continue;

                const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
                if (dataCopy[nIdx] != 0u)
                    ++cnt;
            }
            return cnt;
        };

    // порог "соседей-объектов", при котором считаем нулевой воксель мелким разрезом
    const int minNeighborsToFill = 4; // 4–5 из 6 обычно хорошо сглаживает, не раздувая сильно

    for (int k = ext[4]; k <= ext[5]; ++k)
    {
        for (int j = ext[2]; j <= ext[3]; ++j)
        {
            for (int i = ext[0]; i <= ext[1]; ++i)
            {
                const size_t idx = linearIdx(i, j, k, ext, nx, ny);
                if (dataCopy[idx] != 0u)
                    continue; // это уже объект

                const int nzNeigh = countNonZeroNeighbors(i, j, k);
                if (nzNeigh >= minNeighborsToFill)
                {
                    // этот ноль лежит в "щербинке" между вокселями объекта → заполняем
                    volNew.at(idx) = fillVal;
                }
            }
        }
    }

    for (size_t idx = 0; idx < total; idx++)
    {
        if (volNew.at(idx) == 0u)
            continue;

        // 6 индексов
        const size_t n0 = idx - 1;
        const size_t n1 = idx + 1;
        const size_t n2 = idx - nx;
        const size_t n3 = idx + nx;
        const size_t n4 = idx - slice;
        const size_t n5 = idx + slice;

        if (volNew.at(n0) == 0u ||
            volNew.at(n1) == 0u ||
            volNew.at(n2) == 0u ||
            volNew.at(n3) == 0u ||
            volNew.at(n4) == 0u ||
            volNew.at(n5) == 0u)
            mark[idx] = 1;
    }

    return fillVal;
}

int ToolsRemoveConnected::floodFill6MultiSeed(const Volume& bin,
    const std::vector<size_t>& seeds,
    std::vector<uint8_t>& mark) const
{
    const auto& S = bin.u8();
    if (!S.valid || !S.p0) return 0;

    const int* ext = S.ext;
    const int nx = S.nx, ny = S.ny, nz = S.nz;
    const size_t total = size_t(nx) * ny * nz;

    mark.assign(total, 0);

    std::queue<size_t> q;

    auto pushSeed = [&](size_t w) {
        if (w >= total) return;
        if (mark[w]) return;
        if (bin.at(w) == 0u) return;
        mark[w] = 1;
        q.push(w);
        };

    for (size_t w : seeds)
        pushSeed(w);

    if (q.empty()) return 0;

    static const int N6[6][3] = {
        {+1,0,0},{-1,0,0},{0,+1,0},{0,-1,0},{0,0,+1},{0,0,-1}
    };

    int visited = 0;
    while (!q.empty())
    {
        const size_t idx = q.front(); q.pop();
        ++visited;

        int i, j, k;
        ijkFromLinear(idx, ext, nx, ny, i, j, k);

        for (auto& d : N6)
        {
            const int ni = i + d[0], nj = j + d[1], nk = k + d[2];
            if (ni<ext[0] || ni>ext[1] || nj<ext[2] || nj>ext[3] || nk<ext[4] || nk>ext[5])
                continue;

            const size_t w = linearIdx(ni, nj, nk, ext, nx, ny);
            if (w >= total) continue;
            if (mark[w]) continue;
            if (bin.at(w) == 0u) continue;

            mark[w] = 1;
            q.push(w);
        }
    }
    return visited;
}

void ToolsRemoveConnected::ConnectSurfaceToVolume(Volume& volNew,
    const std::vector<uint8_t>& mark,
    int shift,
    uint8_t fillVal)
{
    if (!volNew.u8().valid || !volNew.raw())
        return;

    const auto& S = volNew.u8();
    if (!S.valid || !S.p0)
        return;

    const int* ext = S.ext; // [xmin,xmax, ymin,ymax, zmin,zmax]
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    if (nx <= 0 || ny <= 0 || nz <= 0)
        return;

    const size_t totalLocal = static_cast<size_t>(nx) * ny * nz;
    if (mark.size() < totalLocal)
        return;

    const int shiftClamped = std::max(1, shift);
    const int shift2 = shiftClamped * shiftClamped;

    // заливка отрезка между (i0,j0,k0) и (i1,j1,k1)
    auto fillSegment = [&](int i0, int j0, int k0,
        int i1, int j1, int k1)
        {
            int dx = i1 - i0;
            int dy = j1 - j0;
            int dz = k1 - k0;

            int steps = std::max({ std::abs(dx), std::abs(dy), std::abs(dz) });
            if (steps <= 1)
                return; // нет промежуточных вокселей

            const double incx = static_cast<double>(dx) / steps;
            const double incy = static_cast<double>(dy) / steps;
            const double incz = static_cast<double>(dz) / steps;

            double x = static_cast<double>(i0);
            double y = static_cast<double>(j0);
            double z = static_cast<double>(k0);

            for (int s = 1; s < steps; ++s)
            {
                x += incx;
                y += incy;
                z += incz;

                int ci = static_cast<int>(std::round(x));
                int cj = static_cast<int>(std::round(y));
                int ck = static_cast<int>(std::round(z));

                if (ci < ext[0] || ci > ext[1] ||
                    cj < ext[2] || cj > ext[3] ||
                    ck < ext[4] || ck > ext[5])
                    break;

                const size_t cIdx = linearIdx(ci, cj, ck, ext, nx, ny);
                if (volNew.at(cIdx) == 0u)
                    volNew.at(cIdx) = fillVal;
            }
        };

    Volume volNewPeel;
    volNewPeel.copy(volNew.raw());

    for (size_t n = 0; n < totalLocal; ++n)
        if (!mark[n])
            volNewPeel.at(n) = 0u;

    for (size_t idx = 0; idx < totalLocal; ++idx)
    {
        if (!mark[idx])
            continue;               // не поверхность

        if (volNew.at(idx) == 0u)
            continue;               // центральный должен быть не ноль

        int i, j, k;
        ijkFromLinear(idx, ext, nx, ny, i, j, k);

        // обходим окрестность радиуса shift
        for (int dz = -shiftClamped; dz <= shiftClamped; ++dz)
        {
            const int nk = k + dz;
            if (nk < ext[4] || nk > ext[5])
                continue;

            for (int dy = -shiftClamped; dy <= shiftClamped; ++dy)
            {
                const int nj = j + dy;
                if (nj < ext[2] || nj > ext[3])
                    continue;

                for (int dx = -shiftClamped; dx <= shiftClamped; ++dx)
                {
                    const int ni = i + dx;
                    if (ni < ext[0] || ni > ext[1])
                        continue;

                    const int dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 == 0 || dist2 > shift2)
                        continue;   // вне сферы радиуса shift или центр

                    const size_t nIdx = linearIdx(ni, nj, nk, ext, nx, ny);
                    if (volNewPeel.at(nIdx) == 0u)
                        continue;   // на сфере должен быть не ноль

                    // оба конца не нули → заполняем всё между ними
                    fillSegment(i, j, k, ni, nj, nk);
                }
            }
        }
    }

    for (size_t n = 0; n < totalLocal; ++n)
        if (!volNew.at(n) && (volNewPeel.at(n)))
            volNew.at(n) = fillVal;
}

void ToolsRemoveConnected::PeelRecoveryVolume()
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();

    Volume volNew;
    volNew.copy(m_vol.raw());

    for (size_t n = 0; n < total; ++n)
        if (!m_bin.at(n))
            volNew.at(n) = 0;

    const uint8_t fillVal = GetAverageVisibleValue();

    if (!AddBy6Neighbors(volNew, fillVal))
        return;

    if (m_hasOrig)
    {
        for (size_t n = 0; n < total; ++n)
            if (m_orig.at(n) && volNew.at(n))
                if ((m_orig.at(n) >= mHistLo) && (m_orig.at(n) <= mHistHi))
                    m_vol.at(n) = m_orig.at(n);
    }
    else
    {
        for (size_t n = 0; n < total; ++n)
            if (m_vol.at(n) && volNew.at(n))
                m_vol.at(n) = volNew.at(n);
    }

}

void ToolsRemoveConnected::TotalSmoothingVolume()
{
    if (!m_vol.u8().valid || !m_vol.raw()) return;

    const size_t total = m_vol.u8().size();

    Volume volNew;
    volNew.copy(m_vol.raw());

    int Shift = m_hoverRadiusVoxels;

    for (size_t n = 0; n < total; ++n)
        if (!m_bin.at(n))
            volNew.at(n) = 0;

    std::vector<uint8_t> mark(total, 0);
    int fillVal = FillAndFindSurf(volNew, mark);

    if (fillVal < 0)
        return;

    ConnectSurfaceToVolume(volNew, mark, Shift, static_cast<uint8_t>(fillVal));

    m_vol = volNew;
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