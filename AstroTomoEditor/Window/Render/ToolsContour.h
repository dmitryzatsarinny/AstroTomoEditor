#pragma once

#include "Tools.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>
#include <vector>

#include <vtkSmartPointer.h>

class QVTKOpenGLNativeWidget;

class vtkActor;
class vtkCellLocator;
class vtkCellPicker;
class vtkGlyph3DMapper;
class vtkPoints;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkSphereSource;
class vtkStaticPointLocator;

class ToolsContour final : public QObject
{
    Q_OBJECT

public:
    explicit ToolsContour(QWidget* hostParent = nullptr);
    ~ToolsContour() override = default;

    void attach(QVTKOpenGLNativeWidget* vtk,
        vtkRenderer* renderer,
        vtkPolyData* mesh,
        vtkActor* meshActor);

    bool handle(Action a);
    void onViewResized();

    void start();
    void cancel();
    void finish();

    void setOnSurfaceReplaced(std::function<void(vtkSmartPointer<vtkPolyData>, QVector<std::array<double, 3>>)> cb)
    {
        m_onSurfaceReplaced = std::move(cb);
    }

    void setOnFinished(std::function<void()> cb)
    {
        m_onFinished = std::move(cb);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class State
    {
        Off,
        Collecting
    };

    using WorldPoint = std::array<double, 3>;

private:
    bool pickPoint(const QPoint& viewPos, vtkIdType& pointId, WorldPoint& worldPoint) const;

    bool ensureCaches();
    void invalidateCaches();
    vtkIdType findClosestPathPointId(const double worldPoint[3]) const;
    void rebuildPathCacheFromMesh();

    bool computeGeodesicSegment(vtkIdType fromId,
        vtkIdType toId,
        std::vector<WorldPoint>& outPath) const;

    void rebuildContourFromControls();
    void updateDisplayContour();

    QVector<WorldPoint> buildSmoothedClosedLoop(const QVector<WorldPoint>& closedLoop) const;
    QVector<WorldPoint> resampleClosedLoop(const QVector<WorldPoint>& closedLoop, int targetCount) const;
    QVector<WorldPoint> smoothClosedLoopOnSurface(const QVector<WorldPoint>& closedLoop,
        int iterations,
        double factor) const;

    std::vector<WorldPoint> smoothOpenPathOnSurface(const std::vector<WorldPoint>& path,
        int iterations,
        double lambda,
        double mu) const;

    vtkSmartPointer<vtkPoints> projectLoopToMesh(
        const QVector<WorldPoint>& loop,
        vtkPolyData* mesh) const;

    bool applyContourCut();

    void createPreviewActors();
    void destroyPreviewActors();
    void updatePreviewGeometry();
    void renderNow();

    static double distanceSquared(const WorldPoint& a, const WorldPoint& b);
    static bool almostEqual(const WorldPoint& a, const WorldPoint& b, double eps2 = 1e-10);

    QVector<WorldPoint> smoothClosedLoopByWindow(const QVector<WorldPoint>& closedLoop,
        int halfWindow,
        int iterations) const;

    QVector<WorldPoint> projectClosedLoopToSurface(const QVector<WorldPoint>& closedLoop) const;

private:
    QWidget* m_host = nullptr;

    QPointer<QVTKOpenGLNativeWidget> m_vtk;
    vtkRenderer* m_renderer = nullptr;
    vtkPolyData* m_mesh = nullptr;
    vtkActor* m_meshActor = nullptr;

    vtkSmartPointer<vtkCellPicker> m_picker;

    vtkSmartPointer<vtkPolyData> m_pathMesh;
    vtkSmartPointer<vtkStaticPointLocator> m_pathPointLocator;
    vtkSmartPointer<vtkCellLocator> m_surfaceLocator;
    bool m_cacheValid = false;

    State m_state = State::Off;
    bool m_finishing = false;

    QVector<vtkIdType> m_controlIds;
    QVector<WorldPoint> m_controlWorldPoints;

    // Контур по сегментам геодезики.
    QVector<WorldPoint> m_rawContourWorldPoints;

    // Сглаженный контур для отображения и для выреза.
    QVector<WorldPoint> m_displayContourWorldPoints;

    vtkSmartPointer<vtkPolyData> m_previewLineData;
    vtkSmartPointer<vtkPolyDataMapper> m_previewLineMapper;
    vtkSmartPointer<vtkActor> m_previewLineActor;

    vtkSmartPointer<vtkPolyData> m_previewPointsData;
    vtkSmartPointer<vtkGlyph3DMapper> m_previewPointsMapper;
    vtkSmartPointer<vtkSphereSource> m_previewSphereSource;
    vtkSmartPointer<vtkActor> m_previewPointsActor;

    std::function<void(vtkSmartPointer<vtkPolyData>, QVector<std::array<double, 3>>)> m_onSurfaceReplaced;
    std::function<void()> m_onFinished;

private:
    // Параметры можно подстраивать под сцену.
    int m_previewMinSampleCount = 180;
    int m_previewWindowHalfSize = 5;
    int m_previewWindowIterations = 4;
    int m_previewSurfaceRelaxIterations = 4;
    double m_previewSurfaceRelaxFactor = 0.22;

    int m_pathfindingSubdivisionIterations = 1;
    double m_maxPathfindingSubdivisionGrowthRatio = 12.0;

    int m_segmentSmoothIterations = 3;
    double m_segmentSmoothLambda = 0.18;
    double m_segmentSmoothMu = -0.19;


    // Перед вырезом можно слегка уплотнить сетку,
    // чтобы линия выреза была менее зубчатой.
    int m_preCutSubdivisionIterations = 1;
    double m_maxPreCutSubdivisionGrowthRatio = 4.5;

    int m_cutSubdivisionIterations = 0;
    double m_maxSubdivisionGrowthRatio = 1.6;
};