#include "FaceOnlyBoxRepresentation.h"

#include <vtkObjectFactory.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkQuad.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkPropCollection.h>
#include <vtkViewport.h>
#include <vtkWindow.h>
#include <vtkRenderer.h>
#include <vtkMath.h>
#include <vtkBoxRepresentation.h>
#include <vtkMapper.h>
#include <vtkCamera.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>

vtkStandardNewMacro(FaceOnlyBoxRepresentation);

static void BuildQuadPoly(vtkPolyData* pd)
{
    vtkNew<vtkPoints> pts;
    pts->SetNumberOfPoints(4);

    vtkNew<vtkQuad> quad;
    quad->GetPointIds()->SetId(0, 0);
    quad->GetPointIds()->SetId(1, 1);
    quad->GetPointIds()->SetId(2, 2);
    quad->GetPointIds()->SetId(3, 3);

    vtkNew<vtkCellArray> cells;
    cells->InsertNextCell(quad);

    pd->SetPoints(pts);
    pd->SetPolys(cells);
}

FaceOnlyBoxRepresentation::FaceOnlyBoxRepresentation()
{
    for (int i = 0; i < 6; ++i)
    {
        mFacePd[i] = vtkSmartPointer<vtkPolyData>::New();
        BuildQuadPoly(mFacePd[i]);

        mFaceMapper[i] = vtkSmartPointer<vtkPolyDataMapper>::New();
        mFaceMapper[i]->SetInputData(mFacePd[i]);

        mFaceActor[i] = vtkSmartPointer<vtkActor>::New();
        mFaceActor[i]->SetMapper(mFaceMapper[i]);

        auto* p = mFaceActor[i]->GetProperty();
        p->SetLighting(false);
        p->SetAmbient(1.0);
        p->SetDiffuse(0.0);
        p->SetRepresentationToWireframe();
        p->SetOpacity(1.0);
        p->SetColor(0.2, 1.0, 0.2);   // зеленый
        p->SetLineWidth(2.0);
        mFaceActor[i]->PickableOff();
    }

    mActiveFillMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mActiveFillMapper->SetInputData(mFacePd[0]);

    mActiveFillActor = vtkSmartPointer<vtkActor>::New();
    mActiveFillActor->SetMapper(mActiveFillMapper);

    auto* fp = mActiveFillActor->GetProperty();
    fp->SetLighting(false);
    fp->SetAmbient(1.0);
    fp->SetDiffuse(0.0);
    fp->SetRepresentationToSurface();
    fp->SetColor(0.6, 0.6, 0.6);
    fp->SetOpacity(0.20);

    fp->SetBackfaceCulling(false);

    mActiveFillActor->PickableOff();
    mActiveFillActor->VisibilityOff();

    double b[6]{ 0,1,0,1,0,1 };
    PlaceBox(b);   // ← теперь безопасно
}


void FaceOnlyBoxRepresentation::PlaceBox(const double b[6])
{
    mCenter[0] = 0.5 * (b[0] + b[1]);
    mCenter[1] = 0.5 * (b[2] + b[3]);
    mCenter[2] = 0.5 * (b[4] + b[5]);

    mHalf[0] = 0.5 * (b[1] - b[0]);
    mHalf[1] = 0.5 * (b[3] - b[2]);
    mHalf[2] = 0.5 * (b[5] - b[4]);

    // оси единичные
    mAxis[0][0] = 1; mAxis[0][1] = 0; mAxis[0][2] = 0;
    mAxis[1][0] = 0; mAxis[1][1] = 1; mAxis[1][2] = 0;
    mAxis[2][0] = 0; mAxis[2][1] = 0; mAxis[2][2] = 1;

    UpdateCornersFromOBB();
    UpdateFaceGeometries();
    Modified();
}


