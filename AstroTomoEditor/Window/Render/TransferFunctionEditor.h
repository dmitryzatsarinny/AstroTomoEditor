#pragma once
#include "..\MainWindow\DialogShell.h"
#include <QVector>
#include <QColor>
#include <QPointer> 
#include "../../Services/DicomRange.h"
#include <QDialogButtonBox> 

class QWidget;
class QSlider;
class QPushButton;
class QLabel;

class vtkImageData;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;

// одна контрольная точка TF
struct TfPoint {
    double x = 0.0;      // 0..255 (или min..max, но здесь u8)
    double a = 0.0;      // 0..1   (opacity)
    QColor color = Qt::white;
};

class TfCanvas;  // виджет рисования (гистограмма+кривая)

class TransferFunctionEditor : public DialogShell
{
    Q_OBJECT
public:
    explicit TransferFunctionEditor(QWidget* parent, vtkImageData* imgU8);
    ~TransferFunctionEditor() override = default;
    void setFixedAxis(double axisMin, double axisMax) { mMin = axisMin;  mMax = axisMax; };

    // текущее состояние
    const QVector<TfPoint>& points() const { return mPts; }
    void setFromVtk(vtkColorTransferFunction* ctf,
        vtkPiecewiseFunction* otf,
        double minVal, double maxVal);
    void refreshHistogram(vtkImageData* img);
signals:
    void preview(vtkColorTransferFunction* ctf, vtkPiecewiseFunction* otf);
    void committed(vtkColorTransferFunction* ctf, vtkPiecewiseFunction* otf);
    void presetSaved();

private slots:
    void onCanvasChanged();      // точки изменились / выбор изменился
    void onRgbChanged();         // ползунки R/G/B
    void onAutoColors();

private:
    void rebuildPreview(bool emitPreview);
    vtkColorTransferFunction* makeCTF(const QVector<TfPoint>& pts);
    vtkPiecewiseFunction* makeOTF(const QVector<TfPoint>& pts);

protected:
    void changeEvent(QEvent* e) override;

private:
    void retranslateUi();

    QLabel* mLblR = nullptr;
    QLabel* mLblG = nullptr;
    QLabel* mLblB = nullptr;

    QPushButton* mBtnAuto = nullptr;
    QPushButton* mBtnSave = nullptr;

    QDialogButtonBox* mBB = nullptr;

    TfCanvas* mCanvas{ nullptr };
    QSlider* mR{ nullptr };
    QSlider* mG{ nullptr };
    QSlider* mB{ nullptr };
    QLabel* mSwatch{ nullptr };

    QVector<TfPoint>  mPts;
    int               mSel{ -1 };
    QVector<quint64>  mHist;   // HistScale столбцов
    double mMin{ HistMin }, mMax{ HistMax };
};