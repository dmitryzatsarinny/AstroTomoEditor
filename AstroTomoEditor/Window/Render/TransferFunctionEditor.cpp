#include "TransferFunctionEditor.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QMouseEvent>
#include <QDialogButtonBox>
#include <algorithm>
#include <QPainterPath>
#include <QInputDialog>
#include <QMessageBox>

#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>

// ===== холст: гистограмма + кривая с точками ===============================

static constexpr double kGuardLeft = -1.0;
static constexpr double kGuardRight = 256.0;

static QVector<double> smoothBox(const QVector<quint64>& h, int win = 9)
{
    const int n = h.size();
    if (n == 0) return {};
    win = std::max(1, win | 1);          // нечётное окно: 1,3,5,7,9...
    const int R = win / 2;

    auto at = [&](int i)->double {        // отражение индекса
        if (i < 0)      i = -i;
        if (i >= n)     i = 2 * (n - 1) - i;
        // на всякий случай, если n==1
        if (i < 0) i = 0; if (i >= n) i = n - 1;
        return double(h[i]);
        };

    QVector<double> out(n, 0.0);

    // стартовое окно: сумма по [-R .. +R] ВКЛЮЧИТЕЛЬНО
    double sum = 0.0;
    for (int k = -R; k <= R; ++k) sum += at(k);
    out[0] = sum / double(win);

    // скользим вправо: прибавили (i+R), вычли (i-1-R)
    for (int i = 1; i < n; ++i) {
        sum += at(i + R) - at(i - 1 - R);
        out[i] = sum / double(win);
    }

    return out;
}

class TfCanvas : public QWidget {
    Q_OBJECT
public:
    explicit TfCanvas(QWidget* p = nullptr) : QWidget(p) {
        setMouseTracking(true);
        setMinimumHeight(240);
    }

