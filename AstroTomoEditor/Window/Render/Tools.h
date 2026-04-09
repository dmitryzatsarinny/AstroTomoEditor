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
    struct MenuOptions
    {
        bool scissorsOnly = false;
    };

    QMenu* CreateMenu(QWidget* parent,
        std::function<void(Action)> onAction,
        const MenuOptions& options = {});

    QString ToDisplayName(Action a);

    QMenu* CreateAppMenu(QWidget* parent,
        std::function<void(App)> onAction,
        bool electrodesEnabled = true);

    QString ToDisplayAppName(App a);
}