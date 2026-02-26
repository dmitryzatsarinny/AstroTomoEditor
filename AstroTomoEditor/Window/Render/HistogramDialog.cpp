#include "HistogramDialog.h"
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>

#include <QStackedLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <numeric>
#include <cmath>
#include "RenderView.h"

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

/// простое «боксовое» сглаживание
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
    : DialogShell(parent, tr("Histogram"), WindowType::Histogram), mImage(image)
{
    autoleft = -1;
    autoright = -1;
    Dicom = DI;
    resize(860, 460);
    buildUi();
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
    QWidget* content = contentWidget();
    content->setObjectName("HistogramContent");

    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(8);

    // === холст гистограммы ===
    mCanvas = new QWidget(content);
    mCanvas->setMinimumHeight(320);
    mCanvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mCanvas->installEventFilter(this);
    v->addWidget(mCanvas, 1);

    // === кнопка прямо на canvas ===
    mBtnAuto = new QPushButton(tr("Auto range"), mCanvas);
    mBtnAuto->setFixedSize(90, 28);   // аккуратный, стабильный размер
    mBtnAuto->raise();

    connect(mBtnAuto, &QPushButton::clicked,
        this, &HistogramDialog::autoRange);

    content->setStyleSheet(
        "#HistogramContent { background: transparent; }"
        "QPushButton {"
        "  min-width:80px;"
        "  padding:4px 14px;"
        "  background:rgba(42,44,48,220);"
        "  color:#f0f0f0;"
        "  border-radius:6px;"
        "  border:1px solid rgba(255,255,255,0.22);"
        "}"
        "QPushButton:hover {"
        "  background:#32353a;"
        "}"
        "QPushButton:pressed {"
        "  background:#3a3d44;"
        "}"
        "QPushButton:disabled {"
        "  background:#25272b;"
        "  color:#777777;"
        "  border-color:rgba(255,255,255,0.10);"
        "}"
    );
}

void HistogramDialog::HideAutoRange(vtkImageData* image)
{
    refreshFromImage(image);
    autoRange(false);
}

void HistogramDialog::HideAllShow(vtkImageData* image)
{
    refreshFromImage(image);
    setRange(HistMin, HistMax, true);
}

void HistogramDialog::HideRangeIfCT(vtkImageData* image, int HLeft, int HRight)
{
    if (Dicom.TypeOfRecord == CT || Dicom.TypeOfRecord == CT3DR) 
        setRange(HLeft, HRight, true);
}

void HistogramDialog::refreshFromImage(vtkImageData* image)
{
    mImage = image;
    buildHistogram();
    autoRange(true);

    if (mLo == (int)HistMin && mHi == (int)HistMax)
        emit rangeChanged(mLo, mHi);
    else
        setRange(mLo, mHi, /*emit*/true);

    if (mCanvas) mCanvas->update();
}

