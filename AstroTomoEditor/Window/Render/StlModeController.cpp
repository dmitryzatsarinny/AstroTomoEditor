#include "StlModeController.h"

#include <vtkPolyData.h>

void StlModeController::setHistoryLimit(int limit)
{
    mHistoryLimit = (limit > 0) ? limit : 1;
}

void StlModeController::setActive(bool active)
{
    mActive = active;
}

void StlModeController::resetSurfaceHistory(vtkPolyData* currentSurface)
{
    mSurfaceUndoStack.clear();
    mSurfaceRedoStack.clear();

    if (currentSurface)
        mSurfaceUndoStack.push_back(cloneSurface(currentSurface));
}

bool StlModeController::canUndoSurface() const
{
    return mSurfaceUndoStack.size() > 1;
}

bool StlModeController::canRedoSurface() const
{
    return !mSurfaceRedoStack.isEmpty();
}

void StlModeController::pushSurfaceUndoState(vtkPolyData* currentSurface)
{
    if (!currentSurface)
        return;

    mSurfaceUndoStack.push_back(cloneSurface(currentSurface));
    while (mSurfaceUndoStack.size() > mHistoryLimit)
        mSurfaceUndoStack.pop_front();

    mSurfaceRedoStack.clear();
}

vtkSmartPointer<vtkPolyData> StlModeController::undoSurface(vtkPolyData* currentSurface)
{
    if (!canUndoSurface() || !currentSurface)
        return nullptr;

    mSurfaceRedoStack.push_back(cloneSurface(currentSurface));
    while (mSurfaceRedoStack.size() > mHistoryLimit)
        mSurfaceRedoStack.pop_front();

    mSurfaceUndoStack.pop_back();
    return cloneSurface(mSurfaceUndoStack.back());
}

vtkSmartPointer<vtkPolyData> StlModeController::redoSurface(vtkPolyData* currentSurface)
{
    if (!canRedoSurface() || !currentSurface)
        return nullptr;

    mSurfaceUndoStack.push_back(cloneSurface(currentSurface));
    while (mSurfaceUndoStack.size() > mHistoryLimit)
        mSurfaceUndoStack.pop_front();

    auto next = mSurfaceRedoStack.takeLast();
    return cloneSurface(next);
}

vtkSmartPointer<vtkPolyData> StlModeController::cloneSurface(vtkPolyData* src) const
{
    if (!src)
        return nullptr;

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(src);
    return out;
}