void FaceOnlyBoxRepresentation::GetBounds(double outB[6]) const
{
    outB[0] = outB[2] = outB[4] = +VTK_DOUBLE_MAX;
    outB[1] = outB[3] = outB[5] = -VTK_DOUBLE_MAX;
    for (int i = 0; i < 8; ++i) {
        const double* p = &mCorners[i * 3];
        outB[0] = std::min(outB[0], p[0]); outB[1] = std::max(outB[1], p[0]);
        outB[2] = std::min(outB[2], p[1]); outB[3] = std::max(outB[3], p[1]);
        outB[4] = std::min(outB[4], p[2]); outB[5] = std::max(outB[5], p[2]);
    }
}

void FaceOnlyBoxRepresentation::BuildRepresentation()
{
    UpdateFaceGeometries();
}

void FaceOnlyBoxRepresentation::UpdateFaceGeometries()
{
    auto P = [&](int idx, double out[3]) {
        out[0] = mCorners[idx * 3 + 0];
        out[1] = mCorners[idx * 3 + 1];
        out[2] = mCorners[idx * 3 + 2];
        };

    const int face[6][4] = {
        {0,2,6,4}, // xmin
        {1,3,7,5}, // xmax
        {0,1,5,4}, // ymin
        {2,3,7,6}, // ymax
        {0,1,3,2}, // zmin
        {4,5,7,6}  // zmax
    };

    for (int f = 0; f < 6; ++f) {
        vtkPoints* pts = mFacePd[f]->GetPoints();
        for (int k = 0; k < 4; ++k) {
            double p[3]; P(face[f][k], p);
            pts->SetPoint(k, p);
        }
        pts->Modified();
        mFacePd[f]->Modified();
    }
}

void FaceOnlyBoxRepresentation::SetActiveFace(int faceIndex)
{
    if (mActiveFace == faceIndex) return;
    mActiveFace = faceIndex;

    // сбрасываем всё
    for (int i = 0; i < 6; ++i)
    {
        auto* pr = mFaceActor[i]->GetProperty();
        pr->SetColor(0.2, 1.0, 0.2);   // зелёные
        pr->SetLineWidth(2.0);
    }

    if (mActiveFace >= 0 && mActiveFace < 6)
    {
        // hover = серая заливка
        mActiveFillMapper->SetInputData(mFacePd[mActiveFace]);
        mActiveFillActor->VisibilityOn();
    }
    else
    {
        mActiveFillActor->VisibilityOff();
    }

    this->Modified();
}



vtkTypeBool FaceOnlyBoxRepresentation::HasTranslucentPolygonalGeometry()
{
    // заливка полупрозрачная -> true, когда видима
    return (mActiveFillActor && mActiveFillActor->GetVisibility())
        ? mActiveFillActor->HasTranslucentPolygonalGeometry()
        : 0;
}

int FaceOnlyBoxRepresentation::RenderTranslucentPolygonalGeometry(vtkViewport* vp)
{
    int c = 0;
    if (mActiveFillActor && mActiveFillActor->GetVisibility())
        c += mActiveFillActor->RenderTranslucentPolygonalGeometry(vp);
    return c;
}


int FaceOnlyBoxRepresentation::RenderOpaqueGeometry(vtkViewport* vp)
{
    int c = 0;
    for (int i = 0; i < 6; ++i)
        c += mFaceActor[i]->RenderOpaqueGeometry(vp);
    return c;
}

void FaceOnlyBoxRepresentation::GetActors(vtkPropCollection* pc)
{
    for (int i = 0; i < 6; ++i) pc->AddItem(mFaceActor[i]);
    pc->AddItem(mActiveFillActor);
}

void FaceOnlyBoxRepresentation::ReleaseGraphicsResources(vtkWindow* w)
{
    for (int i = 0; i < 6; ++i) mFaceActor[i]->ReleaseGraphicsResources(w);
    if (mActiveFillActor) mActiveFillActor->ReleaseGraphicsResources(w);
}


