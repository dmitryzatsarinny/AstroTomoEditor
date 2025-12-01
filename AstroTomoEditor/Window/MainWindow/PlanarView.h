#pragma once
#ifndef PLANARVIEW_h
#define PLANARVIEW_h

#include "..\..\Services\Pool.h"
#include "vtkMatrix3x3.h"
#include "..\..\Services/DicomRange.h"

class QGraphicsPixmapItem;
class QGraphicsScene;
class QGraphicsView;
class QSlider;
class QLabel;
class QProgressBar;
class QHBoxLayout;
class QResizeEvent;
class QWheelEvent;
class QKeyEvent;
class QEvent;

// VTK forward
class vtkAlgorithmOutput;
class vtkImageData;
template <class T> class vtkSmartPointer;

class PlanarView : public QWidget
{
    Q_OBJECT
public:
    explicit PlanarView(QWidget* parent = nullptr);

    // API
    void loadSeriesFiles(const QVector<QString>& files);
    void setWindowLevel(double level, double width);
    void fitToWindow();
    void resetZoom();
    void rebuildPixmap();
    

    QVector3D voxelSpacing() const { return { float(mSpX), float(mSpY), float(mSpZ) }; }

    // Собрать 3D-объём из mSlices (8-bit) для VTK
    vtkSmartPointer<vtkImageData> makeVtkVolume() const;
    DicomInfo GetDicomInfo() const { return Dicom; };

    QMatrix3x3 directionLPS() const { return mDir; }
    QVector3D  originLPS() const { return { float(mOrg[0]), float(mOrg[1]), float(mOrg[2]) }; }
    bool IsAvalibleToReconstruct() { return avalibletoreconstruction;  }
    bool IsLoading() { return loading; }
    void StartLoading() { loading = true; }
    void StopLoading() { loading = false; }
    void sethidescroll() { if (mScroll) mScroll->hide(); }

    struct ValidationReport {
        QVector<QString> good, bad;
        QString reason;
    };
    static ValidationReport filterSeriesByConsistency(const QVector<QString>& files);

signals:
    void loadStarted(int total);
    void loadProgress(int processed, int total);
    void loadFinished(int total);

    void showInfo(const QString& text);
    void showWarning(const QString& text);
    void filesFiltered(int good, int bad);

public slots:
    void setSlice(int i);

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    
private:
    // UI/overlay
    void initUi();
    void nudgeSlice(int delta);
    void pageSlice(int delta);

    // Кэширование QImage из vtkImageData
    void buildCache(vtkImageData* volume, vtkAlgorithmOutput* srcPort, bool invertMono1, DicomInfo Dicom);

    // Валидация/фильтрация серии
    struct DicomPixelKey {
        int rows = 0, cols = 0, bitsAllocated = 0, samplesPerPixel = 0, pixelRepresentation = 0;
        QString photometric;
        bool operator==(const DicomPixelKey& o) const {
            return rows == o.rows && cols == o.cols && bitsAllocated == o.bitsAllocated &&
                samplesPerPixel == o.samplesPerPixel && pixelRepresentation == o.pixelRepresentation &&
                photometric == o.photometric;
        }
    };
    static bool readPixelKeyQuick(const QString& file, DicomPixelKey& out, QString* errMsg = nullptr);
    
    

private:
    DicomInfo Dicom;

    // вид + слайдер
    QGraphicsPixmapItem* mImageItem{ nullptr };
    QGraphicsScene* mScene{ nullptr };
    QGraphicsView* mView{ nullptr };
    QSlider* mScroll{ nullptr };


    // данные
    QVector<QImage> mSlices;
    int             mIndex{ 0 };
    double          mWL{ 0.0 };
    double          mWW{ 0.0 };
    bool            mAutoFit{ true };
    double          mSpX{ 1.0 }, mSpY{ 1.0 }, mSpZ{ 1.0 };
    int             X{ 1 }, Y{ 1 }, Z{ 1 };
    bool            flipX{ false };
    bool            flipY{ false };
    bool            flipZ{ false };

    bool            avalibletoreconstruction{ false };
    bool            loading{ false };
    QMatrix3x3 mDir;
    double     mOrg[3]{ 0.0, 0.0, 0.0 };
};

#endif // PLANARVIEW_h