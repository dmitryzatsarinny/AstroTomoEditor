#include "ClipBoxController.h"

#include <vtkBoxWidget2.h>
#include <vtkBoxRepresentation.h>
#include <vtkCallbackCommand.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkVolume.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkNew.h>
#include <algorithm>
#include <cmath>
#include <vtkAbstractMapper.h>
#include <vtkActor.h>
#include <vtkPlanes.h>
#include <vtkPolyDataMapper.h>
#include <vtkDoubleArray.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <array>

// -- ВСПОМОГАТЕЛЬНОЕ --
// Конверсия AABB в мире -> IJK (по данным vtkImageData),
// учитывая матрицу актёра (Volume). Бокс должен быть осевой.
static inline void WorldBoundsToIJK_WithActor(
    vtkImageData* img,
    vtkVolume* volume,
    const double worldBounds[6],
    int ijkOut[6])
{
    // Готовим World->Actor матрицу
    vtkNew<vtkMatrix4x4> actorToWorld;
    volume->GetMatrix(actorToWorld); // Actor->World
    vtkNew<vtkMatrix4x4> worldToActor;
    vtkMatrix4x4::Invert(actorToWorld, worldToActor); // World->Actor

    // 8 углов мирового AABB
    const double cornersW[8][3] = {
        {worldBounds[0], worldBounds[2], worldBounds[4]},
        {worldBounds[1], worldBounds[2], worldBounds[4]},
        {worldBounds[0], worldBounds[3], worldBounds[4]},
        {worldBounds[1], worldBounds[3], worldBounds[4]},
        {worldBounds[0], worldBounds[2], worldBounds[5]},
        {worldBounds[1], worldBounds[2], worldBounds[5]},
        {worldBounds[0], worldBounds[3], worldBounds[5]},
        {worldBounds[1], worldBounds[3], worldBounds[5]},
    };

    double ijkMin[3] = { +1e300, +1e300, +1e300 };
    double ijkMax[3] = { -1e300, -1e300, -1e300 };

    for (int c = 0; c < 8; ++c)
    {
        // Переводим угол из мира в локальные координаты актёра
        double pW[4] = { cornersW[c][0], cornersW[c][1], cornersW[c][2], 1.0 };
        double pA[4] = { 0,0,0,1 };
        worldToActor->MultiplyPoint(pW, pA);

        // ActorLocal для vtkImageData — это физическое пространство данных
        double ijk[3];
        img->TransformPhysicalPointToContinuousIndex(pA, ijk);

        for (int a = 0; a < 3; ++a) {
            ijkMin[a] = std::min(ijkMin[a], ijk[a]);
            ijkMax[a] = std::max(ijkMax[a], ijk[a]);
        }
    }

    // Преобразуем в целочисленные индексы (включительно)
    ijkOut[0] = static_cast<int>(std::floor(ijkMin[0]));
    ijkOut[1] = static_cast<int>(std::ceil(ijkMax[0]));
    ijkOut[2] = static_cast<int>(std::floor(ijkMin[1]));
    ijkOut[3] = static_cast<int>(std::ceil(ijkMax[1]));
    ijkOut[4] = static_cast<int>(std::floor(ijkMin[2]));
    ijkOut[5] = static_cast<int>(std::ceil(ijkMax[2]));

    int ext[6]; img->GetExtent(ext);
    for (int a = 0; a < 3; ++a) {
        ijkOut[2 * a + 0] = std::clamp(ijkOut[2 * a + 0], ext[2 * a + 0], ext[2 * a + 1]);
        ijkOut[2 * a + 1] = std::clamp(ijkOut[2 * a + 1], ext[2 * a + 0], ext[2 * a + 1]);
        if (ijkOut[2 * a + 0] > ijkOut[2 * a + 1]) std::swap(ijkOut[2 * a + 0], ijkOut[2 * a + 1]);
    }
}

static void ComputeAABB(const std::array<double, 24>& pts, double outB[6])
{
    outB[0] = outB[2] = outB[4] = std::numeric_limits<double>::infinity();
    outB[1] = outB[3] = outB[5] = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 8; i++) {
        const double* p = &pts[i * 3];
        outB[0] = std::min(outB[0], p[0]); outB[1] = std::max(outB[1], p[0]);
        outB[2] = std::min(outB[2], p[1]); outB[3] = std::max(outB[3], p[1]);
        outB[4] = std::min(outB[4], p[2]); outB[5] = std::max(outB[5], p[2]);
    }
}

