#include "ClipBoxController.h"

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
#include <vtkProperty.h>
#include <QDebug.h>
#include <vtkSmartPointer.h>

static inline int iFloor(double x) { return static_cast<int>(std::floor(x)); }
static inline int iCeil(double x) { return static_cast<int>(std::ceil(x)); }

static void WorldBoundsToIJK_WithActor(vtkImageData* img, vtkVolume* vol, const double wb[6], int outIJK[6])
{
    // fallback: если что-то не так, пусть будет весь объём
    int ext[6]; img->GetExtent(ext);
    outIJK[0] = ext[0]; outIJK[1] = ext[1];
    outIJK[2] = ext[2]; outIJK[3] = ext[3];
    outIJK[4] = ext[4]; outIJK[5] = ext[5];

    if (!img) return;

    // 1) матрица world->actorLocal (actorLocal = physical space изображения, если volume не переопределял scale/origin)
    vtkSmartPointer<vtkMatrix4x4> worldToActor = vtkSmartPointer<vtkMatrix4x4>::New();
    worldToActor->Identity();

    if (vol)
    {
        vtkSmartPointer<vtkMatrix4x4> actorToWorld = vtkSmartPointer<vtkMatrix4x4>::New();
        actorToWorld->Identity();
        vol->GetMatrix(actorToWorld);                 // Actor(local)->World
        vtkMatrix4x4::Invert(actorToWorld, worldToActor); // World->Actor(local)
    }

    // 2) 8 углов bounds в world
    const double xs[2]{ wb[0], wb[1] };
    const double ys[2]{ wb[2], wb[3] };
    const double zs[2]{ wb[4], wb[5] };

    // будем считать continuous indices
    double ciMin[3]{ 1e300,  1e300,  1e300 };
    double ciMax[3]{ -1e300, -1e300, -1e300 };

    auto Accum = [&](const double worldP[3])
        {
            // world -> actorLocal(=physical)
            double p4[4]{ worldP[0], worldP[1], worldP[2], 1.0 };
            double a4[4];
            worldToActor->MultiplyPoint(p4, a4);

            double phys[3]{ a4[0], a4[1], a4[2] };

            // physical -> continuous index
            double cidx[3]{ 0,0,0 };

#if VTK_MAJOR_VERSION >= 9
            // В VTK 9 это есть и учитывает origin/spacing/direction
            img->TransformPhysicalPointToContinuousIndex(phys, cidx);
#else
            // более старый fallback (без direction может быть криво)
            double origin[3], spacing[3];
            img->GetOrigin(origin);
            img->GetSpacing(spacing);
            cidx[0] = (phys[0] - origin[0]) / spacing[0];
            cidx[1] = (phys[1] - origin[1]) / spacing[1];
            cidx[2] = (phys[2] - origin[2]) / spacing[2];
#endif

            for (int ax = 0; ax < 3; ++ax)
            {
                ciMin[ax] = std::min(ciMin[ax], cidx[ax]);
                ciMax[ax] = std::max(ciMax[ax], cidx[ax]);
            }
        };

    for (int ix = 0; ix < 2; ++ix)
        for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
            {
                double p[3]{ xs[ix], ys[iy], zs[iz] };
                Accum(p);
            }

    // 3) continuous -> ijk (целые, расширяем наружу)
    int ijk[6];
    ijk[0] = iFloor(ciMin[0]);
    ijk[1] = iCeil(ciMax[0]);
    ijk[2] = iFloor(ciMin[1]);
    ijk[3] = iCeil(ciMax[1]);
    ijk[4] = iFloor(ciMin[2]);
    ijk[5] = iCeil(ciMax[2]);

    // 4) clamp в extent
    ijk[0] = std::max(ext[0], std::min(ext[1], ijk[0]));
    ijk[1] = std::max(ext[0], std::min(ext[1], ijk[1]));
    ijk[2] = std::max(ext[2], std::min(ext[3], ijk[2]));
    ijk[3] = std::max(ext[2], std::min(ext[3], ijk[3]));
    ijk[4] = std::max(ext[4], std::min(ext[5], ijk[4]));
    ijk[5] = std::max(ext[4], std::min(ext[5], ijk[5]));

    // на всякий: порядок
    if (ijk[0] > ijk[1]) std::swap(ijk[0], ijk[1]);
    if (ijk[2] > ijk[3]) std::swap(ijk[2], ijk[3]);
    if (ijk[4] > ijk[5]) std::swap(ijk[4], ijk[5]);

    for (int i = 0; i < 6; ++i) outIJK[i] = ijk[i];
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
MakePlaneCollectionFromAABB(const double b[6], bool flipNormals)
{
    auto out = vtkSmartPointer<vtkPlaneCollection>::New();

    const double normals[6][3] = {
        {-1,0,0}, {+1,0,0},
        {0,-1,0}, {0,+1,0},
        {0,0,-1}, {0,0,+1}
    };

    const double origins[6][3] = {
        {b[0], 0, 0}, {b[1], 0, 0},
        {0, b[2], 0}, {0, b[3], 0},
        {0, 0, b[4]}, {0, 0, b[5]}
    };

    for (int i = 0; i < 6; ++i)
    {
        double n[3] = { normals[i][0], normals[i][1], normals[i][2] };
        double o[3] = { origins[i][0], origins[i][1], origins[i][2] };

        if (flipNormals) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }

        vtkNew<vtkPlane> pl;
        pl->SetNormal(n);
        pl->SetOrigin(o);
        out->AddItem(pl);
    }
    return out;
}