bool FaceOnlyBoxRepresentation::RayFromDisplay(vtkRenderer* ren, int x, int y, double rayO[3], double rayD[3])
{
    if (!ren) return false;

    ren->SetDisplayPoint((double)x, (double)y, 0.0);
    ren->DisplayToWorld();
    double w0[4]; ren->GetWorldPoint(w0);
    if (std::abs(w0[3]) < 1e-12) return false;
    rayO[0] = w0[0] / w0[3]; rayO[1] = w0[1] / w0[3]; rayO[2] = w0[2] / w0[3];

    ren->SetDisplayPoint((double)x, (double)y, 1.0);
    ren->DisplayToWorld();
    double w1[4]; ren->GetWorldPoint(w1);
    if (std::abs(w1[3]) < 1e-12) return false;

    double farP[3]{ w1[0] / w1[3], w1[1] / w1[3], w1[2] / w1[3] };
    rayD[0] = farP[0] - rayO[0];
    rayD[1] = farP[1] - rayO[1];
    rayD[2] = farP[2] - rayO[2];
    const double len = vtkMath::Norm(rayD);
    if (len < 1e-12) return false;
    rayD[0] /= len; rayD[1] /= len; rayD[2] /= len;
    return true;
}

void FaceOnlyBoxRepresentation::UpdateHoverFromDisplay(int x, int y, vtkRenderer* ren)
{
    if (mDragging) return;

    double O[3], D[3];
    if (!RayFromDisplay(ren, x, y, O, D)) { SetActiveFace(-1); return; }
    const int f = PickFaceByRay(O, D);
    SetActiveFace(f);
}


bool FaceOnlyBoxRepresentation::PointInsideQuad(const double p[4][3], const double hit[3])
{
    double n[3], e0[3], e1[3];
    vtkMath::Subtract(p[1], p[0], e0);
    vtkMath::Subtract(p[2], p[0], e1);
    vtkMath::Cross(e0, e1, n);

    const double nn = vtkMath::Norm(n);
    if (nn < 1e-12) return false;
    n[0] /= nn; n[1] /= nn; n[2] /= nn;

    constexpr double eps = 1e-6;

    bool hasPos = false;
    bool hasNeg = false;

    for (int i = 0; i < 4; ++i)
    {
        const double* a = p[i];
        const double* b = p[(i + 1) & 3];

        double edge[3], v[3], c[3];
        vtkMath::Subtract(b, a, edge);
        vtkMath::Subtract(hit, a, v);
        vtkMath::Cross(edge, v, c);

        const double s = vtkMath::Dot(c, n);
        if (s > eps) hasPos = true;
        if (s < -eps) hasNeg = true;

        if (hasPos && hasNeg) return false; // смешались знаки => точно снаружи
    }
    return true;
}

void FaceOnlyBoxRepresentation::SetAllFacesHighlighted(bool on)
{
    for (int i = 0; i < 6; ++i)
    {
        auto* pr = mFaceActor[i]->GetProperty();
        if (on)
        {
            pr->SetColor(1.0, 1.0, 0.2); // жёлтые
            pr->SetLineWidth(3.0);
        }
        else
        {
            pr->SetColor(0.2, 1.0, 0.2);
            pr->SetLineWidth(2.0);
        }
    }

    this->Modified();
}

void FaceOnlyBoxRepresentation::StartDragWhole(int x, int y, vtkRenderer* ren)
{
    double O[3], D[3];
    if (!RayFromDisplay(ren, x, y, O, D)) return;

    // берём плоскость, перпендикулярную взгляду камеры
    double camDir[3];
    ren->GetActiveCamera()->GetDirectionOfProjection(camDir);
    vtkMath::Normalize(camDir);

    double t;
    if (!IntersectRayPlane(O, D, camDir, mCenter, t)) return;

    mMoveHit0[0] = O[0] + t * D[0];
    mMoveHit0[1] = O[1] + t * D[1];
    mMoveHit0[2] = O[2] + t * D[2];

    mDragMode = DragMode::Whole;
    mDragging = true;

    SetAllFacesHighlighted(true);
}

