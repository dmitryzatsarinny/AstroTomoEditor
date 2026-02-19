#pragma once
#include <QWidget>
#include <QVector>
#include <QHash>
#include <QEvent>
#include <QButtonGroup>
#include <array>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkVolumeProperty.h>
#include <vtkPiecewiseFunction.h>
#include <vtkBillboardTextActor3D.h>
#include <Services/DicomRange.h>

class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkVolume;
class QToolButton;
class QPushButton;
class vtkImageData;
class vtkVolumeProperty;

class ElectrodePanel : public QWidget
{
    Q_OBJECT
public:
    enum class ElectrodeId : int {
        L = 0, R, F, N,
        V1, V2, V3, V4, V5,
        V6, V7, V8, V9, V10,
        V11, V12, V13, V14, V15,
        V16, V17, V18, V19, V20,
        V21, V22, V23, V24, V25,
        V26, V27, V28, V29, V30,
        Count
    };

    struct PickContext
    {
        QVTKOpenGLNativeWidget* vtkWidget = nullptr;
        vtkRenderer* renderer = nullptr;
        vtkImageData* image = nullptr;          // mImage->raw() или что у тебя
        vtkVolume* volume = nullptr;            // если нужно
        vtkVolumeProperty* volProp = nullptr;   // главное: opacity function
    };

    void setPickContext(const PickContext& ctx);

    explicit ElectrodePanel(QWidget* parent = nullptr);

    void SetDicomInfo(DicomInfo di) { DI = di; }

    void setModeEnabled(bool on);
    bool isModeEnabled() const { return mEnabled; }

    // состояние кнопок со стороны RenderView
    void setHasCoord(ElectrodeId id, bool has);
    bool hasCoord(ElectrodeId id) const;
    void setCurrent(ElectrodeId id);       // выделить кнопку как активную (ждём клик)
    void clearCurrent();                   // снять выделение

    void retranslateUi();

    struct ElectrodeCoord
    {
        ElectrodeId id{};
        std::array<double, 3> world{};
    };

    struct ElectrodeIJKCoord
    {
        ElectrodeId id{};
        std::array<int, 3> vox{};
    };

    QVector<ElectrodeCoord> coordsWorld() const;
    QVector<ElectrodeIJKCoord> coordsIJK() const;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;
    
signals:
    void requestExit(); // Back/закрыть
    void electrodeChosen(ElectrodeId id);  // нажали на кнопку электрода (перешли в ожидание клика)
    void electrodeClearRequested(ElectrodeId id); // нажали крестик
    void saveRequested(); // сохранить координаты
    void pickCommitted(ElectrodeId id, std::array<int, 3> ijk, std::array<double, 3> world);

public slots:
    void beginPick(ElectrodeId id);   // пользователь нажал кнопку электрода
    void endPick();

private:
    class ElectrodeButton; // ниже в cpp
    void buildUi();
    void rebuildMask();

    bool pickAt(const QPoint& pDevice, std::array<int, 3>& outIJK, std::array<double, 3>& outW) const;
    bool displayRay(const QPoint& pDevice, double outP0[3], double outP1[3]) const;
    bool worldToIJK(const double w[3], int ijk[3]) const;
    double opacityAtIJK(int ijk[3]) const;

    void ensureHoverActor();
    void setHoverVisible(bool v);
    void setHoverAtWorld(const double w[3]);

    bool mEnabled{ false };

    QVector<ElectrodeButton*> mButtons;
    QHash<ElectrodeId, ElectrodeButton*> mById;
    QButtonGroup* mGroup = nullptr;

    QPushButton* mBtnSave = nullptr;

    ElectrodeId mCurrent{ ElectrodeId::Count }; // none

    PickContext mPick;

    DicomInfo DI;

    bool mPicking = false;
    ElectrodeId mPickId = ElectrodeId::Count;

    // hover actor (кружок)
    vtkSmartPointer<vtkSphereSource> mHoverSphere;
    vtkSmartPointer<vtkPolyDataMapper> mHoverMapper;
    vtkSmartPointer<vtkActor> mHoverActor;

    struct GlobalVox
    {
        double original = 0.0;
        int ref = 0;
    };

    struct Marker3D
    {
        vtkSmartPointer<vtkSphereSource> sphere;
        vtkSmartPointer<vtkPolyDataMapper> sphereMapper;
        vtkSmartPointer<vtkActor> sphereActor;

        vtkSmartPointer<vtkBillboardTextActor3D> textActor;
    };

    QHash<ElectrodeId, Marker3D> mMarkers;
    QHash<vtkIdType, GlobalVox> mGlobalCut;                 // pid -> {original, ref}
    QHash<ElectrodeId, QVector<vtkIdType>> mCutByElectrode; // id -> список pid, которые этот электрод "держит"
    QHash<ElectrodeId, std::array<int, 3>> mCutCenterIJK;    // центр для повторного вырезания
    QHash<ElectrodeId, bool> mCutVisible;                   // показываем ли вырез сейчас
    QHash<ElectrodeId, std::array<double, 3>> mCutCenterWorld;

    int mEraseRadiusVox = 10;
    double mEraseMin = 240.0;
    double mEraseMax = 255.0;
    double mEraseOpThr = 0.01;

    void toggleCut(ElectrodeId id);
    void applyCut(ElectrodeId id, const std::array<int, 3>& cIJK);
    void restoreCut(ElectrodeId id);
    void requestRender();

    void ensureMarker(ElectrodeId id);
    void setMarkerVisible(ElectrodeId id, bool vis);
    void updateMarker(ElectrodeId id, const std::array<double, 3>& w, const std::array<int, 3>& ijk);
    void clearAllElectrodesState();
    bool snapToSurfaceTowardsCamera(const std::array<double, 3>& w0, std::array<int, 3>& inOutIJK, std::array<double, 3>& outW) const;
};
