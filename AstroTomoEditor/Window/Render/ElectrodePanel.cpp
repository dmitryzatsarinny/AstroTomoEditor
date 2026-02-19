#include "ElectrodePanel.h"

#include <QToolButton>
#include <QPushButton>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QStylePainter>
#include <QStyleOptionToolButton>
#include <functional>
#include <vtkTextProperty.h>
#include <vtkCamera.h>
#include <vtkProp3D.h>

static const char* kElectrodeBtnQss =
"QToolButton{ color:#fff; background:rgba(40,40,40,110);"
" border:1px solid rgba(255,255,255,30); border-radius:6px; padding:0 8px; }"
"QToolButton:hover{ background:rgba(255,255,255,40); }"
"QToolButton:pressed{ background:rgba(255,255,255,70); }"
"QToolButton:checked{ background:rgba(0,180,100,140); }";

class BadgeClearButton : public QToolButton
{
public:
    explicit BadgeClearButton(QWidget* parent = nullptr) : QToolButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoRaise(true);
        setText("");
        setFixedSize(14, 14);
        setToolTip("Clear");
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QRect r = rect().adjusted(1, 1, -1, -1);

        // красный полупрозрачный круг
        p.setPen(QPen(QColor(255, 255, 255, 70), 1));
        p.setBrush(QColor(255, 70, 70, 150));
        p.drawEllipse(r);

        // hover: черная точка внутри (без крестика)
        if (underMouse())
        {
            QRect inner = r.adjusted(4, 4, -4, -4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 170));
            p.drawEllipse(inner);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        QToolButton::mousePressEvent(e);
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        QToolButton::mouseReleaseEvent(e);
        e->accept();
    }
};

class ElectrodePanel::ElectrodeButton : public QToolButton
{
public:
    ElectrodeButton(ElectrodePanel::ElectrodeId id,
        const QString& text,
        QWidget* parent,
        QWidget* badgeHost)
        : QToolButton(parent), mId(id), mBadgeHost(badgeHost)
    {
        setText(text);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(40, 26);
        setStyleSheet(kElectrodeBtnQss);

        mCross = new BadgeClearButton(mBadgeHost);
        mCross->hide();
        mCross->raise();

        connect(mCross, &QToolButton::clicked, this, [this] {
            if (onClear) onClear(mId);
            });

        updateCrossPos();
    }

    std::function<void(ElectrodePanel::ElectrodeId)> onClear;
    std::function<bool()> isCutVisible;
    ElectrodePanel::ElectrodeId id() const { return mId; }

    void setHasCoord(bool v)
    {
        mHasCoord = v;
        if (mCross)
            mCross->setVisible(mHasCoord);

        updateCrossPos();
        update();
    }

    bool hasCoord() const { return mHasCoord; }

    // пригодится для mask
    QRect crossGeometryInHost() const
    {
        return (mCross && mBadgeHost) ? mCross->geometry() : QRect();
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QToolButton::resizeEvent(e);
        updateCrossPos();
    }

    void moveEvent(QMoveEvent* e) override
    {
        QToolButton::moveEvent(e);
        updateCrossPos();
    }
    void paintEvent(QPaintEvent* e) override
    {
        QToolButton::paintEvent(e);

        if (mHasCoord)
        {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing, true);

            const bool cutOn = isCutVisible ? isCutVisible() : true;

            // 3: cut ON -> светло-серый
            // 4: cut OFF -> темно-серый
            QColor overlay = cutOn ? QColor(255, 255, 255, 55) : QColor(0, 0, 0, 55);
            p.fillRect(rect(), overlay);
        }

        // зелёная обводка только для режима pick (checked) и только пока нет координат
        if (!mHasCoord && isChecked() && underMouse())
        {
            QPainter p(this);
            p.setPen(QPen(QColor(120, 255, 180, 200), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
        }
    }

private:
    void updateCrossPos()
    {
        if (!mCross || !mBadgeHost) return;

        // позиция бейджа относительно кнопки
        const int outX = 6;
        const int outY = 6;

        const int xLocal = width() - mCross->width() + outX;
        const int yLocal = -outY;

        // переводим в координаты badgeHost (ElectrodePanel)
        const QPoint p = mapTo(mBadgeHost, QPoint(xLocal, yLocal));

        mCross->move(p);
        mCross->raise();

        if (auto* ep = qobject_cast<ElectrodePanel*>(mBadgeHost))
            QTimer::singleShot(0, ep, [ep] { ep->rebuildMask(); });
    }

private:
    ElectrodePanel::ElectrodeId mId;
    QWidget* mBadgeHost{ nullptr };
    BadgeClearButton* mCross{ nullptr };
    bool mHasCoord{ false };
    
};

ElectrodePanel::ElectrodePanel(QWidget* parent)
    : QWidget(parent)
{
    setVisible(false);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    buildUi();
}

void ElectrodePanel::buildUi()
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(12, 8, 0, 0);
    root->setSpacing(10);

