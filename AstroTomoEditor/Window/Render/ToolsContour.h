#pragma once

#include <QObject>
#include <QPoint>
#include <QVector>
#include <array>
#include <functional>

#include <vtkSmartPointer.h>
#include <vtkType.h>

class QEvent;
class QWidget;
class QVTKOpenGLNativeWidget;

class vtkActor;
class vtkCellPicker;
class vtkGlyph3DMapper;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkRenderer;
class vtkSphereSource;

enum class Action;

class ToolsContour final : public QObject
{
    Q_OBJECT
public:
    explicit ToolsContour(QWidget* hostParent);
    ~ToolsContour() override = default;

    void attach(QVTKOpenGLNativeWidget* vtk,
        vtkRenderer* renderer,
        vtkPolyData* mesh,
        vtkActor* meshActor);

    bool handle(Action a);
    void onViewResized(); // оставлен для совместимости, сейчас не нужен
    void cancel();

    void setOnSurfaceReplaced(std::function<void(vtkPolyData*)> cb) { m_onSurfaceReplaced = std::move(cb); }
    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class State
    {
        Off,
        Collecting
    };

private:
    bool pickPoint(const QPoint& viewPos, vtkIdType& pointId, std::array<double, 3>& worldPoint) const;
    bool appendGeodesicPath(vtkIdType fromId, vtkIdType toId);
    bool applyContourCut();

    void start();
    void finish();

    void createPreviewActors();
    void destroyPreviewActors();
    void updatePreviewGeometry();
    void renderNow();

private:
    QWidget* m_host{ nullptr };

    State m_state{ State::Off };

    QVTKOpenGLNativeWidget* m_vtk{ nullptr };
    vtkRenderer* m_renderer{ nullptr };
    vtkPolyData* m_mesh{ nullptr };
    vtkActor* m_meshActor{ nullptr };

    vtkSmartPointer<vtkCellPicker> m_picker;

    QVector<vtkIdType> m_controlIds;
    QVector<std::array<double, 3>> m_contourWorldPoints;

    std::function<void(vtkPolyData*)> m_onSurfaceReplaced;
    std::function<void()> m_onFinished;

    // preview линии
    vtkSmartPointer<vtkPolyData> m_previewLineData;
    vtkSmartPointer<vtkPolyDataMapper> m_previewLineMapper;
    vtkSmartPointer<vtkActor> m_previewLineActor;

    // preview точек
    vtkSmartPointer<vtkPolyData> m_previewPointsData;
    vtkSmartPointer<vtkSphereSource> m_previewSphereSource;
    vtkSmartPointer<vtkGlyph3DMapper> m_previewPointsMapper;
    vtkSmartPointer<vtkActor> m_previewPointsActor;
};