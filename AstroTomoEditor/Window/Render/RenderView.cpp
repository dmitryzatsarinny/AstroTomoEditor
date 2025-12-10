#include "RenderView.h"
#include <QVTKOpenGLNativeWidget.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkPointData.h>
#include <vtkDataArray.h> 
#include <vtkAutoInit.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkAxesActor.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkMatrix3x3.h>
#include <QShortcut>
#include <QKeySequence>
#include "Human.h"
#include "MouseControl.h"
#include <QApplication>
#include <QMenu>
#include <QAction>
#include <vtkImagePermute.h>
#include "VolumeStlExporter.h"
#include <QFileDialog>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkPolyDataNormals.h>
#include <Services/DicomRange.h>
#include <QTimer>
#include <QLineEdit>
#include <vtkFlyingEdges3D.h>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

static vtkMatrix3x3* fetchDirectionMatrix(vtkImageData* img, vtkNew<vtkMatrix3x3>& holder)
{
    if (auto* M = img->GetDirectionMatrix())
        return M;
    holder->Identity();
    return holder.GetPointer();
}

static void getPatientAxes(vtkImageData* img, double L[3], double P[3], double S[3])
{
    vtkNew<vtkMatrix3x3> keep;
    vtkMatrix3x3* M = fetchDirectionMatrix(img, keep);

    // столбцы — оси данных в мире (LPS)
    double I[3]{ M->GetElement(0,0), M->GetElement(1,0), M->GetElement(2,0) }; // вдоль индекса i
    double J[3]{ M->GetElement(0,1), M->GetElement(1,1), M->GetElement(2,1) }; // вдоль j
    double K[3]{ M->GetElement(0,2), M->GetElement(1,2), M->GetElement(2,2) }; // вдоль k

    auto norm = [](double v[3]) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
        };
    norm(I); norm(J); norm(K);

    // приводим направления к положительным L,P,S, но сохраняем правосторонность:
    const double eL[3]{ +1,0,0 }, eP[3]{ 0,+1,0 }, eS[3]{ 0,0,+1 };
    auto sgn = [](double x) { return x >= 0 ? 1.0 : -1.0; };
    const double sI = sgn(I[0]); for (int t = 0; t < 3; ++t) I[t] *= sI;       // к +L
    const double sJ = sgn(J[1]); for (int t = 0; t < 3; ++t) J[t] *= sJ;       // к +P

    // восстанавливаем K как cross(I,J), чтобы детерминант был +1
    K[0] = I[1] * J[2] - I[2] * J[1];
    K[1] = I[2] * J[0] - I[0] * J[2];
    K[2] = I[0] * J[1] - I[1] * J[0];
    norm(K);
    if (K[2] < 0) { for (int t = 0; t < 3; ++t) K[t] *= -1.0; }                // к +S

    // возвращаем как пациентские оси
    L[0] = I[0]; L[1] = I[1]; L[2] = I[2];
    P[0] = J[0]; P[1] = J[1]; P[2] = J[2];
    S[0] = K[0]; S[1] = K[1]; S[2] = K[2];
}

static void applyMenuStyle(QMenu* m, int width = 180)
{
    m->setAttribute(Qt::WA_StyledBackground, true);
    m->setAttribute(Qt::WA_TranslucentBackground, false);
    m->setAutoFillBackground(true);
    m->setFixedWidth(width);

    m->setStyleSheet(
        "QMenu{"
        "  background:rgba(22,22,22,0.96);"
        "  border:1px solid rgba(255,255,255,0.18);"
        "  border-radius:10px;"
        "  padding:6px;"
        "}"
        "QMenu::separator{"
        "  height:1px; background:rgba(255,255,255,0.12);"
        "  margin:6px 8px;"
        "}"
        "QMenu::item{"
        "  color:#fff; padding:6px 10px;"
        "  border-radius:6px;"
        "}"
        "QMenu::item:selected{"
        "  background:rgba(255,255,255,0.12);"
        "}"
        "QMenu::icon{ margin-right:8px; }"
    );
}

RenderView::RenderView(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    mVtk = new QVTKOpenGLNativeWidget(this);
    mVtk->setAttribute(Qt::WA_TranslucentBackground, true);
    mVtk->setStyleSheet("background: transparent;");
    lay->addWidget(mVtk);

    // 2) создаём render window и привязываем к виджету
    mWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    mVtk->setRenderWindow(mWindow);

    // 3) создаём renderer и добавляем в окно
    mRenderer = vtkSmartPointer<vtkRenderer>::New();
    mWindow->AddRenderer(mRenderer);

    auto* iren = mVtk->interactor();
    auto style = vtkSmartPointer<InteractorStyleCtrlWheelShiftPan>::New();
    iren->SetInteractorStyle(style);

    auto sc = new QShortcut(QKeySequence(Qt::Key_F12), this);
    connect(sc, &QShortcut::activated, this, &RenderView::centerOnVolume);

    mOrMarker = vtkSmartPointer<vtkOrientationMarkerWidget>::New();
    mOrMarker->SetOrientationMarker(MakeHumanMarker());
    mOrMarker->SetInteractor(mVtk->interactor());
    mOrMarker->SetViewport(0.00, 0.025, 0.15, 0.225);
    mOrMarker->EnabledOn();
    mOrMarker->InteractiveOff();

    // фон/настройки по вкусу
    mRenderer->SetBackground(0.06, 0.06, 0.07);

    // создаём overlay после VTK
    buildOverlay();

    mClip = std::make_unique<ClipBoxController>(this);
    mClip->setRenderer(mRenderer);
    mClip->setInteractor(mVtk->interactor());

    connect(mBtnClip, &QToolButton::toggled, this, [this](bool on) { mClip->setEnabled(on); });
    connect(mBtnSTL, &QToolButton::clicked, this, &RenderView::onBuildStl);
    connect(mBtnSTLSimplify, &QToolButton::clicked, this, &RenderView::onStlSimplify);
    connect(mBtnSTLSave, &QToolButton::clicked, this, &RenderView::onSaveBuiltStl);
}

RenderView::~RenderView() {
    if (mScissors) { mScissors->setOnFinished(nullptr); }
    if (mRemoveConn) { mRemoveConn->setOnFinished(nullptr); }
    if (mScissors)    mScissors->cancel();
    if (mRemoveConn)  mRemoveConn->cancel();
    mScissors.reset();
    mRemoveConn.reset();
    clearStlPreview();
}

static QWidget* makeNativeOverlay(QWidget* owner)
{
    auto* w = new QWidget(owner);
    w->setAttribute(Qt::WA_TranslucentBackground, true);
    w->setAttribute(Qt::WA_NoSystemBackground, true);
    w->setAttribute(Qt::WA_ShowWithoutActivating, true);
    w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    w->setFocusPolicy(Qt::NoFocus);
    w->setMouseTracking(true);

    return w;
}