    auto addBtn = [&](ElectrodeId id, const QString& text, QGridLayout* lay, quint16 row, quint16 column)
        {
            auto* b = new ElectrodeButton(id, text, this, this);

            connect(b, &QToolButton::clicked, this, [this, b]()
                {
                    const auto id = b->id();

                    // --- 1/2: координат нет -> toggle режима pick
                    if (!b->hasCoord())
                    {
                        if (mPicking && mPickId == id)
                        {
                            // было зелёное (2) -> выключаем и возвращаем в пустое (1)
                            endPick();
                            clearCurrent();          // снимет checked со всех
                            mCurrent = ElectrodeId::Count;
                            return;
                        }

                        // было пустое (1) -> включаем зелёное (2)
                        clearCurrent();
                        beginPick(id);
                        b->setChecked(true);         // важное: зелёный только для выбранного
                        emit electrodeChosen(id);
                        return;
                    }

                    // --- 3/4: координаты есть -> переключаем cut (светлый/тёмный), зелёный режим не трогаем
                    if (mPicking && mPickId == id)
                        endPick();

                    clearCurrent(); // на всякий случай чтобы не было зелёных при наличии координат
                    toggleCut(id);
                });

            b->isCutVisible = [this, id] {
                return mCutVisible.value(id, false);
                };

            b->onClear = [this, b](ElectrodeId)
                {
                    const auto id = b->id();

                    // если вдруг это был текущий pick
                    if (mPicking && mPickId == id)
                        endPick();

                    // вернуть вырезание этого электрода (и учесть refcount)
                    restoreCut(id);

                    mCutByElectrode.remove(id);
                    mCutCenterIJK.remove(id);
                    mCutVisible[id] = false;

                    setMarkerVisible(id, false);
                    mCutCenterWorld.remove(id);

                    // убрать координаты -> крестик исчезнет, состояние пустое
                    b->setHasCoord(false);

                    mCurrent = ElectrodeId::Count;
                    b->setChecked(false);

                    emit electrodeClearRequested(id);
                };


            mButtons.push_back(b);
            mById[id] = b;
            lay->addWidget(b, row, column);
        };


    auto* grid = new QGridLayout();
    quint8 column = 7;
    quint8 row = 10;
    quint8 vcolumn = column - 4;
    grid->setHorizontalSpacing(column);
    grid->setVerticalSpacing(row);
    grid->setContentsMargins(0, 0, 0, 0);

    int idx = int(ElectrodeId::V1);
    for (int r = 0; r < row; ++r)          //строки
    {
        for (int c = 0; c < vcolumn; ++c)      //колоноки
        {
            const int v = (r * vcolumn + c) + 1; // V1..V30
            addBtn(ElectrodeId(idx), QString("V%1").arg(v), grid, r + 1, c);
            ++idx;
        }
    }

    column--;
    addBtn(ElectrodeId::N, "N", grid, 0, column);
    column--;
    addBtn(ElectrodeId::F, "F", grid, 0, column);
    column--;
    addBtn(ElectrodeId::L, "L", grid, 0, column);
    column--;
    addBtn(ElectrodeId::R, "R", grid, 0, column);

    // ---------- ПРАВАЯ ЧАСТЬ: Save ----------
    auto* right = new QVBoxLayout();
    right->setSpacing(10);
    right->setContentsMargins(0, 0, 0, 0);

    mBtnSave = new QPushButton(tr("Save electrodes coords"), this);
    mBtnSave->setCursor(Qt::PointingHandCursor);
    mBtnSave->setFixedHeight(26);
    mBtnSave->setStyleSheet(
        "QPushButton{"
        "   background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "     stop:0 rgba(0,200,120,160), stop:1 rgba(0,160,90,140));"
        "   border:1px solid rgba(0,255,160,220);"
        "   border-radius:6px; padding:0 8px; text-align:center;}"
        "QPushButton:hover{background:rgba(0,180,100,200);}"
    );

    connect(mBtnSave, &QPushButton::clicked, this, &ElectrodePanel::saveRequested);

    right->addWidget(mBtnSave);
    right->addStretch(1);

    auto* rightWrap = new QWidget(this);
    rightWrap->setLayout(right);
    rightWrap->setFixedWidth(240); 
    root->addLayout(grid, 0);
    root->addStretch(1);
    root->addSpacing(280);
    root->addWidget(rightWrap, 0, Qt::AlignTop | Qt::AlignRight);

    QTimer::singleShot(0, this, [this] { rebuildMask(); });
}


void ElectrodePanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    QTimer::singleShot(0, this, [this] { rebuildMask(); });
}

void ElectrodePanel::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);

    if (e->type() == QEvent::LanguageChange)
    {
        retranslateUi();

        if (auto* lay = layout())
        {
            lay->invalidate();
            lay->activate();
        }

        QTimer::singleShot(0, this, [this]
            {
                for (auto* b : mButtons)
                {
                    b->setHasCoord(b->hasCoord());
                }

                rebuildMask();
            });

        update();
    }
}


