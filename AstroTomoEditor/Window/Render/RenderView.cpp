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
#include <vtkPlaneCollection.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkAxesActor.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkMatrix3x3.h>
#include <vtkMath.h>
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
#include <vtkCellLocator.h>
#include <vtkCell.h>
#include <Services/DicomRange.h>
#include <QTimer>
#include <QLineEdit>
#include <vtkFlyingEdges3D.h>
#include "..\..\Services\LanguageManager.h"
#include <QSettings>
#include <Services/AppConfig.h>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <Window/ServiceWindow/ShellFileDialog.h>
#include <Window/ServiceWindow/CustomMessageBox.h>
#include "ElectrodeAutoIdentifier.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <QDialog>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

namespace
{
    using ContourPoint = std::array<double, 3>;

    constexpr quint32 kStoredContourColor = 0x00CCCCCC;
    constexpr quint8 kStoredContourWidth = 10;
    constexpr double kContourSimplifyToleranceMm = 0.2;
    constexpr double kContourDuplicatePointEps2 = 1e-10;
    constexpr double kContourNormalOffsetMm = 5.0;

    double pointSegmentDistanceSquared(
        const ContourPoint& p,
        const ContourPoint& a,
        const ContourPoint& b)
    {
        const double abx = b[0] - a[0];
        const double aby = b[1] - a[1];
        const double abz = b[2] - a[2];
        const double apx = p[0] - a[0];
        const double apy = p[1] - a[1];
        const double apz = p[2] - a[2];
        const double ab2 = abx * abx + aby * aby + abz * abz;

        if (ab2 <= kContourDuplicatePointEps2)
            return (apx * apx + apy * apy + apz * apz);

        double t = (apx * abx + apy * aby + apz * abz) / ab2;
        t = std::clamp(t, 0.0, 1.0);

        const double cx = a[0] + t * abx;
        const double cy = a[1] + t * aby;
        const double cz = a[2] + t * abz;
        const double dx = p[0] - cx;
        const double dy = p[1] - cy;
        const double dz = p[2] - cz;
        return dx * dx + dy * dy + dz * dz;
    }

    QVector<ContourPoint> simplifyOpenContourPath(
        const QVector<ContourPoint>& path,
        double toleranceSquared)
    {
        if (path.size() <= 2)
            return path;

        QVector<char> keep(path.size(), 0);
        keep[0] = 1;
        keep[path.size() - 1] = 1;

        QVector<std::pair<int, int>> stack;
        stack.push_back({ 0, static_cast<int>(path.size()) - 1 });

        while (!stack.isEmpty())
        {
            const auto [first, last] = stack.takeLast();
            if (last <= first + 1)
                continue;

            double maxDistance2 = -1.0;
            int maxIndex = -1;
            for (int i = first + 1; i < last; ++i)
            {
                const double distance2 = pointSegmentDistanceSquared(path[i], path[first], path[last]);
                if (distance2 > maxDistance2)
                {
                    maxDistance2 = distance2;
                    maxIndex = i;
                }
            }

            if (maxIndex >= 0 && maxDistance2 > toleranceSquared)
            {
                keep[maxIndex] = 1;
                stack.push_back({ first, maxIndex });
                stack.push_back({ maxIndex, last });
            }
        }

        QVector<ContourPoint> simplified;
        simplified.reserve(path.size());
        for (int i = 0; i < path.size(); ++i)
        {
            if (keep[i])
                simplified.push_back(path[i]);
        }

        return simplified;
    }

    QVector<ContourPoint> loopPathBetween(
        const QVector<ContourPoint>& loop,
        int first,
        int last)
    {
        QVector<ContourPoint> path;
        if (loop.isEmpty())
            return path;

        const int count = static_cast<int>(loop.size());
        int i = first;
        for (;;)
        {
            path.push_back(loop[i]);
            if (i == last)
                break;
            i = (i + 1) % count;
        }

        return path;
    }

    QVector<ContourPoint> simplifyClosedContour(
        const QVector<ContourPoint>& contour,
        double toleranceMm)
    {
        QVector<ContourPoint> loop;
        loop.reserve(contour.size());
        for (const auto& p : contour)
        {
            if (loop.isEmpty())
            {
                loop.push_back(p);
                continue;
            }

            const auto& prev = loop.back();
            const double dx = p[0] - prev[0];
            const double dy = p[1] - prev[1];
            const double dz = p[2] - prev[2];
            if ((dx * dx + dy * dy + dz * dz) > kContourDuplicatePointEps2)
                loop.push_back(p);
        }

        if (loop.size() >= 2)
        {
            const auto& first = loop.front();
            const auto& last = loop.back();
            const double dx = first[0] - last[0];
            const double dy = first[1] - last[1];
            const double dz = first[2] - last[2];
            if ((dx * dx + dy * dy + dz * dz) <= kContourDuplicatePointEps2)
                loop.pop_back();
        }

        if (loop.size() <= 3)
            return loop;

        int anchorA = 0;
        int anchorB = 1;
        double maxDistance2 = -1.0;
        for (int i = 0; i < loop.size(); ++i)
        {
            for (int j = i + 1; j < loop.size(); ++j)
            {
                const double dx = loop[i][0] - loop[j][0];
                const double dy = loop[i][1] - loop[j][1];
                const double dz = loop[i][2] - loop[j][2];
                const double distance2 = dx * dx + dy * dy + dz * dz;
                if (distance2 > maxDistance2)
                {
                    maxDistance2 = distance2;
                    anchorA = i;
                    anchorB = j;
                }
            }
        }

        const double toleranceSquared = toleranceMm * toleranceMm;
        const auto pathA = simplifyOpenContourPath(
            loopPathBetween(loop, anchorA, anchorB),
            toleranceSquared);
        const auto pathB = simplifyOpenContourPath(
            loopPathBetween(loop, anchorB, anchorA),
            toleranceSquared);

        QVector<ContourPoint> simplified;
        simplified.reserve(pathA.size() + pathB.size());

        for (int i = 0; i < pathA.size(); ++i)
            simplified.push_back(pathA[i]);
        for (int i = 1; i + 1 < pathB.size(); ++i)
            simplified.push_back(pathB[i]);

        return simplified.size() >= 3 ? simplified : loop;
    }

    QVector<ContourPoint> fitContourToStoredPointLimit(const QVector<ContourPoint>& contour)
    {
        QVector<ContourPoint> simplified = simplifyClosedContour(contour, kContourSimplifyToleranceMm);

        double tolerance = kContourSimplifyToleranceMm;
        while (simplified.size() > MaxStoredPointPerContour)
        {
            tolerance *= 1.35;
            simplified = simplifyClosedContour(contour, tolerance);
        }

        return simplified;
    }

    void setStoredContourName(StoredContourHeader& header, const QString& name)
    {
        std::fill(std::begin(header.ContourName), std::end(header.ContourName), char16_t{ 0 });
        constexpr int kContourNameCapacity =
            static_cast<int>(sizeof(header.ContourName) / sizeof(header.ContourName[0]));
        const int count = std::min<int>(name.size(), kContourNameCapacity - 1);
        for (int i = 0; i < count; ++i)
            header.ContourName[i] = static_cast<char16_t>(name.at(i).unicode());
    }

    StoredContourCoord makeStoredContourCoord(const ContourPoint& p)
    {
        StoredContourCoord coord{};
        coord.x = p[0];
        coord.y = p[1];
        coord.z = p[2];
        coord.unused = 0.0;
        return coord;
    }

    void setStoredContourPoint(
        StoredContourPoint& point,
        const ContourPoint& initialPoint,
        const ContourPoint& userViewPoint1,
        const ContourPoint& userViewPoint2)
    {
        point = StoredContourPoint{};
        point.InitialContourPoint = makeStoredContourCoord(initialPoint);
        point.UserViewPoint1 = makeStoredContourCoord(userViewPoint1);
        point.UserViewPoint2 = makeStoredContourCoord(userViewPoint2);
    }

    void swapStoredContourCoordYZ(StoredContourCoord& coord)
    {
        std::swap(coord.y, coord.z);
    }

    void subtractStoredContourCoordShift(StoredContourCoord& coord, const DicomInfo& dicom)
    {
        coord.x -= dicom.STLCenterShiftX;
        coord.y -= dicom.STLCenterShiftY;
        coord.z -= dicom.STLCenterShiftZ;
    }

    void swapStoredContourPointYZ(StoredContourPoint& point)
    {
        swapStoredContourCoordYZ(point.InitialContourPoint);
        swapStoredContourCoordYZ(point.UserViewPoint1);
        swapStoredContourCoordYZ(point.UserViewPoint2);
    }

    void subtractStoredContourPointShift(StoredContourPoint& point, const DicomInfo& dicom)
    {
        subtractStoredContourCoordShift(point.InitialContourPoint, dicom);
        subtractStoredContourCoordShift(point.UserViewPoint1, dicom);
        subtractStoredContourCoordShift(point.UserViewPoint2, dicom);
    }

    void swapStoredContourYZ(StoredContourInfo& contour)
    {
        const int pointCount = std::min<int>(
            static_cast<int>(contour.header.nContourPoint),
            MaxStoredPointPerContour);

        for (int i = 0; i < pointCount; ++i)
            swapStoredContourPointYZ(contour.Points[i]);
    }

    void subtractStoredContourShift(StoredContourInfo& contour, const DicomInfo& dicom)
    {
        const int pointCount = std::min<int>(
            static_cast<int>(contour.header.nContourPoint),
            MaxStoredPointPerContour);

        for (int i = 0; i < pointCount; ++i)
            subtractStoredContourPointShift(contour.Points[i], dicom);
    }

