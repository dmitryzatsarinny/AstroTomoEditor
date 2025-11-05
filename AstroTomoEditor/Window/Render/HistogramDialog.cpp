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

// === производные и кривизна ==================================================
static void firstSecondDeriv(const QVector<double>& s, QVector<double>& d1, QVector<double>& d2)
{
    const int N = s.size();
    d1.resize(N); d2.resize(N);
    if (N < 5) { std::fill(d1.begin(), d1.end(), 0.0); std::fill(d2.begin(), d2.end(), 0.0); return; }
    d1[0] = d1[N - 1] = 0.0; d2[0] = d2[1] = d2[N - 2] = d2[N - 1] = 0.0;
    for (int i = 1; i < N - 1; ++i) d1[i] = 0.5 * (s[i + 1] - s[i - 1]);
    for (int i = 2; i < N - 2; ++i) d2[i] = 0.5 * (d1[i + 1] - d1[i - 1]);
}

// перегиб как максимум кривизны на восходящем склоне в [winL..winR]
static int leftInflectionByCurvature(const QVector<double>& s,
    const QVector<double>& d1,
    const QVector<double>& d2,
    int winL, int winR, int peakIdx)
{
    winL = std::max(winL, 2);
    winR = std::max(winR, winL + 2);

    const double peakH = s[peakIdx];
    const double minH = 0.15 * peakH;   // фильтр по уровню (15%..85% высоты пика)
    const double maxH = 0.85 * peakH;

    int best = -1;
    double bestKappa = -1.0;

    for (int i = winL + 5; i <= winR - 5; ++i) {      // не трогаем края окна
        if (d1[i] <= 0.0) continue;               // нужен восходящий склон
        if (s[i] < minH || s[i] > maxH) continue; // избегаем дна и самой вершины

        const double denom = std::pow(1.0 + d1[i] * d1[i], 1.5);
        const double kappa = std::abs(d2[i]) / (denom > 0.0 ? denom : 1.0);
        if (kappa > bestKappa) { bestKappa = kappa; best = i; }
    }
    // фолбэк: если не нашли — берем середину окна
    if (best < 0) best = (winL + winR) / 2;
    return best;
}

// правая граница: μ,σ по правому хвосту [lo..255] в HU
static void meanSigmaHU_rightTail_full(const QVector<quint64>& h, int lo,
    double axisMin, double axisMax,
    double& mu, double& sigma)
{
    lo = std::clamp(lo, 0, 255);
    const int hi = 255;
    const double K = (axisMax - axisMin) / 255.0;

    long double W = 0, S1 = 0, S2 = 0;
    for (int i = lo; i <= hi; ++i) {
        const long double w = (long double)h[i];
        const long double x = (long double)(axisMin + K * i);
        W += w; S1 += w * x; S2 += w * x * x;
    }
    if (W <= 0) { mu = axisMin + K * ((lo + hi) * 0.5); sigma = 0.0; return; }
    mu = (double)(S1 / W);
    const double var = (double)(S2 / W - (S1 / W) * (S1 / W));
    sigma = var > 0 ? std::sqrt(var) : 0.0;
}

// === robust peak utils ======================================================
struct PeakInfo {
    int idx = -1;
    int leftValley = -1;
    int rightValley = -1;
    double prominence = 0.0;
    int fwhmBins = 0;
};

static int findNextValley(const QVector<double>& s, int from) {
    for (int i = std::clamp(from + 1, 1, (int)(s.size() - 2)); i < s.size() - 1; ++i)
        if (s[i - 1] > s[i] && s[i] < s[i + 1]) return i;
    return s.size() - 1;
}
static int findPrevValley(const QVector<double>& s, int from) {
    for (int i = std::clamp(from - 1, 1, (int)(s.size() - 2)); i >= 1; --i)
        if (s[i - 1] < s[i] && s[i] < s[i + 1]) return i;
    return 0;
}

// локальная «вершина» допускает плато (>= соседей)
static bool isLocalPeak(const QVector<double>& s, int i) {
    return (i > 0 && i + 1 < s.size() && s[i] >= s[i - 1] && s[i] >= s[i + 1] &&
        (s[i] > s[i - 1] || s[i] > s[i + 1]));
}

static PeakInfo analyzePeak(const QVector<double>& s, int p) {
    PeakInfo R; R.idx = p;
    R.leftValley = findPrevValley(s, p);
    R.rightValley = findNextValley(s, p);
    const double base = std::max(s[R.leftValley], s[R.rightValley]);
    R.prominence = std::max(0.0, s[p] - base);

    // FWHM (по уровню base + prominence/2)
    const double half = base + R.prominence * 0.5;
    int L = p, Rr = p;
    while (L > R.leftValley && s[L] > half) --L;
    while (Rr<R.rightValley && s[Rr] > half) ++Rr;
    R.fwhmBins = std::max(1, Rr - L);
    return R;
}

static int findContrastPeakAfter(const QVector<double>& s, int startBin,
    int minCenterBin, double minPromFrac, int minWidthBins) // CHANGED
{
    if (s.size() < 5) return -1;
    const double gmax = *std::max_element(s.begin(), s.end());
    const double promLevels[2] = { minPromFrac, std::max(0.5 * minPromFrac, 0.01) };

    for (double promFrac : promLevels) {
        for (int i = std::max(1, startBin); i < s.size() - 1; ++i) {
            if (i < minCenterBin) continue;                           // CHANGED: апекс пика должен быть правее 260 HU
            if (!isLocalPeak(s, i)) continue;
            const PeakInfo P = analyzePeak(s, i);
            if (P.prominence >= gmax * promFrac && P.fwhmBins >= std::max(1, minWidthBins))
                return i;
        }
    }
    // fallback: максимум в правой части, но тоже ≥ minCenterBin
    int argmax = -1; double vmax = -1;
    for (int i = std::max(minCenterBin, startBin); i < s.size(); ++i)
        if (s[i] > vmax) { vmax = s[i]; argmax = i; }
    return argmax;
}

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
    QVector<double> s = mSmooth;
    if (s.isEmpty()) { setRange(0, 255, true); return; }
    if (mIgnoreZeros && !s.isEmpty()) s[0] = 0.0;

    const int bin200 = dataFromAxis(200.0);
    const int minCenterBin = dataFromAxis(260.0);

    // 1) находим контрастный пик правее 260 HU (твоя робастная функция)
    const int peak = findContrastPeakAfter(s, bin200, minCenterBin, 0.035, 4);
    if (peak < 0) { setRange(bin200, 255, true); return; }

    // 2) перегиб = максимум кривизны в окне [200HU .. peak]
    QVector<double> d1, d2; firstSecondDeriv(s, d1, d2);
    const int leftInflect = std::clamp(
        leftInflectionByCurvature(s, d1, d2, bin200, peak - 1, peak), 0, 255);

    // 3) μ,σ по правому хвосту от этого перегиба до конца гистограммы
    double muHU = 0, sigmaHU = 0;
    meanSigmaHU_rightTail_full(mH, leftInflect, mAxisMin, mAxisMax, muHU, sigmaHU);

    // 4) финальные границы: [перегиб ; μ + 2σ] (климпим по оси HU)
    const int loBin = leftInflect;
    int hiBin = dataFromAxis(std::min(muHU + 2.0 * sigmaHU, mAxisMax));
    hiBin = std::clamp(hiBin, 0, 255);

    if (hiBin <= loBin) hiBin = std::min(loBin + 12, 255);

    setRange(loBin, hiBin, true);
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