void ElectrodePanel::rebuildMask()
{
    QRegion reg;

    for (auto* b : mButtons)
    {
        QRect g = b->geometry();
        g = g.adjusted(0, -8, 8, 0);
        reg |= QRegion(g);

        const QRect cb = b->crossGeometryInHost();
        if (!cb.isEmpty())
            reg |= QRegion(cb.adjusted(-2, -2, 2, 2));
    }

    if (mBtnSave)
    {
        // mBtnSave теперь может быть внутри rightWrap, поэтому мапим в координаты панели
        const QPoint topLeft = mBtnSave->mapTo(this, QPoint(0, 0));
        QRect r(topLeft, mBtnSave->size());
        reg |= QRegion(r.adjusted(0, 0, 0, 0));
    }

    setMask(reg);
}




bool ElectrodePanel::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == mPick.vtkWidget && mPicking)
    {
        if (ev->type() == QEvent::MouseMove)
        {
            auto* me = static_cast<QMouseEvent*>(ev);

            std::array<int, 3> ijk;
            std::array<double, 3> w;
            if (pickAt(me->pos(), ijk, w))
            {
                ensureHoverActor();
                const double ww[3] = { w[0], w[1], w[2] };
                setHoverAtWorld(ww);
                setHoverVisible(true);
            }
            else
            {
                setHoverVisible(false);
            }
            return false;
        }

        if (ev->type() == QEvent::MouseButtonPress)
        {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton)
            {
                std::array<int, 3> ijk;
                std::array<double, 3> w;
                if (pickAt(me->pos(), ijk, w))
                {
                    qDebug() << "[PICK] IJK:"
                        << ijk[0] << ijk[1] << ijk[2]
                        << " world:"
                        << w[0] << w[1] << w[2];

                    qDebug() << "[PICK] IJK * spacing:"
                        << ijk[0] * DI.mSpX << ijk[1] * DI.mSpY << ijk[2] * DI.mSpZ;
                    qDebug() << "[PICK] IJK * spacing + Origin in global vorld:"
                        << ijk[0] * DI.mSpX + DI.VolumeOriginX << ijk[1] * DI.mSpY + DI.VolumeOriginY << ijk[2] * DI.mSpZ + DI.VolumeOriginZ;

                    ensureHoverActor();
                    const double ww[3] = { w[0], w[1], w[2] };
                    setHoverAtWorld(ww);
                    setHoverVisible(true);

                    const auto committedId = mPickId;

                    if (committedId != ElectrodeId::Count)
                    {
                        mCutCenterIJK[committedId] = ijk;
                        mCutVisible[committedId] = true;
                        applyCut(committedId, ijk);

                        // состояние 3: координаты есть + вырезание включено
                        setHasCoord(committedId, true);

                        // центр вырезания оставляем как был (для toggleCut и т.д.)
                        mCutCenterWorld[committedId] = { w[0], w[1], w[2] };

                        // а вот маркер ставим на ближайшую поверхность ПОСЛЕ вырезания
                        std::array<double, 3> wMarker = mCutCenterWorld[committedId];
                        std::array<int, 3>    ijkMarker = ijk;

                        snapToSurfaceTowardsCamera(mCutCenterWorld[committedId], ijkMarker, wMarker);

                        updateMarker(committedId, wMarker, ijkMarker);
                        setMarkerVisible(committedId, true);

                        // зелёный режим выключаем
                        clearCurrent();
                    }

                    emit pickCommitted(committedId, ijk, w);
                    endPick();
                    return true;
                }
            }
            return false;
        }
    }

    return QWidget::eventFilter(obj, ev);
}


void ElectrodePanel::setModeEnabled(bool on)
{
    if (mEnabled == on)
        return;

    mEnabled = on;

    if (!on)
        clearAllElectrodesState();

    setVisible(on);
}

void ElectrodePanel::setHasCoord(ElectrodeId id, bool has)
{
    auto it = mById.find(id);
    if (it == mById.end()) return;
    it.value()->setHasCoord(has);
}

bool ElectrodePanel::hasCoord(ElectrodeId id) const
{
    auto it = mById.find(id);
    if (it == mById.end()) return false;
    return it.value()->hasCoord();
}

void ElectrodePanel::setCurrent(ElectrodeId id)
{
    mCurrent = id;
    for (auto* b : mButtons)
        b->setChecked(b->id() == id);
}

void ElectrodePanel::clearCurrent()
{
    for (auto* b : mButtons)
        b->setChecked(false);
}

void ElectrodePanel::retranslateUi()
{
    if (mBtnSave)
        mBtnSave->setText(tr("Save electrodes coords"));
}

QVector<ElectrodePanel::ElectrodeCoord> ElectrodePanel::coordsWorld() const
{
    QVector<ElectrodeCoord> out;
    out.reserve(mCutCenterWorld.size());

    for (auto it = mCutCenterWorld.begin(); it != mCutCenterWorld.end(); ++it)
        out.push_back({ it.key(), it.value() });

    return out;
}