void FaceOnlyBoxRepresentation::DragWhole(int x, int y, vtkRenderer* ren)
{
    if (mDragMode != DragMode::Whole) return;

    double O[3], D[3];
    if (!RayFromDisplay(ren, x, y, O, D)) return;

    double camDir[3];
    ren->GetActiveCamera()->GetDirectionOfProjection(camDir);
    vtkMath::Normalize(camDir);

    // плоскость через текущий центр OBB
    double t;
    if (!IntersectRayPlane(O, D, camDir, mCenter, t)) return;

    double hit[3]{
        O[0] + t * D[0],
        O[1] + t * D[1],
        O[2] + t * D[2]
    };

    double delta[3];
    vtkMath::Subtract(hit, mMoveHit0, delta);

    // двигаем ЦЕНТР
    mCenter[0] += delta[0];
    mCenter[1] += delta[1];
    mCenter[2] += delta[2];

    // запоминаем точку
    mMoveHit0[0] = hit[0];
    mMoveHit0[1] = hit[1];
    mMoveHit0[2] = hit[2];

    UpdateCornersFromOBB();
    UpdateFaceGeometries();
    Modified();
}



int FaceOnlyBoxRepresentation::PickFaceByRay(const double rayO[3], const double rayD[3]) const
{
    int bestFace = -1;
    double bestT = VTK_DOUBLE_MAX;

    for (int f = 0; f < 6; ++f)
    {
        double q[4][3];
        GetFacePoints(f, q);

        // плоскость по 3 точкам
        double e0[3], e1[3], n[3];
        vtkMath::Subtract(q[1], q[0], e0);
        vtkMath::Subtract(q[2], q[0], e1);
        vtkMath::Cross(e0, e1, n);

        const double nn = vtkMath::Norm(n);
        if (nn < 1e-12) continue;
        n[0] /= nn; n[1] /= nn; n[2] /= nn;

        const double denom = vtkMath::Dot(n, rayD);
        if (std::abs(denom) < 1e-9) continue;

        // t = dot(n, (p0 - O)) / dot(n, D)
        double p0mO[3]; vtkMath::Subtract(q[0], rayO, p0mO);
        const double t = vtkMath::Dot(n, p0mO) / denom;
        if (t <= 0.0 || t >= bestT) continue;

        double hit[3] = { rayO[0] + t * rayD[0], rayO[1] + t * rayD[1], rayO[2] + t * rayD[2] };
        if (!PointInsideQuad(q, hit)) continue;

        bestT = t;
        bestFace = f;
    }

    return bestFace;
}

void FaceOnlyBoxRepresentation::GetFacePoints(int f, double p[4][3]) const
{
    const double* c = mCorners.data();

    auto P = [&](int idx, double out[3]) {
        out[0] = c[idx * 3 + 0];
        out[1] = c[idx * 3 + 1];
        out[2] = c[idx * 3 + 2];
        };

    static const int face[6][4] = {
        {0,2,6,4}, // xmin
        {1,3,7,5}, // xmax
        {0,1,5,4}, // ymin
        {2,3,7,6}, // ymax
        {0,1,3,2}, // zmin
        {4,5,7,6}  // zmax
    };

    if (f < 0 || f > 5) {
        for (int i = 0; i < 4; ++i) p[i][0] = p[i][1] = p[i][2] = 0.0;
        return;
    }

    for (int k = 0; k < 4; ++k)
        P(face[f][k], p[k]);
}

void FaceOnlyBoxRepresentation::ComputeFacePlane(int f, double n[3], double p0[3]) const
{
    const int a = (f <= 1) ? 0 : (f <= 3) ? 1 : 2;
    const bool isMax = (f == 1 || f == 3 || f == 5);
    const double s = isMax ? +1.0 : -1.0;

    // нормаль = ± локальная ось
    n[0] = mAxis[a][0] * s;
    n[1] = mAxis[a][1] * s;
    n[2] = mAxis[a][2] * s;

    // точка на плоскости = центр ± ось * half
    p0[0] = mCenter[0] + mAxis[a][0] * (s * mHalf[a]);
    p0[1] = mCenter[1] + mAxis[a][1] * (s * mHalf[a]);
    p0[2] = mCenter[2] + mAxis[a][2] * (s * mHalf[a]);
}


