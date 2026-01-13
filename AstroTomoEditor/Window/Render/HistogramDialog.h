#pragma once
#include <QDialog>
#include <QVector>
#include <QImage>
#include <Services/DicomRange.h>
#include "U8Span.h"
#include "..\MainWindow\DialogShell.h"
#include "..\MainWindow\TitleBar.h"

class QWidget;
class QPushButton;
class vtkImageData;

struct GaussianPeak {
    int    peakBin = -1;   // индекс бина вершины (апекс)
    double muBin = 0.0;  // центр гаусса в БИНАХ
    double sigmaBin = 0.0; // σ в БИНАХ
    double error = std::numeric_limits<double>::infinity();
};

class HistogramDialog : public DialogShell {
    Q_OBJECT
public:
    explicit HistogramDialog(QWidget* parent, DicomInfo DI, vtkImageData* image = nullptr);

    // Фиксированная ось a..b для подписей (a ↔ HistMin, b ↔ HistMax)
    void setFixedAxis(bool enabled,
        double a = 0.0,
        double b = static_cast<double>(HistMax));

    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

    // Полный пересчёт гистограммы от текущего тома
    void refreshFromImage(vtkImageData* image);

    double axisMin() const { return mAxisMin; }
    double axisMax() const { return mAxisMax; }

    void HideAutoRange(vtkImageData* image);
    void HideRangeIfCT(vtkImageData* image, int HLeft, int HRight);

signals:
    // ВАЖНО: отдаем ГРАНИЦЫ В ДАННЫХ HistMin..HistMax
    void rangeChanged(int left, int right);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void done(int r) override;
    void changeEvent(QEvent* e) override;

private:
    enum class Drag { None, Left, Right, Pan };
    std::function<void()> m_onFinished;

    // UI
    void buildUi();
    void paintCanvas();
    void autoRange(bool refresh);
    void setRange(int loBin, int hiBin, bool emitSig);

    void setRangeAxis(double loAxis, double hiAxis, bool emitSig = true);
    GaussianPeak FindSecondPeak(const QVector<double>& s);

    // Подсчёт гистограммы из тома в HistScale бинов (HistMin..HistMax)
    void buildHistogram();

    // Маппинги бин ↔ [0..1] по ширине канвы
    inline double dataToX(int d) const {
        const double span = static_cast<double>(HistMax - HistMin);
        if (span <= 0.0) return 0.0;
        const double t = (static_cast<double>(d) - HistMin) / span;
        return std::clamp(t, 0.0, 1.0);
    }

    inline int xToData(double t) const {
        t = std::clamp(t, 0.0, 1.0);
        const double span = static_cast<double>(HistMax - HistMin);
        const double v = HistMin + t * span;
        return std::clamp(static_cast<int>(std::round(v)),
            static_cast<int>(HistMin),
            static_cast<int>(HistMax));
    }

    // Маппинг бин ↔ физическая ось (HU и т.п.)
    inline double axisFromData(int d) const {
        const double a = mAxisMin, b = mAxisMax;
        const double spanBins = static_cast<double>(HistMax - HistMin);
        if (spanBins <= 0.0) return a;
        const double t = (std::clamp(d, static_cast<int>(HistMin),
            static_cast<int>(HistMax)) - HistMin) / spanBins;
        return a + t * (b - a);
    }

    inline int dataFromAxis(double v) const {
        const double a = mAxisMin, b = mAxisMax;
        if (b <= a) return static_cast<int>(HistMin);
        const double spanBins = static_cast<double>(HistMax - HistMin);
        double t = (v - a) / (b - a);          // 0..1 по физической оси
        t = std::clamp(t, 0.0, 1.0);
        const double bin = HistMin + t * spanBins;
        return std::clamp(static_cast<int>(std::round(bin)),
            static_cast<int>(HistMin),
            static_cast<int>(HistMax));
    }

    // nice step для делений оси
    static double niceStep(double span) {
        if (span <= 0) return 1.0;
        double p = std::pow(10.0, std::floor(std::log10(span)));
        double f = span / p;
        double nf = (f < 1.5) ? 1 : (f < 3) ? 2 : (f < 7) ? 5 : 10;
        return nf * p;
    }

    void retranslateUi();
private:
    // данные
    vtkImageData* mImage{ nullptr };
    QVector<quint64> mH;        // HistScale бинов
    QVector<double>  mSmooth;   // сглаженная кривая (визуализация)
    QImage           mCache;
    DicomInfo        Dicom;

    // ось для подписей
    bool   mAxisFixed{ false };
    double mAxisMin{ 0.0 };
    double mAxisMax{ static_cast<double>(HistMax) };

    int    autoleft = -1;
    int    autoright = -1;

    // выбор пользователя — ВСЕГДА в ДАННЫХ HistMin..HistMax
    int mLo{ static_cast<int>(HistMin) };
    int mHi{ static_cast<int>(HistMax) };

    int SubStep = 2;

    // UI
    QWidget* mCanvas{ nullptr };
    QPushButton* mBtnAuto{ nullptr };

    // ввод
    Drag mDrag{ Drag::None };
    int  mPanStartD{ static_cast<int>(HistMin) }; // начальная позиция в данных

    bool mIgnoreZeros = true;
};
