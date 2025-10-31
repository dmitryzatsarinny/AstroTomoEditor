#include "HistogramDialog.h"
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <numeric>
#include <cmath>

// простое «боксовое» сглаживание
static QVector<double> smoothBox(const QVector<quint64>& h, int win = 9)
{
    if (h.isEmpty()) return {};
    win = std::max(1, win | 1);
    const int R = win / 2, N = h.size();
    auto at = [&](int i) {
        if (i < 0) i = -i;
        if (i >= N) i = 2 * (N - 1) - i;
        if (N == 1) i = 0;
        return double(h[i]);
        };
    QVector<double> out(N); double sum = 0;
    for (int k = -R; k <= R; ++k) sum += at(k);
    out[0] = sum / double(win);
    for (int i = 1; i < N; ++i) {
        sum += at(i + R) - at(i - 1 - R);
        out[i] = sum / double(win);
    }
    return out;
}

HistogramDialog::HistogramDialog(QWidget* parent, vtkImageData* image)
    : QDialog(parent), mImage(image)
{
    setWindowTitle(tr("Histogram"));
    setModal(false);
    resize(860, 460);
    buildUi();
    // ВАЖНО: не считаем гистограмму здесь — первый пересчёт через refreshFromImage()
}

void HistogramDialog::setFixedAxis(bool enabled, double a, double b)
{
    mAxisFixed = enabled;
    if (enabled) {
        mAxisMin = a; mAxisMax = b;
    }
}

void HistogramDialog::buildUi()
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);

    mCanvas = new QWidget(this);
    mCanvas->setMinimumHeight(320);
    mCanvas->installEventFilter(this);
    v->addWidget(mCanvas, 1);

    auto* row = new QHBoxLayout();
    row->addStretch();
    mBtnAuto = new QPushButton(tr("Автодиапазон"), this);
    row->addWidget(mBtnAuto);
    connect(mBtnAuto, &QPushButton::clicked, this, &HistogramDialog::autoRange);
    v->addLayout(row);
}

void HistogramDialog::refreshFromImage(vtkImageData* image)
{
    mImage = image;
    buildHistogram();

    // первый запуск: весь диапазон данных
    if (mLo == 0 && mHi == 255) {
        emit rangeChanged(mLo, mHi);
    }
    else {
        setRange(mLo, mHi, /*emit*/true);
    }
    if (mCanvas) mCanvas->update();
}



void HistogramDialog::buildHistogram()
{
    // гарантируем 256 бинов и обнуляем
    mH.assign(256, 0);

    if (!mImage) { mSmooth = smoothBox(mH, 9); return; }

    // проверим тип
    if (mImage->GetScalarType() != VTK_UNSIGNED_CHAR ||
        mImage->GetNumberOfScalarComponents() != 1)
    {
        mSmooth = smoothBox(mH, 9);
        return;
    }

    int ext[6];
    mImage->GetExtent(ext);

    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    vtkIdType incX, incY, incZ;  // ВНИМАНИЕ: ИНКРЕМЕНТЫ В БАЙТАХ
    mImage->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    auto* p0 = static_cast<const uint8_t*>(
        mImage->GetScalarPointer(ext[0], ext[2], ext[4]));


    const int step = 1; // без субсэмплинга

    for (int i = 0; i < nx * ny * nz; i++)
        mH[(int)*(p0 + i)]++;

    //// тройной цикл с byte-increments
    //const uint8_t* pz = p0;
    //for (int k = 0; k < nz; k += step) {
    //    const uint8_t* py = pz;
    //    for (int j = 0; j < ny; j += step) {
    //        const uint8_t* px = py;
    //        for (int i = 0; i < nx; i += step) {
    //            uint8_t v = *px;
    //            //if (mIgnoreZeros && v == 0) continue;
    //            int bin = std::clamp(int(v), 0, 255);
    //            mH[bin]++;
    //            px += incX * step;   // смещение по X — в байтах
    //        }
    //        py += incY * step;       // смещение по Y — в байтах
    //    }
    //    pz += incZ * step;           // смещение по Z — в байтах
    //}
    mH[0] = 0;
    mSmooth = smoothBox(mH, 9);
}

bool HistogramDialog::eventFilter(QObject* o, QEvent* e)
{
    if (o != mCanvas) return QDialog::eventFilter(o, e);

    const QRectF r = mCanvas->rect().adjusted(60, 18, -24, -48);

    if (e->type() == QEvent::Paint) {
        paintCanvas(); return true;
    }

    if (e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        const double t = std::clamp((me->position().x() - r.left()) / std::max(1.0, r.width()), 0.0, 1.0);
        const int d = xToData(t);

        if (me->button() == Qt::LeftButton) { mDrag = Drag::Left;  setRange(d, mHi, true); }
        if (me->button() == Qt::RightButton) { mDrag = Drag::Right; setRange(mLo, d, true); }
        if (me->button() == Qt::MiddleButton) { mDrag = Drag::Pan;   mPanStartD = d; }
        mCanvas->update();
        return true;
    }

    if (e->type() == QEvent::MouseMove) 
    {
        if (mDrag == Drag::None) 
            return false;

        auto* me = static_cast<QMouseEvent*>(e);
        const double t = std::clamp((me->position().x() - r.left()) / std::max(1.0, r.width()), 0.0, 1.0);
        const int d = xToData(t);

        if (mDrag == Drag::Left)  
            setRange(d, mHi, true);
        if (mDrag == Drag::Right) 
            setRange(mLo, d, true);
        if (mDrag == Drag::Pan) 
        {
            const int dv = d - mPanStartD;
            const int w = mHi - mLo;
            int lo = std::clamp(mLo + dv, 0, 255 - w);
            int hi = lo + w;
            setRange(lo, hi, true);
            mPanStartD = d;
        }
        mCanvas->update();
        return true;
    }

    if (e->type() == QEvent::MouseButtonRelease) {
        mDrag = Drag::None;
        return true;
    }

    return QDialog::eventFilter(o, e);
}