// хелпер: преобразование 3D-точки матрицей 4×4
static inline void MxP(const vtkMatrix4x4* m, const double in[3], double out[3])
{
    out[0] = m->GetElement(0, 0) * in[0] + m->GetElement(0, 1) * in[1] + m->GetElement(0, 2) * in[2] + m->GetElement(0, 3);
    out[1] = m->GetElement(1, 0) * in[0] + m->GetElement(1, 1) * in[1] + m->GetElement(1, 2) * in[2] + m->GetElement(1, 3);
    out[2] = m->GetElement(2, 0) * in[0] + m->GetElement(2, 1) * in[1] + m->GetElement(2, 2) * in[2] + m->GetElement(2, 3);
}

static vtkSmartPointer<vtkPlaneCollection>
MakePlaneCollectionFromBox(vtkBoxRepresentation* rep, bool flipNormals)
{
    vtkNew<vtkPlanes> boxPlanes;
    rep->GetPlanes(boxPlanes);

    auto normals = vtkDoubleArray::SafeDownCast(boxPlanes->GetNormals());
    auto points = boxPlanes->GetPoints();
    auto out = vtkSmartPointer<vtkPlaneCollection>::New();

    double n[3], p[3];
    for (int i = 0; i < 6; ++i) {
        normals->GetTuple(i, n);
        points->GetPoint(i, p);

        if (flipNormals) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

        vtkNew<vtkPlane> pl;
        pl->SetNormal(n);
        pl->SetOrigin(p);
        out->AddItem(pl);
    }
    return out;
}

// ----------------------

ClipBoxController::ClipBoxController(QObject* parent)
    : QObject(parent)
{
    // Репрезентация и настройки поведения
    mRep = vtkSmartPointer<vtkBoxRepresentation>::New();
    mRep->SetPlaceFactor(1.0);
    mRep->HandlesOn();
    mRep->SetOutlineFaceWires(false);
    mRep->SetOutlineCursorWires(false);
    mRep->SetHandleSize(10);
    
    // Виджет
    mWidget = vtkSmartPointer<vtkBoxWidget2>::New();
    mWidget->SetRepresentation(mRep);
    mWidget->RotationEnabledOff();
    mWidget->EnabledOff();

    // Колбэк на движение/перетаскивание ручек
    mCb = vtkSmartPointer<vtkCallbackCommand>::New();
    mCb->SetClientData(this);
    mCb->SetCallback(&ClipBoxController::onInteraction);

    mWidget->AddObserver(vtkCommand::StartInteractionEvent, mCb);
    mWidget->AddObserver(vtkCommand::InteractionEvent, mCb);
    mWidget->AddObserver(vtkCommand::EndInteractionEvent, mCb);
    mWidget->SetPriority(1.0);
}

void ClipBoxController::setRenderer(vtkRenderer* ren)
{
    mRenderer = ren;
    if (mWidget) mWidget->SetCurrentRenderer(mRenderer);
}

void ClipBoxController::setInteractor(vtkRenderWindowInteractor* iren)
{
    mInteractor = iren;
    if (mWidget) mWidget->SetInteractor(mInteractor);
}

void ClipBoxController::attachToVolume(vtkVolume* vol)
{
    mVolume = vol;
    if (!mVolume) { setEnabled(false); return; }

    // Поставить бокс по границам объёма (в МИРОВЫХ координатах сцены)
    double vb[6]; 
    if (mEnabled && mWidget && mWidget->GetEnabled())
    {
        const double* rb = mRep->GetBounds();
        std::copy(rb, rb + 6, vb);
        mRep->PlaceWidget(vb);
    }
    else
    {
        mVolume->GetBounds(vb);
        mRep->PlaceWidget(vb);
    }

    // Сбросить любые старые клипы/кропы
    if (auto* mapper = mVolume->GetMapper()) {
        if (auto* am = vtkPolyDataMapper::SafeDownCast(mapper))
            am->RemoveAllClippingPlanes();        // ← корректный сброс
        if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mapper))
            gm->SetCropping(0);
    }

    if (mRenderer) mRenderer->ResetCameraClippingRange();
    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr) rw->Render();
}

