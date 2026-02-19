#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QPointer>
#include <vtkSmartPointer.h>
#include <QToolButton>
#include <QLabel>
#include <QTimer>
#include <vtkBoxWidget2.h>
#include "ClipBoxController.h"
#include "TransferFunction.h"
#include "TransferFunctionEditor.h"
#include "VolumeStlExporter.h"
#include "Tools.h"
#include "ToolsScissors.h"
#include <vtkRenderer.h>
#include <vtkVolume.h>
#include <vtkImageData.h>
#include "ToolsRemoveConnected.h"
#include "HistogramDialog.h"
#include <vtkDICOMReader.h>
#include <Services/DicomRange.h>
#include <QSpinBox>
#include "WheelSpinButton.h"
#include "U8Span.h"
#include "TemplateDialog.h"
#include "ElectrodePanel.h"
#include <algorithm>

class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkRenderWindow;
class vtkVolume;
class vtkImageData;
class vtkOrientationMarkerWidget;
class vtkAxesActor;
class QToolButton;
class QWidget;
class QMenu;
class TransferFunctionEditor;
class vtkActor;
class HistogramDialog;
class VolumeSurfaceFinder;
class vtkPolyData;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;
class vtkVolumeProperty;
class TemplateDialog;

enum class ViewPreset { AP, PA, LAO, RAO, L, R };

static constexpr const char* kGradOpacityKey = "render/gradientOpacity";
static constexpr const char* kInterpKey = "render/volumeInterpolation"; // 0 nearest, 1 linear

class RenderView : public QWidget
{
    Q_OBJECT
public:
    explicit RenderView(QWidget* parent = nullptr);
    ~RenderView() override;
    void setVolume(vtkSmartPointer<vtkImageData> image, DicomInfo Dicom, PatientInfo info);
    void setViewPreset(ViewPreset v);
    void centerOnVolume();
    void hideOverlays();
    int GetShift() { return mShiftValue; };

    QVTKOpenGLNativeWidget* vtkWidget() const { return mVtk; }
    vtkRenderer* renderer() const { return mRenderer; }
    vtkVolume* volume() const { return mVolume; }
    vtkImageData* image() const { return mImage; }
    void setImage(vtkSmartPointer<vtkImageData> img);
    DicomInfo GetDicomInfo() const { return DI; };

    enum class VolumeInterpolation { Nearest = 0, Linear = 1 };

    void setGradientOpacityEnabled(bool on);
    void setVolumeInterpolation(VolumeInterpolation m);
    void saveTemplates(QString savedir);

signals:
    void renderStarted();
    void renderProgress(int processed);
    void Progress(int p);
    void renderFinished();
    void showInfo(const QString& text);
    void showWarning(const QString& text);
    void gradientOpacityChanged(bool on);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void changeEvent(QEvent* e) override;

private slots:
    void onBuildStl();
    void onSaveBuiltStl();
    void onStlSimplify();
    void onUndo();
    void onRedo();
    void openHistogram();
    void openTemplate();
    void onShiftChanged(int val);
    void onTemplateCapture(TemplateId id);
    void onTemplateSetVisible(TemplateId id, bool on);
    void onTemplateClear(TemplateId id);
    void onTemplateClearAll();
    void onTemplateClearScene();

private:
    vtkSmartPointer<vtkImageData> mImage;
    QVTKOpenGLNativeWidget* mVtk{ nullptr };
    vtkSmartPointer<vtkRenderer> mRenderer;
    vtkSmartPointer<vtkRenderWindow> mWindow;
    vtkSmartPointer<vtkVolume> mVolume;
    vtkSmartPointer<vtkOrientationMarkerWidget> mOrMarker;
    vtkSmartPointer<vtkAxesActor> mAxes;
    vtkSmartPointer<vtkImageData> mVisibleMask;

    QWidget* mRightOverlay{ nullptr };
    QWidget* mTopOverlay{ nullptr };
    QWidget* mElectrodeOverlay{ nullptr };

    QToolButton* mBtnAP{ nullptr };
    QToolButton* mBtnPA{ nullptr };
    QToolButton* mBtnL{ nullptr };
    QToolButton* mBtnR{ nullptr };
    QToolButton* mBtnLAO{ nullptr };
    QToolButton* mBtnRAO{ nullptr };

    std::unique_ptr<ClipBoxController> mClip;
    QToolButton* mBtnClip{ nullptr };

    QToolButton* mBtnSTL{ nullptr };
    QToolButton* mBtnSTLSimplify{ nullptr };
    QToolButton* mBtnSTLSave{ nullptr };
    QLabel* mLblStlSize{ nullptr };
    bool mPrevVolumeVisible = true;
    double mSimplifyTargetMB = 3.0;
    bool   mSimplifyStarted = false;

