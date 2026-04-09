#pragma once
#pragma once

#include <QWidget>
#include <QVector>
#include <QPoint>
#include <functional>
#include <vtkSmartPointer.h>
#include <array>
#include <vtkType.h>

class QVTKOpenGLNativeWidget;
class vtkActor;
class vtkPolyData;
class vtkRenderer;
class vtkCellPicker;

enum class Action;

class ToolsContour : public QObject
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
    void onViewResized();
    void cancel();

    void setOnSurfaceReplaced(std::function<void(vtkPolyData*)> cb) { m_onSurfaceReplaced = std::move(cb); }
    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class State { Off, Collecting };

    QWidget* m_host{ nullptr };
    QWidget* m_overlay{ nullptr };

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

    bool pickPoint(const QPoint& overlayPos, vtkIdType& pointId, std::array<double, 3>& worldPoint) const;
    bool appendGeodesicPath(vtkIdType fromId, vtkIdType toId);
    bool applyContourCut();
    void start();
    void finish();
    void redraw();
    void paintOverlay(QPainter& p);
    bool worldToOverlay(const std::array<double, 3>& world, QPointF& out) const;
};