void HistogramDialog::setRangeAxis(double loAxis, double hiAxis, bool emitSig)
{
    if (loAxis > hiAxis) std::swap(loAxis, hiAxis);
    // переводим физику оси (например, HU -1000..1000) в бины 0..255
    const int loBin = dataFromAxis(loAxis);
    const int hiBin = dataFromAxis(hiAxis);
    setRange(loBin, hiBin, emitSig); // дальше обычный путь
}

void HistogramDialog::setRange(int loBin, int hiBin, bool emitSig)
{
    if (loBin > hiBin) std::swap(loBin, hiBin);
    mLo = std::clamp(loBin, 0, 255);
    mHi = std::clamp(hiBin, 0, 255);
    if (emitSig) 
        emit rangeChanged(mLo, mHi);
    if (mCanvas) 
        mCanvas->update();
}

void HistogramDialog::autoRange()
{
    QVector<quint64> h = mH;
    if (mIgnoreZeros && !h.isEmpty()) 
        h[0] = 0;

    const quint64 total = std::accumulate(h.begin(), h.end(), quint64(0));
    if (total == 0) { setRange(0, 255, true); return; }

    const quint64 lCut = total / 200; // 0.5%
    const quint64 rCut = total / 200;

    quint64 acc = 0; int l = 0;
    for (; l < 256; ++l) { acc += mH[l]; if (acc >= lCut) break; }

    acc = 0; int r = 255;
    for (; r >= 0; --r) { acc += mH[r]; if (acc >= rCut) break; }

    setRange(l, r, true);
}

static double percentileCap(const QVector<quint64>& h, double q /*0..1*/)
{
    QVector<quint64> tmp = h;
    std::sort(tmp.begin(), tmp.end());
    if (tmp.isEmpty()) return 1.0;
    const int idx = std::clamp(int(std::floor(q * (tmp.size() - 1))), 0, (int)(tmp.size() - 1));
    return double(std::max<quint64>(1, tmp[idx]));
}

void HistogramDialog::paintCanvas()
{
    const int w = std::max(300, mCanvas->width());
    const int h = std::max(160, mCanvas->height());
    mCache = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    mCache.fill(Qt::transparent);
    QPainter p(&mCache);
    p.fillRect(0, 0, w, h, QColor(22, 23, 26));

    const QRectF r = QRectF(60, 18, w - 60 - 24, h - 48 - 12);
    p.setPen(QColor(255, 255, 255, 30)); p.drawRect(r);

    // --- робастная нормализация ---
    // база для масштаба: либо максимум по всем, либо по 99.5 перцентилю
    QVector<quint64> hForNorm = mH;
    if (mIgnoreZeros && !hForNorm.isEmpty()) hForNorm[0] = 0; // не даём нулевому бину доминировать
    const double cap = percentileCap(hForNorm, 0.995);        // мягкий клип
    const double denom = (cap > 0.0) ? cap : 1.0;

    auto val = [&](int i)->double {
        const double v = (i >= 0 && i < mSmooth.size()) ? mSmooth[i] : 0.0;
        return std::clamp(v / (denom * 0.95), 0.0, 1.0);
        };

    // кривая
    QPainterPath pathAll, pathSel;
    pathAll.moveTo(r.left(), r.bottom());
    pathSel.moveTo(r.left(), r.bottom());
    for (int i = 0; i < 256; ++i) {
        const double x = r.left() + dataToX(i) * r.width();
        const double y = r.bottom() - r.height() * std::clamp(val(i), 0.0, 1.0);
        pathAll.lineTo(x, y);
        if (i >= mLo && i <= mHi) pathSel.lineTo(x, y);
        else                      pathSel.lineTo(x, r.bottom());
    }
    pathAll.lineTo(r.right(), r.bottom()); pathAll.closeSubpath();
    p.fillPath(pathAll, QColor(200, 220, 220, 60));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(220, 60, 60, 220), 2.0));
    p.drawPath(pathSel);

    // зелёные ограничители + подписи (ОСЬ a..b)
    auto drawLimit = [&](int d) {
        const double x = r.left() + dataToX(d) * r.width();
        const double vAxis = axisFromData(d);
        p.setPen(QPen(QColor(20, 120, 20), 2.0));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QColor(20, 120, 20));
        QFont f = p.font(); f.setBold(true); p.setFont(f);
        p.drawText(QPointF(x - 16, r.top() - 6), QString::number(std::lround(vAxis)));
        };
    drawLimit(mLo);
    drawLimit(mHi);

    // ось X: подписи по a..b
    p.setPen(QColor(255, 255, 255, 160));
    p.drawText(QPointF(8, 14), tr("N"));
    p.drawText(QPointF(w - 40, h - 6), tr("HU")); // подпись оси X (для КТ)

    const double a = mAxisMin, b = mAxisMax;
    const double full = b - a;
    const double step = niceStep(full / 6.0);
    for (double v = std::ceil(a / step) * step; v <= b + 1e-9; v += step) {
        const int d = dataFromAxis(v);
        const double x = r.left() + dataToX(d) * r.width();
        p.setPen(QColor(255, 255, 255, 50));
        p.drawLine(QPointF(x, r.bottom()), QPointF(x, r.bottom() + 6));
        p.setPen(QColor(255, 255, 255, 130));
        p.drawText(QPointF(x - 18, r.bottom() + 20), QString::number(int(std::lround(v))));
    }

    // вывести
    QPainter q(mCanvas);
    q.drawImage(0, 0, mCache);
}
