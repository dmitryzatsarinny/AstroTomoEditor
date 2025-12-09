#pragma once
#include <QMenu>
#include <functional>
#include <vtkVolumeProperty.h>

class QWidget;
class QMenu;

enum class Action 
{
    Scissors,
    InverseScissors,
    RemoveUnconnected,
    RemoveConnected,
    SmartDeleting,
    RemoveSelected,
    VoxelEraser,
    VoxelRecovery,
    AddBase,
    FillEmpty,
    TotalSmoothing,
    SurfaceMapping,
    PeelRecovery,
    Plus,
    Minus
};

enum class App 
{
    Histogram,
    Templates,
    Electrodes
};

namespace Tools {

    QMenu* CreateMenu(QWidget* parent,
        std::function<void(Action)> onAction);

    QString ToDisplayName(Action a);

    QMenu* CreateAppMenu(QWidget* parent,
        std::function<void(App)> onAction);

    QString ToDisplayAppName(App a);
}