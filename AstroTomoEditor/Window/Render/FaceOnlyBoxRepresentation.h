#pragma once
#include <vtkWidgetRepresentation.h>
#include <vtkSmartPointer.h>
#include <vtkSmartPointer.h>
#include <array>

class vtkActor;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkActor;
class vtkRenderer;
class vtkWindow;
class vtkViewport;
class vtkPlaneCollection;

enum class DragMode
{
    None,
    Face,   // LMB
    Whole,  // RMB
    Rotate  // Alt+RMB
};

class FaceOnlyBoxRepresentation : public vtkWidgetRepresentation
{
public:
    static FaceOnlyBoxRepresentation* New();
    vtkTypeMacro(FaceOnlyBoxRepresentation, vtkWidgetRepresentation);

    // --- Геометрия куба (world-space AABB) ---
    void PlaceBox(const double bounds[6]);           // xmin xmax ymin ymax zmin zmax
    void GetBounds(double outB[6]) const;

    // --- Hover/pick ---
    void UpdateHoverFromDisplay(int x, int y, vtkRenderer* ren);

    // --- Drag ---
    void StartDrag(int x, int y, vtkRenderer* ren);
    void DragTo(int x, int y, vtkRenderer* ren);
    void EndDrag();

    int  ActiveFace() const { return mActiveFace; }

    // --- VTK pipeline ---
    void BuildRepresentation() override;
    void GetActors(vtkPropCollection* pc) override;
    void ReleaseGraphicsResources(vtkWindow* w) override;

    int RenderOpaqueGeometry(vtkViewport* viewport) override;

    int RenderTranslucentPolygonalGeometry(vtkViewport* viewport) override;
    vtkTypeBool HasTranslucentPolygonalGeometry() override;
    
    void StartDragWhole(int x, int y, vtkRenderer* ren);
    void DragWhole(int x, int y, vtkRenderer* ren);
    void StartRotate(int x, int y, vtkRenderer* ren);
    void DragRotate(int x, int y, vtkRenderer* ren);
    vtkSmartPointer<vtkPlaneCollection> MakeClippingPlanes() const;

    void SetFillActorUnVisible();
    void SetFillActorVisible();

protected:
    FaceOnlyBoxRepresentation();
    ~FaceOnlyBoxRepresentation() override = default;

private:
    void SetActiveFace(int faceIndex);
    void UpdateFaceGeometries();
    void UpdateCornersFromBounds();
    int  PickFaceByRay(const double rayO[3], const double rayD[3]) const;
    void GetFacePoints(int f, double p[4][3]) const;
    static bool PointInsideQuad(const double p[4][3], const double hit[3]);
    void SetAllFacesHighlighted(bool on);
    void UpdateCornersFromOBB();
    static bool RayFromDisplay(vtkRenderer* ren, int x, int y, double rayO[3], double rayD[3]);

    // Драг по плоскости активной грани
    void ComputeFacePlane(int f, double n[3], double p0[3]) const;
    bool IntersectRayPlane(const double rayO[3], const double rayD[3],
        const double n[3], const double p0[3], double& t) const;
    void ApplyFaceDelta(int face, double delta);
    bool ClosestPointParam_LineLine(
        const double O[3], const double D[3],
        const double A[3], const double N[3],
        double& uOut);
private:
    int mActiveFace = -1;
    bool mDragging = false;

    int    mDragFace = -1;
    double mDragHit0[3]{ 0,0,0 };   // точка пересечения при старте
    int mLastX = 0;
    int mLastY = 0;

    DragMode mDragMode = DragMode::None;

    // для перемещения всего куба
    double mMoveHit0[3]{ 0,0,0 };

    // 8 углов (world): [0..7], как обычно у AABB
    std::array<double, 24> mCorners{}; // 8*3

    double mDragAxisO[3]{ 0,0,0 };   // точка на оси
    double mDragAxisN[3]{ 0,0,1 };   // нормаль (unit)
    double mDragU0 = 0.0;          // прошлое значение параметра

    vtkSmartPointer<vtkPolyData>       mFacePd[6];
    vtkSmartPointer<vtkPolyDataMapper> mFaceMapper[6];
    vtkSmartPointer<vtkActor>          mFaceActor[6];

    vtkSmartPointer<vtkPolyDataMapper> mActiveFillMapper;
    vtkSmartPointer<vtkActor>          mActiveFillActor;

    double mCenter[3]{ 0,0,0 };                 // центр
    double mAxis[3][3]{ {1,0,0},{0,1,0},{0,0,1} }; // локальные оси куба (unit)
    double mHalf[3]{ 1,1,1 };                   // половины размеров по осям
};
