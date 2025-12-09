#pragma once

#include <QWidget>
#include <QTimer>

class AsyncProgressBar : public QWidget
{
    Q_OBJECT
public:
    enum class Mode {
        Hidden,
        Loading,
        Determinate
    };
    Q_ENUM(Mode)

        explicit AsyncProgressBar(QWidget* parent = nullptr);

    int value() const { return mValue; }
    int minimum() const { return mMin; }
    int maximum() const { return mMax; }
    Mode mode()  const { return mMode; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

public slots:
    void setValue(int v);
    void setMinimum(int v);
    void setMaximum(int v);
    void setRange(int min, int max);
    void setMode(Mode m);

    void startLoading() { setMode(Mode::Loading); }
    void startFill() { setMode(Mode::Determinate); }
    void hideBar() { setMode(Mode::Hidden); }

signals:
    void valueChanged(int value);
    void modeChanged(AsyncProgressBar::Mode mode);

protected:
    void paintEvent(QPaintEvent* e) override;

private slots:
    void onBusyTick();

private:
    QTimer mBusyTimer;
    int mValue = 0;
    int mMin = 0;
    int mMax = 100;
    Mode mMode = Mode::Hidden;

    double mBusyOffset = 0.0;
};