    void setHistogram(const QVector<quint64>& h) {
        hist_ = h;
        recomputeHistMax_();
        update();
    }
    void setPoints(const QVector<TfPoint>& pts, int sel = -1) { pts_ = pts; sel_ = sel; update(); }
    QVector<TfPoint> points() const { return pts_; }
    int  selected() const { return sel_; }
    void setSelected(int i) { sel_ = i; update(); }
    void setLogHistogram(bool on) { log_ = on; update(); } 
    void setHistogramHeadroom(double frac) {
        headroom_ = std::clamp(frac, 0.0, 0.9); // 0..90%
        recomputeHistMax_();
        update();
    }
signals:
    void changed();              // любые изменения (перемещение/добавление/удаление)
    void selectionChanged(int);  // изм. выбора

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(22, 23, 26));

        const QRectF r = rect().adjusted(8, 8, -8, -8);
        // сетка
        p.setPen(QColor(255, 255, 255, 25));
        for (int i = 1; i < 8; ++i) {
            const qreal x = r.left() + r.width() * i / 8.0;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        // гистограмма
        if (!hist_.isEmpty()) {
            // сглаживаем ЛИНЕЙНЫЕ счётчики
            const QVector<double> sLin = smoothBox(hist_);

            // максимум берём из НЕсглаженной гистограммы (без бина 0)
            auto val = [&](double v) { return log_ ? std::log1p(v) : v; };

            double mRaw = 0.0;
            for (int i = 1; i < hist_.size(); ++i) mRaw = std::max(mRaw, val(double(hist_[i])));
            if (mRaw <= 0.0) mRaw = 1.0;

            // «крышу» делаем на 90% от этого максимума
            constexpr double kHeadroom = 0.10;
            const double denom = std::max(1e-12, mRaw * (1.0 - kHeadroom));

            QPainterPath area;
            area.moveTo(r.left(), r.bottom());
            for (int i = 0; i < 256; ++i) {
                const qreal x = r.left() + (i / 255.0) * r.width();
                const double nv = val(i < sLin.size() ? sLin[i] : 0.0) / denom;
                const qreal y = r.bottom() - (r.height() * std::min(1.0, nv));
                area.lineTo(x, y);
            }
            area.lineTo(r.right(), r.bottom());
            area.closeSubpath();
            p.fillPath(area, QColor(255, 255, 255, 48));
        }

        // линия TF
        if (pts_.size() >= 2) {
            QPainterPath path;
            auto toPt = [&](const TfPoint& tp) {
                const qreal t = tp.x / 255.0;
                const qreal x = r.left() + t * r.width();
                const qreal y = r.bottom() - tp.a * r.height();
                return QPointF(x, y);
                };
            path.moveTo(toPt(pts_.front()));
            for (int i = 1; i < pts_.size(); ++i) path.lineTo(toPt(pts_[i]));
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(255, 255, 255, 200), 2.0));
            p.drawPath(path);

            // точки
            for (int i = 0; i < pts_.size(); ++i) {
                const QPointF c = toPt(pts_[i]);
                const QColor  col = pts_[i].color;
                p.setBrush(col);
                p.setPen(QPen(i == sel_ ? QColor(255, 255, 255) : QColor(255, 255, 255, 170), 1.5));
                p.drawEllipse(c, 5, 5);
            }
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        const int hit = pickPoint(e->pos());
        if (e->button() == Qt::LeftButton) {
            if (hit >= 0) {
                sel_ = hit; dragging_ = true; dragOff_ = project(e->pos()) - QPointF(pts_[sel_].x, pts_[sel_].a * 255.0);
                emit selectionChanged(sel_);
                update();
            }
            else if (rect().adjusted(8, 8, -8, -8).contains(e->pos())) {
                const QPointF pa = project(e->pos());
                TfPoint np;
                np.x = std::clamp<double>(pa.x(), 0, 255);
                np.a = std::clamp<double>(pa.y() / 255.0, 0, 1);
                np.color = Qt::white;

                pts_.push_back(np);
                std::sort(pts_.begin(), pts_.end(), [](auto& a, auto& b) { return a.x < b.x; });

                // выбираем ближайшую к добавленной координату
                auto nearestIdx = 0;
                double bestD2 = std::numeric_limits<double>::infinity();
                for (int k = 0; k < pts_.size(); ++k) {
                    const double dx = pts_[k].x - np.x;
                    const double dy = (pts_[k].a - np.a) * 255.0;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < bestD2) { bestD2 = d2; nearestIdx = k; }
                }
                sel_ = nearestIdx;

                emit selectionChanged(sel_);
                emit changed();
                update();
            }
        }
        else if (e->button() == Qt::RightButton && hit >= 0 && pts_.size() > 2) {
            if (hit == 0 || hit == pts_.size() - 1) return;  // не удаляем крайние
            pts_.removeAt(hit);
            sel_ = -1;
            emit selectionChanged(sel_);
            emit changed();
            update();
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!dragging_ || sel_ < 0) return;
        QPointF pa = project(e->pos()) - dragOff_;
        const double newX = std::clamp<double>(pa.x(), 0, 255);
        const double newA = std::clamp<double>(pa.y() / 255.0, 0, 1);

        double x = newX;
        if (sel_ == 0)        x = 0.0;     // левый узел зафиксирован по X
        if (sel_ == pts_.size() - 1) x = 255.0;  // правый узел зафиксирован по X

        pts_[sel_].x = x;
        pts_[sel_].a = newA;

        std::sort(pts_.begin(), pts_.end(), [](auto& a, auto& b) { return a.x < b.x; });

        // переоцениваем индекс выбранной точки: берём ближайшую к (newX,newA)
        int nearestIdx = 0;
        double bestD2 = std::numeric_limits<double>::infinity();
        for (int k = 0; k < pts_.size(); ++k) {
            const double dx = pts_[k].x - newX;
            const double dy = (pts_[k].a - newA) * 255.0;
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; nearestIdx = k; }
        }
        sel_ = nearestIdx;

        emit changed();
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }

