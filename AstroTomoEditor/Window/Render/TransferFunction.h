#pragma once
// TransferFunction.h
// Небольшой модуль с пресетами цветовой/прозрачностной передаточной функции для VTK.

#include <functional>
#include <memory>
#include <vtkSmartPointer.h>
#include <QDIR>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QMenu>

class QWidget;
class QMenu;
class vtkVolumeProperty;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;

enum class TFPreset {
    Grayscale,
    Rainbow,
    Bone,
    Angio,
    SoftTissue,
    Lungs,
    Skin,
    HotMetal,
    InvertCurrent // инвертирует текущую LUT/opacity в свойстве
};

namespace TF {
    QMenu* CreateMenu(QWidget* parent, std::function<void(TFPreset)> onChosen);
    void   ApplyPreset(vtkVolumeProperty* prop, TFPreset preset, double min = 0.0, double max = 255.0);
    void   InvertInPlace(vtkVolumeProperty* prop, double min = 0.0, double max = 255.0);

    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Grayscale(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Grayscale(double min, double max);
    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Rainbow(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Rainbow(double min, double max);
    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Bone(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Bone(double min, double max);
    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Angio(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Angio(double min, double max);
    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_SoftTissue(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_SoftTissue(double min, double max);
    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Lungs(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Lungs(double min, double max);

    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Skin(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Skin(double min, double max);
    vtkSmartPointer<vtkColorTransferFunction> MakeCTF_Hot(double min, double max);
    vtkSmartPointer<vtkPiecewiseFunction>     MakeOTF_Hot(double min, double max);

    struct TFPoint { double x = 0, a = 0, r = 1, g = 1, b = 1; }; // x:0..255, a/r/g/b:0..1

    struct CustomPreset {
        QString  name;
        QString  filePath; // полный путь к json
        double   opacityK = 1.0;
        QString  colorSpace = "Lab"; // "Lab"|"RGB"|"HSV"
        QVector<TFPoint> points;
    };

    QString PresetsRoot();                       // <exe>/Presets

    bool SaveCustomPreset(const CustomPreset& P);            // write JSON
    QVector<CustomPreset> LoadCustomPresets();

    void ApplyPoints(vtkVolumeProperty* prop,
        const QVector<TFPoint>& pts,
        double min = 0.0, double max = 255.0,
        const QString& colorSpace = "Lab");       // применить к prop

}