    QMenu* mTfMenu{ nullptr };
    vtkSmartPointer<vtkVolumeProperty> mProp;
    QToolButton* mBtnTF{ nullptr };
    QPointer<TransferFunctionEditor>   mTfEditor;
    QVector<TF::CustomPreset> mCustom;
    void reloadTfMenu();
    void reloadHistogram();
    void reloadTemplate();
    void reloadElectrodes();
    void updateAfterImageChange(bool reattachTools);

    bool mOverlaysBuilt{ false };
    bool mOverlaysShown{ false };

    QVector<vtkSmartPointer<vtkActor>> mStlActors;

    vtkSmartPointer<vtkPolyData> mIsoMesh = nullptr;
    vtkSmartPointer<vtkActor>    mIsoActor = nullptr;

    QToolButton* mBtnTools{ nullptr };
    QMenu* mToolsMenu{ nullptr };

    std::unique_ptr<ToolsScissors> mScissors;
    std::unique_ptr<ToolsRemoveConnected> mRemoveConn;

    void buildOverlay();
    void repositionOverlay();
    void showOverlays();
    bool applyPreset(TFPreset p);
    bool ToolModeChanged(Action a);
    bool AppModeChanged(App a);

    void ensureTemplateDialog();
    void rebuildVisibleMaskFromImage(vtkImageData* src);

    void ensureElectrodPanel();

    bool mToolActive{ false };
    Action mCurrentTool{};
    void setToolUiActive(bool on, Action a);

    QToolButton* mBtnUndo{ nullptr };
    QToolButton* mBtnRedo{ nullptr };

    QVector<vtkSmartPointer<vtkImageData>> mUndoStack;
    QVector<vtkSmartPointer<vtkImageData>> mRedoStack;
    int  mHistoryLimit = 20;
    vtkSmartPointer<vtkImageData> cloneImage(vtkImageData* src);
    void commitNewImage(vtkImageData* im);
    void setMapperInput(vtkImageData* im);
    void updateUndoRedoUi();
    void applyCustomPresetByIndex(int idx, vtkVolumeProperty* prop, double dataMin, double dataMax);

    QToolButton* mBtnApps{ nullptr };
    QMenu* mAppsMenu{ nullptr };

    bool mAppActive{ false };
    App mCurrentApp{};
    void setAppUiActive(bool on, App a);

    QPointer<HistogramDialog> mHistDlg;
    vtkSmartPointer<vtkColorTransferFunction> mBaseCTF;
    vtkSmartPointer<vtkPiecewiseFunction>     mBaseOTF;

    QPointer<TemplateDialog> mTemplateDlg;
    std::unordered_map<TemplateId, Volume> mTemplateVolumes;
    void applyTemplateLayer(TemplateId id, bool visible);
    void removeTemplateLayer(TemplateId id);
    void removeAllTemplateLayers();

    WheelSpinButton* mBtnShift{ nullptr };
    int mShiftValue = 3;

    ElectrodePanel* mElectrodePanel{ nullptr };
    void setElectrodesUiActive(bool on);
    void updateElectrodeOverlayMask();
    void updateElectrodePickContext();
    void onSaveElectrodesCoords();

    QHash<ElectrodePanel::ElectrodeId, std::array<int, 3>> mElectrodeIJK;
    void captureElectrodesTemplateFromCurrentVolume();
    void beginElectrodesPreview();
    void endElectrodesPreview();
    vtkSmartPointer<vtkImageData> mImageBeforeElectrodes;
    vtkSmartPointer<vtkImageData> mElectrodesPreviewImage;
    vtkSmartPointer<vtkColorTransferFunction> mElectrodesPreviewSavedCTF;
    vtkSmartPointer<vtkPiecewiseFunction> mElectrodesPreviewSavedOTF;
    bool mElectrodesPreviewActive{ false };

    bool   mHistMaskActive{ false };
    double mHistMaskLo{ static_cast<double>(HistMin) };
    double mHistMaskHi{ static_cast<double>(HistMax) };

    double DataMin = static_cast<double>(HistMin);
    double DataMax = static_cast<double>(HistMax);

    int mCustomCtIndex = -1;
    int mCustomMrIndex = -1;
    
    DicomInfo DI;

    QStringList mCtKeys = { "CT", };
    QStringList mMrKeys = { "MR", "MRI" };

    void openTfEditor();

    void addStlPreview();
    void clearStlPreview();
    void updateStlSizeLabel();

    bool mGradientOpacityOn = false;
    void updateGradientOpacity();
    
    void retranslateUi();
    void reloadToolsMenu();
    void reloadAppsMenu();
    void updateToolCaptionFromState();
    void updateAppCaptionFromState();

    void loadRenderSettings();
    void saveRenderSettings();
    VolumeInterpolation mInterpolation = VolumeInterpolation::Nearest;
};

static constexpr double kMB = 1024.0 * 1024.0;
static constexpr double kFirstMinMB = 2.0;
static constexpr double kFirstMaxMB = 4.0;
static constexpr double kFirstAimMB = 3.0;
static constexpr double kStepFactor = 0.85;
static constexpr double kMinMB = 0.5;
