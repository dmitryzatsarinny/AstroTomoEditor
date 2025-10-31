#pragma once
#include <QObject>
#include <functional>
#include <memory>
#include <vtkSmartPointer.h>
#include <QPoint>

class QWidget;
class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkImageData;
class vtkVolume;

enum class Action; // из Tools.h

// Клик по объекту => оставить (или удалить) только его связную компоненту.
class ToolsRemoveConnected : public QObject
{
    Q_OBJECT
public:
    explicit ToolsRemoveConnected(QWidget* hostParent);
    ~ToolsRemoveConnected() override = default;

    // привязка к текущему виду
    void attach(QVTKOpenGLNativeWidget* vtk, vtkRenderer* renderer,
        vtkImageData* image, vtkVolume* volume);

    // включить / запретить навигацию под инструментом (если true — колёсико/панорамирование работают)
    void setAllowNavigation(bool on) { m_allowNav = on; }

    // коллбэки на замену изображения (DeepCopy внутрь) и завершение инструмента
    void setOnImageReplaced(std::function<void(vtkImageData*)> cb) { m_onImageReplaced = std::move(cb); }
    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

    // запуск инструмента выбранным действием
    bool handle(Action a);

    // отмена (снятие overlay, выход из режима)
    void cancel();

    // обновление позиции overlay при ресайзе вида
    void onViewResized();

    void setHistogramMask(double lo, double hi) { mHistLo = lo; mHistHi = hi; }
    
    void notifyTfChanged() { rebuildVisibilityLUT(); }
protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void onLeftClick(const QPoint& pDevice);
    void start(Action a);
    void redraw();

    // ядро
    vtkSmartPointer<vtkImageData> makeBinaryMask(vtkImageData* src) const;  // uchar 0/1
    bool screenToSeedIJK(const QPoint& pDevice, vtkImageData* binMask, int ijk[3]) const;
    int  floodFill6(vtkImageData* binMask, const int seed[3], std::vector<uint8_t>& mark) const;
    void applyKeepOnlySelected(vtkImageData* image, const std::vector<uint8_t>& mark) const;
    void applyRemoveSelected(vtkImageData* image, const std::vector<uint8_t>& mark) const;

    // world↔ijk
    bool worldToIJK(const double world[3], int ijk[3]) const;
    void displayToWorld(double xd, double yd, double z01, double out[3]) const;
    void setHoverHighlightSizeVoxels(int r) { m_hoverRadiusVoxels = std::max(0, r); }

private:
    enum class State { Off, WaitingClick};
    State m_state{ State::Off };

    QWidget* m_host = nullptr;
    QWidget* m_overlay{ nullptr };
    QVector<QPoint> m_pts;

    QVTKOpenGLNativeWidget* m_vtk = nullptr;
    vtkRenderer* m_renderer = nullptr;
    vtkSmartPointer<vtkImageData> m_image;
    vtkVolume* m_volume = nullptr;

    Action                         m_mode;

    std::function<void(vtkImageData*)> m_onImageReplaced;
    std::function<void()>              m_onFinished;

    bool m_allowNav{ true }; // по умолчанию — включено
    void forwardMouseToVtk(QEvent* e); // проброс в QVTK

    std::vector<uint8_t> mVisibleLut; // 0/1 по интенсивности
    uint8_t  mHistLo{ 0 }, mHistHi{ 255 };
    double mLutMin = 0.0, mLutMax = 255.0;
    int    mLutBins = 256;
    double mVisibleEps = 0.01; // opacity

    void rebuildVisibilityLUT();    // дергаем при attach() и смене tp
    inline bool isVisible(double s) const; // быстрый тест через LUT

    QPoint m_lastMouse{};
    bool   m_hasHover{ false };
    int    m_hoverIJK[3]{ 0,0,0 };

    // 3D-подсветка выбранного вокселя
    vtkSmartPointer<class vtkExtractVOI>      mHoverVOI;
    vtkSmartPointer<class vtkOutlineFilter>   mHoverOutline;
    vtkSmartPointer<class vtkPolyDataMapper>  mHoverMapper;
    vtkSmartPointer<class vtkActor>           mHoverActor;

    // Кэш маски на время работы инструмента (чтобы не пересчитывать на каждый move)
    vtkSmartPointer<vtkImageData> mHoverMask;

    int m_hoverRadiusVoxels{ 0 };
    void ensureHoverPipeline();
    void updateHover(const QPoint& pDevice);
    void setHoverVisible(bool on);

};