#pragma once
#include <QObject>
#include <functional>
#include <memory>
#include <vector>

#include <vtkSmartPointer.h>
#include <QPoint>

#include "U8Span.h"
#include <vtkSphereSource.h>

class QWidget;
class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkImageData;
class vtkVolume;

enum class Action; // из Tools.h

struct ShellVoxelInfo
{
    size_t idx;      // линейный индекс в объёме
    int i, j, k;     // локальные индексы 0..nx-1, 0..ny-1, 0..nz-1
    double n[3];     // нормаль (единичный вектор)
};


// Клик по объекту => оставить (или удалить) только его связную компоненту.
class ToolsRemoveConnected : public QObject
{
    Q_OBJECT
public:
    explicit ToolsRemoveConnected(QWidget* hostParent);
    ~ToolsRemoveConnected() override = default;

    // привязка к текущему виду
    void attach(QVTKOpenGLNativeWidget* vtk,
        vtkRenderer* renderer,
        vtkImageData* image,
        vtkVolume* volume);

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

    // диапазон гист-маски в физике (HU или что у тебя на оси гистограммы)
    void setHistogramMask(double lo, double hi)
    {
        mHistLo = lo;
        mHistHi = hi;
        rebuildVisibilityLUT();
    }

    // дергаем при изменении TF/OTF, чтобы пересчитать видимость
    void notifyTfChanged() { rebuildVisibilityLUT(); }
    void setHoverHighlightSizeVoxels(int r) { m_hoverRadiusVoxels = std::max(1, r); }
    void EnsureOriginalSnapshot(vtkImageData* _image);
    void ClearOriginalSnapshot();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void onLeftClick(const QPoint& pDevice);
    void start(Action a);
    void startnohover(Action a);
    void redraw();

    // ядро
    
    void makeBinaryMask(vtkImageData* image);  // uchar 0/1
    bool screenToSeedIJK(const QPoint& pDevice, int ijk[3]) const;
    int  floodFill6(const Volume& bin, const int seed[3], std::vector<uint8_t>& mark) const;
    void applyKeepOnlySelected(const std::vector<uint8_t>& mark);
    void applyRemoveSelected(const std::vector<uint8_t>& mark);
    void applyVoxelErase(const int seed[3]);
    void applyVoxelRecover(const int seed[3]);
    void RemoveConnectedRegions(const std::vector<uint8_t>& mark, const int seed[3]);
    void SmartDeleting(const int seed[3]);
    void MinusVoxels();
    void PlusVoxels();
    void ErodeBy6Neighbors(Volume& volume);
    bool AddBy6Neighbors(Volume& volume);

    // world↔ijk
    bool worldToIJK(const double world[3], int ijk[3]) const;
    void displayToWorld(double xd, double yd, double z01, double out[3]) const;
   

    // Удалить несвязанные области (оставить только выбранную компоненту)
    void removeUnconnected(const std::vector<uint8_t>& selMask);

    // Снять «кожуру» за один шаг (обнулить граничные воксели mask). Возвращает число единиц в outMask после шага.
    int peelOnce(const std::vector<uint8_t>& inMask,
        std::vector<uint8_t>& outMask,
        const int                   ext[6]) const;

    // Оставить только часть маски, связанную с seed (через floodFill6)
    int keepOnlyConnectedFromSeed(std::vector<uint8_t>& mask,
        const int             ext[6],
        const int             seed[3]) const;

    // Умное снятие кожуры (итерации + порог «резкого падения»). Возвращает число выполненных итераций.
    int smartPeel(std::vector<uint8_t>& mask,
        const std::vector<uint8_t>& selMask,
        const int                   ext[6],
        const int                   seed[3],
        double                      dropFrac,
        int                         maxIters) const;

    // «Recovery»: дилатация внутри исходной выбранной компоненты
    void recoveryDilate(std::vector<uint8_t>& mask,
        const std::vector<uint8_t>& selMask,
        const int                   ext[6],
        int                         iters) const;

    // Применить бинарную маску к изображению: 0 — обнулить, 1 — оставить
    void applyKeepMask(vtkImageData* image, const std::vector<uint8_t>& keepMask) const;

    // быстрый поиск ближайшего непустого соседа
    bool findNearestNonEmptyConnectedVoxel(vtkImageData* image,
        const int      seed[3],
        int            outIJK[3]) const;

    // Найти "кожуру" (6-связная граница) вокселей в заданном объёме.
    // В shell возвращаются линейные индексы (как в Volume::at(size_t)).
    void CollectShellVoxels(const Volume& vol,
        std::vector<size_t>& shell) const;
        
    double ClearingVolume(Volume& vol,
        const int seedIn[3],
        double percent);


private:
    enum class State { Off, WaitingClick };
    State   m_state{ State::Off };

    QWidget* m_host = nullptr;
    QWidget* m_overlay = nullptr;
    QVector<QPoint> m_pts;

    QVTKOpenGLNativeWidget* m_vtk = nullptr;
    vtkRenderer* m_renderer = nullptr;

    vtkSmartPointer<vtkImageData> m_image;
    vtkSmartPointer<vtkVolume>    m_volume;

    Volume        m_vol;
    Volume        m_bin;

    Action m_mode{};

    Volume m_orig;
    bool   m_hasOrig{ false };

    std::function<void(vtkImageData*)> m_onImageReplaced;
    std::function<void()>              m_onFinished;

    bool m_allowNav{ true }; // по умолчанию — включено
    void forwardMouseToVtk(QEvent* e); // проброс в QVTK
    void ijkToWorld(const int ijk[3], double world[3]) const;

    // LUT видимости по интенсивности (в том же диапазоне, что и LUT TF/OTF)
    std::vector<uint8_t> mVisibleLut;  // 0/1 по интенсивности

    // гист-маска в физике (HU, и т.п.), больше не режем до uint8
    double mHistLo{ 0.0 };
    double mHistHi{ 0.0 };

    double mLutMin = static_cast<double>(HistMin);
    double mLutMax = static_cast<double>(HistMax);
    int    mLutBins = HistScale;
    double mVisibleEps = 0.01; // порог по opacity

    void  rebuildVisibilityLUT(); // дергаем при attach() и смене TF / hist-mask
    bool  isVisible(double s) const; // быстрый тест через LUT

    QPoint m_lastMouse{};
    bool   m_hasHover{ false };
    int    m_hoverIJK[3]{ 0, 0, 0 };

    // hover-сфера для кисти
    vtkSmartPointer<class vtkSphereSource>    mBrushSphere;
    vtkSmartPointer<class vtkPolyDataMapper>  mBrushMapper;
    vtkSmartPointer<class vtkActor>           mBrushActor;


    // 3D-подсветка выбранного вокселя
    vtkSmartPointer<class vtkExtractVOI>        mHoverVOI;
    vtkSmartPointer<class vtkOutlineFilter>     mHoverOutline;
    vtkSmartPointer<class vtkPolyDataMapper>    mHoverMapper;
    vtkSmartPointer<class vtkActor>             mHoverActor;

    int m_hoverRadiusVoxels{ 0 };
    void ensureHoverPipeline();
    void updateHover(const QPoint& pDevice);
    void setHoverVisible(bool on);
};