void RenderView::buildOverlay()
{
    // ---------- ПРАВАЯ ПАНЕЛЬ ----------
    mRightOverlay = makeNativeOverlay(this);
    mRightOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* rightPanel = new QWidget(mRightOverlay);
    rightPanel->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* rv = new QVBoxLayout(rightPanel);
    rv->setContentsMargins(0, 42, 12, 0);
    rv->setSpacing(6);

    auto makeSmallBtn = [&](QWidget* parent, const QString& text) {
        auto* b = new QToolButton(parent);
        b->setText(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(44, 26);
        b->setStyleSheet(
            "QToolButton{ color:#fff; background:rgba(40,40,40,110);"
            " border:1px solid rgba(255,255,255,30); border-radius:6px; padding:0 8px; }"
            "QToolButton:hover{ background:rgba(255,255,255,40); }"
            "QToolButton:pressed{ background:rgba(255,255,255,70); }"
            "QToolButton:checked{ background:rgba(0,180,100,140); }"
        );
        return b;
        };

    auto makeBtn = [&](QWidget* parent, const QString& text) {
        auto* b = new QToolButton(parent);
        b->setText(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(62, 26);
        b->setStyleSheet(
            "QToolButton{ color:#fff; background:rgba(40,40,40,110);"
            " border:1px solid rgba(255,255,255,30); border-radius:6px; padding:0 8px; }"
            "QToolButton:hover{ background:rgba(255,255,255,40); }"
            "QToolButton:pressed{ background:rgba(255,255,255,70); }"
            "QToolButton:checked{ background:rgba(0,180,100,140); }"
        );
        return b;
        };

    auto makeMiddleBtn = [&](QWidget* parent, const QString& text) {
        auto* b = new QToolButton(parent);
        b->setText(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(126, 26);
        b->setStyleSheet(
            "QToolButton{ color:#fff; background:rgba(40,40,40,110);"
            " border:1px solid rgba(255,255,255,30); border-radius:6px; padding:0 8px; }"
            "QToolButton:hover{ background:rgba(255,255,255,40); }"
            "QToolButton:pressed{ background:rgba(255,255,255,70); }"
            "QToolButton:checked{ background:rgba(0,180,100,140); }"
        );
        return b;
        };

    auto makeBigBtn = [&](QWidget* parent, const QString& text) {
        auto* b = new QToolButton(parent);
        b->setText(text);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(146, 26);
        b->setStyleSheet(
            "QToolButton{ color:#fff; background:rgba(40,40,40,110);"
            " border:1px solid rgba(255,255,255,30); border-radius:6px; padding:0 8px; }"
            "QToolButton:hover{ background:rgba(255,255,255,40); }"
            "QToolButton:pressed{ background:rgba(255,255,255,70); }"
            "QToolButton:checked{ background:rgba(0,180,100,140); }"
        );
        return b;
        };


    mBtnAP = makeBtn(rightPanel, "AP");
    mBtnPA = makeBtn(rightPanel, "PA");
    mBtnL = makeBtn(rightPanel, "L");
    mBtnR = makeBtn(rightPanel, "R");
    mBtnLAO = makeBtn(rightPanel, "LAO");
    mBtnRAO = makeBtn(rightPanel, "RAO");

    for (auto* b : { mBtnAP, mBtnPA, mBtnL, mBtnR, mBtnLAO, mBtnRAO })
        rv->addWidget(b);

    auto* line = new QFrame(rightPanel);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("color: rgba(255,255,255,40);");
    rv->addWidget(line);

    mBtnClip = makeBtn(rightPanel, "Clip");
    mBtnClip->setCheckable(true);
    rv->addWidget(mBtnClip);
    rv->addStretch();

    mBtnSTL = makeBtn(rightPanel, "STL");
    mBtnSTL->setCheckable(true);
    rv->addWidget(mBtnSTL);

    mBtnSTLSimplify = makeBtn(rightPanel, "Simple");
    mBtnSTLSimplify->setEnabled(false);
    mBtnSTLSimplify->setVisible(false);
    auto pol = mBtnSTLSimplify->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mBtnSTLSimplify->setSizePolicy(pol);
    rv->addWidget(mBtnSTLSimplify);

    mBtnSTLSave = makeBtn(rightPanel, "Save");
    mBtnSTLSave->setEnabled(false);
    mBtnSTLSave->setVisible(false);
    pol = mBtnSTLSave->sizePolicy();
    pol.setRetainSizeWhenHidden(true);
    mBtnSTLSave->setSizePolicy(pol);
    rv->addWidget(mBtnSTLSave);


    rightPanel->adjustSize();
    mRightOverlay->resize(rightPanel->sizeHint());

    // ---------- ВЕРХНЯЯ ПАНЕЛЬ ----------
    mTopOverlay = makeNativeOverlay(this);
    mTopOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* topPanel = new QWidget(mTopOverlay);
    topPanel->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* th = new QHBoxLayout(topPanel);
    th->setContentsMargins(12, 8, 0, 0);
    th->setSpacing(6);

    mBtnTF = makeBigBtn(topPanel, "Transfer Function");
    mTfMenu = TF::CreateMenu(mTopOverlay, [this](TFPreset p) { applyPreset(p); });
    applyMenuStyle(mTfMenu, mBtnTF->width());
    QAction* actCustom = mTfMenu->addAction(tr("Custom…"));
    connect(actCustom, &QAction::triggered, this, &RenderView::openTfEditor);
    mBtnTF->setMenu(mTfMenu);
    mBtnTF->setPopupMode(QToolButton::InstantPopup);
    th->addWidget(mBtnTF);


    mBtnTools = makeBigBtn(topPanel, tr("Edit"));
    mBtnTools->setCheckable(true);


    mBtnTools->setStyleSheet(
        mBtnTools->styleSheet() +
        " QToolButton:checked{"
        "   background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "     stop:0 rgba(0,200,120,160), stop:1 rgba(0,160,90,140));"
        "   border:1px solid rgba(0,255,160,220);"
        "   border-radius:6px;"
        "   padding:0 8px;"
        "}"
        " QToolButton:checked:!pressed{"
        "   outline: none;"
        "}"
    );


    mToolsMenu = Tools::CreateMenu(mTopOverlay, [this](Action a) { ToolModeChanged(a); });
    applyMenuStyle(mToolsMenu, mBtnTools->width());

    connect(mToolsMenu, &QMenu::aboutToShow, this, [this] {
        if (!mToolActive) return;
        if (mScissors)
            mScissors->cancel();
        if (mRemoveConn)
            mRemoveConn->cancel();
        setToolUiActive(false, mCurrentTool);
        });

    mBtnTools->setMenu(mToolsMenu);
    mBtnTools->setPopupMode(QToolButton::InstantPopup);
    th->addWidget(mBtnTools);
    
    mBtnShift = new WheelSpinButton(topPanel);
    mBtnShift->setRange(1, 99);
    mBtnShift->setValue(mShiftValue);
    mBtnShift->setWheelStep(1);        // обычный шаг
    th->addWidget(mBtnShift);

    connect(mBtnShift, QOverload<int>::of(&QSpinBox::valueChanged),
        this, [this](int v) {
            onShiftChanged(v);
        });


    mBtnApps = makeMiddleBtn(topPanel, tr("Applications"));
    mBtnApps->setCheckable(true);


    mBtnApps->setStyleSheet(
        mBtnApps->styleSheet() +
        " QToolButton:checked{"
        "   background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "     stop:0 rgba(0,200,120,160), stop:1 rgba(0,160,90,140));"
        "   border:1px solid rgba(0,255,160,220);"
        "   border-radius:6px;"
        "   padding:0 8px;"
        "}"
        " QToolButton:checked:!pressed{"
        "   outline: none;"
        "}"
    );


    mAppsMenu = Tools::CreateAppMenu(mTopOverlay, [this](App a) { AppModeChanged(a); });
    applyMenuStyle(mAppsMenu, mBtnApps->width());

    connect(mAppsMenu, &QMenu::aboutToShow, this, [this] {
        if (!mAppActive) return;
        if (mHistDlg)
            mHistDlg->close();
        setAppUiActive(false, mCurrentApp);
        });

    mBtnApps->setMenu(mAppsMenu);
    mBtnApps->setPopupMode(QToolButton::InstantPopup);
    th->addWidget(mBtnApps);

    {
        mBtnUndo = makeSmallBtn(topPanel, "");
        mBtnUndo->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mBtnUndo->setIcon(QIcon(":/icons/Resources/undo-no.svg"));
        mBtnUndo->setIconSize(QSize(20, 20));
        mBtnUndo->setToolTip(tr("Undo (Ctrl+Z)"));
        mBtnUndo->setEnabled(false);
        th->addWidget(mBtnUndo);

        mBtnRedo = makeSmallBtn(topPanel, "");
        mBtnRedo->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mBtnRedo->setIcon(QIcon(":/icons/Resources/redo-no.svg"));
        mBtnRedo->setIconSize(QSize(20, 20));
        mBtnRedo->setToolTip(tr("Redo (Ctrl+Y)"));
        mBtnRedo->setEnabled(false);
        th->addWidget(mBtnRedo);


        connect(mBtnUndo, &QToolButton::clicked, this, &RenderView::onUndo);
        connect(mBtnRedo, &QToolButton::clicked, this, &RenderView::onRedo);
        auto* scUndo = new QShortcut(QKeySequence::Undo, this);
        auto* scRedo = new QShortcut(QKeySequence::Redo, this);
        connect(scUndo, &QShortcut::activated, this, &RenderView::onUndo);
        connect(scRedo, &QShortcut::activated, this, &RenderView::onRedo);
    }

    auto* scHist = new QShortcut(QKeySequence(Qt::Key_H), this);
    connect(scHist, &QShortcut::activated, this, [this] {
        if (!mImage || !mVolume)
            return;

        // помечаем, что активен режим Histogram, чтобы кнопка "Applications" тоже подсветилась
        setAppUiActive(true, App::Histogram);
        openHistogram();
        });

    auto* scHistAuto = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this);
    connect(scHistAuto, &QShortcut::activated, this, [this] {
        if (!mImage)
            return;

        if (!mHistDlg)
            return;

        // и вызываем нужную функцию
        mHistDlg->HideAutoRange(mImage);
        });

    auto* scGradOpacity = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this);
    connect(scGradOpacity, &QShortcut::activated, this, [this] {
        if (!mVolume)
            return;

        mGradientOpacityOn = !mGradientOpacityOn;
        updateGradientOpacity();
        });

    topPanel->adjustSize();
    mTopOverlay->resize(topPanel->sizeHint());
    
    th->addStretch();

    // ---------- Сигналы ----------
    connect(mBtnAP, &QToolButton::clicked, this, [this] { setViewPreset(ViewPreset::AP);  });
    connect(mBtnPA, &QToolButton::clicked, this, [this] { setViewPreset(ViewPreset::PA);  });
    connect(mBtnL, &QToolButton::clicked, this, [this] { setViewPreset(ViewPreset::L);   });
    connect(mBtnR, &QToolButton::clicked, this, [this] { setViewPreset(ViewPreset::R);   });
    connect(mBtnLAO, &QToolButton::clicked, this, [this] { setViewPreset(ViewPreset::LAO); });
    connect(mBtnRAO, &QToolButton::clicked, this, [this] { setViewPreset(ViewPreset::RAO); });


    repositionOverlay();

    mOverlaysBuilt = true;
}