private:
    int pickPoint(const QPoint& mpos) const {
        const QRectF r = rect().adjusted(8, 8, -8, -8);
        auto toPt = [&](const TfPoint& tp) {
            const qreal x = r.left() + (tp.x / 255.0) * r.width();
            const qreal y = r.bottom() - tp.a * r.height();
            return QPointF(x, y);
            };
        for (int i = int(pts_.size()) - 1; i >= 0; --i) {
            if (QLineF(mpos, toPt(pts_[i])).length() <= 7.5) return i;
        }
        return -1;
    }
    QPointF project(const QPoint& mp) const {
        const QRectF r = rect().adjusted(8, 8, -8, -8);
        const qreal t = std::clamp((mp.x() - r.left()) / r.width(), 0.0, 1.0);
        const qreal u = 1.0 - std::clamp((mp.y() - r.top()) / r.height(), 0.0, 1.0);
        return QPointF(t * 255.0, u * 255.0);
    }

    void recomputeHistMax_() {
        maxLin_ = 0.0;
        for (int v : hist_) if (v > maxLin_) maxLin_ = double(v);

        // для лог-режима считаем максимум уже по log1p
        maxLog_ = 0.0;
        for (int v : hist_) {
            double lv = std::log1p(double(v));
            if (lv > maxLog_) maxLog_ = lv;
        }

        // защита от нуля
        if (maxLin_ <= 0.0) maxLin_ = 1.0;
        if (maxLog_ <= 0.0) maxLog_ = 1.0;
    }
    QVector<quint64> hist_;
    QVector<TfPoint> pts_;
    int   sel_ = -1;
    bool  dragging_ = false;
    QPointF dragOff_;
    bool log_ = false;
    double headroom_ = 0.10;
    double maxLin_ = 1.0;
    double maxLog_ = 1.0;

};

#include "TransferFunctionEditor.moc"  // для TfCanvas signals/slots
#include "TransferFunction.h"

// ===== TransferFunctionEditor ===============================================

static QVector<quint64> buildHistogram256Fast(vtkImageData* img,
    int sampleStep = 2,
    bool ignoreZeros = true,
    double zeroEps = 1e-12)
{
    QVector<quint64> h(256, 0);
    if (!img) return h;


    int ext[6];
    img->GetExtent(ext);

    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    vtkIdType incX, incY, incZ;  // ВНИМАНИЕ: ИНКРЕМЕНТЫ В БАЙТАХ
    img->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    auto* p0 = static_cast<const uint8_t*>(
        img->GetScalarPointer(ext[0], ext[2], ext[4]));

    for (int i = 0; i < nx * ny * nz; i++)
        h[(int)*(p0 + i)]++;

    if (ignoreZeros)
        h[0] = 0;

    //const int step = 1; // без субсэмплинга

    //// тройной цикл с byte-increments
    //const uint8_t* pz = p0;
    //for (int k = 0; k < nz; k += step) {
    //    const uint8_t* py = pz;
    //    for (int j = 0; j < ny; j += step) {
    //        const uint8_t* px = py;
    //        for (int i = 0; i < nx; i += step) {
    //            uint8_t v = *px;
    //            int bin = std::clamp((int)v, (int)0, (int)255);
    //            ++h[bin];
    //            px += incX * step;   // смещение по X — в байтах
    //        }
    //        py += incY * step;       // смещение по Y — в байтах
    //    }
    //    pz += incZ * step;           // смещение по Z — в байтах
    //}

    if (ignoreZeros)
       h[0] = 0;
    return h;
}

static QString sliderCss(const QColor& color)
{
    const QString c = QString("rgb(%1,%2,%3)")
        .arg(color.red()).arg(color.green()).arg(color.blue());

    return
        "QSlider::groove:horizontal {"
        "  height:6px; border-radius:3px;"
        "  margin: 0 8px;"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "               stop:0 rgb(0,0,0), stop:1 " + c + ");"
        "}"
        "QSlider::handle:horizontal {"
        "  width:12px; height:12px;"
        "  margin:-4px 0;"
        "  border-radius:6px;"
        "  background: white;"
        "  border:1px solid rgba(0,0,0,120);"
        "}";
}

