#pragma once
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkObjectFactory.h>
#include <QGuiApplication>

static inline bool isCtrlDown()
{
#if defined(Q_OS_MAC)
    return QGuiApplication::keyboardModifiers().testFlag(Qt::MetaModifier); // Cmd на macOS
#else
    return QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
#endif
}

class InteractorStyleCtrlWheelShiftPan : public vtkInteractorStyleTrackballCamera
{
public:
    static InteractorStyleCtrlWheelShiftPan* New();
    vtkTypeMacro(InteractorStyleCtrlWheelShiftPan, vtkInteractorStyleTrackballCamera);

    void OnMiddleButtonDown() override {
        if (this->Interactor && this->Interactor->GetControlKey()) 
        {
            this->StartDolly();
        }
        else if (this->Interactor && this->Interactor->GetShiftKey())
        {
            this->StartPan();
        }
        else 
        {
            this->StartRotate();
        }
        this->InvokeEvent(vtkCommand::MiddleButtonPressEvent);
    }
    void OnMiddleButtonUp() override 
    {
        switch (this->State)
        {
        case VTKIS_DOLLY:
            this->EndDolly();
            break;
        case VTKIS_ROTATE:
            this->EndRotate();
            break;
        case VTKIS_PAN:
            this->EndPan();
            break;
        default:
                break;
        }
        this->InvokeEvent(vtkCommand::MiddleButtonReleaseEvent);
        if (this->Interactor) this->Interactor->Render();
    }

    void OnMouseWheelBackward() override {}
    void OnMouseWheelForward() override {}
    void OnLeftButtonDown() override {}
    void OnLeftButtonUp()   override {}
    void OnRightButtonDown() override {}
    void OnRightButtonUp()   override {}
};
vtkStandardNewMacro(InteractorStyleCtrlWheelShiftPan);