static vtkSmartPointer<vtkPiecewiseFunction>
BuildMaskedOTF(vtkPiecewiseFunction* base, double lo, double hi)
{
    vtkNew<vtkPiecewiseFunction> out;
    if (!base || hi < lo) return out;

    // 1) домен базы (тот же, что у изображения: HU для КТ)
    double dom[2]{ 0,0 };
    if (base->GetSize() >= 2) {
        double n0[4], n1[4];
        base->GetNodeValue(0, n0);
        base->GetNodeValue(base->GetSize() - 1, n1);
        dom[0] = n0[0]; dom[1] = n1[0];
    }
    else {
        dom[0] = lo; dom[1] = hi;
    }

    // 2) пересекаем окно с доменом
    const double eps = 1e-6;
    lo = std::max(lo, dom[0]);
    hi = std::min(hi, dom[1]);

    // 3) «ноль» слева
    out->AddPoint(dom[0], 0.0);
    if (lo > dom[0]) out->AddPoint(lo - eps, 0.0);

    // значение на входной границе
    const double vLo = std::clamp(base->GetValue(lo), 0.0, 1.0);
    out->AddPoint(lo, vLo);

    // 4) копируем узлы внутри окна
    for (int i = 0, n = base->GetSize(); i < n; ++i) {
        double node[4]; base->GetNodeValue(i, node);
        const double x = node[0];
        if (x < lo || x > hi) continue;
        out->AddPoint(x, std::clamp(node[1], 0.0, 1.0), node[2], node[3]);
    }

    // 5) правый край + «ноль» справа
    const double vHi = std::clamp(base->GetValue(hi), 0.0, 1.0);
    out->AddPoint(hi, vHi);
    if (hi < dom[1]) out->AddPoint(hi + eps, 0.0);
    out->AddPoint(dom[1], 0.0);

    return out;
}

void RenderView::onShiftChanged(int val)
{
    mShiftValue = val;
    if (mRemoveConn)
        mRemoveConn->setHoverHighlightSizeVoxels(mShiftValue);
}

void RenderView::openHistogram()
{
    if (!mImage || !mVolume) return;

    mHistDlg->show();
    mHistDlg->raise();
    mHistDlg->activateWindow();
    mHistDlg->refreshFromImage(mImage);
}

void RenderView::reloadHistogram()
{
    if (!mImage || !mVolume) return;

    if (mHistDlg)
    {
        mHistDlg->close();
        mHistDlg = nullptr;
    }

    if (!mHistDlg) {

        mHistDlg = new HistogramDialog(this, DI, mImage);
        mHistDlg->setFixedAxis(true, DI.RealMin, DI.RealMax);

        // 1) при первом открытии — снять копии базовых функций
        if (mVolume && !mHistMaskActive) {
            auto* prop = mVolume->GetProperty();
            if (prop) {
                mBaseCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
                mBaseOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
                if (auto* c = prop->GetRGBTransferFunction(0)) mBaseCTF->DeepCopy(c);
                if (auto* o = prop->GetScalarOpacity(0))       mBaseOTF->DeepCopy(o);
            }
        }

        connect(mHistDlg, &HistogramDialog::rangeChanged, this,
            [this](int loBin, int hiBin)
            {
                if (!mVolume || !mBaseOTF || !mBaseCTF) return;

                mHistMaskActive = true;
                mHistMaskLo = loBin; 
                mHistMaskHi = hiBin;

                if (mRemoveConn) 
                    mRemoveConn->setHistogramMask(mHistMaskLo, mHistMaskHi);

                if (auto* prop = mVolume->GetProperty()) {
                    prop->SetColor(0, mBaseCTF);
                    auto otfMasked = BuildMaskedOTF(mBaseOTF, mHistMaskLo, mHistMaskHi); // физика (HU)
                    prop->SetScalarOpacity(0, otfMasked);
                    prop->Modified();
                }

                if (auto* m = mVolume->GetMapper()) m->Modified();
                if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
            });

        connect(mHistDlg, &QObject::destroyed, this, [this] {
            if (!mVolume || !mHistMaskActive || !mBaseCTF || !mBaseOTF) return;
            if (auto* prop = mVolume->GetProperty()) {
                prop->SetColor(0, mBaseCTF);
                prop->SetScalarOpacity(0, mBaseOTF);
                prop->Modified();
            }
            if (auto* m = mVolume->GetMapper()) m->Modified();
            if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
            mHistDlg = nullptr;
            });

        mHistDlg->setOnFinished([this]()
            {
                setAppUiActive(false, mCurrentApp);
            });

        mHistDlg->refreshFromImage(mImage);
    }
}