TransferFunctionEditor::TransferFunctionEditor(QWidget* parent, vtkImageData* imgU8)
    : QDialog(parent)
{
    setWindowTitle(tr("Transfer Function"));
    setModal(true);

    const QSize base(760, 420); 
    resize(sizeHint().expandedTo(base));

    //double rr[2]{ 0,255 };
    //if (imgU8) imgU8->GetScalarRange(rr);
    //mMin = rr[0];
    //mMax = rr[1];

    mHist = buildHistogram256Fast(imgU8, /*step=*/2, /*ignoreZeros=*/true, /*eps=*/1e-12);
    mMin = 0;
    mMax = 255;

    // дефолтные точки
    mPts = {
        {  0, 0.00, QColor(0,0,0) },
        { 64, 0.25, QColor(96,96,96) },
        {160, 0.45, QColor(200,200,200)},
        {255, 0.90, QColor(255,255,255)}
    };

    auto* v = new QVBoxLayout(this);

    mCanvas = new TfCanvas(this);
    mCanvas->setHistogram(mHist);
    mCanvas->setPoints(mPts, /*sel*/1);
    v->addWidget(mCanvas, 1);

    // RGB
    auto makeSlider = [&](const char* name)->QSlider* {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel(name, row);
        auto* s = new QSlider(Qt::Horizontal, row);
        s->setRange(0, 255); s->setSingleStep(1); s->setPageStep(8);
        h->addWidget(lbl); h->addWidget(s, 1);
        v->addWidget(row);
        return s;
        };
    mR = makeSlider("R"); mG = makeSlider("G"); mB = makeSlider("B");

    // раскраска треков
    mR->setStyleSheet(sliderCss(QColor(255, 0, 0)));
    mG->setStyleSheet(sliderCss(QColor(0, 255, 0)));
    mB->setStyleSheet(sliderCss(QColor(0, 0, 255)));


    // плашка текущего цвета
    mSwatch = new QLabel(this);
    mSwatch->setFixedHeight(16);
    mSwatch->setStyleSheet("background:#808080; border:1px solid rgba(255,255,255,60);");
    v->addWidget(mSwatch);

    auto bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto btnAuto = bb->addButton("Auto", QDialogButtonBox::ActionRole);
    auto btnSave = bb->addButton("Save preset…", QDialogButtonBox::ActionRole);
    v->addWidget(bb);

    connect(mCanvas, &TfCanvas::changed, this, &TransferFunctionEditor::onCanvasChanged);
    connect(mCanvas, &TfCanvas::selectionChanged, this, [this](int i) {
        mSel = i;
        if (i < 0 || i >= mPts.size()) return;  // ← защита от выхода за границы

        const QColor c = mPts[i].color;
        mR->blockSignals(true); mG->blockSignals(true); mB->blockSignals(true);
        mR->setValue(c.red());  mG->setValue(c.green()); mB->setValue(c.blue());
        mR->blockSignals(false); mG->blockSignals(false); mB->blockSignals(false);
        mSwatch->setStyleSheet(QString("background:rgb(%1,%2,%3); border:1px solid rgba(255,255,255,60);")
            .arg(c.red()).arg(c.green()).arg(c.blue()));
        });

    connect(mR, &QSlider::valueChanged, this, &TransferFunctionEditor::onRgbChanged);
    connect(mG, &QSlider::valueChanged, this, &TransferFunctionEditor::onRgbChanged);
    connect(mB, &QSlider::valueChanged, this, &TransferFunctionEditor::onRgbChanged);

    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        rebuildPreview(false);
        auto* c = makeCTF(mPts);
        auto* o = makeOTF(mPts);
        emit committed(c, o);
        c->Delete(); o->Delete();
        accept();
        });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(btnAuto, &QPushButton::clicked, this, &TransferFunctionEditor::onAutoColors);

    connect(btnSave, &QPushButton::clicked, this, [this] {
        bool ok = true;
        QString name = QInputDialog::getText(this, tr("Save TF preset"),
            tr("Name:"), QLineEdit::Normal,
            "My Preset", &ok);
        if (!ok || name.isEmpty()) return;

        TF::CustomPreset P;
        P.name = name;
        P.opacityK = 1.0;
        P.colorSpace = "Lab";
        P.points.reserve(mPts.size());
        for (const auto& t : mPts) {
            TF::TFPoint q; q.x = t.x; q.a = t.a;
            q.r = t.color.redF(); q.g = t.color.greenF(); q.b = t.color.blueF();
            P.points.push_back(q);
        }
        if (!TF::SaveCustomPreset(P))
            QMessageBox::warning(this, tr("Save preset"), tr("Failed to save preset file."));
        else
            emit presetSaved();
        });


    // инициализируем выбор
    if (mCanvas->points().size() > 1)
        emit mCanvas->selectionChanged(1);
    else if (!mCanvas->points().isEmpty())
        emit mCanvas->selectionChanged(0);
    rebuildPreview(true);
}