ClipBoxController::ClipBoxController(QObject* parent)
    : QObject(parent)
{
    mRep = vtkSmartPointer<FaceOnlyBoxRepresentation>::New();

    mWidget = vtkSmartPointer<FaceOnlyBoxWidget>::New();
    mWidget->SetRepresentation(mRep); // важно: теперь без кастов

    mWidget->EnabledOff();

    mCb = vtkSmartPointer<vtkCallbackCommand>::New();
    mCb->SetClientData(this);
    mCb->SetCallback(&ClipBoxController::onInteraction);

    mWidget->AddObserver(vtkCommand::StartInteractionEvent, mCb);
    mWidget->AddObserver(vtkCommand::InteractionEvent, mCb);
    mWidget->AddObserver(vtkCommand::EndInteractionEvent, mCb);
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
    mSurface = nullptr;

    if (!mVolume) { setEnabled(false); return; }

    double vb[6];
    if (mEnabled && mWidget && mWidget->GetEnabled())
    {
        mRep->GetBounds(vb);
        mRep->PlaceBox(vb);
    }
    else
    {
        mVolume->GetBounds(vb);
        mRep->PlaceBox(vb);
    }

    if (auto* mapper = mVolume->GetMapper())
    {
        if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mapper))
            gm->SetCropping(0);
    }

    if (mRenderer) mRenderer->ResetCameraClippingRange();
    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr) rw->Render();
}

void ClipBoxController::attachToSTL(vtkActor* actor)
{
    mSurface = actor;
    mVolume = nullptr;

    if (!mSurface) { setEnabled(false); return; }

    double vb[6];
    if (mEnabled && mWidget && mWidget->GetEnabled())
    {
        mRep->GetBounds(vb);
        mRep->PlaceBox(vb);
    }
    else
    {
        mSurface->GetBounds(vb);
        mRep->PlaceBox(vb);
    }

    if (auto* mapper = mSurface->GetMapper())
    {
        if (auto* pm = vtkPolyDataMapper::SafeDownCast(mapper))
        {
            pm->RemoveAllClippingPlanes();
            pm->Modified();
        }
    }

    if (mRenderer) mRenderer->ResetCameraClippingRange();
    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr) rw->Render();
}