bool FaceOnlyBoxRepresentation::IntersectRayPlane(
    const double O[3], const double D[3],
    const double n[3], const double p0[3],
    double& t) const
{
    const double denom = vtkMath::Dot(n, D);
    if (std::abs(denom) < 1e-9) return false;

    double p0O[3];
    vtkMath::Subtract(p0, O, p0O);
    t = vtkMath::Dot(n, p0O) / denom;
    return t > 0.0;
}


void FaceOnlyBoxRepresentation::StartDrag(int x, int y, vtkRenderer* ren)
{
    double O[3], D[3];
    if (!RayFromDisplay(ren, x, y, O, D)) return;

    mDragFace = mActiveFace;
    if (mDragFace < 0) return;

    double n[3], p0[3];
    ComputeFacePlane(mDragFace, n, p0);
    vtkMath::Normalize(n);

    mDragAxisN[0] = n[0];
    mDragAxisN[1] = n[1];
    mDragAxisN[2] = n[2];

    double t;
    if (!IntersectRayPlane(O, D, n, p0, t)) return;

    mDragHit0[0] = O[0] + t * D[0];
    mDragHit0[1] = O[1] + t * D[1];
    mDragHit0[2] = O[2] + t * D[2];

    mDragging = true;

    auto* pr = mFaceActor[mActiveFace]->GetProperty();
    pr->SetColor(1.0, 1.0, 0.2);
    pr->SetLineWidth(3.0);

    Modified();
}

void FaceOnlyBoxRepresentation::DragTo(int x, int y, vtkRenderer* ren)
{
    if (!mDragging || mDragFace < 0 || !ren) return;

    double O[3], D[3];
    if (!RayFromDisplay(ren, x, y, O, D)) return;

    // плоскость, перпендикулярная взгляду камеры, проходящая через mDragHit0
    double camDir[3];
    ren->GetActiveCamera()->GetDirectionOfProjection(camDir);
    vtkMath::Normalize(camDir);

    double t;
    if (!IntersectRayPlane(O, D, camDir, mDragHit0, t)) return;

    double hit[3]{
        O[0] + t * D[0],
        O[1] + t * D[1],
        O[2] + t * D[2]
    };

    double deltaVec[3];
    vtkMath::Subtract(hit, mDragHit0, deltaVec);

    // проекция движения мыши на нормаль выбранной грани
    const double d = vtkMath::Dot(deltaVec, mDragAxisN);
    if (std::abs(d) < 1e-9) {
        // все равно обновим опорную точку, чтобы не "залипало" при микродвижениях
        mDragHit0[0] = hit[0];
        mDragHit0[1] = hit[1];
        mDragHit0[2] = hit[2];
        return;
    }

    ApplyFaceDelta(mDragFace, d);

    // как в Whole: опорная точка обновляется каждый тик
    mDragHit0[0] = hit[0];
    mDragHit0[1] = hit[1];
    mDragHit0[2] = hit[2];

    UpdateFaceGeometries();
    Modified();
}



void FaceOnlyBoxRepresentation::ApplyFaceDelta(int face, double d)
{
    // face: 0 xmin, 1 xmax, 2 ymin, 3 ymax, 4 zmin, 5 zmax
    int a = (face <= 1) ? 0 : (face <= 3) ? 1 : 2;

    // знак: для “минусовой” грани нормаль = -axis[a], для “плюсовой” = +axis[a]
    const bool isMax = (face == 1 || face == 3 || face == 5);
    const double s = isMax ? +1.0 : -1.0;

    // хотим сдвинуть грань на величину d вдоль её нормали.
    // это означает:
    // half[a] увеличится на d/2
    // center сдвинется на (d/2) * normal
    const double dh = 0.5 * d;

    // не даем “перевернуться”
    const double newHalf = mHalf[a] + dh;
    if (newHalf < 0.5) return; // подбери минимум под себя

    mHalf[a] = newHalf;

    mCenter[0] += mAxis[a][0] * (s * dh);
    mCenter[1] += mAxis[a][1] * (s * dh);
    mCenter[2] += mAxis[a][2] * (s * dh);

    UpdateCornersFromOBB();
}