void TransferFunctionEditor::onCanvasChanged()
{
    mPts = mCanvas->points();
    rebuildPreview(true);
}

void TransferFunctionEditor::onRgbChanged()
{
    if (mSel < 0 || mSel >= mPts.size()) return;
    QColor c(mR->value(), mG->value(), mB->value());
    mPts[mSel].color = c;
    mSwatch->setStyleSheet(QString("background:rgb(%1,%2,%3); border:1px solid rgba(255,255,255,60);")
        .arg(c.red()).arg(c.green()).arg(c.blue()));
    mCanvas->setPoints(mPts, mSel);
    rebuildPreview(true);
}

void TransferFunctionEditor::onAutoColors()
{
    if (mHist.size() != 256) { rebuildPreview(true); return; }

    // --- 1) чуть сгладим линейную гистограмму для ДЕТЕКТА (узкое окно)
    const QVector<double> s = smoothBox(mHist, /*win=*/5);

    // глобальный максимум без нулевого бина
    double smax = 0.0;
    for (int i = 1; i < s.size() - 1; ++i) smax = std::max(smax, s[i]);
    if (smax <= 0.0) { rebuildPreview(true); return; }

    struct Peak { int x, l, r; double v, prom; int w; };
    QVector<Peak> cand;

    // --- 2) кандидаты: простые локальные максимумы
    for (int i = 1; i <= 254; ++i)
        if (s[i] > s[i - 1] && s[i] >= s[i + 1])
            cand.push_back({ i, -1, -1, s[i], 0.0, 0 });

    if (cand.isEmpty()) { rebuildPreview(true); return; }

    // --- 3) устойчивые седловины (лево/право)
    auto leftMin = [&](int i) {
        int j = i - 1;
        while (j > 1 && s[j - 1] >= s[j]) --j; // спуск
        while (j > 1 && s[j - 1] <= s[j]) --j; // плато минимума
        return std::max(1, j);
        };
    auto rightMin = [&](int i) {
        int j = i + 1;
        while (j < 254 && s[j + 1] >= s[j]) ++j;
        while (j < 254 && s[j + 1] <= s[j]) ++j;
        return std::min(254, j);
        };

    // --- 4) prominence + ширина на половине prominence (FWHM-подобная)
    auto halfWidth = [&](int i, int L, int R, double v, double base)->int {
        const double h = v - base;
        const double th = v - 0.5 * h;     // уровень половины проминентности
        int xl = i, xr = i;
        // влево до пересечения с th
        for (int k = i - 1; k >= L; --k) { xl = k; if (s[k] <= th) break; }
        // вправо до пересечения с th
        for (int k = i + 1; k <= R; ++k) { xr = k; if (s[k] <= th) break; }
        return std::max(1, xr - xl);
        };

    for (auto& p : cand) {
        p.l = leftMin(p.x);
        p.r = rightMin(p.x);
        const double base = std::max(s[p.l], s[p.r]);
        p.prom = p.v - base;
        p.w = halfWidth(p.x, p.l, p.r, p.v, base);
    }

    // --- 5) отбор: динамическое подавление близнецов
    const double minProm = smax * 0.06;      // 6% от глобального max — мягче
    std::sort(cand.begin(), cand.end(),
        [](const Peak& A, const Peak& B) { return A.prom > B.prom; });

    QVector<Peak> peaks;
    QVector<char> taken(256, 0);

    auto deepValley = [&](const Peak& a, const Peak& b)->bool {
        // глубина седловины между пиками
        const int L = std::min(a.x, b.x), R = std::max(a.x, b.x);
        double valley = s[a.x]; int idx = a.x;
        for (int k = L; k <= R; ++k) if (s[k] < valley) { valley = s[k]; idx = k; }
        const double low = std::min(a.v, b.v);
        return (valley <= low * 0.55);   // седловина опускается хотя бы до 55% от меньшего пика
        };

    for (const auto& p : cand) {
        if (p.prom < minProm) continue;

        // окно подавления зависит от ширины пика
        const int nms = std::max(6, int(0.6 * p.w));

        bool clash = false;
        for (int k = std::max(1, p.x - nms); k <= std::min(254, p.x + nms); ++k)
            if (taken[k]) { clash = true; break; }

        if (clash) {
            // попробуем сохранить оба, если между ними глубокая седловина
            bool allow = false;
            for (const auto& q : peaks) {
                if (std::abs(q.x - p.x) <= std::max(6, int(0.6 * std::min(p.w, q.w)))) {
                    if (deepValley(p, q)) { allow = true; break; }
                }
            }
            if (!allow) continue;
        }

        peaks.push_back(p);
        for (int k = std::max(1, p.x - nms); k <= std::min(254, p.x + nms); ++k)
            taken[k] = 1;

        if (peaks.size() >= 8) break;   // ограничим число пиков
    }

    if (peaks.isEmpty()) { rebuildPreview(true); return; }
    std::sort(peaks.begin(), peaks.end(), [](auto& A, auto& B) { return A.x < B.x; });

    // --- 6) СБОРКА ТОЧЕК (как у тебя было)
    QVector<TfPoint> pts;
    pts.push_back(TfPoint{ 0, 0.0, QColor(255,255,255) });
    const QVector<QColor> palette = {
        QColor(80,  80, 255), QColor(80, 200, 255),
        QColor(80, 220, 120), QColor(240, 220,  80),
        QColor(250, 150, 120), QColor(230,  90, 200)
    };
    auto push = [&](int x, double a, QColor c) {
        x = std::clamp(x, 0, 255); a = std::clamp(a, 0.0, 1.0);
        if (!pts.isEmpty() && pts.back().x == x) { pts.back().a = a; pts.back().color = c; }
        else pts.push_back(TfPoint{ (double)x, a, c });
        };
    const double aLow = 0.02, aHigh = 0.85;

    for (int i = 0; i < peaks.size(); ++i) {
        const auto& pk = peaks[i];
        const QColor col = palette[i % palette.size()];
        push(pk.l, aLow, col);
        push(pk.x, aHigh, col);
        push(pk.r, aLow, col);
    }
    push(255, 0.0, QColor(255, 255, 255));

    std::sort(pts.begin(), pts.end(), [](auto& A, auto& B) { return A.x < B.x; });
    QVector<TfPoint> uniq; uniq.reserve(pts.size());
    for (const auto& t : pts) {
        if (!uniq.isEmpty() && uniq.back().x == t.x) uniq.back() = t; else uniq.push_back(t);
    }

    mPts = uniq;
    mSel = (mPts.size() > 1 ? 1 : 0);
    mCanvas->setPoints(mPts, mSel);
    rebuildPreview(true);
    update();
}


