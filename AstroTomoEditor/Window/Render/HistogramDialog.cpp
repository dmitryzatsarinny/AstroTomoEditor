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

HistogramDialog::HistogramDialog(QWidget* parent, DicomInfo DI, vtkImageData* image)
    : QDialog(parent), mImage(image)
{
    setWindowTitle(tr("Histogram"));
    Dicom = DI;
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
    mBtnAuto = new QPushButton(tr("Auto Range"), this);
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
    const int w = std::max(360, mCanvas->width());
    const int h = std::max(220, mCanvas->height());
    mCache = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    mCache.fill(Qt::transparent);
    QPainter p(&mCache);
    p.setRenderHint(QPainter::Antialiasing, true);

    // --- Calm dark palette ---
    const QColor bg(62, 66, 73);          // фон
    const QColor grid(102, 108, 116);     // сетка
    const QColor axisCol(232, 235, 238);  // оси/подписи
    const QColor lineCol(215, 60, 60);    // линия
    const QColor fillAll(210, 215, 222, 60);  // серая заливка вне окна
    const QColor fillSel(235, 120, 120, 70);  // розовая заливка в окне
    const QColor limitCol(90, 200, 90);   // зелёные ограничители

    p.fillRect(0, 0, w, h, bg);

    // Поле графика
    const QRectF r(78, 32, w - 120, h - 96);

    // --- нормализация (как было) ---
    QVector<quint64> hForNorm = mH;
    if (mIgnoreZeros && !hForNorm.isEmpty()) hForNorm[0] = 0;
    const double cap = percentileCap(hForNorm, 0.995);
    const double denom = (cap > 0.0) ? cap : 1.0;
    auto val = [&](int i)->double {
        const double v = (i >= 0 && i < mSmooth.size()) ? mSmooth[i] : 0.0;
        return std::clamp(v / denom, 0.0, 1.0);
        };

    // --- сетка Y ---
    p.setPen(QPen(grid, 1));
    QFontMetrics fm(p.font());
    const double stepY = niceStep(cap / 5.0);
    for (double v = 0; v <= cap + 1e-9; v += stepY) {
        const double y = r.bottom() - (v / cap) * r.height();
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        QString lbl;
        if (v >= 1e6) lbl = QString::number(v / 1e6, 'f', 1) + "M";
        else if (v >= 1e3) lbl = QString::number(v / 1e3, 'f', 0) + "k";
        else               lbl = QString::number(int(std::round(v)));
        p.setPen(axisCol);
        p.drawText(QPointF(r.left() - fm.horizontalAdvance(lbl) - 10, y + fm.ascent() / 3), lbl);
        p.setPen(QPen(grid, 1));
    }

    // --- сетка X + аккуратная центровка подписей ---
    const double a = mAxisMin, b = mAxisMax;
    const double full = b - a;
    const double stepX = niceStep(full / 8.0);
    QFontMetrics fmX(p.font());
    auto drawXLabel = [&](double x, const QString& s) {
        const int tw = fmX.horizontalAdvance(s);
        // если подпись у левого/правого края — прижимаем, иначе центрируем
        double left = x - tw / 2.0;
        if (left < r.left() + 1) left = r.left() + 1;
        if (left + tw > r.right() - 1) left = r.right() - 1 - tw;
        p.drawText(QPointF(left, r.bottom() + 22), s);
        };

    for (double v = std::ceil(a / stepX) * stepX; v <= b + 1e-9; v += stepX) {
        const int d = dataFromAxis(v);
        const double x = r.left() + dataToX(d) * r.width();
        p.setPen(QPen(grid, 1));
        p.drawLine(QPointF(x, r.bottom()), QPointF(x, r.top()));
        p.setPen(axisCol);
        drawXLabel(x, QString::number(int(std::lround(v))));
    }

    // --- готовим пути: вся гистограмма и окно выбора ---
    QPainterPath pathAll, pathSel, pathLine;
    auto addPoint = [&](int i, QPainterPath& ph) {
        const double x = r.left() + dataToX(i) * r.width();
        const double y = r.bottom() - r.height() * val(i);
        ph.lineTo(x, y);
        };

    // полная область
    pathAll.moveTo(r.left(), r.bottom());
    for (int i = 0; i < 256; ++i) addPoint(i, pathAll);
    pathAll.lineTo(r.right(), r.bottom()); pathAll.closeSubpath();

    // только между mLo..mHi
    pathSel.moveTo(r.left() + dataToX(mLo) * r.width(), r.bottom());
    for (int i = mLo; i <= mHi; ++i) addPoint(i, pathSel);
    pathSel.lineTo(r.left() + dataToX(mHi) * r.width(), r.bottom());
    pathSel.closeSubpath();

    // линия контура (по всем точкам)
    pathLine.moveTo(r.left(), r.bottom() - r.height() * val(0));
    for (int i = 1; i < 256; ++i) {
        const double x = r.left() + dataToX(i) * r.width();
        const double y = r.bottom() - r.height() * val(i);
        pathLine.lineTo(x, y);
    }

    // заливки
    p.fillPath(pathAll, fillAll);  // серая везде
    p.fillPath(pathSel, fillSel);  // розовая только внутри окна

    // контур
    p.setPen(QPen(lineCol, 2.0));
    p.drawPath(pathLine);

    // --- зелёные ограничители ---
    auto drawLimit = [&](int d) {
        const double x = r.left() + dataToX(d) * r.width();
        const double vAxis = axisFromData(d);
        p.setPen(QPen(limitCol, 2.0));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(limitCol);
        QFont f = p.font(); f.setBold(true); p.setFont(f);
        p.drawText(QPointF(x - 16, r.top() - 6), QString::number(std::lround(vAxis)));
        p.setFont(QFont());
        };
    drawLimit(mLo);
    drawLimit(mHi);

    // --- оси со стрелками ---
    auto arrow = [&](QPointF a0, QPointF a1) {
        p.setPen(QPen(axisCol, 2));
        p.drawLine(a0, a1);
        const double L = 8.0;
        const double ang = std::atan2(a1.y() - a0.y(), a1.x() - a0.x());
        QPointF u(a1.x() - L * std::cos(ang - 0.35), a1.y() - L * std::sin(ang - 0.35));
        QPointF v2(a1.x() - L * std::cos(ang + 0.35), a1.y() - L * std::sin(ang + 0.35));
        p.drawLine(a1, u); p.drawLine(a1, v2);
        };
    arrow(QPointF(r.left(), r.bottom()), QPointF(r.right() + 12, r.bottom())); // X →
    arrow(QPointF(r.left(), r.bottom()), QPointF(r.left(), r.top() - 12));     // Y ↑

    // --- подписи осей (EN) + маленькие "N" и "HU" у стрелок ---
    p.setPen(axisCol);
    QFont f = p.font(); f.setBold(true); p.setFont(f);
    p.drawText(QRectF(r.left(), h - 38, r.width(), 30),
        Qt::AlignHCenter | Qt::AlignVCenter, Dicom.XTitle);
    p.save();
    p.translate(28, r.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-r.height() / 2, -18, r.height(), 30),
        Qt::AlignHCenter | Qt::AlignVCenter, Dicom.YTitle);
    p.restore();
    p.setFont(QFont());
    // маленькие подписи у стрелок
    p.drawText(QPointF(r.right() + 12, r.bottom() + 16), Dicom.XLable);
    p.drawText(QPointF(r.left() - 14, r.top() - 12), Dicom.YLable);

    // вывод
    QPainter q(mCanvas);
    q.drawImage(0, 0, mCache);
}