#pragma once
#include <vtkAbstractWidget.h>
#include <vtkSmartPointer.h>

class FaceOnlyBoxRepresentation;

class FaceOnlyBoxWidget : public vtkAbstractWidget
{
public:
    static FaceOnlyBoxWidget* New();
    vtkTypeMacro(FaceOnlyBoxWidget, vtkAbstractWidget);

    void SetEnabled(int enabling) override;
    void CreateDefaultRepresentation() override;

    FaceOnlyBoxRepresentation* GetFaceRep();
    void SetRepresentation(FaceOnlyBoxRepresentation* rep);
protected:
    FaceOnlyBoxWidget();
    ~FaceOnlyBoxWidget() override = default;

    static void MouseMoveCallback(vtkAbstractWidget*);
    static void LeftPressCallback(vtkAbstractWidget*);
    static void LeftReleaseCallback(vtkAbstractWidget*);
    static void RightPressCallback(vtkAbstractWidget*);
    static void RightReleaseCallback(vtkAbstractWidget*);

    void OnMouseMove();
    void OnLeftPress();
    void OnLeftRelease();
    void OnRightPress();
    void OnRightRelease();

private:
    enum class State { Idle, Dragging };
    State mState = State::Idle;
};