QVector<ElectrodePanel::ElectrodeIJKCoord> ElectrodePanel::coordsIJK() const
{
    QVector<ElectrodeIJKCoord> out;
    out.reserve(mCutCenterIJK.size());

    for (auto it = mCutCenterIJK.begin(); it != mCutCenterIJK.end(); ++it)
        out.push_back({ it.key(), it.value() });

    return out;
}


void ElectrodePanel::setPickContext(const PickContext& ctx)
{
    mPick = ctx;

    // панель должна ловить мышь поверх vtk
    if (mPick.vtkWidget)
    {
        mPick.vtkWidget->setMouseTracking(true);
        mPick.vtkWidget->installEventFilter(this);
    }

    for (auto it = mMarkers.begin(); it != mMarkers.end(); ++it)
    {
        if (mPick.renderer)
        {
            mPick.renderer->RemoveActor(it->sphereActor);
            mPick.renderer->RemoveActor(it->textActor);
        }
    }
    mMarkers.clear();

    ensureHoverActor();
}

void ElectrodePanel::beginPick(ElectrodeId id)
{
    mPicking = true;
    mPickId = id;
    setHoverVisible(false);
}

void ElectrodePanel::endPick()
{
    mPicking = false;
    mPickId = ElectrodeId::Count;
    setHoverVisible(false);
    clearCurrent(); // чтобы зелёное точно ушло
}

void ElectrodePanel::ensureHoverActor()
{
    if (!mPick.renderer) return;
    if (mHoverActor) return;

    mHoverSphere = vtkSmartPointer<vtkSphereSource>::New();
    mHoverSphere->SetThetaResolution(24);
    mHoverSphere->SetPhiResolution(24);

    mHoverMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mHoverMapper->SetInputConnection(mHoverSphere->GetOutputPort());

    mHoverActor = vtkSmartPointer<vtkActor>::New();
    mHoverActor->SetMapper(mHoverMapper);
    mHoverActor->PickableOff();
    mHoverActor->GetProperty()->SetRepresentationToWireframe();
    mHoverActor->GetProperty()->SetLineWidth(2.0);
    mHoverActor->GetProperty()->SetColor(1.0, 0.85, 0.2); // “желтый кружок”
    mHoverActor->SetVisibility(0);

    mPick.renderer->AddActor(mHoverActor);
}

void ElectrodePanel::setHoverVisible(bool v)
{
    if (!mHoverActor) return;
    mHoverActor->SetVisibility(v ? 1 : 0);
    if (mPick.vtkWidget && mPick.vtkWidget->renderWindow())
        mPick.vtkWidget->renderWindow()->Render();
}

void ElectrodePanel::setHoverAtWorld(const double w[3])
{
    if (!mHoverSphere || !mPick.image) return;

    double sp[3]; mPick.image->GetSpacing(sp);
    const double smin = std::min({ sp[0], sp[1], sp[2] });

    const double r = 6.0 * std::min({ sp[0], sp[1], sp[2] });
    mHoverSphere->SetCenter(w);
    mHoverSphere->SetRadius(r);
    mHoverSphere->Modified();
}

bool ElectrodePanel::displayRay(const QPoint& pDevice, double outP0[3], double outP1[3]) const
{
    if (!mPick.renderer) return false;

    int winH = 0;
    double dpr = 1.0;

    if (mPick.vtkWidget && mPick.vtkWidget->renderWindow())
    {
        dpr = mPick.vtkWidget->devicePixelRatioF();

        int* sz = mPick.vtkWidget->renderWindow()->GetSize(); // <-- ВАЖНО: без аргументов
        winH = sz ? sz[1] : 0;
    }
    else if (mPick.vtkWidget)
    {
        dpr = mPick.vtkWidget->devicePixelRatioF();
        winH = int(std::lround(mPick.vtkWidget->height() * dpr));
    }

    const double x = pDevice.x() * dpr;

    double y = pDevice.y() * dpr;
    if (winH > 0)
        y = (winH - 1) - y;

    mPick.renderer->SetDisplayPoint(x, y, 0.0);
    mPick.renderer->DisplayToWorld();
    double p0[4];
    mPick.renderer->GetWorldPoint(p0);
    if (p0[3] == 0.0) return false;
    outP0[0] = p0[0] / p0[3]; outP0[1] = p0[1] / p0[3]; outP0[2] = p0[2] / p0[3];

    mPick.renderer->SetDisplayPoint(x, y, 1.0);
    mPick.renderer->DisplayToWorld();
    double p1[4];
    mPick.renderer->GetWorldPoint(p1);
    if (p1[3] == 0.0) return false;
    outP1[0] = p1[0] / p1[3]; outP1[1] = p1[1] / p1[3]; outP1[2] = p1[2] / p1[3];

    return true;
}

bool ElectrodePanel::worldToIJK(const double w[3], int ijk[3]) const
{
    if (!mPick.image) return false;

    double origin[3], sp[3];
    int dims[3];
    mPick.image->GetOrigin(origin);
    mPick.image->GetSpacing(sp);
    mPick.image->GetDimensions(dims);

    for (int i = 0; i < 3; ++i)
    {
        const double v = (w[i] - origin[i]) / sp[i];
        ijk[i] = int(std::floor(v + 0.5)); // nearest voxel
        if (ijk[i] < 0 || ijk[i] >= dims[i])
            return false;
    }
    return true;
}

