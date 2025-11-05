#include "..\Services\Pool.h"
#include "..\Window\Explorer\ExplorerDialog.h"
#include "..\Window\MainWindow\MainWindow.h"

#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>

static void ConfigureDllSearch()
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(L".\\bin");
    AddDllDirectory(L".\\plugins");
    SetDllDirectoryW(L"");
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + "/plugins");
}

int main(int argc, char* argv[])
{
    ConfigureDllSearch();

    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    //vtkNew<vtkFileOutputWindow> fow;
    //fow->SetFileName("vtk.log");
    //vtkOutputWindow::SetInstance(fow);
    //vtkOutputWindow::SetGlobalWarningDisplay(false);

    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);


    ExplorerDialog dlg;
    if (dlg.exec() == QDialog::Accepted) {
        const auto kind = dlg.selectedKind();
        const QString path = dlg.selectedPath();

        switch (kind) {
        case ExplorerDialog::SelectionKind::DicomFile:
            qDebug() << "Selected DICOM-file:" << path;
            break;
        case ExplorerDialog::SelectionKind::DicomDir:
            qDebug() << "Selected DICOMDIR:" << path;
            break;
        case ExplorerDialog::SelectionKind::File3DR:
            qDebug() << "Selected 3DR:" << path;
            break;
        case ExplorerDialog::SelectionKind::DicomFolder:
            qDebug() << "Selected folder with DICOM-files:" << path;
            break;
        default:
            return 0;
            break;
        }

        if (!path.isEmpty())
        {
            MainWindow w(nullptr, path, kind);
            w.show();
            QTimer::singleShot(0, &w, &MainWindow::onOpenStudy);
            return app.exec();
        }
    }

    return 0;
}