void TransferFunctionEditor::rebuildPreview(bool emitPreview)
{
    if (!emitPreview) return;
    auto* c = makeCTF(mPts);
    auto* o = makeOTF(mPts);
    emit preview(c, o);
    c->Delete(); o->Delete();
}

void TransferFunctionEditor::refreshHistogram(vtkImageData* img)
{
    double minPhys = 0, maxPhys = 255;
    mHist = buildHistogram256Fast(img, /*step=*/2, /*ignoreZeros=*/true, /*eps=*/1e-12);
    // Синхронизируем ось редактора с реальным диапазоном данных
    mMin = minPhys;
    mMax = maxPhys;

    mCanvas->setHistogram(mHist); // пересчитает maxLin/maxLog и перерисует
}

void TransferFunctionEditor::setFromVtk(vtkColorTransferFunction* ctf,
    vtkPiecewiseFunction* otf,
    double minVal, double maxVal)
{
    // диапазон переводим в нашу шкалу 0..255
    const double a = minVal, b = maxVal, d = (b > a ? b - a : 1.0);

    // Цветовые стопы
    if (ctf && ctf->GetSize() >= 2) {
        QVector<TfPoint> pts;
        pts.reserve(ctf->GetSize());
        for (int i = 0, n = ctf->GetSize(); i < n; ++i) {
            double node[6]; ctf->GetNodeValue(i, node); // x,r,g,b,mid,sharp
            TfPoint p;
            p.x = std::clamp((node[0] - a) * 255.0 / d, 0.0, 255.0);
            p.a = 0.0; // заполним из otf ниже
            p.color = QColor::fromRgbF(node[1], node[2], node[3]);
            pts.push_back(p);
        }
        std::sort(pts.begin(), pts.end(), [](auto& A, auto& B) { return A.x < B.x; });
        mPts = pts;
    }

    // Точки непрозрачности — подмешиваем в mPts по X
    if (otf && otf->GetSize() >= 2) {
        // если цветовых точек мало — построим сетку из otf
        if (mPts.size() < 2) {
            mPts.clear(); mPts.reserve(otf->GetSize());
            for (int i = 0, n = otf->GetSize(); i < n; ++i) {
                double node[4]; otf->GetNodeValue(i, node); // x,val,mid,sharp
                TfPoint p;
                p.x = std::clamp((node[0] - a) * 255.0 / d, 0.0, 255.0);
                p.a = std::clamp(node[1], 0.0, 1.0);
                p.color = Qt::white;
                mPts.push_back(p);
            }
        }
        else {
            // есть цветовые — интерполируем альфу в их X
            // соберём массив узлов otf
            struct O { double x, y; };
            QVector<O> o; o.reserve(otf->GetSize());
            for (int i = 0, n = otf->GetSize(); i < n; ++i) {
                double node[4]; otf->GetNodeValue(i, node);
                o.push_back({ std::clamp((node[0] - a) * 255.0 / d, 0.0, 255.0),
                              std::clamp(node[1],0.0,1.0) });
            }
            auto eval = [&](double x)->double {
                if (x <= o.front().x) return o.front().y;
                if (x >= o.back().x)  return o.back().y;
                for (int i = 1; i < o.size(); ++i) {
                    if (x <= o[i].x) {
                        const double t = (x - o[i - 1].x) / (o[i].x - o[i - 1].x);
                        return o[i - 1].y * (1.0 - t) + o[i].y * t;
                    }
                }
                return o.back().y;
                };
            for (auto& p : mPts) p.a = eval(p.x);
        }
    }

    // Обновляем канвас и слайдеры
    mCanvas->setPoints(mPts, mPts.isEmpty() ? -1 : 0);
    emit mCanvas->selectionChanged(mPts.isEmpty() ? -1 : 0);

    mCanvas->setHistogram(mHist);

    rebuildPreview(true);
}