double ElectrodePanel::opacityAtIJK(int ijk[3]) const
{
    if (!mPick.image || !mPick.volProp) return 0.0;

    auto* arr = mPick.image->GetPointData()->GetScalars();
    if (!arr) return 0.0;

    const vtkIdType id = mPick.image->ComputePointId(ijk);
    const double s = arr->GetComponent(id, 0);

    auto* of = mPick.volProp->GetScalarOpacity();
    if (!of) return 0.0;

    return of->GetValue(s);
}

void ElectrodePanel::requestRender()
{
    if (mPick.vtkWidget && mPick.vtkWidget->renderWindow())
        mPick.vtkWidget->renderWindow()->Render();
}


bool ElectrodePanel::pickAt(const QPoint& pDevice,
    std::array<int, 3>& outIJK,
    std::array<double, 3>& outW) const
{
    if (!mPick.image || !mPick.volProp || !mPick.renderer) return false;


    double p0[3], p1[3];
    if (!displayRay(pDevice, p0, p1))
        return false;

    // шаг по миру: ~половина минимального spacing
    double sp[3]; mPick.image->GetSpacing(sp);
    const double step = 0.5 * std::min({ sp[0], sp[1], sp[2] });

    const double dir[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len <= 1e-9) return false;

    const double v[3] = { dir[0] / len, dir[1] / len, dir[2] / len };

    // ограничим дальность, чтобы не сканить бесконечно
    const double maxT = len;

    // порог “ткань есть”
    const double opThr = 0.01;

    for (double t = 0.0; t <= maxT; t += step)
    {
        double w[3] = { p0[0] + v[0] * t, p0[1] + v[1] * t, p0[2] + v[2] * t };

        int ijk[3];
        if (!worldToIJK(w, ijk))
            continue;

        const double op = opacityAtIJK(ijk);
        if (op > opThr)
        {
            outIJK = { ijk[0], ijk[1], ijk[2] };
            outW = { w[0], w[1], w[2] };
            return true;
        }
    }

    return false;
}

void ElectrodePanel::applyCut(ElectrodeId id, const std::array<int, 3>& cIJK)
{
    if (!mPick.image) return;

    // если для этого электрода уже было вырезано - сначала убрать его вклад (но не восстановить чужие)
    restoreCut(id);

    auto* arr = mPick.image->GetPointData()->GetScalars();
    if (!arr) return;

    int dims[3];
    mPick.image->GetDimensions(dims);

    const int R = std::max(1, mEraseRadiusVox);
    const int cx = cIJK[0], cy = cIJK[1], cz = cIJK[2];
    const int R2 = R * R;

    QVector<vtkIdType> affected;
    affected.reserve((2 * R + 1) * (2 * R + 1) * (2 * R + 1));

    auto inside = [&](int x, int y, int z)
        {
            return x >= 0 && y >= 0 && z >= 0 && x < dims[0] && y < dims[1] && z < dims[2];
        };

    for (int z = cz - R; z <= cz + R; ++z)
        for (int y = cy - R; y <= cy + R; ++y)
            for (int x = cx - R; x <= cx + R; ++x)
            {
                if (!inside(x, y, z)) continue;

                const int dx = x - cx, dy = y - cy, dz = z - cz;
                if (dx * dx + dy * dy + dz * dz > R2) continue;

                int ijk[3] = { x, y, z };

                if (mEraseOpThr > 0.0 && mPick.volProp)
                {
                    const double op = opacityAtIJK(ijk);
                    if (op <= mEraseOpThr)
                        continue;
                }

                const vtkIdType pid = mPick.image->ComputePointId(ijk);
                const double v = arr->GetComponent(pid, 0);

                if (v < mEraseMin || v > mEraseMax)
                    continue;

                auto& g = mGlobalCut[pid];
                if (g.ref == 0)
                {
                    g.original = v;
                    arr->SetComponent(pid, 0, 0.0); // режем
                }
                g.ref++;

                affected.push_back(pid);
            }

    if (affected.isEmpty())
        return;

    mCutByElectrode[id] = std::move(affected);
    mPick.image->Modified();
    requestRender();
}

void ElectrodePanel::restoreCut(ElectrodeId id)
{
    if (!mPick.image) return;

    auto it = mCutByElectrode.find(id);
    if (it == mCutByElectrode.end() || it->isEmpty())
        return;

    auto* arr = mPick.image->GetPointData()->GetScalars();
    if (!arr) return;

    auto& affected = it.value();

    for (vtkIdType pid : affected)
    {
        auto git = mGlobalCut.find(pid);
        if (git == mGlobalCut.end())
            continue;

        auto& g = git.value();
        if (g.ref <= 0)
            continue;

        g.ref--;

        // если это был последний "держатель" дырки -> вернуть исходное значение
        if (g.ref == 0)
        {
            arr->SetComponent(pid, 0, g.original);
            mGlobalCut.erase(git);
        }
    }

    affected.clear();
    mPick.image->Modified();
    requestRender();
}

