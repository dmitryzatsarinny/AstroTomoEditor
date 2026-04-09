#pragma once

#include <QVector>
#include <vtkSmartPointer.h>

class vtkPolyData;

class StlModeController
{
public:
    void setHistoryLimit(int limit);

    void setActive(bool active);
    bool isActive() const { return mActive; }

    void resetSurfaceHistory(vtkPolyData* currentSurface);

    bool canUndoSurface() const;
    bool canRedoSurface() const;

    void pushSurfaceUndoState(vtkPolyData* currentSurface);

    vtkSmartPointer<vtkPolyData> undoSurface(vtkPolyData* currentSurface);
    vtkSmartPointer<vtkPolyData> redoSurface(vtkPolyData* currentSurface);

private:
    vtkSmartPointer<vtkPolyData> cloneSurface(vtkPolyData* src) const;

    bool mActive{ false };
    int mHistoryLimit{ 128 };
    QVector<vtkSmartPointer<vtkPolyData>> mSurfaceUndoStack;
    QVector<vtkSmartPointer<vtkPolyData>> mSurfaceRedoStack;
};