void ClipBoxController::setEnabled(bool on)
{
    const bool hasTarget = (mRenderer && mInteractor && (mVolume || mSurface));
    const bool want = on && hasTarget;
    if (mEnabled == want) return;

    // --- ВЫКЛЮЧЕНИЕ CLIP ---
    if (mEnabled && !want)
    {
        // 1. вернуть бокс в full bounds (XYZ, без поворотов)
        resetToBounds();

        // 2. быстро применить клип по full bounds
        applyClippingFromBox();

        // 3. теперь безопасно выключаем клип
        if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(
            mVolume ? mVolume->GetMapper() : nullptr))
        {
            gm->SetCropping(false);
            gm->Modified();
        }

        if (auto* pm = vtkPolyDataMapper::SafeDownCast(
            mSurface ? mSurface->GetMapper() : nullptr))
        {
            pm->RemoveAllClippingPlanes();
            pm->Modified();
        }
    }

    mEnabled = want;

    if (mWidget)
        mWidget->SetEnabled(mEnabled ? 1 : 0);

    // --- ВКЛЮЧЕНИЕ CLIP ---
    if (mEnabled)
    {
        // всегда стартуем из ровного XYZ-бокса
        resetToBounds();
        applyClippingFromBox();
    }

    if (mRenderer)
        mRenderer->ResetCameraClippingRange();

    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr)
        rw->Render();
}

void ClipBoxController::resetToBounds()
{
    if (mSurface) {
        double b[6]; mSurface->GetBounds(b);
        mRep->PlaceBox(b);
        return;
    }

    if (mVolume) {
        double b[6]; mVolume->GetBounds(b);
        mRep->PlaceBox(b);
        return;
    }
}

void ClipBoxController::applyNow()
{
    if (!mEnabled) return;
    applyClippingFromBox();

    if (mRenderer) mRenderer->ResetCameraClippingRange();
    if (auto* rw = mRenderer ? mRenderer->GetRenderWindow() : nullptr)
        rw->Render();
}
#include <vtkVolumeMapper.h> 

void ClipBoxController::applyClippingFromBox()
{
    if (!mEnabled) return;
    if (!mRep) return;

    // --- volume: НЕ через cropping, а через 6 плоскостей (поддерживает поворот) ---
    if (mVolume)
    {
        auto* mapper = mVolume->GetMapper();
        auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mapper);
        if (!gm) return;

        // важно: если раньше включали cropping, он будет мешать и создавать ощущение "не режется"
        gm->SetCropping(false);

        // сбросим старые плоскости
        gm->RemoveAllClippingPlanes();

        // плоскости берем из репрезентации (OBB)
        auto planes = mRep->MakeClippingPlanes();     // <-- метод в FaceOnlyBoxRepresentation
        gm->SetClippingPlanes(planes);

        gm->Modified();
    }

    // --- surface: тоже лучше резать теми же плоскостями, а не AABB ---
    if (mSurface)
    {
        if (auto* pm = vtkPolyDataMapper::SafeDownCast(mSurface->GetMapper()))
        {
            auto planes = mRep->MakeClippingPlanes();
            pm->SetClippingPlanes(planes);
            pm->Modified();
        }
    }

    if (mRenderer && mRenderer->GetRenderWindow())
        mRenderer->GetRenderWindow()->Render();
}

void ClipBoxController::onInteraction(vtkObject*, unsigned long evId, void* cd, void*)
{
    auto* self = static_cast<ClipBoxController*>(cd);
    if (!self) return;

    self->applyClippingFromBox();

    if (evId == vtkCommand::EndInteractionEvent)
    {
        if (self->mRenderer) self->mRenderer->ResetCameraClippingRange();
        if (self->mInteractor && self->mInteractor->GetRenderWindow())
            self->mInteractor->GetRenderWindow()->Render();
        else if (self->mRenderer && self->mRenderer->GetRenderWindow())
            self->mRenderer->GetRenderWindow()->Render();
    }
}
