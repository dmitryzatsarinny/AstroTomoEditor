#pragma once
#ifndef CLIPBOXCONTROLLER_H
#define CLIPBOXCONTROLLER_H

#include <QObject>
#include <vtkSmartPointer.h>
#include <vtkObject.h>
#include <vtkRenderer.h>
#include <vtkVolume.h>
#include <vtkImageData.h>

class vtkRenderer;
class vtkRenderWindowInteractor;
class vtkBoxWidget2;
class vtkBoxRepresentation;
class vtkCallbackCommand;
class vtkImageData;
class vtkVolume;
class vtkGPUVolumeRayCastMapper;
class vtkActor;
class vtkPlanes;
class vtkPolyDataMapper;

class ClipBoxController : public QObject
{
    Q_OBJECT
public:
    explicit ClipBoxController(QObject* parent = nullptr);
    ~ClipBoxController() override = default;

    void setRenderer(vtkRenderer* ren);
    void setInteractor(vtkRenderWindowInteractor* iren);

    // Привязать к объёмному актёру (vtkVolume) и подготовить виджет
    void attachToVolume(vtkVolume* vol);
    void attachToSTL(vtkActor* vol);

    // Вкл/выкл клиппинг
    void setEnabled(bool on);
    bool isEnabled() const { return mEnabled; }

    // Положить коробку заново по границам объёма
    void resetToBounds();
    void applyNow();

private:
    // применяет текущие границы виджета как planes в мэппер
    void applyClippingFromBox();

    // callback для InteractionEvent у box-representation
    static void onInteraction(vtkObject*, unsigned long, void*, void*);

private:
    bool mEnabled{ false };

    vtkSmartPointer<vtkRenderer>                mRenderer;
    vtkSmartPointer<vtkRenderWindowInteractor>  mInteractor;
    vtkSmartPointer<vtkVolume>                  mVolume;
    vtkSmartPointer<vtkActor>                   mSurface;
    vtkSmartPointer<vtkBoxWidget2>              mWidget;
    vtkSmartPointer<vtkBoxRepresentation>       mRep;
    vtkSmartPointer<vtkCallbackCommand>         mCb;
};

#endif // CLIPBOXCONTROLLER_H