void HistogramDialog::buildHistogram()
{
    // гарантируем HistScale бинов и обнуляем
    mH.assign(HistScale, 0);

    if (!mImage)
    {
        mSmooth = smoothBox(mH, 9);
        return;
    }

    if (!mAxisFixed) {
        double range[2]{};
        mImage->GetScalarRange(range);
        mAxisMin = range[0];
        mAxisMax = range[1];
        if (mAxisMin == mAxisMax) {
            mAxisMin -= 1.0;
            mAxisMax += 1.0;
        }
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


    for (int z = 0; z < nz; z += SubStep) {
        const uint8_t* pZ = p0 + z * incZ;

        for (int y = 0; y < ny; y += SubStep) {
            const uint8_t* pY = pZ + y * incY;

            for (int x = 0; x < nx; x += SubStep) {
                const uint8_t v = pY[x * incX];
                if (v >= HistMin && v <= HistMax)
                    mH[v]++;
            }
        }
    }

    //for (int i = 0; i <= 255; i++)
    //    qDebug() << i << "  " << mH[i];

    if (!mH.isEmpty())
        mH[(int)HistMin] = 0;   // глушим "0", если нужно

    mSmooth = smoothBox(mH, 9);
}

bool HistogramDialog::eventFilter(QObject* o, QEvent* e)
{
    if (o != mCanvas)
        return DialogShell::eventFilter(o, e);   // или QDialog::eventFilter

    if (e->type() == QEvent::Resize || e->type() == QEvent::Show) {

        if (mBtnAuto) {
            const int margin = 6;   // минимальный, красиво близко к углу
            const QSize btnSz = mBtnAuto->size();
            const QSize canvasSz = mCanvas->size();

            int x = canvasSz.width() - btnSz.width() - margin;
            int y = canvasSz.height() - btnSz.height() - margin;

            mBtnAuto->move(x, y);
            mBtnAuto->raise();
        }
    }

    const QRectF r = mCanvas->rect().adjusted(60, 18, -24, -48);

    if (e->type() == QEvent::Paint) {
        paintCanvas();
        return true;
    }

    if (e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        const double t = std::clamp(
            (me->position().x() - r.left()) / std::max(1.0, r.width()),
            0.0, 1.0);
        const int d = xToData(t);

        if (me->button() == Qt::LeftButton) {
            mDrag = Drag::Left;
            setRange(d, mHi, true);
        }
        if (me->button() == Qt::RightButton) {
            mDrag = Drag::Right;
            setRange(mLo, d, true);
        }
        if (me->button() == Qt::MiddleButton) {
            mDrag = Drag::Pan;
            mPanStartD = d;
        }
        mCanvas->update();
        return true;
    }

    if (e->type() == QEvent::MouseMove) {
        if (mDrag == Drag::None)
            return false;

        auto* me = static_cast<QMouseEvent*>(e);
        const double t = std::clamp(
            (me->position().x() - r.left()) / std::max(1.0, r.width()),
            0.0, 1.0);
        const int d = xToData(t);

        if (mDrag == Drag::Left)
            setRange(d, mHi, true);
        if (mDrag == Drag::Right)
            setRange(mLo, d, true);
        if (mDrag == Drag::Pan) {
            const int dv = d - mPanStartD;
            const int w = mHi - mLo;
            const int loMin = (int)HistMin;
            const int loMax = (int)HistMax - w;
            int lo = std::clamp(mLo + dv, loMin, loMax);
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

    return DialogShell::eventFilter(o, e); // или QDialog::eventFilter(o, e);
}

void HistogramDialog::done(int r)
{
    if (m_onFinished) m_onFinished();
    QDialog::done(r);
}

void HistogramDialog::changeEvent(QEvent* e)
{
    DialogShell::changeEvent(e);

    if (e->type() == QEvent::LanguageChange)
    {
        retranslateUi();
        mCache = QImage();          // сбросить кэш, иначе останется старый текст
        if (mCanvas) mCanvas->update();
        update();
    }
}

static QString trPV(const char* s)
{
    return QCoreApplication::translate("PlanarView", s);
}

void HistogramDialog::retranslateUi()
{
    const QString title = tr("Histogram");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);

    if (mBtnAuto)
        mBtnAuto->setText(tr("Auto range"));

    // подписи осей зависят от типа исследования
    if (Dicom.TypeOfRecord == CT || Dicom.TypeOfRecord == CT3DR)
    {
        Dicom.XTitle = trPV("Hounsfield Units");
        Dicom.YTitle = trPV("Voxel count");
        Dicom.XLable = trPV("HU");
        Dicom.YLable = trPV("N");
    }
    else if (Dicom.TypeOfRecord == MRI || Dicom.TypeOfRecord == MRI3DR)
    {
        Dicom.XTitle = trPV("MRI intensity");
        Dicom.YTitle = trPV("Voxel count");
        Dicom.XLable = trPV("AU");
        Dicom.YLable = trPV("N");
    }
}

void HistogramDialog::setRangeAxis(double loAxis, double hiAxis, bool emitSig)
{
    if (loAxis > hiAxis) std::swap(loAxis, hiAxis);
    // переводим физику оси (например, HU -1000..1000) в бины HistMin..HistMax
    const int loBin = dataFromAxis(loAxis);
    const int hiBin = dataFromAxis(hiAxis);
    setRange(loBin, hiBin, emitSig); // дальше обычный путь
}

void HistogramDialog::setRange(int loBin, int hiBin, bool emitSig)
{
    if (loBin > hiBin) std::swap(loBin, hiBin);
    mLo = std::clamp(loBin, (int)HistMin, (int)HistMax);
    mHi = std::clamp(hiBin, (int)HistMin, (int)HistMax);
    if (emitSig)
        emit rangeChanged(mLo, mHi);
    if (mCanvas)
        mCanvas->update();
}


// самая глубокая "впадина" в диапазоне [L..R] как просто минимум s[i]
static int deepestValleyInRange(const QVector<double>& s, int L, int R)
{
    L = std::max(L, 1);
    R = std::min(R, (int)s.size() - 2);
    if (R <= L) return L;

    int best = L;
    double bestVal = s[L];
    for (int i = L + 1; i <= R; ++i) {
        if (s[i] < bestVal) {
            bestVal = s[i];
            best = i;
        }
    }
    return best;
}



// оценка "гауссоподобия" пика в окне [L..R]
static double gaussianFitErrorBins(const QVector<double>& s,
    int L, int R,
    double& muBin, double& sigmaBin)
{
    const int N = s.size();
    if (N <= 0) {
        muBin = 0.0;
        sigmaBin = 0.0;
        return std::numeric_limits<double>::infinity();
    }

    L = std::clamp(L, 0, N - 1);
    R = std::clamp(R, 0, N - 1);
    if (R <= L + 1) {
        muBin = 0.0;
        sigmaBin = 0.0;
        return std::numeric_limits<double>::infinity();
    }

    long double W = 0.0L;
    long double S1 = 0.0L;
    long double S2 = 0.0L;

    // считаем моменты по x = i (индекс бина)
    for (int i = L; i <= R; ++i) {
        const long double y = (long double)std::max(0.0, s[i]);
        const long double x = (long double)i;
        W += y;
        S1 += y * x;
        S2 += y * x * x;
    }

    if (W <= 0.0L) {
        muBin = 0.0;
        sigmaBin = 0.0;
        return std::numeric_limits<double>::infinity();
    }

    const long double m = S1 / W;
    const long double m2 = S2 / W;
    const long double var = m2 - m * m;
    if (var <= 0.0L) {
        muBin = 0.0;
        sigmaBin = 0.0;
        return std::numeric_limits<double>::infinity();
    }

    muBin = (double)m;
    sigmaBin = std::sqrt((double)var);

    // подгоняем A по площади: W ≈ A * σ * sqrt(2π)
    const double sigma = sigmaBin;
    if (sigma <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double A = (double)W / (sigma * std::sqrt(2.0 * M_PI));

    // ошибка аппроксимации в нормированных единицах
    long double num = 0.0L;
    long double den = 0.0L;

    for (int i = L; i <= R; ++i) {
        const double x = (double)i;
        const double y = std::max(0.0, s[i]);
        const double g = A * std::exp(-(x - muBin) * (x - muBin) / (2.0 * sigma * sigma));
        const double d = y - g;
        num += d * d;
        den += y * y;
    }

    if (den <= 0.0L)
        return std::numeric_limits<double>::infinity();

    return (double)(num / den); // чем меньше, тем гауссоподобнее
}

static GaussianPeak findMostGaussianPeakBins(const QVector<double>& s,
    int binLo, int binHi)
{
    GaussianPeak best;
    const int N = s.size();
    if (N < 5) return best;

    binLo = std::clamp(binLo, 1, N - 2);
    binHi = std::clamp(binHi, 1, N - 2);
    if (binLo >= binHi) return best;

    auto itLo = s.begin() + binLo;
    auto itHi = s.begin() + binHi + 1;
    const double gmax = *std::max_element(itLo, itHi);
    if (gmax <= 0.0) return best;

    // три набора порогов от "строгих" к "мягким"
    const double promFracs[3] = { 0.002, 0.001, 0.0 };
    const int    widthMins[3] = { 2,     1,     1 };

    for (int pass = 0; pass < 3 && best.peakBin < 0; ++pass) {
        const double promThr = gmax * promFracs[pass];
        const int    minWidth = widthMins[pass];

        for (int i = binLo + 1; i < binHi; ++i) {
            if (!isLocalPeak(s, i))
                continue;

            PeakInfo P = analyzePeak(s, i);
            if (P.prominence < promThr)
                continue;
            if (P.fwhmBins < minWidth)
                continue;

            int L = P.leftValley;
            int R = P.rightValley;
            if (L < binLo) L = binLo;
            if (R > binHi) R = binHi;

            double muBin = 0.0, sigmaBin = 0.0;
            double err = gaussianFitErrorBins(s, L, R, muBin, sigmaBin);
            if (!std::isfinite(err))
                continue;

            if (best.peakBin < 0 || err < best.error) {
                best.error = err;
                best.peakBin = P.idx;
                best.muBin = muBin;
                best.sigmaBin = sigmaBin;
            }
        }
    }

    // крайний фолбэк: просто максимум в окне
    if (best.peakBin < 0) {
        int argmax = binLo;
        double vmax = s[binLo];
        for (int i = binLo + 1; i <= binHi; ++i) {
            if (s[i] > vmax) { vmax = s[i]; argmax = i; }
        }
        if (vmax > 0.0) {
            best.peakBin = argmax;
            best.muBin = argmax;     // центр по индексу
            best.sigmaBin = 3.0;     // какая-то разумная ширина по умолчанию
            best.error = 1.0;
        }
    }

    return best;
}

int findLeftValleySimple(const QVector<double>& s, int peakBin, int limit)
{
    int best = peakBin;
    double prev = s[peakBin];

    for (int i = peakBin - 1; i >= limit; --i)
    {
        if (s[i] < prev) {
            best = i;
            prev = s[i];
        }
        else {
            // как только пошёл подъём — это и есть впадина
            break;
        }
    }
    return best;
}

GaussianPeak HistogramDialog::FindSecondPeak(const QVector<double>& s)
{
    GaussianPeak mSecondPeak;

    if (s.size() != (int)HistScale)
        return mSecondPeak;

    // --- 1. Находим пик около 0 (центр гистограммы) -------------------------
    const int binA = (int)HistScale / 2 - (int)HistScale / 64;
    const int binB = (int)HistScale / 2 + (int)HistScale / 64;

    if (binA >= binB)
        return mSecondPeak;

    GaussianPeak zero = findMostGaussianPeakBins(s, binA, binB);
    if (zero.peakBin < 0 || zero.sigmaBin <= 0.0)
        return mSecondPeak;      // не нашли нулевой пик

    // --- 2. Ищем ПЕРВЫЙ гауссоподобный пик справа от zero.peakBin ----------
    const int N = s.size();
    const int minGap = (int)HistScale / 64;   // небольшой отступ от нулевого пика
    const int startBin = std::min(zero.peakBin + minGap, N - 2);

    const int halfWin = (int)HistScale / 64;   // окно для подгонки гауссианы
    const double maxErr = 1.5;                // порог на "гауссоподобность"
    const double minSigma = 1.0;              // минимальная ширина пика (в биннах)

    for (int i = startBin; i < N - 1; ++i)
    {
        // простой тест на локальный максимум
        if (!(s[i] > s[i - 1] && s[i] >= s[i + 1]))
            continue;

        int wL = std::max(0, i - halfWin);
        int wR = std::min(N - 1, i + halfWin);

        GaussianPeak gp = findMostGaussianPeakBins(s, wL, wR);

        // нас интересует ПЕРВЫЙ подходящий пик:
        if (gp.peakBin == i && gp.sigmaBin >= minSigma && gp.error <= maxErr)
        {
            mSecondPeak = gp;
            return mSecondPeak;
        }
    }

    // справа ничего гауссоподобного не нашли
    return mSecondPeak;
}

void HistogramDialog::autoRange(bool refresh)
{
    if (mH.isEmpty() || mSmooth.isEmpty()) {
        setRange((int)HistMin, (int)HistMax, true);
        return;
    }

    if (Dicom.TypeOfRecord != CT && Dicom.TypeOfRecord != CT3DR) {
        setRange((int)HistMin, (int)HistMax, true);
        return;
    }

    
    if (autoleft != -1 && autoright != -1)
    {
        if (!refresh)
            setRange(autoleft, autoright, true);
        return;
    }

    QVector<double> s = mSmooth;
    if (mIgnoreZeros && !s.isEmpty())
        s[(int)HistMin] = 0.0;

    const int N = s.size();
    if (N < 5) {
        setRange((int)HistMin, (int)HistMax, true);
        return;
    }

    const double gmax = *std::max_element(s.begin(), s.end());
    if (gmax <= 0.0) {
        setRange((int)HistMin, (int)HistMax, true);
        return;
    }

    GaussianPeak findSecondPeak = FindSecondPeak(s);

    int A = 200;
    if (findSecondPeak.peakBin != -1)
        A = (int)axisFromData(findSecondPeak.peakBin);

    const int B = 800;

    // рабочий диапазон поиска пика: A..B HU
    const int binA = std::clamp(dataFromAxis(A), (int)HistMin, (int)HistMax);
    const int binB = std::clamp(dataFromAxis(B), (int)HistMin, (int)HistMax);

    if (binA >= binB) {
        setRange((int)HistMin, (int)HistMax, true);
        return;
    }

    GaussianPeak gp = findMostGaussianPeakBins(s, binA, binB);
    //qDebug() << "peakBin" << gp.peakBin << "muBin" << gp.muBin << "sigmaBin" << gp.sigmaBin << "error" << gp.error;
    if (gp.peakBin < 0 || gp.sigmaBin <= 0.0)
    {
        setRange(binA, binB, true);
        return;
    }

    //for (int i = binA - 1; i < binB + 1; ++i) {
    //    qDebug() << " i " << i << " axis " << axisFromData(i) << " s " << s[i];
    //}

    // === 2. Левая граница: как раньше (впадина или перегиб) ===
    PeakInfo P = analyzePeak(s, gp.peakBin);
    int loBin = -1;

    if (binA >= gp.peakBin)
    {
        loBin = binA;
    }
    else
    {
        // 1) простая впадина
        int valleySimple = findLeftValleySimple(s, gp.peakBin, binA);
        if (valleySimple >= binA && valleySimple < gp.peakBin)
        {
            loBin = valleySimple;
        }
        else if (P.leftValley >= binA && P.leftValley < gp.peakBin)
        {
            // 2) впадина из анализа пика
            loBin = P.leftValley;
        }
        else
        {
            // 3) fallback: перегиб по кривизне, ОГРАНИЧЕННЫЙ справа/слева
            QVector<double> d1, d2;
            firstSecondDeriv(s, d1, d2);

            int winL = gp.peakBin - 40;
            int winR = gp.peakBin - 1;

            // жёстко ограничиваем диапазон поиска
            winL = std::max(winL, binA);
            winR = std::clamp(winR, binA, gp.peakBin - 1);

            int inflect = leftInflectionByCurvature(s, d1, d2, winL, winR, gp.peakBin);

            // страхуемся: результат тоже должен быть >= binA
            loBin = std::clamp(inflect, binA, gp.peakBin - 1);
        }
    }

    //qDebug() << " Left i " << loBin << " axis " << axisFromData(loBin) << " s " << s[loBin];

    // === 3. Правая граница: центр + 2.8 σ (в биннах) ===
    int RightBySigma = int(std::round(gp.peakBin + 2.8 * gp.sigmaBin));
    RightBySigma = std::clamp(RightBySigma, binB, (int)HistMax);

    //qDebug() << " Right i " << RightBySigma << " axis " << axisFromData(RightBySigma) << " s " << s[RightBySigma];

    autoleft = loBin;
    autoright = RightBySigma;
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
    if (mIgnoreZeros && !hForNorm.isEmpty())
        hForNorm[(int)HistMin] = 0;
    const double cap = percentileCap(hForNorm, 0.995);
    const double denom = (cap > 0.0) ? cap : 1.0;
    auto val = [&](int i)->double {
        const double v = (i >= 0 && i < mSmooth.size()) ? mSmooth[i] : 0.0;
        return std::clamp(v / denom, 0.0, 1.0);
        };

    // --- сетка Y ---
    p.setPen(QPen(grid, 1));
    QFontMetrics fm(p.font());

    const int mul = SubStep * SubStep * SubStep;

    const double stepY = niceStep(cap / 5.0);
    for (double v = 0; v <= cap + 1e-9; v += stepY) {
        const double y = r.bottom() - (v / cap) * r.height();
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        double shownV = v * mul;
        QString lbl;
        if (shownV >= 1e6) lbl = QString::number(shownV / 1e6, 'f', 1) + "M";
        else if (shownV >= 1e3) lbl = QString::number(shownV / 1e3, 'f', 0) + "k";
        else               lbl = QString::number(int(std::round(shownV)));
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
    for (int i = (int)HistMin; i <= (int)HistMax; ++i) addPoint(i, pathAll);
    pathAll.lineTo(r.right(), r.bottom()); pathAll.closeSubpath();

    // только между mLo..mHi
    pathSel.moveTo(r.left() + dataToX(mLo) * r.width(), r.bottom());
    for (int i = mLo; i <= mHi; ++i) addPoint(i, pathSel);
    pathSel.lineTo(r.left() + dataToX(mHi) * r.width(), r.bottom());
    pathSel.closeSubpath();

    // линия контура (по всем точкам)
    pathLine.moveTo(r.left(), r.bottom() - r.height() * val((int)HistMin));
    for (int i = (int)HistMin + 1; i <= (int)HistMax; ++i) {
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