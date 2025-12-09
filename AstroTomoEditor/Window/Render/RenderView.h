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

enum class ViewPreset { AP, PA, LAO, RAO, L, R };

class RenderView : public QWidget
{
    Q_OBJECT
public:
    explicit RenderView(QWidget* parent = nullptr);
    ~RenderView() override;
    void setVolume(vtkSmartPointer<vtkImageData> image, DicomInfo Dicom);
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

signals:
    void renderStarted();
    void renderProgress(int processed);
    void renderFinished();
    void showInfo(const QString& text);
    void showWarning(const QString& text);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;

private slots:
    void onBuildStl();
    void onSaveBuiltStl();
    void onStlSimplify();
    void onUndo();
    void onRedo();
    void openHistogram();
    void onShiftChanged(int val);

private:
    vtkSmartPointer<vtkImageData> mImage;
    QVTKOpenGLNativeWidget* mVtk{ nullptr };
    vtkSmartPointer<vtkRenderer> mRenderer;
    vtkSmartPointer<vtkRenderWindow> mWindow;
    vtkSmartPointer<vtkVolume> mVolume;
    vtkSmartPointer<vtkOrientationMarkerWidget> mOrMarker;
    vtkSmartPointer<vtkAxesActor> mAxes;

    QWidget* mRightOverlay{ nullptr };

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
    bool mPrevVolumeVisible = true;

    QWidget* mTopOverlay{ nullptr };

    QMenu* mTfMenu{ nullptr };
    vtkSmartPointer<vtkVolumeProperty> mProp;
    QToolButton* mBtnTF{ nullptr };
    QPointer<TransferFunctionEditor>   mTfEditor;

    QVector<TF::CustomPreset> mCustom;
    void reloadTfMenu();
    void reloadHistogram();
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

    WheelSpinButton* mBtnShift{ nullptr };
    int mShiftValue = 3;

    QToolButton* mBtnTemplate{ nullptr };
    QToolButton* mBtnElectrod{ nullptr };

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

    bool mGradientOpacityOn = false;
    void updateGradientOpacity();
};
