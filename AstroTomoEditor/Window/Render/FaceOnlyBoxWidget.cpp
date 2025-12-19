#include "FaceOnlyBoxWidget.h"
#include "FaceOnlyBoxRepresentation.h"

#include <vtkObjectFactory.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkWidgetCallbackMapper.h>
#include <vtkWidgetEvent.h>
#include <vtkWidgetEventTranslator.h>
#include <vtkRenderWindow.h>
#include <vtkCallbackCommand.h>

vtkStandardNewMacro(FaceOnlyBoxWidget);

FaceOnlyBoxWidget::FaceOnlyBoxWidget()
{
    this->ManagesCursor = 1;

    this->CallbackMapper->SetCallbackMethod(
        vtkCommand::MouseMoveEvent, 
        vtkWidgetEvent::Move,
        this, FaceOnlyBoxWidget::MouseMoveCallback);

    this->CallbackMapper->SetCallbackMethod(
        vtkCommand::LeftButtonPressEvent,
        vtkWidgetEvent::Select,
        this, FaceOnlyBoxWidget::LeftPressCallback);

    this->CallbackMapper->SetCallbackMethod(
        vtkCommand::LeftButtonReleaseEvent,
        vtkWidgetEvent::EndSelect,
        this, FaceOnlyBoxWidget::LeftReleaseCallback);

    this->CallbackMapper->SetCallbackMethod(
        vtkCommand::RightButtonPressEvent,
        vtkWidgetEvent::Translate,
        this, FaceOnlyBoxWidget::RightPressCallback);

    this->CallbackMapper->SetCallbackMethod(
        vtkCommand::RightButtonReleaseEvent,
        vtkWidgetEvent::EndTranslate,
        this, FaceOnlyBoxWidget::RightReleaseCallback);

}

void FaceOnlyBoxWidget::CreateDefaultRepresentation()
{
    if (!this->WidgetRep)
        this->WidgetRep = FaceOnlyBoxRepresentation::New();
}

FaceOnlyBoxRepresentation* FaceOnlyBoxWidget::GetFaceRep()
{
    return FaceOnlyBoxRepresentation::SafeDownCast(this->WidgetRep);
}

void FaceOnlyBoxWidget::SetEnabled(int enabling)
{
    this->Superclass::SetEnabled(enabling);
    if (!enabling) mState = State::Idle;
}

void FaceOnlyBoxWidget::SetRepresentation(FaceOnlyBoxRepresentation* rep)
{
    if (this->WidgetRep == rep) return;

    if (this->WidgetRep) {
        this->WidgetRep->UnRegister(this);
        this->WidgetRep = nullptr;
    }

    this->WidgetRep = rep;

    if (this->WidgetRep) {
        this->WidgetRep->Register(this);
    }

    this->Modified();
}

void FaceOnlyBoxWidget::MouseMoveCallback(vtkAbstractWidget* w)
{
    static_cast<FaceOnlyBoxWidget*>(w)->OnMouseMove();
}
void FaceOnlyBoxWidget::LeftPressCallback(vtkAbstractWidget* w)
{
    static_cast<FaceOnlyBoxWidget*>(w)->OnLeftPress();
}
void FaceOnlyBoxWidget::LeftReleaseCallback(vtkAbstractWidget* w)
{
    static_cast<FaceOnlyBoxWidget*>(w)->OnLeftRelease();
}
void FaceOnlyBoxWidget::RightPressCallback(vtkAbstractWidget* w)
{
    static_cast<FaceOnlyBoxWidget*>(w)->OnRightPress();
}
void FaceOnlyBoxWidget::RightReleaseCallback(vtkAbstractWidget* w)
{
    static_cast<FaceOnlyBoxWidget*>(w)->OnRightRelease();
}

void FaceOnlyBoxWidget::OnMouseMove()
{
    auto* rep = this->GetFaceRep();
    auto* iren = this->Interactor;
    auto* ren = this->CurrentRenderer;

    if (!rep || !iren || !ren) return;

    const int x = iren->GetEventPosition()[0];
    const int y = iren->GetEventPosition()[1];

    if (mState == State::Dragging)
    {
        // rep сам знает, что сейчас за режим (Whole/Rotate/Face)
        rep->DragWhole(x, y, ren);
        rep->DragRotate(x, y, ren);
        rep->DragTo(x, y, ren);

        this->EventCallbackCommand->SetAbortFlag(1);
        this->InvokeEvent(vtkCommand::InteractionEvent, nullptr);
        this->Render();
        return;
    }

    rep->UpdateHoverFromDisplay(x, y, ren);
    this->Render();
}


void FaceOnlyBoxWidget::OnLeftPress()
{
    auto* iren = this->Interactor;
    auto* ren = this->CurrentRenderer;
    auto* rep = GetFaceRep();
    if (!iren || !ren || !rep) return;

    int* p = iren->GetEventPosition();
    if (!p) return;

    rep->UpdateHoverFromDisplay(p[0], p[1], ren);
    if (rep->ActiveFace() < 0) return;

    rep->StartDrag(p[0], p[1], ren);
    mState = State::Dragging;

    this->InvokeEvent(vtkCommand::StartInteractionEvent, nullptr);

    if (auto* rw = iren->GetRenderWindow())
        rw->Render();
}

void FaceOnlyBoxWidget::OnLeftRelease()
{
    auto* rep = GetFaceRep();
    if (!rep) return;

    if (mState == State::Dragging) {
        rep->EndDrag();
        mState = State::Idle;
        this->InvokeEvent(vtkCommand::EndInteractionEvent, nullptr);
    }

    if (auto* rw = this->Interactor->GetRenderWindow())
        rw->Render();
}

void FaceOnlyBoxWidget::OnRightPress()
{
    auto* rep = this->GetFaceRep();
    auto* iren = this->Interactor;
    auto* ren = this->CurrentRenderer;

    if (!rep || !iren || !ren) return;

    const int x = iren->GetEventPosition()[0];
    const int y = iren->GetEventPosition()[1];

    if (iren->GetAltKey())
        rep->StartRotate(x, y, ren);
    else
        rep->StartDragWhole(x, y, ren);

    mState = State::Dragging;

    // важно: чтобы VTK-стиль (пан/зум) не сработал параллельно
    this->EventCallbackCommand->SetAbortFlag(1);

    rep->SetFillActorUnVisible();

    this->InvokeEvent(vtkCommand::StartInteractionEvent, nullptr);
    this->Render();
}


void FaceOnlyBoxWidget::OnRightRelease()
{
    auto* rep = GetFaceRep();
    if (!rep) return;

    rep->EndDrag();
    mState = State::Idle; // <-- тоже надо

    rep->SetFillActorVisible();

    this->InvokeEvent(vtkCommand::EndInteractionEvent, nullptr);
    this->Interactor->GetRenderWindow()->Render();
}

