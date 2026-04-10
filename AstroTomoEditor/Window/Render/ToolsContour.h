#pragma once

#include "Tools.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>

#include <vtkSmartPointer.h>

class QVTKOpenGLNativeWidget;

class vtkActor;
class vtkCellPicker;
class vtkGlyph3DMapper;
class vtkPoints;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkSphereSource;

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

    void setOnSurfaceReplaced(std::function<void(vtkSmartPointer<vtkPolyData>)> cb)
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

private:
    using WorldPoint = std::array<double, 3>;

    bool pickPoint(const QPoint& viewPos, vtkIdType& pointId, WorldPoint& worldPoint) const;

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

    bool applyContourCut();

    void createPreviewActors();
    void destroyPreviewActors();
    void updatePreviewGeometry();
    void renderNow();

    static double distanceSquared(const WorldPoint& a, const WorldPoint& b);
    static bool almostEqual(const WorldPoint& a, const WorldPoint& b, double eps2 = 1e-10);

private:
    QWidget* m_host = nullptr;

    QPointer<QVTKOpenGLNativeWidget> m_vtk;
    vtkRenderer* m_renderer = nullptr;
    vtkPolyData* m_mesh = nullptr;
    vtkActor* m_meshActor = nullptr;

    vtkSmartPointer<vtkCellPicker> m_picker;

    State m_state = State::Off;
    bool m_finishing = false;

    QVector<vtkIdType> m_controlIds;
    QVector<WorldPoint> m_controlWorldPoints;

    // Сырой контур по геодезическим сегментам.
    QVector<WorldPoint> m_rawContourWorldPoints;

    // То, что реально рисуем: для >= 3 точек уже сглаженный замкнутый луп.
    QVector<WorldPoint> m_displayContourWorldPoints;

    vtkSmartPointer<vtkPolyData> m_previewLineData;
    vtkSmartPointer<vtkPolyDataMapper> m_previewLineMapper;
    vtkSmartPointer<vtkActor> m_previewLineActor;

    vtkSmartPointer<vtkPolyData> m_previewPointsData;
    vtkSmartPointer<vtkGlyph3DMapper> m_previewPointsMapper;
    vtkSmartPointer<vtkSphereSource> m_previewSphereSource;
    vtkSmartPointer<vtkActor> m_previewPointsActor;

    std::function<void(vtkSmartPointer<vtkPolyData>)> m_onSurfaceReplaced;
    std::function<void()> m_onFinished;

private:
    // Параметры можно спокойно крутить под сцену.
    int m_previewMinSampleCount = 140;
    int m_previewSmoothIterations = 8;
    double m_previewSmoothFactor = 0.42;

    // 0 = быстрее, 1 = более мягкий край выреза за счёт дополнительной триангуляции.
    int m_cutSubdivisionIterations = 1;
};