#pragma once
#include <QObject>
#include <functional>
#include <memory>
#include <vector>

#include <vtkSmartPointer.h>
#include <QPoint>

#include "U8Span.h"
#include <vtkSphereSource.h>
#include <vtkImplicitPolyDataDistance.h>
#include <QPolygon.h>

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

enum class HoverMode {
    Default,
    None
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
    void Unsuccessful(std::function<void(vtkImageData*)> cb) { m_Unsuccessful = std::move(cb); }
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
    uint8_t ReturnAverageVisibleValue() { return AverageVisibleValue; }
    using StatusFn = std::function<void(const QString&)>;
    using ProgressFn = std::function<void(const int)>;
    void setStatusCallback(StatusFn fn) { mStatus = std::move(fn); }
    void setProgressCallback(ProgressFn fn) { mProgress = std::move(fn); }
    bool AddBy6Neighbors(Volume& volume, uint8_t fillVal);
   /* vtkImageData* ClearImage(QVTKOpenGLNativeWidget* vtk,
        vtkRenderer* renderer,
        vtkImageData* image,
        vtkVolume* volume);*/

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void onLeftClick(const QPoint& pDevice);
    void start(Action a, HoverMode hm);
    void redraw();

    // ядро
    void RecoveryNonVisibleVoxels(Volume& volume);
    void makeBinaryMask(vtkImageData* image);  // uchar 0/1
    bool screenToSeedIJK(const QPoint& pDevice, int ijk[3]) const;
    int  floodFill6(const Volume& bin, const int seed[3], std::vector<uint8_t>& mark) const;
    int floodFill6MultiSeed(const Volume& bin, const std::vector<size_t>& seeds, std::vector<uint8_t>& mark) const;
    void applyKeepOnlySelected(const std::vector<uint8_t>& mark);
    void applyRemoveSelected(const std::vector<uint8_t>& mark);
    void applyVoxelErase(const int seed[3]);
    void applyVoxelRecover(const int seed[3]);
    void RemoveConnectedRegions(const std::vector<uint8_t>& mark, const int seed[3], int steps = 1);
    void SmartDeleting(const int seed[3]);
    void MinusVoxels();
    void PlusVoxels();
    void AddBaseToBounds(const std::vector<uint8_t>& mark, const int seedIn[3]);
    void ErodeBy6Neighbors(Volume& volume);
   
    void FillEmptyRegions(const std::vector<uint8_t>& mark, const int seedIn[3]);
    void TotalSmoothingVolume();
    void PeelRecoveryVolume();
    void FindSurf(Volume& volNew, std::vector<uint8_t>& mark);
    void ConnectSurfaceToVolume(Volume& volNew, const std::vector<uint8_t>& mark, int shift);
    void SurfaceMappingVolume();
    bool pickSeedNearScreenPoint(const QPoint& p0, int outSeed[3]) const;

    uint8_t GetAverageVisibleValue();

    // world-ijk
    bool worldToIJK(const double world[3], int ijk[3]) const;
    void displayToWorld(double xd, double yd, double z01, double out[3]) const;

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
    HoverMode m_hm{ HoverMode::None };

    Volume m_orig;
    bool   m_hasOrig{ false };

    std::function<void(vtkImageData*)> m_onImageReplaced;
    std::function<void(vtkImageData*)> m_Unsuccessful;
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
    double mVisibleEps = 0.001; // порог по opacity

    void  rebuildVisibilityLUT(); // дергаем при attach() и смене TF / hist-mask
    bool  isVisible(double v) const; // быстрый тест через LUT

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
    void AddBaseBottomZ(Volume& vol, uint8_t shift);
    void AddBaseTopZ(Volume& vol, uint8_t shift);
    void AddBaseRightX(Volume& vol, uint8_t shift);
    void AddBaseLeftX(Volume& vol, uint8_t shift);
    void AddBaseFrontY(Volume& vol, uint8_t shift);
    void AddBaseBackY(Volume& vol, uint8_t shift);

    uint8_t AverageVisibleValue = 0;

    void status(const QString& s)
    {
        if (mStatus) mStatus(s);
    }
    void progress(const int p)
    {
        if (mProgress)
        {
            currentprogress = p;
            mProgress(currentprogress);
        }
    }
    void addprogress(const int p)
    {
        if (mProgress)
        {
            currentprogress += p;
            if (currentprogress >= 100)
                currentprogress = 99;
            mProgress(currentprogress);
        }
    }
    void addprogresstomax(const int p, const int max)
    {
        if (mProgress)
        {
            currentprogress += p;
            if (currentprogress >= max && max < 100)
                currentprogress = max;
            mProgress(currentprogress);
        }
    }

    int currentprogress = 0;
    StatusFn mStatus;
    ProgressFn mProgress;
};
