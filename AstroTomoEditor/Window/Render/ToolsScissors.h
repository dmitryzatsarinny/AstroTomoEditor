#pragma once
#include <QWidget>
#include <QVector>
#include <QPoint>
#include <array>
#include <functional>
#include <vtkVolume.h>
#include <vtkWeakPointer.h>

class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkImageData;
class vtkVolume;
class vtkPolyData;
class vtkActor;

enum class Action; // из Tools.h

// Инструмент "ножницы": рисует 2D-полигон на оверлее и вырезает том внутри/снаружи.
class ToolsScissors : public QObject
{
    Q_OBJECT
public:
    explicit ToolsScissors(QWidget* hostParent);
    ~ToolsScissors() override = default;

    // Привязка актуальных указателей из RenderView (без геттеров и «дружбы»)
    void attach(QVTKOpenGLNativeWidget* vtk,
        vtkRenderer* renderer,
        vtkImageData* image,
        vtkVolume* volume);

    void attachSurface(QVTKOpenGLNativeWidget* vtk,
        vtkRenderer* renderer,
        vtkPolyData* mesh,
        vtkActor* actor);

    // Колбэк, чтобы заменить mImage в RenderView (иначе только маппер обновится)
    void setOnImageReplaced(std::function<void(vtkImageData*)> cb) { m_onImageReplaced = std::move(cb); }
    void setOnSurfaceReplaced(std::function<void(vtkPolyData*, QVector<QVector<std::array<double, 3>>>)> cb) { mOnSurfaceReplaced = std::move(cb); }

    // Обработка выбора из меню Tools (Scissors / InverseScissors)
    bool handle(Action a);

    // Перестройка геометрии оверлея (зови из RenderView::repositionOverlay)
    void onViewResized();

    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

    // Разрешить навигацию мышью при активных ножницах
    void setAllowNavigation(bool on) { m_allowNav = on; }
    void cancel();
protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    enum class State { Off, Collecting };
    State m_state{ State::Off };
    bool  m_cutInside{ true }; // Scissors=true, Inverse=false

    QWidget* m_host{ nullptr };       // родитель (обычно RenderView)
    QWidget* m_overlay{ nullptr };    // прозрачный слой над VTK
    QVector<QPoint> m_pts;            // 2D-точки в координатах overlay

    QPoint m_cursorPos{};
    bool   m_hasCursor{ false };

    QVTKOpenGLNativeWidget* m_vtk{ nullptr };
    vtkRenderer* m_renderer{ nullptr };
    vtkImageData* m_image{ nullptr };
    vtkVolume* m_volume{ nullptr };

    vtkWeakPointer<vtkPolyData> mSurfaceMesh;
    vtkWeakPointer<vtkActor>    mSurfaceActor;
    bool mSurfaceMode = false;

    std::function<void(vtkImageData*)> m_onImageReplaced;
    std::function<void(vtkPolyData*, QVector<QVector<std::array<double, 3>>>)> mOnSurfaceReplaced;
    std::function<void()> m_onFinished;

    double mSurfaceVoxelSpacing[3]{ 0.8, 0.8, 0.8 };
    int mSurfaceVoxelPadding = 4;

    // Внутренняя логика
    void start(bool cutInside);
    void finish();
    void redraw();
    void repeat();

    // Построение маски/вырез тома
    vtkImageData* applyPolygonCut(vtkImageData* sourceImage, const QVector<QPoint>& pts2D, bool cutInside);
    vtkImageData* applyPolygonCut(const QVector<QPoint>& pts2D, bool cutInside);
    vtkPolyData* applySurfaceCut(const QVector<QPoint>& pts2D, bool cutInside);
    vtkPolyData* applySurfaceCutVoxelized(const QVector<QPoint>& pts2D, bool cutInside);

    // События overlay
    void paintOverlay(QPainter& p);

    bool m_allowNav{ true }; // по умолчанию — включено
    void forwardMouseToVtk(QEvent* e); // проброс в QVTK
    bool mUseVoxelizedSurfaceCut{ false };
    vtkImageData* buildVoxelVolumeFromSurface(vtkPolyData* surface) const;
};
