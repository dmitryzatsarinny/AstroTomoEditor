#include "Tools.h"
#include <QAction>
#include <QWidget>
#include <QPointer>
#include <QVector>
#include <QPoint>
#include <functional>
#include <vtkSmartPointer.h>

class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkVolume;
class vtkImageData;

namespace Tools 
{

    QMenu* CreateMenu(QWidget* parent, std::function<void(Action)> onAction)
    {
        auto* menu = new QMenu(parent);

        QObject::connect(menu->addAction(QObject::tr("Scissors")), &QAction::triggered, [onAction] { onAction(Action::Scissors); });
        QObject::connect(menu->addAction(QObject::tr("InverseScissors")), &QAction::triggered, [onAction] { onAction(Action::InverseScissors); });
        menu->addSeparator();
        QObject::connect(menu->addAction(QObject::tr("RemoveUnconnected")), &QAction::triggered, [onAction] { onAction(Action::RemoveUnconnected); });
        QObject::connect(menu->addAction(QObject::tr("RemoveSelected")), &QAction::triggered, [onAction] { onAction(Action::RemoveSelected); });
        QObject::connect(menu->addAction(QObject::tr("RemoveConnected")), &QAction::triggered, [onAction] { onAction(Action::RemoveConnected); });

        return menu;
    }

    QString ToDisplayName(Action a)
    {
        switch (a) {
        case Action::Scissors:        return QObject::tr("Scissors");
        case Action::InverseScissors: return QObject::tr("Inverse scissors");
        case Action::RemoveUnconnected:return QObject::tr("Remove unconnected");
        case Action::RemoveSelected: return QObject::tr("Remove selected");
        case Action::RemoveConnected: return QObject::tr("Remove connected");
        }
        return QObject::tr("Tool");
    }
} // namespace Tools
