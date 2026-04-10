#pragma once
#include <QWidget>
#include <QVector>
#include <QPoint>
#include <functional>
#include <vtkVolume.h>

class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkImageData;
class vtkVolume;

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

    // Колбэк, чтобы заменить mImage в RenderView (иначе только маппер обновится)
    void setOnImageReplaced(std::function<void(vtkImageData*)> cb) { m_onImageReplaced = std::move(cb); }

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

    std::function<void(vtkImageData*)> m_onImageReplaced;
    std::function<void()> m_onFinished;

    // Внутренняя логика
    void start(bool cutInside);
    void finish();
    void redraw();
    void repeat();

    // Построение маски/вырез тома
    vtkImageData* applyPolygonCut(const QVector<QPoint>& pts2D, bool cutInside);

    // События overlay
    void paintOverlay(QPainter& p);

    bool m_allowNav{ true }; // по умолчанию — включено
    void forwardMouseToVtk(QEvent* e); // проброс в QVTK
};