void FaceOnlyBoxRepresentation::EndDrag()
{
    mDragging = false;
    mDragFace = -1;
    mDragMode = DragMode::None;
    SetAllFacesHighlighted(false);
    this->Modified();
}

bool FaceOnlyBoxRepresentation::ClosestPointParam_LineLine(
    const double O[3], const double D[3],
    const double A[3], const double N[3],
    double& uOut)
{
    double AO[3];
    vtkMath::Subtract(A, O, AO);

    const double a = vtkMath::Dot(D, D);   // ~1
    const double b = vtkMath::Dot(D, N);
    const double c = vtkMath::Dot(N, N);   // =1
    const double d = vtkMath::Dot(D, AO);
    const double e = vtkMath::Dot(N, AO);

    const double det = a * c - b * b;
    if (std::abs(det) < 1e-9)
        return false;

    uOut = (a * e - b * d) / det;
    return true;
}

void FaceOnlyBoxRepresentation::StartRotate(int x, int y, vtkRenderer* ren)
{
    if (!ren) return;

    mDragMode = DragMode::Rotate;
    mDragging = true;
    mLastX = x;
    mLastY = y;

    SetAllFacesHighlighted(true);
    this->Modified();
}

static inline void RotatePointAroundAxis(const double axis[3], double angleRad,
    const double in[3], double out[3])
{
    // Rodrigues
    double k[3]{ axis[0], axis[1], axis[2] };
    vtkMath::Normalize(k);

    const double c = std::cos(angleRad);
    const double s = std::sin(angleRad);

    double kxin[3]; vtkMath::Cross(k, in, kxin);
    const double kdot = vtkMath::Dot(k, in);

    out[0] = in[0] * c + kxin[0] * s + k[0] * (kdot * (1.0 - c));
    out[1] = in[1] * c + kxin[1] * s + k[1] * (kdot * (1.0 - c));
    out[2] = in[2] * c + kxin[2] * s + k[2] * (kdot * (1.0 - c));
}

void FaceOnlyBoxRepresentation::SetFillActorUnVisible()
{
    if (mActiveFillActor) mActiveFillActor->VisibilityOff();
}

void FaceOnlyBoxRepresentation::SetFillActorVisible()
{
    if (mActiveFillActor) mActiveFillActor->VisibilityOn();
}