vtkColorTransferFunction* TransferFunctionEditor::makeCTF(const QVector<TfPoint>& pts)
{
    auto* c = vtkColorTransferFunction::New();
    c->RemoveAllPoints();
    if (pts.isEmpty()) return c;

    auto mapX = [&](double x)->double {
        return mMin + (x / 255.0) * (mMax - mMin);
        };

    // сторожевые
    c->AddRGBPoint(mapX(0.0) - 1.0,  // чуть левее диапазона
        pts.front().color.redF(),
        pts.front().color.greenF(),
        pts.front().color.blueF());

    for (const auto& p : pts)
        c->AddRGBPoint(mapX(p.x), p.color.redF(), p.color.greenF(), p.color.blueF());

    c->AddRGBPoint(mapX(255.0) + 1.0, // чуть правее диапазона
        pts.back().color.redF(),
        pts.back().color.greenF(),
        pts.back().color.blueF());
    return c;
}
vtkPiecewiseFunction* TransferFunctionEditor::makeOTF(const QVector<TfPoint>& pts)
{
    auto* o = vtkPiecewiseFunction::New();
    o->RemoveAllPoints();
    if (pts.isEmpty()) return o;

    auto mapX = [&](double x)->double {
        return mMin + (x / 255.0) * (mMax - mMin);
        };

    o->AddPoint(mapX(0.0) - 1.0, std::clamp(pts.front().a, 0.0, 1.0));
    for (const auto& p : pts)
        o->AddPoint(mapX(p.x), std::clamp(p.a, 0.0, 1.0));
    o->AddPoint(mapX(255.0) + 1.0, std::clamp(pts.back().a, 0.0, 1.0));
    return o;
}
