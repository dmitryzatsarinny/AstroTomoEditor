#include "AsyncProgressBar.h"
#include <QPainter>

AsyncProgressBar::AsyncProgressBar(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(10);

    mBusyTimer.setInterval(40);
    connect(&mBusyTimer, &QTimer::timeout,
        this, &AsyncProgressBar::onBusyTick);
}

QSize AsyncProgressBar::minimumSizeHint() const { return { 80, 10 }; }
QSize AsyncProgressBar::sizeHint() const { return { 160, 12 }; }

void AsyncProgressBar::setMinimum(int v)
{
    mMin = v;
    if (mValue < mMin) mValue = mMin;
    update();
}
void AsyncProgressBar::setMaximum(int v)
{
    mMax = v;
    if (mValue > mMax) mValue = mMax;
    update();
}
void AsyncProgressBar::setRange(int min, int max)
{
    mMin = min;
    mMax = max;
    mValue = std::clamp(mValue, mMin, mMax);
    update();
}

void AsyncProgressBar::setValue(int v)
{
    v = std::clamp(v, mMin, mMax);
    if (mValue == v) return;

    mValue = v;
    if (mMode == Mode::Loading)
        mMode = Mode::Determinate; // переходим в режим заполнения

    emit valueChanged(mValue);
    update();
}

void AsyncProgressBar::setMode(Mode m)
{
    if (m == mMode) return;

    mMode = m;

    if (mMode == Mode::Loading) {
        mBusyOffset = 0;
        mBusyTimer.start();
    }
    else {
        mBusyTimer.stop();
    }

    emit modeChanged(mMode);
    update();
}

void AsyncProgressBar::onBusyTick()
{
    mBusyOffset += 0.02;      // было 0.04 — помедленнее и плавнее
    if (mBusyOffset > 1.0)
        mBusyOffset -= 1.0;
    update();
}

void AsyncProgressBar::paintEvent(QPaintEvent*)
{
    if (mMode == Mode::Hidden)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF r = rect().adjusted(1, 1, -1, -1);
    if (r.width() <= 0 || r.height() <= 0)
        return;

    QColor bg = palette().window().color();
    QColor fill = palette().highlight().color();
    QColor frame = palette().mid().color();

    const qreal radius = r.height() * 0.5;

    // фон
    p.setPen(QPen(frame, 1));
    p.setBrush(bg);
    p.drawRoundedRect(r, radius, radius);

    if (mMode == Mode::Determinate) {
        // твоя ветка с обычным прогрессом как была – можно не трогать
        if (mMax > mMin) {
            double t = double(mValue - mMin) / double(mMax - mMin);
            t = std::clamp(t, 0.0, 1.0);

            if (t > 0.0) {
                QRectF rf = r;
                rf.setWidth(r.width() * t);

                QColor fillGrad = fill;
                fillGrad.setAlpha(200);

                p.setPen(Qt::NoPen);
                p.setBrush(fillGrad);
                p.drawRoundedRect(rf, radius, radius);
            }
        }
    }
    else if (mMode == Mode::Loading)
    {
        // слабый фон
        QColor base = fill;
        base.setAlpha(40);
        p.setPen(Qt::NoPen);
        p.setBrush(base);
        p.drawRoundedRect(r, radius, radius);

        // сегмент
        const qreal segFrac = 0.30;              // сегмент = 30% ширины
        const qreal segW = r.width() * segFrac;

        // положение сегмента внутри диапазона
        const qreal minX = r.left();
        const qreal maxX = r.right() - segW;

        // смещаем по синусоиде — туда-сюда плавно, красиво
        const qreal t = (1.0 - std::cos(mBusyOffset * M_PI * 2)) * 0.5;
        const qreal x = minX + t * (maxX - minX);

        QRectF seg(x, r.top(), segW, r.height());

        QColor segColor = fill;
        segColor.setAlpha(160);

        p.setBrush(segColor);
        p.drawRoundedRect(seg, radius, radius);
    }
}