void ElectrodePanel::toggleCut(ElectrodeId id)
{
    // нет центра -> нечего показывать/скрывать
    if (!mCutCenterIJK.contains(id))
        return;

    const bool vis = mCutVisible.value(id, false);

    if (vis)
    {
        restoreCut(id);
        mCutVisible[id] = false;
        setMarkerVisible(id, false);   // <-- состояние 4
    }
    else
    {
        applyCut(id, mCutCenterIJK[id]);
        mCutVisible[id] = true;

        auto itW = mCutCenterWorld.find(id);
        if (itW != mCutCenterWorld.end())
            updateMarker(id, itW.value(), mCutCenterIJK[id]);

        setMarkerVisible(id, true);    // <-- состояние 3
        requestRender();
    }

}

static bool computeNormalFromScalars(vtkImageData* img, vtkDataArray* arr,
    const std::array<int, 3>& ijk,
    double outN[3])
{
    if (!img || !arr) return false;

    int dims[3]; img->GetDimensions(dims);

    auto inside = [&](int x, int y, int z) {
        return x >= 0 && y >= 0 && z >= 0 && x < dims[0] && y < dims[1] && z < dims[2];
        };

    const int x = ijk[0], y = ijk[1], z = ijk[2];
    if (!inside(x, y, z)) return false;

    auto sample = [&](int xi, int yi, int zi)->double {
        int p[3]{ xi,yi,zi };
        const vtkIdType pid = img->ComputePointId(p);
        return arr->GetComponent(pid, 0);
        };

    // центральные разности, с подстраховкой на границах
    const int xm = inside(x - 1, y, z) ? x - 1 : x;
    const int xp = inside(x + 1, y, z) ? x + 1 : x;
    const int ym = inside(x, y - 1, z) ? y - 1 : y;
    const int yp = inside(x, y + 1, z) ? y + 1 : y;
    const int zm = inside(x, y, z - 1) ? z - 1 : z;
    const int zp = inside(x, y, z + 1) ? z + 1 : z;

    const double gx = sample(xp, y, z) - sample(xm, y, z);
    const double gy = sample(x, yp, z) - sample(x, ym, z);
    const double gz = sample(x, y, zp) - sample(x, y, zm);

    double n[3]{ gx, gy, gz };
    const double L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (L < 1e-9) return false;

    outN[0] = n[0] / L; outN[1] = n[1] / L; outN[2] = n[2] / L;
    return true;
}

static QString electrodeLabel(ElectrodePanel::ElectrodeId id)
{
    // Подстрой под твой enum! Я предполагаю что V1..V30 идут подряд.
    if (id >= ElectrodePanel::ElectrodeId::V1 && id <= ElectrodePanel::ElectrodeId::V30)
    {
        const int v = int(id) - int(ElectrodePanel::ElectrodeId::V1) + 1;
        return QString("V%1").arg(v);
    }

    switch (id)
    {
    case ElectrodePanel::ElectrodeId::N: return "N";
    case ElectrodePanel::ElectrodeId::F: return "F";
    case ElectrodePanel::ElectrodeId::L: return "L";
    case ElectrodePanel::ElectrodeId::R: return "R";
    default: return "?";
    }
}

static bool worldNormalFromIJK(vtkImageData* img, const int ijk[3], double nWorld[3])
{
    if (!img) return false;

    auto* arr = img->GetPointData() ? img->GetPointData()->GetScalars() : nullptr;
    if (!arr) return false;

    int dims[3]; img->GetDimensions(dims);
    auto inside = [&](int x, int y, int z) {
        return x >= 0 && y >= 0 && z >= 0 && x < dims[0] && y < dims[1] && z < dims[2];
        };

    auto sample = [&](int x, int y, int z)->double {
        int p[3]{ x,y,z };
        vtkIdType id = img->ComputePointId(p);
        return arr->GetComponent(id, 0);
        };

    int x = ijk[0], y = ijk[1], z = ijk[2];
    if (!inside(x, y, z)) return false;

    // центральные разности (в индексном пространстве)
    double gx = (inside(x + 1, y, z) ? sample(x + 1, y, z) : sample(x, y, z)) -
        (inside(x - 1, y, z) ? sample(x - 1, y, z) : sample(x, y, z));
    double gy = (inside(x, y + 1, z) ? sample(x, y + 1, z) : sample(x, y, z)) -
        (inside(x, y - 1, z) ? sample(x, y - 1, z) : sample(x, y, z));
    double gz = (inside(x, y, z + 1) ? sample(x, y, z + 1) : sample(x, y, z)) -
        (inside(x, y, z - 1) ? sample(x, y, z - 1) : sample(x, y, z));

    // перевод в мир: учесть spacing и direction matrix (если есть)
    double sp[3]{ 1,1,1 }; img->GetSpacing(sp);
    double gData[3]{ gx / std::max(sp[0],1e-9), gy / std::max(sp[1],1e-9), gz / std::max(sp[2],1e-9) };

    vtkMatrix3x3* M = img->GetDirectionMatrix();
    if (M)
    {
        // world = M * data
        nWorld[0] = M->GetElement(0, 0) * gData[0] + M->GetElement(0, 1) * gData[1] + M->GetElement(0, 2) * gData[2];
        nWorld[1] = M->GetElement(1, 0) * gData[0] + M->GetElement(1, 1) * gData[1] + M->GetElement(1, 2) * gData[2];
        nWorld[2] = M->GetElement(2, 0) * gData[0] + M->GetElement(2, 1) * gData[1] + M->GetElement(2, 2) * gData[2];
    }
    else
    {
        nWorld[0] = gData[0]; nWorld[1] = gData[1]; nWorld[2] = gData[2];
    }

    double L = std::sqrt(nWorld[0] * nWorld[0] + nWorld[1] * nWorld[1] + nWorld[2] * nWorld[2]);
    if (L < 1e-9) return false;
    nWorld[0] /= L; nWorld[1] /= L; nWorld[2] /= L;
    return true;
}