vtkSmartPointer<vtkImageData> RenderView::cloneImage(vtkImageData* src)
{
    if (!src) return nullptr;
    auto out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(src); // копируем и структуру, и все значения (в т.ч. multi-component)
    return out;
}

void RenderView::setMapperInput(vtkImageData* im)
{
    if (!mVolume || !mVolume->GetMapper()) return;
    if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mVolume->GetMapper()))
        gm->SetInputData(im);
    else
        mVolume->GetMapper()->SetInputDataObject(im);
    mVolume->Modified();
}

void RenderView::updateUndoRedoUi()
{
    if (!mBtnUndo || !mBtnRedo) return;
    mBtnUndo->setEnabled(!mUndoStack.isEmpty());
    mBtnRedo->setEnabled(!mRedoStack.isEmpty());
    mBtnUndo->setIcon(QIcon(!mUndoStack.isEmpty() ? ":/icons/Resources/undo-yes.svg"
        : ":/icons/Resources/undo-no.svg"));
    mBtnRedo->setIcon(QIcon(!mRedoStack.isEmpty() ? ":/icons/Resources/redo-yes.svg"
        : ":/icons/Resources/redo-no.svg"));
}

static vtkSmartPointer<vtkActor> MakeSurfaceActor(vtkPolyData* pd)
{
    if (!pd) return nullptr;

    vtkNew<vtkPolyDataNormals> normals;
    normals->SetInputData(pd);
    normals->ConsistencyOn();       // согласованные нормали
    normals->SplittingOff();        // не ломать по углам
    normals->ComputePointNormalsOn();
    normals->ComputeCellNormalsOff();
    normals->Update();

    vtkNew<vtkPolyDataMapper> mp;
    mp->SetInputConnection(normals->GetOutputPort());
    mp->ScalarVisibilityOff();

    vtkNew<vtkActor> ac;
    ac->SetMapper(mp);

    // ВНЕШНЯЯ сторона (front faces)
    auto* front = ac->GetProperty();
    front->SetInterpolationToPhong();
    front->SetColor(0.98, 0.99, 1.00);
    front->SetAmbient(0.12);
    front->SetDiffuse(0.88);
    front->SetSpecular(0.12);
    front->SetSpecularPower(24.0);
    front->SetSpecularColor(1.0, 1.0, 1.0);
    front->SetOpacity(0.86);           // чуть больше просвета
    front->BackfaceCullingOff();       // важное условие

    // Бек — светлее и со спекуляром
    vtkNew<vtkProperty> back;
    back->SetInterpolationToPhong();
    back->SetColor(0.75, 0.22, 0.20);  // светлее и теплее
    back->SetAmbient(0.20);            // «подсветка изнутри»
    back->SetDiffuse(0.85);
    back->SetSpecular(0.18);           // добавим блик
    back->SetSpecularPower(18.0);
    back->SetSpecularColor(1.0, 0.95, 0.95);
    back->SetOpacity(0.32);            // чтобы вклад был заметен

    ac->SetBackfaceProperty(back);

    return ac;
}


void RenderView::updateAfterImageChange(bool reattachTools)
{
    setMapperInput(mImage);
    if (reattachTools)
    {
        if (mRemoveConn)
            mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume);
        if (mScissors)
            mScissors->attach(mVtk, mRenderer, mImage, mVolume);
    }
    updateUndoRedoUi();
    if (mHistDlg && mHistDlg->isVisible())
        mHistDlg->refreshFromImage(mImage);
    if (mVtk && mVtk->renderWindow()) 
        mVtk->renderWindow()->Render();
}

void RenderView::commitNewImage(vtkImageData* im)
{
    // 1) текущий том → в undo (глубокая копия), очистить redo
    if (mImage)
    {
        mUndoStack.push_back(cloneImage(mImage));
        while (mUndoStack.size() > mHistoryLimit)
            mUndoStack.pop_front();
        mRedoStack.clear();
    }

    // 2) принять новый том (возможно им владеет инструмент)
    if (im)
        mImage = im;

    // 3) обновить маппер и перерисовать
    updateAfterImageChange(false);
}

void RenderView::onUndo()
{
    if (mUndoStack.isEmpty() || !mImage) return;

    // текущий в Redo
    mRedoStack.push_back(cloneImage(mImage));
    auto prev = mUndoStack.takeLast();

    mImage = prev;
    updateAfterImageChange(true);
}

void RenderView::onRedo()
{
    if (mRedoStack.isEmpty() || !mImage) return;

    // текущий в Undo
    mUndoStack.push_back(cloneImage(mImage));
    auto next = mRedoStack.takeLast();

    mImage = next;
    updateAfterImageChange(true);
}

void RenderView::setToolUiActive(bool on, Action a)
{
    mToolActive = on;
    if (!mBtnTools) return;
    if (on) 
    {
        mCurrentTool = a;
        mBtnTools->setText(tr("%1").arg(Tools::ToDisplayName(a)));
        mBtnTools->setChecked(true);
    }
    else 
    {
        mBtnTools->setText(tr("Edit"));
        mBtnTools->setChecked(false);
    }
}

void RenderView::setAppUiActive(bool on, App a)
{
    mAppActive = on;
    if (!mBtnApps) return;
    if (on)
    {
        mCurrentApp = a;
        mBtnApps->setText(tr("App: %1").arg(Tools::ToDisplayAppName(a)));
        mBtnApps->setChecked(true);
    }
    else
    {
        mBtnApps->setText(tr("Applications"));
        mBtnApps->setChecked(false);
    }
}

bool RenderView::ToolModeChanged(Action a)
{
    if (!mVolume) return false;

    if (mToolActive) {
        if (mScissors)    mScissors->cancel();
        if (mRemoveConn)  mRemoveConn->cancel();
        setToolUiActive(false, mCurrentTool);
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (mScissors && (a == Action::Scissors || a == Action::InverseScissors)) 
    {
        setToolUiActive(true, a);
        mScissors->attach(mVtk, mRenderer, mImage, mVolume);
        mScissors->handle(a);
        return true;
    }
    else if (mRemoveConn && (
        a == Action::RemoveUnconnected || 
        a == Action::RemoveSelected || 
        a == Action::RemoveConnected || 
        a == Action::SmartDeleting || 
        a == Action::VoxelEraser || 
        a == Action::VoxelRecovery || 
        a == Action::Minus || 
        a == Action::Plus ||
        a == Action::AddBase ||
        a == Action::FillEmpty ||
        a == Action::TotalSmoothing ||
        a == Action::PeelRecovery ||
        a == Action::SurfaceMapping))
    {
        setToolUiActive(true, a);
        mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume);
        mRemoveConn->handle(a);
        return true;
    }

    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
    return false;
}