void FaceOnlyBoxRepresentation::DragRotate(int x, int y, vtkRenderer* ren)
{
    if (mDragMode != DragMode::Rotate || !mDragging || !ren) return;

    const int dx = x - mLastX;
    const int dy = y - mLastY;
    mLastX = x;
    mLastY = y;

    if (dx == 0 && dy == 0) return;

    // центр куба
    double c[3]{ 0,0,0 };
    for (int i = 0; i < 8; ++i)
    {
        c[0] += mCorners[i * 3 + 0];
        c[1] += mCorners[i * 3 + 1];
        c[2] += mCorners[i * 3 + 2];
    }
    c[0] /= 8.0; c[1] /= 8.0; c[2] /= 8.0;

    auto* cam = ren->GetActiveCamera();
    if (!cam) return;

    double viewUp[3]; cam->GetViewUp(viewUp); vtkMath::Normalize(viewUp);

    double dop[3]; cam->GetDirectionOfProjection(dop);
    vtkMath::Normalize(dop);

    // right = viewUp x dir (аккуратно со знаком)
    double right[3];
    vtkMath::Cross(viewUp, dop, right);
    vtkMath::Normalize(right);

    // чувствительность (градусы на пиксель)
    const double k = 0.35;
    const double angUp = vtkMath::RadiansFromDegrees(-dx * k); // по X мыши крутим вокруг Up
    const double angRight = vtkMath::RadiansFromDegrees(-dy * k); // по Y мыши крутим вокруг Right

    for (int a = 0; a < 3; ++a)
    {
        double v[3]{ mAxis[a][0], mAxis[a][1], mAxis[a][2] };
        double v1[3], v2[3];
        RotatePointAroundAxis(viewUp, angUp, v, v1);
        RotatePointAroundAxis(right, angRight, v1, v2);
        mAxis[a][0] = v2[0]; mAxis[a][1] = v2[1]; mAxis[a][2] = v2[2];
    }

    // ортонормализация (чтобы не расползалось)
    vtkMath::Normalize(mAxis[0]);
    vtkMath::Cross(mAxis[0], mAxis[1], mAxis[2]); vtkMath::Normalize(mAxis[2]);
    vtkMath::Cross(mAxis[2], mAxis[0], mAxis[1]); vtkMath::Normalize(mAxis[1]);

    UpdateCornersFromOBB();
    UpdateFaceGeometries();
    Modified();
}

void FaceOnlyBoxRepresentation::UpdateCornersFromOBB()
{
    // углы: (+/-X) (+/-Y) (+/-Z) в локальных координатах
    // порядок тот же, что у твоих faceCorners
    const int s[8][3] = {
        {-1,-1,-1}, {+1,-1,-1}, {-1,+1,-1}, {+1,+1,-1},
        {-1,-1,+1}, {+1,-1,+1}, {-1,+1,+1}, {+1,+1,+1}
    };

    for (int i = 0; i < 8; ++i)
    {
        double p[3] = { mCenter[0], mCenter[1], mCenter[2] };
        for (int a = 0; a < 3; ++a)
        {
            const double k = (double)s[i][a] * mHalf[a];
            p[0] += mAxis[a][0] * k;
            p[1] += mAxis[a][1] * k;
            p[2] += mAxis[a][2] * k;
        }
        mCorners[i * 3 + 0] = p[0];
        mCorners[i * 3 + 1] = p[1];
        mCorners[i * 3 + 2] = p[2];
    }
}

#include <vtkPlane.h>
#include <vtkPlaneCollection.h>

vtkSmartPointer<vtkPlaneCollection> FaceOnlyBoxRepresentation::MakeClippingPlanes() const
{
    auto planes = vtkSmartPointer<vtkPlaneCollection>::New();

    // 6 плоскостей: по две на каждую локальную ось
    for (int a = 0; a < 3; ++a)
    {
        for (int side = 0; side < 2; ++side)
        {
            const double s = (side == 0) ? +1.0 : -1.0;

            // точка на плоскости
            double origin[3]{
                mCenter[0] + mAxis[a][0] * (s * mHalf[a]),
                mCenter[1] + mAxis[a][1] * (s * mHalf[a]),
                mCenter[2] + mAxis[a][2] * (s * mHalf[a])
            };

            // нормаль (первично как ±axis)
            double normal[3]{
                mAxis[a][0] * s,
                mAxis[a][1] * s,
                mAxis[a][2] * s
            };

            auto pl = vtkSmartPointer<vtkPlane>::New();
            pl->SetOrigin(origin);
            pl->SetNormal(normal);

            // ключ: гарантируем, что центр бокса НЕ отсекается этой плоскостью
            // если центр "не с той стороны", переворачиваем нормаль
            const double v = pl->EvaluateFunction(
                mCenter[0],
                mCenter[1],
                mCenter[2]
            );

            if (v < 0.0)
                pl->SetNormal(-normal[0], -normal[1], -normal[2]);

            planes->AddItem(pl);
        }
    }

    return planes;
}


