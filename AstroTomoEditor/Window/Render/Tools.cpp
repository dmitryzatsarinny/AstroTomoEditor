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
        QObject::connect(menu->addAction(QObject::tr("Inverse scissors")), &QAction::triggered, [onAction] { onAction(Action::InverseScissors); });
        menu->addSeparator();
        QObject::connect(menu->addAction(QObject::tr("Remove unconnected")), &QAction::triggered, [onAction] { onAction(Action::RemoveUnconnected); });
        QObject::connect(menu->addAction(QObject::tr("Remove selected")), &QAction::triggered, [onAction] { onAction(Action::RemoveSelected); });
        QObject::connect(menu->addAction(QObject::tr("Remove connected")), &QAction::triggered, [onAction] { onAction(Action::RemoveConnected); });
        QObject::connect(menu->addAction(QObject::tr("Smart deleting")), &QAction::triggered, [onAction] { onAction(Action::SmartDeleting); });
        menu->addSeparator();
        QObject::connect(menu->addAction(QObject::tr("Voxel eraser")), &QAction::triggered, [onAction] { onAction(Action::VoxelEraser); });
        QObject::connect(menu->addAction(QObject::tr("Voxel recovery")), &QAction::triggered, [onAction] { onAction(Action::VoxelRecovery); });
        menu->addSeparator();
        QObject::connect(menu->addAction(QObject::tr("Add base")), &QAction::triggered, [onAction] { onAction(Action::AddBase); });
        QObject::connect(menu->addAction(QObject::tr("Fill the void")), &QAction::triggered, [onAction] { onAction(Action::FillEmpty); });
        QObject::connect(menu->addAction(QObject::tr("Total smoothing")), &QAction::triggered, [onAction] { onAction(Action::TotalSmoothing); });
        QObject::connect(menu->addAction(QObject::tr("Surface mapping")), &QAction::triggered, [onAction] { onAction(Action::SurfaceMapping); });
        QObject::connect(menu->addAction(QObject::tr("Peel recovery")), &QAction::triggered, [onAction] { onAction(Action::PeelRecovery); });
        menu->addSeparator();
        QObject::connect(menu->addAction(QObject::tr("Plus")), &QAction::triggered, [onAction] { onAction(Action::Plus); });
        QObject::connect(menu->addAction(QObject::tr("Minus")), &QAction::triggered, [onAction] { onAction(Action::Minus); });

        return menu;
    }

    QString ToDisplayName(Action a)
    {
        switch (a) 
        {
        case Action::Scissors:        return QObject::tr("Scissors");
        case Action::InverseScissors: return QObject::tr("Inverse scissors");
        case Action::RemoveUnconnected:return QObject::tr("Remove unconnected");
        case Action::RemoveSelected: return QObject::tr("Remove selected");
        case Action::RemoveConnected: return QObject::tr("Remove connected");
        case Action::SmartDeleting: return QObject::tr("Smart deleting");
        case Action::VoxelEraser: return QObject::tr("Voxel eraser");
        case Action::VoxelRecovery: return QObject::tr("Voxel recovery");
        }
        return QObject::tr("Edit");
    }

    QMenu* CreateAppMenu(QWidget* parent, std::function<void(App)> onAction)
    {
        auto* menu = new QMenu(parent);

        QObject::connect(menu->addAction(QObject::tr("Histogram")), &QAction::triggered, [onAction] { onAction(App::Histogram); });
        QObject::connect(menu->addAction(QObject::tr("Templates")), &QAction::triggered, [onAction] { onAction(App::Templates); });
        QObject::connect(menu->addAction(QObject::tr("Electrodes")), &QAction::triggered, [onAction] { onAction(App::Electrodes); });

        return menu;
    }

    QString ToDisplayAppName(App a)
    {
        switch (a) 
        {
            case App::Histogram:    return QObject::tr("Histogram");
            case App::Templates:    return QObject::tr("Templates");
            case App::Electrodes:   return QObject::tr("Electrodes");
        }
        return QObject::tr("Applications");
    }
}