bool RenderView::AppModeChanged(App a)
{
    if (!mVolume) return false;

    if (mAppActive) 
    {
        if (mHistDlg)    mHistDlg->close();
        
        setAppUiActive(false, mCurrentApp);
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (mHistDlg && (a == App::Histogram))
    {
        setAppUiActive(true, a);
        openHistogram();
        return true;
    }

    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
    return false;
}


void RenderView::clearStlPreview()
{
    if (!mRenderer) return;

    if (mIsoActor) {
        mRenderer->RemoveActor(mIsoActor);
        mIsoActor = nullptr;
    }

    if (mVolume) mVolume->SetVisibility(mPrevVolumeVisible);

    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
}

void RenderView::addStlPreview()
{
    if (!mRenderer) return;

    // Сначала уберём старый актёр (но НЕ теряем mIsoMesh)
    if (mIsoActor) 
    {
        mRenderer->RemoveActor(mIsoActor);
        mIsoActor = nullptr;
    }

    // Прячем объём до показа поверхности, запомнив, как было
    if (mVolume) {
        mPrevVolumeVisible = mVolume->GetVisibility();
        mVolume->SetVisibility(false);
    }

    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0) {
        if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
        return;
    }

    mIsoActor = MakeSurfaceActor(mIsoMesh);
    if (mIsoActor) mRenderer->AddActor(mIsoActor);

    if (mClip && mIsoActor) 
    {
        const bool wasClipOn = (mBtnClip && mBtnClip->isChecked());
        mClip->attachToSTL(mIsoActor);
        if (!wasClipOn)
            mClip->resetToBounds();
        mClip->setEnabled(wasClipOn); 
        if (wasClipOn) mClip->applyNow();
    }

    if (mVtk && mVtk->renderWindow()) 
        mVtk->renderWindow()->Render();

}

