#pragma once
#include <QMenu>
#include <functional>
#include <vtkVolumeProperty.h>

class QWidget;
class QMenu;

enum class Action {
    Scissors,
    InverseScissors,
    RemoveUnconnected,
    RemoveSelected,
    RemoveConnected,
    // задел: FillHoles, MorphOpen, ...
};

namespace Tools {

    QMenu* CreateMenu(QWidget* parent,
        std::function<void(Action)> onAction);

    QString ToDisplayName(Action a);
    void ApplyAction(vtkVolumeProperty* prop, Action action, double min, double max);
}