void ClipBoxController::applyNow()
{
    if (!mEnabled) return;

    // переиспользуем твою логику
    applyClippingFromBox();

    if (mRenderer) mRenderer->ResetCameraClippingRange();
    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr)
        rw->Render();
}

void ClipBoxController::attachToSTL(vtkActor* actor)
{
    mSurface = actor;
    // Разрываем связь с объёмом, чтобы не мешать
    mVolume = nullptr;

    if (!mSurface) { setEnabled(false); return; }

    // Поставить бокс по границам актёра (мир)
    double vb[6];
    if (mEnabled && mWidget && mWidget->GetEnabled())
    {
        const double* rb = mRep->GetBounds();
        std::copy(rb, rb + 6, vb);
        mRep->PlaceWidget(vb);
    }
    else
    {
        mSurface->GetBounds(vb);
        mRep->PlaceWidget(vb);
    }

    // Сбросить прежние клип-плоскости на мэппере
    if(auto* mapper = mSurface->GetMapper()) {
        if (auto* pm = vtkPolyDataMapper::SafeDownCast(mapper)) {
            pm->RemoveAllClippingPlanes();
            pm->Modified();
        }
    }

    // (на всякий случай) если до этого был включён кроп у volume-мэппера — выключим
    if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mVolume ? mVolume->GetMapper() : nullptr))
        gm->SetCropping(0);

}


void ClipBoxController::setEnabled(bool on)
{
    const bool hasTarget = (mRenderer && mInteractor && (mVolume || mSurface));
    const bool want = on && hasTarget;
    if (mEnabled == want) return;

    mEnabled = want;
    if (mWidget) mWidget->SetEnabled(mEnabled ? 1 : 0);

    // объём
    if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mVolume ? mVolume->GetMapper() : nullptr)) 
    {
        if (mEnabled)
        {
            applyClippingFromBox();
        }
        else          
            gm->SetCropping(0);
    }

    // поверхность
    if (auto* pm = vtkPolyDataMapper::SafeDownCast(mSurface ? mSurface->GetMapper() : nullptr)) {
        if (mEnabled) 
        {
            applyClippingFromBox();
        }
        else 
        {
            pm->RemoveAllClippingPlanes();
            pm->Modified();
        }
    }


    if (mRenderer) mRenderer->ResetCameraClippingRange();
    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr)
        rw->Render();
}

void ClipBoxController::resetToBounds()
{
    // 1) если режем поверхность — всё просто
    if (mSurface) {
        double b[6];
        mSurface->GetBounds(b);
        mRep->PlaceWidget(b);
        return;
    }

    // 2) объём
    if (mVolume) {
        auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(
            mVolume->GetMapper());

        // 2a) если клип-бокс включён прямо сейчас — используем его текущие границы
        //     (они в мировых координатах и соответствуют "видимому" региону)
        if (mEnabled && mWidget && mWidget->GetEnabled()) 
        {
            double wb[6];
            const double* rb = mRep->GetBounds();   // ← без аргументов
            std::copy(rb, rb + 6, wb);
            mRep->PlaceWidget(wb);
            return;
        }

        // 2b) если у volume включён кроп — ставим бокс по CroppingRegionPlanes
        if (gm && gm->GetCropping()) {
            double region[6];
            gm->GetCroppingRegionPlanes(region);

            // region уже в координатах данных (мм), переведём 8 углов в мир актёра
            const double corners[8][3] = {
                {region[0], region[2], region[4]},
                {region[1], region[2], region[4]},
                {region[0], region[3], region[4]},
                {region[1], region[3], region[4]},
                {region[0], region[2], region[5]},
                {region[1], region[2], region[5]},
                {region[0], region[3], region[5]},
                {region[1], region[3], region[5]},
            };

            vtkMatrix4x4* M = mVolume->GetMatrix();
            double wb[6] = { +DBL_MAX, -DBL_MAX, +DBL_MAX, -DBL_MAX, +DBL_MAX, -DBL_MAX };
            for (int i = 0; i < 8; i++) {
                double w[3];
                w[0] = M->GetElement(0, 0) * corners[i][0] + M->GetElement(0, 1) * corners[i][1] + M->GetElement(0, 2) * corners[i][2] + M->GetElement(0, 3);
                w[1] = M->GetElement(1, 0) * corners[i][0] + M->GetElement(1, 1) * corners[i][1] + M->GetElement(1, 2) * corners[i][2] + M->GetElement(1, 3);
                w[2] = M->GetElement(2, 0) * corners[i][0] + M->GetElement(2, 1) * corners[i][1] + M->GetElement(2, 2) * corners[i][2] + M->GetElement(2, 3);
                wb[0] = std::min(wb[0], w[0]); wb[1] = std::max(wb[1], w[0]);
                wb[2] = std::min(wb[2], w[1]); wb[3] = std::max(wb[3], w[1]);
                wb[4] = std::min(wb[4], w[2]); wb[5] = std::max(wb[5], w[2]);
            }
            mRep->PlaceWidget(wb);
            return;
        }

        // 2c) по умолчанию — весь объём
        double b[6];
        mVolume->GetBounds(b);
        mRep->PlaceWidget(b);
        return;
    }
}