void RenderView::onStlSimplify()
{
    if (!mImage || !mIsoMesh || !mVolume || !mRenderer) {
        emit showWarning(tr("No volume to build STL"));
        if (mBtnSTL) mBtnSTL->setChecked(false);
        return;
    }

    emit showInfo(tr("Building surface…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);

    mIsoMesh = VolumeStlExporter::SimplifySurface(mIsoMesh, /*targetReduction=*/0.60, /*smoothIter=*/30, /*passBand=*/0.10);

    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0)
    {
        if (mBtnSTL) mBtnSTL->setChecked(false);
        mBtnSTLSave->setVisible(false);
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
        mBtnSTLSimplify->setEnabled(false);
        emit showWarning(tr("Ready Volume"));
        QApplication::restoreOverrideCursor();
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        return;
    }

    addStlPreview();

    mBtnSTLSave->setVisible(true);
    mBtnSTLSave->setEnabled(true);
    mBtnSTLSimplify->setVisible(true);
    mBtnSTLSimplify->setEnabled(true);
    emit showWarning(tr("Ready Surface"));
    QApplication::restoreOverrideCursor();
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}


void RenderView::onBuildStl()
{
    if (!mImage || !mVolume || !mRenderer) {
        emit showWarning(tr("No volume to build STL"));
        if (mBtnSTL) mBtnSTL->setChecked(false);
        return;
    }

    if (mBtnSTL->isChecked() == false)
    {
        mRenderer->RemoveActor(mIsoActor);
        mIsoActor = nullptr;
        mVolume->SetVisibility(true);
        mPrevVolumeVisible = true;
        mBtnSTLSave->setVisible(false);
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
        mBtnSTLSimplify->setEnabled(false);
        if (mClip)
        {
            const bool wasClipOn = (mBtnClip && mBtnClip->isChecked());
            mClip->attachToVolume(mVolume);
            if (!wasClipOn)
                mClip->resetToBounds();
            mClip->setEnabled(wasClipOn);
            if (wasClipOn) mClip->applyNow();
        }
        if (mVtk && mVtk->renderWindow())
            mVtk->renderWindow()->Render();

        emit showWarning(tr("Ready Volume"));
        return;
    }

    emit showInfo(tr("Building surface…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    VisibleExportOptions opt;
    opt.alphaThreshold = 0.15;
    opt.smoothIterations = 2;
    opt.smoothPassBand = 0.20;
    opt.decimate = 0.0;

    // Прогресс в статус-бар + «прокачка» событий
    opt.progress = [this](int p, const char* msg) {
        emit showInfo(QString("%1 (%2%)")
            .arg(QString::fromUtf8(msg ? msg : "Working"))
            .arg(p));
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        };


    
    mIsoMesh = VolumeStlExporter::BuildFromBinaryVoxels(mImage, opt);
    if (isCtrlDown())
    {
        emit showInfo(tr("Simplify Surface"));
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        mIsoMesh = VolumeStlExporter::SimplifySurface(mIsoMesh, /*targetReduction=*/0.70, /*smoothIter=*/20, /*passBand=*/0.1);
    }

    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0) 
    {
        if (mBtnSTL) mBtnSTL->setChecked(false);
        mBtnSTLSave->setVisible(false);
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
        mBtnSTLSimplify->setEnabled(false);
        emit showInfo(tr("Ready Volume"));
        QApplication::restoreOverrideCursor();
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        return;
    }

    addStlPreview();

    mBtnSTLSave->setVisible(true);
    mBtnSTLSave->setEnabled(true);
    mBtnSTLSimplify->setVisible(true);
    mBtnSTLSimplify->setEnabled(true);
    emit showInfo(tr("Ready Surface"));
    QApplication::restoreOverrideCursor();
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void RenderView::onSaveBuiltStl()
{
    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0) {
        emit showWarning(tr("Nothing to save — build STL first"));
        return;
    }
    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Save STL"), QString(), tr("STL binary (*.stl)")
    );
    if (filePath.isEmpty()) return;

    const bool ok = VolumeStlExporter::SaveStl(mIsoMesh, filePath, true);
    if (ok) emit showInfo(tr("Saved STL: %1").arg(filePath));
    else    emit showWarning(tr("Save failed."));
}

void RenderView::setImage(vtkSmartPointer<vtkImageData> img)
{
    if (img)
        mImage = img;

    if (mVolume) {
        if (auto* m = mVolume->GetMapper())
            m->SetInputDataObject(mImage);
        mVolume->Modified();
    }
    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
}

void RenderView::openTfEditor()
{
    if (!mImage || !mVolume) return;

    auto* prop = mVolume->GetProperty();
    if (!prop) return;

    // 1) Гарантируем наличие "базы" TF/OTF (немаскированной)
    // Если базы ещё нет — инициализируем её текущими кривыми из volume.
    if (!mBaseCTF) mBaseCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
    if (!mBaseOTF) mBaseOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
    if (mBaseCTF->GetSize() == 0 && prop->GetRGBTransferFunction(0))
        mBaseCTF->DeepCopy(prop->GetRGBTransferFunction(0));
    if (mBaseOTF->GetSize() == 0 && prop->GetScalarOpacity(0))
        mBaseOTF->DeepCopy(prop->GetScalarOpacity(0));

    // 2) Создаём TF-editor один раз
    if (!mTfEditor) {
        mTfEditor = new TransferFunctionEditor(this, mImage);

        // Предпросмотр: всегда пропускаем OTF через Hist-фильтр
        connect(mTfEditor, &TransferFunctionEditor::preview, this,
            [this](vtkColorTransferFunction* c, vtkPiecewiseFunction* o)
            {
                if (!mVolume) return;
                auto* prop = mVolume->GetProperty();
                if (!prop) return;

                prop->SetIndependentComponents(true);

                // Маска по гистограмме в домене изображения (HU)
                vtkSmartPointer<vtkPiecewiseFunction> masked = vtkSmartPointer<vtkPiecewiseFunction>::New();
                if (mHistMaskActive && o) 
                {
                    masked = BuildMaskedOTF(o, mHistMaskLo, mHistMaskHi); // lo/hi — в HU
                }
                else if (o) 
                {
                    masked->DeepCopy(o);
                }

                prop->SetColor(0, c ? c : mBaseCTF.GetPointer());
                prop->SetScalarOpacity(0, masked ? masked.GetPointer() : mBaseOTF.GetPointer());
                prop->Modified();
                if (auto* m = mVolume->GetMapper()) m->Modified();
                if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
            });

        // Коммит: обновляем "базу", а в сцену кладём её + Hist-маску
        connect(mTfEditor, &TransferFunctionEditor::committed, this,
            [this](vtkColorTransferFunction* c, vtkPiecewiseFunction* o)
            {
                if (!mVolume) return;
                auto* prop = mVolume->GetProperty();
                if (!prop) return;

                prop->SetIndependentComponents(true);

                // База: немаскированные кривые из редактора
                if (!mBaseCTF) mBaseCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
                if (!mBaseOTF) mBaseOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
                mBaseCTF->DeepCopy(c);
                mBaseOTF->DeepCopy(o);

                // Визуализация: база + Hist-маска (если активна)
                vtkSmartPointer<vtkPiecewiseFunction> masked = vtkSmartPointer<vtkPiecewiseFunction>::New();
                if (mHistMaskActive) {
                    masked = BuildMaskedOTF(mBaseOTF, mHistMaskLo, mHistMaskHi);
                }
                else {
                    masked->DeepCopy(mBaseOTF);
                }

                prop->SetColor(0, mBaseCTF);
                prop->SetScalarOpacity(0, masked);
                prop->Modified();
                if (auto* m = mVolume->GetMapper()) m->Modified();
                if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();

                // Обновим меню пресетов/кнопки, если нужно
                reloadTfMenu();
            });

        connect(mTfEditor, &TransferFunctionEditor::presetSaved, this, [this] {
            reloadTfMenu();
            });
    }

    // 4) Открываем редактор на НЕМаскированной базе + настоящей оси
    mTfEditor->setFixedAxis(DataMin, DataMax); // <<< важная строка
    mTfEditor->refreshHistogram(mImage);
    mTfEditor->setFromVtk(
        mBaseCTF ? mBaseCTF.GetPointer() : prop->GetRGBTransferFunction(0),
        mBaseOTF ? mBaseOTF.GetPointer() : prop->GetScalarOpacity(0),
        DataMin, DataMax
    );

    mTfEditor->show();
    mTfEditor->raise();
    mTfEditor->activateWindow();

    QTimer::singleShot(0, this, [this] { reloadTfMenu(); });
}


bool RenderView::applyPreset(TFPreset p)
{
    if (!mVolume || !mImage) return false;
    auto* prop = mVolume->GetProperty();
    if (!prop) return false;

    prop->SetIndependentComponents(true);

    // 2) Применяем пресет к свойству volume
    TF::ApplyPreset(prop, p, DataMin, DataMax);

    // 3) Синхронизируем "базу" TF/OTF (немаскированные)
    if (!mBaseCTF) mBaseCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
    if (!mBaseOTF) mBaseOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
    if (auto* c = prop->GetRGBTransferFunction(0)) mBaseCTF->DeepCopy(c);
    if (auto* o = prop->GetScalarOpacity(0))       mBaseOTF->DeepCopy(o);

    // 4) Если активна гист-маска — наложим её поверх базы
    vtkSmartPointer<vtkPiecewiseFunction> masked = vtkSmartPointer<vtkPiecewiseFunction>::New();
    if (mHistMaskActive && mBaseOTF) {
        masked = BuildMaskedOTF(mBaseOTF, mHistMaskLo, mHistMaskHi); // lo/hi в HU
    }
    else if (mBaseOTF) {
        masked->DeepCopy(mBaseOTF);
    }

    // 5) В сцену отправляем: цвет = база, прозрачность = (база + маска)
    prop->SetColor(0, mBaseCTF ? mBaseCTF.GetPointer() : nullptr);
    prop->SetScalarOpacity(0, masked ? masked.GetPointer() : nullptr);
    prop->Modified();
    if (auto* m = mVolume->GetMapper()) m->Modified();
    mVolume->Modified();

    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
    return true;
}


void RenderView::hideOverlays()
{
    if (!mOverlaysShown) return;

    if (mRightOverlay) { mRightOverlay->hide(); }
    if (mTopOverlay) { mTopOverlay->hide(); }

    if (mToolActive)
    {
        if (mScissors)    mScissors->cancel();
        if (mRemoveConn) 
        {
            mRemoveConn->cancel();
            mRemoveConn->ClearOriginalSnapshot();
        }
        setToolUiActive(false, mCurrentTool);
    }

    if (mHistDlg)    mHistDlg->close();


    

    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);

    mOverlaysShown = false;
}


void RenderView::showOverlays()
{
    if(mOverlaysShown) return;
    if (mRightOverlay) { mRightOverlay->show(); mRightOverlay->raise(); }
    if (mTopOverlay) { mTopOverlay->show();   mTopOverlay->raise(); }
    mOverlaysShown = true;
}

void RenderView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    repositionOverlay();
}

void RenderView::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);

    if (!mOverlaysBuilt) {
        buildOverlay();
    }

    // Показ и позиционирование — на следующем тике,
    // когда у виджета уже есть валидная геометрия/глобальные координаты
    QTimer::singleShot(0, this, [this] {
        showOverlays();
        repositionOverlay();
        if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
        });
}

void RenderView::repositionOverlay()
{
    if (!mVtk) return;

    const QRect r = mVtk->geometry();     // локальная геометрия внутри RenderView
    const int pad = 8;

    if (mScissors) 
        mScissors->onViewResized();
    if (mRemoveConn)
        mRemoveConn->onViewResized();
    if (mRightOverlay) {
        const QSize sz = mRightOverlay->size();
        const int x = r.right() - sz.width() - pad;  // прижать к правому краю mVtk
        const int y = r.top() + pad;                 // немного отступить сверху
        mRightOverlay->move(x, y);
        mRightOverlay->raise();
    }
    if (mTopOverlay) {
        const QSize sz = mTopOverlay->size();
        const int x = r.left() + pad;                // левый верх mVtk
        const int y = r.top() + pad;
        mTopOverlay->move(x, y);
        mTopOverlay->raise();
    }
}