void ElectrodePanel::ensureMarker(ElectrodeId id)
{
    if (!mPick.renderer) return;
    if (mMarkers.contains(id)) return;

    Marker3D mk;

    mk.sphere = vtkSmartPointer<vtkSphereSource>::New();
    mk.sphere->SetThetaResolution(24);
    mk.sphere->SetPhiResolution(24);

    mk.sphereMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mk.sphereMapper->SetInputConnection(mk.sphere->GetOutputPort());

    mk.sphereActor = vtkSmartPointer<vtkActor>::New();
    mk.sphereActor->SetMapper(mk.sphereMapper);
    mk.sphereActor->PickableOff();
    mk.sphereActor->GetProperty()->SetOpacity(0.9);
    mk.sphereActor->GetProperty()->SetColor(0.2, 0.9, 0.35); // чуть зеленоватая сфера
    mk.sphereActor->SetForceOpaque(true);
    mk.sphereActor->SetVisibility(0);
    
    
    mk.textActor = vtkSmartPointer<vtkBillboardTextActor3D>::New();
    mk.textActor->GetTextProperty()->SetFontSize(18);
    mk.textActor->GetTextProperty()->SetBold(1);
    mk.textActor->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
    mk.textActor->SetForceOpaque(true);
    mk.textActor->GetTextProperty()->SetJustificationToCentered();
    mk.textActor->GetTextProperty()->SetVerticalJustificationToCentered();
    mk.textActor->PickableOff();
    mk.textActor->SetVisibility(0);
    

    mPick.renderer->AddActor(mk.sphereActor);
    mPick.renderer->AddActor(mk.textActor);

    mMarkers.insert(id, mk);
}

void ElectrodePanel::setMarkerVisible(ElectrodeId id, bool vis)
{
    auto it = mMarkers.find(id);
    if (it == mMarkers.end()) return;
    it->sphereActor->SetVisibility(vis ? 1 : 0);
    it->textActor->SetVisibility(vis ? 1 : 0);
    requestRender();
}

void ElectrodePanel::updateMarker(ElectrodeId id,
    const std::array<double, 3>& w,
    const std::array<int, 3>& ijk)
{
    ensureMarker(id);
    auto it = mMarkers.find(id);
    if (it == mMarkers.end()) return;

    double sp[3]{ 1,1,1 };
    if (mPick.image) mPick.image->GetSpacing(sp);
    const double smin = std::min({ sp[0], sp[1], sp[2] });

    // размер сферы
    const double r = 10.0 * smin;

    // 1) СФЕРА: хотим, чтобы поверхность шара была на поверхности модели,
    // поэтому центр утапливаем внутрь по направлению от камеры.
    double nCam[3]{ 0,0,1 };
    if (auto* cam = (mPick.renderer ? mPick.renderer->GetActiveCamera() : nullptr))
    {
        double cpos[3]{};
        cam->GetPosition(cpos);

        double v[3]{ cpos[0] - w[0], cpos[1] - w[1], cpos[2] - w[2] };
        const double L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (L > 1e-9) { nCam[0] = v[0] / L; nCam[1] = v[1] / L; nCam[2] = v[2] / L; }
    }

    // коэффициент утопления: 1.0 = ровно на радиус (верх шара примерно в точке w)
    // 0.8..1.1 подбирай по ощущению
    const double sink = 0.95;

    const double c[3]{
        w[0] - nCam[0] * (r * sink),
        w[1] - nCam[1] * (r * sink),
        w[2] - nCam[2] * (r * sink)
    };

    it->sphere->SetCenter(c[0], c[1], c[2]);
    it->sphere->SetRadius(r);
    it->sphere->Modified();

    // -----------------------------
    // 2) НОРМАЛЬ (для подписи)
    // -----------------------------
    double n[3]{ 0,0,1 };
    bool hasN = false;

    if (mPick.image)
    {
        if (auto* arr = mPick.image->GetPointData()->GetScalars())
            hasN = computeNormalFromScalars(mPick.image, arr, ijk, n);
    }

    if (!hasN)
    {
        // fallback: к камере
        auto* cam = (mPick.renderer ? mPick.renderer->GetActiveCamera() : nullptr);
        double pos[3]{};
        if (cam) cam->GetPosition(pos);

        double v[3]{ pos[0] - w[0], pos[1] - w[1], pos[2] - w[2] };
        const double L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (L > 1e-9) { n[0] = v[0] / L; n[1] = v[1] / L; n[2] = v[2] / L; }
    }

    // -----------------------------
    // 3) ТЕКСТ: на поверхности сферы со стороны камеры
    // -----------------------------

    if (auto* cam = (mPick.renderer ? mPick.renderer->GetActiveCamera() : nullptr))
    {
        double cpos[3]{};
        cam->GetPosition(cpos);

        double v[3]{ cpos[0] - w[0], cpos[1] - w[1], cpos[2] - w[2] };
        const double L = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (L > 1e-9) { nCam[0] = v[0] / L; nCam[1] = v[1] / L; nCam[2] = v[2] / L; }
    }

    // чуть наружу от поверхности, чтобы не было z-fighting со сферой
    const double textOff = r * 0.5;

    const double tp[3]{
        w[0] + nCam[0] * textOff,
        w[1] + nCam[1] * textOff,
        w[2] + nCam[2] * textOff
    };

    const QByteArray lbl = electrodeLabel(id).toUtf8();
    it->textActor->SetInput(lbl.constData());
    it->textActor->SetPosition(tp[0], tp[1], tp[2]);

    requestRender();
}

