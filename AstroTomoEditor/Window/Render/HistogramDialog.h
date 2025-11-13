#pragma once
#include <QDialog>
#include <QVector>
#include <QImage>
#include <Services/DicomRange.h>
#include "U8Span.h"

class QWidget;
class QPushButton;
class vtkImageData;

class HistogramDialog : public QDialog {
    Q_OBJECT
public:
    explicit HistogramDialog(QWidget* parent, DicomInfo DI, vtkImageData* image = nullptr);

    // Фиксированная ось a..b для подписей (a ↔ 0, b ↔ 255)
    void setFixedAxis(bool enabled, double a = 0.0, double b = 255.0);
    void setOnFinished(std::function<void()> cb) { m_onFinished = std::move(cb); }

    // Полный пересчёт гистограммы от текущего тома
    void refreshFromImage(vtkImageData* image);

    double axisMin() const { return mAxisMin; }
    double axisMax() const { return mAxisMax; }
signals:
    // ВАЖНО: отдаем ГРАНИЦЫ В ДАННЫХ 0..255
    void rangeChanged(int left, int right);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void done(int r) override;

private:
    enum class Drag { None, Left, Right, Pan };
    std::function<void()> m_onFinished;
    // UI
    void buildUi();
    void paintCanvas();
    void autoRange();
    void setRange(int loBin, int hiBin, bool emitSig);
    void setRangeAxis(double loAxis, double hiAxis, bool emitSig = true);
    // Подсчёт гистограммы из тома в 256 бинов (0..255)
    void buildHistogram();

    // Маппинги
    inline double dataToX(int d) const {
        return std::clamp(d / 255.0, 0.0, 1.0);
    }
    inline int xToData(double t) const {
        t = std::clamp(t, 0.0, 1.0);
        return std::clamp(int(std::round(t * 255.0)), 0, 255);
    }
    inline double axisFromData(int d) const {
        const double a = mAxisMin, b = mAxisMax;
        return a + (std::clamp(d, 0, 255) / 255.0) * (b - a);
    }
    inline int dataFromAxis(double v) const {
        const double a = mAxisMin, b = mAxisMax;
        if (b <= a) return 0;
        double t = (v - a) / (b - a);
        return std::clamp(int(std::round(t * 255.0)), 0, 255);
    }

    // nice step для делений оси
    static double niceStep(double span) {
        if (span <= 0) return 1.0;
        double p = std::pow(10.0, std::floor(std::log10(span)));
        double f = span / p;
        double nf = (f < 1.5) ? 1 : (f < 3) ? 2 : (f < 7) ? 5 : 10;
        return nf * p;
    }

private:
    // данные
    vtkImageData* mImage{ nullptr };
    QVector<quint64> mH;        // 256 бинов
    QVector<double>  mSmooth;   // сглаженная кривая (визуализация)
    QImage           mCache;
    DicomInfo        Dicom;
    // ось для подписей
    bool   mAxisFixed{ false };
    double mAxisMin{ 0.0 };
    double mAxisMax{ 255.0 };

    // выбор пользователя — ВСЕГДА в ДАННЫХ 0..255
    int mLo{ 0 };
    int mHi{ 255 };

    int SubStep = 2;

    // UI
    QWidget* mCanvas{ nullptr };
    QPushButton* mBtnAuto{ nullptr };

    // ввод
    Drag   mDrag{ Drag::None };
    int    mPanStartD{ 0 }; // начальная позиция в данных (0..255)

    bool mIgnoreZeros = true;
};
