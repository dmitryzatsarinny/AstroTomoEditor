#pragma once
#include <QObject>
#include <functional>
#include <memory>
#include <vtkSmartPointer.h>
#include <QPoint>
#include "U8Span.h"

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
    void makeBinaryMask(vtkImageData* m_image);  // uchar 0/1
    bool screenToSeedIJK(const QPoint& pDevice, int ijk[3]) const;
    int floodFill6(const Volume& bin, const int seed[3], std::vector<uint8_t>& mark) const;
    void applyKeepOnlySelected(const std::vector<uint8_t>& mark);
    void applyRemoveSelected(const std::vector<uint8_t>& mark);
    void RemoveConnectedRegions(const std::vector<uint8_t>& mark, const int seed[3]) const;
    
    // world↔ijk
    bool worldToIJK(const double world[3], int ijk[3]) const;
    void displayToWorld(double xd, double yd, double z01, double out[3]) const;
    void setHoverHighlightSizeVoxels(int r) { m_hoverRadiusVoxels = std::max(0, r); }

    // Построить маску выбранной компоненты (1 = принадлежит компоненте seed)
    bool buildSelectedComponentMask(vtkImageData* image, const int seed[3], std::vector<uint8_t>& outMask, int& outCount) const;

    // Удалить несвязанные области (оставить только выбранную компоненту)
    void removeUnconnected(const std::vector<uint8_t>& selMask);

    // Снять «кожуру» за один шаг (обнулить граничные воксели mask) Возвращает число единиц в outMask после шага.
    int peelOnce(const std::vector<uint8_t>& inMask, std::vector<uint8_t>& outMask, const int ext[6]) const;

    // Оставить только часть маски, связанную с seed (через floodFill6)
    int keepOnlyConnectedFromSeed(std::vector<uint8_t>& mask, const int ext[6], const int seed[3]) const;

    // Умное снятие кожуры (итерации + порог «резкого падения») Возвращает число выполненных итераций (didPeelIters).
    int smartPeel(std::vector<uint8_t>& mask, const std::vector<uint8_t>& selMask, const int ext[6], const int seed[3], double dropFrac, int maxIters) const;

    // «Recovery»: дилатация внутри исходной выбранной компоненты
    void recoveryDilate(std::vector<uint8_t>& mask, const std::vector<uint8_t>& selMask, const int ext[6], int iters) const;

    // Применить бинарную маску к изображению: 0 — обнулить, 1 — оставить
    void applyKeepMask(vtkImageData* image, const std::vector<uint8_t>& keepMask) const;

    // быстрый поиск ближайшего непустого соседа
    bool findNearestNonEmptyConnectedVoxel(vtkImageData* image, const int seed[3], int outIJK[3]) const;

private:
    enum class State { Off, WaitingClick};
    State m_state{ State::Off };

    QWidget* m_host = nullptr;
    QWidget* m_overlay{ nullptr };
    QVector<QPoint> m_pts;

    QVTKOpenGLNativeWidget* m_vtk = nullptr;
    vtkRenderer* m_renderer = nullptr;
    vtkVolume* m_volume = nullptr;

    vtkImageData* m_image;
    Volume m_vol;
    Volume m_bin;

    Action m_mode;

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

    int m_hoverRadiusVoxels{ 0 };
    void ensureHoverPipeline();
    void updateHover(const QPoint& pDevice);
    void setHoverVisible(bool on);

};