void RenderView::centerOnVolume()
{
    if (!mRenderer || !mVolume) return;

    // 1) центр объёма
    double b[6]; mVolume->GetBounds(b);
    const double cx = 0.5 * (b[0] + b[1]);
    const double cy = 0.5 * (b[2] + b[3]);
    const double cz = 0.5 * (b[4] + b[5]);

    auto* cam = mRenderer->GetActiveCamera();
    if (!cam) return;

    // 2) сохраняем текущую дистанцию и направление взгляда
    double oldPos[3], oldFoc[3];
    cam->GetPosition(oldPos);
    cam->GetFocalPoint(oldFoc);

    double dir[3]{ oldPos[0] - oldFoc[0], oldPos[1] - oldFoc[1], oldPos[2] - oldFoc[2] };
    double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-9) len = 1.0;
    dir[0] /= len; dir[1] /= len; dir[2] /= len;

    // 3) ставим фокус в центр, позицию — на ту же дистанцию по текущему направлению
    cam->SetFocalPoint(cx, cy, cz);
    cam->SetPosition(cx + dir[0] * len, cy + dir[1] * len, cz + dir[2] * len);

    // 4) поправляем клиппинг и перерисовываем
    mRenderer->ResetCameraClippingRange();
    if (auto* rw = mVtk->renderWindow()) rw->Render();
}

void RenderView::updateGradientOpacity()
{
    if (!mVolume)
        return;

    auto* prop = mVolume->GetProperty();
    if (!prop)
        return;

    if (mGradientOpacityOn) {
        auto gtf = vtkSmartPointer<vtkPiecewiseFunction>::New();
        gtf->AddPoint(0, 0.00);
        gtf->AddPoint(15, 0.25);
        gtf->AddPoint(60, 0.80);
        prop->SetGradientOpacity(gtf);
    }
    else {
        auto gtf = vtkSmartPointer<vtkPiecewiseFunction>::New();
        gtf->AddPoint(0, 1.0);
        gtf->AddPoint(255, 1.0);
        prop->SetGradientOpacity(gtf);
    }

    if (auto* rw = mVtk->renderWindow())
        rw->Render();
}

void RenderView::setViewPreset(ViewPreset v)
{
    if (!mVolume || !mRenderer) return;
    auto* img = vtkImageData::SafeDownCast(mVolume->GetMapper()->GetInputDataObject(0, 0));
    if (!img) return;

    double b[6]; img->GetBounds(b);
    const double cx = 0.5 * (b[0] + b[1]);
    const double cy = 0.5 * (b[2] + b[3]);
    const double cz = 0.5 * (b[4] + b[5]);
    const double diag = std::sqrt((b[1] - b[0]) * (b[1] - b[0]) + (b[3] - b[2]) * (b[3] - b[2]) + (b[5] - b[4]) * (b[5] - b[4]));
    const double dist = diag * 1.5;

    double L[3], P[3], S[3];
    getPatientAxes(img, L, P, S);

    auto* cam = mRenderer->GetActiveCamera();
    double pos[3]{ cx,cy,cz };
    auto setPosByDir = [&](const double d[3]) {
        pos[0] = cx - d[0] * dist; pos[1] = cy - d[1] * dist; pos[2] = cz - d[2] * dist;
        };

    switch (v) {
    case ViewPreset::AP: {            // камера спереди, смотрим кзади (к Posterior)
        setPosByDir(P);               // взгляд вдоль +P
        break;
    }
    case ViewPreset::PA: {            // камера сзади, смотрим кпереди (к Anterior)
        double A[3]{ -P[0], -P[1], -P[2] };
        setPosByDir(A);               // взгляд вдоль Anterior
        break;
    }
    case ViewPreset::L: { setPosByDir(L); break; }
    case ViewPreset::R: { double Rv[3]{ -L[0], -L[1], -L[2] }; setPosByDir(Rv); break; }
    case ViewPreset::LAO: {
        const double a = std::sqrt(0.5);
        double A[3]{ -P[0], -P[1], -P[2] };
        double dir[3]{ a * A[0] + a * L[0], a * A[1] + a * L[1], a * A[2] + a * L[2] };
        const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int i = 0; i < 3; ++i) dir[i] /= (len > 0 ? len : 1);
        setPosByDir(dir);
        break;
    }
    case ViewPreset::RAO: {
        const double a = std::sqrt(0.5);
        double A[3]{ -P[0], -P[1], -P[2] };
        double Rv[3]{ -L[0], -L[1], -L[2] };
        double dir[3]{ a * A[0] + a * Rv[0], a * A[1] + a * Rv[1], a * A[2] + a * Rv[2] };
        const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int i = 0; i < 3; ++i) dir[i] /= (len > 0 ? len : 1);
        setPosByDir(dir);
        break;
    }
    }

    cam->SetFocalPoint(cx, cy, cz);
    cam->SetPosition(pos);
    cam->SetViewUp(S);                 // верх = Superior
    cam->OrthogonalizeViewUp();
    mRenderer->ResetCameraClippingRange();
    mVtk->renderWindow()->Render();
}