void ElectrodePanel::clearAllElectrodesState()
{
    // 0) если был активен pick
    endPick();

    // 1) восстановить все вырезания, которые были применены
    // (важно: restoreCut сам снимает refcount с mGlobalCut)
    for (auto it = mCutByElectrode.begin(); it != mCutByElectrode.end(); ++it)
        restoreCut(it.key());

    mCutByElectrode.clear();
    mCutCenterIJK.clear();
    mCutCenterWorld.clear();
    mCutVisible.clear();
    mGlobalCut.clear();

    // 2) скрыть hover
    setHoverVisible(false);

    // 3) скрыть/сбросить маркеры (сферы+текст)
    for (auto it = mMarkers.begin(); it != mMarkers.end(); ++it)
    {
        it->sphereActor->SetVisibility(0);
        it->textActor->SetVisibility(0);
    }

    // если хочешь прям полностью убрать из рендера, а не просто скрыть:
    if (mPick.renderer)
    {
        for (auto it = mMarkers.begin(); it != mMarkers.end(); ++it)
        {
            mPick.renderer->RemoveActor(it->sphereActor);
            mPick.renderer->RemoveActor(it->textActor);
        }
    }
    mMarkers.clear();

    // 4) сбросить состояние кнопок (крестики, “есть координаты”, checked)
    for (auto* b : mButtons)
    {
        b->setHasCoord(false);
        b->setChecked(false);
    }

    mCurrent = ElectrodeId::Count;

    requestRender();
}

bool ElectrodePanel::snapToSurfaceTowardsCamera(
    const std::array<double, 3>& w0,
    std::array<int, 3>& inOutIJK,
    std::array<double, 3>& outW) const
{
    if (!mPick.image || !mPick.renderer || !mPick.volProp)
        return false;

    auto* cam = mPick.renderer->GetActiveCamera();
    if (!cam) return false;

    double cpos[3]{};
    cam->GetPosition(cpos);

    // направление к камере
    double dir[3]{ cpos[0] - w0[0], cpos[1] - w0[1], cpos[2] - w0[2] };
    double L = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (L < 1e-9) return false;
    dir[0] /= L; dir[1] /= L; dir[2] /= L;

    double sp[3]{ 1,1,1 };
    mPick.image->GetSpacing(sp);
    const double smin = std::min({ sp[0], sp[1], sp[2] });

    const double step = 0.5 * smin;
    const double maxDist = 80.0 * smin;   // подстрой (например 5-10 мм в мире)

    const double opThr = 0.01;

    // идем от точки клика к камере, ищем ближайший "не пустой" воксель
    for (double t = 0.0; t <= maxDist; t += step)
    {
        double w[3]{ w0[0] + dir[0] * t, w0[1] + dir[1] * t, w0[2] + dir[2] * t };

        int ijk[3];
        if (!worldToIJK(w, ijk))
            continue;

        // важно: после applyCut в вырезанной зоне скаляры уже 0
        auto* arr = mPick.image->GetPointData()->GetScalars();
        if (!arr) return false;

        const vtkIdType pid = mPick.image->ComputePointId(ijk);
        const double s = arr->GetComponent(pid, 0);
        if (s <= 0.0)
            continue;

        const double op = opacityAtIJK(ijk);
        if (op <= opThr)
            continue;

        // нашли поверхность
        inOutIJK = { ijk[0], ijk[1], ijk[2] };
        outW = { w[0], w[1], w[2] };
        return true;
    }

    return false;
}