void ClipBoxController::applyClippingFromBox()
{
    if (!mEnabled) return;

    // 1) берём текущие плоскости из box-rep в МИРОВЫХ координатах
    auto* rep = vtkBoxRepresentation::SafeDownCast(mRep);
    if (!rep) return;

    vtkNew<vtkPlanes> planes;
    rep->GetPlanes(planes); // ← готовая шестерка плоскостей в world-space

    // 2a) Если клипим объём — переводим в IJK и включаем кроп (как у тебя было)
    if (mVolume) {
        auto* mapper = mVolume->GetMapper();
        auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mapper);
        auto* img = vtkImageData::SafeDownCast(mapper ? mapper->GetInputDataObject(0, 0) : nullptr);
        if (!gm || !img) return;

        // берём границы коробки и твой существующий путь World→Actor→IJK
        double wb[6];
        const double* rb = rep->GetBounds();
        std::copy(rb, rb + 6, wb);

        int ijk[6];
        WorldBoundsToIJK_WithActor(img, mVolume, wb, ijk);

        double origin[3], spacing[3];
        img->GetOrigin(origin);
        img->GetSpacing(spacing);

        double region[6]{
            origin[0] + spacing[0] * ijk[0], origin[0] + spacing[0] * ijk[1],
            origin[1] + spacing[1] * ijk[2], origin[1] + spacing[1] * ijk[3],
            origin[2] + spacing[2] * ijk[4], origin[2] + spacing[2] * ijk[5]
        };

        gm->SetCroppingRegionFlagsToSubVolume();
        gm->SetCroppingRegionPlanes(region);
        gm->SetCropping(1);
        gm->Modified();
        return;
    }

    // 2b) Если клипим поверхность — просто задаём плоскости на мэппер
    if (mSurface) 
    {
        if (auto* pm = vtkPolyDataMapper::SafeDownCast(mSurface->GetMapper())) 
        {
            auto pc = MakePlaneCollectionFromBox(rep, /*flipNormals=*/true);
            pm->SetClippingPlanes(pc);
            pm->Modified();
        }
    }

    if (mRenderer) mRenderer->ResetCameraClippingRange();
}

void ClipBoxController::onInteraction(vtkObject* caller, unsigned long evId, void* cd, void*)
{
    auto* self = static_cast<ClipBoxController*>(cd);
    if (!self) return;

    // Всегда обновляем planes (дешёво)
    self->applyClippingFromBox();

    // Тяжёлый пересчёт клиппинг-рейнджа и отрисовка — только в конце драга
    if (evId == vtkCommand::EndInteractionEvent) 
    {
        if (self->mRenderer) self->mRenderer->ResetCameraClippingRange();
        if (self->mInteractor && self->mInteractor->GetRenderWindow()) 
        {
            self->mInteractor->GetRenderWindow()->Render();
        }
        else if (self->mRenderer && self->mRenderer->GetRenderWindow()) {
            self->mRenderer->GetRenderWindow()->Render();
        }
    }
}