void RenderView::setVolume(vtkSmartPointer<vtkImageData> image, DicomInfo Dicom)
{
    auto pump = [&](int p) {
        emit renderProgress(std::clamp(p, 0, 100));
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        };

    emit renderStarted();
    const auto finish = qScopeGuard([&] {
        pump(100);
        emit renderFinished();
        });

    // 0) входные проверки
    if (!image) {
        emit showInfo(tr("No image"));
        return; // finish сработает guard-ом
    }
    pump(5);

    int ext[6]; image->GetExtent(ext);
    auto* scal = image->GetPointData() ? image->GetPointData()->GetScalars() : nullptr;
    if (ext[1] < ext[0] || ext[3] < ext[2] || ext[5] < ext[4] || !scal) {
        emit showWarning(tr("Invalid image: empty extent or no scalars."));
        return;
    }
    pump(12);

    // 1) гарантируем окно/рендерер
    if (!mVtk) {
        emit showInfo(tr("No render window"));
        return;
    }
    auto* rw = mVtk->renderWindow();
    if (!rw) {
        mWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        mVtk->setRenderWindow(mWindow);
        rw = mVtk->renderWindow();
    }
    pump(20);

    if (!mRenderer) {
        mRenderer = vtkSmartPointer<vtkRenderer>::New();
        rw->AddRenderer(mRenderer);
    }
    pump(25);

    DI = Dicom;

    // 2) настраиваем mapper/prop
    auto mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
    mapper->SetInputData(image);
    mapper->SetBlendModeToComposite();
    mapper->SetAutoAdjustSampleDistances(true);
    mapper->SetUseJittering(true);

    auto ctf = vtkSmartPointer<vtkColorTransferFunction>::New();
    ctf->AddRGBPoint(HistMin, 0, 0, 0);
    ctf->AddRGBPoint(HistMax, 1, 1, 1);
    ctf->SetColorSpaceToLab();

    auto otf = vtkSmartPointer<vtkPiecewiseFunction>::New();
    otf->AddPoint(HistMin, 0.00);
    otf->AddPoint(HistMax, 0.80);

    auto prop = vtkSmartPointer<vtkVolumeProperty>::New();
    prop->SetIndependentComponents(true);
    prop->SetColor(0, ctf);
    prop->SetScalarOpacity(0, otf);
    prop->ShadeOn();
    prop->SetAmbient(0.05);
    prop->SetDiffuse(0.9);
    prop->SetSpecular(0.1);
    prop->SetInterpolationType(VTK_NEAREST_INTERPOLATION);

    double sp[3]{ 1,1,1 };
    image->GetSpacing(sp);
    const double smin = std::min({ sp[0],sp[1],sp[2] });
    prop->SetScalarOpacityUnitDistance(std::max(0.3 * smin, 1e-3));
    pump(40);


    mShiftValue = 3;

    // 3) собираем volume и сцену
    auto vol = vtkSmartPointer<vtkVolume>::New();
    vol->SetMapper(mapper);
    vol->SetProperty(prop);

    mRenderer->RemoveAllViewProps();
    mRenderer->AddVolume(vol);
    mRenderer->ResetCamera();
    pump(58);

    // 4) сохранить ссылки, преднастройки вида/клипбокса
    mImage = image;
    mVolume = vol;
    setViewPreset(ViewPreset::AP);

    if (mClip) 
    {
        mClip->attachToVolume(mVolume);
        mClip->resetToBounds();
        mClip->setEnabled(false);
        if (mBtnClip) 
            mBtnClip->setChecked(false);
    }
    pump(66);

    if (mBtnSTL) 
    {
        mBtnSTL->setChecked(false);
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSave->setVisible(false);
        mBtnSTLSimplify->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
    }

    if (!mScissors)
    {
        mScissors = std::make_unique<ToolsScissors>(this);
        mScissors->setAllowNavigation(true);
        mScissors->setOnImageReplaced([this](vtkImageData* im)
            {
                commitNewImage(im);
                mScissors->attach(mVtk, mRenderer, mImage, mVolume);
            });
        mScissors->setOnFinished([this]() {
            setToolUiActive(false, mCurrentTool);
            });
    }
    mScissors->attach(mVtk, mRenderer, mImage, mVolume);

    if (!mRemoveConn) 
    {
        mRemoveConn = std::make_unique<ToolsRemoveConnected>(this);
        mRemoveConn->setAllowNavigation(true);
        mRemoveConn->setOnImageReplaced([this](vtkImageData* im) 
            {
                commitNewImage(im);
                mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume);
            });
        mRemoveConn->setOnFinished([this]() 
            {
            setToolUiActive(false, mCurrentTool);
            });
        mRemoveConn->EnsureOriginalSnapshot(mImage);
    }
    mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume);

    mUndoStack.clear();
    mRedoStack.clear();
    updateUndoRedoUi();

    // 5) применяем разумный пресет TF по диапазону данных
    reloadHistogram();
    reloadTfMenu();
    pump(78);

    TF::ApplyPreset(prop, TFPreset::Grayscale, DataMin, DataMax);

    if ((Dicom.TypeOfRecord == CT || Dicom.TypeOfRecord == CT3DR) && mCustomCtIndex >= 0)
        applyCustomPresetByIndex(mCustomCtIndex, prop, DataMin, DataMax);
    else if ((Dicom.TypeOfRecord == MRI || Dicom.TypeOfRecord == MRI3DR) && mCustomMrIndex >= 0)
        applyCustomPresetByIndex(mCustomMrIndex, prop, DataMin, DataMax);

    pump(88);

    // 6) включаем маркер, первый рендер
    if (mOrMarker) mOrMarker->SetEnabled(1);
    if (rw) rw->Render();
    pump(96);

    // guard доведёт до 100 и вызовет renderFinished()
}

void RenderView::applyCustomPresetByIndex(int idx, vtkVolumeProperty* prop,
    double dataMin, double dataMax)
{
    if (idx < 0 || idx >= mCustom.size() || !prop) return;

    const auto& P = mCustom[idx];
    TF::ApplyPoints(prop, P.points, dataMin, dataMax, P.colorSpace);

    // --- синхронизируем базу (как у тебя в меню) ---
    if (!mBaseCTF) mBaseCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
    if (!mBaseOTF) mBaseOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
    if (auto* c = prop->GetRGBTransferFunction(0)) mBaseCTF->DeepCopy(c);
    if (auto* o = prop->GetScalarOpacity(0))       mBaseOTF->DeepCopy(o);

    // --- если активна маска — наложить её поверх базы ---
    if (mHistMaskActive && mBaseOTF) {
        auto masked = BuildMaskedOTF(mBaseOTF, mHistMaskLo, mHistMaskHi);
        prop->SetColor(0, mBaseCTF);
        prop->SetScalarOpacity(0, masked);
    }

    if (auto* m = mVolume ? mVolume->GetMapper() : nullptr) m->Modified();
    if (mVolume) mVolume->Modified();
}

void RenderView::reloadTfMenu()
{
    if (mTfMenu) { delete mTfMenu; mTfMenu = nullptr; }
    mTfMenu = TF::CreateMenu(mTopOverlay, [this](TFPreset p) { applyPreset(p); });
    applyMenuStyle(mTfMenu, mBtnTF->width());

    // кнопка редактора
    QAction* actCustom = mTfMenu->addAction(tr("Custom…"));
    connect(actCustom, &QAction::triggered, this, &RenderView::openTfEditor);

    // загрузить кастомные из диска
    mCustom = TF::LoadCustomPresets();
    
    // --- Найдём индексы для автоприменения CT/MR ---
    mCustomCtIndex = -1;
    mCustomMrIndex = -1;

    auto findByKeys = [&](const QStringList& keys) -> int {
        for (int i = 0; i < mCustom.size(); ++i) {
            const QString name = mCustom[i].name;
            for (const auto& k : keys) {
                if (name.contains(k, Qt::CaseInsensitive))
                    return i;
            }
        }
        return -1;
        };
    mCustomCtIndex = findByKeys(mCtKeys);
    mCustomMrIndex = findByKeys(mMrKeys);


    if (!mCustom.isEmpty()) {
        mTfMenu->addSeparator();
        for (int i = 0; i < mCustom.size(); ++i) {
            const auto& P = mCustom[i];
            QAction* a = mTfMenu->addAction(QString::fromUtf8("★ %1").arg(P.name));
            connect(a, &QAction::triggered, this, [this, i]
                {
                    if (!mVolume) 
                        return;

                    auto* prop = mVolume->GetProperty();
                    // Применяем точки в prop
                    TF::ApplyPoints(prop, mCustom[i].points, DataMin, DataMax, mCustom[i].colorSpace);
                    // --- Синхронизируем базу ---
                    if (!mBaseCTF) mBaseCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
                    if (!mBaseOTF) mBaseOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
                    if (auto* c = prop->GetRGBTransferFunction(0)) mBaseCTF->DeepCopy(c);
                    if (auto* o = prop->GetScalarOpacity(0))       mBaseOTF->DeepCopy(o);
                    // --- Если активна маска — пере-наложить её ---
                    if (mHistMaskActive && mBaseOTF)
                    {
                        auto masked = BuildMaskedOTF(mBaseOTF, mHistMaskLo, mHistMaskHi);
                        prop->SetColor(0, mBaseCTF);
                        prop->SetScalarOpacity(0, masked);
                    }
                    if (auto* m = mVolume->GetMapper()) m->Modified();
                    mVolume->Modified();
                    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render(); 
                });
        }
    }
    mBtnTF->setMenu(mTfMenu);
}