    double contourDistanceSquared(const ContourPoint& a, const ContourPoint& b)
    {
        const double dx = a[0] - b[0];
        const double dy = a[1] - b[1];
        const double dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    ContourPoint contourCentroid(const QVector<ContourPoint>& contour)
    {
        ContourPoint c{ 0.0, 0.0, 0.0 };
        if (contour.isEmpty())
            return c;

        for (const auto& p : contour)
        {
            c[0] += p[0];
            c[1] += p[1];
            c[2] += p[2];
        }

        const double invCount = 1.0 / static_cast<double>(contour.size());
        c[0] *= invCount;
        c[1] *= invCount;
        c[2] *= invCount;
        return c;
    }

    double contourPerimeter(const QVector<ContourPoint>& contour)
    {
        if (contour.size() < 2)
            return 0.0;

        double length = 0.0;
        for (int i = 0; i < contour.size(); ++i)
        {
            const auto& a = contour[i];
            const auto& b = contour[(i + 1) % contour.size()];
            length += std::sqrt(contourDistanceSquared(a, b));
        }
        return length;
    }

    bool sampledNearestStatsWithinLimits(
        const QVector<ContourPoint>& source,
        const QVector<ContourPoint>& target,
        double maxMeanDistance2,
        double maxSingleDistance2)
    {
        if (source.isEmpty() || target.isEmpty())
            return false;

        const int stride = std::max(1, static_cast<int>(source.size()) / 80);
        double sumNearest2 = 0.0;
        int sampleCount = 0;

        for (int i = 0; i < source.size(); i += stride)
        {
            double nearest2 = std::numeric_limits<double>::max();
            for (const auto& p : target)
                nearest2 = std::min(nearest2, contourDistanceSquared(source[i], p));

            if (nearest2 > maxSingleDistance2)
                return false;

            sumNearest2 += nearest2;
            ++sampleCount;
        }

        if (sampleCount == 0)
            return false;

        return (sumNearest2 / static_cast<double>(sampleCount)) <= maxMeanDistance2;
    }

    bool contoursMatchGeometry(const QVector<ContourPoint>& a, const QVector<ContourPoint>& b)
    {
        if (a.size() < 3 || b.size() < 3)
            return false;

        constexpr double kMaxCentroidDistanceMm = 3.0;
        if (contourDistanceSquared(contourCentroid(a), contourCentroid(b)) >
            kMaxCentroidDistanceMm * kMaxCentroidDistanceMm)
        {
            return false;
        }

        const double lenA = contourPerimeter(a);
        const double lenB = contourPerimeter(b);
        if (lenA <= 0.0 || lenB <= 0.0)
            return false;

        const double maxLen = std::max(lenA, lenB);
        if (std::abs(lenA - lenB) / maxLen > 0.15)
            return false;

        constexpr double kMaxMeanDistanceMm = 1.0;
        constexpr double kMaxSingleDistanceMm = 3.0;
        const double maxMeanDistance2 = kMaxMeanDistanceMm * kMaxMeanDistanceMm;
        const double maxSingleDistance2 = kMaxSingleDistanceMm * kMaxSingleDistanceMm;

        return sampledNearestStatsWithinLimits(a, b, maxMeanDistance2, maxSingleDistance2) &&
            sampledNearestStatsWithinLimits(b, a, maxMeanDistance2, maxSingleDistance2);
    }

    bool closestSurfaceNormal(
        vtkPolyData* surface,
        vtkCellLocator* locator,
        const ContourPoint& pointWorld,
        double normal[3])
    {
        if (!surface || !locator || !normal)
            return false;

        double closest[3]{ 0.0, 0.0, 0.0 };
        double dist2 = std::numeric_limits<double>::max();
        vtkIdType cellId = -1;
        int subId = 0;
        locator->FindClosestPoint(pointWorld.data(), closest, cellId, subId, dist2);
        if (cellId < 0)
            return false;

        vtkCell* cell = surface->GetCell(cellId);
        if (!cell || cell->GetNumberOfPoints() < 3)
            return false;

        double p0[3], p1[3], p2[3];
        cell->GetPoints()->GetPoint(0, p0);
        cell->GetPoints()->GetPoint(1, p1);
        cell->GetPoints()->GetPoint(2, p2);

        const double u[3]{ p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const double v[3]{ p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        vtkMath::Cross(u, v, normal);
        if (vtkMath::Normalize(normal) == 0.0)
            return false;

        double bounds[6];
        surface->GetBounds(bounds);
        const double meshCenter[3]{
            0.5 * (bounds[0] + bounds[1]),
            0.5 * (bounds[2] + bounds[3]),
            0.5 * (bounds[4] + bounds[5])
        };
        const double outward[3]{
            closest[0] - meshCenter[0],
            closest[1] - meshCenter[1],
            closest[2] - meshCenter[2]
        };

        if (vtkMath::Dot(normal, outward) < 0.0)
        {
            normal[0] = -normal[0];
            normal[1] = -normal[1];
            normal[2] = -normal[2];
        }

        return true;
    }
}

static vtkMatrix3x3* fetchDirectionMatrix(vtkImageData* img, vtkNew<vtkMatrix3x3>& holder)
{
    if (auto* M = img->GetDirectionMatrix())
        return M;
    holder->Identity();
    return holder.GetPointer();
}

static vtkSmartPointer<vtkPolyData> clonePolyData(vtkPolyData* src)
{
    if (!src)
        return nullptr;

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(src);
    return out;
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
    mStlModeController.setHistoryLimit(mHistoryLimit);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    mVtk = new QVTKOpenGLNativeWidget(this);
    mVtk->setAttribute(Qt::WA_TranslucentBackground, true);
    mVtk->setStyleSheet("background: transparent;");
    lay->addWidget(mVtk);

    // 2) создаём render window и привязываем к виджету
    mWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    mWindow->SetAlphaBitPlanes(1);
    mWindow->SetMultiSamples(0);
    mVtk->setRenderWindow(mWindow);

    // 3) создаём renderer и добавляем в окно
    mRenderer = vtkSmartPointer<vtkRenderer>::New();
    mWindow->AddRenderer(mRenderer);

    auto* iren = mVtk->interactor();
    auto style = vtkSmartPointer<InteractorStyleCtrlWheelShiftPan>::New();
    iren->SetInteractorStyle(style);

    auto sc = new QShortcut(QKeySequence(Qt::Key_F12), this);
    connect(sc, &QShortcut::activated, this, &RenderView::centerOnVolume);

    auto* scMaterialDebug = new QShortcut(QKeySequence(Qt::Key_F9), this);
    connect(scMaterialDebug, &QShortcut::activated, this, [this]
        {
            openStlMaterialDebugDialog();
        });


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
    showOverlays();
    repositionOverlay();

    mClip = std::make_unique<ClipBoxController>(this);
    mClip->setRenderer(mRenderer);
    mClip->setInteractor(mVtk->interactor());

    connect(mBtnClip, &QToolButton::toggled, this, [this](bool on) {
        mClip->setEnabled(on);
        rebuildContourOverlay();
        if (mVtk && mVtk->renderWindow())
            mVtk->renderWindow()->Render();
        });
    connect(mBtnSTL, &QToolButton::clicked, this, &RenderView::onBuildStl);
    connect(mBtnSTLSimplify, &QToolButton::clicked, this, &RenderView::onStlSimplify);
    connect(mBtnSTLSave, &QToolButton::clicked, this, &RenderView::onSaveBuiltStl);

    loadRenderSettings();
}

RenderView::~RenderView() {
    if (mScissors) { mScissors->setOnFinished(nullptr); }
    if (mContour) { mContour->setOnFinished(nullptr); }
    if (mRemoveConn) { mRemoveConn->setOnFinished(nullptr); }
    if (mScissors)    mScissors->cancel();
    if (mContour)     mContour->cancel();
    if (mRemoveConn)  mRemoveConn->cancel();
    mScissors.reset();
    mContour.reset();
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
        b->setFixedSize(40, 26);
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
        b->setFixedSize(86, 26);
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
        b->setFixedSize(130, 26);
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
        b->setFixedSize(200, 26);
        b->setStyleSheet(
            "QToolButton{ color:#fff; background:rgba(40,40,40,110);"
            " border:1px solid rgba(255,255,255,30); border-radius:6px; padding:0 8px; }"
            "QToolButton:hover{ background:rgba(255,255,255,40); }"
            "QToolButton:pressed{ background:rgba(255,255,255,70); }"
            "QToolButton:checked{ background:rgba(0,180,100,140); }"
        );
        return b;
        };

    auto retain = [](QWidget* w)
        {
            if (!w) return;
            auto pol = w->sizePolicy();
            pol.setRetainSizeWhenHidden(true);
            w->setSizePolicy(pol);
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

    mBtnSTLSimplify = makeBtn(rightPanel, tr("Simplify"));
    mBtnSTLSimplify->setEnabled(false);
    mBtnSTLSimplify->setVisible(false);
    retain(mBtnSTLSimplify);
    rv->addWidget(mBtnSTLSimplify);

    mBtnSTLSave = makeBtn(rightPanel, tr("Save"));
    mBtnSTLSave->setEnabled(false);
    mBtnSTLSave->setVisible(false);
    retain(mBtnSTLSave);
    rv->addWidget(mBtnSTLSave);

    mLblStlSize = new QLabel(rightPanel);
    mLblStlSize->setVisible(false);
    retain(mLblStlSize);

    mLblStlSize->setText(" ");
    mLblStlSize->setWordWrap(true);

    // ширина = ширина кнопки Save (чтобы точно совпало)
    const int w = mBtnSTLSave ? mBtnSTLSave->width() : 86;
    mLblStlSize->setFixedWidth(w);
    const int h = mLblStlSize->fontMetrics().lineSpacing() * 3;
    mLblStlSize->setFixedHeight(h);

    // центр текста внутри
    mLblStlSize->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    // стиль: меньше шрифт и почти без паддинга, чтобы не резало
    mLblStlSize->setStyleSheet(
        "QLabel{ color:rgba(255,255,255,190); background:transparent;"
        "padding:0px; font-size:12px; }"
    );

    // ВАЖНО: добавить с выравниванием по центру (центрируется сам виджет в колонке)
    rv->addWidget(mLblStlSize, 0, Qt::AlignHCenter);


    rightPanel->adjustSize();
    mRightOverlay->resize(rightPanel->sizeHint());

    // ---------- ВЕРХНЯЯ ПАНЕЛЬ ----------
    mTopOverlay = makeNativeOverlay(this);
    mTopOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    mElectrodeOverlay = makeNativeOverlay(this);
    mElectrodeOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* topPanel = new QWidget(mTopOverlay);
    topPanel->setObjectName("TopPanel");
    topPanel->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* topElectrodPanel = new QWidget(mElectrodeOverlay);
    topElectrodPanel->setObjectName("TopElectrodPanel");
    topElectrodPanel->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    auto* th = new QHBoxLayout(topPanel);
    th->setContentsMargins(12, 8, 0, 0);
    th->setSpacing(6);

    auto* the = new QHBoxLayout(topElectrodPanel);
    the->setContentsMargins(0, 0, 0, 0);
    the->setSpacing(6);
    
    mElectrodePanel = new ElectrodePanel(topElectrodPanel);
    mElectrodePanel->setVisible(false);
    mElectrodePanel->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    mElectrodePanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(mElectrodePanel, &ElectrodePanel::requestExit, this, [this] {
        if (mAppActive && mCurrentApp == App::Electrodes) {
            setElectrodesUiActive(false);
            setAppUiActive(false, mCurrentApp);
        }
        });

    connect(mElectrodePanel, &ElectrodePanel::electrodeChosen, this,
        [this](ElectrodePanel::ElectrodeId id)
        {
            if (mElectrodePanel)
                mElectrodePanel->beginPick(id);
        });

    connect(mElectrodePanel, &ElectrodePanel::pickCommitted, this,
        [this](ElectrodePanel::ElectrodeId id, std::array<int, 3> ijk, std::array<double, 3> w)
        {
            mElectrodeIJK[id] = ijk;

            if (mElectrodePanel)
                mElectrodePanel->setHasCoord(id, true);
        });

    connect(mElectrodePanel, &ElectrodePanel::electrodeClearRequested, this,
        [this](ElectrodePanel::ElectrodeId id)
        {
            mElectrodeIJK.remove(id);
            if (mElectrodePanel)
                mElectrodePanel->setHasCoord(id, false);
        });

    the->addWidget(mElectrodePanel);

    connect(mElectrodePanel, &ElectrodePanel::autoRequested, this, [this]()
        {
            if (!mImage || !mRenderer || !mElectrodePanel) return;

            auto& detector = ElectrodeSurfaceDetector::instance();
            ElectrodeSurfaceDetector::Options opt = detector.options();
            detector.setOptions(opt);

            std::vector<std::array<double, 3>> excludedWorld = detector.currentSphereCenters();
            const auto committed = mElectrodePanel->coordsWorld();
            excludedWorld.reserve(excludedWorld.size() + committed.size());
            for (const auto& e : committed)
                excludedWorld.push_back(e.world);

            const auto centers = detector.detectAndShow(mImage, mRenderer, excludedWorld);
            mElectrodePanel->setManualAddEnabled(true);
            mElectrodePanel->refreshSearchRLFNButton();
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();

            qDebug() << "[AutoElectrodes] detected:" << int(centers.size())
                << "excluded:" << int(excludedWorld.size())
                << "radiusMm:" << opt.exclusionRadiusMm;

            if (mVtk && mVtk->renderWindow())
                mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::searchRLFNRequested, this, [this]()
        {
            if (!mElectrodePanel || !mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            setViewPreset(ViewPreset::AP);
            ElectrodeAutoIdentifier::SearchRLFN(mElectrodePanel, mRenderer, DI);
            mElectrodePanel->refreshSearchRLFNButton();
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::searchV1V6Requested, this, [this]()
        {
            if (!mElectrodePanel || !mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            setViewPreset(ViewPreset::AP);
            ElectrodeAutoIdentifier::SearchV1V6(mElectrodePanel, mRenderer, DI);
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::searchV7V12Requested, this, [this]()
        {
            if (!mElectrodePanel || !mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            setViewPreset(ViewPreset::AP);
            ElectrodeAutoIdentifier::SearchV7V12(mElectrodePanel, mRenderer, DI);
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::searchV13V19Requested, this, [this]()
        {
            if (!mElectrodePanel || !mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            setViewPreset(ViewPreset::AP);
            ElectrodeAutoIdentifier::SearchV13V19(mElectrodePanel, mRenderer, DI);
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::searchV20V25Requested, this, [this]()
        {
            if (!mElectrodePanel || !mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            setViewPreset(ViewPreset::AP);
            ElectrodeAutoIdentifier::SearchV20V25(mElectrodePanel, mRenderer, DI);
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::searchV26V30Requested, this, [this]()
        {
            if (!mElectrodePanel || !mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            setViewPreset(ViewPreset::AP);
            ElectrodeAutoIdentifier::SearchV26V30(mElectrodePanel, mRenderer, DI);
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    connect(mElectrodePanel, &ElectrodePanel::electrodeAltRightClicked, this,
        [this](std::array<double, 3> world)
        {
            if (!mRenderer || !mVtk || !mVtk->renderWindow())
                return;

            ElectrodeSurfaceDetector::instance().addManualSphere(mRenderer, world);
            mElectrodePanel->refreshSearchRLFNButton();
            mElectrodePanel->refreshSearchV1V6Button();
            mElectrodePanel->refreshSearchV7V12Button();
            mElectrodePanel->refreshSearchV26V30Button();
            mElectrodePanel->refreshSearchV13V19Button();
            mElectrodePanel->refreshSearchV20V25Button();
            mVtk->renderWindow()->Render();
        });

    mBtnTF = makeBigBtn(topPanel, tr("Transfer function"));
    reloadTfMenu();
    mBtnTF->setPopupMode(QToolButton::InstantPopup);
    retain(mBtnTF);
    th->addWidget(mBtnTF);

    mBtnTools = makeBigBtn(topPanel, tr("Edit"));
    retain(mBtnTools);
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



    reloadToolsMenu();
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
    retain(mBtnShift);
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

    reloadAppsMenu();
    applyMenuStyle(mAppsMenu, mBtnApps->width());

    connect(mAppsMenu, &QMenu::aboutToShow, this, [this] {
        if (!mAppActive) return;
        if (mCurrentApp == App::Electrodes && mElectrodePanel)
            mElectrodePanel->setModeEnabled(false);
        if (mCurrentApp == App::Electrodes)
            setAppUiActive(false, mCurrentApp);
        });

    connect(mElectrodePanel, &ElectrodePanel::saveRequested,
        this, &RenderView::onSaveElectrodesCoords);

    mBtnApps->setMenu(mAppsMenu);
    mBtnApps->setPopupMode(QToolButton::InstantPopup);
    retain(mBtnApps);
    th->addWidget(mBtnApps);
    

    {
        mBtnUndo = makeSmallBtn(topPanel, "");
        mBtnUndo->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mBtnUndo->setIcon(QIcon(":/icons/Resources/undo-no.svg"));
        mBtnUndo->setIconSize(QSize(20, 20));
        mBtnUndo->setToolTip(tr("Undo (Ctrl+Z)"));
        mBtnUndo->setEnabled(false);
        retain(mBtnUndo);
        th->addWidget(mBtnUndo);

        mBtnRedo = makeSmallBtn(topPanel, "");
        mBtnRedo->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mBtnRedo->setIcon(QIcon(":/icons/Resources/redo-no.svg"));
        mBtnRedo->setIconSize(QSize(20, 20));
        mBtnRedo->setToolTip(tr("Redo (Ctrl+Y)"));
        mBtnRedo->setEnabled(false);
        retain(mBtnRedo);
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

        mHistDlg->HideAutoRange(mImage);
        });

    auto* scHistAutoReturn = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
    connect(scHistAutoReturn, &QShortcut::activated, this, [this] {
        if (!mImage)
            return;

        if (!mHistDlg)
            return;

        mHistDlg->HideAllShow(mImage);
        });

    auto* scGradOpacity = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this);
    connect(scGradOpacity, &QShortcut::activated, this, [this] {
        if (!mVolume) return;
        setGradientOpacityEnabled(!mGradientOpacityOn);
        });

    topPanel->adjustSize();
    mTopOverlay->resize(topPanel->sizeHint());

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

void RenderView::alignApViewToWorldPoint(const std::array<double, 3>& world)
{
    if (!mRenderer || !mVtk || !mVtk->renderWindow())
        return;

    auto* cam = mRenderer->GetActiveCamera();
    if (!cam)
        return;

    double cx = 0.0;
    double cy = 0.0;
    if (!ElectrodeAutoIdentifier::ComputeVolumeDisplayCenter(mRenderer, cx, cy))
        return;
    (void)cy;

    const int maxIter = 24;
    for (int iter = 0; iter < maxIter; ++iter)
    {
        double x = 0.0;
        double y = 0.0;
        if (!ElectrodeAutoIdentifier::WorldToDisplay(mRenderer, world, x, y))
            break;

        const double dx = x - cx;
        if (std::abs(dx) < 2.0)
            break;

        const double step = std::clamp(-dx * 0.05, -6.0, 6.0);
        cam->Azimuth(step);
        cam->OrthogonalizeViewUp();
        mRenderer->ResetCameraClippingRange();
    }

    if (auto* rw = mVtk->renderWindow())
        rw->Render();
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

void RenderView::updateElectrodeOverlayMask()
{
    if (!mElectrodeOverlay || !mElectrodePanel) return;

    // берём маску ElectrodePanel и переносим её в координаты overlay
    QRegion reg = mElectrodePanel->mask();
    if (reg.isEmpty())
        reg = QRegion(mElectrodePanel->rect()); // на всякий

    const QPoint off = mElectrodePanel->mapTo(mElectrodeOverlay, QPoint(0, 0));
    reg.translate(off);

    mElectrodeOverlay->setMask(reg);
}

void RenderView::captureElectrodesTemplateFromCurrentVolume()
{
    if (!mImage)
        return;

    // Нужен диалог шаблонов, потому что именно он хранит слоты.
    ensureTemplateDialog();
    if (!mTemplateDlg)
        return;

    if (!mTemplateDlg->isCaptured(TemplateId::Electrodes))
    {

        // Делаем КОПИЮ, чтобы не портить текущий volume/рендер.
        Volume vol;
        vol.clear();
        vol.copy(mImage);
        if (!vol.raw() || !vol.u8().valid)
            return;

        const size_t total = vol.u8().size();
        for (size_t i = 0; i < total; ++i)
        {
            const uint8_t v = vol.at(i);
            if (v < HistMax - 1)
                vol.at(i) = 0;
            else
                vol.at(i) = HistMax - 1;
        }

        // Сохраняем в слот Electrodes. Диалог сам обновит UI.
        mTemplateDlg->setCaptured(TemplateId::Electrodes, vol);
    }
}

void RenderView::beginElectrodesPreview()
{
    if (mElectrodesPreviewActive)
        return;
    if (!mImage || !mVolume)
        return;

    // Шаблон хранится в TemplateDialog слотах.
    ensureTemplateDialog();
    if (!mTemplateDlg)
        return;

    // Если ещё не захвачен, пусть создастся автоматически.
    if (!mTemplateDlg->isCaptured(TemplateId::Electrodes))
        captureElectrodesTemplateFromCurrentVolume();

    const auto* s = mTemplateDlg->slot(TemplateId::Electrodes);
    if (!s || !s->hasData())
        return;

    // Запоминаем текущий volume и делаем «картинку для просмотра».
    mImageBeforeElectrodes = mImage;
    mElectrodesPreviewImage = cloneImage(mImage);
    if (!mElectrodesPreviewImage)
        return;

    Volume volPreview;
    volPreview.clear();
    volPreview.copy(mElectrodesPreviewImage);

    if (!volPreview.raw() || !volPreview.u8().valid)
        return;



    Volume templ = s->data;
    if (!templ.raw() || !templ.u8().valid)
        return;

    size_t total = templ.u8().size();
    for (size_t i = 0; i < total; ++i)
    {
        const uint8_t v = templ.at(i);
        if (v < HistMax - 1)
            templ.at(i) = 0;
        else
            templ.at(i) = HistMax - 1;
    }
    if (mRemoveConn)
    {
        mRemoveConn->AddBy6Neighbors(templ, HistMax - 1);
    }

    total = std::min(volPreview.u8().size(), templ.u8().size());
    for (size_t i = 0; i < total; ++i)
    {
        const uint8_t tv = templ.at(i);
        const uint8_t tvp = volPreview.at(i);
        if (tv >= HistMax - 1 && tvp)
            volPreview.at(i) = tv;
    }

    if (volPreview.raw())
        volPreview.raw()->Modified();

    mImage = volPreview.raw();
    mElectrodesPreviewActive = true;

    if (mVolume && mVolume->GetProperty())
    {
        auto* prop = mVolume->GetProperty();

        // Сохраняем текущую TF, чтобы вернуть её при выходе из режима электродов.
        mElectrodesPreviewSavedCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
        mElectrodesPreviewSavedOTF = vtkSmartPointer<vtkPiecewiseFunction>::New();
        if (auto* c = prop->GetRGBTransferFunction(0))
            mElectrodesPreviewSavedCTF->DeepCopy(c);
        if (auto* o = prop->GetScalarOpacity(0))
            mElectrodesPreviewSavedOTF->DeepCopy(o);

        // В режиме электродов: весь объём серый, электроды (250..255) — красные.
        auto previewCTF = vtkSmartPointer<vtkColorTransferFunction>::New();
        previewCTF->SetColorSpaceToLab();
        previewCTF->AddRGBPoint(DataMin, 0.0, 0.0, 0.0);
        previewCTF->AddRGBPoint(DataMax - 10.0, 1.0, 1.0, 1.0);
        previewCTF->AddRGBPoint(DataMax - 5.0, 1.0, 0.0, 0.0);
        previewCTF->AddRGBPoint(DataMax, 1.0, 0.0, 0.0);
        prop->SetColor(0, previewCTF);

        if (mInterpolation == VolumeInterpolation::Linear)
            prop->SetInterpolationTypeToLinear();
        else
            prop->SetInterpolationTypeToNearest();
    }


    setMapperInput(mImage);
    updateElectrodePickContext();

    if (mVtk && mVtk->renderWindow())
        mVtk->renderWindow()->Render();
}

void RenderView::endElectrodesPreview()
{
    if (!mElectrodesPreviewActive)
        return;

    if (mImageBeforeElectrodes)
    {
        mImage = mImageBeforeElectrodes;

        mImage->Modified();

        if (mVolume && mVolume->GetProperty())
        {
            auto* prop = mVolume->GetProperty();
            if (mElectrodesPreviewSavedCTF)
                prop->SetColor(0, mElectrodesPreviewSavedCTF);
            if (mElectrodesPreviewSavedOTF)
                prop->SetScalarOpacity(0, mElectrodesPreviewSavedOTF);

            if (mInterpolation == VolumeInterpolation::Linear)
                prop->SetInterpolationTypeToLinear();
            else
                prop->SetInterpolationTypeToNearest();
        }

        setMapperInput(mImage);
        updateElectrodePickContext();
    }

    mElectrodesPreviewImage = nullptr;
    mImageBeforeElectrodes = nullptr;
    mElectrodesPreviewActive = false;
    mElectrodesPreviewSavedCTF = nullptr;
    mElectrodesPreviewSavedOTF = nullptr;

    updateUndoRedoUi();
    if (mHistDlg && mHistDlg->isVisible())
        mHistDlg->refreshFromImage(mImage);
    if (mVtk && mVtk->renderWindow())
        mVtk->renderWindow()->Render();
}


void RenderView::setElectrodesUiActive(bool on)
{

    if (mBtnTF)    mBtnTF->setVisible(!on);
    if (mBtnTools) mBtnTools->setVisible(!on);
    if (mBtnUndo) mBtnUndo->setVisible(!on);
    if (mBtnRedo) mBtnRedo->setVisible(!on);
    if (mBtnShift) mBtnShift->setVisible(!on);

    if (mBtnClip) mBtnClip->setChecked(false);

    if (mBtnSTL) 
    {
        mBtnSTL->setChecked(false);
        mBtnSTL->setVisible(!on);
        if (mBtnSTLSimplify) mBtnSTLSimplify->setVisible(false);
        if (mBtnSTLSave)     mBtnSTLSave->setVisible(false);
    }

    if (on) {
        if (mToolActive) {
            if (mScissors)   mScissors->cancel();
            if (mContour)    mContour->cancel();
            if (mRemoveConn) mRemoveConn->cancel();
            setToolUiActive(false, mCurrentTool);
        }
        clearStlPreview();
    }

    if (mElectrodePanel)
    {
        mElectrodePanel->setModeEnabled(on);
        mElectrodePanel->setVisible(on);
        mElectrodePanel->SetDicomInfo(DI);

        if (auto* panel = mElectrodeOverlay ? mElectrodeOverlay->findChild<QWidget*>("TopElectrodPanel") : nullptr) {
            if (panel->layout()) panel->layout()->invalidate();
            panel->adjustSize();
        }

        if (on)
            QTimer::singleShot(0, this, [this] { updateElectrodeOverlayMask(); });
        else if (mElectrodeOverlay)
        {
            mElectrodePanel->endPick();
            mElectrodeOverlay->clearMask();

            if (mRenderer)
                ElectrodeSurfaceDetector::instance().clear(mRenderer);
        }
    }
    if (!on)
        updateTopPanelForStlMode(mStlModeController.isActive());

    repositionOverlay();
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

void RenderView::ensureElectrodPanel()
{
    if (mTemplateDlg) return;

    mTemplateDlg = new TemplateDialog(this, mImage, &DI);

    connect(mTemplateDlg, &TemplateDialog::requestCapture,
        this, &RenderView::onTemplateCapture);

    connect(mTemplateDlg, &TemplateDialog::requestSetVisible,
        this, &RenderView::onTemplateSetVisible);

    connect(mTemplateDlg, &TemplateDialog::requestClear,
        this, &RenderView::onTemplateClear);

    connect(mTemplateDlg, &TemplateDialog::requestClearAll,
        this, &RenderView::onTemplateClearAll);

    connect(mTemplateDlg, &QObject::destroyed, this, [this] {
        mTemplateDlg = nullptr;
        });

    connect(mTemplateDlg, &TemplateDialog::requestClearScene,
        this, &RenderView::onTemplateClearScene);

    mTemplateDlg->setOnFinished([this] {
        if (!mHistDlg || !mHistDlg->isVisible())
            setAppUiActive(false, mCurrentApp);
        });
}

void RenderView::ensureTemplateDialog()
{
    if (mTemplateDlg) return;

    mTemplateDlg = new TemplateDialog(this, mImage, &DI);

    connect(mTemplateDlg, &TemplateDialog::requestCapture,
        this, &RenderView::onTemplateCapture);

    connect(mTemplateDlg, &TemplateDialog::requestSetVisible,
        this, &RenderView::onTemplateSetVisible);

    connect(mTemplateDlg, &TemplateDialog::requestClear,
        this, &RenderView::onTemplateClear);

    connect(mTemplateDlg, &TemplateDialog::requestClearAll,
        this, &RenderView::onTemplateClearAll);

    connect(mTemplateDlg, &QObject::destroyed, this, [this] {
        mTemplateDlg = nullptr;
        });

    connect(mTemplateDlg, &TemplateDialog::requestClearScene,
        this, &RenderView::onTemplateClearScene);

    mTemplateDlg->setOnFinished([this] {
        if (!mHistDlg || !mHistDlg->isVisible())
            setAppUiActive(false, mCurrentApp);
        });
}

void RenderView::openTemplate()
{
    if (!mImage || !mVolume) return;

    ensureTemplateDialog();

    mTemplateDlg->show();
    mTemplateDlg->raise();
    mTemplateDlg->activateWindow();
}

void RenderView::onTemplateCapture(TemplateId id)
{
    if (!mTemplateDlg) return;

    qDebug() << (int)mLastTemplateForStl;

    Volume m_vol;
    m_vol.clear();
    m_vol.copy(mImage);
    if (!m_vol.raw()) return;

    mTemplateDlg->setCaptured(id, m_vol);
}

QString getParentPath(const QString& path)
{
    QFileInfo fi(path);

    if (fi.isDir())
    {
        QDir dir(fi.absoluteFilePath());
        if (dir.cdUp())
            return dir.absolutePath();
        return QString(); // уже корень
    }
    else
    {
        return fi.absolutePath(); // папка файла
    }
}

QString RenderView::defaultStlDirectory() const
{
    const QString dicomDir = DI.DicomDirPath.trimmed();
    QString parentDir = getParentPath(dicomDir);

    if (!parentDir.isEmpty() && QDir(parentDir).exists())
        return QDir(parentDir).filePath("chamber0");



    QSettings s;
    return s.value(
        "Paths/LastStlDir",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
    ).toString();
}

QString RenderView::defaultStlFileName() const
{
    switch (resolveTemplateForStl())
    {
    case TemplateId::LA:
    case TemplateId::LA_Endo:
        return "heart0.stl";
    case TemplateId::LV:
    case TemplateId::LV_Endo:
        return "heart1.stl";
    case TemplateId::RA:
    case TemplateId::RA_Endo:
        return "heart2.stl";
    case TemplateId::RV:
    case TemplateId::RV_Endo:
        return "heart3.stl";
    default:
        return "tors.stl";
    }
}

void RenderView::onTemplateSetVisible(TemplateId id, bool on)
{
    if (!mTemplateDlg) return;

    if (mBtnSTL && mBtnSTL->isChecked())
    {
        mBtnSTL->setChecked(false);
        onBuildStl();
    }

    mLastTemplateForStl = id;
    if (on)
        mLastEnabledTemplateForStl = id;

    const auto* s = mTemplateDlg->slot(id);
    if (!s || !s->hasData()) return;

    applyTemplateLayer(id, on);
}


void RenderView::applyTemplateLayer(TemplateId id, bool visible)
{
    Volume m_vol;
    m_vol.clear();
    m_vol.copy(mImage);
    if (!m_vol.raw()) return;

    const auto* s = mTemplateDlg->slot(id);
    Volume m_Template;
    m_Template.clear();
    m_Template = s->data;
    if (!m_Template.raw()) return;

    const auto& S = m_vol.u8();
    const int* ext = S.ext;
    const int nx = S.nx;
    const int ny = S.ny;
    const int nz = S.nz;
    const size_t total = static_cast<size_t>(nx) * ny * nz;

    for (size_t i = 0; i < total; i++)
    {
       if (visible)
        {
            if (m_Template.at(i))
                m_vol.at(i) = m_Template.at(i);
        }
        else
        {
            if (m_Template.at(i))
                m_vol.at(i) = 0u;
        }
    }

    if (m_vol.raw())
        m_vol.raw()->Modified();


    if (mVtk && mVtk->renderWindow()) 
        mVtk->renderWindow()->Render();

    commitNewImage(m_vol.raw());
}

void RenderView::onTemplateClear(TemplateId id)
{
    removeTemplateLayer(id);
}

void RenderView::onTemplateClearAll()
{
    removeAllTemplateLayers();
}

void RenderView::removeTemplateLayer(TemplateId id)
{
    auto it = mTemplateVolumes.find(id);
    if (it == mTemplateVolumes.end()) return;

    mTemplateVolumes.erase(it);
    if (mWindow) mWindow->Render();
}

void RenderView::removeAllTemplateLayers()
{
    mTemplateVolumes.clear();
    if (mWindow) mWindow->Render();
}

void RenderView::reloadElectrodes()
{
    if (mAppActive && mCurrentApp == App::Electrodes) 
    {
        setElectrodesUiActive(false);
        endElectrodesPreview();
        mLblStlSize->setVisible(false);
        mLblStlSize->setText("");
        setAppUiActive(false, mCurrentApp);
    }
}

void RenderView::reloadTemplate()
{
    if (!mImage || !mVolume) return;

    if (mTemplateDlg) {
        mTemplateDlg->close();
        mTemplateDlg = nullptr;
    }

    ensureTemplateDialog();
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
                if (!mTemplateDlg || !mTemplateDlg->isVisible())
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
    if (!im || !mVolume || !mVolume->GetMapper())
        return;

    if (auto* sc = im->GetPointData() ? im->GetPointData()->GetScalars() : nullptr)
        sc->Modified();
    im->Modified();

    if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mVolume->GetMapper()))
        gm->SetInputData(im);
    else
        mVolume->GetMapper()->SetInputDataObject(im);

    mVolume->GetMapper()->Modified();
    mVolume->Modified();
}

void RenderView::updateUndoRedoUi()
{
    if (!mBtnUndo || !mBtnRedo) return;
    const bool canUndo = mStlModeController.isActive()
        ? mStlModeController.canUndoSurface()
        : !mUndoStack.isEmpty();
    const bool canRedo = mStlModeController.isActive()
        ? mStlModeController.canRedoSurface()
        : !mRedoStack.isEmpty();

    mBtnUndo->setEnabled(canUndo);
    mBtnRedo->setEnabled(canRedo);
    mBtnUndo->setIcon(QIcon(canUndo ? ":/icons/Resources/undo-yes.svg"
        : ":/icons/Resources/undo-no.svg"));
    mBtnRedo->setIcon(QIcon(canRedo ? ":/icons/Resources/redo-yes.svg"
        : ":/icons/Resources/redo-no.svg"));
}

namespace StlDebugMaterial
{
    double frontR = 0.90;
    double frontG = 0.90;
    double frontB = 0.90;

    double frontAmbient = 0.36;
    double frontDiffuse = 0.36;
    double frontOpacity = 1.00;

    double frontSpecular = 0.09;
    double frontSpecularPower = 12.0;

    double frontSpecularR = 0.6;
    double frontSpecularG = 0.6;
    double frontSpecularB = 0.6;

    double backR = 0.6;
    double backG = 0.3;
    double backB = 0.3;

    double backAmbient = 0.63;
    double backDiffuse = 0.63;
    double backOpacity = 0.90;

    double backSpecular = 0.36;
    double backSpecularPower = 12.0;

    double backSpecularR = 0.6;
    double backSpecularG = 0.3;
    double backSpecularB = 0.3;
}

void RenderView::openStlMaterialDebugDialog()
{
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("STL material debug");
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);

    auto* lay = new QFormLayout(dlg);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    auto addSpin = [&](const QString& name, double& value, double minVal, double maxVal, double step)
        {
            auto* spin = new QDoubleSpinBox(dlg);
            spin->setRange(minVal, maxVal);
            spin->setDecimals(3);
            spin->setSingleStep(step);
            spin->setValue(value);

            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, &value](double v)
                {
                    value = v;
                    applyDebugStlMaterial();
                });

            lay->addRow(name, spin);
        };

    addSpin("front R", StlDebugMaterial::frontR, 0.0, 1.0, 0.01);
    addSpin("front G", StlDebugMaterial::frontG, 0.0, 1.0, 0.01);
    addSpin("front B", StlDebugMaterial::frontB, 0.0, 1.0, 0.01);

    addSpin("front ambient", StlDebugMaterial::frontAmbient, 0.0, 1.0, 0.01);
    addSpin("front diffuse", StlDebugMaterial::frontDiffuse, 0.0, 1.0, 0.01);
    addSpin("front opacity", StlDebugMaterial::frontOpacity, 0.0, 1.0, 0.01);

    addSpin("front specular", StlDebugMaterial::frontSpecular, 0.0, 1.0, 0.01);
    addSpin("front specular power", StlDebugMaterial::frontSpecularPower, 1.0, 80.0, 1.0);

    addSpin("specular R", StlDebugMaterial::frontSpecularR, 0.0, 1.0, 0.01);
    addSpin("specular G", StlDebugMaterial::frontSpecularG, 0.0, 1.0, 0.01);
    addSpin("specular B", StlDebugMaterial::frontSpecularB, 0.0, 1.0, 0.01);

    addSpin("back R", StlDebugMaterial::backR, 0.0, 1.0, 0.01);
    addSpin("back G", StlDebugMaterial::backG, 0.0, 1.0, 0.01);
    addSpin("back B", StlDebugMaterial::backB, 0.0, 1.0, 0.01);

    addSpin("back ambient", StlDebugMaterial::backAmbient, 0.0, 1.0, 0.01);
    addSpin("back diffuse", StlDebugMaterial::backDiffuse, 0.0, 1.0, 0.01);
    addSpin("back opacity", StlDebugMaterial::backOpacity, 0.0, 1.0, 0.01);

    addSpin("back specular", StlDebugMaterial::backSpecular, 0.0, 1.0, 0.01);
    addSpin("back specular power", StlDebugMaterial::backSpecularPower, 1.0, 80.0, 1.0);

    addSpin("back specular R", StlDebugMaterial::backSpecularR, 0.0, 1.0, 0.01);
    addSpin("back specular G", StlDebugMaterial::backSpecularG, 0.0, 1.0, 0.01);
    addSpin("back specular B", StlDebugMaterial::backSpecularB, 0.0, 1.0, 0.01);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    lay->addRow(buttons);

    dlg->resize(340, 520);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void RenderView::applyDebugStlMaterial()
{
    if (!mIsoActor)
        return;

    auto* front = mIsoActor->GetProperty();
    if (!front)
        return;

    front->SetInterpolationToPhong();

    front->SetColor(
        StlDebugMaterial::frontR,
        StlDebugMaterial::frontG,
        StlDebugMaterial::frontB
    );

    front->SetAmbient(StlDebugMaterial::frontAmbient);
    front->SetDiffuse(StlDebugMaterial::frontDiffuse);
    front->SetOpacity(StlDebugMaterial::frontOpacity);

    front->SetSpecular(StlDebugMaterial::frontSpecular);
    front->SetSpecularPower(StlDebugMaterial::frontSpecularPower);

    front->SetSpecularColor(
        StlDebugMaterial::frontSpecularR,
        StlDebugMaterial::frontSpecularG,
        StlDebugMaterial::frontSpecularB
    );

    front->BackfaceCullingOff();

    auto* back = mIsoActor->GetBackfaceProperty();
    if (!back)
    {
        vtkNew<vtkProperty> newBack;
        mIsoActor->SetBackfaceProperty(newBack);
        back = mIsoActor->GetBackfaceProperty();
    }

    if (back)
    {
        back->SetInterpolationToPhong();

        back->SetColor(
            StlDebugMaterial::backR,
            StlDebugMaterial::backG,
            StlDebugMaterial::backB
        );

        back->SetAmbient(StlDebugMaterial::backAmbient);
        back->SetDiffuse(StlDebugMaterial::backDiffuse);
        back->SetOpacity(StlDebugMaterial::backOpacity);

        back->SetSpecular(StlDebugMaterial::backSpecular);
        back->SetSpecularPower(StlDebugMaterial::backSpecularPower);

        back->SetSpecularColor(
            StlDebugMaterial::backSpecularR,
            StlDebugMaterial::backSpecularG,
            StlDebugMaterial::backSpecularB
        );

        back->Modified();
    }

    front->Modified();
    mIsoActor->Modified();

    if (mVtk && mVtk->renderWindow())
        mVtk->renderWindow()->Render();
}

static vtkSmartPointer<vtkActor> MakeSurfaceActor(vtkPolyData* pd)
{
    if (!pd) return nullptr;

    vtkNew<vtkPolyDataNormals> normals;
    normals->SetInputData(pd);
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->ComputePointNormalsOn();
    normals->ComputeCellNormalsOff();
    normals->Update();

    vtkNew<vtkPolyDataMapper> mp;
    mp->SetInputConnection(normals->GetOutputPort());
    mp->ScalarVisibilityOff();

    vtkNew<vtkActor> ac;
    ac->SetMapper(mp);

    auto* front = ac->GetProperty();
    front->SetInterpolationToPhong();

    front->SetColor(
        StlDebugMaterial::frontR,
        StlDebugMaterial::frontG,
        StlDebugMaterial::frontB
    );

    front->SetAmbient(StlDebugMaterial::frontAmbient);
    front->SetDiffuse(StlDebugMaterial::frontDiffuse);
    front->SetOpacity(StlDebugMaterial::frontOpacity);

    front->SetSpecular(StlDebugMaterial::frontSpecular);
    front->SetSpecularPower(StlDebugMaterial::frontSpecularPower);

    front->SetSpecularColor(
        StlDebugMaterial::frontSpecularR,
        StlDebugMaterial::frontSpecularG,
        StlDebugMaterial::frontSpecularB
    );

    front->BackfaceCullingOff();

    vtkNew<vtkProperty> back;
    back->SetInterpolationToPhong();

    back->SetColor(
        StlDebugMaterial::backR,
        StlDebugMaterial::backG,
        StlDebugMaterial::backB
    );

    back->SetAmbient(StlDebugMaterial::backAmbient);
    back->SetDiffuse(StlDebugMaterial::backDiffuse);
    back->SetOpacity(StlDebugMaterial::backOpacity);

    back->SetSpecular(StlDebugMaterial::backSpecular);
    back->SetSpecularPower(StlDebugMaterial::backSpecularPower);

    back->SetSpecularColor(
        StlDebugMaterial::backSpecularR,
        StlDebugMaterial::backSpecularG,
        StlDebugMaterial::backSpecularB
    );

    ac->SetBackfaceProperty(back);

    return ac;
}

void RenderView::updateAfterImageChange(bool reattachTools)
{
    if (mVolume && mVolume->GetProperty())
    {
        updateSamplingFromImage();

        if (mInterpolation == VolumeInterpolation::Linear)
            mVolume->GetProperty()->SetInterpolationTypeToLinear();
        else
            mVolume->GetProperty()->SetInterpolationTypeToNearest(); 
    }

    setMapperInput(mImage);

    if (reattachTools)
    {
        if (mRemoveConn)
            mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume, mHistMaskLo, mHistMaskHi);
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
    updateAfterImageChange(true);
}

void RenderView::pushStlUndoSnapshot()
{
    if (!mIsoMesh)
        return;

    mStlModeController.pushSurfaceUndoState(clonePolyData(mIsoMesh));

    mStlSaveUndoStack.push_back(clonePolyData(mStlSaveMesh ? mStlSaveMesh.GetPointer() : mIsoMesh.GetPointer()));
    while (mStlSaveUndoStack.size() > mHistoryLimit)
        mStlSaveUndoStack.pop_front();

    mStlStepUndoStack.push_back(mCurrentStlStep);
    while (mStlStepUndoStack.size() > mHistoryLimit)
        mStlStepUndoStack.pop_front();

    mContourVisibleUndoStack.push_back(mVisibleContoursNow);
    while (mContourVisibleUndoStack.size() > mHistoryLimit)
        mContourVisibleUndoStack.pop_front();

    mStlStepRedoStack.clear();
    mStlSaveRedoStack.clear();
    mContourVisibleRedoStack.clear();
}

void RenderView::resetStlContourHistory()
{
    mCurrentStlStep = 0;
    mStlStepUndoStack.clear();
    mStlStepRedoStack.clear();
    mStlSaveUndoStack.clear();
    mStlSaveRedoStack.clear();
    mContourVisibleUndoStack.clear();
    mContourVisibleRedoStack.clear();
    mSavedContours.clear();
    mVisibleContoursNow.clear();
    rebuildContourOverlay();
}

void RenderView::applyNewStlSurface(vtkPolyData* poly)
{
    if (!poly || poly->GetNumberOfCells() == 0)
        return;

    pushStlUndoSnapshot();
    mIsoMesh = clonePolyData(poly);
    ++mCurrentStlStep;
    rebuildContourOverlay();
}

void RenderView::addSavedContour(const QVector<std::array<double, 3>>& contourPointsWorld)
{
    if (contourPointsWorld.size() < 3)
        return;

    if (hasMatchingVisibleContour(contourPointsWorld))
        return;

    const int contourNumber = mSavedContours.size() + 1;
    mSavedContours.push_back(makeStoredContour(contourPointsWorld, contourNumber));
    mVisibleContoursNow.insert(contourNumber);
    rebuildContourOverlay();
}

bool RenderView::hasMatchingVisibleContour(const QVector<std::array<double, 3>>& contourPointsWorld) const
{
    if (contourPointsWorld.size() < 3 || mVisibleContoursNow.isEmpty())
        return false;

    for (const int contourNumber : mVisibleContoursNow)
    {
        const int contourIndex = contourNumber - 1;
        if (contourIndex < 0 || contourIndex >= mSavedContours.size())
            continue;

        if (contoursMatchGeometry(contourPointsWorld, storedContourWorldPoints(mSavedContours[contourIndex])))
            return true;
    }

    return false;
}

void RenderView::addSavedContours(const QVector<QVector<std::array<double, 3>>>& contoursWorld)
{
    for (const auto& contour : contoursWorld)
        addSavedContour(contour);
}

void RenderView::keepOnlyContoursAttachedToCurrentSurface()
{
    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0 || mVisibleContoursNow.isEmpty())
        return;

    vtkNew<vtkCellLocator> surfaceLocator;
    surfaceLocator->SetDataSet(mIsoMesh);
    surfaceLocator->BuildLocator();

    QSet<int> validContours;
    constexpr double kMaxAttachDistanceMm = 1.5;
    const double maxDist2 = kMaxAttachDistanceMm * kMaxAttachDistanceMm;

    for (const int contourNumber : mVisibleContoursNow)
    {
        const int contourIndex = contourNumber - 1;
        if (contourIndex < 0 || contourIndex >= mSavedContours.size())
            continue;

        const auto points = storedContourWorldPoints(mSavedContours[contourIndex]);
        if (points.size() < 3)
            continue;

        bool isAttached = true;
        for (const auto& p : points)
        {
            double closest[3]{};
            double dist2 = std::numeric_limits<double>::max();
            vtkIdType cellId = -1;
            int subId = 0;
            surfaceLocator->FindClosestPoint(p.data(), closest, cellId, subId, dist2);
            if (cellId < 0 || dist2 > maxDist2)
            {
                isAttached = false;
                break;
            }
        }

        if (isAttached)
            validContours.insert(contourNumber);
    }

    mVisibleContoursNow = std::move(validContours);
    rebuildContourOverlay();
}

void RenderView::rebuildContourOverlay()
{
    if (!mRenderer)
        return;

    if (mContourOverlayActor)
    {
        mRenderer->RemoveActor(mContourOverlayActor);
        mContourOverlayActor = nullptr;
    }
    mContourOverlayMapper = nullptr;
    mContourOverlayMesh = nullptr;

    if (!mStlModeController.isActive())
        return;
    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0)
        return;
    if (mSavedContours.isEmpty() || mVisibleContoursNow.isEmpty())
        return;

    vtkNew<vtkCellLocator> surfaceLocator;
    surfaceLocator->SetDataSet(mIsoMesh);
    surfaceLocator->BuildLocator();

    vtkNew<vtkPoints> points;
    vtkNew<vtkCellArray> lines;

    for (const int contourNumber : mVisibleContoursNow)
    {
        const int contourIndex = contourNumber - 1;
        if (contourIndex < 0 || contourIndex >= mSavedContours.size())
            continue;

        const auto contourPoints = storedContourWorldPoints(mSavedContours[contourIndex]);
        const int pointCount = contourPoints.size();
        if (pointCount < 3)
            continue;

        QVector<std::array<double, 3>> snappedPoints;
        snappedPoints.reserve(pointCount);
        bool contourStillOnSurface = true;
        constexpr double kMaxAttachDistanceMm = 1.5;
        const double maxDist2 = kMaxAttachDistanceMm * kMaxAttachDistanceMm;

        for (const auto& p : contourPoints)
        {
            double closest[3]{ 0.0, 0.0, 0.0 };
            double dist2 = std::numeric_limits<double>::max();
            vtkIdType cellId = -1;
            int subId = 0;
            surfaceLocator->FindClosestPoint(p.data(), closest, cellId, subId, dist2);
            if (cellId < 0 || dist2 > maxDist2)
            {
                contourStillOnSurface = false;
                break;
            }
            snappedPoints.push_back({ closest[0], closest[1], closest[2] });
        }

        if (!contourStillOnSurface || snappedPoints.size() < 3)
            continue;

        const vtkIdType startId = points->GetNumberOfPoints();
        for (const auto& p : snappedPoints)
            points->InsertNextPoint(p[0], p[1], p[2]);

        for (int i = 0; i < pointCount; ++i)
        {
            const vtkIdType a = startId + i;
            const vtkIdType b = startId + ((i + 1) % pointCount);
            lines->InsertNextCell(2);
            lines->InsertCellPoint(a);
            lines->InsertCellPoint(b);
        }
    }

    if (lines->GetNumberOfCells() == 0 || points->GetNumberOfPoints() == 0)
        return;

    mContourOverlayMesh = vtkSmartPointer<vtkPolyData>::New();
    mContourOverlayMesh->SetPoints(points);
    mContourOverlayMesh->SetLines(lines);

    vtkNew<vtkTubeFilter> tube;
    tube->SetInputData(mContourOverlayMesh);
    tube->SetRadius(0.35);
    tube->SetNumberOfSides(18);
    tube->CappingOn();
    tube->Update();

    mContourOverlayMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mContourOverlayMapper->SetInputConnection(tube->GetOutputPort());
    if (mClip && mClip->isEnabled())
    {
        if (auto planes = mClip->currentClippingPlanes())
            mContourOverlayMapper->SetClippingPlanes(planes);
    }

    mContourOverlayActor = vtkSmartPointer<vtkActor>::New();
    mContourOverlayActor->SetMapper(mContourOverlayMapper);
    mContourOverlayActor->GetProperty()->SetColor(0.95, 0.72, 0.10);
    mContourOverlayActor->GetProperty()->SetOpacity(0.95);
    mContourOverlayActor->GetProperty()->SetAmbient(0.55);
    mContourOverlayActor->GetProperty()->SetDiffuse(0.45);
    mContourOverlayActor->GetProperty()->SetSpecular(0.03);
    mContourOverlayActor->GetProperty()->SetSpecularPower(8.0);
    mContourOverlayActor->PickableOff();

    mRenderer->AddActor(mContourOverlayActor);
}


std::array<double, 3> RenderView::worldToSavedStlCoords(const std::array<double, 3>& world) const
{
    const double centerWorldX = DI.VolumeOriginX + DI.VolumeCenterX;
    const double centerWorldY = DI.VolumeOriginY + DI.VolumeCenterY;
    const double centerWorldZ = DI.VolumeOriginZ + DI.VolumeCenterZ;

    return {
        world[0] - centerWorldX,
        world[1] - centerWorldY,
        world[2] - centerWorldZ
    };
}

std::array<double, 3> RenderView::savedStlToWorldCoords(const std::array<double, 3>& saved) const
{
    const double centerWorldX = DI.VolumeOriginX + DI.VolumeCenterX;
    const double centerWorldY = DI.VolumeOriginY + DI.VolumeCenterY;
    const double centerWorldZ = DI.VolumeOriginZ + DI.VolumeCenterZ;
    return {
        saved[0] + centerWorldX,
        saved[1] + centerWorldY,
        saved[2] + centerWorldZ
    };
}

StoredContourInfo RenderView::makeStoredContour(
    const QVector<std::array<double, 3>>& contourPointsWorld,
    int contourNumber) const
{
    StoredContourInfo contour{};
    auto& header = contour.header;

    // Заголовок
    std::memcpy(header.ContourSign, "CONT0UR", 7);
    header.HeaderSize = sizeof(StoredContourHeader);
    header.Unused = 0;
    header.StoredContourPointSize = sizeof(StoredContourPoint);
    header.ContourPurpose = 0;
    header.UnVisible = 0;
    header.Width = kStoredContourWidth;
    header.ContourTypeALL = 0x80000000u;
    header.ContourColor = kStoredContourColor;
    setStoredContourName(header, QStringLiteral("Контур%1").arg(contourNumber));

    // Ограничение количества точек
    const auto fittedPoints = fitContourToStoredPointLimit(contourPointsWorld);
    header.nContourPoint = static_cast<quint32>(fittedPoints.size());
    header.TotalDataSize = header.HeaderSize + header.nContourPoint * header.StoredContourPointSize;

    vtkNew<vtkCellLocator> surfaceLocator;
    vtkPolyData* surface = mIsoMesh.GetPointer();
    if (surface && surface->GetNumberOfCells() > 0)
    {
        surfaceLocator->SetDataSet(surface);
        surfaceLocator->BuildLocator();
    }
    else
    {
        surface = nullptr;
    }

    for (int i = 0; i < fittedPoints.size(); ++i)
    {
        const auto savedPoint = worldToSavedStlCoords(fittedPoints[i]);

        ContourPoint viewPointOnSurface{
            savedPoint[0],
            savedPoint[1],
            savedPoint[2]
        };
        ContourPoint viewPointAboveNormal = viewPointOnSurface;

        double normal[3]{ 0.0, 0.0, 0.0 };
        if (closestSurfaceNormal(surface, surfaceLocator.GetPointer(), fittedPoints[i], normal))
        {
            const ContourPoint worldAboveNormal{
                fittedPoints[i][0] + normal[0] * kContourNormalOffsetMm,
                fittedPoints[i][1] + normal[1] * kContourNormalOffsetMm,
                fittedPoints[i][2] + normal[2] * kContourNormalOffsetMm
            };
            viewPointAboveNormal = worldToSavedStlCoords(worldAboveNormal);
        }

        setStoredContourPoint(
            contour.Points[i],
            savedPoint,
            viewPointOnSurface,
            viewPointAboveNormal);
    }

    return contour;
}

QVector<std::array<double, 3>> RenderView::storedContourWorldPoints(const StoredContourInfo& contour) const
{
    QVector<std::array<double, 3>> points;
    const int pointCount = std::min<int>(
        static_cast<int>(contour.header.nContourPoint),
        MaxStoredPointPerContour);
    points.reserve(pointCount);

    for (int i = 0; i < pointCount; ++i)
    {
        const auto& p = contour.Points[i].InitialContourPoint;
        points.push_back(savedStlToWorldCoords({ p.x, p.y, p.z }));
    }

    return points;
}

bool RenderView::saveContoursSidecar(const QString& stlPath) const
{
    if (stlPath.isEmpty())
        return false;

    const QFileInfo stlInfo(stlPath);
    const QString contoursPath = stlInfo.absolutePath() + "/contours";
    const QString oldTxtPath = stlInfo.absolutePath() + "/" + stlInfo.completeBaseName() + "_counters.txt";

    QSet<int> contoursToSave = mVisibleContoursNow;
    if (!contoursToSave.isEmpty() && mIsoMesh && mIsoMesh->GetNumberOfCells() > 0)
    {
        vtkNew<vtkCellLocator> surfaceLocator;
        surfaceLocator->SetDataSet(mIsoMesh);
        surfaceLocator->BuildLocator();

        QSet<int> stillVisibleOnCurrentSurface;
        constexpr double kMaxAttachDistanceMm = 1.5;
        const double maxDist2 = kMaxAttachDistanceMm * kMaxAttachDistanceMm;
        for (const int contourNumber : contoursToSave)
        {
            const int contourIndex = contourNumber - 1;
            if (contourIndex < 0 || contourIndex >= mSavedContours.size())
                continue;

            const auto points = storedContourWorldPoints(mSavedContours[contourIndex]);
            if (points.size() < 3)
                continue;

            bool isOnSurface = true;
            for (const auto& p : points)
            {
                double closest[3]{};
                double dist2 = std::numeric_limits<double>::max();
                vtkIdType cellId = -1;
                int subId = 0;
                surfaceLocator->FindClosestPoint(p.data(), closest, cellId, subId, dist2);
                if (cellId < 0 || dist2 > maxDist2)
                {
                    isOnSurface = false;
                    break;
                }
            }

            if (isOnSurface)
                stillVisibleOnCurrentSurface.insert(contourNumber);
        }

        contoursToSave = std::move(stillVisibleOnCurrentSurface);
    }

    QVector<StoredContourInfo> uniqueContours;
    uniqueContours.reserve(contoursToSave.size());
    for (int contourIndex = 0; contourIndex < mSavedContours.size(); ++contourIndex)
    {
        const int contourNumber = contourIndex + 1;
        if (!contoursToSave.contains(contourNumber))
            continue;

        const auto contourPoints = storedContourWorldPoints(mSavedContours[contourIndex]);
        if (contourPoints.size() < 3)
            continue;

        bool duplicate = false;
        for (const auto& uniqueContour : uniqueContours)
        {
            if (contoursMatchGeometry(contourPoints, storedContourWorldPoints(uniqueContour)))
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            uniqueContours.push_back(mSavedContours[contourIndex]);
    }

    if (uniqueContours.isEmpty())
    {
        if (QFile::exists(contoursPath))
            QFile::remove(contoursPath);
        if (QFile::exists(oldTxtPath))
            QFile::remove(oldTxtPath);
        return true;
    }

    QSaveFile out(contoursPath);
    if (!out.open(QIODevice::WriteOnly))
        return false;

    for (int contourIndex = 0; contourIndex < uniqueContours.size(); ++contourIndex)
    {
        StoredContourInfo contour = uniqueContours[contourIndex];
        setStoredContourName(contour.header, QStringLiteral("Контур%1").arg(contourIndex + 1));

        contour.header.HeaderSize = sizeof(StoredContourHeader);
        contour.header.StoredContourPointSize = sizeof(StoredContourPoint);
        contour.header.nContourPoint = std::min<quint32>(
            contour.header.nContourPoint,
            static_cast<quint32>(MaxStoredPointPerContour));
        contour.header.TotalDataSize =
            contour.header.HeaderSize +
            contour.header.nContourPoint * contour.header.StoredContourPointSize;

        subtractStoredContourShift(contour, DI);
        swapStoredContourYZ(contour);

        const qint64 headerWritten = out.write(
            reinterpret_cast<const char*>(&contour.header),
            sizeof(StoredContourHeader));
        if (headerWritten != static_cast<qint64>(sizeof(StoredContourHeader)))
            return false;

        const qint64 pointsSize =
            static_cast<qint64>(contour.header.nContourPoint) *
            static_cast<qint64>(sizeof(StoredContourPoint));
        const qint64 pointsWritten = out.write(
            reinterpret_cast<const char*>(contour.Points),
            pointsSize);
        if (pointsWritten != pointsSize)
            return false;
    }

    if (QFile::exists(oldTxtPath))
        QFile::remove(oldTxtPath);

    return out.commit();
}

void RenderView::onUndo()
{
    if (mStlModeController.isActive())
    {
        if (!mIsoMesh)
            return;

        auto prevSurface = mStlModeController.undoSurface(mIsoMesh);
        if (!prevSurface)
            return;

        mStlSaveRedoStack.push_back(clonePolyData(mStlSaveMesh ? mStlSaveMesh.GetPointer() : mIsoMesh.GetPointer()));
        while (mStlSaveRedoStack.size() > mHistoryLimit)
            mStlSaveRedoStack.pop_front();
        if (!mStlSaveUndoStack.isEmpty())
            mStlSaveMesh = mStlSaveUndoStack.takeLast();

        mStlStepRedoStack.push_back(mCurrentStlStep);
        if (!mStlStepUndoStack.isEmpty())
            mCurrentStlStep = mStlStepUndoStack.takeLast();

        mContourVisibleRedoStack.push_back(mVisibleContoursNow);
        if (!mContourVisibleUndoStack.isEmpty())
            mVisibleContoursNow = mContourVisibleUndoStack.takeLast();

        mIsoMesh = prevSurface;
        addStlPreview();
        updateStlSizeLabel();
        updateUndoRedoUi();

        if (mContour)
            mContour->attach(mVtk, mRenderer, mIsoMesh, mIsoActor);
        if (mScissors)
            mScissors->attachSurface(mVtk, mRenderer, mIsoMesh, mIsoActor);
        rebuildContourOverlay();

        return;
    }

    if (mUndoStack.isEmpty() || !mImage) return;

    // текущий в Redo
    mRedoStack.push_back(cloneImage(mImage));
    while (mRedoStack.size() > mHistoryLimit)
        mRedoStack.pop_front();

    auto prev = mUndoStack.takeLast();

    mImage = prev;
    updateAfterImageChange(true);
}

void RenderView::onRedo()
{
    if (mStlModeController.isActive())
    {
        if (!mIsoMesh)
            return;

        auto nextSurface = mStlModeController.redoSurface(mIsoMesh);
        if (!nextSurface)
            return;

        mStlSaveUndoStack.push_back(clonePolyData(mStlSaveMesh ? mStlSaveMesh.GetPointer() : mIsoMesh.GetPointer()));
        while (mStlSaveUndoStack.size() > mHistoryLimit)
            mStlSaveUndoStack.pop_front();
        if (!mStlSaveRedoStack.isEmpty())
            mStlSaveMesh = mStlSaveRedoStack.takeLast();

        mStlStepUndoStack.push_back(mCurrentStlStep);
        if (!mStlStepRedoStack.isEmpty())
            mCurrentStlStep = mStlStepRedoStack.takeLast();

        mContourVisibleUndoStack.push_back(mVisibleContoursNow);
        if (!mContourVisibleRedoStack.isEmpty())
            mVisibleContoursNow = mContourVisibleRedoStack.takeLast();

        mIsoMesh = nextSurface;
        addStlPreview();
        updateStlSizeLabel();
        updateUndoRedoUi();

        if (mContour)
            mContour->attach(mVtk, mRenderer, mIsoMesh, mIsoActor);
        if (mScissors)
            mScissors->attachSurface(mVtk, mRenderer, mIsoMesh, mIsoActor);
        rebuildContourOverlay();

        return;
    }

    if (mRedoStack.isEmpty() || !mImage) return;

    // текущий в Undo
    mUndoStack.push_back(cloneImage(mImage));
    while (mUndoStack.size() > mHistoryLimit)
        mUndoStack.pop_front();

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
        mBtnTools->setText(Tools::ToDisplayName(a));
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
        mBtnApps->setText(Tools::ToDisplayAppName(a));
        mBtnApps->setChecked(true);
    }
    else
    {
        mBtnApps->setText(tr("Applications"));
        mBtnApps->setChecked(false);
    }
}

void RenderView::updateTopPanelForStlMode(bool stlModeOn)
{
    if (mBtnTF)
        mBtnTF->setVisible(!stlModeOn);
    if (mBtnApps)
        mBtnApps->setVisible(!stlModeOn);
    if (mBtnShift)
        mBtnShift->setVisible(!stlModeOn);
}

bool RenderView::rebuildStlFromEditedImage(vtkImageData* editedImage)
{
    if (!editedImage)
        return false;

    rebuildVisibleMaskFromImage(editedImage);
    auto nextMesh = VolumeStlExporter::BuildFromBinaryVoxelsNew(mVisibleMask, VisibleExportOptions{});
    if (!nextMesh || nextMesh->GetNumberOfCells() == 0)
        return false;

    /*mStlModeController.pushSurfaceUndoState(clonePolyData(mIsoMesh));
    mIsoMesh = clonePolyData(nextMesh);*/
    applyNewStlSurface(nextMesh);
    addStlPreview();
    updateStlSizeLabel();
    updateUndoRedoUi();
    return true;
}

bool RenderView::ToolModeChanged(Action a)
{
    if (mStlModeController.isActive() &&
        a != Action::Scissors &&
        a != Action::InverseScissors &&
        a != Action::Scissors2 &&
        a != Action::InverseScissors2 &&
        a != Action::Contour)
        return false;

    if (!mStlModeController.isActive() &&
        a == Action::Contour)
        return false;

    if (!mVolume) return false;

    if (mToolActive) {
        if (mScissors)    mScissors->cancel();
        if (mContour)     mContour->cancel();
        if (mRemoveConn)  mRemoveConn->cancel();
        setToolUiActive(false, mCurrentTool);
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (mScissors && (a == Action::Scissors || a == Action::InverseScissors ||
        a == Action::Scissors2 || a == Action::InverseScissors2))
    {
        setToolUiActive(true, a);

        if (mStlModeController.isActive())
        {
            if (!mIsoMesh || !mIsoActor)
                return false;

            mScissors->attachSurface(mVtk, mRenderer, mIsoMesh, mIsoActor);
        }
        else
        {
            mScissors->attach(mVtk, mRenderer, mImage, mVolume);
        }

        mScissors->handle(a);
        return true;
    }
    else if (mContour && a == Action::Contour)
    {
        if (!mStlModeController.isActive() || !mIsoMesh || !mIsoActor)
            return false;

        setToolUiActive(true, a);
        mContour->attach(mVtk, mRenderer, mIsoMesh, mIsoActor);
        mContour->handle(a);
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
        a == Action::PeelRecovery))
    {
        setToolUiActive(true, a);
        mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume, mHistMaskLo, mHistMaskHi);
        mRemoveConn->handle(a);
        return true;
    }
    else if (mRemoveConn && a == Action::SurfaceMapping)
    {
        if (!mImage)
            return false;

        if (!mHistDlg)
            return false;

        mHistDlg->HideRangeIfCT(mImage, 64, 255);
        setViewPreset(ViewPreset::AP);
        centerOnVolume();
        setToolUiActive(true, a);
        mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume, mHistMaskLo, mHistMaskHi);
        mRemoveConn->handle(a);
        captureElectrodesTemplateFromCurrentVolume();
        return true;
    }

    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
    return false;
}

bool RenderView::AppModeChanged(App a)
{
    if (!mVolume) return false;

    if (mCurrentApp == App::Electrodes && mAppActive)
    {
        endElectrodesPreview();
        setElectrodesUiActive(false);

        setAppUiActive(false, mCurrentApp);
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    if (a == App::Histogram)
    {
        setAppUiActive(true, a);
        openHistogram();
        return true;
    }

    if (a == App::Templates)
    {
        setAppUiActive(true, a);
        openTemplate();
        return true;
    }

    if (a == App::Electrodes)
    {
        if (!AppConfig::loadCurrent().electrodesEnabled)
            return false;

        if (mHistDlg) mHistDlg->close();
        if (mTemplateDlg) mTemplateDlg->close();

        beginElectrodesPreview();
        setAppUiActive(true, a);
        setElectrodesUiActive(true);
        
        return true;
    }

    if (mVtk && mVtk->renderWindow()) mVtk->renderWindow()->Render();
    return false;
}

#include <vtkUnsignedCharArray.h>
#include <algorithm>

void RenderView::rebuildVisibleMaskFromImage(vtkImageData* src)
{
    if (!src) {
        mVisibleMask = nullptr;
        return;
    }

    if (!mVisibleMask)
        mVisibleMask = vtkSmartPointer<vtkImageData>::New();

    // копируем геометрию + данные (неважно какой тип), потом выделим u8
    mVisibleMask->DeepCopy(src);
    mVisibleMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    auto* inScAny = src->GetPointData() ? src->GetPointData()->GetScalars() : nullptr;
    auto* outU8 = vtkUnsignedCharArray::SafeDownCast(
        mVisibleMask->GetPointData() ? mVisibleMask->GetPointData()->GetScalars() : nullptr
    );

    if (!inScAny || !outU8) {
        mVisibleMask = nullptr;
        return;
    }

    int ext[6];
    src->GetExtent(ext);

    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    // если объём слишком тонкий — проще занулить всё
    if (nx < 4 || ny < 4 || nz < 4) {
        outU8->FillValue(0);
        mVisibleMask->Modified();
        return;
    }

    const vtkIdType nPts = src->GetNumberOfPoints();
    auto* outPtr = outU8->WritePointer(0, nPts);

    // 1) делаем бинарь 0/255
    // (если src тоже u8, можно ускорить, но универсально оставим так)
    for (vtkIdType id = 0; id < nPts; ++id) 
    {
        const double v = inScAny->GetTuple1(id);
        outPtr[id] = (v != 0.0 && v >= mHistMaskLo && v <= mHistMaskHi) ? 255u : 0u;
    }

    // 2) стираем первые 2 слоя по всем граням: 0,1 и n-2,n-1
    constexpr int L = 2; // сколько слоёв стираем

    auto clampi = [](int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); };

    const int i0 = ext[0], i1 = ext[1];
    const int j0 = ext[2], j1 = ext[3];
    const int k0 = ext[4], k1 = ext[5];

    const int iL0 = i0;
    const int iL1 = i0 + (L - 1);
    const int iR0 = i1 - (L - 1);
    const int iR1 = i1;

    const int jL0 = j0;
    const int jL1 = j0 + (L - 1);
    const int jR0 = j1 - (L - 1);
    const int jR1 = j1;

    const int kL0 = k0;
    const int kL1 = k0 + (L - 1);
    const int kR0 = k1 - (L - 1);
    const int kR1 = k1;

    // helper: set voxel (i,j,k) = 0
    auto zeroAt = [&](int i, int j, int k)
        {
            int ijk[3] = { i, j, k };
            const vtkIdType id = src->ComputePointId(ijk);
            outPtr[id] = 0u;
        };

    // X-грани (левая и правая)
    for (int k = k0; k <= k1; ++k)
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = iL0; i <= iL1; ++i) zeroAt(i, j, k);
            for (int i = iR0; i <= iR1; ++i) zeroAt(i, j, k);
        }

    // Y-грани (перед/зад)
    for (int k = k0; k <= k1; ++k)
        for (int i = i0; i <= i1; ++i)
        {
            for (int j = jL0; j <= jL1; ++j) zeroAt(i, j, k);
            for (int j = jR0; j <= jR1; ++j) zeroAt(i, j, k);
        }

    // Z-грани (низ/верх)
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
        {
            for (int k = kL0; k <= kL1; ++k) zeroAt(i, j, k);
            for (int k = kR0; k <= kR1; ++k) zeroAt(i, j, k);
        }

    outU8->Modified();
    mVisibleMask->Modified();
}


void RenderView::clearStlPreview()
{
    if (!mRenderer) return;

    if (mContourOverlayActor)
    {
        mRenderer->RemoveActor(mContourOverlayActor);
        mContourOverlayActor = nullptr;
    }
    mContourOverlayMapper = nullptr;
    mContourOverlayMesh = nullptr;

    if (mIsoActor) {
        mRenderer->RemoveActor(mIsoActor);
        mIsoActor = nullptr;
    }

    if (mLblStlSize) {
        mLblStlSize->setVisible(false);
        mLblStlSize->setText("");
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
    if (mIsoActor)
        mRenderer->AddActor(mIsoActor);

    if (mClip && mIsoActor) 
    {
        const bool wasClipOn = (mBtnClip && mBtnClip->isChecked());
        mClip->attachToSTL(mIsoActor);
        if (!wasClipOn)
            mClip->resetToBounds();
        mClip->setEnabled(wasClipOn); 
        if (wasClipOn) mClip->applyNow();
    }

    rebuildContourOverlay();

    if (mVtk && mVtk->renderWindow()) 
        mVtk->renderWindow()->Render();
}

void RenderView::onStlSimplify()
{
    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0) {
        emit showWarning(tr("No surface to simplify"));
        return;
    }

    if (!mStlSaveMesh || mStlSaveMesh->GetNumberOfCells() == 0)
        mStlSaveMesh = clonePolyData(mIsoMesh);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);

    // если это первое нажатие после построения, целимся сразу в 2-4 MB (в 3 MB)
    if (!mSimplifyStarted)
    {
        //mStlModeController.pushSurfaceUndoState(clonePolyData(mIsoMesh));
        pushStlUndoSnapshot();
        mIsoMesh = VolumeStlExporter::NormalizeSurface(mIsoMesh);
        mStlSaveMesh = VolumeStlExporter::NormalizeSurface(mStlSaveMesh);
        mSimplifyTargetMB = kFirstAimMB;
        mSimplifyStarted = true;
    }
    else
    {
        // последующие нажатия: уменьшаем цель плавно
        //mStlModeController.pushSurfaceUndoState(clonePolyData(mIsoMesh));
        pushStlUndoSnapshot();
        mSimplifyTargetMB = std::max(kMinMB, mSimplifyTargetMB * kStepFactor);
    }

    const std::int64_t targetBytes = static_cast<std::int64_t>(mSimplifyTargetMB * kMB);

    const bool first = (mSimplifyTargetMB >= 2.5); // или по mSimplifyStarted==false
    const int smoothIter = first ? 16 : 10;
    const double passBand = first ? 0.12 : 0.16;;

    auto simplified = VolumeStlExporter::SimplifyToTargetBytes(mIsoMesh, targetBytes, smoothIter, passBand);
    auto saveSimplified = VolumeStlExporter::SimplifyToTargetBytes(mStlSaveMesh, targetBytes, smoothIter, passBand);
    auto nextVisibleMesh = clonePolyData(simplified);
    auto nextSaveMesh = clonePolyData(saveSimplified);

    if (!nextVisibleMesh || nextVisibleMesh->GetNumberOfCells() == 0 ||
        !nextSaveMesh || nextSaveMesh->GetNumberOfCells() == 0)
    {
        QApplication::restoreOverrideCursor();
        emit showWarning(tr("Simplify failed"));
        return;
    }

    mIsoMesh = nextVisibleMesh;
    mStlSaveMesh = nextSaveMesh;

    ++mCurrentStlStep;

    addStlPreview();
    updateStlSizeLabel();
    updateUndoRedoUi();

    if (mContour)
        mContour->attach(mVtk, mRenderer, mIsoMesh, mIsoActor);
    if (mScissors)
        mScissors->attachSurface(mVtk, mRenderer, mIsoMesh, mIsoActor);

    QApplication::restoreOverrideCursor();
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}


void RenderView::onBuildStl()
{
    if (!mImage || !mVolume || !mRenderer) {
        mStlModeController.setActive(false);
        reloadToolsMenu();
        updateUndoRedoUi();
        emit showWarning(tr("No volume to build STL"));
        if (mBtnSTL) mBtnSTL->setChecked(false);
        updateTopPanelForStlMode(false);
        if (mTopOverlay) { mTopOverlay->show(); }
        return;
    }

    if (mBtnSTL->isChecked() == false)
    {
        mStlModeController.setActive(false);
        reloadToolsMenu();
        updateUndoRedoUi();

        if (mToolActive &&
            (mCurrentTool == Action::Scissors ||
             mCurrentTool == Action::InverseScissors ||
             mCurrentTool == Action::Scissors2 ||
             mCurrentTool == Action::InverseScissors2 ||
             mCurrentTool == Action::Contour))
        {
            if (mScissors) mScissors->cancel();
            if (mContour)  mContour->cancel();
            setToolUiActive(false, mCurrentTool);
        }

        mRenderer->RemoveActor(mIsoActor);
        mIsoActor = nullptr;
        mStlSaveMesh = nullptr;
        rebuildContourOverlay();
        mVolume->SetVisibility(true);
        mPrevVolumeVisible = true;
        mBtnSTLSave->setVisible(false);
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
        mBtnSTLSimplify->setEnabled(false);

        updateTopPanelForStlMode(false);

        if (mTopOverlay) { mTopOverlay->show(); }

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

        if (mLblStlSize) {
            mLblStlSize->setVisible(false);
            mLblStlSize->setText("");
        }

        if (mScissors)
            mScissors->attach(mVtk, mRenderer, mImage, mVolume);

        emit showInfo(tr("Ready volume"));
        QTimer::singleShot(800, this, [this] {
            emit showInfo(tr("Ready"));
            });
        return;
    }

    emit showInfo(tr("Building surface…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    VisibleExportOptions opt;
    // Прогресс в статус-бар + «прокачка» событий
    opt.progress = [this](int p, const QString& msg) {
        emit showInfo(tr("%1 (%2%)").arg(msg.isEmpty() ? tr("Working…") : msg).arg(p));
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        };

    rebuildVisibleMaskFromImage(mImage);
    mIsoMesh = VolumeStlExporter::BuildFromBinaryVoxelsNew(mVisibleMask, opt);
    mStlSaveMesh = clonePolyData(mIsoMesh);
    if (isCtrlDown())
    {
        onStlSimplify();
    }

    qDebug() << "mIsoMeshNumber" << "  " << mIsoMesh->GetNumberOfCells();

    if (!mIsoMesh || mIsoMesh->GetNumberOfCells() == 0) 
    {
        mStlModeController.setActive(false);
        reloadToolsMenu();
        updateUndoRedoUi();
        if (mBtnSTL) mBtnSTL->setChecked(false);
        mStlSaveMesh = nullptr;
        mBtnSTLSave->setVisible(false);
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
        mBtnSTLSimplify->setEnabled(false);
        updateTopPanelForStlMode(false);
        if (mTopOverlay) { mTopOverlay->show(); }
        emit showInfo(tr("Ready volume"));
        QApplication::restoreOverrideCursor();
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        return;
    }

    mStlModeController.setActive(true);
    mStlModeController.resetSurfaceHistory(clonePolyData(mIsoMesh));
    resetStlContourHistory();
    addStlPreview();
    updateStlSizeLabel();

    mBtnSTLSave->setVisible(true);
    mBtnSTLSave->setEnabled(true);
    mBtnSTLSimplify->setVisible(true);
    mBtnSTLSimplify->setEnabled(true);

    if (mAppActive)
    {
        if (mCurrentApp == App::Electrodes)
        {
            endElectrodesPreview();
            setElectrodesUiActive(false);
            setAppUiActive(false, mCurrentApp);
        }
    }

    if (mToolActive &&
        mCurrentTool != Action::Scissors &&
        mCurrentTool != Action::InverseScissors &&
        mCurrentTool != Action::Scissors2 &&
        mCurrentTool != Action::InverseScissors2)
    {
        if (mScissors) mScissors->cancel();
        if (mContour)  mContour->cancel();
        if (mRemoveConn) mRemoveConn->cancel();
        setToolUiActive(false, mCurrentTool);
    }

    updateTopPanelForStlMode(true);
    if (mTopOverlay) { mTopOverlay->show(); }
    reloadToolsMenu();
    updateUndoRedoUi();
    emit showInfo(tr("Ready surface"));
    QTimer::singleShot(800, this, [this] {
        emit showInfo(tr("Ready"));
        });
    QApplication::restoreOverrideCursor();
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void RenderView::updateStlSizeLabel()
{
    if (!mLblStlSize) return;

    vtkPolyData* sizeMesh = mIsoMesh.GetPointer();
    if (!sizeMesh || sizeMesh->GetNumberOfPolys() == 0)
    {
        mLblStlSize->setVisible(false);
        mLblStlSize->setText("");
        repositionOverlay();
        return;
    }

    mLblStlSize->setText(VolumeStlExporter::makeStlSizeText(sizeMesh));
    mLblStlSize->setVisible(true);

    mLblStlSize->adjustSize();
    repositionOverlay();
}


void RenderView::onSaveBuiltStl()
{
    vtkPolyData* saveMesh = mIsoMesh.GetPointer();
    if (!saveMesh || saveMesh->GetNumberOfCells() == 0) {
        emit showWarning(tr("Nothing to save - build STL first"));
        QTimer::singleShot(800, this, [this] {
            emit showInfo(tr("Ready"));
            });
        return;
    }

    QSettings s;
    const QString defDir = defaultStlDirectory();

    if (!QDir(defDir).exists())
        QDir(defDir).mkpath(defDir);
    const QString defPath = QDir(defDir).filePath(defaultStlFileName());
    qDebug() << defPath;

    ShellFileDialog shell(this,
        QObject::tr("Save STL"),
        ServiceWindow,
        defPath,
        QObject::tr("STL binary (*.stl)")
    );

    auto* dlg = shell.fileDialog();
    dlg->setAcceptMode(QFileDialog::AcceptSave);
    dlg->setFileMode(QFileDialog::AnyFile);
    dlg->setOption(QFileDialog::ShowDirsOnly, false);
    dlg->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
    dlg->setDefaultSuffix("stl");
    dlg->setOption(QFileDialog::DontConfirmOverwrite, true);

    if (shell.exec() != QDialog::Accepted) {
        QTimer::singleShot(800, this, [this] {
            emit showInfo(tr("Ready"));
            });
        return;
    }

    QString path = dlg->selectedFiles().isEmpty() ? QString() : dlg->selectedFiles().first();
    if (path.isEmpty())
        return;

    while (path.endsWith('.')) path.chop(1);
    if (!path.endsWith(".stl", Qt::CaseInsensitive))
        path += ".stl";

    const QFileInfo outInfo(path);
    const QString outPath = outInfo.absolutePath();
    if (!QDir(outPath).exists() && !QDir().mkpath(outPath)) {
        emit showWarning(tr("Save failed"));
        return;
    }

    s.setValue("Paths/LastStlDir", QFileInfo(path).absolutePath());

    const bool ok = VolumeStlExporter::SaveStlMyBinary_NoCenter(
        saveMesh,
        path,
        DI.VolumeOriginX,
        DI.VolumeOriginY,
        DI.VolumeOriginZ,
        DI.VolumeCenterX,
        DI.VolumeCenterY,
        DI.VolumeCenterZ,
        true,
        &DI.STLCenterShiftX,
        &DI.STLCenterShiftY,
        &DI.STLCenterShiftZ
    );

    if (ok)
    {
        const bool contoursOk = saveContoursSidecar(path);
        if (!contoursOk)
            emit showWarning(tr("Saved STL, but contours file was not saved"));
        else
            emit showInfo(tr("Saved STL: %1").arg(path));
    }
    else
        emit showWarning(tr("Save failed"));

    QTimer::singleShot(800, this, [this] {
        emit showInfo(tr("Ready"));
        });
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
        mTfEditor->setModal(false);
        mTfEditor->setWindowModality(Qt::NonModal);

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
    if (mTemplateDlg)    mTemplateDlg->close();
    if (mTfEditor)    mTfEditor->close();
    
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


void RenderView::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);

    if (e && e->type() == QEvent::LanguageChange)
    {
        retranslateUi();
    }
}

void RenderView::repositionOverlay()
{
    if (!mVtk) return;

    const QRect r = mVtk->geometry();
    const int pad = 8;

    if (mScissors) mScissors->onViewResized();
    if (mContour)  mContour->onViewResized();
    if (mRemoveConn) mRemoveConn->onViewResized();

    if (mRightOverlay) {
        // важно: пересчитать размер по фактическому содержимому
        if (auto* panel = mRightOverlay->findChild<QWidget*>()) {
            panel->adjustSize();
            mRightOverlay->resize(panel->sizeHint());
        }
        else {
            mRightOverlay->adjustSize();
        }

        const QSize sz = mRightOverlay->size();
        const int x = r.right() - sz.width() - pad;
        const int y = r.top() + pad;
        mRightOverlay->move(x, y);
        mRightOverlay->raise();
    }

    if (mTopOverlay)
    {
        const int pad = 8;
        const QRect r = mVtk->geometry();

        QWidget* panel = mTopOverlay->findChild<QWidget*>("TopPanel");
        if (panel)
        {
            panel->adjustSize();
            const QSize sz = panel->sizeHint();

            mTopOverlay->setGeometry(r.left() + pad, r.top() + pad, sz.width(), sz.height());
        }
        else
        {
            mTopOverlay->adjustSize();
            const QSize sz = mTopOverlay->sizeHint();
            mTopOverlay->setGeometry(r.left() + pad, r.top() + pad, sz.width(), sz.height());
        }

        mTopOverlay->raise();
    }

    if (mElectrodeOverlay && mElectrodeOverlay->isVisible())
    {
        const int pad = 8;
        const QRect r = mVtk->geometry();

        QWidget* panel = mElectrodeOverlay->findChild<QWidget*>("TopElectrodPanel");
        if (panel)
        {
            panel->adjustSize();
            const QSize sz = panel->sizeHint();

            mElectrodeOverlay->setGeometry(r.left() + pad, r.top() + pad, sz.width(), sz.height());
        }
        else
        {
            mElectrodeOverlay->adjustSize();
            const QSize sz = mElectrodeOverlay->sizeHint();
            mElectrodeOverlay->setGeometry(r.left() + pad, r.top() + pad, sz.width(), sz.height());
        }

        mElectrodeOverlay->raise();
        updateElectrodeOverlayMask();
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

bool RenderView::isHeartChamberTemplate(TemplateId id) const
{
    switch (id)
    {
    case TemplateId::LA:
    case TemplateId::LA_Endo:
    case TemplateId::RA:
    case TemplateId::RA_Endo:
    case TemplateId::LV:
    case TemplateId::LV_Endo:
    case TemplateId::RV:
    case TemplateId::RV_Endo:
        return true;
    default:
        return false;
    }
}

TemplateId RenderView::resolveTemplateForStl() const
{
    if (!mTemplateDlg)
        return mLastTemplateForStl;

    int visibleHeartTemplates = 0;
    TemplateId singleVisibleHeartTemplate = TemplateId::Count;

    for (int i = 0; i < static_cast<int>(TemplateId::Count); ++i)
    {
        const auto id = static_cast<TemplateId>(i);
        if (!isHeartChamberTemplate(id))
            continue;

        const auto* slot = mTemplateDlg->slot(id);
        if (!slot || !slot->hasData() || !slot->visible)
            continue;

        ++visibleHeartTemplates;
        singleVisibleHeartTemplate = id;
    }

    if (visibleHeartTemplates == 1)
        return singleVisibleHeartTemplate;

    if (visibleHeartTemplates > 1 && isHeartChamberTemplate(mLastEnabledTemplateForStl))
    {
        const auto* lastEnabledSlot = mTemplateDlg->slot(mLastEnabledTemplateForStl);
        if (lastEnabledSlot && lastEnabledSlot->hasData() && lastEnabledSlot->visible)
            return mLastEnabledTemplateForStl;
    }

    return mLastTemplateForStl;
}

void RenderView::updateGradientOpacity()
{
    if (!mVolume)
        return;

    auto* prop = mVolume->GetProperty();
    if (!prop)
        return;

    if (mGradientOpacityOn)
    {
        prop->SetDisableGradientOpacity(false);
        auto gtf = vtkSmartPointer<vtkPiecewiseFunction>::New();
        gtf->AddPoint(0, 0.00);
        gtf->AddPoint(15, 0.25);
        gtf->AddPoint(60, 0.80);
        prop->SetGradientOpacity(gtf);
    }
    else 
    {
        prop->SetDisableGradientOpacity(true);
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
    case ViewPreset::L: { double realL[3]{ -L[0], -L[1], -L[2] }; setPosByDir(realL); break; }
    case ViewPreset::R: { double realR[3]{ L[0], L[1], L[2] }; setPosByDir(realR); break; }
    case ViewPreset::LAO: {
        const double a = std::sqrt(0.5);
        double realL[3]{ -L[0], -L[1], -L[2] };
        double dir[3]{ a * P[0] + a * realL[0], a * P[1] + a * realL[1], a * P[2] + a * realL[2] };
        const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int i = 0; i < 3; ++i) dir[i] /= (len > 0 ? len : 1);
        setPosByDir(dir);
        break;
    }
    case ViewPreset::RAO: {
        const double a = std::sqrt(0.5);
        double realR[3]{ L[0], L[1], L[2] };
        double dir[3]{ a * P[0] + a * realR[0], a * P[1] + a * realR[1], a * P[2] + a * realR[2] };
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

void RenderView::setVolume(vtkSmartPointer<vtkImageData> image, DicomInfo Dicom, PatientInfo info)
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
        emit showWarning(tr("Invalid image: empty extent or no scalars"));
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
        mWindow->SetAlphaBitPlanes(1);
        mWindow->SetMultiSamples(0);
        mVtk->setRenderWindow(mWindow);
        rw = mVtk->renderWindow();
    }
    pump(20);

    if (!mRenderer) {
        mRenderer = vtkSmartPointer<vtkRenderer>::New();
        rw->AddRenderer(mRenderer);
    }
    pump(25);
    // Полный сброс состояния "Электродов" при смене серии, чтобы
    // ничего не наследовалось из предыдущего объёма.
    if (mElectrodesPreviewActive)
        endElectrodesPreview();

    if (mAppActive && mCurrentApp == App::Electrodes)
    {
        setElectrodesUiActive(false);
        setAppUiActive(false, mCurrentApp);
    }

    mElectrodeIJK.clear();
    mImageBeforeElectrodes = nullptr;
    mElectrodesPreviewImage = nullptr;
    mElectrodesPreviewActive = false;
    mElectrodesPreviewSavedCTF = nullptr;
    mElectrodesPreviewSavedOTF = nullptr;

    if (mElectrodePanel)
        mElectrodePanel->resetState();

    if (mRenderer)
        ElectrodeSurfaceDetector::instance().clear(mRenderer);

    // При загрузке нового исследования всегда сбрасываем STL-предпросмотр,
    // чтобы в сцене не оставались старый меш и его оценка размера.
    clearStlPreview();
    mIsoMesh = nullptr;
    mStlSaveMesh = nullptr;
    mStlModeController.setActive(false);
    mStlModeController.resetSurfaceHistory(nullptr);
    resetStlContourHistory();
    updateTopPanelForStlMode(false);
    reloadToolsMenu();
    mSimplifyStarted = false;
    mSimplifyTargetMB = kFirstAimMB;

    if (mBtnSTL)
        mBtnSTL->setChecked(false);
    if (mBtnSTLSave) {
        mBtnSTLSave->setEnabled(false);
        mBtnSTLSave->setVisible(false);
    }
    if (mBtnSTLSimplify) {
        mBtnSTLSimplify->setEnabled(false);
        mBtnSTLSimplify->setVisible(false);
    }

    DI = Dicom;

    double sp[3]{ 1,1,1 };
    image->GetSpacing(sp);
    DI.mSpX = sp[0];
    DI.mSpY = sp[1];
    DI.mSpZ = sp[2];
    const double factor = std::clamp(mSamplingFactor, 0.5, 10.0);

    // 2) настраиваем mapper/prop
    auto mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
    mapper->SetInputData(image);
    mapper->SetBlendModeToComposite();
    mapper->SetAutoAdjustSampleDistances(false);
    const double smin = std::min({ sp[0], sp[1], sp[2] });
    mapper->SetSampleDistance(std::max(1 * smin, 0.05));
    mapper->SetUseJittering(true);


    auto ctf = vtkSmartPointer<vtkColorTransferFunction>::New();
    ctf->AddRGBPoint(HistMin, 0, 0, 0);
    ctf->AddRGBPoint(HistMax, 1, 1, 1);
    ctf->SetColorSpaceToLab();

    auto otf = vtkSmartPointer<vtkPiecewiseFunction>::New();
    otf->AddPoint(0.0, 0.0);
    otf->AddPoint(0.5, 0.0);
    otf->AddPoint(1.0, 0.02);
    otf->AddPoint(255.0, 0.8);

    auto prop = vtkSmartPointer<vtkVolumeProperty>::New();
    prop->SetIndependentComponents(true);
    prop->SetColor(0, ctf);
    prop->SetScalarOpacity(0, otf);
    prop->ShadeOn();
    prop->SetAmbient(0.15);
    prop->SetDiffuse(0.85);
    prop->SetSpecular(0.08);
    prop->SetSpecularPower(20.0);
    if (mInterpolation == VolumeInterpolation::Linear)
        prop->SetInterpolationTypeToLinear();
    else
        prop->SetInterpolationTypeToNearest();
    

    prop->SetScalarOpacityUnitDistance(std::max(1 * smin, 1e-3));
    pump(40);


    mShiftValue = 3;

    reloadElectrodes();

    // 3) собираем volume и сцену
    auto vol = vtkSmartPointer<vtkVolume>::New();
    vol->SetMapper(mapper);
    vol->SetProperty(prop);

    if (mVolume) mRenderer->RemoveViewProp(mVolume);
    mRenderer->AddVolume(vol);
    mRenderer->ResetCamera();
    pump(58);

    // 4) сохранить ссылки, преднастройки вида/клипбокса
    mImage = image;
    mVolume = vol;
    updateElectrodePickContext();

    updateGradientOpacity();

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
        mScissors->setOnSurfaceReplaced([this](vtkPolyData* poly, QVector<QVector<std::array<double, 3>>> cutContours)
            {
                if (!mStlModeController.isActive())
                {
                    if (mScissors)
                        mScissors->attach(mVtk, mRenderer, mImage, mVolume);
                    return;
                }

                if (!poly || poly->GetNumberOfCells() == 0)
                {
                    emit showWarning(tr("STL scissors failed"));
                    return;
                }

                /*if (mIsoMesh)
                    mStlModeController.pushSurfaceUndoState(clonePolyData(mIsoMesh));

                mIsoMesh = clonePolyData(poly);*/

                applyNewStlSurface(poly);
                addSavedContours(cutContours);
                keepOnlyContoursAttachedToCurrentSurface();

                addStlPreview();
                updateStlSizeLabel();
                updateUndoRedoUi();

                mScissors->attachSurface(mVtk, mRenderer, mIsoMesh, mIsoActor);
            });
        mScissors->setOnFinished([this]() {
            setToolUiActive(false, mCurrentTool);
            });
    }
    mScissors->attach(mVtk, mRenderer, mImage, mVolume);
    if (!mContour)
    {
        mContour = std::make_unique<ToolsContour>(this);
        mContour->setOnSurfaceReplaced([this](vtkPolyData* poly, QVector<std::array<double, 3>> contourPoints)
            {
                if (!mStlModeController.isActive())
                    return;

                if (!poly || poly->GetNumberOfCells() == 0)
                {
                    emit showWarning(tr("Contour cut failed"));
                    return;
                }

                applyNewStlSurface(poly);
                addSavedContour(contourPoints);
                addStlPreview();
                updateStlSizeLabel();
                updateUndoRedoUi();
                mContour->attach(mVtk, mRenderer, mIsoMesh, mIsoActor);
            });
        mContour->setOnFinished([this]() {
            setToolUiActive(false, mCurrentTool);
            });
    }
    mContour->attach(mVtk, mRenderer, mIsoMesh, mIsoActor);

    if (!mRemoveConn) 
    {
        mRemoveConn = std::make_unique<ToolsRemoveConnected>(this);
        mRemoveConn->setAllowNavigation(true);
        mRemoveConn->setOnImageReplaced([this](vtkImageData* im) 
            {
                QTimer::singleShot(800, this, [this] {
                    emit showInfo(tr("Ready"));
                    });
                emit Progress(100);
                commitNewImage(im);
                mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume, mHistMaskLo, mHistMaskHi);
            });
        mRemoveConn->Unsuccessful([this](vtkImageData* im)
            {
                QTimer::singleShot(800, this, [this] {
                    emit showInfo(tr("Ready"));
                    });
                emit Progress(100);
                mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume, mHistMaskLo, mHistMaskHi);
            });
        mRemoveConn->setOnFinished([this]() 
            {
                setToolUiActive(false, mCurrentTool);
            });
        mRemoveConn->EnsureOriginalSnapshot(mImage);

        mRemoveConn->setStatusCallback(
            [this](const QString& s)
            {
                emit showInfo(s);
            }
        );
        mRemoveConn->setProgressCallback(
            [this](const int p)
            {
                emit Progress(std::clamp(p, 0, 100));;
            }
        );
    }
    mRemoveConn->attach(mVtk, mRenderer, mImage, mVolume, mHistMaskLo, mHistMaskHi);

    mUndoStack.clear();
    mRedoStack.clear();
    updateUndoRedoUi();

    // 5) применяем разумный пресет TF по диапазону данных
    reloadHistogram();
    reloadTemplate();
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

    removeAllTemplateLayers();

    DI.patientName = info.patientName;
    DI.patientId = info.patientId;
    DI.Description = info.Description;
    DI.Sequence = info.Sequence;
    DI.SeriesNumber = info.SeriesNumber;
    DI.DicomPath = info.DicomPath;
    DI.DicomDirPath = info.DicomDirPath;


    auto sexStr = info.sex.trimmed().toLower();

    if (sexStr == "m" || sexStr == "male")
        DI.Sex = 1;
    else if (sexStr == "f" || sexStr == "female")
        DI.Sex = 2;
    else
        DI.Sex = 0;

    if (mTemplateDlg) {
        const QString tplFolder = mTemplateDlg->templatesFolderPath();
        if (!tplFolder.isEmpty() && QDir(tplFolder).exists())
            mTemplateDlg->loadAllTemplatesFromDisk(mImage);
    }

    qDebug() << "///////////////////////////////";

    double vb[6];
    mImage->GetBounds(vb);
    qDebug() << "VOLUME BOUNDS:"
        << vb[0] << vb[1]
        << vb[2] << vb[3]
        << vb[4] << vb[5];

    double origin[3];
    mImage->GetOrigin(origin);
    qDebug() << "VOLUME ORIGIN:"
        << origin[0] << origin[1] << origin[2];

    DI.VolumeOriginX = origin[0];
    DI.VolumeOriginY = origin[1];
    DI.VolumeOriginZ = origin[2];

    DI.VolumeCenterX = (vb[1] - vb[0])/2;
    DI.VolumeCenterY = (vb[3] - vb[2])/2;
    DI.VolumeCenterZ = (vb[5] - vb[4])/2;

    qDebug() << "VOLUME CENTER:"
        << DI.VolumeCenterX << DI.VolumeCenterY << DI.VolumeCenterZ;
    qDebug() << "VOLUME XYZ"
        << ext[1] << ext[0] << ext[3] << ext[2] << ext[5] << ext[4];

    updateAfterImageChange(false);
}

void RenderView::saveTemplates(QString filename)
{
    QFileInfo fi(filename);
    QString savedir = fi.absolutePath();
    if (mTemplateDlg)
        mTemplateDlg->onSaveAllTemplates(true, true, savedir);
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

void RenderView::retranslateUi()
{
    mBtnSTLSimplify->setText(tr("Simplify"));
    mBtnSTLSave->setText(tr("Save"));

    mBtnTF->setText(tr("Transfer function"));

    mBtnUndo->setToolTip(tr("Undo (Ctrl+Z)"));
    mBtnRedo->setToolTip(tr("Redo (Ctrl+Y)"));

    reloadTfMenu();
    reloadToolsMenu();
    reloadAppsMenu();
    updateToolCaptionFromState();
    updateAppCaptionFromState();
}

void RenderView::reloadToolsMenu()
{
    if (!mTopOverlay || !mBtnTools) return;

    if (mToolsMenu) { delete mToolsMenu; mToolsMenu = nullptr; }

    mToolsMenu = Tools::CreateMenu(mTopOverlay, [this](Action a) { ToolModeChanged(a); });
    Tools::MenuOptions options;
    options.scissorsOnly = mStlModeController.isActive();
    mToolsMenu = Tools::CreateMenu(mTopOverlay, [this](Action a) { ToolModeChanged(a); }, options);
    applyMenuStyle(mToolsMenu, mBtnTools->width());

    connect(mToolsMenu, &QMenu::aboutToShow, this, [this] {
        if (!mToolActive) return;
        if (mScissors)   mScissors->cancel();
        if (mContour)    mContour->cancel();
        if (mRemoveConn) mRemoveConn->cancel();
        setToolUiActive(false, mCurrentTool);
        });

    mBtnTools->setMenu(mToolsMenu);
}

void RenderView::reloadAppsMenu()
{
    if (!mTopOverlay || !mBtnApps) return;

    if (mAppsMenu) { delete mAppsMenu; mAppsMenu = nullptr; }

    const AppConfig cfg = AppConfig::loadCurrent();
    mAppsMenu = Tools::CreateAppMenu(mTopOverlay, [this](App a) { AppModeChanged(a); }, cfg.electrodesEnabled);
    applyMenuStyle(mAppsMenu, mBtnApps->width());

    connect(mAppsMenu, &QMenu::aboutToShow, this, [this] {

        // если активны электроды, корректно закрываем режим и возвращаем кнопки
        if (mAppActive && mCurrentApp == App::Electrodes)
        {
            setElectrodesUiActive(false);
            endElectrodesPreview();
            setAppUiActive(false, mCurrentApp);
        }
        });

    mBtnApps->setMenu(mAppsMenu);
}

void RenderView::updateToolCaptionFromState()
{
    if (!mBtnTools) return;

    if (mToolActive)
    {
        // если Tools::ToDisplayName умеет возвращать локализованную строку - отлично
        mBtnTools->setText(Tools::ToDisplayName(mCurrentTool));
        mBtnTools->setChecked(true);
    }
    else
    {
        mBtnTools->setText(tr("Edit"));
        mBtnTools->setChecked(false);
    }
}


void RenderView::updateAppCaptionFromState()
{
    if (!mBtnApps) return;

    if (mAppActive)
    {
        mBtnApps->setText(tr("App: %1").arg(Tools::ToDisplayAppName(mCurrentApp)));
        mBtnApps->setChecked(true);
    }
    else
    {
        mBtnApps->setText(tr("Applications"));
        mBtnApps->setChecked(false);
    }
}

void RenderView::setGradientOpacityEnabled(bool on)
{
    if (mGradientOpacityOn == on) return;   // важно, чтобы не спамить сигналами

    mGradientOpacityOn = on;
    saveRenderSettings();
    updateGradientOpacity();

    emit gradientOpacityChanged(mGradientOpacityOn);
}

void RenderView::setVolumeInterpolation(VolumeInterpolation m)
{
    mInterpolation = m;
    saveRenderSettings();

    if (mVolume && mVolume->GetProperty())
    {
        if (mInterpolation == VolumeInterpolation::Linear)
            mVolume->GetProperty()->SetInterpolationTypeToLinear();
        else
            mVolume->GetProperty()->SetInterpolationTypeToNearest();

        mVolume->GetProperty()->Modified();
    }

    if (mVtk && mVtk->renderWindow())
        mVtk->renderWindow()->Render();
}

void RenderView::onTemplateClearScene()
{
    if (!mImage || !mVolume)
        return;

    // 1) undo/redo как в commitNewImage, но без смены указателя mImage
    mUndoStack.push_back(cloneImage(mImage));
    while (mUndoStack.size() > mHistoryLimit)
        mUndoStack.pop_front();
    mRedoStack.clear();

    // 2) зануляем все скаляры
    auto* scalars = mImage->GetPointData() ? mImage->GetPointData()->GetScalars() : nullptr;
    if (!scalars)
    {
        // на всякий: если скаляров нет, создадим стандартные под текущее изображение
        // (если у тебя всегда есть scalars, этот блок почти никогда не нужен)
        mImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        scalars = mImage->GetPointData()->GetScalars();
        if (!scalars)
            return;
    }

    const vtkIdType tuples = scalars->GetNumberOfTuples();
    const int comps = scalars->GetNumberOfComponents();
    const size_t bytes = size_t(tuples) * size_t(comps) * size_t(scalars->GetDataTypeSize());

    if (bytes > 0)
        std::memset(scalars->GetVoidPointer(0), 0, bytes);

    scalars->Modified();
    mImage->Modified();

    // 3) обновляем маппер/рендер, инструменты можно не переаттачивать
    updateAfterImageChange(false);
}

void RenderView::updateElectrodePickContext()
{
    if (!mElectrodePanel || !mVtk || !mRenderer || !mImage || !mVolume)
        return;

    ElectrodePanel::PickContext ctx;
    ctx.vtkWidget = mVtk;
    ctx.renderer = mRenderer;
    ctx.image = mImage;
    ctx.volume = mVolume;
    ctx.volProp = mVolume->GetProperty();

    mElectrodePanel->setPickContext(ctx);
}

static QString electrodeIdToString(ElectrodePanel::ElectrodeId id)
{
    if (id >= ElectrodePanel::ElectrodeId::V1 && id <= ElectrodePanel::ElectrodeId::V30)
        return QString("V%1").arg(int(id) - int(ElectrodePanel::ElectrodeId::V1) + 1);

    switch (id)
    {
    case ElectrodePanel::ElectrodeId::L: return "L";
    case ElectrodePanel::ElectrodeId::R: return "R";
    case ElectrodePanel::ElectrodeId::F: return "F";
    case ElectrodePanel::ElectrodeId::N: return "N";
    default: return "?";
    }

}

static int electrodeIdToint(ElectrodePanel::ElectrodeId id)
{
    if (id >= ElectrodePanel::ElectrodeId::V1 && id <= ElectrodePanel::ElectrodeId::V30)
        return (int)id - (int)ElectrodePanel::ElectrodeId::V1 + 1;
        

    switch (id)
    {
    case ElectrodePanel::ElectrodeId::L: return 31;
    case ElectrodePanel::ElectrodeId::R: return 32;
    case ElectrodePanel::ElectrodeId::F: return 33;
    case ElectrodePanel::ElectrodeId::N: return 34;
    default: return 0;
    }
}

struct ElectrodeOut
{
    int    n;      // 1..34
    double x, y, z;
};

void RenderView::loadRenderSettings()
{
    QSettings s;
    mGradientOpacityOn = s.value(kGradOpacityKey, false).toBool();
    const int im = s.value(kInterpKey, 0).toInt();
    mInterpolation = (im == 1) ? VolumeInterpolation::Linear : VolumeInterpolation::Nearest;

    mSamplingFactor = s.value(kSamplingFactorKey, 0.35).toDouble();
    mSamplingFactor = std::clamp(mSamplingFactor, 0.5, 10.0);
}

void RenderView::saveRenderSettings()
{
    QSettings s;
    s.setValue(kGradOpacityKey, mGradientOpacityOn);
    s.setValue(kInterpKey, (mInterpolation == VolumeInterpolation::Linear) ? 1 : 0);
    s.setValue(kSamplingFactorKey, mSamplingFactor);
}

void RenderView::setSamplingFactor(double f)
{
    f = std::clamp(f, 0.5, 10.0);
    if (std::abs(mSamplingFactor - f) < 1e-9)
        return;

    mSamplingFactor = f;
    saveRenderSettings();

    updateSamplingFromImage();

    if (mVtk && mVtk->renderWindow())
        mVtk->renderWindow()->Render();

    emit samplingFactorChanged(mSamplingFactor);
}

void RenderView::updateSamplingFromImage()
{
    if (!mImage || !mVolume) return;

    double sp[3]{ 1,1,1 };
    mImage->GetSpacing(sp);
    const double smin = std::min({ sp[0], sp[1], sp[2] });

    const double factor = std::clamp(mSamplingFactor, 0.5, 10.0);
    const double sd = std::max(factor * smin, 0.05); // 0.05 чтобы не улететь в ноль
    const double ud = std::max(factor * smin, 1e-3);

    if (auto* gm = vtkGPUVolumeRayCastMapper::SafeDownCast(mVolume->GetMapper())) {
        gm->SetAutoAdjustSampleDistances(false);
        gm->SetSampleDistance(sd);
        gm->SetUseJittering(true);
        gm->Modified();
    }

    //if (auto* prop = mVolume->GetProperty()) {
    //    prop->SetScalarOpacityUnitDistance(ud);
    //    prop->Modified();
    //}
}

void RenderView::onSaveElectrodesCoords()
{
    if (!mElectrodePanel)
        return;

    const QString dicomDir = DI.DicomDirPath.trimmed();
    QString parentDir = getParentPath(dicomDir);

    QDir base(parentDir);
    if (!base.exists())
        return;

    const auto coords = mElectrodePanel->coordsIJK(); // как у тебя сейчас
    const int count = coords.size();
    if (count <= 0)
        return;

    QString fileName = "Electrod.txt";
    if (count >= 30)
        fileName = "ElectrodBodyMm.txt";
    else if (count >= 4)
        fileName = "ElectrodHeartMm.txt";

    QVector<ElectrodeOut> out;
    out.reserve(coords.size());

    for (const auto& c : coords)
    {
        const int n = electrodeIdToint(c.id);
        if (n <= 0 || n > 34)
            continue;

        out.push_back({
            n,
            c.vox[0] * DI.mSpX,
            c.vox[1] * DI.mSpY,
            c.vox[2] * DI.mSpZ
            });
    }

    std::sort(out.begin(), out.end(),
        [](const ElectrodeOut& a, const ElectrodeOut& b)
        {
            return a.n < b.n;
        });

    const QString filePath = base.filePath(fileName);

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream s(&f);
    s.setRealNumberNotation(QTextStream::FixedNotation);
    s.setRealNumberPrecision(6);

    for (const auto& e : out)
    {
        s << e.n << "\t"
            << e.x << "\t"
            << e.z << "\t"
            << e.y << "\n";
    }

    f.close();

    CustomMessageBox::information(
        this,
        tr("Save electrodes coords"),
        tr("Coordinates saved to:\n%1").arg(filePath),
        ServiceWindow);
}
